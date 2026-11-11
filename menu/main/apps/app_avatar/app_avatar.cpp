#include "app_avatar.h"
#include "avatar_config.h"
#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

AppAvatar::AppAvatar()
{
    setAppInfo().name = AVATAR_APP_NAME;
    setAppInfo().icon = (void*)&icon_avatar;
}

void AppAvatar::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppAvatar::onOpen()
{
    const esp_partition_t* voice =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    esp_app_desc_t desc;
    if (voice == nullptr || esp_ota_get_partition_description(voice, &desc) != ESP_OK) {
        mclog::tagError(getAppInfo().name, "no voice app in ota_1");
        close();
        return;
    }
    mclog::tagInfo(getAppInfo().name, "switching to {} in ota_1", desc.project_name);
    esp_err_t err = esp_ota_set_boot_partition(voice);
    if (err != ESP_OK) {
        mclog::tagError(getAppInfo().name, "set boot partition failed: {}", esp_err_to_name(err));
        close();
        return;
    }
    esp_restart();
}
