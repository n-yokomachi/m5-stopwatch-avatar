## Changes in this repository

This folder is the upstream [M5StopWatch-UserDemo](https://github.com/m5stack/M5StopWatch-UserDemo) (MIT, see `LICENSE`) with these changes. The upstream README follows below.

- Added launcher entry `main/apps/app_avatar/`, included in `main/apps/apps.h` and installed in `main/main.cpp`. Opening it sets `ota_1` as the boot partition and restarts into the voice app (see `../docs/spec/firmware.md`). Its name comes from `AVATAR_NAME` in the repository's `.env` (default `Avatar`).
- Added `scripts/env_config.py`, a PlatformIO pre-script that writes `main/apps/app_avatar/avatar_config.h` from `AVATAR_NAME`.
- Added `tools/make_icon.py`, which generates the entry's icon `main/assets/images/icon_avatar.c` (declared in `main/assets/assets.h`).
- `PROJECT_VER` is set to `userdemo-avatar` in `CMakeLists.txt`.
- `upload_port` is removed from `platformio.ini`. Pass the port on the command line: `pio run -t upload --upload-port <PORT>`.

---

# M5StopWatch-UserDemo
M5Stack StopWatch user demo for hardware evaluation.

## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Flash

```bash
idf.py flash
```

## FluidBox Integration
With genuine thanks to [omiya-bonsai](https://github.com/omiya-bonsai) for the open-source "FluidBox" fluid simulation demo for the M5Stack Stopwatch.
Incorporated into the *M5StopWatch-UserDemo* project by [Simon J. Stuart](https://github.com/LK-Simon)

## Power LED Off while Display On
Incorporated into the *M5StopWatch-UserDemo* project by [Simon J. Stuart](https://github.com/LK-Simon) the green power indicator LED will now switch off when the AMOLED display is on. This will save a small amount of power, but also eliminate the distracting green LED when it is not necessary.

## PlatformIO/PIOArduino build support
Incorporated into the *M5StopWatch-UserDemo* project by [Simon J. Stuart](https://github.com/LK-Simon) you can now load the repository folder in Visual Studio Code with either PlatformIO or PIOArduino installed, and build/upload the demo directly to your M5Stack StopWatch device.