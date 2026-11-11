# Firmware

The StopWatch carries two programs:

| Partition | Program | Source | Framework |
|---|---|---|---|
| `ota_0` | Launcher menu (M5StopWatch-UserDemo with an added entry) | `menu/` | ESP-IDF 5.5 via PlatformIO (pioarduino 55.03.312) |
| `ota_1` | Voice app | `firmware/` | Arduino-ESP32 via PlatformIO (pioarduino 55.03.312), M5Unified, M5GFX, ArduinoJson 7 |

The UI of the voice app is described in [ui.md](ui.md).

## Build-time settings

Both projects read the repository's `.env` when they are built. Lines starting with `#` are ignored and matching quotes around a value are removed.

### Voice app (`firmware/scripts/env_config.py`)

This PlatformIO pre-script writes `firmware/src/env_config.h` (git-ignored). The file is rewritten only when its content changes.

| Setting | Meaning |
|---|---|
| `WIFI_SSID` | Wi-Fi network the watch joins |
| `WIFI_PASS` | Wi-Fi password |
| `RELAY_URL` | Base URL of the relay, for example `http://192.168.1.10:8765` |
| `RELAY_TOKEN` | Shared secret, sent as `X-Token` over HTTP and used as the HMAC key over BLE |
| `TRANSPORT` | `auto` (default), `wifi` or `ble` |
| `AVATAR_NAME` | BLE device name `<AVATAR_NAME>-StopWatch` (default `Avatar-StopWatch`). Read from `.env` only. |

A missing `WIFI_SSID`, `WIFI_PASS`, `RELAY_URL` or `RELAY_TOKEN` is reported in the build log and compiled in as an empty string. The build does not stop.

| `TRANSPORT` | Joins Wi-Fi | BLE advertising |
|---|---|---|
| `auto` | on | on |
| `wifi` | on | off |
| `ble` | off | on |
| any other value | as `auto`, with a warning in the build log | |

### Launcher (`menu/scripts/env_config.py`)

This pre-script writes `menu/main/apps/app_avatar/avatar_config.h` (git-ignored).

| Setting | Default | Meaning |
|---|---|---|
| `AVATAR_NAME` | `Avatar` | Name of the voice app's entry in the launcher. An environment variable of the same name takes precedence over `.env`. |

## Partitions

Both projects use the same table (`firmware/partitions.csv`, `menu/partitions.csv`) on 16 MB flash. Offsets left empty in the file are placed by the partition tool; the resulting offsets are:

| Name | Type | Offset | Size |
|---|---|---|---|
| `nvs` | data, nvs | `0x9000` | `0x4000` |
| `otadata` | data, ota | `0xd000` | `0x2000` |
| `phy_init` | data, phy | `0xf000` | `0x1000` |
| `ota_0` | app | `0x20000` | `0x4f0000` (4.9 MB) |
| `ota_1` | app | `0x510000` | `0x4f0000` (4.9 MB) |
| `storage` | data, fat | `0xa00000` | 4 MB |
| `coredump` | data, coredump | `0xe00000` | `0x10000` |

`ota_1` starts where `ota_0` ends: `0x20000 + 0x4f0000 = 0x510000`. The voice app image (`firmware/.pio/build/m5stopwatch/firmware.bin`) is written there:

```sh
cd firmware
pio run
uvx esptool --chip esp32s3 --port <PORT> write-flash 0x510000 .pio/build/m5stopwatch/firmware.bin
```

The launcher is flashed with `pio run -t upload --upload-port <PORT>` in `menu/`. The voice app is written only with the esptool command above, at the `ota_1` offset.

### Switching between the launcher and the voice app

| From | Action | What happens |
|---|---|---|
| Launcher | open the entry named `AVATAR_NAME` | The entry looks for an app in `ota_1`. If there is one, it sets `ota_1` as the boot partition and restarts. If not, it logs `no voice app in ota_1` and closes. |
| Voice app | hold the blue key | The app checks for an app in `ota_0`, clears the screen, sets `ota_0` as the boot partition, waits 300 ms and restarts. If there is none, it shows `メニューが見つかりません`. |

The boot partition is stored in `otadata`, so the watch starts the last selected program after a reset or power cycle.

## Transport

