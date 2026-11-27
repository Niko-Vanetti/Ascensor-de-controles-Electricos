/**
 * @file elevator.c
 * @brief Maquina de estados del ascensor.
 *
 * Posicion confirmada exclusivamente mediante sensores GPIO.
 */

#include "elevator.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "driver/gpio.h"

#include "board_config.h"
#include "rules.h"

static const char *TAG = "ASCENSOR";

#define STOP_DWELL_MS       3000   /* tiempo total de parada antes de salir */
#define DOORS_CLOSE_MS      700    /* tramo final del ciclo: animacion de cierre */
#define SENSOR_FILTER_MS    30

static elevator_config_t s_cfg;
static elevator_state_t s_state;

static uint8_t s_pending[ELEVATOR_MAX_FLOORS]; /* cabina=1, subir=2, bajar=4 */
static elevator_dir_t s_service_dir;
static uint32_t s_button_until[3][ELEVATOR_MAX_FLOORS];
static uint16_t s_button_seen[3];
static bool s_has_target;
static uint8_t s_target;

static uint32_t s_doors_deadline_ms;
static bool s_departure_scheduled;

/* Filtrado de sensores */
static bool     s_sensor_raw[ELEVATOR_MAX_FLOORS];
static bool     s_sensor_stable[ELEVATOR_MAX_FLOORS];
static uint32_t s_sensor_change_ms[ELEVATOR_MAX_FLOORS];

static elevator_event_cb_t s_cb;
static void *s_cb_ctx;

/* ------------------------------------------------------------------ */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void emit(elevator_event_type_t type, uint8_t floor, const char *msg)
{
    if (s_cb == NULL) {
        return;
    }
    elevator_event_t ev = {
        .type       = type,
        .floor      = floor,
        .dir        = s_state.dir,
        .doors_open = s_state.doors_open,
        .message    = msg,
    };
    s_cb(&ev, s_cb_ctx);
}

static void open_doors(void)
{
    s_departure_scheduled = false;
    s_state.doors_closing = false;
    if (!s_state.doors_open) {
        s_state.doors_open = true;
        emit(ELEV_EVT_DOORS, s_state.current_floor,
             "Puertas abiertas: ya puedes entrar o salir.");
        ESP_LOGI(TAG, "Llegada a P%02u - cabina detenida.", s_state.current_floor);
    }
}

static void schedule_departure(uint32_t now)
{
    if (!s_departure_scheduled && s_state.doors_open &&
        !s_state.fault && s_state.position_known && elevator_has_pending()) {
        s_doors_deadline_ms = now + STOP_DWELL_MS;
        s_departure_scheduled = true;
    }
}

static bool beyond(uint8_t floor, elevator_dir_t dir)
{
    for (int f = 1; f <= s_cfg.floors; f++)
        if (s_pending[f-1] && ((dir == ELEV_DIR_UP && f > floor) ||
                              (dir == ELEV_DIR_DOWN && f < floor))) return true;
    return false;
}

static bool should_stop(uint8_t floor)
{
    uint8_t calls = s_pending[floor-1];
    uint8_t matching = s_state.dir == ELEV_DIR_UP ? 2 : 4;
    return (calls & (1 | matching)) || (calls && !beyond(floor, s_state.dir));
}

static void clear_pending(uint8_t floor)
{
    if (floor >= 1 && floor <= ELEVATOR_MAX_FLOORS) {
        uint8_t served = 1 | (s_service_dir == ELEV_DIR_UP ? 2 : 4);
        if (!beyond(floor, s_service_dir)) served = 7;
        s_pending[floor - 1] &= ~served;
    }
}

static bool any_pending_from(uint8_t floor, bool up)
{
    for (int f = 1; f <= s_cfg.floors; f++) {
        if (!s_pending[f - 1]) {
            continue;
        }
        if (up && f > floor) {
            return true;
        }
        if (!up && f < floor) {
            return true;
        }
    }
    return false;
}

