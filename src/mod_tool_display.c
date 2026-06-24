#include "core/mod/brn_mod.h"
#include "tool_display.h"
#include "tools/tool_registry.h"

static const char *const tool_display_deps[] = {
    "core.tool_registry",
    NULL,
};

static const brn_tool_t display_text_tool = {
    .name = "display_text",
    .description = "Display ASCII text on the configured ST7735S 128x160 TFT screen.",
    .input_schema_json =
        "{\"type\":\"object\","
        "\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to display\"}},"
        "\"required\":[\"text\"]}",
    .execute = tool_display_execute,
};

static esp_err_t tool_display_mod_init(void)
{
    esp_err_t err = tool_display_init();
    if (err != ESP_OK) {
        return err;
    }
    return brn_tool_register(&display_text_tool);
}

const brn_mod_t brn_mod_tool_display = {
    .id = "tool-display",
    .name = "Display Tool",
    .version = "0.1.1",
    .deps = tool_display_deps,
    .init = tool_display_mod_init,
};
