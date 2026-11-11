/*
 * Launcher entry for the voice chat app in the ota_1 partition.
 *
 * Opening this entry marks ota_1 as the boot partition and restarts; the voice
 * app switches back to ota_0 (this menu) the same way. The entry's name comes
 * from AVATAR_NAME in the repository's .env (scripts/env_config.py).
 */
#pragma once
#include <mooncake.h>

class AppAvatar : public mooncake::AppAbility {
public:
    AppAvatar();

    void onCreate() override;
    void onOpen() override;
};
