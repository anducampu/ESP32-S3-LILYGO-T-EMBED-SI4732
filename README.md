# ESP32-S3 LILYGO T-Embed SI4732 — DSP Radio Receiver

A complete AM / FM / SW / LW + SSB DSP radio sketch for the
[**LILYGO T-Embed SI4732**](https://wiki.lilygo.cc/products/t-embed-series/t-embed-si4732/),
based on the PU2CLR `OLED_ALL_IN_ONE` example and Ralph Xavier's port to the
T-Display S3, with fixes and UX tweaks to make it work cleanly on the
**T-Embed SI4732** hardware running the **Arduino ESP32 core 3.x**.

## Features

- AM / FM / SW / LW reception with SSB (LSB / USB) demodulation
- 1, 5, 10, 50, 500 kHz steps on AM; 10 Hz step on SSB
- External-mute pin, AGC, attenuation, soft-mute, bandwidth control
- Encoder-driven UI with band menu (single-press) and full menu (double-press)
- 1.9" 320×170 ST7789V display driven via TFT_eSPI
- RDS (FM) and signal-strength indicators

## What changed vs upstream

This repo started from
[`Xinyuan-LilyGO/T-Embed/examples/SI473x_Shield`](https://github.com/Xinyuan-LilyGO/T-Embed/tree/main/examples/SI473x_Shield)
(which itself is a port of
[PU2CLR's OLED_ALL_IN_ONE](https://github.com/pu2clr/SI4735/tree/master/examples/SI47XX_06_ESP32/OLED_ALL_IN_ONE)
by [Ralph Xavier](https://github.com/ralphxavier), drawing UI inspiration from
[Volos's T-Embed FM radio](https://github.com/VolosR/TEmbedFMRadio)).

The original example was written against Arduino ESP32 core **2.x**. This
version makes it work on core **3.x** and refines the UX:

- **LEDC API migrated** from `ledcSetup` / `ledcAttachPin` / `ledcWrite(channel,…)`
  (core 2.x) to `ledcAttach(pin, freq, res)` / `ledcWrite(pin, …)` (core 3.x).
- **TFT_eSPI patch (external)**: on ESP32-S3 + core 3.x, TFT_eSPI's default
  `SPI_PORT = FSPI` resolves to `0`, which crashes inside `tft.begin()` with
  `Guru Meditation: StoreProhibited, EXCVADDR=0x10`. Workaround:
  `#define USE_HSPI_PORT` in the active User_Setup file. Details and the exact
  edits live in [`LIBRARY_PATCHES.md`](./LIBRARY_PATCHES.md).
- **Display setup** switched to `Setup210_LilyGo_T_Embed_S3.h` with RGB colour
  order enabled.
- **UX: double-press of the encoder** now opens the menu with the cursor on
  **Volume** (was Band) — quicker volume access.
- **UX: encoder direction reversed** — turning the encoder **left** now
  *increments* (frequency, volume, etc.). Personal preference; one-line flip
  in the `rotaryEncoder()` ISR if you want the original behaviour back.
- **UI colours**: peak-signal bars, frequency-grid centre marker, and the
  "FM Stereo" indicator are **green** instead of red.
- **Boot diagnostics**: `Serial.begin(115200)` + boot prints in `setup()` so
  USB-CDC shows the boot flow and any crash trace.

## Build & flash

See [`LIBRARY_PATCHES.md`](./LIBRARY_PATCHES.md) for the full step-by-step:
required library versions, the TFT_eSPI edits (in
`Arduino/libraries/TFT_eSPI/User_Setup_Select.h` and
`User_Setups/Setup210_LilyGo_T_Embed_S3.h`), the Arduino IDE board settings
from the LilyGO wiki, and the equivalent `arduino-cli` FQBN.

Quick FQBN reference:

```
esp32:esp32:esp32s3:PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,LoopCore=1,EventsCore=1,EraseFlash=none,UploadSpeed=921600,DebugLevel=none
```

## Hardware

- LILYGO **T-Embed SI4732** (ESP32-S3, 16 MB flash, 8 MB OPI PSRAM, ST7789V
  1.9" 320×170 display, SI4732-A10 DSP receiver, rotary encoder with push)
- Power on GPIO46, backlight on GPIO15, encoder button on GPIO0 (also BOOT
  strapping pin — pressing it during reset enters download mode)
- SI4732 on I²C: SDA=GPIO18, SCL=GPIO8, RESET=GPIO16
- Audio mute control: GPIO17

## Controls

- **Rotary encoder turn** — change frequency, or change the selected value
  when in menu mode
- **Single click** — Band selection mode (rotate to pick band; click again to
  exit)
- **Double click** — Full menu (Volume, Step, Mode, BFO, BW, AGC/Att,
  SoftMute, Seek Up / Down, Band, Mute). Defaults to Volume.

## Credits

- **[PU2CLR (Ricardo Caratti)](https://github.com/pu2clr/SI4735)** — original
  SI4735 library and `OLED_ALL_IN_ONE` example
- **[Ralph Xavier](https://github.com/ralphxavier)** — port to LilyGO
  T-Display S3
- **[VolosR](https://github.com/VolosR/TEmbedFMRadio)** — UI inspiration
- **[Xinyuan-LilyGO](https://github.com/Xinyuan-LilyGO/T-Embed)** — T-Embed
  hardware and the SI473x_Shield example this is forked from
- **Ben Buxton** — `Rotary.cpp` encoder library (GPL-3)

## License

GPL-3.0 (inherited via `Rotary.cpp` and the PU2CLR SI4735 library). See
[`LICENSE`](./LICENSE).

## Disclaimer

The author does not guarantee these procedures will work in your environment.
Proceed at your own risk.
