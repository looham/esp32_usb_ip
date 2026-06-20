/**
 * @file wifi_manager.c
 * @brief ESP32 WiFi 管理：NVS 持久化 + STA 连接 + AP Web 配网
 *
 * ## 启动流程 (wifi_init)
 * 1. 初始化 NVS / netif / 事件循环 / WiFi 驱动
 * 2. NVS 无 ssid → 直接进入 AP 配网门户
 * 3. NVS 有 ssid → 尝试 STA 连接（最多 WIFI_STA_FAIL_BEFORE_AP 次）
 * 4. STA 全部失败 → 进入 AP 配网门户
 *
 * ## AP 配网门户 (wifi_start_ap_portal)
 * - 开启 AP（SSID: CONFIG_AP_SSID，默认 IP 192.168.4.1）
 * - 扫描周边 WiFi 填入 Web 下拉列表
 * - 启动 HTTP 服务器，用户 POST /config 保存 SSID/密码到 NVS 后重启
 *
 * ## 连接判定
 * - wifi_is_connected() 以 s_sta_has_ip 为准（已拿到 DHCP IP），而非仅 L2 关联
 * - 事件组 WIFI_CONNECTED_BIT 在 GOT_IP 时置位，供同步等待使用
 *
 * ## 日志模块标识 (WLOGx 第一个参数)
 * INIT | STA | AP | NVS | WEB | SCAN
 */

#include "wifi_manager.h"
#include "web_page.h"

static const char *TAG = "WiFiManager";

/* ---------- NVS 与连接参数 ---------- */
#define NVS_NS "wifi_config"              /**< NVS 命名空间，键: ssid / password */
#define WIFI_CONNECT_WAIT_MS 30000        /**< 单次 esp_wifi_connect 最长等待(ms) */
#define WIFI_RETRY_BASE_DELAY_MS 2000     /**< 重试退避基数(ms) */
#define WIFI_RETRY_MAX_DELAY_MS 8000      /**< 重试退避上限(ms) */

/* ---------- 事件组位（FreeRTOS EventGroup） ---------- */
#define WIFI_CONNECTED_BIT BIT0  /**< STA 已获得 IP（IP_EVENT_STA_GOT_IP） */
#define WIFI_FAIL_BIT BIT1       /**< STA 连接过程中意外断开（非主动断开） */
#define WIFI_AP_STARTED_BIT BIT2 /**< AP 已启动（WIFI_EVENT_AP_START） */
#define WIFI_SCAN_DONE_BIT BIT3  /**< 扫描完成（供外部可选等待） */

/** 统一日志：输出格式 WiFiManager: [模块] 消息 */
#define WLOGI(mod, fmt, ...) ESP_LOGI(TAG, "[%s] " fmt, mod, ##__VA_ARGS__)
#define WLOGW(mod, fmt, ...) ESP_LOGW(TAG, "[%s] " fmt, mod, ##__VA_ARGS__)
#define WLOGE(mod, fmt, ...) ESP_LOGE(TAG, "[%s] " fmt, mod, ##__VA_ARGS__)
#define WLOGD(mod, fmt, ...) ESP_LOGD(TAG, "[%s] " fmt, mod, ##__VA_ARGS__)

/* ---------- 模块状态 ---------- */
static EventGroupHandle_t s_wifi_event_group = NULL;
static httpd_handle_t s_http_server = NULL;
static char s_wifi_list_buffer[1024] = ""; /**< Web 页 WiFi 下拉列表 HTML 片段 */
static esp_netif_t *s_ap_netif = NULL;
static bool s_wifi_initialized = false;
static bool s_ap_mode_active = false;
static bool s_connect_attempted = false;   /**< 是否至少尝试过 STA 连接 */
static bool s_handlers_registered = false;
static wifi_status_callback_t s_status_callback = NULL;
static volatile bool s_sta_connecting = false;       /**< 正在等待单次 connect 结果 */
static volatile bool s_intentional_disconnect = false; /**< 主动断开时不触发 FAIL_BIT */
static volatile bool s_sta_has_ip = false;           /**< 对外“已连接”判定依据 */

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void wifi_start_ap(void);
static void wifi_stop_ap(void);
static bool wifi_scan_and_update_list(void);
static bool wifi_scan_target_ap(const char *ssid, wifi_config_t *wifi_config);
static bool wifi_try_connect_saved(void);
static void wifi_start_webserver(void);
static void wifi_start_ap_portal(void);

