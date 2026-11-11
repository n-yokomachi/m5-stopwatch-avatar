# PlatformIO pre-script: writes src/env_config.h from the repository's .env
# (WIFI_SSID, WIFI_PASS, RELAY_URL, RELAY_TOKEN, TRANSPORT, AVATAR_NAME). The header is git-ignored.
import json
from pathlib import Path

Import("env")  # noqa: F821 (provided by PlatformIO)

project = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
values = {}
dotenv = project.parent / ".env"
if dotenv.exists():
    for line in dotenv.read_text().splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            k, v = line.split("=", 1)
            v = v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                v = v[1:-1]
            values[k.strip()] = v

keys = ["WIFI_SSID", "WIFI_PASS", "RELAY_URL", "RELAY_TOKEN"]
values.setdefault("AVATAR_NAME", "Avatar")  # also names the BLE peripheral: "<name>-StopWatch"
missing = [k for k in keys if not values.get(k)]
if missing:
    print(f"env_config: missing in {dotenv}: {', '.join(missing)} (see .env.example)")
lines = ["// Generated from .env by scripts/env_config.py. Do not edit.", "#pragma once"]
lines += [f"#define {k} {json.dumps(values.get(k, ''), ensure_ascii=False)}" for k in keys + ["AVATAR_NAME"]]
# TRANSPORT: auto (Wi-Fi first, BLE when that fails), wifi (never BLE), ble (Wi-Fi off)
transport = values.get("TRANSPORT", "auto").lower()
if transport not in ("auto", "wifi", "ble"):
    print(f"env_config: unknown TRANSPORT={transport!r}, using auto")
    transport = "auto"
lines += [f"#define TRANSPORT_USE_WIFI {int(transport != 'ble')}", f"#define TRANSPORT_USE_BLE {int(transport != 'wifi')}"]
text = "\n".join(lines) + "\n"
out = project / "src" / "env_config.h"
if not out.exists() or out.read_text() != text:
    out.write_text(text)
