# PlatformIO pre-script: writes main/apps/app_avatar/avatar_config.h from the
# repository's .env (AVATAR_NAME = the launcher entry's name). The header is git-ignored.
import os
from pathlib import Path

Import("env")  # noqa: F821 (provided by PlatformIO)

root = Path(env.subst("$PROJECT_DIR")).parent  # noqa: F821
values = {}
dotenv = root / ".env"
if dotenv.exists():
    for line in dotenv.read_text().splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            k, v = line.split("=", 1)
            v = v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                v = v[1:-1]
            values[k.strip()] = v

name = os.environ.get("AVATAR_NAME") or values.get("AVATAR_NAME") or "Avatar"
out = Path(env.subst("$PROJECT_DIR")) / "main" / "apps" / "app_avatar" / "avatar_config.h"  # noqa: F821
text = (
    "// Generated from .env by scripts/env_config.py. Do not edit.\n"
    "#pragma once\n"
    f"#define AVATAR_APP_NAME {name!r}\n".replace("'", '"')
)
if not out.exists() or out.read_text() != text:
    out.write_text(text)
print(f"env_config: launcher name {name!r}")
