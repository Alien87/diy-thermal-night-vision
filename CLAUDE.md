# Project: ESP32-P4 thermal night-vision scope

## Goal
Build a standalone thermal scope: a USB thermal camera plugged into an ESP32-P4 board, with the image shown on a small eyepiece display. No Linux, so it boots almost instantly. This previously worked on a Raspberry Pi; the goal is to replace the Pi with a microcontroller.

## Hardware
- **Board:** Waveshare ESP32-P4-NANO
  - ESP32-P4 with 32 MB PSRAM in the chip package and 16 MB NOR flash
  - ESP32-C6 coprocessor over SDIO, for Wi-Fi 6 and BLE
  - Type-A port: USB 2.0 OTG **High-Speed** host. The camera goes here, through a USB-A to USB-C adapter.
  - Type-C port: CH343 USB-UART bridge, used for flashing and the serial monitor
- **Camera:** HIKMICRO Mini2Plus V2
  - 256×192 VOx sensor, 25 Hz
  - USB-C phone dongle, powered over USB
  - Enumerates as standard UVC; confirmed working on a Pi
  - Image stream is **NV12 256×192 @ 25 fps**, the same mode ffplay uses on the PC (`-video_size 256x192 -pixel_format nv12 -framerate 25`). Only the image is needed, not the temperature data.
  - UVC descriptors (bulk endpoint 0x81, MPS 512, UVC 1.1):
    - Format 1 YUY2 has frames idx 1/3/4/8/9: 256×344, 256×196, 5521×16, 256×360, 463×220. These are temperature/raw modes and are ignored.
    - Format 2 NV12 has frame **idx 2**: 256×192 @ 25 fps (interval 400000).
    - Every frame declares dwMaxVideoFrameBufferSize 209920.
    - Power-on default is format 1 / frame 1, and the camera keeps the last committed mode until it is unplugged.
  - For a greyscale or palette image only the luma plane (the first 256×192 bytes) is used.