static int pick_target(void)
{
    uint8_t cur = s_state.current_floor;

    if (s_state.dir == ELEV_DIR_UP && any_pending_from(cur, true)) {
        for (int f = cur + 1; f <= s_cfg.floors; f++) {
            if (s_pending[f - 1]) {
                return f;
            }
        }
    }
    if (s_state.dir == ELEV_DIR_DOWN && any_pending_from(cur, false)) {
        for (int f = cur - 1; f >= 1; f--) {
            if (s_pending[f - 1]) {
                return f;
            }
        }
    }

    /* Cambio de sentido */
    if (any_pending_from(cur, true)) {
        s_state.dir = ELEV_DIR_UP;
        for (int f = cur + 1; f <= s_cfg.floors; f++) {
            if (s_pending[f - 1]) {
                return f;
            }
        }
    }
    if (any_pending_from(cur, false)) {
        s_state.dir = ELEV_DIR_DOWN;
        for (int f = cur - 1; f >= 1; f--) {
            if (s_pending[f - 1]) {
                return f;
            }
        }
    }
    return -1;
}

static void plan_next(void)
{
    if (s_state.fault || s_state.moving || s_state.doors_open) {
        return;
    }

    /* El temporizador ya se inició en un piso confirmado. El sensor puede
     * quedar inactivo antes que el temporizador de la interfaz al salir. */
    uint8_t cur = s_state.current_floor;

    if (s_pending[cur - 1] && (s_state.dir == ELEV_DIR_IDLE || should_stop(cur))) {
        s_service_dir = s_state.dir == ELEV_DIR_IDLE ?
                        (s_pending[cur-1] & 2 ? ELEV_DIR_UP : ELEV_DIR_DOWN) : s_state.dir;
        clear_pending(cur);
        open_doors();
        return;
    }

    int t = pick_target();
    if (t < 0) {
        s_state.dir = ELEV_DIR_IDLE;
        open_doors();
        return;
    }

    s_target     = (uint8_t)t;
    s_has_target = true;
    s_state.moving = true;
    s_state.dir = (t > cur) ? ELEV_DIR_UP : ELEV_DIR_DOWN;

    char msg[48];
    snprintf(msg, sizeof(msg), "Parada P%02u agregada a la ruta.", s_target);
    emit(ELEV_EVT_MOVING, s_target, msg);
    ESP_LOGI(TAG, "Recorrido hacia P%02u (%s)", s_target,
             s_state.dir == ELEV_DIR_UP ? "subiendo" : "bajando");
}

/* ------------------------------------------------------------------ */
/*  Lectura de posicion por sensores                                  */
/* ------------------------------------------------------------------ */

static uint8_t s_last_sensor_floor;

static void read_sensors(void)
{
    uint32_t now = now_ms();
    int active_floor = 0;
    int active_count = 0;

    for (int f = 1; f <= s_cfg.floors; f++) {
        uint8_t gpio = s_cfg.floor_gpio[f - 1];
        if (gpio == 0xFF) {
            s_sensor_stable[f - 1] = false;
            continue;
        }

        bool raw = (gpio_get_level(gpio) == s_cfg.floor_level[f - 1]);
        if (raw != s_sensor_raw[f - 1]) {
            s_sensor_raw[f - 1]    = raw;
            s_sensor_change_ms[f - 1] = now;
        }
        if ((now - s_sensor_change_ms[f - 1]) >= SENSOR_FILTER_MS) {
            s_sensor_stable[f - 1] = raw;
        }
        if (s_sensor_stable[f - 1]) {
            active_count++;
            active_floor = f;
        }
    }

    if (active_count == 1) {
        if (s_last_sensor_floor != 0 && active_floor != s_last_sensor_floor) {
            s_state.observed_dir = active_floor > s_last_sensor_floor ? ELEV_DIR_UP : ELEV_DIR_DOWN;
        }
        s_last_sensor_floor = (uint8_t)active_floor;
        if (s_state.current_floor != active_floor || !s_state.position_known) {
            s_state.current_floor = (uint8_t)active_floor;
            s_state.position_known = true;
            emit(ELEV_EVT_POSITION, (uint8_t)active_floor,
                 "Sensor confirmado: luz del piso encendida.");
            ESP_LOGI(TAG, "Piso detectado: %u", active_floor);
        }

        if (s_state.moving && s_has_target && should_stop((uint8_t)active_floor)) {
            s_state.moving = false;
            s_has_target = false;
            s_service_dir = s_state.dir;
            clear_pending((uint8_t)active_floor);
            {
                char amsg[56];
                snprintf(amsg, sizeof(amsg), "Llegada a P%02u \xC2\xB7 cabina detenida.", active_floor);
                emit(ELEV_EVT_ARRIVED, (uint8_t)active_floor, amsg);
            }
            open_doors();
        }
        return;
    }

    if (active_count > 1) {
        s_last_sensor_floor = 0;
        s_state.observed_dir = ELEV_DIR_IDLE;
        s_state.position_known = false;
        s_state.fault = true;
        s_state.moving = false;
        s_state.dir = ELEV_DIR_IDLE;
        s_has_target = false;
        emit(ELEV_EVT_REJECTED, 0, "Conflicto: hay varios sensores de piso activos.");
        ESP_LOGW(TAG, "Conflicto: hay varios sensores de piso activos.");
        return;
    }

    /* Ningun sensor activo: entre pisos, posicion sin confirmar */
    s_state.position_known = false;
}

