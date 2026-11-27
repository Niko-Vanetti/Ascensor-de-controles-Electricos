/**
 * @file ui.c
 * @brief UI de AscensorCrowPanel reconstruida a partir del firmware original.
 *
 * Datos recuperados del binario (ver docs/reverse/HALLAZGOS_UI.md):
 *  - Tres pantallas independientes (Exterior / Interior / Tecnica) de 600x1024,
 *    todo en coordenadas absolutas, que se cambian con un gesto (swipe).
 *  - Tipografia propia (fonts_orig.c) con Latin-1 completo; Montserrat solo
 *    como "fallback" de simbolos, igual que el original.
 *  - Helpers make_label / make_button / make_panel con los mismos estilos.
 *  - Puntos indicadores en y=948 y aviso "Desliza para cambiar de pantalla".
 *
 * Lo unico que NO se pudo recuperar es el editor de bloques a pantalla
 * completa: esta es una version propia (misma funcionalidad y estilo).
 */

#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "app.h"
#include "audio.h"
#include "board.h"
#include "board_config.h"
#include "elevator.h"
#include "fonts_orig.h"
#include "rules.h"

/* ------------------------------------------------------------------ */
/*  Paleta (RGB565 del original convertido a RGB888)                  */
/* ------------------------------------------------------------------ */

#define COL_BLACK       0x000000
#define COL_BTN         0x102031   /* 0x1106 */
#define COL_BTN_PRESS   0x21354A   /* 0x21a9 */
#define COL_BORDER      0x293542   /* 0x29a8 */
#define COL_BTN_TEXT    0xEFF3F7   /* 0xef9e */
#define COL_PANEL       0x101821   /* 0x10c4 */
#define COL_PANEL_EXT   0x101410   /* 0x10a2 */
#define COL_CALL        0x216952   /* 0x234a */
#define COL_OK          0x73D7A5   /* 0x76b4 */
#define COL_BLUE_BORDER 0x8CBEFF   /* 0x8dff */
#define COL_DOT_OFF     0x738284   /* 0x7410 */

/* Por pantalla (texto/acento) */
#define EXT_MUTED       0xA7B3AC
#define EXT_TEXT        0xF2F5F0
#define EXT_ACCENT      0x206A50
#define INT_MUTED       0x97ABA0
#define INT_ACCENT      0xCAEDB0
#define TEC_ACCENT      0x8EBEFF
#define TEC_TEXT        0xEEF2F6
#define TEC_MUTED       0x91A0B2
#define TEC_GREEN       0x76D6A2

/* Orientacion: vertical 600x1024 (por defecto) u horizontal 1024x600. */
static bool s_land;
#define SCREEN_W        (s_land ? LCD_V_RES : LCD_H_RES)
#define SCREEN_H        (s_land ? LCD_H_RES : LCD_V_RES)
#define LAND            s_land

enum {
    ID_PREV = 1, ID_NEXT, ID_UP, ID_DOWN, ID_MINUS, ID_PLUS,
    ID_MODE = 22, ID_EDIT = 30,
};

static const int s_gpios[USER_GPIO_COUNT] = USER_GPIO_LIST;
static char s_gpio_options[220];

/* ------------------------------------------------------------------ */
/*  Fuentes: copia en RAM con fallback Montserrat (como el original)  */
/* ------------------------------------------------------------------ */

static lv_font_t F19, F21, F25, F30, F38, F59, F66;

static void init_fonts(void)
{
    F19 = font_ui_xs;   F19.fallback = &lv_font_montserrat_14;
    F21 = font_ui_s;    F21.fallback = &lv_font_montserrat_16;
    F25 = font_ui_m;    F25.fallback = &lv_font_montserrat_20;
    F30 = font_ui_l;    F30.fallback = &lv_font_montserrat_24;
    F38 = font_ui_xl;   F38.fallback = &lv_font_montserrat_32;
    F59 = font_ui_xxl;  F59.fallback = &lv_font_montserrat_48;
    F66 = font_ui_digits;
}

/* ------------------------------------------------------------------ */
/*  Estado de widgets                                                 */
/* ------------------------------------------------------------------ */

static lv_obj_t *s_scr[3];
static lv_obj_t *s_scr_blocks;
static lv_obj_t *s_scr_overview, *s_overview_rows;
static int       s_cur_scr;

/* Exterior */
static lv_obj_t *s_ext_user_floor;
static lv_obj_t *s_ext_cabin_floor;
static lv_obj_t *s_ext_arrow;
static lv_obj_t *s_ext_dir;
static lv_obj_t *s_ext_hint;

/* Interior */
static lv_obj_t *s_int_current;
static lv_obj_t *s_int_dir;
static lv_obj_t *s_int_grid;
static lv_obj_t *s_int_status;
static lv_obj_t *s_int_btn[ELEVATOR_MAX_FLOORS];
static uint8_t   s_int_btn_count;

/* Tecnica */
static lv_obj_t *s_tec_count;
static lv_obj_t *s_tec_mode_lbl;
static lv_obj_t *s_tec_mode_btn_lbl;
static lv_obj_t *s_tec_dd_floor;
static lv_obj_t *s_tec_dd_gpio;
static lv_obj_t *s_tec_dd_level;
static lv_obj_t *s_tec_read;
static lv_obj_t *s_tec_then;
static lv_obj_t *s_tec_note;
static uint8_t   s_sel_floor = 1;

/* Editor de bloques */
static lv_obj_t *s_blk_status;
static lv_obj_t *s_blk_sw;
static lv_obj_t *s_blk_list;
static lv_obj_t *s_msgbox;

static lv_obj_t *s_b_prev, *s_b_next, *s_b_up, *s_b_down;
static lv_obj_t *s_b_minus, *s_b_plus, *s_b_mode, *s_b_edit, *s_b_back, *s_b_add;

static uint8_t s_user_floor = 1;

static elevator_state_t s_last_state;

/* ------------------------------------------------------------------ */
/*  Helpers (equivalentes a make_label / make_button / make_panel)    */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Puerta animada de la cabina (dos hojas que se deslizan)            */
/*  Solo se anima el rectangulo de la puerta (~100x96 px): barato.     */
/* ------------------------------------------------------------------ */

#define DOOR_MAX        2
#define DOOR_ANIM_MS    600
#define DOOR_POCKET     4     /* px que queda visible de cada hoja abierta */

typedef struct {
    lv_obj_t *frame, *left, *right;
    int inner_w;          /* ancho util dentro del marco */
    int open;             /* apertura actual 0..100 */
    bool target;          /* ultimo destino pedido (true = abierta) */
    bool used;
} door_t;

static door_t s_door[DOOR_MAX];
static int s_door_n;

static void door_apply(door_t *d, int open)
{
    d->open = open;
    int half = d->inner_w / 2;
    int w = DOOR_POCKET + (half - DOOR_POCKET) * (100 - open) / 100;
    lv_obj_set_width(d->left, w);
    lv_obj_set_x(d->left, 0);
    lv_obj_set_width(d->right, w);
    lv_obj_set_x(d->right, d->inner_w - w);
}

static void door_anim_cb(void *var, int32_t v)
{
    door_apply((door_t *)var, (int)v);
}

static lv_obj_t *door_leaf(lv_obj_t *parent, int h)
{
    lv_obj_t *l = lv_obj_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(l, h);
    lv_obj_set_y(l, 0);
    lv_obj_set_style_bg_color(l, lv_color_hex(0x5A6A78), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(l, lv_color_hex(0x93A4B3), 0);
    lv_obj_set_style_border_width(l, 2, 0);
    lv_obj_set_style_border_side(l, LV_BORDER_SIDE_FULL, 0);
    return l;
}

static void make_door(lv_obj_t *parent, int x, int y, int w, int h, uint32_t accent)
{
    if (s_door_n >= DOOR_MAX) {
        return;
    }
    door_t *d = &s_door[s_door_n++];
    lv_obj_t *f = lv_obj_create(parent);
    lv_obj_remove_style_all(f);
    lv_obj_clear_flag(f, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(f, x, y);
    lv_obj_set_size(f, w, h);
    lv_obj_set_style_bg_color(f, lv_color_hex(0x2E5D4B), 0);   /* cabina iluminada */
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(f, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(f, 3, 0);
    lv_obj_set_style_radius(f, 8, 0);
    lv_obj_set_style_pad_all(f, 0, 0);
    lv_obj_set_style_clip_corner(f, true, 0);

    d->frame = f;
    d->inner_w = w - 6;                   /* menos el borde */
    int ih = h - 6;
    d->left = door_leaf(f, ih);
    d->right = door_leaf(f, ih);
    elevator_state_t state;
    elevator_get_state(&state);
    d->target = state.doors_open && !state.doors_closing;
    d->used = true;
    door_apply(d, d->target ? 100 : 0);
}

/* Lanza la animacion hacia abierta/cerrada solo si cambia el destino. */
static void door_set(bool open)
{
    for (int i = 0; i < s_door_n; i++) {
        door_t *d = &s_door[i];
        if (!d->used || d->target == open) {
            continue;
        }
        d->target = open;
        int to = open ? 100 : 0;
        lv_anim_del(d, door_anim_cb);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, d);
        lv_anim_set_exec_cb(&a, door_anim_cb);
        lv_anim_set_values(&a, d->open, to);
        int span = d->open > to ? d->open - to : to - d->open;
        lv_anim_set_time(&a, DOOR_ANIM_MS * span / 100 + 1);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
}

static void tap_cb(lv_event_t *e)
{
    (void)e;
    audio_play(AUDIO_SND_TAP);
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h,
                            const char *txt, const lv_font_t *font,
                            uint32_t color, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(p, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 16, 0);
    return p;
}

static lv_obj_t *make_button(lv_obj_t *parent, int x, int y, int w, int h,
                             const char *txt, lv_event_cb_t cb, intptr_t id,
                             lv_obj_t **out_label)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_BTN), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_BTN_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(COL_BTN_TEXT), 0);
    lv_obj_set_style_text_font(b, &F21, 0);
    lv_obj_add_event_cb(b, tap_cb, LV_EVENT_PRESSED, NULL);
    if (cb != NULL) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)id);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    if (out_label != NULL) {
        *out_label = l;
    }
    return b;
}

