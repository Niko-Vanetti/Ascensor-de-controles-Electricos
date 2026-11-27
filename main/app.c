/**
 * @file app.c
 * @brief Operaciones de alto nivel sobre el estado de la aplicacion.
 */

#include "app.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define APP_ERR_MAX 160

static const char *TAG = "APP";

static app_state_t s_app;

static void load_default_button_blocks(void)
{
    static const struct {
        rule_source_t source;
        uint8_t floor;
        uint8_t gpio;
    } buttons[] = {
        { RULE_BUTTON_CABIN, 1, 52 },
        { RULE_BUTTON_CABIN, 2, 51 },
        { RULE_BUTTON_CABIN, 3, 50 },
        { RULE_BUTTON_UP,    1, 52 },
        { RULE_BUTTON_DOWN,  3, 50 },
        { RULE_BUTTON_UP,    2, 51 },
        { RULE_BUTTON_DOWN,  2, 51 },
    };

    s_app.block_count = sizeof(buttons) / sizeof(buttons[0]);
    for (size_t i = 0; i < s_app.block_count; i++) {
        rule_block_t *block = &s_app.blocks[i];
        rules_block_defaults(block);
        block->source = buttons[i].source;
        block->floor = buttons[i].floor;
        block->out_gpio = buttons[i].gpio;
    }
}

app_state_t *app_state(void)
{
    return &s_app;
}

esp_err_t app_load(void)
{
    memset(&s_app, 0, sizeof(s_app));

    esp_err_t config_rc = storage_load_config(&s_app.cfg);
    size_t capacity = RULES_MAX_BLOCKS;
    esp_err_t blocks_rc = storage_load_blocks(s_app.blocks, &capacity);
    if (blocks_rc == ESP_OK) {
        s_app.block_count = capacity;
    } else if (blocks_rc == ESP_ERR_NVS_NOT_FOUND) {
        if (config_rc == ESP_ERR_NVS_NOT_FOUND) {
            s_app.cfg.floors = 3;
            s_app.cfg.blocks_enabled = true;
            load_default_button_blocks();
        } else if (config_rc == ESP_OK && s_app.cfg.floors == 3) {
            load_default_button_blocks();
        }
    }

    ESP_ERROR_CHECK(elevator_init(&s_app.cfg));

    if (s_app.cfg.blocks_enabled && s_app.block_count > 0) {
        char err[APP_ERR_MAX] = { 0 };
        esp_err_t rc = rules_validate(s_app.blocks, s_app.block_count,
                                      err, sizeof(err));
        if (rc == ESP_OK) {
            rc = rules_start(s_app.blocks, s_app.block_count);
        }
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "Bloques: %s", err);
            s_app.cfg.blocks_enabled = false;
            storage_save_config(&s_app.cfg);
        }
    }
    return ESP_OK;
}

esp_err_t app_apply_config(void)
{
    elevator_state_t st;
    elevator_get_state(&st);
    if (st.moving || rules_enabled()) return ESP_ERR_INVALID_STATE;
    esp_err_t rc = elevator_reload(&s_app.cfg);
    if (rc != ESP_OK) return rc;
    s_app.cfg.demo_mode = false;
    return storage_save_config(&s_app.cfg);
}

esp_err_t app_save_blocks(void)
{
    return storage_save_blocks(s_app.blocks, s_app.block_count);
}

esp_err_t app_set_blocks_enabled(bool enabled, char *err, size_t err_len)
{
    if (enabled) {
        esp_err_t rc = rules_validate(s_app.blocks, s_app.block_count,
                                      err, err_len);
        if (rc != ESP_OK) {
            return rc;
        }
        if (s_app.block_count == 0) {
            if (err != NULL) {
                snprintf(err, err_len, "Agrega al menos un bloque completo.");
            }
            return ESP_ERR_INVALID_STATE;
        }
        rc = rules_start(s_app.blocks, s_app.block_count);
        if (rc != ESP_OK) {
            return rc;
        }
    } else {
        rules_stop();
    }

    s_app.cfg.blocks_enabled = enabled;
    storage_save_config(&s_app.cfg);
    return ESP_OK;
}

esp_err_t app_add_block(rule_block_t **out)
{
    if (s_app.block_count >= RULES_MAX_BLOCKS) {
        return ESP_ERR_NO_MEM;
    }
    rule_block_t *b = &s_app.blocks[s_app.block_count];
    rules_block_defaults(b);
    s_app.block_count++;

    if (out != NULL) {
        *out = b;
    }
    return ESP_OK;
}

esp_err_t app_remove_block(size_t index)
{
    if (index >= s_app.block_count) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = index; i + 1 < s_app.block_count; i++) {
        s_app.blocks[i] = s_app.blocks[i + 1];
    }
    s_app.block_count--;
    return app_save_blocks();
}