/** 通知上层 UI/LED：0=断开 1=连接中 2=已连接（有 IP） */
static void wifi_notify_status(int status)
{
    if (s_status_callback)
    {
        s_status_callback(status);
    }
}

/* ========== Web 表单解析 ========== */

/** 解析 URL 编码十六进制字符，非法返回 -1 */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/** application/x-www-form-urlencoded 解码：%XX → 字节，'+' → 空格 */
static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0, j = 0;

    while (src[i] && j < dst_size - 1)
    {
        if (src[i] == '%' && src[i + 1] && src[i + 2])
        {
            int a = hex_nibble(src[i + 1]);
            int b = hex_nibble(src[i + 2]);
            if (a >= 0 && b >= 0)
            {
                dst[j++] = (char)(16 * a + b);
                i += 3;
                continue;
            }
        }
        else if (src[i] == '+')
        {
            dst[j++] = ' ';
            i++;
            continue;
        }
        dst[j++] = src[i++];
    }
    dst[j] = '\0';
}

/**
 * 从 POST body 提取 key=value（如 ssid=xxx&password=yyy）
 * @return 找到并解码成功返回 true；未找到 key 返回 false
 */
static bool form_get_value(const char *body, const char *key, char *out, size_t out_sz)
{
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%s=", key);
    const char *p = strstr(body, prefix);
    if (!p)
        return false;

    p += strlen(prefix);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char tmp[128];
    if (len >= sizeof(tmp))
        len = sizeof(tmp) - 1;
    memcpy(tmp, p, len);
    tmp[len] = '\0';
    url_decode(tmp, out, out_sz);
    return true;
}

/* ========== NVS 读写 ========== */

/**
 * 从 NVS 读取 WiFi 凭据
 * @param pass 可为 NULL（仅检查 ssid 是否存在，wifi_has_saved_config 使用）
 */
static bool wifi_nvs_load(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;

    size_t len = ssid_sz;
    if (nvs_get_str(h, "ssid", ssid, &len) != ESP_OK || ssid[0] == '\0')
    {
        nvs_close(h);
        return false;
    }

    if (pass && pass_sz > 0)
    {
        len = pass_sz;
        nvs_get_str(h, "password", pass, &len);
    }
    nvs_close(h);
    return true;
}

/** 保存 SSID/密码到 NVS 并 commit */
static esp_err_t wifi_nvs_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "password", pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ========== STA 连接辅助 ========== */

/** 清除 CONNECTED/FAIL 位，避免上次结果影响 WaitBits */
static void wifi_clear_connect_events(void)
{
    if (s_wifi_event_group)
    {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
}

/**
 * 指数退避：每 3 次重试 delay 翻倍，上限 WIFI_RETRY_MAX_DELAY_MS
 * retry=0 → 2000ms, retry=3 → 4000ms, retry=6 → 8000ms
 */
static uint32_t wifi_retry_delay_ms(int retry_count)
{
    uint32_t delay = WIFI_RETRY_BASE_DELAY_MS * (1U << (retry_count / 3));
    return delay > WIFI_RETRY_MAX_DELAY_MS ? WIFI_RETRY_MAX_DELAY_MS : delay;
}

/**
 * 重试前主动断开：置 intentional 标志，避免 STA_DISCONNECTED 误报 FAIL
 * 等待 1s 让栈稳定后再清事件位
 */
static void wifi_prepare_sta_disconnect(void)
{
    s_intentional_disconnect = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));
    s_intentional_disconnect = false;
    wifi_clear_connect_events();
}

/* ========== 基础设施 ========== */

