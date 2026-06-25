# BareBrain Display Tool

Version 0.1.1 supports ST7735S-compatible 1.8-inch 128x160 SPI TFT modules.
The first hardware release renders wrapped ASCII text. Non-ASCII characters
are replaced with `?`.

BareBrain 的外部 SPI 屏幕插件。插件注册 `display_text` 工具，用于将文本、状态和调试信息输出到 SPI TFT 屏幕。

## 当前状态

- 已完成 BareBrain Mod manifest。
- 已完成工具注册和输入校验。
- 尚未确定屏幕控制器、总线类型和 GPIO。
- 在硬件配置完成前，工具会返回明确的未配置错误。

## 硬件接口

照片中的模块为 1.8 英寸 128x160 RGB TFT，接口标注为：

```text
GND VDD SCL SDA RST DC CS BLK
```

- `VDD` 必须先按卖家资料或模块数据手册确认供电电压；照片本身无法确认是 3.3V 还是 5V
- `GND` 接 `GND`
- `SCL/SCK`、`SDA/MOSI`、`RST`、`DC`、`CS`、`BLK` 在 Manager 引脚配置页自行选择

插件不提供默认 GPIO，避免与其他外设或板载功能冲突。

## 接入 BareBrain

云端构建下载 Release 后，将插件解压到：

```text
BareBrain/main/external_mods/tool-display/
```

固件 Profile 中启用：

```json
{
  "enabled_mods": ["tool-display"]
}
```