/* ------------------------------------------------------------------ */
/*  API publica                                                       */
/* ------------------------------------------------------------------ */

esp_err_t elevator_init(const elevator_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->floors < ELEVATOR_MIN_FLOORS || cfg->floors > ELEVATOR_MAX_FLOORS)
        return ESP_ERR_INVALID_ARG;
    uint64_t used = 0;
    for (int f = 0; f < cfg->floors; f++) {
        int pin = cfg->floor_gpio[f];
        if (pin == 0xFF) continue;
        if (!rules_input_allowed(pin) || cfg->floor_level[f] > 1 || (used & (1ULL << pin)))
            return ESP_ERR_INVALID_ARG;
        used |= 1ULL << pin;
    }
    s_cfg = *cfg;

    memset(&s_state, 0, sizeof(s_state));
    s_last_sensor_floor = 0;
    memset(s_pending, 0, sizeof(s_pending));
    memset(s_sensor_raw, 0, sizeof(s_sensor_raw));
    memset(s_sensor_stable, 0, sizeof(s_sensor_stable));
    memset(s_sensor_change_ms, 0, sizeof(s_sensor_change_ms));
    memset(s_button_until, 0, sizeof(s_button_until));
    memset(s_button_seen, 0, sizeof(s_button_seen));
    s_has_target = false;
    s_target = 0;
    s_departure_scheduled = false;

    s_state.floors      = cfg->floors;
    s_state.demo_mode   = false;
    s_state.current_floor = 1;
    s_state.position_known = false;
    s_state.dir = ELEV_DIR_IDLE;
    s_state.doors_open = true;

    for (int f = 0; f < s_cfg.floors; f++) {
        uint8_t pin = s_cfg.floor_gpio[f];
        if (pin == 0xFF) continue;
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = s_cfg.floor_level[f] == 0,
            .pull_down_en = s_cfg.floor_level[f] != 0,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t rc = gpio_config(&io);
        if (rc != ESP_OK) return rc;
    }

    ESP_LOGI(TAG, "Ascensor listo: %u pisos, %s", s_cfg.floors,
             "GPIO reales");
    return ESP_OK;
}

esp_err_t elevator_reload(const elevator_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Mantiene la cabina donde esta, pero reajusta pisos y sensores */
    elevator_state_t keep = s_state;
    esp_err_t err = elevator_init(cfg);
    if (err == ESP_OK) {
        s_state.current_floor = (keep.current_floor <= cfg->floors)
                                ? keep.current_floor : 1;
    }
    return err;
}

void elevator_set_callback(elevator_event_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}