/** 注册 WiFi/IP 事件回调（仅一次） */
static void wifi_register_handlers(void)
{
    if (s_handlers_registered)
        return;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                        &wifi_event_handler, NULL, NULL));
    s_handlers_registered = true;
}

/** 创建 AP netif 并固定 IP 192.168.4.1/24（配网门户地址） */
static void wifi_setup_ap_netif(void)
{
    if (s_ap_netif)
        return;

    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_ip_info_t ip;
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_set_ip_info(s_ap_netif, &ip);
}

/**
 * 构造扫描参数
 * @param ssid 非 NULL 时仅扫描指定 SSID（含隐藏）；NULL 为全信道扫描
 */
static wifi_scan_config_t wifi_make_scan_config(const char *ssid, bool show_hidden,
                                                uint16_t min_ms, uint16_t max_ms)
{
    wifi_scan_config_t cfg = {
        .ssid = ssid ? (uint8_t *)ssid : NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = show_hidden,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = min_ms,
        .scan_time.active.max = max_ms,
    };
    return cfg;
}

/** 填充 AP 模式 wifi_config（SSID/信道/开放或 WPA2） */
static void wifi_build_ap_config(wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy((char *)cfg->ap.ssid, CONFIG_AP_SSID, sizeof(cfg->ap.ssid));
    cfg->ap.ssid_len = strlen(CONFIG_AP_SSID);
    cfg->ap.channel = 1;
    cfg->ap.authmode = strlen(CONFIG_AP_PASS) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg->ap.max_connection = 4;
    strncpy((char *)cfg->ap.password, CONFIG_AP_PASS, sizeof(cfg->ap.password));
}

/** 填充 STA 模式 wifi_config（扫描策略、PMF、重试次数等） */
static void wifi_fill_sta_config(wifi_config_t *cfg, const char *ssid, const char *pass)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf((char *)cfg->sta.ssid, sizeof(cfg->sta.ssid), "%s", ssid);

    if (pass && pass[0])
    {
        snprintf((char *)cfg->sta.password, sizeof(cfg->sta.password), "%s", pass);
        cfg->sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    }
    else
    {
        cfg->sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg->sta.threshold.rssi = -127;
    cfg->sta.failure_retry_cnt = 4;
    cfg->sta.pmf_cfg.capable = true;
    cfg->sta.pmf_cfg.required = false;
}

/**
 * WiFi / IP 事件统一入口
 * - STA 路径：STA_START → CONNECTED(L2) → GOT_IP(L3) → 置 CONNECTED_BIT
 * - 意外 DISCONNECTED：清 IP、通知断开；若正在 connect 则置 FAIL_BIT 唤醒 WaitBits
 * - GOT_IP 后若 AP 仍在运行则自动 wifi_stop_ap()
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_AP_START:
            WLOGI("AP", "已启动");
            xEventGroupSetBits(s_wifi_event_group, WIFI_AP_STARTED_BIT);
            s_ap_mode_active = true;
            break;

        case WIFI_EVENT_AP_STACONNECTED:
        {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
            WLOGI("AP", "客户端接入 MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                  e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
            WLOGI("AP", "客户端断开 MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                  e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
            break;
        }

        case WIFI_EVENT_STA_START:
            WLOGI("STA", "已启动");
            wifi_notify_status(1);
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)event_data;
            if (s_intentional_disconnect)
            {
                /* 重试前的主动 disconnect，不计入失败 */
                WLOGD("STA", "预期断开 reason=%d", e->reason);
                break;
            }

            WLOGW("STA", "断开 reason=%d", e->reason);
            s_sta_has_ip = false;
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            wifi_notify_status(0);
            if (!s_ap_mode_active && s_sta_connecting)
            {
                /* 唤醒 wifi_wait_sta_connected() */
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            break;
        }

        case WIFI_EVENT_STA_CONNECTED:
            WLOGI("STA", "已关联 AP");
            break;

        default:
            break;
        }
    }
    else if (event_base == IP_EVENT)
    {
        switch (event_id)
        {
        case IP_EVENT_STA_GOT_IP:
        {
            ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
            WLOGI("STA", "已获 IP " IPSTR " gw=" IPSTR " mask=" IPSTR,
                  IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw), IP2STR(&e->ip_info.netmask));
            s_sta_has_ip = true;
            wifi_notify_status(2);
            if (s_ap_mode_active)
            {
                /* STA 已联网，关闭配网 AP 与 Web */
                wifi_stop_ap();
            }
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            break;
        }

        case IP_EVENT_AP_STAIPASSIGNED:
            WLOGI("AP", "已分配客户端 IP");
            break;

        default:
            break;
        }
    }
}