| Situation | Upload and reply | History and reset |
|---|---|---|
| Wi-Fi connected | HTTP. If `/stream/start` fails and BLE is authenticated, that recording goes over BLE. | HTTP, then BLE if HTTP fails |
| No Wi-Fi, BLE authenticated | BLE | BLE |
| Neither | Offline screen ([ui.md](ui.md#offline-messages)) | Not available |

The reply always uses the same link as its recording.

At boot the watch starts BLE advertising (when BLE is enabled), then puts Wi-Fi in station mode and joins the network (when Wi-Fi is enabled). The idle eyes are shown immediately; both links come up in the background.

### HTTP timings

| Request | Connect timeout | Timeout | Interval |
|---|---|---|---|
| `/stream/start`, `/stream/chunk`, `/stream/end` | 1.5 s | 10 s | chunks about every 400 ms |
| `/result` | library default | 10 s | about every 150 ms; failures are retried on the next poll |
| `/history` | 1.5 s | 5 s | |
| `/reset` | 1.5 s | library default | |

### BLE timings

| Item | Value |
|---|---|
| Audio packets | about every 200 ms while recording |
| Waiting for `HEARD` after `END` | 20 s |
| Reply: link down before giving up | 15 s |
| `HISTORY` answer | 5 s |
| `RESET_OK` answer | 3 s |

## Streaming upload

| Item | Value |
|---|---|
| Sample rate | 16 kHz, 16-bit mono |
| Mic noise filter level | 64 |
| Record call | 800 samples (50 ms) |
| Buffer | 20 s in PSRAM; recording stops when it is full |
| Minimum length | 0.4 s; shorter recordings are not sent for transcription |
| HTTP format | raw 16-bit little-endian PCM, header `X-Sample-Rate: 16000` |
| BLE format | IMA ADPCM, 4 bits per sample |

A mic task fills the buffer and an upload task sends it while the key is held. The upload task holds back the two newest chunks (100 ms), which may still be filling. On release, the app stops the mic, waits until every queued sample is filled, and sends the rest with `/stream/end` (HTTP) or `AUDIO` + `END` (BLE).

A recording shorter than 0.4 s ends without `/stream/end`; its stream expires on the relay. Over BLE it is ended with `CANCEL`.

The level bar shows the peak of the last finished 50 ms chunk divided by 400, capped at 40.

## Power

| Item | Behaviour |
|---|---|
| Wi-Fi | Modem sleep while the eyes are idle. Switched off (full power) while recording, sending or receiving a reply. |
| Microphone | Started when the yellow key is pressed, stopped on release |
| Speaker | Disabled at start-up |
| Frame interval | No limit during a conversation; 33 ms on the idle eyes; 100 ms while asleep |
| Brightness | 150; 40 while the eyes are asleep |
| Rotation check | Not run while recording |
| Battery check | Every 30 s |
| BLE advertising | Interval 500 to 1000 ms |

## BLE peripheral

| Item | Value |
|---|---|
| Stack | BLE library bundled with Arduino-ESP32 (NimBLE) |
| Advertising data | the 128-bit service UUID `7c3e0001-5a1d-4b8e-9f3a-2d6c1e0b7a01` |
| Scan response | the device name, `<AVATAR_NAME>-StopWatch` |
| Connections | one central at a time; advertising restarts after a disconnect |
| MTU | 247 |
| Authentication timeout | a central that does not authenticate within 10 s is disconnected |
| Notification retry | up to 3 s per packet while the stack has no buffers |
| Largest incoming message | 64 KiB; larger messages are dropped |

The protocol is in [ble-protocol.md](ble-protocol.md).

## Font

The voice app embeds Noto Sans JP Medium as a VLW font in `firmware/src/font_noto.h`. The header is generated from `firmware/fonts/NotoSansJP-Medium.otf`; neither file is in the repository.

```sh
cd firmware
mkdir -p fonts
curl -L -o fonts/NotoSansJP-Medium.otf \
  https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/JP/NotoSansJP-Medium.otf
uv run --with freetype-py python tools/make_font.py
```

`make_font.py` takes an optional pixel size (default 24). The character set and the fallback font are described in [ui.md](ui.md#font).

## Launcher icon

`menu/tools/make_icon.py` generates `menu/main/assets/images/icon_avatar.c`, a 200 x 200 RGB565 LVGL image of the two eyes on a dark disc. The generated file is in the repository.

```sh
cd menu
uv run --with pillow python tools/make_icon.py
```

## Serial log

Both programs log over USB at 115200 baud:

```sh
pio device monitor -b 115200
```
