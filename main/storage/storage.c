/**
 * @file storage.c
 * @brief Lectura y escritura de la configuracion del ascensor en NVS.
 */

#include "storage.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "rules.h"

static const char *TAG = "STORAGE";

void storage_defaults(elevator_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->floors       = 4;
    cfg->demo_mode    = false;
    cfg->blocks_enabled = false;
    cfg->base3        = 1;
    cfg->sound        = 70;

    for (int i = 0; i < ELEVATOR_MAX_FLOORS; i++) {
        cfg->floor_gpio[i]  = 0xFF;  /* sin asignar */
        cfg->floor_level[i] = 1;
    }
}

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS incompatible, borrando particion");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static nvs_handle_t open_ns(nvs_open_mode_t mode)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(STORAGE_NS_ELEVATOR, mode, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo abrir el namespace: %s", esp_err_to_name(err));
        return 0;
    }
    return h;
}

esp_err_t storage_load_config(elevator_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    storage_defaults(cfg);

    nvs_handle_t h;
    esp_err_t open_rc = nvs_open(STORAGE_NS_ELEVATOR, NVS_READONLY, &h);
    if (open_rc != ESP_OK) return open_rc;

    uint8_t u8 = 0;
    if (nvs_get_u8(h, STORAGE_KEY_FLOORS, &u8) == ESP_OK) {
        cfg->floors = u8;
    }

    size_t len = sizeof(cfg->floor_gpio);
    if (nvs_get_blob(h, STORAGE_KEY_FLOOR_GPIO, cfg->floor_gpio, &len) != ESP_OK) {
        memset(cfg->floor_gpio, 0xFF, sizeof(cfg->floor_gpio));
    }

    len = sizeof(cfg->floor_level);
    if (nvs_get_blob(h, STORAGE_KEY_FLOOR_LEVEL, cfg->floor_level, &len) != ESP_OK) {
        memset(cfg->floor_level, 1, sizeof(cfg->floor_level));
    }

    if (nvs_get_u8(h, STORAGE_KEY_DEMO, &u8) == ESP_OK) {
        cfg->demo_mode = (u8 != 0);
    }
    if (nvs_get_u8(h, STORAGE_KEY_BASE3, &u8) == ESP_OK) {
        cfg->base3 = u8;
    }
    if (nvs_get_u8(h, STORAGE_KEY_SOUND, &u8) == ESP_OK) {
        cfg->sound = u8;
    }
    if (nvs_get_u8(h, STORAGE_KEY_BLOCKS_ON, &u8) == ESP_OK) {
        cfg->blocks_enabled = (u8 != 0);
    }
    if (nvs_get_u8(h, STORAGE_KEY_LANDSCAPE, &u8) == ESP_OK) {
        cfg->landscape = (u8 != 0);
    }

    cfg->demo_mode = false; /* Migra configuraciones antiguas sin simular posicion. */
    nvs_close(h);

    if (cfg->floors < ELEVATOR_MIN_FLOORS || cfg->floors > ELEVATOR_MAX_FLOORS) {
        cfg->floors = ELEVATOR_MIN_FLOORS;
    }
    ESP_LOGI(TAG, "Configuracion cargada: %u pisos, %s",
             cfg->floors, cfg->demo_mode ? "modo demo" : "GPIO reales");
    return ESP_OK;
}