/* ========== AP 模式 ========== */

/** 切换到 APSTA 并应用 AP 配置（配网期间 STA 仍可后台连接） */
static void wifi_start_ap(void)
{
    if (s_ap_mode_active)
    {
        WLOGI("AP", "已在运行");
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t cfg;
    wifi_build_ap_config(&cfg);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    s_ap_mode_active = true;
    WLOGI("AP", "SSID=%s IP=192.168.4.1", CONFIG_AP_SSID);
}

/** 停止 Web 服务并切回纯 STA 模式 */
static void wifi_stop_ap(void)
{
    if (!s_ap_mode_active)
        return;

    if (s_http_server)
    {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        WLOGI("WEB", "服务器已停止");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_ap_mode_active = false;
    WLOGI("AP", "已停止");
}

/* ========== 扫描 ========== */

/**
 * 全信道扫描，结果格式化为 HTML <option> 写入 s_wifi_list_buffer
 * 供 Web 配网页下拉选择；隐藏 SSID 不展示
 */
static bool wifi_scan_and_update_list(void)
{
    wifi_scan_config_t scan = wifi_make_scan_config(NULL, false, 100, 300);
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK)
    {
        WLOGE("SCAN", "失败 %s", esp_err_to_name(err));
        snprintf(s_wifi_list_buffer, sizeof(s_wifi_list_buffer), "<option>扫描失败</option>");
        return false;
    }

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    if (ap_count == 0)
    {
        WLOGW("SCAN", "未发现网络");
        snprintf(s_wifi_list_buffer, sizeof(s_wifi_list_buffer), "<option>未发现可用网络</option>");
        return false;
    }

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_list)
    {
        WLOGE("SCAN", "内存不足");
        return false;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));
    memset(s_wifi_list_buffer, 0, sizeof(s_wifi_list_buffer));
    char *ptr = s_wifi_list_buffer;
    int remaining = sizeof(s_wifi_list_buffer);

    for (int i = 0; i < ap_count; i++)
    {
        if (ap_list[i].ssid[0] == 0 || strlen((char *)ap_list[i].ssid) == 0)
            continue;

        int len = snprintf(ptr, remaining,
                           "<option value=\"%.32s\">%.32s (信号强度: %d)</option>",
                           ap_list[i].ssid, ap_list[i].ssid, ap_list[i].rssi);
        if (len <= 0 || len >= remaining)
            break;
        ptr += len;
        remaining -= len;
    }

    free(ap_list);
    WLOGI("SCAN", "完成 count=%u", ap_count);
    xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
    return true;
}

/**
 * 连接前定向扫描：锁定 channel + BSSID，缩短关联时间、提高成功率
 * 失败时调用方应清除 bssid_set，回退全信道扫描
 */
static bool wifi_scan_target_ap(const char *ssid, wifi_config_t *wifi_config)
{
    wifi_scan_config_t scan = wifi_make_scan_config(ssid, true, 120, 400);
    if (esp_wifi_scan_start(&scan, true) != ESP_OK)
    {
        WLOGW("SCAN", "目标 AP 扫描失败 ssid=%s", ssid);
        return false;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0)
    {
        WLOGW("SCAN", "未找到 ssid=%s", ssid);
        return false;
    }

    wifi_ap_record_t ap_record = {0};
    ap_count = 1;
    if (esp_wifi_scan_get_ap_records(&ap_count, &ap_record) != ESP_OK || ap_count == 0)
        return false;

    wifi_config->sta.channel = ap_record.primary;
    memcpy(wifi_config->sta.bssid, ap_record.bssid, sizeof(wifi_config->sta.bssid));
    wifi_config->sta.bssid_set = true;
    WLOGI("SCAN", "命中 ssid=%s ch=%d rssi=%d", ssid, ap_record.primary, ap_record.rssi);
    return true;
}

