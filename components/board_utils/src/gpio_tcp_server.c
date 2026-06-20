/**
 * @file gpio_tcp_server.c
 * @brief 轻量 LwIP TCP GPIO 控制（无 asio）
 *
 * 设计目标：尽量不影响 USB/IP
 * - 独立端口 8080，纯 LwIP socket
 * - 任务绑 Core 0（USB Host 事件在 Core 1）
 * - 低优先级、小栈、单客户端、行文本协议
 *
 * 协议（\\n 结尾）:
 *   PING
 *   GET <pin>
 *   SET <pin> <0|1>
 *   LED <r> <g> <b>   (0-255，需 init 时传入 led_handle)
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

#define GPIO_TCP_TASK_STACK   3072
#define GPIO_TCP_TASK_PRIO    2
#define GPIO_TCP_TASK_CORE    0
#define GPIO_TCP_RX_BUF       128
#define GPIO_TCP_CLIENT_MS    30000

static led_handle_t *s_led = NULL;
static TaskHandle_t s_task = NULL;

static bool network_ready(void)
{
    wifi_status_t st;
    wifi_get_status(&st);
    return st.connected || st.ap_mode_active;
}

static bool pin_output_allowed(int pin)
{
    return pin_board_output_allowed(pin);
}

static bool pin_read_allowed(int pin)
{
    return pin_board_input_allowed(pin);
}

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

static esp_err_t ensure_input_pin(int pin)
{
    if (!pin_read_allowed(pin))
        return ESP_ERR_NOT_ALLOWED;

    if (!pin_board_is_button(pin))
        return ESP_OK;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

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
        n = (int)sizeof(buf) - 1;
    send(fd, buf, (size_t)n, 0);
}

static void handle_command(int fd, char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;
    if (line[0] == '\0')
        return;

    char cmd[16] = {0};
    if (sscanf(line, "%15s", cmd) != 1)
    {
        send_line(fd, "ERR parse\n");
        return;
    }

    if (strcmp(cmd, "PING") == 0)
    {
        send_line(fd, "PONG\n");
        return;
    }

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
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        if (led_set_all(s_led, (uint8_t)r, (uint8_t)g, (uint8_t)b) == ESP_OK)
            send_line(fd, "OK\n");
        else
            send_line(fd, "ERR led\n");
        return;
    }

    send_line(fd, "ERR unknown %s\n", cmd);
}

static void handle_client(int client_fd)
{
    char buf[GPIO_TCP_RX_BUF];
    size_t len = 0;

    struct timeval tv = {
        .tv_sec = GPIO_TCP_CLIENT_MS / 1000,
        .tv_usec = (GPIO_TCP_CLIENT_MS % 1000) * 1000,
    };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    send_line(client_fd, "OK gpio-tcp ready\n");

    while (true)
    {
        ssize_t n = recv(client_fd, buf + len, sizeof(buf) - len - 1, 0);
        if (n <= 0)
            break;

        len += (size_t)n;
        buf[len] = '\0';

        char *start = buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL)
        {
            *nl = '\0';
            if (nl > start && *(nl - 1) == '\r')
                *(nl - 1) = '\0';
            handle_command(client_fd, start);
            start = nl + 1;
        }

        if (start > buf)
        {
            len = strlen(start);
            memmove(buf, start, len + 1);
        }
        if (len >= sizeof(buf) - 1)
        {
            send_line(client_fd, "ERR line too long\n");
            len = 0;
            buf[0] = '\0';
        }
    }
}

static int open_listen_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0)
        return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(GPIO_TCP_PORT),
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static void gpio_tcp_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "task on core %d prio %d port %d", GPIO_TCP_TASK_CORE, GPIO_TCP_TASK_PRIO, GPIO_TCP_PORT);

    ensure_output_pin(GPIO_OUT_1);

    while (true)
    {
        while (!network_ready())
            vTaskDelay(pdMS_TO_TICKS(2000));

        int listen_fd = open_listen_socket();
        if (listen_fd < 0)
        {
            ESP_LOGW(TAG, "listen failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "listening on %d", GPIO_TCP_PORT);

        while (network_ready())
        {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listen_fd, &rfds);
            struct timeval tv = {.tv_sec = 1, .tv_usec = 0};

            int sel = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
            if (sel < 0)
                break;
            if (sel == 0)
                continue;

            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int client = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
            if (client < 0)
                continue;

            ESP_LOGD(TAG, "client connected");
            handle_client(client);
            close(client);
            ESP_LOGD(TAG, "client closed");
        }

        close(listen_fd);
        ESP_LOGI(TAG, "stopped (network down)");
    }
}

void gpio_tcp_server_init(led_handle_t *led_handle)
{
    if (s_task)
        return;

    s_led = led_handle;
    xTaskCreatePinnedToCore(gpio_tcp_task, "gpio_tcp", GPIO_TCP_TASK_STACK, NULL,
                            GPIO_TCP_TASK_PRIO, &s_task, GPIO_TCP_TASK_CORE);
}
