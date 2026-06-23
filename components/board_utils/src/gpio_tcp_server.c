/**
 * @file gpio_tcp_server.c
 * @brief 轻量 LwIP TCP GPIO 控制（无 asio 依赖）
 *
 * 设计目标：尽量不影响 USB/IP 主服务
 * - 独立端口 8080，纯 LwIP socket（不依赖 asio，减少资源占用）
 * - 任务绑 Core 0（USB Host 事件和 USB/IP Server 运行在 Core 1）
 * - 低优先级（2）、小栈（3KB）、单客户端、行文本协议
 *
 * 协议（\\n 结尾，兼容 \\r\\n）:
 *   PING                      → PONG
 *   GET <pin>                 → OK <pin> <level>   | ERR pin <pin> denied
 *   SET <pin> <0|1>           → OK                  | ERR pin <pin> denied
 *   LED <r> <g> <b>           → OK                  | ERR led unavailable
 *
 * 引脚安全：仅允许 pin_define.h 中白名单定义的引脚操作
 * - GET 允许：输出白名单(GPIO_OUT_1~4) + 按钮列表(BOOT/BTN1/BTN2)
 * - SET 允许：输出白名单(GPIO_OUT_1~4)
 *
 * 生命周期：
 * - 网络就绪（STA/AP）时监听并接受连接
 * - 网络断开时关闭监听 socket，等待网络恢复后重新监听
 * - 每次仅服务一个客户端，断开后才接受下一个
 */

#include "gpio_tcp_server.h"
#include "pin_define.h"
#include "wifi_manager.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <lwip/sockets.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *TAG = "GpioTcp";

/** FreeRTOS 任务栈大小（字节）—— 纯 socket + 简单字符串解析，3KB 足够 */
#define GPIO_TCP_TASK_STACK   3072

/** 任务优先级 —— 低优先级，不抢占 USB/IP 相关任务 */
#define GPIO_TCP_TASK_PRIO    2

/** 绑定的 CPU 核心 —— Core 0，与 Core 1 上的 USB/IP 服务互不干扰 */
#define GPIO_TCP_TASK_CORE    0

/** 接收缓冲区大小（字节）—— 单行命令最大长度 */
#define GPIO_TCP_RX_BUF       128

/** 客户端空闲超时（毫秒）—— 超时后自动断开，防止连接泄露 */
#define GPIO_TCP_CLIENT_MS    30000

/** LED 句柄，由 gpio_tcp_server_init() 传入，用于 LED 命令（可为 NULL） */
static led_handle_t *s_led = NULL;

/** FreeRTOS 任务句柄，防止重复创建 */
static TaskHandle_t s_task = NULL;

/**
 * @brief 检查网络是否就绪（STA 已连接 或 AP 模式激活）
 * @return true 网络可用，可监听 TCP 连接
 *
 * 网络未就绪时，任务会挂起等待，避免在无网络时浪费资源
 */
static bool network_ready(void)
{
    wifi_status_t st;
    wifi_get_status(&st);
    return st.connected || st.ap_mode_active;
}

/**
 * @brief 检查引脚是否允许作为输出（SET 命令）
 * @param pin GPIO 编号
 * @return true 允许
 *
 * 委托给 pin_define.h 中的白名单检查函数
 */
static bool pin_output_allowed(int pin)
{
    return pin_board_output_allowed(pin);
}

/**
 * @brief 检查引脚是否允许读取（GET 命令）
 * @param pin GPIO 编号
 * @return true 允许
 *
 * 允许读取的范围 = 输出白名单 + 按钮引脚
 */
static bool pin_read_allowed(int pin)
{
    return pin_board_input_allowed(pin);
}