void elevator_get_state(elevator_state_t *out)
{
    if (out != NULL) {
        *out = s_state;
        out->cabin_calls = out->up_calls = out->down_calls = 0;
        uint16_t buttons[3] = {0};
        for (int kind = 0; kind < 3; kind++)
            for (int f = 0; f < s_cfg.floors; f++)
                if ((s_button_seen[kind] & (1u << f)) &&
                    (int32_t)(s_button_until[kind][f] - now_ms()) > 0)
                    buttons[kind] |= 1u << f;
        out->cabin_buttons = buttons[0]; out->up_buttons = buttons[1]; out->down_buttons = buttons[2];
        for (int f = 0; f < s_cfg.floors; f++) {
            if (s_pending[f] & 1) out->cabin_calls |= 1u << f;
            if (s_pending[f] & 2) out->up_calls |= 1u << f;
            if (s_pending[f] & 4) out->down_calls |= 1u << f;
        }
    }
}

esp_err_t elevator_call(uint8_t floor, elevator_dir_t dir)
{
    if (floor < 1 || floor > s_cfg.floors) {
        emit(ELEV_EVT_LIMIT, floor, "L\xC3\xADmite de recorrido alcanzado.");
        return ESP_ERR_INVALID_ARG;
    }

    if ((dir == ELEV_DIR_UP && floor == s_cfg.floors) ||
        (dir == ELEV_DIR_DOWN && floor == 1) || dir > ELEV_DIR_DOWN) return ESP_ERR_INVALID_ARG;

    if (s_state.fault) return ESP_ERR_INVALID_STATE;
    int button = dir == ELEV_DIR_IDLE ? 0 : dir == ELEV_DIR_UP ? 1 : 2;
    s_button_seen[button] |= 1u << (floor-1);
    s_button_until[button][floor-1] = now_ms() + 300;
    if (floor == s_state.current_floor && s_state.position_known && !s_state.moving) {
        emit(ELEV_EVT_REJECTED, floor, "La cabina ya est\xC3\xA1 en este piso.");
        return ESP_OK;
    }

    uint8_t call_bit = dir == ELEV_DIR_UP ? 2 : dir == ELEV_DIR_DOWN ? 4 : 1;
    if (s_pending[floor - 1] & call_bit) {
        emit(ELEV_EVT_REJECTED, floor,
             "Ese piso ya est\xC3\xA1 incluido en el recorrido.");
        return ESP_OK;
    }

    s_pending[floor - 1] |= call_bit;

    if (dir == ELEV_DIR_UP || dir == ELEV_DIR_DOWN) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Llamada P%02u \xC2\xB7 %s", floor,
                 dir == ELEV_DIR_UP ? "subiendo" : "bajando");
        emit(ELEV_EVT_MOVING, floor, msg);
    }

    schedule_departure(now_ms());
    return ESP_OK;
}

bool elevator_sensor_gpio(int gpio)
{
    for (int f=0; f<s_cfg.floors; f++)
        if (s_cfg.floor_gpio[f] == gpio) return true;
    return false;
}

bool elevator_has_pending(void)
{
    for (int f = 0; f < s_cfg.floors; f++) {
        if (s_pending[f]) {
            return true;
        }
    }
    return false;
}

void elevator_tick(void)
{
    uint32_t now = now_ms();
    read_sensors();
    if (s_state.fault) {
        return;
    }
    schedule_departure(now);
    if (!s_departure_scheduled) {
        return;
    }

    if (!s_state.doors_closing &&
        (int32_t)(s_doors_deadline_ms - now) <= DOORS_CLOSE_MS) {
        /* ultimos 700 ms: la puerta se cierra; la cabina no sale hasta terminar */
        s_state.doors_closing = true;
        emit(ELEV_EVT_DOORS, s_state.current_floor, "Cerrando puertas...");
    }

    if ((int32_t)(now - s_doors_deadline_ms) >= 0) {
        s_departure_scheduled = false;
        s_state.doors_open = false;
        s_state.doors_closing = false;
        emit(ELEV_EVT_DOORS, s_state.current_floor, "Puertas cerradas.");
        plan_next();
    }

}