/** 确保当前为 STA 或 APSTA（AP 配网后重连 STA 时使用） */
static bool wifi_ensure_sta_mode(void)
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA)
        return true;
    return esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK;
}

/**
 * 阻塞等待单次 connect 结果
 * @note clear_on_exit 必须为 pdFALSE，否则 GOT_IP 后 CONNECTED_BIT 被清掉会导致
 *       wifi_is_connected / LED 误判为未连接
 */
static bool wifi_wait_sta_connected(void)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_WAIT_MS));
    if (bits & WIFI_CONNECTED_BIT)
        return true;

    if (!(bits & WIFI_FAIL_BIT))
    {
        WLOGW("STA", "连接超时 %d ms", WIFI_CONNECT_WAIT_MS);
        wifi_prepare_sta_disconnect();
    }
    return false;
}

/**
 * 从 NVS 加载凭据并尝试 STA 连接
 * 循环 WIFI_STA_FAIL_BEFORE_AP 次，含退避、定向扫描、超时/失败处理
 * @return true 已获 IP；false 全部重试失败
 */
static bool wifi_try_connect_saved(void)
{
    char saved_ssid[33] = {0};
    char saved_pass[65] = {0};

    if (!wifi_nvs_load(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass)))
    {
        WLOGI("NVS", "无已保存配置");
        return false;
    }

    WLOGI("NVS", "加载 SSID=%s pass_len=%u", saved_ssid, (unsigned)strlen(saved_pass));
    wifi_clear_connect_events();

    for (int retry = 0; retry < WIFI_STA_FAIL_BEFORE_AP; retry++)
    {
        if (retry > 0)
        {
            uint32_t delay_ms = wifi_retry_delay_ms(retry);
            WLOGI("STA", "重试 %d/%d 等待 %lu ms", retry + 1, WIFI_STA_FAIL_BEFORE_AP, (unsigned long)delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            wifi_prepare_sta_disconnect();
        }

        wifi_config_t cfg;
        wifi_fill_sta_config(&cfg, saved_ssid, saved_pass);
        if (!wifi_scan_target_ap(saved_ssid, &cfg))
        {
            /* 定向扫描失败时回退：不绑定 BSSID/信道 */
            cfg.sta.bssid_set = false;
            cfg.sta.channel = 0;
        }

        if (!wifi_ensure_sta_mode())
        {
            WLOGW("STA", "切换 STA 模式失败");
            retry++;
            continue;
        }

        esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (cfg_err != ESP_OK)
        {
            WLOGW("STA", "设置配置失败 %s", esp_err_to_name(cfg_err));
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_BASE_DELAY_MS));
            retry++;
            continue;
        }

        wifi_clear_connect_events();
        WLOGI("STA", "连接 %s (%d/%d)", saved_ssid, retry + 1, WIFI_STA_FAIL_BEFORE_AP);
        s_connect_attempted = true;
        s_sta_connecting = true;

        esp_err_t conn_err = esp_wifi_connect();
        if (conn_err != ESP_OK)
        {
            s_sta_connecting = false;
            WLOGE("STA", "esp_wifi_connect 失败 %s", esp_err_to_name(conn_err));
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_BASE_DELAY_MS));
            retry++;
            continue;
        }

        bool ok = wifi_wait_sta_connected();
        s_sta_connecting = false;
        if (ok)
        {
            WLOGI("STA", "连接成功 ssid=%s", saved_ssid);
            return true;
        }

        WLOGW("STA", "连接失败 (%d/%d)", retry + 1, WIFI_STA_FAIL_BEFORE_AP);
    }

    WLOGW("STA", "%d 次失败后进入 AP 配网", WIFI_STA_FAIL_BEFORE_AP);
    return false;
}

