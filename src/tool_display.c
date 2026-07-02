#include "tool_display.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "brn_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "generated/brn_profile_config.h"
#include "wifi/wifi_manager.h"

#ifndef BRN_PROFILE_DISPLAY_SPI_SCLK
#error "tool-display requires display.spi_sclk in the firmware Profile"
#endif
#ifndef BRN_PROFILE_DISPLAY_SPI_MOSI
#error "tool-display requires display.spi_mosi in the firmware Profile"
#endif
#ifndef BRN_PROFILE_DISPLAY_RESET
#error "tool-display requires display.reset in the firmware Profile"
#endif
#ifndef BRN_PROFILE_DISPLAY_DC
#error "tool-display requires display.dc in the firmware Profile"
#endif
#ifndef BRN_PROFILE_DISPLAY_CS
#error "tool-display requires display.cs in the firmware Profile"
#endif
#ifndef BRN_PROFILE_DISPLAY_BACKLIGHT
#error "tool-display requires display.backlight in the firmware Profile"
#endif

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 160
#define DISPLAY_SPI_HOST SPI2_HOST
#define DISPLAY_CLOCK_HZ (20 * 1000 * 1000)
#define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define STATUS_REFRESH_MS 3000
#define WIFI_TIMEOUT_MS 35000

enum {
    COLOR_BG = RGB565(236, 241, 245),
    COLOR_PANEL = RGB565(253, 254, 252),
    COLOR_PANEL_DARK = RGB565(24, 62, 76),
    COLOR_LINE = RGB565(192, 205, 212),
    COLOR_TEXT = RGB565(24, 35, 43),
    COLOR_MUTED = RGB565(91, 107, 116),
    COLOR_ACCENT = RGB565(0, 137, 123),
    COLOR_ACCENT_SOFT = RGB565(214, 238, 235),
    COLOR_WARN = RGB565(226, 139, 54),
    COLOR_ERROR = RGB565(202, 74, 74),
    COLOR_SKY = RGB565(56, 124, 170),
    COLOR_WHITE = RGB565(255, 255, 255),
};

static const char *TAG = "tool_display";
static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static bool s_time_sync_started;
static TickType_t s_init_tick;
static char s_weather1[32] = "BEIJING";
static char s_weather2[32] = "SUNNY";

static esp_err_t transmit(bool data_mode, const void *data, size_t length)
{
    if (length == 0) {
        return ESP_OK;
    }
    gpio_set_level(BRN_PROFILE_DISPLAY_DC, data_mode ? 1 : 0);
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi, &transaction);
}

static esp_err_t command(uint8_t value)
{
    return transmit(false, &value, 1);
}

static esp_err_t command_data(uint8_t value, const uint8_t *data, size_t length)
{
    esp_err_t err = command(value);
    return err == ESP_OK ? transmit(true, data, length) : err;
}

static esp_err_t set_window(uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    uint16_t x_end = x + width - 1;
    uint16_t y_end = y + height - 1;
    uint8_t columns[] = {x >> 8, x & 0xFF, x_end >> 8, x_end & 0xFF};
    uint8_t rows[] = {y >> 8, y & 0xFF, y_end >> 8, y_end & 0xFF};
    esp_err_t err = command_data(0x2A, columns, sizeof(columns));
    if (err == ESP_OK) {
        err = command_data(0x2B, rows, sizeof(rows));
    }
    if (err == ESP_OK) {
        err = command(0x2C);
    }
    return err;
}

static esp_err_t fill(uint16_t color)
{
    uint8_t line[DISPLAY_WIDTH * 2];
    for (size_t i = 0; i < DISPLAY_WIDTH; ++i) {
        line[i * 2] = color >> 8;
        line[i * 2 + 1] = color & 0xFF;
    }
    esp_err_t err = set_window(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    for (int y = 0; err == ESP_OK && y < DISPLAY_HEIGHT; ++y) {
        err = transmit(true, line, sizeof(line));
    }
    return err;
}

static esp_err_t fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (width <= 0 || height <= 0) {
        return ESP_OK;
    }
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) {
        return ESP_OK;
    }
    if (x + width > DISPLAY_WIDTH) {
        width = DISPLAY_WIDTH - x;
    }
    if (y + height > DISPLAY_HEIGHT) {
        height = DISPLAY_HEIGHT - y;
    }
    if (width <= 0 || height <= 0) {
        return ESP_OK;
    }

    esp_err_t err = set_window((uint16_t)x, (uint16_t)y, (uint16_t)width, (uint16_t)height);
    uint8_t pixels[128];
    for (size_t i = 0; i < sizeof(pixels); i += 2) {
        pixels[i] = color >> 8;
        pixels[i + 1] = color & 0xFF;
    }
    int remaining = width * height;
    while (err == ESP_OK && remaining > 0) {
        int chunk = remaining > (int)(sizeof(pixels) / 2) ? (int)(sizeof(pixels) / 2) : remaining;
        err = transmit(true, pixels, (size_t)chunk * 2);
        remaining -= chunk;
    }
    return err;
}