/**
 * @brief 确保引脚配置为输出模式（SET 命令前置条件）
 * @param pin GPIO 编号
 * @return ESP_OK 成功，ESP_ERR_NOT_ALLOWED 引脚不在白名单
 *
 * 如果引脚当前是输入模式（如按钮引脚），会重新配置为输出模式。
 * 注意：对按钮引脚调用 SET 会改变其模式，之后 GET 可能不再反映按钮状态。
 */
static esp_err_t ensure_output_pin(int pin)
{
    if (!pin_output_allowed(pin))
        return ESP_ERR_NOT_ALLOWED;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

/**
 * @brief 确保引脚配置为输入模式（GET 命令前置条件）
 * @param pin GPIO 编号
 * @return ESP_OK 成功，ESP_ERR_NOT_ALLOWED 引脚不在白名单
 *
 * 仅对按钮引脚做配置（启用内部上拉，确保读取正确），
 * 输出引脚读取时无需重新配置，直接读取当前电平即可。
 */
static esp_err_t ensure_input_pin(int pin)
{
    if (!pin_read_allowed(pin))
        return ESP_ERR_NOT_ALLOWED;

    // 输出引脚直接读取当前电平，不需要重新配置
    if (!pin_board_is_button(pin))
        return ESP_OK;

    // 按钮引脚：配置为输入 + 上拉（按下时接 GND = 低电平）
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

/**
 * @brief 格式化发送一行文本到 socket（不带缓冲，直接 send）
 * @param fd  socket 文件描述符
 * @param fmt 格式化字符串（自动追加内容，需手动包含 \n）
 *
 * 缓冲区 96 字节，超长截断。发送失败时静默忽略（客户端可能已断开）。
 */
static void send_line(int fd, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;   // 截断超长内容
    send(fd, buf, (size_t)n, 0);
}

/**
 * @brief 解析并执行一行命令
 * @param fd   客户端 socket，用于发送响应
 * @param line 已去除 \r\n 的命令行
 *
 * 命令格式及响应：
 * ┌──────────────────┬───────────────────────────────────────┐
 * │ 命令             │ 响应                                  │
 * ├──────────────────┼───────────────────────────────────────┤
 * │ PING             │ PONG                                  │
 * │ GET <pin>        │ OK <pin> <0|1> | ERR pin <pin> denied │
 * │ SET <pin> <0|1>  │ OK | ERR pin <pin> denied             │
 * │ LED <r> <g> <b>  │ OK | ERR led unavailable              │
 * │ (其他)           │ ERR unknown <cmd>                     │
 * └──────────────────┴───────────────────────────────────────┘
 */
static void handle_command(int fd, char *line)
{
    // 跳过前导空白
    while (*line == ' ' || *line == '\t')
        line++;
    if (line[0] == '\0')
        return;

    // 提取命令关键字（最多 15 字符）
    char cmd[16] = {0};
    if (sscanf(line, "%15s", cmd) != 1)
    {
        send_line(fd, "ERR parse\n");
        return;
    }

    // ---- PING：心跳检测 ----
    if (strcmp(cmd, "PING") == 0)
    {
        send_line(fd, "PONG\n");
        return;
    }

    // ---- GET <pin>：读取引脚电平 ----
    if (strcmp(cmd, "GET") == 0)
    {
        int pin = -1;
        if (sscanf(line, "%*s %d", &pin) != 1)
        {
            send_line(fd, "ERR usage GET <pin>\n");
            return;
        }
        if (!pin_read_allowed(pin) || ensure_input_pin(pin) != ESP_OK)
        {
            send_line(fd, "ERR pin %d denied\n", pin);
            return;
        }
        send_line(fd, "OK %d %d\n", pin, gpio_get_level((gpio_num_t)pin));
        return;
    }

    // ---- SET <pin> <0|1>：设置引脚输出电平 ----
    if (strcmp(cmd, "SET") == 0)
    {
        int pin = -1;
        int level = -1;
        if (sscanf(line, "%*s %d %d", &pin, &level) != 2 || (level != 0 && level != 1))
        {
            send_line(fd, "ERR usage SET <pin> <0|1>\n");
            return;
        }
        if (!pin_output_allowed(pin) || ensure_output_pin(pin) != ESP_OK)
        {
            send_line(fd, "ERR pin %d denied\n", pin);
            return;
        }
        gpio_set_level((gpio_num_t)pin, level);
        send_line(fd, "OK\n");
        return;
    }

    // ---- LED <r> <g> <b>：设置 WS2812 RGB LED 颜色 ----
    if (strcmp(cmd, "LED") == 0)
    {
        int r = 0, g = 0, b = 0;
        if (sscanf(line, "%*s %d %d %d", &r, &g, &b) != 3)
        {
            send_line(fd, "ERR usage LED <r> <g> <b>\n");
            return;
        }
        if (!s_led)
        {
            send_line(fd, "ERR led unavailable\n");
            return;
        }
        // 钳位到 0~255 范围
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        if (led_set_all(s_led, (uint8_t)r, (uint8_t)g, (uint8_t)b) == ESP_OK)
            send_line(fd, "OK\n");
        else
            send_line(fd, "ERR led\n");
        return;
    }

    // 未知命令
    send_line(fd, "ERR unknown %s\n", cmd);
}

/**
 * @brief 处理单个客户端连接（阻塞直到客户端断开或超时）
 * @param client_fd 客户端 socket 文件描述符
 *
 * 流程：
 * 1. 设置接收超时（30秒），防止连接泄露
 * 2. 发送欢迎消息 "OK gpio-tcp ready"
 * 3. 循环接收数据，按 \n 分行解析命令
 * 4. 支持缓冲区中存在多条命令（一次性收到多个命令的情况）
 * 5. 兼容 \r\n 行尾（自动去除 \r）
 * 6. 行过长（≥127字节）时返回 ERR 并丢弃
 *
 * 退出条件：recv 返回 0（EOF）或负值（错误/超时）
 */
static void handle_client(int client_fd)
{
    char buf[GPIO_TCP_RX_BUF];
    size_t len = 0;   // 缓冲区中未处理的数据长度

    // 设置 socket 接收超时，防止客户端挂起后连接永远不释放
    struct timeval tv = {
        .tv_sec = GPIO_TCP_CLIENT_MS / 1000,
        .tv_usec = (GPIO_TCP_CLIENT_MS % 1000) * 1000,
    };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 发送欢迎消息，客户端可据此确认连接成功
    send_line(client_fd, "OK gpio-tcp ready\n");

    while (true)
    {
        // 追加接收数据到缓冲区末尾（保留 1 字节给 \0）
        ssize_t n = recv(client_fd, buf + len, sizeof(buf) - len - 1, 0);
        if (n <= 0)
            break;    // 连接关闭或超时

        len += (size_t)n;
        buf[len] = '\0';

        // 按行分割并逐条处理命令
        char *start = buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL)
        {
            *nl = '\0';
            // 兼容 Windows \r\n 行尾：去除末尾 \r
            if (nl > start && *(nl - 1) == '\r')
                *(nl - 1) = '\0';
            handle_command(client_fd, start);
            start = nl + 1;
        }

        // 将未处理的残留数据移到缓冲区头部
        if (start > buf)
        {
            len = strlen(start);
            memmove(buf, start, len + 1);
        }

        // 缓冲区满但未找到换行符 → 行过长，丢弃
        if (len >= sizeof(buf) - 1)
        {
            send_line(client_fd, "ERR line too long\n");
            len = 0;
            buf[0] = '\0';
        }
    }
}

/**
 * @brief 创建并绑定 TCP 监听 socket
 * @return 监听 socket fd，失败返回 -1
 *
 * 配置：
 * - SO_REUSEADDR：允许重启后快速重用端口（避免 TIME_WAIT 阻塞）
 * - 监听所有网卡（INADDR_ANY）
 * - 端口 GPIO_TCP_PORT（8080）
 * - backlog = 1（单客户端）
 */
static int open_listen_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0)
        return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),  // 监听所有网络接口
        .sin_port = htons(GPIO_TCP_PORT),        // 端口 8080
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0)                      // backlog=1，仅允许一个等待连接
    {
        close(fd);
        return -1;
    }
    return fd;
}