/* ========== HTTP 配网 Web ========== */

/** GET / ：拼接 ROOT_HTML + 扫描列表 + ROOT_HTML_2 */
static esp_err_t web_root_handler(httpd_req_t *req)
{
    size_t total_len = strlen(ROOT_HTML_1) + strlen(s_wifi_list_buffer) + strlen(ROOT_HTML_2) + 1;
    char *html = malloc(total_len);
    if (!html)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    strcpy(html, ROOT_HTML_1);
    strcat(html, s_wifi_list_buffer);
    strcat(html, ROOT_HTML_2);
    httpd_resp_set_type(req, "text/html");
    esp_err_t ret = httpd_resp_send(req, html, strlen(html));
    free(html);
    return ret;
}

/**
 * POST /config ：解析 ssid/password → 写 NVS → 响应 HTML → 3s 后 esp_restart()
 * 密码仅记录长度，不打印明文
 */
static esp_err_t web_config_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 512)
    {
        WLOGE("WEB", "无效请求长度 %d", total_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *content = malloc(total_len + 1);
    if (!content)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, content, total_len);
    if (ret <= 0)
    {
        free(content);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            httpd_resp_send_408(req);
        WLOGE("WEB", "接收失败 ret=%d", ret);
        return ESP_FAIL;
    }
    content[ret] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    form_get_value(content, "ssid", ssid, sizeof(ssid));
    form_get_value(content, "password", password, sizeof(password));
    free(content);

    if (ssid[0] == '\0')
    {
        WLOGW("WEB", "SSID 为空");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID不能为空");
        return ESP_FAIL;
    }

    WLOGI("WEB", "保存 SSID=%s pass_len=%u", ssid, (unsigned)strlen(password));
    esp_err_t err = wifi_nvs_save(ssid, password);
    if (err != ESP_OK)
    {
        WLOGE("NVS", "保存失败 %s", esp_err_to_name(err));
    }
    else
    {
        WLOGI("NVS", "配置已保存");
    }

    char response[512];
    snprintf(response, sizeof(response),
             "<html><head><meta charset='UTF-8'></head>"
             "<body style='text-align:center;padding:20px;'>"
             "<h3>配置完成</h3><p>SSID: %s</p><p>重启中...</p>"
             "<script>setTimeout(()=>location.href='/', 2000);</script>"
             "</body></html>",
             ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    vTaskDelay(pdMS_TO_TICKS(3000));
    WLOGI("INIT", "重启以应用新配置");
    esp_restart();
    return ESP_OK;
}

/** 启动 HTTP 服务器，注册 / 与 /config 路由 */
static void wifi_start_webserver(void)
{
    if (s_http_server)
        return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_PORT;
    config.max_open_sockets = 3;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = web_root_handler},
        {.uri = "/config", .method = HTTP_POST, .handler = web_config_handler},
    };

    if (httpd_start(&s_http_server, &config) != ESP_OK)
    {
        WLOGE("WEB", "启动失败");
        return;
    }

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
    {
        httpd_register_uri_handler(s_http_server, &uris[i]);
    }
    WLOGI("WEB", "已启动 http://192.168.4.1:%d", WEB_PORT);
}

/** AP + 扫描列表 + Web 三件套，供 init / reconnect / provisioning 共用 */
static void wifi_start_ap_portal(void)
{
    wifi_start_ap();
    wifi_scan_and_update_list();
    wifi_start_webserver();
    wifi_notify_status(1);
    WLOGI("AP", "配网就绪 SSID=%s 请访问 http://192.168.4.1", CONFIG_AP_SSID);
}

/* ========== 对外 API（见 wifi_manager.h） ========== */

bool wifi_has_saved_config(void)
{
    char ssid[33] = {0};
    return wifi_nvs_load(ssid, sizeof(ssid), NULL, 0);
}

void wifi_set_status_callback(wifi_status_callback_t callback)
{
    s_status_callback = callback;
}

