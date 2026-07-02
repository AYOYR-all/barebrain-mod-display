# BareBrain SPI Display Tool

External BareBrain mod for ST7735S-compatible 1.8-inch 128x160 SPI TFT modules.

Version 0.1.6 shows a WiFi waiting/setup screen first, then switches to a
white-header, dark-body dashboard after WiFi connects. The dashboard shows
synced Beijing time above the WiFi card, and a default `WEATHER / SUNNY` card
with a gold sun icon. Non-ASCII weather text is shown as `?`.

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