esp_err_t storage_save_config(const elevator_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (h == 0) {
        return ESP_FAIL;
    }

    esp_err_t err;
    err = nvs_set_u8(h, STORAGE_KEY_FLOORS, cfg->floors);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, STORAGE_KEY_FLOOR_GPIO, cfg->floor_gpio,
                           sizeof(cfg->floor_gpio));
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(h, STORAGE_KEY_FLOOR_LEVEL, cfg->floor_level,
                           sizeof(cfg->floor_level));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, STORAGE_KEY_DEMO, cfg->demo_mode ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, STORAGE_KEY_BLOCKS_ON, cfg->blocks_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, STORAGE_KEY_BASE3, cfg->base3);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, STORAGE_KEY_SOUND, cfg->sound);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, STORAGE_KEY_LANDSCAPE, cfg->landscape ? 1 : 0);
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo guardar la configuracion: %s",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_load_blocks(struct rule_block_s *blocks, size_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t open_rc = nvs_open(STORAGE_NS_ELEVATOR, NVS_READONLY, &h);
    if (open_rc != ESP_OK) {
        *count = 0;
        return open_rc;
    }

    typedef struct {
        bool enabled;
        uint8_t in_gpio, in_level, out_gpio, out_level, rest_level;
        char note[RULES_NOTE_MAX];
    } legacy_block_t;
    char schema[16] = {0};
    size_t schema_len = sizeof(schema);
    esp_err_t schema_rc = nvs_get_str(h, STORAGE_KEY_RULES_VER, schema, &schema_len);
    if (schema_rc != ESP_OK && schema_rc != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        *count = 0;
        return schema_rc;
    }
    if (schema[0] && strcmp(schema, "rules_v1") != 0 &&
        strcmp(schema, STORAGE_RULES_VERSION) != 0) {
        nvs_close(h); *count = 0; return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(schema, STORAGE_RULES_VERSION) != 0) {
        legacy_block_t old[RULES_MAX_BLOCKS];
        size_t bytes = sizeof(old);
        esp_err_t rc = nvs_get_blob(h, STORAGE_KEY_BLOCKS, old, &bytes);
        nvs_close(h);
        if (rc != ESP_OK) {
            *count = 0;
            return rc;
        }
        if (bytes % sizeof(old[0]) || bytes / sizeof(old[0]) > *count) {
            *count = 0;
            return ESP_ERR_INVALID_SIZE;
        }
        *count = bytes / sizeof(old[0]);
        for (size_t i = 0; blocks != NULL && i < *count; i++) {
            rules_block_defaults(&blocks[i]);
            memcpy(&blocks[i], &old[i], 6);
            memcpy(blocks[i].note, old[i].note, RULES_NOTE_MAX);
        }
        return ESP_OK;
    }

    size_t len = (blocks == NULL) ? 0 : (*count) * sizeof(rule_block_t);
    esp_err_t err = nvs_get_blob(h, STORAGE_KEY_BLOCKS, blocks, &len);

    char ver[16] = { 0 };
    size_t ver_len = sizeof(ver) - 1;
    if (nvs_get_str(h, STORAGE_KEY_RULES_VER, ver, &ver_len) != ESP_OK) {
        ver[0] = '\0';
    }
    nvs_close(h);

    if (err != ESP_OK) {
        *count = 0;
        return err;
    }

    if (len % sizeof(rule_block_t)) { *count = 0; return ESP_ERR_INVALID_SIZE; }
    *count = len / sizeof(rule_block_t);
    if (ver[0] != '\0' && strcmp(ver, STORAGE_RULES_VERSION) != 0) {
        ESP_LOGW(TAG, "Esquema de reglas '%s' distinto de '%s', se conserva",
                 ver, STORAGE_RULES_VERSION);
    }
    ESP_LOGI(TAG, "%u bloques de logica cargados", (unsigned)*count);
    return ESP_OK;
}

esp_err_t storage_save_blocks(const struct rule_block_s *blocks, size_t count)
{
    if (blocks == NULL && count > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (h == 0) {
        return ESP_FAIL;
    }

    esp_err_t err = nvs_set_blob(h, STORAGE_KEY_BLOCKS, blocks,
                                 count * sizeof(rule_block_t));
    if (err == ESP_OK) {
        err = nvs_set_str(h, STORAGE_KEY_RULES_VER, STORAGE_RULES_VERSION);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se guardaron los bloques: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_erase_all(void)
{
    nvs_handle_t h = open_ns(NVS_READWRITE);
    if (h == 0) {
        return ESP_FAIL;
    }
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