/** 擦除 NVS 中 ssid/password（如恢复出厂 / 长按清除） */
void wifi_clear_saved_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
    {
        WLOGW("NVS", "打开失败");
        return;
    }
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "password");
    nvs_commit(h);
    nvs_close(h);
    WLOGI("NVS", "配置已清除");
}

/** 外部触发 AP 配网（如按钮长按）；WiFi 须已通过 wifi_init 初始化 */
void wifi_start_ap_provisioning(void)
{
    if (s_ap_mode_active)
    {
        WLOGI("AP", "配网已在运行");
        return;
    }
    WLOGI("AP", "STA 失败次数过多，启动配网");
    wifi_start_ap_portal();
}

/**
 * 主入口：NVS → netif → WiFi 驱动 → 按配置选择 STA 或 AP 配网
 * 重复调用安全（s_wifi_initialized 守卫）
 */
void wifi_init(void)
{
    if (s_wifi_initialized)
    {
        WLOGI("INIT", "已初始化");
        return;
    }

    WLOGI("INIT", "开始初始化");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        WLOGW("NVS", "分区需擦除后重建");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    wifi_setup_ap_netif();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_register_handlers();

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* 不使用 WiFi 驱动内置 NVS，由本模块管理 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);       /* 禁用省电，降低 USB/IP 传输延迟 */
    esp_wifi_set_max_tx_power(78);       /* 19.5 dBm */

    s_wifi_initialized = true;
    WLOGI("INIT", "硬件就绪");

    if (!wifi_has_saved_config())
    {
        WLOGI("INIT", "NVS 无配置，进入 AP 配网");
        wifi_start_ap_portal();
        return;
    }

    WLOGI("INIT", "NVS 有配置，尝试 STA 连接");
    if (wifi_try_connect_saved())
        return;

    WLOGI("INIT", "STA 连接失败，进入 AP 配网");
    wifi_start_ap_portal();
}

/** 主动断开 STA，清除 IP 状态与 CONNECTED 事件位 */
void wifi_disconnect(void)
{
    WLOGI("STA", "主动断开");
    s_sta_has_ip = false;
    ESP_ERROR_CHECK(esp_wifi_disconnect());
    wifi_clear_connect_events();
}

/** 是否已完成 L3 连接（有 DHCP IP），非仅 L2 关联 */
bool wifi_is_connected(void)
{
    return s_sta_has_ip;
}

/** 填充运行时状态：连接时附带 SSID/RSSI/IP/网关/掩码 */
void wifi_get_status(wifi_status_t *status)
{
    if (!status)
        return;

    memset(status, 0, sizeof(*status));
    status->initialized = s_wifi_initialized;
    status->connected = wifi_is_connected();
    status->ap_mode_active = s_ap_mode_active;

    if (!status->connected)
        return;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        snprintf(status->ssid, sizeof(status->ssid), "%s", (char *)ap_info.ssid);
        status->rssi = ap_info.rssi;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif)
    {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
        {
            status->ip.addr = ip_info.ip.addr;
            status->gw.addr = ip_info.gw.addr;
            status->netmask.addr = ip_info.netmask.addr;
        }
    }
}

/** 触发全信道扫描并更新 s_wifi_list_buffer（Web 页可刷新列表） */
void wifi_scan(void)
{
    if (!s_wifi_initialized)
    {
        WLOGW("SCAN", "WiFi 未初始化");
        return;
    }
    wifi_scan_and_update_list();
}

/** 返回 HTML <option> 片段，供 Web 或外部展示 */
const char *wifi_get_scan_results(void)
{
    return s_wifi_list_buffer;
}

/**
 * 手动重连：停 AP → 断开 → 读 NVS 重连；失败则重新进入配网门户
 */
void wifi_reconnect(void)
{
    if (!s_wifi_initialized)
    {
        wifi_init();
        return;
    }

    if (s_ap_mode_active)
        wifi_stop_ap();

    WLOGI("STA", "重新连接");
    wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (wifi_try_connect_saved())
    {
        WLOGI("STA", "重连成功");
        return;
    }

    WLOGW("STA", "重连失败，进入 AP 配网");
    wifi_start_ap_portal();
}
