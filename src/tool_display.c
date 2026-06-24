#include "tool_display.h"

#include <stdio.h>

#include "cJSON.h"

esp_err_t tool_display_init(void)
{
    /*
     * The display controller, bus and GPIO mapping will be configured after
     * the target hardware is selected.
     */
    return ESP_OK;
}

esp_err_t tool_display_execute(const char *input_json, char *output, size_t output_size)
{
    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
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

    cJSON_Delete(root);
    snprintf(output, output_size,
             "Error: display hardware and GPIO configuration are not set");
    return ESP_ERR_NOT_SUPPORTED;
}
