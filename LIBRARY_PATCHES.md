# External modifications needed to build this sketch

_Created: 2026-05-22 14:07 EEST_
_Last updated: 2026-05-22 14:54 EEST_

This sketch depends on changes made *outside* the project folder. When porting
to another machine, replicate the changes below after installing the required
libraries via the Arduino Library Manager.

## Environment used

- Arduino IDE / arduino-cli
- ESP32 Arduino core **3.3.8** (board package `esp32:esp32`)
- Libraries (current versions known to work):
  - TFT_eSPI 2.5.43
  - PU2CLR SI4735 2.1.8
  - PU2CLR RDA5807 1.1.9
  - PU2CLR SI470X 1.0.5
  - PU2CLR SI4844 1.2.2
  - RotaryEncoder 1.6.0
  - OneButton (mathertel)
  - Battery18650Stats (danilopinotti)

Library paths below assume the Arduino sketchbook is `~/Arduino`. Adjust if
yours lives elsewhere.

---

## 1. TFT_eSPI: select the T-Embed S3 setup

File: `~/Arduino/libraries/TFT_eSPI/User_Setup_Select.h`

Comment out the T-Dongle S3 line, uncomment the T-Embed S3 line:

```diff
-#include <User_Setups/Setup209_LilyGo_T_Dongle_S3.h>      // For the LilyGo T-Dongle S3 ...
-//#include <User_Setups/Setup210_LilyGo_T_Embed_S3.h>     // For the LilyGo T-Embed S3 ...
+//#include <User_Setups/Setup209_LilyGo_T_Dongle_S3.h>    // For the LilyGo T-Dongle S3 ...
+#include <User_Setups/Setup210_LilyGo_T_Embed_S3.h>       // For the LilyGo T-Embed S3 ...
```

## 2. TFT_eSPI: patch Setup210 for ESP32-S3 + core 3.x

File: `~/Arduino/libraries/TFT_eSPI/User_Setups/Setup210_LilyGo_T_Embed_S3.h`

Two edits:

**a)** Force HSPI port. arduino-esp32 v3.x changed `FSPI` from `1` to `0`,
which makes `REG_SPI_BASE(FSPI)` return `0` on the S3. The default crashes
with `Guru Meditation: StoreProhibited` at `EXCVADDR=0x00000010` inside
`tft.begin()`. Forcing HSPI gives a valid SPI peripheral base.

```diff
 #define USER_SETUP_ID 210

+// arduino-esp32 v3.x changed FSPI=0 (invalid index for REG_SPI_BASE on S3),
+// so force HSPI (SPI3) explicitly to get a valid SPI peripheral base.
+#define USE_HSPI_PORT
+
 #define ST7789_DRIVER     // Configure all registers
```

**b)** Set RGB colour order (the panel on this device wants RGB, not the
default BGR):

```diff
-//#define TFT_RGB_ORDER TFT_RGB  // Colour order Red-Green-Blue
+#define TFT_RGB_ORDER TFT_RGB  // Colour order Red-Green-Blue
 //#define TFT_RGB_ORDER TFT_BGR  // Colour order Blue-Green-Red
```

Nothing else in Setup210 needs to change.

---

## 3. Sketch-internal changes (already in this folder, listed for completeness)

- `ESP32_S3_LILYGO_T_EMBED_SI4732.ino`: migrated LEDC API from core 2.x
  (`ledcSetup`/`ledcAttachPin`/`ledcWrite(channel,...)`) to core 3.x
  (`ledcAttach(pin,freq,res)`/`ledcWrite(pin,...)`).
- `ESP32_S3_LILYGO_T_EMBED_SI4732.ino`: added `Serial.begin(115200)` + boot
  diagnostic prints in `setup()`. Harmless; useful when debugging.
- UX: double-press of the encoder now enters the menu with selection defaulting
  to **Volume** (was Band) — easier volume access. See the
  `cmdMenu = !cmdMenu;` block in the click handler (`menuIdx = VOLUME;` set on
  entering menu mode).
- UI colour tweaks: peak signal bars (>=10), the frequency-grid center marker /
  vertical line / triangle, and the "FM Stereo" indicator are now **green**
  (`TFT_GREEN`) instead of red — green reads as a positive/locked indicator.
- Encoder direction reversed: turning **left** (CCW) now increments frequency,
  volume, etc. One-line change in `rotaryEncoder()` ISR — flips the CW/CCW
  mapping at the source, so every consumer of `encoderCount` follows.

---

## 4. Arduino IDE board settings (per LilyGO wiki)

Tools → Board → ESP32S3 Dev Module, then:

| Setting                              | Value                                       |
|--------------------------------------|---------------------------------------------|
| USB CDC On Boot                      | Enable                                      |
| CPU Frequency                        | 240 MHz (WiFi)                              |
| Core Debug Level                     | None                                        |
| USB DFU On Boot                      | Disable                                     |
| Erase All Flash Before Sketch Upload | Disable                                     |
| Events Run On                        | Core1                                       |
| Flash Mode                           | QIO 80 MHz                                  |
| Flash Size                           | 16MB (128Mb)                                |
| Arduino Runs On                      | Core1                                       |
| USB Firmware MSC On Boot             | Disable                                     |
| Partition Scheme                     | 16M Flash (3MB APP/9.9MB FATFS)             |
| PSRAM                                | OPI PSRAM                                   |
| Upload Mode                          | UART0/Hardware CDC                          |
| Upload Speed                         | 921600                                      |
| USB Mode                             | CDC and JTAG                                |

Equivalent arduino-cli FQBN:

```
esp32:esp32:esp32s3:PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,LoopCore=1,EventsCore=1,EraseFlash=none,UploadSpeed=921600,DebugLevel=none
```

---

## 5. Flashing notes

- The rotary-encoder push button is on **GPIO0** (the BOOT strapping pin).
  Pressing it during reset puts the chip into download mode.
- esptool's "Hard resetting via RTS pin" is unreliable on the S3
  USB-Serial/JTAG port. After upload, a physical **RST** tap (without
  touching the encoder) is the most reliable way to start the firmware.
- To force download mode manually: hold **BOOT** (rotary encoder), tap
  **RST**, release both. Then upload. Then tap **RST** to run.