/**
 * @brief GPIO TCP 服务主任务（FreeRTOS 任务，常驻循环）
 * @param arg 未使用
 *
 * 整体流程（无限循环）：
 * ┌─────────────────────────────────────────────┐
 * │ 1. 等待网络就绪（STA 连接 或 AP 激活）        │
 * │ 2. 创建监听 socket (0.0.0.0:8080)            │
 * │ 3. select + accept 等待客户端连接              │
 * │ 4. 处理客户端命令直到断开                      │
 * │ 5. 网络断开时关闭 socket，回到步骤 1           │
 * └─────────────────────────────────────────────┘
 *
 * 启动时还会预配置 GPIO_OUT_1（默认使能脚）为输出模式，
 * 以便客户端无需先 SET 即可 GET 读取其电平。
 *
 * select 超时 1 秒：定期检查网络状态，网络断开时及时退出 accept 循环
 */
static void gpio_tcp_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "task on core %d prio %d port %d", GPIO_TCP_TASK_CORE, GPIO_TCP_TASK_PRIO, GPIO_TCP_PORT);

    // 预配置默认使能脚为输出模式，方便远程读取状态
    ensure_output_pin(GPIO_OUT_1);

    while (true)
    {
        // 等待网络就绪（WiFi STA 连接 或 AP 配网模式激活）
        while (!network_ready())
            vTaskDelay(pdMS_TO_TICKS(2000));    // 2秒轮询

        int listen_fd = open_listen_socket();
        if (listen_fd < 0)
        {
            ESP_LOGW(TAG, "listen failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(5000));    // 监听失败，5秒后重试
            continue;
        }

        ESP_LOGI(TAG, "listening on %d", GPIO_TCP_PORT);

        // 网络就绪期间持续接受客户端连接
        while (network_ready())
        {
            // 使用 select 实现 1 秒超时的 accept，避免网络断开时阻塞
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listen_fd, &rfds);
            struct timeval tv = {.tv_sec = 1, .tv_usec = 0};

            int sel = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
            if (sel < 0)
                break;     // select 出错，退出内层循环重建 socket
            if (sel == 0)
                continue;  // 超时，重新检查网络状态

            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int client = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
            if (client < 0)
                continue;

            ESP_LOGD(TAG, "client connected");
            handle_client(client);  // 阻塞处理直到客户端断开
            close(client);
            ESP_LOGD(TAG, "client closed");
        }

        // 网络断开，关闭监听 socket
        close(listen_fd);
        ESP_LOGI(TAG, "stopped (network down)");
    }
}

/**
 * @brief 初始化 GPIO TCP 服务器（启动后台任务）
 * @param led_handle LED 句柄，非 NULL 时支持 LED 命令；传 NULL 则 LED 命令返回 ERR
 *
 * 创建 FreeRTOS 任务：
 * - 名称："gpio_tcp"
 * - 栈大小：3072 字节
 * - 优先级：2（低优先级，不抢占 USB/IP）
 * - 绑定核心：Core 0（USB/IP 在 Core 1）
 *
 * 幂等：重复调用不会创建多个任务
 */
void gpio_tcp_server_init(led_handle_t *led_handle)
{
    if (s_task)
        return;     // 已初始化，防止重复创建

    s_led = led_handle;
    xTaskCreatePinnedToCore(gpio_tcp_task, "gpio_tcp", GPIO_TCP_TASK_STACK, NULL,
                            GPIO_TCP_TASK_PRIO, &s_task, GPIO_TCP_TASK_CORE);
}
