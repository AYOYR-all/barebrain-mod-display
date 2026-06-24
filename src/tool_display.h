#pragma once

#include <stddef.h>

#include "esp_err.h"

esp_err_t tool_display_init(void);
esp_err_t tool_display_execute(const char *input_json, char *output, size_t output_size);