- **Displays:**
  1. A small round OLED/LCD (SPI). This is the first target.
  2. A micro display with composite AV video input. This comes later.
     - Its driver board: 7-pin input connector → **TI TVP5150AM1** (U5, composite decoder; auto-detects PAL/NTSC; outputs 8-bit BT.656 at 27 MHz) → **Anlogic EG4S20NG88** FPGA (U3, scaler) → FPC J15 → panel.
     - **Plan (decided 2026-10-07): an original-ESP32 daughter board generates composite** from its internal 8-bit DAC (I2S→DAC, ESP_8_BIT-style; colour PAL/NTSC). Its APLL is tuned to 4×fsc, so no resistor ladder or external oscillator is needed. The P4 (now the ESP32-P4-Pico, on hand) sends 8-bit frames over SPI.
       - Link budget: 256×192×25 fps ≈ 1.23 MB/s ≈ 9.8 Mbit/s, which is at the edge of the ESP32 SPI slave limit. Use the native IOMUX SPI pins (HSPI 14/13/12/15 or VSPI 18/23/19/5) and aim for about 20 MHz. Fallbacks: make the ESP32 the SPI master, or reduce the data.
       - DAC output: GPIO25 (DAC1) or GPIO26 (DAC2) → CVBS (pin 3), with a common GND. A 256×240 8-bit framebuffer is 61 KB, which fits in internal RAM, so no PSRAM is needed.
       - Daughter firmware lives in `composite/`, a separate PlatformIO project (`pio run -d composite`). It's Arduino with the ESP_8_BIT_composite library, `ESP_8_BIT_composite(false)` for PAL. It is pinned to the official `espressif32@6.5.0` platform (Arduino core 2.0.14, the author's last verified version) because the library breaks on core 3.x. Envs: `esp32dev` (full-size boards) and `d1mini32` (the ordered CH9102F "Mini" boards).
       - Stage 1 **working on hardware (2026-10-07)**: the test pattern (colour bars, grey ramp, square, reticle) shows in **colour PAL** on the eyepiece, driven straight from GPIO25 into pin 3 with no extra parts. There is slight blurring at the colour bar edges, which is normal for composite's limited chroma bandwidth, possibly made worse by temporary wiring.
       - If uploads fail with "No serial data received", hold BOOT and tap EN; some DevKits don't auto-reset.
       - **The picture froze on the first frame** even though the ESP32 was swapping buffers 50×/s. Cause: the stock library outputs 312-line non-interlaced PAL, every field identical, and the eyepiece's FPGA scaler apparently only updates on alternating odd/even fields.
       - Fix: the library is now **vendored and patched** in `composite/lib/ESP_8_BIT_composite`, with `LOCAL PATCH` markers and `pal_line()`. It outputs full 625-line interlaced PAL with standard half-line equalising/broad pulses. Both fields show the same 240-row framebuffer (rows at lines 48+ and 360+), and buffers swap once per 625-line frame (25 Hz). **Confirmed on hardware 2026-10-07**: clear image, moving square updates smoothly.
       - D1 Mini32 (CH9102F) boards arrived 2026-10-08: the test pattern uploads with `-e d1mini32` and works on the eyepiece with no code changes.
       - Thermal palettes (2026-10-08, **confirmed on hardware**: ramps and all three palettes look right on the eyepiece): third `LOCAL PATCH` in the library. `setPalette(rgb[256][3])` generates the PAL tables at runtime with the same encoding fitted from the stock RGB332 `pal_yuyv` table (luma + 0.567·span·(R−Y) / 0.328·span·(B−Y) at the 4 sample phases, s0/s2 swapped on V-switched lines). Black is moved to the PAL blanking level (no 7.5 IRE setup).
         - The DAC only gives **53 luma steps** from black to white, so "256-level grey" is impossible on the wire. `setFieldDither(true)` (default) rounds field 1 down and field 2 up by a quarter step, giving 106 levels as an interlaced pair. If the eyepiece shows 12.5 Hz flicker, turn it off.
         - `src/palettes.cpp/.h`: white-hot / black-hot / ironbow (same stops as the P4's `processing.c`). Indices 0–253 = ramp, 254 = reticle (red / cyan on ironbow), 255 = white overlay. `thermal_index()` maps 0–255 luma onto 0–253.
         - Test pattern: smooth ramp, 16 steps, moving hot square, reticle. Auto-cycles palettes every 5 s; serial keys `p` / `a` / `d`.
       - SPI link (2026-10-08, both sides build, **not yet tested on hardware**). Protocol, chunk format and wiring are in `shared/link_protocol.h`, which both projects include. Tested on the NANO first, because the Pico needs soldering.
         - Each frame is the raw 256×192 luma, sent as 12 chunks (an 8-byte header plus 16 rows = 4104 bytes). The header carries a magic number, frame seq, chunk number, palette and a 16-bit sum checksum.
         - A handshake line (daughter IO4 → P4 GPIO48) goes high when the slave has a buffer loaded. The P4 waits for a rising edge before each chunk.
         - P4: `src/composite_link.c/.h`, SPI3 master at 10 MHz (bring-up; aim for 20). It runs in its own task and drops frames if busy, so it never stalls the LCD. Counters are logged in the 1 s stats line.
           - Pins: **Pico** SCLK 14, MOSI 15, CS 16, handshake 17 (`-DBOARD_ESP32_P4_PICO`, env `esp32-p4-pico`). **NANO** 45/46/47/48.
           - NANO hardware test (2026-10-12): handshake and CS worked, but every chunk was short (1.5k–13k of 32832 clocks at 10 MHz). At 1 MHz about 25% of chunks were corrupt; a couple of frames got through.
           - Suspected cause, from the schematics: GPIO39–48 are a separate I/O domain fed by LDO4, which defaults to 1.8 V. That is below the original ESP32's ~2.5 V input-high threshold, and the daughter's 3.3 V handshake overdrives GPIO48. GPIO45 also gates the SD card power MOSFET (Q1) on both boards. Unconfirmed: measure the NANO header pin `ESP_LDO_VO4`. The temporary wiring was also poor.
           - The daughter logs the bit count of the last short transfer (`short N (last X of 32832 bits)`).
         - Daughter: `composite/src/link.cpp/.h`, VSPI slave (SCLK 18, MOSI 23, CS 5; MISO unused), 3 queued DMA buffers. `main.cpp` maps luma through `thermal_index()`, stretches 192 → 240 rows bilinearly (composite pixels are 1.25× wider than tall, so this gives a correct 4:3), and draws the reticle. The palette follows the P4's header. It falls back to the test pattern after 1 s with no frames.
     - Earlier option (dropped 2026-10-07): a P4 PARLIO resistor ladder plus an external 4×fsc oscillator for colour.
     - Advanced option: hold the TVP5150 off and inject BT.656 from the P4 straight into the FPGA. Fine-pitch soldering.
     - Power: the scope has two built-in supplies, 3.3 V for the display controller board and 5 V for the ESP32-P4.
     - Driver board silk "DB V1.3". Back side: 50 MHz oscillator (FPGA), a second oscillator U14 (probably the TVP5150 crystal), two DC-DC converters, a 3-pad header labelled roughly GND/RX/TX (probably a service UART, not a video input; unprobed), and the 7-pin connector along the bottom edge.
     - Panel: a small module on a carrier PCB with a fine-pitch board-to-board connector, label "D5H01413#296". Adapter flex "MB000_FPC39TO35_01" (2020-11-16); the name suggests a 39→35-pin adapter, or a 0.39" panel. Probably a micro-OLED, so driving it directly from the P4 is unlikely to be practical. Composite into the existing board stays the plan.
     - 7-pin connector, measured 2026-10-06: 1 = 3.5 V, 2 = GND, 3 = CVBS, 4 = GND, 5 = 10.5 V, 6 = GND, 7 = 4 V (battery voltage). Not yet known which power pins are inputs and which are outputs. The video source connects only to CVBS (3) and GND.
     - Still needed: unpowered resistance from CVBS to GND (expected about 75 Ω), and what pin 5 (10.5 V) and pin 7 (battery) feed.

## Why the P4
The ESP32-S3 USB host is full-speed only (12 Mbps). The camera's stream is about 2.5–5 MB/s, so it needs the P4's high-speed host.

## Toolchain
VS Code + PlatformIO, using the community pioarduino platform, because the official one doesn't support the P4. Use ESP-IDF, not Arduino, because Espressif's `usb_host_uvc` component is IDF-only.

```ini
[env:esp32-p4-nano]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board = esp32-p4-evboard
framework = espidf
monitor_speed = 115200
upload_speed = 921600
```

## Milestones
1. **Hello world:** confirm serial output and note the chip revision from the boot log. Some NANOs have an older P4 revision that may need an sdkconfig option.
2. **Enable PSRAM** in sdkconfig and add the `espressif/usb_host_uvc` component via `idf_component.yml`. Base the code on Espressif's `basic_uvc_stream` example.
3. **Camera enumeration:** log the camera's descriptors, formats, resolutions and frame rates, and confirm frames arrive at high speed.
4. **Frame parsing:** identify the image and any temperature-data region, then convert to an 8-bit greyscale or palette image (white-hot / black-hot / ironbow).
5. **Display:** scale and show the image on the round SPI display with a reticle overlay. Aim for low latency at the camera's 25 fps.
6. **Later:**
   - Composite output via a helper ESP32
   - Buttons or touch for palette and zoom
   - Battery power: one 18650 cell with a 5V boost converter feeding the board and the camera

## Power build (2026-10-12, in progress)
- Parts: a 3.7 V 1300 mAh 883048 LiPo, a TP4056 charger with protection, a mini adjustable boost (3.7 → 5–12 V, LED; set to 5.0 V) and an AMS1117-3.3 module (4.75–12 V in, 800 mA).
- Plan: TP4056 `OUT±` → switch → boost 5 V → P4 5 V pin (the camera runs from its USB VBUS), daughter VIN, and AMS1117 → 3.3 V for the eyepiece display board. All grounds common. This replaces the earlier 18650 idea.
- Runtime is not yet measured; the rough estimate is 1.5–2 h. The user will take photos while shrinking the wiring and fitting it into the housing, for the GitHub repo.

## Notes
- Use Espressif's official IDF components. A third-party MicroPython-style USB library has a known bug where the P4 HS host never detects devices.
- The camera needs 5V on the Type-A port. Check VBUS is live when debugging enumeration.
- Keep the code modular (camera / processing / display), so the display backend can be swapped and the code can later move to a smaller ESP32-P4-Pico.

## Status
- Milestone 1 done (2026-10-05): builds, flashes and logs over the CH343 at 115200.
  - Chip revision **v3.1** (newest P4 revision; no old-revision workaround needed). ESP-IDF 5.5.5 via pioarduino.
  - Flash detected as 16 MB (GD, DIO, 80 MHz). Console UART on GPIO 37/38.
- Milestones 2 and 3 done (2026-10-05). NV12 256×192 streams at a steady **25.0 fps, 1.76 MB/s** over HS bulk, with no overflow or underflow.
  - Frames were 73768 bytes: 40 bytes of leaked payload headers, now fixed in `uvc_bulk.c`. They should be exactly 73728.
  - Luma reads 0–255 for the first second, then settles to about 2–245 (camera AGC/NUC settling).
  - Set the `camera` log tag to DEBUG to get the raw descriptor hex dump and `uvc_host_desc_print`.
  - `sdkconfig.defaults`: PSRAM at 200 MHz, 16 MB flash, custom `partitions.csv` (4 MB app), 1 kHz tick.
    After editing it, delete `sdkconfig.esp32-p4-nano` so it is regenerated.
  - `usb_host_uvc` 2.6.0 via `src/idf_component.yml`; USB host on the HS controller (`peripheral_map = BIT0`).
  - `src/camera.c/.h`: USB/UVC module. Logs formats, selects NV12 256×192, streams into PSRAM buffers, handles hot-plug.
  - `src/main.c`: logs fps / bytes per frame / MB/s, a hex dump of the first frame, and the luma range once a second.
  - PSRAM confirmed on hardware (32 MB). Camera enumerates once the control transfer size is 4096.
- Milestones 4 and 5 working on hardware (2026-10-05): live thermal image on the GC9A01 with the reticle.
  - `src/processing.c/.h`:
    - Takes the NV12 luma plane, crops a centred 192×192 square and scales it **bilinearly** to 240×240. Nearest-neighbour at 1.25× gave stepped "scan line" edges.
    - Maps through a 256-entry RGB565 palette LUT (white-hot / black-hot / ironbow), byte-swapped for SPI.
    - Draws the reticle: red on the greyscale palettes, cyan on ironbow.
  - `src/display.c/.h`: GC9A01 via `espressif/esp_lcd_gc9a01` on SPI2 at 40 MHz. Two 115 KB internal DMA buffers; the next frame renders while the previous one transfers. Panel settings: BGR, inverted colours, mirror X.
  - GC9A01 wiring (NANO header): SCLK **20**, MOSI/SDA **21**, CS **22**, DC **23**, RST **32**, BL **33**, VCC 3V3, GND. Pins are defined at the top of `display.c`.
  - NANO pins in use on the board: I2C 7/8, I2S 9–13, C6 SDIO 14–19 and reset 54, Ethernet 28–31/34/35/49–52, UART 37/38, SD 39–44, speaker amp 53. GPIO 34–38 are strapping pins and 24–27 are USB PHY pins.
- 2026-10-05: clean image confirmed on hardware after the bulk header fix: straight edges, no scan lines. Bilinear scaling kept.
- Palette button working on hardware (2026-10-05). `src/input.c/.h` polls the BOOT button (GPIO35, active low, 30 ms debounce) and sends `INPUT_EVENT_PALETTE_NEXT`; main cycles white-hot → black-hot → ironbow. Zoom is deferred.
- Current milestone: 6 (remaining: composite output via helper ESP32, battery power, optional zoom, Pico port).

## Porting to ESP32-P4-Pico (-M = pre-soldered headers)
- Same SoC family (ESP32-P4NRW32, 32 MB in-package PSRAM) with 32 MB flash. No ESP32-C6, so no Wi-Fi/BLE (not needed).
- Type-C is power plus a CH343P UART for flashing.
- **USB HS is on a 4-pin MX1.25 header**, not a Type-A port. The camera needs an MX1.25 → USB-A/USB-C cable. Check the header supplies 5 V VBUS; if it doesn't, feed the camera 5 V separately.
- **Chip revision:** early Pico units have **v1.0** silicon (360 MHz). Check the boot log. Below v3.0 needs a separate build with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`; v1.x and v3.x binaries are not interchangeable. If needed, add a second PlatformIO env with its own sdkconfig defaults.
- Same onboard pins as the NANO for I2C 7/8, I2S 9–13 / PA 53 and SD 39–44 (SD power switch on GPIO45). BOOT is GPIO35.
- From the schematic (2026-10-12), the header GPIOs are 2–8, 14–23, 26, 27, 32, 33, 36, 46–48 and 54. The display pins (20–23, 32, 33) are all on it. GPIO14–19 are free because there is no C6. The schematic only gives "GPx" pad names, so header positions still need checking against Waveshare's pinout image.
- PlatformIO env `esp32-p4-pico` extends the NANO env, adding `-DBOARD_ESP32_P4_PICO` (composite link pins) and using 16 MB of the 32 MB flash.
- **This Pico has v1.x silicon** (2026-10-12): ROM banner `esp32p4-eco2-20240710`, and the v3 build panicked with an illegal instruction at the bootloader entry.
  - `sdkconfig.defaults.pico` sets `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` (rev min 0, CPU 360 MHz, PSRAM 200 MHz kept).
  - The top-level `CMakeLists.txt` layers it on `sdkconfig.defaults` when `SDKCONFIG` ends in `sdkconfig.esp32-p4-pico`.
  - After editing either defaults file, delete `sdkconfig.esp32-p4-pico`.
  - **Boots on hardware (2026-10-12):** chip **v1.3**, 360 MHz, 32 MB PSRAM passes its test at 200 MHz, flash reads as 32 MB (GD, DIO). The GC9A01 initialises, the link starts on pins 14–17, and the app waits for the camera.
  - Camera on the Pico's 4-pin USB HS header (2026-10-12):
    - With the first cable, enumeration failed at the first descriptor request: `USBH: Dev 0 EP 0 Error` / `CHECK_SHORT_DEV_DESC FAILED`. This is a signal-integrity problem; the cable was reworked.
    - After that the camera works, but streaming stops if the cable is disturbed. A better MX1.25 → USB adapter is to be sourced.
  - **Composite link working on the Pico (2026-10-12):** live image on the eyepiece.
    - At 10 MHz: 13 fps, 156 chunks/s, 0 bad header / checksum / short / partial. This supports the NANO 1.8 V-domain theory.
    - 13 fps rather than 25 because a frame takes about 40 ms at 10 MHz, so every other frame is dropped. The clock was raised to 20 MHz.
    - **At 20 MHz: full 25 fps, 300 chunks/s, zero errors (confirmed on hardware).** The P4 → daughter → composite chain is complete.

## Build environment notes
- The pioarduino platform needs Git on PATH (installed via winget).
- If a build fails with `MissingPackageManifestError`, a package download in `~/.platformio/packages` was interrupted (it happened to `framework-espidf`). Delete that package folder and rebuild.
- A build failing with `PermissionError: [WinError 32]` means something has a file or folder in `managed_components/` open (e.g. a shell `cd`'d into it). Close it and rebuild.
- **`usb_host_uvc` is vendored and patched** in `components/usb_host_uvc` (2.6.0), referenced through `override_path` in `src/idf_component.yml`. The Mini2Plus numbers its frame descriptors non-sequentially, and stock `uvc_desc_get_frame_format_by_index` rejected any `bFrameIndex > bNumFrameDescriptors`, so every stream open failed with `Could not find requested frame format`. The patch is marked `LOCAL PATCH` in `uvc_descriptor_parsing.c`. Re-apply it if the component is ever upgraded.
  - A second `LOCAL PATCH` is in `uvc_bulk.c`, in the EOF state. The camera sends each frame as about 40 bulk payloads of 1862 bytes (a 2-byte header plus 1860 data), one per transfer, each ending in a short packet.
    - Stock code switched to DATA after an EOF-state payload, so every second 2-byte header ended up in the image. That added 40 extra bytes per frame and shifted rows 2 px about every 14 lines, giving jagged "scan lines".
    - The patch stays in header-parsing state after a short transfer, and keeps the data of a payload that carries the end-of-frame flag.
- **esptool clash between the two projects (2026-10-08):** pioarduino's `tool-esptoolpy` 5.4.0 is a stub package whose esptool source gets unpacked into `~/.platformio/packages/tool-esptoolpy`. `composite/` uses `espressif32@6.5.0`, which installs its own `tool-esptoolpy` 1.40501 (esptool 4.5.1, no P4 support) into the same folder. Symptoms in the P4 build: `does not appear to be a Python project` and `invalid choice: 'esp32p4'`, or `No module named 'esptool'`.
  - Fix used: delete the folder, let the P4 build reinstall the stub, then unpack `https://github.com/pioarduino/esptool/releases/download/v5.4.0/esptool.zip` (sha256 `23a3f30b…a8fcd`) into it, keeping the stub's `package.json`.
  - Not yet confirmed whether building `composite/` clobbers it again.
- After changing component paths or manifests, `touch CMakeLists.txt` to force PlatformIO to re-run CMake. Otherwise it keeps stale component paths.
