#include "tool_display.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "generated/brn_profile_config.h"

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
#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF

static const char *TAG = "tool_display";
static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_lock;
static bool s_ready;

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

    unsigned char value = (unsigned char)input;
    if (value == ' ') return blank;
    if (value >= '0' && value <= '9') return digits[value - '0'];
    value = (unsigned char)toupper(value);
    if (value >= 'A' && value <= 'Z') return letters[value - 'A'];
    if (value == '-') return dash;
    if (value == '.') return dot;
    if (value == ':') return colon;
    if (value == '/') return slash;
    return unknown;
}

static esp_err_t draw_character(uint16_t x, uint16_t y, char value)
{
    const uint8_t *bitmap = glyph(value);
    uint8_t pixels[6 * 8 * 2];
    size_t offset = 0;
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 6; ++column) {
            bool active = column < 5 && (bitmap[column] & (1U << row));
            uint16_t color = active ? COLOR_WHITE : COLOR_BLACK;
            pixels[offset++] = color >> 8;
            pixels[offset++] = color & 0xFF;
        }
    }
    esp_err_t err = set_window(x, y, 6, 8);
    return err == ESP_OK ? transmit(true, pixels, sizeof(pixels)) : err;
}

static esp_err_t draw_text(const char *text)
{
    uint16_t x = 1;
    uint16_t y = 1;
    esp_err_t err = fill(COLOR_BLACK);
    for (const unsigned char *cursor = (const unsigned char *)text;
         err == ESP_OK && *cursor != '\0' && y + 8 <= DISPLAY_HEIGHT;
         ++cursor) {
        if (*cursor == '\n' || x + 6 > DISPLAY_WIDTH) {
            x = 1;
            y += 8;
            if (*cursor == '\n') {
                continue;
            }
        }
        char value = (*cursor < 0x80) ? (char)*cursor : '?';
        err = draw_character(x, y, value);
        x += 6;
        while (cursor[1] >= 0x80 && cursor[1] < 0xC0) {
            ++cursor;
        }
    }
    return err;
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
    gpio_set_level(BRN_PROFILE_DISPLAY_BACKLIGHT, 0);
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
    s_ready = true;
    gpio_set_level(BRN_PROFILE_DISPLAY_BACKLIGHT, 1);
    ESP_LOGI(TAG, "ST7735S display ready on SPI2");
    return fill(COLOR_BLACK);
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
    esp_err_t err = draw_text(text);
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: display write failed (%s)", esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size, "Displayed text on ST7735S");
    return ESP_OK;
}
