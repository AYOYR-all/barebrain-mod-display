# Changelog

## 0.1.5

- Switch the dashboard to a white header and dark body for better contrast.
- Improve the WiFi waiting screen layout.
- Use a yellow sun icon and default `WEATHER / SUNNY` card.
- Add a shadowed, brighter time display.

## 0.1.4

- Add a WiFi waiting/setup screen before the main dashboard.
- Start SNTP time sync automatically after WiFi connects.
- Refresh the dashboard colors and card layout.
- Show Beijing time and a default sunny weather card with a sun icon.

## 0.1.3

- Render a mimiclaw-style status dashboard with WiFi, date/time, and weather cards.
- Show `WIFI OK` with the current IP after connection.
- Show setup information when WiFi is missing or times out.
- Keep `display_text` compatible while allowing optional weather card updates.

## 0.1.2

- Display `READY` after the ST7735S screen initializes successfully.

## 0.1.1

- Add a real ST7735S-compatible 128x160 SPI display driver.
- Read all six GPIO assignments from the generated firmware Profile.
- Add screen initialization, clear, wrapping, and ASCII text rendering.

## 0.1.0

- 创建 BareBrain 外部 Mod 结构。
- 注册 `display_text` 工具。
- 添加 JSON 输入校验。
- 为后续显示屏和 GPIO 配置预留实现位置。