static const uint8_t *glyph(char input)
{
    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    static const uint8_t unknown[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
    static const uint8_t digits[10][5] = {
        {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
        {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
        {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
        {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    };
    static const uint8_t letters[26][5] = {
        {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
        {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
        {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
        {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
        {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
        {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
        {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
        {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
        {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
        {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
        {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
        {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
    };
    static const uint8_t dash[5] = {0x08,0x08,0x08,0x08,0x08};
    static const uint8_t dot[5] = {0,0x60,0x60,0,0};
    static const uint8_t colon[5] = {0,0x36,0x36,0,0};
    static const uint8_t slash[5] = {0x20,0x10,0x08,0x04,0x02};
    static const uint8_t percent[5] = {0x62,0x64,0x08,0x13,0x23};

    unsigned char value = (unsigned char)input;
    if (value == ' ') return blank;
    if (value >= '0' && value <= '9') return digits[value - '0'];
    value = (unsigned char)toupper(value);
    if (value >= 'A' && value <= 'Z') return letters[value - 'A'];
    if (value == '-') return dash;
    if (value == '.') return dot;
    if (value == ':') return colon;
    if (value == '/') return slash;
    if (value == '%') return percent;
    return unknown;
}

static esp_err_t draw_character(uint16_t x, uint16_t y, char value, uint16_t fg, uint16_t bg)
{
    const uint8_t *bitmap = glyph(value);
    uint8_t pixels[6 * 8 * 2];
    size_t offset = 0;
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 6; ++column) {
            bool active = column < 5 && (bitmap[column] & (1U << row));
            uint16_t color = active ? fg : bg;
            pixels[offset++] = color >> 8;
            pixels[offset++] = color & 0xFF;
        }
    }
    esp_err_t err = set_window(x, y, 6, 8);
    return err == ESP_OK ? transmit(true, pixels, sizeof(pixels)) : err;
}

static esp_err_t draw_character_scaled(int x, int y, char value, uint16_t fg, uint16_t bg, int scale)
{
    if (scale <= 1) {
        return draw_character((uint16_t)x, (uint16_t)y, value, fg, bg);
    }
    const uint8_t *bitmap = glyph(value);
    esp_err_t err = fill_rect(x, y, 6 * scale, 8 * scale, bg);
    for (int row = 0; err == ESP_OK && row < 8; ++row) {
        for (int column = 0; err == ESP_OK && column < 6; ++column) {
            bool active = column < 5 && (bitmap[column] & (1U << row));
            if (active) {
                err = fill_rect(x + column * scale, y + row * scale, scale, scale, fg);
            }
        }
    }
    return err;
}

static esp_err_t draw_text_at(int x, int y, const char *text, uint16_t fg, uint16_t bg,
                              int scale, int max_width)
{
    if (!text) {
        return ESP_OK;
    }
    int cursor_x = x;
    int step = 6 * scale;
    esp_err_t err = ESP_OK;
    for (const unsigned char *cursor = (const unsigned char *)text;
         err == ESP_OK && *cursor != '\0';
         ++cursor) {
        if (*cursor == '\n' || cursor_x + step > x + max_width || cursor_x + step > DISPLAY_WIDTH) {
            break;
        }
        char value = (*cursor < 0x80) ? (char)*cursor : '?';
        err = draw_character_scaled(cursor_x, y, value, fg, bg, scale);
        cursor_x += step;
        while (cursor[1] >= 0x80 && cursor[1] < 0xC0) {
            ++cursor;
        }
    }
    return err;
}

static esp_err_t draw_rect(int x, int y, int width, int height, uint16_t color)
{
    esp_err_t err = fill_rect(x, y, width, 1, color);
    if (err == ESP_OK) err = fill_rect(x, y + height - 1, width, 1, color);
    if (err == ESP_OK) err = fill_rect(x, y, 1, height, color);
    if (err == ESP_OK) err = fill_rect(x + width - 1, y, 1, height, color);
    return err;
}

static esp_err_t draw_panel(int x, int y, int width, int height)
{
    esp_err_t err = fill_rect(x, y, width, height, COLOR_PANEL);
    if (err == ESP_OK) {
        err = draw_rect(x, y, width, height, COLOR_LINE);
    }
    return err;
}

static esp_err_t draw_dot(int x, int y, uint16_t color)
{
    esp_err_t err = fill_rect(x + 1, y, 4, 1, color);
    if (err == ESP_OK) err = fill_rect(x, y + 1, 6, 4, color);
    if (err == ESP_OK) err = fill_rect(x + 1, y + 5, 4, 1, color);
    return err;
}

static esp_err_t draw_sun_icon(int x, int y)
{
    const uint16_t sun = RGB565(244, 174, 55);
    const uint16_t ray = RGB565(236, 125, 49);
    esp_err_t err = fill_rect(x + 5, y + 5, 8, 8, sun);
    if (err == ESP_OK) err = fill_rect(x + 7, y + 3, 4, 2, ray);
    if (err == ESP_OK) err = fill_rect(x + 7, y + 13, 4, 2, ray);
    if (err == ESP_OK) err = fill_rect(x + 3, y + 7, 2, 4, ray);
    if (err == ESP_OK) err = fill_rect(x + 13, y + 7, 2, 4, ray);
    if (err == ESP_OK) err = fill_rect(x + 4, y + 4, 2, 2, ray);
    if (err == ESP_OK) err = fill_rect(x + 12, y + 4, 2, 2, ray);
    if (err == ESP_OK) err = fill_rect(x + 4, y + 12, 2, 2, ray);
    if (err == ESP_OK) err = fill_rect(x + 12, y + 12, 2, 2, ray);
    return err;
}

static bool time_is_valid(void)
{
    return time(NULL) > 1700000000;
}

static void ensure_time_sync_started(void)
{
    if (s_time_sync_started || !wifi_manager_is_connected()) {
        return;
    }
    setenv("TZ", BRN_TIMEZONE, 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_init();
    s_time_sync_started = true;
    ESP_LOGI(TAG, "SNTP time sync started");
}

static void format_time(char *date_text, size_t date_size, char *time_text, size_t time_size)
{
    setenv("TZ", BRN_TIMEZONE, 1);
    tzset();
    strncpy(date_text, "SYNCING", date_size);
    date_text[date_size - 1] = '\0';
    strncpy(time_text, "--:--", time_size);
    time_text[time_size - 1] = '\0';

    time_t now = time(NULL);
    if (time_is_valid()) {
        struct tm local;
        localtime_r(&now, &local);
        strftime(date_text, date_size, "%m/%d %a", &local);
        strftime(time_text, time_size, "%H:%M", &local);
    }
}

static esp_err_t draw_wifi_screen_locked(bool has_credentials, bool timed_out)
{
    const char *title = timed_out ? "WIFI TIMEOUT" : (has_credentials ? "WIFI WAIT" : "SETUP WIFI");
    const char *detail = timed_out ? "OPEN SETUP PORTAL" : (has_credentials ? "CONNECTING..." : "AP 192.168.4.1");
    uint16_t state_color = timed_out || !has_credentials ? COLOR_WARN : COLOR_ACCENT;
    uint32_t pulse = (uint32_t)((xTaskGetTickCount() - s_init_tick) / pdMS_TO_TICKS(STATUS_REFRESH_MS));

    esp_err_t err = fill(COLOR_BG);
    if (err == ESP_OK) err = fill_rect(0, 0, DISPLAY_WIDTH, 34, COLOR_PANEL_DARK);
    if (err == ESP_OK) err = draw_text_at(12, 10, "BAREBRAIN", COLOR_WHITE, COLOR_PANEL_DARK, 1, 78);
    if (err == ESP_OK) err = draw_text_at(92, 10, "BOOT", RGB565(152, 219, 208), COLOR_PANEL_DARK, 1, 30);

    if (err == ESP_OK) err = draw_panel(10, 48, 108, 66);
    if (err == ESP_OK) err = draw_text_at(22, 62, title, COLOR_TEXT, COLOR_PANEL, 1, 84);
    if (err == ESP_OK) err = draw_text_at(22, 82, detail, COLOR_MUTED, COLOR_PANEL, 1, 84);
    for (int i = 0; err == ESP_OK && i < 4; ++i) {
        uint16_t color = ((pulse + (uint32_t)i) % 4 == 0) ? state_color : COLOR_LINE;
        err = draw_dot(42 + i * 12, 100, color);
    }

    if (err == ESP_OK) err = draw_text_at(12, 130, "WAIT FOR WIFI", COLOR_MUTED, COLOR_BG, 1, 104);
    if (err == ESP_OK && (timed_out || !has_credentials)) {
        err = draw_text_at(12, 144, "BRN-XXXX SETUP", COLOR_SKY, COLOR_BG, 1, 104);
    }
    return err;
}

static esp_err_t draw_dashboard_screen_locked(void)
{
    char date_text[16];
    char time_text[8];
    format_time(date_text, sizeof(date_text), time_text, sizeof(time_text));

    esp_err_t err = fill(COLOR_BG);
    if (err == ESP_OK) err = fill_rect(0, 0, DISPLAY_WIDTH, 27, COLOR_PANEL_DARK);
    if (err == ESP_OK) err = draw_text_at(8, 9, "BAREBRAIN", COLOR_WHITE, COLOR_PANEL_DARK, 1, 70);
    if (err == ESP_OK) err = draw_text_at(90, 9, "LIVE", RGB565(152, 219, 208), COLOR_PANEL_DARK, 1, 30);

    if (err == ESP_OK) err = draw_panel(6, 33, 116, 34);
    if (err == ESP_OK) err = draw_dot(14, 43, COLOR_ACCENT);
    if (err == ESP_OK) err = draw_text_at(27, 40, "WIFI OK", COLOR_TEXT, COLOR_PANEL, 1, 84);
    if (err == ESP_OK) err = draw_text_at(14, 54, wifi_manager_get_ip(), COLOR_SKY, COLOR_PANEL, 1, 96);

    if (err == ESP_OK) err = draw_panel(6, 74, 116, 40);
    if (err == ESP_OK) err = fill_rect(8, 76, 112, 10, COLOR_ACCENT_SOFT);
    if (err == ESP_OK) err = draw_text_at(14, 80, date_text, COLOR_MUTED, COLOR_ACCENT_SOFT, 1, 84);
    if (err == ESP_OK) err = draw_text_at(14, 96, time_text, COLOR_TEXT, COLOR_PANEL, 2, 72);

    if (err == ESP_OK) err = draw_panel(6, 121, 116, 33);
    if (err == ESP_OK) err = draw_text_at(14, 128, s_weather1, COLOR_ACCENT, COLOR_PANEL, 1, 66);
    if (err == ESP_OK) err = draw_text_at(14, 142, s_weather2, COLOR_MUTED, COLOR_PANEL, 1, 66);
    if (err == ESP_OK) err = draw_sun_icon(96, 130);
    return err;
}

static void copy_ascii_line(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    size_t out = 0;
    if (src) {
        for (const unsigned char *p = (const unsigned char *)src;
             *p && out + 1 < dst_size;
             ++p) {
            if (*p == '\r' || *p == '\n') {
                break;
            }
            if (*p < 0x80) {
                dst[out++] = (char)*p;
            } else if (out + 1 < dst_size) {
                dst[out++] = '?';
                while (p[1] >= 0x80 && p[1] < 0xC0) {
                    ++p;
                }
            }
        }
    }
    dst[out] = '\0';
}

static esp_err_t draw_status_screen_locked(void)
{
    bool connected = wifi_manager_is_connected();
    bool has_credentials = wifi_manager_has_credentials();
    uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - s_init_tick) * portTICK_PERIOD_MS);
    bool timed_out = has_credentials && !connected && elapsed_ms >= WIFI_TIMEOUT_MS;
    if (!connected) {
        return draw_wifi_screen_locked(has_credentials, timed_out);
    }
    ensure_time_sync_started();
    return draw_dashboard_screen_locked();
}

static void status_task(void *arg)
{
    (void)arg;
    while (true) {
        if (s_ready && xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
            esp_err_t err = draw_status_screen_locked();
            xSemaphoreGive(s_lock);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "status screen refresh failed: %s", esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(STATUS_REFRESH_MS));
    }
}

esp_err_t tool_display_init(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << BRN_PROFILE_DISPLAY_DC) |
                        (1ULL << BRN_PROFILE_DISPLAY_RESET) |
                        (1ULL << BRN_PROFILE_DISPLAY_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "configure display GPIO");
    gpio_set_level(BRN_PROFILE_DISPLAY_BACKLIGHT, 1);
    gpio_set_level(BRN_PROFILE_DISPLAY_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BRN_PROFILE_DISPLAY_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus = {
        .mosi_io_num = BRN_PROFILE_DISPLAY_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BRN_PROFILE_DISPLAY_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(DISPLAY_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "initialize SPI bus");

    spi_device_interface_config_t device = {
        .clock_speed_hz = DISPLAY_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = BRN_PROFILE_DISPLAY_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(DISPLAY_SPI_HOST, &device, &s_spi),
                        TAG, "attach ST7735S");

    static const uint8_t frame[] = {0x01, 0x2C, 0x2D};
    static const uint8_t frame3[] = {0x01,0x2C,0x2D,0x01,0x2C,0x2D};
    static const uint8_t gamma_pos[] = {
        0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,
        0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10
    };
    static const uint8_t gamma_neg[] = {
        0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,
        0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10
    };
    esp_err_t err = command(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));
    if (err == ESP_OK) err = command(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (err == ESP_OK) err = command_data(0xB1, frame, sizeof(frame));
    if (err == ESP_OK) err = command_data(0xB2, frame, sizeof(frame));
    if (err == ESP_OK) err = command_data(0xB3, frame3, sizeof(frame3));
    const uint8_t inv = 0x07, p1[] = {0xA2,0x02,0x84}, p2 = 0xC5;
    const uint8_t p3[] = {0x0A,0x00}, p4[] = {0x8A,0x2A}, p5[] = {0x8A,0xEE};
    const uint8_t vm = 0x0E, madctl = 0xC8, color_mode = 0x05;
    if (err == ESP_OK) err = command_data(0xB4, &inv, 1);
    if (err == ESP_OK) err = command_data(0xC0, p1, sizeof(p1));
    if (err == ESP_OK) err = command_data(0xC1, &p2, 1);
    if (err == ESP_OK) err = command_data(0xC2, p3, sizeof(p3));
    if (err == ESP_OK) err = command_data(0xC3, p4, sizeof(p4));
    if (err == ESP_OK) err = command_data(0xC4, p5, sizeof(p5));
    if (err == ESP_OK) err = command_data(0xC5, &vm, 1);
    if (err == ESP_OK) err = command_data(0x36, &madctl, 1);
    if (err == ESP_OK) err = command_data(0x3A, &color_mode, 1);
    if (err == ESP_OK) err = command_data(0xE0, gamma_pos, sizeof(gamma_pos));
    if (err == ESP_OK) err = command_data(0xE1, gamma_neg, sizeof(gamma_neg));
    if (err == ESP_OK) err = command(0x13);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (err == ESP_OK) err = command(0x29);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    gpio_set_level(BRN_PROFILE_DISPLAY_BACKLIGHT, 1);
    ESP_LOGI(TAG, "ST7735S display ready on SPI2");
    s_ready = true;
    s_init_tick = xTaskGetTickCount();
    esp_err_t screen_err = draw_status_screen_locked();
    if (screen_err != ESP_OK) {
        return screen_err;
    }
    BaseType_t task_ok = xTaskCreate(status_task, "display_status", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
    return task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t tool_display_execute(const char *input_json, char *output, size_t output_size)
{
    if (!input_json || !output || output_size == 0 || !s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
    if (!text || text[0] == '\0') {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'text' is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: display is busy");
        return ESP_ERR_TIMEOUT;
    }
    const char *weather1 = cJSON_GetStringValue(cJSON_GetObjectItem(root, "weather1"));
    const char *weather2 = cJSON_GetStringValue(cJSON_GetObjectItem(root, "weather2"));
    if (weather1 && weather1[0] != '\0') {
        copy_ascii_line(s_weather1, sizeof(s_weather1), weather1);
    }
    if (weather2 && weather2[0] != '\0') {
        copy_ascii_line(s_weather2, sizeof(s_weather2), weather2);
    } else {
        copy_ascii_line(s_weather2, sizeof(s_weather2), text);
    }
    esp_err_t err = draw_status_screen_locked();
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: display write failed (%s)", esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size, "Displayed text on ST7735S");
    return ESP_OK;
}