static void style_dropdown(lv_obj_t *dd, const lv_font_t *font);

static void dd_open_cb(lv_event_t *e)
{
    audio_play(AUDIO_SND_OPEN);
    lv_obj_t *list = lv_dropdown_get_list(lv_event_get_target(e));
    if (list == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(COL_BTN_TEXT), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_text_font(list, &F25, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_CALL),
                              LV_PART_SELECTED | LV_STATE_CHECKED);
}

static void style_dropdown(lv_obj_t *dd, const lv_font_t *font)
{
    lv_obj_set_style_bg_color(dd, lv_color_hex(COL_BTN), 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_radius(dd, 12, 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(COL_BTN_TEXT), 0);
    lv_obj_set_style_text_font(dd, font, 0);
    lv_obj_add_event_cb(dd, dd_open_cb, LV_EVENT_READY, NULL);
}

static void build_gpio_options(void)
{
    strcpy(s_gpio_options, "Sin asignar");
    for (int i = 0; i < USER_GPIO_COUNT; i++) {
        char tmp[24];
        snprintf(tmp, sizeof(tmp), "\nGPIO %d", s_gpios[i]);
        strcat(s_gpio_options, tmp);
    }
}

static int gpio_to_index(uint8_t gpio)
{
    for (int i = 0; i < USER_GPIO_COUNT; i++) {
        if ((uint8_t)s_gpios[i] == gpio) {
            return i + 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Navegacion entre pantallas (gesto horizontal)                     */
/* ------------------------------------------------------------------ */

static void goto_screen(int idx, bool from_right)
{
    if (idx < 0 || idx > 2 || idx == s_cur_scr) {
        return;
    }
    audio_play(from_right ? AUDIO_SND_SWIPE_NEXT : AUDIO_SND_SWIPE_PREV);
    s_cur_scr = idx;
    lv_scr_load_anim(s_scr[idx],
                     from_right ? LV_SCR_LOAD_ANIM_OVER_LEFT
                                : LV_SCR_LOAD_ANIM_OVER_RIGHT,
                     200, 0, false);
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT) {
        goto_screen(s_cur_scr + 1, true);
    } else if (dir == LV_DIR_RIGHT) {
        goto_screen(s_cur_scr - 1, false);
    }
}

static lv_obj_t *new_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s, lv_color_hex(COL_BLACK), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s, gesture_cb, LV_EVENT_GESTURE, NULL);
    return s;
}

/* Boton de sonido (mejora respecto al original): silencia / activa los avisos */
static lv_obj_t *s_snd_lbl[3];
static int s_snd_count;

static const char *snd_icon(void)
{
    return audio_get_volume() > 0 ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE;
}

static void snd_toggle_cb(lv_event_t *e)
{
    (void)e;
    app_state_t *app = app_state();
    app->cfg.sound = (audio_get_volume() > 0) ? 0 : 70;
    audio_set_volume(app->cfg.sound);
    storage_save_config(&app->cfg);
    for (int i = 0; i < s_snd_count; i++) {
        lv_label_set_text(s_snd_lbl[i], snd_icon());
    }
    audio_play(AUDIO_SND_ON);
}

static void rotate_async(void *unused);
static bool s_rotate_pending;

static void rot_cb(lv_event_t *e)
{
    (void)e;
    /* No se reconstruye la UI dentro del propio evento del boton */
    if (!s_rotate_pending) {
        s_rotate_pending = lv_async_call(rotate_async, NULL) == LV_RES_OK;
    }
}

/* Boton de giro 90 grados (vertical <-> horizontal); esta en TODAS las pantallas */
static void make_rotate_button(lv_obj_t *scr, int x, int y, int w, int h)
{
    lv_obj_t *b = make_button(scr, x, y, w, h, LV_SYMBOL_REFRESH, rot_cb, 0, NULL);
    lv_obj_set_style_text_font(b, &F25, 0);
}

static void make_sound_button(lv_obj_t *scr)
{
    if (s_snd_count >= 3) {
        return;
    }
    lv_obj_t *lbl = NULL;
    int x = LAND ? 952 : 496, y = LAND ? 24 : 34;
    lv_obj_t *b = make_button(scr, x, y, 60, 46, snd_icon(), snd_toggle_cb, 0, &lbl);
    (void)b;
    s_snd_lbl[s_snd_count++] = lbl;
    make_rotate_button(scr, x - 68, y, 60, 46);
}

/* Puntos indicadores + aviso inferior (0x40014dc8 del original) */
static void make_footer(lv_obj_t *scr, int active, uint32_t accent)
{
    make_sound_button(scr);
    int dot_x = LAND ? 488 : 270, dot_y = LAND ? 580 : 948;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *d = make_panel(scr, dot_x + 24 * i, dot_y, 8, 8);
        lv_obj_set_style_bg_color(d, lv_color_hex(i == active ? accent
                                                              : COL_DOT_OFF), 0);
        lv_obj_set_style_border_width(d, 0, 0);
    }
    if (LAND) {
        make_label(scr, 212, 554, 600, 22, "<  Desliza para cambiar de pantalla  >",
                   &F19, accent, LV_TEXT_ALIGN_CENTER);
    } else {
        make_label(scr, 40, 978, 520, 22, "<  Desliza para cambiar de pantalla  >",
                   &F19, accent, LV_TEXT_ALIGN_CENTER);
    }
}

/* ------------------------------------------------------------------ */
/*  Mensajes modales (errores de reglas)                              */
/* ------------------------------------------------------------------ */

static void msgbox_close_cb(lv_event_t *e)
{
    lv_obj_t *mb = lv_event_get_current_target(e);
    lv_msgbox_close(mb);
    s_msgbox = NULL;
}

