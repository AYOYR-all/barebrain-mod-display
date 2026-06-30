# BareBrain SPI Display Tool

External BareBrain mod for ST7735S-compatible 1.8-inch 128x160 SPI TFT modules.

Version 0.1.3 renders a mimiclaw-style status dashboard instead of a plain
black text page. The screen shows WiFi state, IP/setup hint, a date/time card,
and a weather card. Non-ASCII weather text is shown as `?`.

## Hardware

The supported display module uses these pins:

```text
GND VDD SCL SDA RST DC CS BLK
```

- `VDD`: verify the module voltage from the seller or datasheet first.
- `GND`: connect to board ground.
- `SCL/SCK`, `SDA/MOSI`, `RST`, `DC`, `CS`, and `BLK`: choose pins in the BareBrain Manager profile.

The mod does not provide default GPIO choices so it will not silently conflict
with other peripherals.

## BareBrain Usage

Enable the mod in the firmware profile:

```json
{
  "enabled_mods": ["tool-display"]
}
```

After boot, the display refreshes automatically every few seconds. The
registered `display_text` tool can update the weather card while keeping WiFi
and date/time status visible.
