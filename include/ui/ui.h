/**
 * ui.h - Dual-screen UI system
 *
 * Top screen (400x240): now-playing info, album art, server branding
 * Bottom screen (320x240): touch-driven list navigation, controls
 *
 * Uses citro2d for GPU-accelerated 2D rendering.
 */

#ifndef JFIN_UI_H
#define JFIN_UI_H

#include <stdbool.h>
#include <3ds.h>
#include "api/jellyfin.h"
#include "audio/player.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOP_SCREEN_WIDTH    400
#define TOP_SCREEN_HEIGHT   240
#define BOTTOM_SCREEN_WIDTH 320
#define BOTTOM_SCREEN_HEIGHT 240

#define UI_LIST_ITEM_HEIGHT  40
#define UI_MAX_VISIBLE_ITEMS  5  /* (240 - 30 header - 20 footer) / 40 ≈ 5 */
#define UI_FONT_SIZE         14
#define UI_FONT_SIZE_SMALL   11

/* ── Colors (RGBA8) ──────────────────────────────────────────────── */
#define COLOR_BG_DARK        0x1E1E2EFF   /* warmer dark background */
#define COLOR_BG_CARD        0x2A2A3CFF   /* more contrast with bg */
#define COLOR_PRIMARY        0x6C9BD2FF   /* muted steel blue */
#define COLOR_TEXT_PRIMARY   0xE8E8E8FF   /* slightly brighter white */
#define COLOR_TEXT_SECONDARY 0x9898A8FF   /* warmer mid-gray */
#define COLOR_ACCENT         0xC084FCFF   /* soft lavender */
#define COLOR_HIGHLIGHT      0x6C9BD230   /* matches primary, 19% alpha */

/* Settings-specific colors */
#define COLOR_SEPARATOR      0x3A3A4CFF   /* subtle divider lines */
#define COLOR_VALUE          0xA8D8A8FF   /* muted green for values */
#define COLOR_DANGER         0xE88888FF   /* soft red for logout */

/* ── Screens / Views ─────────────────────────────────────────────── */

typedef enum {
    VIEW_LOGIN,          /* server URL + credentials input */
    VIEW_LIBRARIES,      /* top-level library list */
    VIEW_BROWSE,         /* browsing items within a library */
    VIEW_NOW_PLAYING,    /* audio playback screen */
    VIEW_SETTINGS,       /* settings / account / about */
} ui_view_t;

/* ── UI State ────────────────────────────────────────────────────── */

typedef struct {
    /* Navigation */
    ui_view_t    current_view;
    ui_view_t    previous_view;

    /* List state */
    jfin_item_list_t items;
    int          selected_index;   /* cursor position in list */
    int          scroll_offset;    /* first visible item index */

    /* Breadcrumb for back navigation */
    char         parent_stack_ids[8][JFIN_MAX_ID];
    char         parent_stack_names[8][JFIN_MAX_NAME];
    int          parent_depth;

    /* Now playing */
    jfin_item_t  now_playing;
    bool         has_now_playing;
    int          playing_index;   /* index of currently playing item in items list */
    bool         auto_advance;    /* auto-play next track/episode when current finishes */
    bool         auto_stopped;    /* true when user manually stopped (X), false on natural end */
    bool         bottom_hidden;   /* hide bottom screen (night mode) */

    /* Login form */
    char         server_url[JFIN_MAX_URL];
    char         username[64];
    char         password[64];
    int          login_field;  /* 0=url, 1=user, 2=pass */

    /* Touch state */
    bool         touch_held;
    int          touch_start_y;
    int          scroll_velocity;

    /* Settings */
    int          settings_index;   /* cursor position in settings list */
    int          settings_scroll;  /* scroll offset */
} ui_state_t;

/* ── Lifecycle ───────────────────────────────────────────────────── */

/**
 * Initialize the UI subsystem. Call after gfxInitDefault() and C2D_Init().
 */
bool ui_init(void);

/**
 * Shut down the UI subsystem.
 */
void ui_cleanup(void);

/* ── Frame Loop ──────────────────────────────────────────────────── */

/**
 * Process input (buttons + touch) and update UI state.
 */
void ui_update(ui_state_t *state, const jfin_session_t *session,
               u32 kdown, u32 kheld, touchPosition touch);

/**
 * Render both screens for the current frame.
 */
void ui_render(const ui_state_t *state, const jfin_session_t *session,
               const player_status_t *player);

/* ── View-Specific Renderers ─────────────────────────────────────── */

void ui_render_login(const ui_state_t *state);
void ui_render_libraries(const ui_state_t *state);
void ui_render_browse(const ui_state_t *state);
void ui_render_now_playing(const ui_state_t *state, const player_status_t *player);
void ui_render_settings(const ui_state_t *state, const jfin_session_t *session);

/* ── Helpers ─────────────────────────────────────────────────────── */

/**
 * Navigate into an item (push to breadcrumb stack, load children).
 */
void ui_navigate_into(ui_state_t *state, const jfin_session_t *session,
                      const jfin_item_t *item);

/**
 * Go back one level in the breadcrumb stack.
 */
void ui_navigate_back(ui_state_t *state, const jfin_session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_UI_H */