void ui_show_message(const char *title, const char *body)
{
    if (!board_lock(200)) {
        return;
    }
    if (s_msgbox != NULL) {
        lv_msgbox_close(s_msgbox);
        s_msgbox = NULL;
    }
    static const char *btns[] = { "CERRAR", "" };
    s_msgbox = lv_msgbox_create(NULL, title, body, btns, true);
    lv_obj_set_width(s_msgbox, 480);
    lv_obj_set_style_bg_color(s_msgbox, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_text_color(s_msgbox, lv_color_hex(COL_BTN_TEXT), 0);
    lv_obj_set_style_text_font(s_msgbox, &F25, 0);
    lv_obj_add_event_cb(s_msgbox, msgbox_close_cb, LV_EVENT_VALUE_CHANGED, NULL);
    board_unlock();
}

/* ------------------------------------------------------------------ */
/*  Pantalla 1: EXTERIOR                                              */
/* ------------------------------------------------------------------ */

static void set_txt(lv_obj_t *l, const char *txt)
{
    if (l != NULL && strcmp(lv_label_get_text(l), txt) != 0) {
        lv_label_set_text(l, txt);
    }
}

static void set_hint(const char *txt)
{
    if (s_ext_hint != NULL) {
        lv_label_set_text(s_ext_hint, txt);
    }
}

static void ext_cb(lv_event_t *e)
{
    intptr_t id = (intptr_t)lv_event_get_user_data(e);
    uint8_t floors = app_state()->cfg.floors;
    char buf[96];

    switch (id) {
    case ID_PREV:
        if (s_user_floor > 1) {
            s_user_floor--;
            audio_play_floor(AUDIO_SND_PRESS, s_user_floor);
            snprintf(buf, sizeof(buf),
                     "Ahora estás en el piso %u. Pulsa SUBIR o BAJAR.", s_user_floor);
            set_hint(buf);
        } else {
            audio_play(AUDIO_SND_ERROR);
            set_hint("Ya estás en el piso más bajo.");
        }
        break;
    case ID_NEXT:
        if (s_user_floor < floors) {
            s_user_floor++;
            audio_play_floor(AUDIO_SND_PRESS, s_user_floor);
            snprintf(buf, sizeof(buf),
                     "Ahora estás en el piso %u. Pulsa SUBIR o BAJAR.", s_user_floor);
            set_hint(buf);
        } else {
            audio_play(AUDIO_SND_ERROR);
            set_hint("Ya estás en el piso más alto.");
        }
        break;
    case ID_UP:
        audio_play(AUDIO_SND_CALL_UP);
        elevator_call(s_user_floor, ELEV_DIR_UP);
        break;
    case ID_DOWN:
        audio_play(AUDIO_SND_CALL_DOWN);
        elevator_call(s_user_floor, ELEV_DIR_DOWN);
        break;
    default:
        break;
    }
    ui_refresh();
}

static void create_exterior(lv_obj_t *scr)
{
    /* Vertical: una columna. Horizontal: izquierda = estado, derecha = llamadas. */
    int lx = LAND ? 40 : 44, lw = LAND ? 460 : 512;

    make_label(scr, 44, LAND ? 30 : 42, 360, 25, "ASCENSOR / EXTERIOR", &F21, EXT_MUTED,
               LV_TEXT_ALIGN_LEFT);
    make_label(scr, 40, LAND ? 84 : 120, LAND ? 460 : 520, 32, "ESTÁS EN EL PISO", &F25,
               EXT_MUTED, LV_TEXT_ALIGN_CENTER);
    s_ext_user_floor = make_label(scr, LAND ? 40 : 100, LAND ? 112 : 182, LAND ? 460 : 400,
                                  144, "", &F66, EXT_TEXT, LV_TEXT_ALIGN_CENTER);
    s_b_prev = make_button(scr, LAND ? 50 : 90, LAND ? 268 : 358, 190, 66, "Piso anterior",
                           ext_cb, ID_PREV, NULL);
    s_b_next = make_button(scr, LAND ? 270 : 320, LAND ? 268 : 358, 190, 66, "Piso siguiente",
                           ext_cb, ID_NEXT, NULL);

    lv_obj_t *p = make_panel(scr, lx, LAND ? 352 : 470, lw, 170);
    lv_obj_set_style_bg_color(p, lv_color_hex(COL_PANEL_EXT), 0);
    make_label(p, 24, 20, 330, 26, "EL ASCENSOR ESTÁ EN", &F21, EXT_MUTED,
               LV_TEXT_ALIGN_LEFT);
    s_ext_cabin_floor = make_label(p, 24, 62, 180, 60, "", &F59, EXT_TEXT,
                                   LV_TEXT_ALIGN_LEFT);
    s_ext_arrow = make_label(p, 350, 46, 100, 65, "", &F59, EXT_TEXT,
                             LV_TEXT_ALIGN_LEFT);
    s_ext_dir = make_label(p, 225, 122, LAND ? 220 : 265, 28, "", &F19, EXT_TEXT,
                           LV_TEXT_ALIGN_LEFT);
    make_door(p, 248, 52, 90, 66, COL_OK);   /* bajo el titulo, entre el piso y la flecha */

    int rx = LAND ? 540 : 40;     /* bloque de llamadas */
    make_label(scr, rx, LAND ? 110 : 685, LAND ? 444 : 520, 34, "¿Hacia dónde vas?", &F30,
               EXT_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *up = make_button(scr, LAND ? 540 : 44, LAND ? 170 : 746, LAND ? 210 : 246,
                               LAND ? 130 : 112, LV_SYMBOL_UP "  SUBIR", ext_cb, ID_UP, NULL);
    lv_obj_t *dn = make_button(scr, LAND ? 774 : 310, LAND ? 170 : 746, LAND ? 210 : 246,
                               LAND ? 130 : 112, LV_SYMBOL_DOWN "  BAJAR", ext_cb, ID_DOWN, NULL);
    s_b_up = up;
    s_b_down = dn;
    lv_obj_set_style_bg_color(up, lv_color_hex(COL_CALL), 0);
    lv_obj_set_style_bg_color(dn, lv_color_hex(COL_CALL), 0);
    lv_obj_set_style_text_font(up, &F38, 0);
    lv_obj_set_style_text_font(dn, &F38, 0);

    s_ext_hint = make_label(scr, rx, LAND ? 330 : 884, LAND ? 444 : 520, LAND ? 70 : 48,
                            "1) Elige tu piso   2) Pulsa SUBIR o BAJAR   3) Espera la campanilla",
                            &F19, EXT_MUTED, LV_TEXT_ALIGN_CENTER);
    make_footer(scr, 0, EXT_ACCENT);
}

/* ------------------------------------------------------------------ */
/*  Pantalla 2: INTERIOR                                              */
/* ------------------------------------------------------------------ */

static void int_floor_cb(lv_event_t *e)
{
    uint32_t floor = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    audio_play_floor(AUDIO_SND_PRESS, (int)floor);
    elevator_call((uint8_t)floor, ELEV_DIR_IDLE);
    ui_refresh();
}

static void rebuild_interior_buttons(void)
{
    if (s_int_grid == NULL) {
        return;
    }
    lv_obj_clean(s_int_grid);
    memset(s_int_btn, 0, sizeof(s_int_btn));

    uint8_t floors = app_state()->cfg.floors;
    s_int_btn_count = floors;

    for (uint8_t n = 1; n <= floors; n++) {
        char txt[8];
        snprintf(txt, sizeof(txt), "P%02u", n);
        int col = (n - 1) % 3;
        int row = (n - 1) / 3;
        lv_obj_t *lbl = NULL;
        lv_obj_t *b = make_button(s_int_grid, col * 176, row * 106, 160, 90, txt,
                                  int_floor_cb, n, &lbl);
        lv_obj_set_style_text_font(b, &F38, 0);
        lv_obj_set_style_radius(b, 45, 0);
        s_int_btn[n - 1] = b;
    }
    s_last_state.current_floor = 0xFF;  /* fuerza refresco */
}

static void create_interior(lv_obj_t *scr)
{
    int cx = LAND ? 40 : 40, cw = LAND ? 380 : 520;     /* columna de estado */
    make_label(scr, 44, LAND ? 30 : 42, 400, 28, "ASCENSOR / INTERIOR", &F21, INT_MUTED,
               LV_TEXT_ALIGN_LEFT);
    make_label(scr, cx, LAND ? 90 : 111, cw, 30, "ESTÁS AQUÍ", &F25, INT_MUTED,
               LV_TEXT_ALIGN_CENTER);
    s_int_current = make_label(scr, LAND ? 40 : 100, LAND ? 125 : 165, LAND ? 380 : 400, 144,
                               "", &F66, INT_ACCENT, LV_TEXT_ALIGN_CENTER);
    s_int_dir = make_label(scr, cx, LAND ? 285 : 325, cw, 32, "", &F21, INT_ACCENT,
                           LV_TEXT_ALIGN_CENTER);
    make_label(scr, LAND ? 450 : 40, LAND ? 90 : 394, LAND ? 534 : 520, 36,
               "¿A qué piso quieres ir?", &F30, TEC_TEXT, LV_TEXT_ALIGN_CENTER);

    s_int_grid = make_panel(scr, LAND ? 456 : 44, LAND ? 140 : 454, 512, LAND ? 400 : 320);
    lv_obj_set_style_bg_opa(s_int_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_int_grid, 0, 0);
    lv_obj_add_flag(s_int_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_int_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_int_grid, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *status = lv_obj_create(scr);
    lv_obj_remove_style_all(status);
    lv_obj_set_pos(status, cx, LAND ? 340 : 786);
    lv_obj_set_size(status, cw, LV_SIZE_CONTENT);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(status, 8, 0);
    s_int_status = make_label(status, 0, 0, cw, LV_SIZE_CONTENT,
                              "Toca el piso al que quieres ir: la cabina lo agrega a su ruta.",
                              &F19, INT_MUTED, LV_TEXT_ALIGN_CENTER);
    make_door(status, 0, 0, 100, 96, INT_ACCENT);
    make_footer(scr, 1, INT_ACCENT);
}

/* ------------------------------------------------------------------ */
/*  Pantalla 3: TECNICA                                               */
/* ------------------------------------------------------------------ */

static void open_blocks_cb(lv_event_t *e);
static void rebuild_block_rows(void);

static void tec_note(const char *txt)
{
    if (s_tec_note != NULL) {
        lv_label_set_text(s_tec_note, txt);
    }
}

static void rebuild_floor_dropdown(void)
{
    if (s_tec_dd_floor == NULL) {
        return;
    }
    app_state_t *app = app_state();
    char opts[ELEVATOR_MAX_FLOORS * 10];
    opts[0] = '\0';
    for (uint8_t f = 1; f <= app->cfg.floors; f++) {
        char tmp[12];
        snprintf(tmp, sizeof(tmp), "%sPiso %u", f == 1 ? "" : "\n", f);
        strcat(opts, tmp);
    }
    if (s_sel_floor > app->cfg.floors) {
        s_sel_floor = app->cfg.floors;
    }
    lv_dropdown_set_options(s_tec_dd_floor, opts);
    lv_dropdown_set_selected(s_tec_dd_floor, s_sel_floor - 1);
}

static void refresh_tech_labels(void)
{
    app_state_t *app = app_state();
    char buf[64];

    if (s_tec_count != NULL) {
        snprintf(buf, sizeof(buf), "%u", app->cfg.floors);
        lv_label_set_text(s_tec_count, buf);
    }
    const char *mode = "GPIO REALES";
    if (s_tec_mode_lbl != NULL) {
        lv_label_set_text(s_tec_mode_lbl, mode);
    }
    if (s_tec_mode_btn_lbl != NULL) {
        lv_label_set_text(s_tec_mode_btn_lbl, "VER PISOS");
    }
    if (s_tec_dd_gpio != NULL) {
        lv_dropdown_set_selected(
            s_tec_dd_gpio, gpio_to_index(app->cfg.floor_gpio[s_sel_floor - 1]));
    }
    if (s_tec_dd_level != NULL) {
        lv_dropdown_set_selected(s_tec_dd_level,
                                 app->cfg.floor_level[s_sel_floor - 1] ? 1 : 0);
    }
    if (s_tec_then != NULL) {
        snprintf(buf, sizeof(buf), "ENTONCES piso %u + encender su luz",
                 s_sel_floor);
        lv_label_set_text(s_tec_then, buf);
    }
    if (s_tec_read != NULL) {
        bool has_gpio = app->cfg.floor_gpio[s_sel_floor - 1] != 0xFF;
        elevator_state_t st;
        elevator_get_state(&st);
        if (st.position_known) {
            snprintf(buf, sizeof(buf), "Piso detectado: %u · %s",
                     st.current_floor, has_gpio ? "GPIO activo" : "sin GPIO");
        } else {
            snprintf(buf, sizeof(buf), "Lectura: %s",
                     has_gpio ? "GPIO activo" : "sin GPIO");
        }
        lv_label_set_text(s_tec_read, buf);
    }
    if (s_blk_status != NULL) {
        snprintf(buf, sizeof(buf), "BLOQUES ACTIVOS %u/%u",
                 (unsigned)rules_active_count(), (unsigned)RULES_MAX_BLOCKS);
        lv_label_set_text(s_blk_status, buf);
    }
}

static void tec_floors_cb(lv_event_t *e)
{
    intptr_t id = (intptr_t)lv_event_get_user_data(e);
    app_state_t *app = app_state();

    elevator_state_t state; elevator_get_state(&state);
    if (state.moving || app->cfg.blocks_enabled) {
        audio_play(AUDIO_SND_ERROR);
        tec_note("Deten la cabina y desactiva bloques antes de configurar.");
        return;
    }
    if (id == ID_MINUS && app->cfg.floors > ELEVATOR_MIN_FLOORS) {
        app->cfg.floors--;
        audio_play(AUDIO_SND_MINUS);
    } else if (id == ID_PLUS && app->cfg.floors < ELEVATOR_MAX_FLOORS) {
        app->cfg.floors++;
        audio_play(AUDIO_SND_PLUS);
    } else {
        audio_play(AUDIO_SND_ERROR);
        tec_note(id == ID_MINUS ? "Mínimo de pisos alcanzado."
                                : "Máximo de pisos alcanzado.");
        return;
    }
    if (s_user_floor > app->cfg.floors) {
        s_user_floor = app->cfg.floors;
    }
    app_apply_config();
    rebuild_floor_dropdown();
    rebuild_interior_buttons();
    refresh_tech_labels();
    ui_refresh();
}

static void overview_back_cb(lv_event_t *e)
{
    (void)e;
    lv_scr_load(s_scr[2]);
}

static void tec_mode_cb(lv_event_t *e)
{
    (void)e;
    lv_scr_load(s_scr_overview);
}

static void tec_floor_sel_cb(lv_event_t *e)
{
    s_sel_floor = (uint8_t)(lv_dropdown_get_selected(lv_event_get_target(e)) + 1);
    audio_play_floor(AUDIO_SND_PRESS, s_sel_floor);
    tec_note("Configuras este piso: elige qué GPIO lo detecta y con qué nivel.");
    refresh_tech_labels();
}

static void tec_gpio_cb(lv_event_t *e)
{
    app_state_t *app = app_state();
    elevator_state_t state; elevator_get_state(&state);
    if (state.moving || rules_enabled()) {
        tec_note("Deten la cabina y desactiva bloques antes de configurar.");
        refresh_tech_labels(); return;
    }

    int sel = (int)lv_dropdown_get_selected(lv_event_get_target(e));

    uint8_t gpio = (sel == 0) ? 0xFF : (uint8_t)s_gpios[sel - 1];
    for (int i = 0; i < app->cfg.floors; i++) {
        if (i != (s_sel_floor - 1) && gpio != 0xFF &&
            app->cfg.floor_gpio[i] == gpio) {
            audio_play(AUDIO_SND_ERROR);
            tec_note("Ese GPIO ya identifica otro piso.");
            refresh_tech_labels();
            return;
        }
    }
    app->cfg.floor_gpio[s_sel_floor - 1] = gpio;
    app_apply_config();
    audio_play(AUDIO_SND_OK);
    tec_note("Regla GPIO guardada.");
    refresh_tech_labels();
}

static void tec_level_cb(lv_event_t *e)
{
    app_state_t *app = app_state();
    elevator_state_t state; elevator_get_state(&state);
    if (state.moving || rules_enabled()) {
        tec_note("Deten la cabina y desactiva bloques antes de configurar.");
        refresh_tech_labels(); return;
    }

    app->cfg.floor_level[s_sel_floor - 1] =
        (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    app_apply_config();
    audio_play(AUDIO_SND_OK);
    tec_note("Regla GPIO guardada.");
}

/* Tecnica en horizontal: izquierda = edificio y editor; derecha = regla por piso */
static void create_tecnica_land(lv_obj_t *scr)
{
    make_label(scr, 44, 30, 400, 28, "ASCENSOR / TÉCNICA", &F21, TEC_ACCENT, LV_TEXT_ALIGN_LEFT);
    make_label(scr, 44, 70, 456, 40, "Configura tu edificio", &F30, TEC_TEXT, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *p = make_panel(scr, 40, 118, 460, 232);
    make_label(p, 24, 20, 420, 30, "Cantidad de pisos", &F25, TEC_TEXT, LV_TEXT_ALIGN_LEFT);
    s_tec_count = make_label(p, 24, 64, 170, 56, "", &F59, TEC_ACCENT, LV_TEXT_ALIGN_LEFT);
    s_b_minus = make_button(p, 230, 58, 90, 64, "-", tec_floors_cb, ID_MINUS, NULL);
    s_b_plus = make_button(p, 336, 58, 90, 64, "+", tec_floors_cb, ID_PLUS, NULL);
    make_label(p, 24, 154, 220, 24, "Origen de posición", &F21, TEC_MUTED, LV_TEXT_ALIGN_LEFT);
    s_tec_mode_lbl = make_label(p, 24, 182, 180, 32, "", &F30, TEC_TEXT, LV_TEXT_ALIGN_LEFT);
    s_b_mode = make_button(p, 206, 150, 220, 60, "SENSORES", tec_mode_cb, ID_MODE,
                           &s_tec_mode_btn_lbl);

    s_tec_note = make_label(scr, 40, 364, 460, 60,
                            "Elige el piso, asigna su GPIO y el nivel con el que se activa.",
                            &F19, TEC_MUTED, LV_TEXT_ALIGN_CENTER);
    s_b_edit = make_button(scr, 40, 440, 460, 57, "EDITAR BLOQUES Y NOTAS", open_blocks_cb,
                           ID_EDIT, NULL);

    make_label(scr, 524, 92, 240, 36, "Regla por piso", &F30, TEC_TEXT, LV_TEXT_ALIGN_LEFT);
    s_tec_dd_floor = lv_dropdown_create(scr);
    lv_obj_set_pos(s_tec_dd_floor, 774, 86);
    lv_obj_set_size(s_tec_dd_floor, 206, 52);
    style_dropdown(s_tec_dd_floor, &F25);
    lv_obj_add_event_cb(s_tec_dd_floor, tec_floor_sel_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *rule = make_panel(scr, 524, 150, 460, 180);
    lv_obj_set_style_border_color(rule, lv_color_hex(COL_BLUE_BORDER), 0);
    make_label(rule, 24, 12, 410, 27, "SI entrada digital", &F25, TEC_ACCENT, LV_TEXT_ALIGN_LEFT);
    s_tec_dd_gpio = lv_dropdown_create(rule);
    lv_dropdown_set_options(s_tec_dd_gpio, s_gpio_options);
    lv_obj_set_pos(s_tec_dd_gpio, 24, 53);
    lv_obj_set_size(s_tec_dd_gpio, 220, 52);
    style_dropdown(s_tec_dd_gpio, &F25);
    lv_obj_add_event_cb(s_tec_dd_gpio, tec_gpio_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_tec_dd_level = lv_dropdown_create(rule);
    lv_dropdown_set_options(s_tec_dd_level, "= 0\n= 1");
    lv_obj_set_pos(s_tec_dd_level, 262, 53);
    lv_obj_set_size(s_tec_dd_level, 170, 52);
    style_dropdown(s_tec_dd_level, &F25);
    lv_obj_add_event_cb(s_tec_dd_level, tec_level_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_tec_read = make_label(rule, 24, 126, 420, 32, "", &F25, TEC_TEXT, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *then = make_panel(scr, 524, 346, 460, 83);
    lv_obj_set_style_border_color(then, lv_color_hex(COL_OK), 0);
    s_tec_then = make_label(then, 24, 26, 420, 32, "", &F25, TEC_GREEN, LV_TEXT_ALIGN_LEFT);
    make_footer(scr, 2, TEC_ACCENT);
}

static void create_tecnica(lv_obj_t *scr)
{
    if (LAND) {
        create_tecnica_land(scr);
        return;
    }
    make_label(scr, 44, 42, 400, 28, "ASCENSOR / TÉCNICA", &F21, TEC_ACCENT,
               LV_TEXT_ALIGN_LEFT);
    make_label(scr, 44, 110, 512, 48, "Configura tu edificio", &F30, TEC_TEXT,
               LV_TEXT_ALIGN_LEFT);

    lv_obj_t *p = make_panel(scr, 44, 184, 512, 232);
    make_label(p, 24, 20, 440, 30, "Cantidad de pisos", &F25, TEC_TEXT,
               LV_TEXT_ALIGN_LEFT);
    s_tec_count = make_label(p, 24, 64, 170, 56, "", &F59, TEC_ACCENT,
                             LV_TEXT_ALIGN_LEFT);
    s_b_minus = make_button(p, 272, 58, 90, 64, "-", tec_floors_cb, ID_MINUS, NULL);
    s_b_plus = make_button(p, 386, 58, 90, 64, "+", tec_floors_cb, ID_PLUS, NULL);
    make_label(p, 24, 154, 220, 24, "Origen de posición", &F21, TEC_MUTED,
               LV_TEXT_ALIGN_LEFT);
    s_tec_mode_lbl = make_label(p, 24, 182, 180, 32, "", &F30, TEC_TEXT,
                                LV_TEXT_ALIGN_LEFT);
    s_b_mode = make_button(p, 248, 150, 230, 60, "SENSORES", tec_mode_cb, ID_MODE,
                &s_tec_mode_btn_lbl);

    make_label(scr, 44, 453, 290, 36, "Regla por piso", &F30, TEC_TEXT,
               LV_TEXT_ALIGN_LEFT);
    s_tec_dd_floor = lv_dropdown_create(scr);
    lv_obj_set_pos(s_tec_dd_floor, 350, 448);
    lv_obj_set_size(s_tec_dd_floor, 206, 52);
    style_dropdown(s_tec_dd_floor, &F25);
    lv_obj_add_event_cb(s_tec_dd_floor, tec_floor_sel_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *rule = make_panel(scr, 44, 526, 512, 180);
    lv_obj_set_style_border_color(rule, lv_color_hex(COL_BLUE_BORDER), 0);
    make_label(rule, 24, 12, 450, 27, "SI entrada digital", &F25, TEC_ACCENT,
               LV_TEXT_ALIGN_LEFT);
    s_tec_dd_gpio = lv_dropdown_create(rule);
    lv_dropdown_set_options(s_tec_dd_gpio, s_gpio_options);
    lv_obj_set_pos(s_tec_dd_gpio, 24, 53);
    lv_obj_set_size(s_tec_dd_gpio, 250, 52);
    style_dropdown(s_tec_dd_gpio, &F25);
    lv_obj_add_event_cb(s_tec_dd_gpio, tec_gpio_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_tec_dd_level = lv_dropdown_create(rule);
    lv_dropdown_set_options(s_tec_dd_level, "= 0\n= 1");
    lv_obj_set_pos(s_tec_dd_level, 300, 53);
    lv_obj_set_size(s_tec_dd_level, 185, 52);
    style_dropdown(s_tec_dd_level, &F25);
    lv_obj_add_event_cb(s_tec_dd_level, tec_level_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);

    s_tec_read = make_label(rule, 24, 126, 460, 32, "", &F25, TEC_TEXT,
                            LV_TEXT_ALIGN_LEFT);

    lv_obj_t *then = make_panel(scr, 44, 725, 512, 83);
    lv_obj_set_style_border_color(then, lv_color_hex(COL_OK), 0);
    s_tec_then = make_label(then, 24, 26, 465, 32, "", &F25, TEC_GREEN,
                            LV_TEXT_ALIGN_LEFT);

    s_tec_note = make_label(scr, 40, 823, 520, 45, "Elige el piso, asigna su GPIO y el nivel con el que se activa.",
                            &F19, TEC_MUTED, LV_TEXT_ALIGN_CENTER);
    s_b_edit = make_button(scr, 44, 878, 512, 57, "EDITAR BLOQUES Y NOTAS", open_blocks_cb,
                ID_EDIT, NULL);
    make_footer(scr, 2, TEC_ACCENT);
}

/* ------------------------------------------------------------------ */
/*  Editor de bloques (pantalla propia, 32 bloques compartidos)       */
/* ------------------------------------------------------------------ */

static void rebuild_block_rows_async(void *unused)
{
    (void)unused;
    rebuild_block_rows();
}

static void close_blocks_cb(lv_event_t *e)
{
    (void)e;
    audio_play(AUDIO_SND_SWIPE_PREV);
    lv_scr_load_anim(s_scr[2], LV_SCR_LOAD_ANIM_OVER_RIGHT, 200, 0, false);
}

static void open_blocks_cb(lv_event_t *e)
{
    (void)e;
    audio_play(AUDIO_SND_SWIPE_NEXT);
    refresh_tech_labels();
    lv_scr_load_anim(s_scr_blocks, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0, false);
}

static bool blocks_editable(void)
{
    elevator_state_t st; elevator_get_state(&st);
    if (st.moving) { ui_show_message("Cabina en marcha", "Deten la cabina antes de editar."); return false; }
    if (app_state()->cfg.blocks_enabled) {
        audio_play(AUDIO_SND_ERROR);
        ui_show_message("Reglas activas",
                        "Desactiva las reglas para editarlas.");
        return false;
    }
    return true;
}

static void blk_sw_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    char err[RULES_ERR_MAX] = { 0 };
    if (app_set_blocks_enabled(on, err, sizeof(err)) != ESP_OK) {
        if (on) {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
        }
        audio_play(AUDIO_SND_ERROR);
        ui_show_message("No se pudo activar", err);
        return;
    }
    audio_play(on ? AUDIO_SND_POWER_ON : AUDIO_SND_POWER_OFF);
    refresh_tech_labels();
}

static void blk_add_cb(lv_event_t *e)
{
    (void)e;
    if (!blocks_editable()) {
        return;
    }
    if (app_add_block(NULL) != ESP_OK) {
        audio_play(AUDIO_SND_ERROR);
        ui_show_message("Capacidad completa",
                        "32 bloques compartidos entre todos los pisos.");
        return;
    }
    app_save_blocks();
    audio_play(AUDIO_SND_ADD);
    rebuild_block_rows();
    refresh_tech_labels();
}

static void blk_delete_cb(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (!blocks_editable()) {
        return;
    }
    app_remove_block(idx);
    audio_play(AUDIO_SND_REMOVE);
    lv_async_call(rebuild_block_rows_async, NULL);
    refresh_tech_labels();
}

enum {
    BLK_IN_GPIO, BLK_IN_LEVEL, BLK_OUT_GPIO, BLK_OUT_LEVEL, BLK_REST_LEVEL,
    BLK_SOURCE, BLK_FLOOR, BLK_ACTION, BLK_ACTION_FLOOR, BLK_MODE
};

static const char *const block_modes[] = {
    "Programaci\xC3\xB3n tipo GPIO",
    "Programaci\xC3\xB3n tipo Bot\xC3\xB3n",
    "Condici\xC3\xB3n de ascensor",
};

static int block_mode(const rule_block_t *block)
{
    if (block->source == RULE_GPIO) return 0;
    return block->source <= RULE_BUTTON_CABIN ? 1 : 2;
}

static void blk_dd_cb(lv_event_t *e)
{
    uint32_t packed = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    size_t idx    = packed >> 8;
    uint8_t field = packed & 0xFF;
    int sel = (int)lv_dropdown_get_selected(lv_event_get_target(e));

    app_state_t *app = app_state();
    if (idx >= app->block_count) {
        return;
    }
    if (!blocks_editable()) { lv_async_call(rebuild_block_rows_async, NULL); return; }
    rule_block_t *b = &app->blocks[idx];

    rule_block_t previous = *b;
    bool rebuild = false;
    switch (field) {
    case BLK_IN_GPIO: b->in_gpio = sel == 0 ? 0xFF : (uint8_t)s_gpios[sel - 1]; break;
    case BLK_IN_LEVEL: b->in_level = (uint8_t)sel; break;
    case BLK_OUT_GPIO: b->out_gpio = sel == 0 ? 0xFF : (uint8_t)s_gpios[sel - 1]; break;
    case BLK_OUT_LEVEL: b->out_level = (uint8_t)sel; break;
    case BLK_REST_LEVEL: b->rest_level = (uint8_t)sel; break;
    case BLK_ACTION:
        b->action = (uint8_t)sel;
        rebuild = true;
        break;
    case BLK_ACTION_FLOOR: b->action_floor = (uint8_t)sel; break;
    case BLK_SOURCE:
        b->source = (uint8_t)(sel + (block_mode(b) == 1 ? RULE_BUTTON_UP : RULE_AT_FLOOR));
        break;
    case BLK_FLOOR: b->floor = (uint8_t)sel; break;
    case BLK_MODE:
        if (sel == block_mode(b)) return;
        b->source = sel == 0 ? RULE_GPIO : sel == 1 ? RULE_BUTTON_UP : RULE_AT_FLOOR;
        b->in_level = 1;
        rebuild = true;
        break;
    default: return;
    }
    if (app_save_blocks() != ESP_OK) {
        *b = previous;
        ui_show_message("Error de guardado", "No se pudo guardar el bloque.");
        rebuild = true;
    } else {
        audio_play(AUDIO_SND_OK);
    }
    if (rebuild) lv_async_call(rebuild_block_rows_async, NULL);
}

static lv_obj_t *blk_dropdown(lv_obj_t *parent, const char *options,
                              int selected, int w, size_t idx, uint8_t field)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, selected);
    lv_obj_set_user_data(dd, (void *)(uintptr_t)field);
    lv_obj_set_width(dd, w);
    style_dropdown(dd, &F21);
    lv_obj_add_event_cb(dd, blk_dd_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)((idx << 8) | field));
    return dd;
}

static lv_obj_t *blk_text(lv_obj_t *parent, const char *txt, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &F19, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void rebuild_block_rows(void)
{
    if (s_blk_list == NULL) {
        return;
    }
    lv_coord_t scroll_y = lv_obj_get_scroll_y(s_blk_list);
    lv_obj_clean(s_blk_list);
    app_state_t *app = app_state();

    for (size_t i = 0; i < app->block_count; i++) {
        rule_block_t *b = &app->blocks[i];

        int bw = LAND ? 936 : 512;
        lv_obj_t *box = make_panel(s_blk_list, 0, 0, bw, LV_SIZE_CONTENT);
        lv_obj_set_width(box, bw);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(box, 14, 0);
        lv_obj_set_style_pad_row(box, 10, 0);

        lv_obj_t *head = lv_obj_create(box);
        lv_obj_remove_style_all(head);
        lv_obj_set_size(head, lv_pct(100), 48);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        char title[32];
        snprintf(title, sizeof(title), "Bloque %u", (unsigned)(i + 1));
        lv_obj_t *t = blk_text(head, title, TEC_TEXT);
        lv_obj_set_style_text_font(t, &F25, 0);
        make_button(head, 0, 0, 140, 44, "ELIMINAR", blk_delete_cb, (intptr_t)i,
                    NULL);
        lv_obj_set_pos(lv_obj_get_child(head, 1), 0, 0);

        lv_obj_t *row = lv_obj_create(box);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(row, 8, 0);
        lv_obj_set_style_pad_column(row, 8, 0);

        int mode = block_mode(b);
        char modes[128];
        snprintf(modes, sizeof(modes), "%s\n%s%s%s", block_modes[0], block_modes[1],
                 mode == 2 ? "\n" : "", mode == 2 ? block_modes[2] : "");
        blk_dropdown(row, modes, mode, 460, i, BLK_MODE);

        char floors[180] = "Cualquier piso";
        for (int f = 1; f <= app->cfg.floors; f++) {
            char item[12];
            snprintf(item, sizeof(item), "\nPiso %d", f);
            strncat(floors, item, sizeof(floors) - strlen(floors) - 1);
        }

        lv_obj_add_flag(blk_text(row, "SI", TEC_MUTED), LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
        if (mode == 0) {
            blk_dropdown(row, s_gpio_options, gpio_to_index(b->in_gpio), 170, i, BLK_IN_GPIO);
            blk_dropdown(row, "ES IGUAL A 0\nES IGUAL A 1", b->in_level, 190, i, BLK_IN_LEVEL);
        } else {
            const char *sources = mode == 1 ? "SUBIR\nBAJAR\nCABINA" :
                "En piso\nSubiendo\nBajando\nPuertas abiertas\nLlamada pendiente";
            blk_dropdown(row, sources,
                         b->source - (mode == 1 ? RULE_BUTTON_UP : RULE_AT_FLOOR),
                         235, i, BLK_SOURCE);
            blk_dropdown(row, floors, b->floor, 185, i, BLK_FLOOR);
            blk_dropdown(row, mode == 1 ? "Sin pulso\nAl pulsar" :
                         "Condicion falsa\nCondicion verdadera", b->in_level, 220, i, BLK_IN_LEVEL);
        }

        lv_obj_add_flag(blk_text(row, "ENTONCES", TEC_MUTED), LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
        blk_dropdown(row, "Salida GPIO\nPedir cabina\nLlamada SUBIR\nLlamada BAJAR",
                     b->action, 220, i, BLK_ACTION);
        if (b->action == 0) {
            blk_dropdown(row, s_gpio_options, gpio_to_index(b->out_gpio), 170, i, BLK_OUT_GPIO);
            blk_dropdown(row, "ESCRIBIR 0\nESCRIBIR 1", b->out_level, 190, i, BLK_OUT_LEVEL);
            lv_obj_add_flag(blk_text(row, "REPOSO", TEC_MUTED), LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
            blk_dropdown(row, "= 0\n= 1", b->rest_level, 110, i, BLK_REST_LEVEL);
        } else {
            char destinations[180];
            snprintf(destinations, sizeof(destinations), "Elige piso%s", strchr(floors, '\n'));
            blk_dropdown(row, destinations, b->action_floor, 200, i, BLK_ACTION_FLOOR);
        }

        if (b->note[0] != '\0') {
            blk_text(box, b->note, TEC_MUTED);
        }
    }

    if (app->block_count == 0) {
        blk_text(s_blk_list, "Sin bloques. Usa + Agregar condicional.", TEC_MUTED);
    }
    lv_obj_update_layout(s_blk_list);
    lv_obj_scroll_to_y(s_blk_list, scroll_y, LV_ANIM_OFF);
}

static void create_blocks_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BLACK), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_scr_blocks = scr;

    int cw = LAND ? 936 : 512;               /* ancho del contenido */
    make_label(scr, 44, LAND ? 30 : 42, 330, 28, "EDITAR BLOQUES Y NOTAS", &F21, TEC_ACCENT,
               LV_TEXT_ALIGN_LEFT);
    make_rotate_button(scr, LAND ? 770 : 330, LAND ? 24 : 30, 76, 50);
    s_b_back = make_button(scr, LAND ? 848 : 420, LAND ? 24 : 30, 136, 50, "VOLVER",
                           close_blocks_cb, 0, NULL);

    int top = LAND ? 90 : 100;
    lv_obj_t *head = make_panel(scr, 44, top, cw, LAND ? 100 : 120);
    make_label(head, 24, 14, 330, 30, "Programación por bloques", &F25, TEC_TEXT,
               LV_TEXT_ALIGN_LEFT);
    s_blk_sw = lv_switch_create(head);
    lv_obj_set_pos(s_blk_sw, cw - 112, 14);
    lv_obj_set_size(s_blk_sw, 90, 46);
    lv_obj_add_event_cb(s_blk_sw, blk_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_blk_status = make_label(head, 24, 62, 460, 28, "", &F21, TEC_MUTED,
                              LV_TEXT_ALIGN_LEFT);
    s_b_add = make_button(scr, 44, top + (LAND ? 114 : 134), cw, 60, "+ Agregar condicional",
                          blk_add_cb, 0, NULL);

    int ly = top + (LAND ? 114 : 134) + 74;
    s_blk_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_blk_list);
    lv_obj_set_pos(s_blk_list, 44, ly);
    lv_obj_set_size(s_blk_list, cw, SCREEN_H - ly - (LAND ? 8 : 26));
    lv_obj_set_flex_flow(s_blk_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_blk_list, 12, 0);
    lv_obj_add_flag(s_blk_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_blk_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_blk_list, LV_SCROLLBAR_MODE_OFF);
}

/* ------------------------------------------------------------------ */
/*  Creacion y refresco                                               */
/* ------------------------------------------------------------------ */

static void build_overview(void)
{
    s_scr_overview = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_overview, lv_color_hex(COL_BLACK), 0);
    make_label(s_scr_overview, 30, LAND ? 28 : 35, 280, 45, "TODOS LOS PISOS", &F30, TEC_TEXT,
               LV_TEXT_ALIGN_LEFT);
    make_rotate_button(s_scr_overview, LAND ? 770 : 330, LAND ? 24 : 30, 76, 50);
    make_button(s_scr_overview, LAND ? 848 : 420, LAND ? 24 : 30, 140, 50, "VOLVER",
                overview_back_cb, 0, NULL);
    lv_obj_t *list = lv_obj_create(s_scr_overview);
    lv_obj_set_pos(list, 20, LAND ? 90 : 100);
    lv_obj_set_size(list, LAND ? 984 : 555, LAND ? 490 : 850);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_BLACK), 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    s_overview_rows = make_label(list, 5, 5, LAND ? 940 : 520, LV_SIZE_CONTENT, "", &F21,
                                 TEC_TEXT, LV_TEXT_ALIGN_LEFT);
}

/* Construye las 5 pantallas con la orientacion actual (s_land). */
static void build_all(void)
{
    s_door_n = 0;
    memset(s_door, 0, sizeof(s_door));
    s_snd_count = 0;

    for (int i = 0; i < 3; i++) {
        s_scr[i] = new_screen();
    }
    create_exterior(s_scr[0]);
    create_interior(s_scr[1]);
    create_tecnica(s_scr[2]);
    create_blocks_screen();
    build_overview();

    rebuild_interior_buttons();
    rebuild_floor_dropdown();
    rebuild_block_rows();
    refresh_tech_labels();

    if (app_state()->cfg.blocks_enabled) {
        lv_obj_add_state(s_blk_sw, LV_STATE_CHECKED);
    }
    s_last_state.floors = 0xFF;   /* fuerza que ui_refresh repinte todo */
}

esp_err_t ui_create(void)
{
    if (!board_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    init_fonts();
    build_gpio_options();

    s_land = app_state()->cfg.landscape;
    if (s_land) {
        board_set_landscape(true);
        lv_obj_set_size(lv_layer_top(), SCREEN_W, SCREEN_H);
        lv_obj_set_size(lv_layer_sys(), SCREEN_W, SCREEN_H);
    }
    build_all();

    s_cur_scr = 0;
    lv_scr_load(s_scr[0]);

    board_unlock();
    return ESP_OK;
}

/* Gira la pantalla y reconstruye la UI. Con el mutex de LVGL tomado. */
static bool apply_orientation(bool land)
{
    lv_obj_t *transition = lv_obj_create(NULL);
    if (transition == NULL) {
        return false;
    }

    if (s_msgbox != NULL) {
        lv_msgbox_close(s_msgbox);
        s_msgbox = NULL;
    }
    lv_obj_t *act = lv_scr_act();
    int which = act == s_scr_blocks ? 3 : act == s_scr_overview ? 4 : s_cur_scr;
    lv_obj_t *old[5] = { s_scr[0], s_scr[1], s_scr[2], s_scr_blocks, s_scr_overview };

    /* Libera las vistas anteriores antes de reservar las de la nueva orientacion. */
    lv_scr_load(transition);
    for (int i = 0; i < s_door_n; i++) {
        lv_anim_del(&s_door[i], door_anim_cb);
    }
    for (int i = 0; i < 5; i++) {
        lv_obj_del(old[i]);
    }

    s_land = land;
    board_set_landscape(land);
    lv_obj_set_size(lv_layer_top(), SCREEN_W, SCREEN_H);
    lv_obj_set_size(lv_layer_sys(), SCREEN_W, SCREEN_H);

    build_all();
    lv_obj_t *nw[5] = { s_scr[0], s_scr[1], s_scr[2], s_scr_blocks, s_scr_overview };
    lv_scr_load(nw[which]);
    lv_obj_del(transition);
    return true;
}

static void rotate_async(void *unused)
{
    (void)unused;
    s_rotate_pending = false;
    app_state_t *app = app_state();
    audio_play(AUDIO_SND_SWIPE_NEXT);
    if (!apply_orientation(!s_land)) {
        return;
    }
    app->cfg.landscape = s_land;
    storage_save_config(&app->cfg);
    ui_refresh();
}

void ui_set_landscape(bool landscape)
{
    if (!board_lock(1000)) {
        return;
    }
    if (landscape != s_land) {
        rotate_async(NULL);
    }
    board_unlock();
}

bool ui_is_landscape(void)
{
    return s_land;
}

static void update_floor_button_states(const elevator_state_t *st)
{
    for (uint8_t f = 0; f < s_int_btn_count; f++) {
        lv_obj_t *btn = s_int_btn[f];
        if (btn == NULL) {
            continue;
        }
        bool here = st->position_known && (f + 1) == st->current_floor;
        unsigned bit = 1u << f;
        bool pending = (st->cabin_calls & bit) != 0;
        lv_obj_set_style_bg_color(btn, lv_color_hex(here || pending ? COL_CALL : COL_BTN), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(pending ? COL_OK : COL_BORDER), 0);
    }
}

void ui_refresh(void)
{
    elevator_state_t st;
    elevator_get_state(&st);

    static uint8_t shown_user = 0;
    static bool first = true;
    if (!first && memcmp(&st, &s_last_state, sizeof(st)) == 0 &&
        shown_user == s_user_floor) {
        return;   /* nada que redibujar: evita trabajo inutil cada 100 ms */
    }

    if (!board_lock(100)) {
        return;
    }
    first = false;
    shown_user = s_user_floor;

    char buf[64];

    /* abre al llegar; se cierra en los ultimos 700 ms, antes de que la cabina salga */
    door_set(st.doors_open && !st.doors_closing);

    char rows[1900] = "C=interior S=subir B=bajar\n\n";
    for (int f = st.floors; f >= 1; f--) {
        unsigned bit = 1u << (f-1);
        char row[112];
        int pin = app_state()->cfg.floor_gpio[f-1];
        snprintf(row, sizeof(row), "P%02d  %s  %s%s%s\nSensor GPIO: %d\n\n", f,
                 st.position_known && st.current_floor == f ? "[CABINA]" : "",
                 st.cabin_calls & bit ? "C " : "", st.up_calls & bit ? "S " : "",
                 st.down_calls & bit ? "B" : "", pin == 0xFF ? -1 : pin);
        strncat(rows, row, sizeof(rows)-strlen(rows)-1);
    }
    set_txt(s_overview_rows, rows);
    /* Exterior */
    snprintf(buf, sizeof(buf), "P%02u", s_user_floor);
    set_txt(s_ext_user_floor, buf);

    if (st.position_known) {
        snprintf(buf, sizeof(buf), "P%02u", st.current_floor);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    set_txt(s_ext_cabin_floor, buf);

    const char *dir_txt = st.fault ? "FALLO DE SENSORES" :
        st.observed_dir == ELEV_DIR_UP ? "\xC3\x9Altimo: SUBIENDO" :
        st.observed_dir == ELEV_DIR_DOWN ? "\xC3\x9Altimo: BAJANDO" :
        "Sentido sin confirmar";
    set_txt(s_ext_dir, dir_txt);
    lv_obj_set_style_text_color(s_ext_dir, lv_color_hex(
        st.observed_dir == ELEV_DIR_IDLE ? EXT_MUTED : COL_OK), 0);
    set_txt(s_ext_arrow, st.observed_dir == ELEV_DIR_UP ? LV_SYMBOL_UP :
                         st.observed_dir == ELEV_DIR_DOWN ? LV_SYMBOL_DOWN : "");
    lv_obj_set_style_text_color(s_ext_arrow, lv_color_hex(COL_OK), 0);

    /* Interior */
    bool moving = st.moving && !st.fault;
    const char *interior_dir = dir_txt;
    const lv_font_t *position_font = &F30;
    if (moving) {
        bool up = st.dir == ELEV_DIR_UP;
        bool down = st.dir == ELEV_DIR_DOWN;
        snprintf(buf, sizeof(buf), "%s", up ? LV_SYMBOL_UP : down ? LV_SYMBOL_DOWN : "EN VIAJE");
        position_font = (up || down) ? &lv_font_montserrat_48 : &F30;
        interior_dir = up ? "SUBIENDO" : down ? "BAJANDO" : "EN MOVIMIENTO";
    } else if (st.position_known && !st.fault) {
        snprintf(buf, sizeof(buf), "P%02u", st.current_floor);
        position_font = &F66;
    } else {
        snprintf(buf, sizeof(buf), "SIN POSICIÓN");
    }
    if (lv_obj_get_style_text_font(s_int_current, LV_PART_MAIN) != position_font) {
        lv_obj_set_style_text_font(s_int_current, position_font, LV_PART_MAIN);
    }
    set_txt(s_int_current, buf);
    set_txt(s_int_dir, interior_dir);
    update_floor_button_states(&st);
    set_txt(s_int_status, st.fault ? "Fallo de sensores" :
            moving ? "Cabina en movimiento." :
            st.position_known ? "Toca un piso para solicitarlo." :
                                "Posici\xC3\xB3n sin confirmar: espera un sensor de piso.");
    s_last_state = st;

    board_unlock();
}

static elevator_dir_t s_travel_dir = ELEV_DIR_IDLE;   /* sentido del ultimo trayecto */

void ui_on_event(const elevator_event_t *ev)
{
    if (ev == NULL) {
        return;
    }
    if (ev->dir != ELEV_DIR_IDLE) {
        s_travel_dir = ev->dir;
    }
    if (ev->message != NULL) {
        if (!board_lock(100)) {
            return;
        }
        lv_label_set_text(s_int_status, ev->message);
        lv_label_set_text(s_ext_hint, ev->message);
        board_unlock();
    }
    switch (ev->type) {
    case ELEV_EVT_MOVING:
        if (ev->message == NULL || strncmp(ev->message, "Llamada", 7) != 0) {
            audio_play(AUDIO_SND_MOVE);
        }
        break;
    case ELEV_EVT_POSITION:
        audio_play_floor(AUDIO_SND_BEEP, ev->floor);
        if (ev->dir != ELEV_DIR_IDLE) {
            audio_play(AUDIO_SND_RUN);   /* zumbido mientras sigue en marcha */
        }
        break;
    case ELEV_EVT_ARRIVED:
        /* un ding al subir, dos al bajar, ding-dong si no se sabe */
        audio_play(s_travel_dir == ELEV_DIR_UP ? AUDIO_SND_ARRIVE_UP
                   : s_travel_dir == ELEV_DIR_DOWN ? AUDIO_SND_ARRIVE_DOWN
                                                   : AUDIO_SND_ARRIVE);
        s_travel_dir = ELEV_DIR_IDLE;
        break;
    case ELEV_EVT_DOORS:
        /* abrir: al llegar; cerrar: suena al empezar a cerrarse (700 ms antes de salir) */
        if (ev->message != NULL && strncmp(ev->message, "Cerrando", 8) == 0) {
            audio_play(AUDIO_SND_DOOR_CLOSE);
        } else if (ev->doors_open) {
            audio_play(AUDIO_SND_DOOR_OPEN);
        }
        break;
    case ELEV_EVT_REJECTED:
        audio_play(AUDIO_SND_ERROR);
        break;
    case ELEV_EVT_LIMIT:
        audio_play(AUDIO_SND_LIMIT);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Ganchos de prueba (los usa la consola UART: debug/cli.c)           */
/*  Ejecutan exactamente los mismos callbacks que un toque real.      */
/* ------------------------------------------------------------------ */

static void send_click(lv_obj_t *o)
{
    lv_event_send(o, LV_EVENT_CLICKED, NULL);
}

bool ui_test_click(const char *name, int arg)
{
    lv_obj_t *o = NULL;
    if (!strcmp(name, "prev")) o = s_b_prev;
    else if (!strcmp(name, "next")) o = s_b_next;
    else if (!strcmp(name, "up")) o = s_b_up;
    else if (!strcmp(name, "down")) o = s_b_down;
    else if (!strcmp(name, "minus")) o = s_b_minus;
    else if (!strcmp(name, "plus")) o = s_b_plus;
    else if (!strcmp(name, "mode")) o = s_b_mode;
    else if (!strcmp(name, "edit")) o = s_b_edit;
    else if (!strcmp(name, "back")) o = s_b_back;
    else if (!strcmp(name, "add")) o = s_b_add;
    else if (!strcmp(name, "floor") && arg >= 1 && arg <= s_int_btn_count)
        o = s_int_btn[arg - 1];
    else if (!strcmp(name, "del") && s_blk_list != NULL &&
             arg >= 0 && (uint32_t)arg < lv_obj_get_child_cnt(s_blk_list)) {
        lv_obj_t *box = lv_obj_get_child(s_blk_list, arg);
        lv_obj_t *head = box ? lv_obj_get_child(box, 0) : NULL;
        o = head ? lv_obj_get_child(head, 1) : NULL;
    }
    if (!strcmp(name, "blksw")) {
        if (!board_lock(500)) {
            return false;
        }
        if (lv_obj_has_state(s_blk_sw, LV_STATE_CHECKED)) {
            lv_obj_clear_state(s_blk_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(s_blk_sw, LV_STATE_CHECKED);
        }
        lv_event_send(s_blk_sw, LV_EVENT_VALUE_CHANGED, NULL);
        board_unlock();
        return true;
    }
    if (o == NULL) {
        printf("ERR sin widget '%s' (%d)\n", name, arg);
        return false;
    }
    if (!board_lock(500)) {
        printf("ERR bloqueo LVGL ocupado >500 ms (%s)\n", name);
        return false;
    }
    send_click(o);
    board_unlock();
    return true;
}

bool ui_test_dd(const char *name, int a, int sel)
{
    lv_obj_t *dd = NULL;
    if (!strcmp(name, "tfloor")) dd = s_tec_dd_floor;
    else if (!strcmp(name, "tgpio")) dd = s_tec_dd_gpio;
    else if (!strcmp(name, "tlevel")) dd = s_tec_dd_level;
    else if (!strcmp(name, "block") && s_blk_list != NULL &&
             a >= 0 && (uint32_t)(a / 10) < lv_obj_get_child_cnt(s_blk_list)) {
        /* a = idx*10 + campo (0..4) */
        lv_obj_t *box = lv_obj_get_child(s_blk_list, a / 10);
        lv_obj_t *row = box ? lv_obj_get_child(box, 1) : NULL;
        int want = a % 10;
        for (uint32_t i = 0; row && i < lv_obj_get_child_cnt(row); i++) {
            lv_obj_t *c = lv_obj_get_child(row, i);
            if (lv_obj_check_type(c, &lv_dropdown_class)) {
                if ((int)(uintptr_t)lv_obj_get_user_data(c) == want) { dd = c; break; }
            }
        }
    }
    if (dd == NULL || !board_lock(500)) {
        return false;
    }
    lv_dropdown_set_selected(dd, sel);
    lv_event_send(dd, LV_EVENT_VALUE_CHANGED, NULL);
    board_unlock();
    return true;
}

/* Cambio de pantalla CON animacion (igual que un gesto): dir>0 siguiente */
void ui_test_swipe(int dir)
{
    if (!board_lock(500)) {
        return;
    }
    goto_screen(s_cur_scr + (dir > 0 ? 1 : -1), dir > 0);
    board_unlock();
}

void ui_test_goto(int idx)
{
    if (!board_lock(500)) {
        return;
    }
    if (idx >= 0 && idx <= 2) {
        s_cur_scr = idx;
        lv_scr_load(s_scr[idx]);
    } else if (idx == 3) {
        lv_scr_load(s_scr_blocks);
    } else if (idx == 4) {
        lv_scr_load(s_scr_overview);
    }
    board_unlock();
}

void ui_test_dump(void)
{
    if (!board_lock(500)) {
        return;
    }
    printf("UI scr=%d user=%s cabin=%s dir='%s' ext_hint='%s'\n", (lv_scr_act() == s_scr_overview ? 4 : lv_scr_act() == s_scr_blocks ? 3 : s_cur_scr),
           lv_label_get_text(s_ext_user_floor), lv_label_get_text(s_ext_cabin_floor),
           lv_label_get_text(s_ext_dir), lv_label_get_text(s_ext_hint));
    printf("UI int=%s status='%s' btns=%u\n", lv_label_get_text(s_int_current),
           lv_label_get_text(s_int_status), s_int_btn_count);
    for (uint8_t f = 0; f < s_int_btn_count; f++) {
        printf("UI floor=%s\n", lv_label_get_text(lv_obj_get_child(s_int_btn[f], 0)));
    }
    printf("UI tec count=%s mode='%s' selfloor=%u gpio_sel=%u level_sel=%u read='%s' then='%s' note='%s'\n",
           lv_label_get_text(s_tec_count), lv_label_get_text(s_tec_mode_lbl),
           s_sel_floor, (unsigned)lv_dropdown_get_selected(s_tec_dd_gpio),
           (unsigned)lv_dropdown_get_selected(s_tec_dd_level),
           lv_label_get_text(s_tec_read), lv_label_get_text(s_tec_then),
           lv_label_get_text(s_tec_note));
    printf("UI blocks widgets=%u status='%s' sw=%d\n",
           (unsigned)lv_obj_get_child_cnt(s_blk_list),
           lv_label_get_text(s_blk_status), lv_obj_has_state(s_blk_sw, LV_STATE_CHECKED));
    board_unlock();
}
