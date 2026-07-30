/**
 * ui.c - Dual-screen UI implementation
 *
 * Uses citro2d for GPU-accelerated 2D rendering.
 * Top screen: now-playing / branding
 * Bottom screen: touch-driven list navigation
 *
 * MVP: text-based rendering. Album art loading deferred to phase 2.
 */

#include <stdio.h>
#include <string.h>
#include <3ds.h>
#include <citro2d.h>

#include "ui/ui.h"
#include "api/jellyfin.h"
#include "audio/player.h"
#include "video/video_player.h"
#include "ui/album_art.h"
#include "util/cache.h"
#include "util/config.h"
#include "util/log.h"

#include <curl/curl.h>

extern jfin_config_t g_config;

/* ── Render targets ────────────────────────────────────────────────── */

static C3D_RenderTarget *s_top       = NULL;
static C3D_RenderTarget *s_top_right = NULL;  /* right eye for stereoscopic 3D */
static C3D_RenderTarget *s_bottom    = NULL;
static C2D_TextBuf       s_text_buf = NULL;
static C2D_Font          s_font = NULL;

/* ── Helpers ───────────────────────────────────────────────────────── */

static u32 rgba(u32 hex)
{
    /* Convert 0xRRGGBBAA to citro2d's ABGR format */
    u8 r = (hex >> 24) & 0xFF;
    u8 g = (hex >> 16) & 0xFF;
    u8 b = (hex >> 8)  & 0xFF;
    u8 a = hex & 0xFF;
    return C2D_Color32(r, g, b, a);
}

static void draw_text(float x, float y, float size, u32 color, const char *text)
{
    C2D_Text c2d_text;
    C2D_TextParse(&c2d_text, s_text_buf, text);
    C2D_TextOptimize(&c2d_text);
    C2D_DrawText(&c2d_text, C2D_WithColor, x, y, 0.5f, size, size, color);
}

static void draw_rect(float x, float y, float w, float h, u32 color)
{
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

static void format_ticks(int64_t ticks, char *out, int out_len)
{
    int total_sec = (int)(ticks / 10000000LL);
    int min = total_sec / 60;
    int sec = total_sec % 60;
    snprintf(out, out_len, "%d:%02d", min, sec);
}

/* Map Jellyfin's 3D metadata onto the video player's render mode.
 * TAB formats aren't renderable in 3D yet — they play flat (2D). */
static vp_3d_mode_t item_3d_mode(const jfin_item_t *item)
{
    switch (item->video_3d_format) {
    case JFIN_3D_HSBS: return VP_3D_HSBS;
    case JFIN_3D_FSBS: return VP_3D_FSBS;
    default:           return VP_3D_NONE;
    }
}

/* ── Playback / offline-cache helpers ──────────────────────────────── */

static bool item_is_video(const jfin_item_t *item)
{
    return item->type == JFIN_ITEM_MOVIE || item->type == JFIN_ITEM_EPISODE;
}

static bool item_is_playable(const jfin_item_t *item)
{
    return item->type == JFIN_ITEM_AUDIO || item_is_video(item);
}

static const char *item_cache_ext(const jfin_item_t *item)
{
    return item_is_video(item) ? "ts" : "mp3";
}

/* Cache size shown in settings — refreshed on entry/clear, never per-frame
 * (cache_total_bytes walks the SD directory) */
static u64 s_cache_bytes_ui;

/* Re-select sticky subtitle language on episode change; clears if no match. */
static void apply_sticky_subtitle(ui_state_t *state, const jfin_session_t *session,
                                  const char *item_id)
{
    state->subtitle_stream_index = -1;
    state->subtitle_list_loaded = false;
    if (!state->subtitle_sticky || state->subtitle_lang_pref[0] == '\0') return;
    if (!jfin_get_subtitle_streams(session, item_id, &state->subtitle_list)) return;
    state->subtitle_list_loaded = true;
    for (int i = 0; i < state->subtitle_list.count; i++) {
        if (strcmp(state->subtitle_list.subs[i].language,
                   state->subtitle_lang_pref) == 0) {
            state->subtitle_stream_index = state->subtitle_list.subs[i].index;
            return;
        }
    }
}

/**
 * Start playback of a playable item, preferring the offline cache.
 * Covers: video with audio-stream fallback, audio/Old-3DS path, playback
 * reporting, album art, and now-playing state. View transitions stay at
 * the call sites (browse switches to now-playing; auto-advance doesn't).
 */
static bool ui_start_item(ui_state_t *state, const jfin_session_t *session,
                          const jfin_item_t *item, int item_index,
                          int64_t start_ticks)
{
    jfin_stream_t stream;
    char cpath[512];
    bool started = false;

    if (item_is_video(item) && video_player_is_supported()) {
        vp_3d_mode_t mode_3d = item_3d_mode(item);
        audio_player_stop();
        apply_sticky_subtitle(state, session, item->id);
        if (cache_has(item->id, "ts") &&
            cache_path(item->id, "ts", cpath, sizeof(cpath))) {
            started = video_player_play(cpath, NULL, item->runtime_ticks,
                                        start_ticks, mode_3d);
        } else if (jfin_get_video_stream(session, item->id, start_ticks,
                                         mode_3d != VP_3D_NONE,
                                         state->subtitle_stream_index, &stream)) {
            started = video_player_play(stream.url, stream.subtitle_url,
                                        item->runtime_ticks, start_ticks, mode_3d);
        }
    }

    if (!started) {
        /* Audio track, Old 3DS, or the video path failed.
         * Cached audio can't seek locally (mpg123 feed API), so a
         * non-zero start position falls back to streaming. */
        video_player_stop();
        if (start_ticks == 0 && cache_has(item->id, "mp3") &&
            cache_path(item->id, "mp3", cpath, sizeof(cpath))) {
            started = audio_player_play(cpath, item->runtime_ticks, 0);
        } else if (jfin_get_audio_stream(session, item->id, start_ticks, &stream)) {
            started = audio_player_play(stream.url, item->runtime_ticks,
                                        start_ticks);
        }
    }

    if (started) {
        state->now_playing = *item;
        state->has_now_playing = true;
        state->playing_index = item_index;
        state->auto_stopped = false;
        jfin_report_start(session, item->id);
        album_art_load(session, item);
    }
    return started;
}

/* ── Modal download (blocking, B cancels) ──────────────────────────── */

typedef struct {
    FILE              *fp;
    const jfin_item_t *item;
    int64_t            est_total;  /* runtime x bitrate estimate (bytes) */
    u64                last_render_ms;
    bool               cancelled;
} dl_ctx_t;

static size_t dl_file_write_cb(void *ptr, size_t size, size_t nmemb, void *ud)
{
    dl_ctx_t *dl = (dl_ctx_t *)ud;
    return fwrite(ptr, size, nmemb, dl->fp);
}

static void dl_render_progress(dl_ctx_t *dl, s64 dlnow)
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(s_text_buf);
    C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_bottom);
    draw_text(10, 10, 0.6f, rgba(COLOR_PRIMARY), "Downloading");
    draw_text(10, 40, 0.5f, rgba(COLOR_TEXT_PRIMARY), dl->item->name);

    char line[96];
    double mb = (double)dlnow / (1024.0 * 1024.0);
    if (dl->est_total > 0) {
        int pct = (int)((double)dlnow * 100.0 / (double)dl->est_total);
        if (pct > 99) pct = 99; /* size is an estimate — never claim done */
        draw_rect(20, 80, 280, 8, rgba(COLOR_BG_CARD));
        draw_rect(20, 80, 280.0f * pct / 100.0f, 8, rgba(COLOR_PRIMARY));
        snprintf(line, sizeof(line), "%.1f MB  (~%d%%)", mb, pct);
    } else {
        snprintf(line, sizeof(line), "%.1f MB", mb);
    }
    draw_text(20, 100, 0.5f, rgba(COLOR_VALUE), line);
    draw_text(10, 140, 0.4f, rgba(COLOR_TEXT_SECONDARY),
              "Keep the lid open — closing it drops WiFi.");
    draw_text(10, 210, 0.45f, rgba(COLOR_TEXT_SECONDARY), "B: cancel");
    C3D_FrameEnd(0);
}

static int dl_progress_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                          curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)ultotal; (void)ulnow;
    dl_ctx_t *dl = (dl_ctx_t *)ud;

    hidScanInput();
    if (hidKeysDown() & KEY_B) {
        dl->cancelled = true;
        return 1; /* abort the transfer */
    }

    u64 now = osGetTime();
    if (now - dl->last_render_ms >= 250) {
        dl->last_render_ms = now;
        dl_render_progress(dl, (s64)dlnow);
    }
    return 0;
}

static void ui_download_item(const jfin_session_t *session,
                             const jfin_item_t *item)
{
    const char *ext = item_cache_ext(item);

    /* If the index is full, a completed download would be renamed on disk
     * but never registered, so cache_has() stays false and the next X-press
     * would delete the good file. Refuse up front with a visible message. */
    if (cache_is_full()) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(s_text_buf);
        C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
        C2D_SceneBegin(s_bottom);
        draw_text(10, 90, 0.5f, rgba(COLOR_TEXT_PRIMARY),
                  "Offline cache full.");
        draw_text(10, 120, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  "Clear some downloads in Settings.");
        C3D_FrameEnd(0);
        svcSleepThread(1500000000LL); /* 1.5s so the message is readable */
        return;
    }

    jfin_stream_t stream;
    bool ok_url;
    if (item_is_video(item)) {
        vp_3d_mode_t mode_3d = item_3d_mode(item);
        ok_url = jfin_get_video_stream(session, item->id, 0,
                                       mode_3d != VP_3D_NONE, -1, &stream);
    } else {
        ok_url = jfin_get_audio_stream(session, item->id, 0, &stream);
    }
    if (!ok_url) return;

    char part[512], final_path[512];
    if (!cache_part_path(item->id, ext, part, sizeof(part)) ||
        !cache_path(item->id, ext, final_path, sizeof(final_path)))
        return;

    dl_ctx_t dl = {0};
    dl.item = item;
    dl.fp = fopen(part, "wb");
    if (!dl.fp) {
        log_write("DL: fopen failed: %s", part);
        return;
    }

    /* Transcoded streams carry no Content-Length; estimate from
     * runtime x configured bitrate for the progress bar */
    int kbps = item_is_video(item)
        ? g_config.video_bitrate + g_config.audio_bitrate
        : g_config.audio_bitrate;
    if (item->runtime_ticks > 0 && kbps > 0)
        dl.est_total = item->runtime_ticks / 10000000LL * kbps * 1000 / 8;

    dl_render_progress(&dl, 0);

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(dl.fp);
        remove(part);
        return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, stream.url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dl_file_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dl);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Jellyfin-3DS/" JFIN_VERSION);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    /* Server-side transcode throttling can slow the stream to ~realtime;
     * only abort if it stalls outright for a minute */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, dl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &dl);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(dl.fp);

    if (res == CURLE_OK && http_code >= 200 && http_code < 300) {
        remove(final_path); /* replace an older copy if re-downloading */
        if (rename(part, final_path) == 0) {
            cache_index_add(item->id, ext);
            log_write("DL: cached %s.%s", item->id, ext);
        } else {
            log_write("DL: rename failed for %s", part);
            remove(part);
        }
    } else {
        remove(part);
        log_write("DL: %s res=%d http=%ld",
                  dl.cancelled ? "cancelled" : "failed", res, http_code);
    }
}

/* ── Selection style helper ────────────────────────────────────────── */

static void draw_list_item_bg(float y, float w, float h, bool selected)
{
    u32 bg = selected ? rgba(COLOR_HIGHLIGHT) : rgba(COLOR_BG_CARD);
    draw_rect(5, y, w, h, bg);
    if (selected)
        draw_rect(5, y, 3, h, rgba(COLOR_PRIMARY));  /* left accent bar */
}

/* ── Settings items ───────────────────────────────────────────────── */

enum {
    SET_AUDIO_BITRATE,
    SET_VIDEO_BITRATE,
    SET_AUTO_ADVANCE,
    SET_CACHE_CLEAR,         /* action: clear offline cache */
    SET_SEPARATOR_ACCOUNT,   /* non-selectable divider */
    SET_SERVER,              /* display-only */
    SET_USERNAME,            /* display-only */
    SET_LOGOUT,              /* action */
    SET_SEPARATOR_ABOUT,     /* non-selectable divider */
    SET_VERSION,             /* display-only */
    SET_DEVICE_ID,           /* display-only */
    SET_COUNT
};

static const int audio_rates[] = {64, 128, 192, 256};
static const int video_rates[] = {256, 472, 768};
#define AUDIO_RATES_COUNT (int)(sizeof(audio_rates) / sizeof(audio_rates[0]))
#define VIDEO_RATES_COUNT (int)(sizeof(video_rates) / sizeof(video_rates[0]))

static bool settings_is_separator(int idx)
{
    return idx == SET_SEPARATOR_ACCOUNT || idx == SET_SEPARATOR_ABOUT;
}

static int settings_next_selectable(int from, int dir)
{
    int n = from + dir;
    while (n >= 0 && n < SET_COUNT && settings_is_separator(n))
        n += dir;
    if (n < 0 || n >= SET_COUNT) return from;
    return n;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

bool ui_init(void)
{
    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_top_right = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    s_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    if (!s_top || !s_top_right || !s_bottom) return false;

    s_text_buf = C2D_TextBufNew(4096);
    if (!s_text_buf) return false;

    /* Use system font */
    s_font = NULL; /* NULL = default system font */

    return true;
}

void ui_cleanup(void)
{
    if (s_text_buf) {
        C2D_TextBufDelete(s_text_buf);
        s_text_buf = NULL;
    }
}

/* ── Input Handling ────────────────────────────────────────────────── */

void ui_update(ui_state_t *state, const jfin_session_t *session,
               u32 kdown, u32 kheld, touchPosition touch)
{
    (void)kheld;

    switch (state->current_view) {
    case VIEW_LOGIN:
        /* D-pad up/down to select field */
        if (kdown & KEY_DUP) {
            state->login_field = (state->login_field + 2) % 3;
        }
        if (kdown & KEY_DDOWN) {
            state->login_field = (state->login_field + 1) % 3;
        }
        /* A to activate swkbd for the selected field */
        if (kdown & KEY_A) {
            SwkbdState swkbd;
            char buf[JFIN_MAX_URL] = {0};

            SwkbdType type = (state->login_field == 2)
                ? SWKBD_TYPE_WESTERN : SWKBD_TYPE_WESTERN;
            swkbdInit(&swkbd, type, 2, 255);

            switch (state->login_field) {
            case 0:
                swkbdSetHintText(&swkbd, "Server URL (e.g. http://192.168.1.100:8096)");
                snprintf(buf, sizeof(buf), "%s", state->server_url);
                break;
            case 1:
                swkbdSetHintText(&swkbd, "Username");
                snprintf(buf, sizeof(buf), "%s", state->username);
                break;
            case 2:
                swkbdSetHintText(&swkbd, "Password");
                swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
                break;
            }

            swkbdSetInitialText(&swkbd, buf);
            SwkbdButton button = swkbdInputText(&swkbd, buf, sizeof(buf));

            if (button == SWKBD_BUTTON_CONFIRM) {
                switch (state->login_field) {
                case 0: snprintf(state->server_url, sizeof(state->server_url), "%s", buf); break;
                case 1: snprintf(state->username, sizeof(state->username), "%s", buf); break;
                case 2: snprintf(state->password, sizeof(state->password), "%s", buf); break;
                }
            }
        }
        /* START to attempt login */
        if (kdown & KEY_R) {
            /* Show connecting indicator before blocking login call */
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TextBufClear(s_text_buf);
            C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
            C2D_SceneBegin(s_bottom);
            draw_text(100, 110, 0.6f, rgba(COLOR_PRIMARY), "Connecting...");
            C3D_FrameEnd(0);

            jfin_session_t *s = (jfin_session_t *)session; /* cast away const for login */
            if (jfin_login(s, state->server_url, state->username, state->password,
                           g_config.device_id)) {
                /* Save credentials immediately so they persist even on crash */
                config_save_session(&g_config, s->server_url,
                                    s->access_token, s->user_id,
                                    state->username);
                /* Token obtained — no reason to keep the password in RAM */
                memset(state->password, 0, sizeof(state->password));
                state->current_view = VIEW_LIBRARIES;
                jfin_get_views(session, &state->items);
                state->selected_index = 0;
                state->scroll_offset = 0;
            }
        }
        break;

    case VIEW_LIBRARIES:
    case VIEW_BROWSE:
        /* D-pad navigation with hold-to-repeat: ~270ms initial delay,
         * then one step every other frame (~15 items/s at 30fps).
         * Statics instead of ui_state_t fields: keeps this out of the
         * header while the wip UI branch is active; ui_update is
         * main-thread only so there is no reentrancy concern. */
        {
            static int dpad_repeat_frames = 0;
            static int dpad_last_view = -1;

            /* Entering this view with the D-pad already held must not
             * inherit a matured timer from the previous view */
            if (dpad_last_view != (int)state->current_view) {
                dpad_repeat_frames = 0;
                dpad_last_view = (int)state->current_view;
            }

            bool nav_up = (kdown & KEY_DUP) != 0;
            bool nav_down = (kdown & KEY_DDOWN) != 0;

            if (nav_up || nav_down) {
                dpad_repeat_frames = 0; /* fresh press resets the timer */
            } else if (kheld & (KEY_DUP | KEY_DDOWN)) {
                dpad_repeat_frames++;
                if (dpad_repeat_frames >= 8 && (dpad_repeat_frames % 2) == 0) {
                    nav_up = (kheld & KEY_DUP) != 0;
                    nav_down = (kheld & KEY_DDOWN) != 0;
                }
            } else {
                dpad_repeat_frames = 0;
            }

            if (nav_up) {
                if (state->selected_index > 0) {
                    state->selected_index--;
                    if (state->selected_index < state->scroll_offset)
                        state->scroll_offset = state->selected_index;
                }
            }
            if (nav_down) {
                if (state->selected_index < state->items.count - 1) {
                    state->selected_index++;
                    if (state->selected_index >= state->scroll_offset + UI_MAX_VISIBLE_ITEMS)
                        state->scroll_offset = state->selected_index - UI_MAX_VISIBLE_ITEMS + 1;
                }
            }
        }
        /* Touch: drag to scroll, tap to select+activate */
        {
            /* List items start at y=25 (browse) or y=30 (libraries) on screen */
            int list_top = (state->current_view == VIEW_LIBRARIES) ? 30 : 25;

            if (kdown & KEY_TOUCH) {
                state->touch_held = true;
                state->touch_start_y = touch.py;
                state->scroll_velocity = 0;
            }
            if ((kheld & KEY_TOUCH) && state->touch_held) {
                int dy = state->touch_start_y - touch.py;
                if (dy > UI_LIST_ITEM_HEIGHT / 2 || dy < -UI_LIST_ITEM_HEIGHT / 2) {
                    /* Dragging — scroll the list */
                    int scroll_items = dy / UI_LIST_ITEM_HEIGHT;
                    if (scroll_items != 0) {
                        state->scroll_offset += scroll_items;
                        state->touch_start_y = touch.py;
                        state->scroll_velocity = scroll_items;
                        /* Clamp */
                        int max_scroll = state->items.count - UI_MAX_VISIBLE_ITEMS;
                        if (max_scroll < 0) max_scroll = 0;
                        if (state->scroll_offset > max_scroll) state->scroll_offset = max_scroll;
                        if (state->scroll_offset < 0) state->scroll_offset = 0;
                        if (state->selected_index < state->scroll_offset)
                            state->selected_index = state->scroll_offset;
                        if (state->selected_index >= state->scroll_offset + UI_MAX_VISIBLE_ITEMS)
                            state->selected_index = state->scroll_offset + UI_MAX_VISIBLE_ITEMS - 1;
                    }
                }
            }
            if (!(kheld & KEY_TOUCH) && state->touch_held) {
                state->touch_held = false;
                if (state->scroll_velocity == 0) {
                    /* Tap — select AND activate (same as D-pad + A) */
                    int tapped = state->scroll_offset + ((state->touch_start_y - list_top) / UI_LIST_ITEM_HEIGHT);
                    if (tapped >= 0 && tapped < state->items.count) {
                        state->selected_index = tapped;
                        /* Simulate A press by setting kdown */
                        kdown |= KEY_A;
                    }
                }
            }
            /* Momentum scrolling */
            if (!state->touch_held && state->scroll_velocity != 0) {
                state->scroll_offset += state->scroll_velocity;
                if (state->scroll_velocity > 0) state->scroll_velocity--;
                else state->scroll_velocity++;
                int max_scroll = state->items.count - UI_MAX_VISIBLE_ITEMS;
                if (max_scroll < 0) max_scroll = 0;
                if (state->scroll_offset > max_scroll) state->scroll_offset = max_scroll;
                if (state->scroll_offset < 0) { state->scroll_offset = 0; state->scroll_velocity = 0; }
            }
        }
        /* A to enter / play */
        if (kdown & KEY_A) {
            if (state->selected_index < state->items.count) {
                jfin_item_t *item = &state->items.items[state->selected_index];
                bool is_playable = (item->type == JFIN_ITEM_AUDIO ||
                                    item->type == JFIN_ITEM_MOVIE ||
                                    item->type == JFIN_ITEM_EPISODE);
                bool is_container = (item->type == JFIN_ITEM_FOLDER ||
                                     item->type == JFIN_ITEM_MUSIC_ALBUM ||
                                     item->type == JFIN_ITEM_MUSIC_ARTIST ||
                                     item->type == JFIN_ITEM_SERIES ||
                                     item->type == JFIN_ITEM_SEASON);
                if (is_playable) {
                    if (ui_start_item(state, session, item,
                                      state->selected_index, 0)) {
                        state->previous_view = state->current_view;
                        state->current_view = VIEW_NOW_PLAYING;
                    }
                } else if (is_container) {
                    ui_navigate_into(state, session, item);
                }
                /* JFIN_ITEM_UNKNOWN: do nothing */
            }
        }
        /* B to go back */
        if (kdown & KEY_B) {
            ui_navigate_back(state, session);
        }
        /* X: download to / remove from the offline cache */
        if ((kdown & KEY_X) && state->selected_index < state->items.count) {
            jfin_item_t *item = &state->items.items[state->selected_index];
            /* Video on Old 3DS can't be played, so don't cache it either */
            bool cacheable = item_is_playable(item) &&
                             (!item_is_video(item) || video_player_is_supported());
            if (cacheable) {
                const char *ext = item_cache_ext(item);
                if (cache_has(item->id, ext))
                    cache_remove(item->id, ext);
                else
                    ui_download_item(session, item);
            }
        }
        /* L/R for pagination */
        if (kdown & KEY_R) {
            /* Next page */
            if (state->items.start_index + state->items.count < state->items.total_count) {
                int next_start = state->items.start_index + JFIN_MAX_ITEMS;
                const char *parent_id = (state->parent_depth > 0)
                    ? state->parent_stack_ids[state->parent_depth - 1] : NULL;
                if (state->current_view == VIEW_LIBRARIES) {
                    /* Libraries don't paginate the same way */
                } else if (parent_id) {
                    jfin_get_items(session, parent_id, next_start, JFIN_MAX_ITEMS, &state->items);
                    state->selected_index = 0;
                    state->scroll_offset = 0;
                }
            }
        }
        if (kdown & KEY_L) {
            /* Previous page */
            if (state->items.start_index > 0) {
                int prev_start = state->items.start_index - JFIN_MAX_ITEMS;
                if (prev_start < 0) prev_start = 0;
                const char *parent_id = (state->parent_depth > 0)
                    ? state->parent_stack_ids[state->parent_depth - 1] : NULL;
                if (parent_id) {
                    jfin_get_items(session, parent_id, prev_start, JFIN_MAX_ITEMS, &state->items);
                    state->selected_index = 0;
                    state->scroll_offset = 0;
                }
            }
        }
        /* Y to toggle now-playing view */
        if ((kdown & KEY_Y) && state->has_now_playing) {
            state->previous_view = state->current_view;
            state->current_view = VIEW_NOW_PLAYING;
        }
        /* SELECT: settings in libraries, search in browse */
        if (kdown & KEY_SELECT) {
            if (state->current_view == VIEW_LIBRARIES) {
                state->settings_index = 0;
                state->settings_scroll = 0;
                s_cache_bytes_ui = cache_total_bytes(); /* one dir walk on entry */
                state->current_view = VIEW_SETTINGS;
            } else {
                SwkbdState swkbd;
                char query[128] = {0};
                swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, 127);
                swkbdSetHintText(&swkbd, "Search...");
                SwkbdButton button = swkbdInputText(&swkbd, query, sizeof(query));
                if (button == SWKBD_BUTTON_CONFIRM && query[0] != '\0') {
                    jfin_search(session, query, JFIN_MAX_ITEMS, &state->items);
                    state->selected_index = 0;
                    state->scroll_offset = 0;
                    state->current_view = VIEW_BROWSE;
                }
            }
        }
        /* Auto-advance: when current track/episode finishes, play next */
        if (state->has_now_playing && state->auto_advance && !state->auto_stopped) {
            player_status_t ps = audio_player_get_status();
            video_status_t vs = video_player_get_status();
            bool audio_finished = (ps.state == PLAYER_STOPPED && vs.state == VIDEO_STOPPED);
            bool video_finished = (vs.state == VIDEO_STOPPED && ps.state == PLAYER_STOPPED);

            if (audio_finished || video_finished) {
                int next = state->playing_index + 1;
                if (next < state->items.count &&
                    item_is_playable(&state->items.items[next])) {
                    if (!ui_start_item(state, session,
                                       &state->items.items[next], next, 0))
                        state->has_now_playing = false;
                } else {
                    state->has_now_playing = false;
                }
            }
        }
        break;

    case VIEW_NOW_PLAYING:
        {
            video_status_t vs = video_player_get_status();
            bool vid_active = (vs.state == VIDEO_PLAYING || vs.state == VIDEO_PAUSED ||
                               vs.state == VIDEO_LOADING);

            /* Error state: any button press stops and returns to browse */
            if (vs.state == VIDEO_ERROR) {
                if (kdown) {
                    video_player_stop();
                    audio_player_stop();
                    state->has_now_playing = false;
                    state->current_view = state->previous_view;
                }
                break;
            }

            /* Watch mode: D-pad down hides controls, only up exits */
            if (state->bottom_hidden) {
                if (kdown & KEY_DUP)
                    state->bottom_hidden = false;
                break; /* ignore all other buttons in watch mode */
            }
            /* D-pad down: enter watch mode */
            if (kdown & KEY_DDOWN) {
                state->bottom_hidden = true;
                break;
            }
            /* A to pause/resume */
            if (kdown & KEY_A) {
                if (vid_active)
                    video_player_pause();
                else
                    audio_player_pause();
            }
            /* L/R to seek ±30 seconds */
            if ((kdown & KEY_L) || (kdown & KEY_R)) {
                int64_t offset = (kdown & KEY_R) ? 300000000LL : -300000000LL; /* ±30s */
                int64_t cur_pos;
                if (vid_active)
                    cur_pos = vs.position_ticks;
                else
                    cur_pos = audio_player_get_status().position_ticks;

                int64_t new_pos = cur_pos + offset;
                if (new_pos < 0) new_pos = 0;
                if (new_pos >= state->now_playing.runtime_ticks)
                    new_pos = state->now_playing.runtime_ticks - 100000000LL; /* 10s before end */

                /* ui_start_item picks cache vs stream; cached video seeks
                 * in-place on the SD file (no transcode restart) */
                if (vid_active)
                    video_player_stop();
                else
                    audio_player_stop();
                ui_start_item(state, session, &state->now_playing,
                              state->playing_index, new_pos);
            }
            /* B to go back to browse */
            if (kdown & KEY_B) {
                state->bottom_hidden = false;
                state->current_view = state->previous_view;
            }
            /* X to stop */
            if (kdown & KEY_X) {
                state->bottom_hidden = false;
                video_player_stop();
                audio_player_stop();
                state->has_now_playing = false;
                state->auto_stopped = true;
                state->current_view = state->previous_view;
            }
            /* Y (video): cycle subtitle tracks (Off → track[0] → … → Off) */
            if ((kdown & KEY_Y) && vid_active) {
                if (!state->subtitle_list_loaded) {
                    if (jfin_get_subtitle_streams(session, state->now_playing.id,
                                                  &state->subtitle_list))
                        state->subtitle_list_loaded = true;
                }
                int next_index = -1;
                if (state->subtitle_stream_index < 0) {
                    if (state->subtitle_list_loaded && state->subtitle_list.count > 0)
                        next_index = state->subtitle_list.subs[0].index;
                } else {
                    int cur_pos = -1;
                    for (int si = 0; si < state->subtitle_list.count; si++) {
                        if (state->subtitle_list.subs[si].index == state->subtitle_stream_index) {
                            cur_pos = si; break;
                        }
                    }
                    if (cur_pos < 0 || cur_pos >= state->subtitle_list.count - 1)
                        next_index = -1;
                    else
                        next_index = state->subtitle_list.subs[cur_pos + 1].index;
                }
                state->subtitle_stream_index = next_index;
                if (next_index >= 0) {
                    state->subtitle_sticky = true;
                    for (int si = 0; si < state->subtitle_list.count; si++) {
                        if (state->subtitle_list.subs[si].index == next_index) {
                            snprintf(state->subtitle_lang_pref,
                                     sizeof(state->subtitle_lang_pref),
                                     "%s", state->subtitle_list.subs[si].language);
                            break;
                        }
                    }
                } else {
                    state->subtitle_sticky = false;
                    state->subtitle_lang_pref[0] = '\0';
                }
                int64_t resume_ticks = vs.position_ticks;
                vp_3d_mode_t sub_mode = item_3d_mode(&state->now_playing);
                video_player_stop();
                state->video_retry_count = 0;
                state->video_retry_timer = 0;
                state->video_retry_ticks = resume_ticks;
                jfin_stream_t sub_stream;
                if (jfin_get_video_stream(session, state->now_playing.id, resume_ticks,
                                          sub_mode != VP_3D_NONE,
                                          state->subtitle_stream_index, &sub_stream))
                    video_player_play(sub_stream.url, sub_stream.subtitle_url,
                                      state->now_playing.runtime_ticks,
                                      resume_ticks, sub_mode);
            }
            /* Auto-advance from now-playing screen */
            if (state->has_now_playing && state->auto_advance && !state->auto_stopped) {
                player_status_t ps = audio_player_get_status();
                bool audio_done = (ps.state == PLAYER_STOPPED && vs.state == VIDEO_STOPPED);
                bool video_done = (vs.state == VIDEO_STOPPED && ps.state == PLAYER_STOPPED);

                if (audio_done || video_done) {
                    int next = state->playing_index + 1;
                    bool advanced = false;
                    if (next < state->items.count &&
                        item_is_playable(&state->items.items[next]))
                        advanced = ui_start_item(state, session,
                                                 &state->items.items[next],
                                                 next, 0);
                    if (!advanced) {
                        state->has_now_playing = false;
                        state->current_view = state->previous_view;
                    }
                }
            }
        }
        break;

    case VIEW_SETTINGS:
        /* D-pad up/down: move cursor, skip separators */
        if (kdown & KEY_DUP)
            state->settings_index = settings_next_selectable(state->settings_index, -1);
        if (kdown & KEY_DDOWN)
            state->settings_index = settings_next_selectable(state->settings_index, 1);

        /* Scroll to keep cursor visible */
        if (state->settings_index < state->settings_scroll)
            state->settings_scroll = state->settings_index;
        if (state->settings_index >= state->settings_scroll + UI_MAX_VISIBLE_ITEMS)
            state->settings_scroll = state->settings_index - UI_MAX_VISIBLE_ITEMS + 1;

        /* L/R: cycle selector values */
        if ((kdown & KEY_DLEFT) || (kdown & KEY_DRIGHT) ||
            (kdown & KEY_L) || (kdown & KEY_R)) {
            int dir = ((kdown & KEY_DRIGHT) || (kdown & KEY_R)) ? 1 : -1;
            if (state->settings_index == SET_AUDIO_BITRATE) {
                int cur = 0;
                for (int i = 0; i < AUDIO_RATES_COUNT; i++)
                    if (g_config.audio_bitrate == audio_rates[i]) { cur = i; break; }
                cur = (cur + dir + AUDIO_RATES_COUNT) % AUDIO_RATES_COUNT;
                g_config.audio_bitrate = audio_rates[cur];
            } else if (state->settings_index == SET_VIDEO_BITRATE) {
                int cur = 0;
                for (int i = 0; i < VIDEO_RATES_COUNT; i++)
                    if (g_config.video_bitrate == video_rates[i]) { cur = i; break; }
                cur = (cur + dir + VIDEO_RATES_COUNT) % VIDEO_RATES_COUNT;
                g_config.video_bitrate = video_rates[cur];
            }
        }

        /* A: toggle booleans, activate actions */
        if (kdown & KEY_A) {
            if (state->settings_index == SET_AUTO_ADVANCE) {
                g_config.auto_advance = !g_config.auto_advance;
                state->auto_advance = g_config.auto_advance;
            } else if (state->settings_index == SET_CACHE_CLEAR) {
                /* Stop any active playback first: a cached track/video may
                 * be open via the sdmc devoptab, and deleting an open file
                 * underneath FFmpeg/mpg123 is undefined on FAT. */
                audio_player_stop();
                video_player_stop();
                state->has_now_playing = false;
                cache_clear();
                s_cache_bytes_ui = 0;
            } else if (state->settings_index == SET_LOGOUT) {
                jfin_session_t *s = (jfin_session_t *)session;
                jfin_logout(s);
                g_config.access_token[0] = '\0';
                config_save(&g_config);
                state->current_view = VIEW_LOGIN;
                if (g_config.server_url[0] != '\0')
                    snprintf(state->server_url, sizeof(state->server_url),
                             "%s", g_config.server_url);
            }
        }

        /* B: save and return to libraries */
        if (kdown & KEY_B) {
            config_save(&g_config);
            state->current_view = VIEW_LIBRARIES;
            jfin_get_views(session, &state->items);
            state->selected_index = 0;
            state->scroll_offset = 0;
        }
        break;
    }
}

/* ── Navigation ────────────────────────────────────────────────────── */

void ui_navigate_into(ui_state_t *state, const jfin_session_t *session,
                      const jfin_item_t *item)
{
    /* Push breadcrumb first, then fetch. If empty, pop it back. */
    char saved_id[JFIN_MAX_ID];
    char saved_name[JFIN_MAX_NAME];
    snprintf(saved_id, sizeof(saved_id), "%s", item->id);
    snprintf(saved_name, sizeof(saved_name), "%s", item->name);

    if (state->parent_depth < 8) {
        snprintf(state->parent_stack_ids[state->parent_depth],
                 sizeof(state->parent_stack_ids[0]), "%s", saved_id);
        snprintf(state->parent_stack_names[state->parent_depth],
                 sizeof(state->parent_stack_names[0]), "%s", saved_name);
        state->parent_depth++;
    }

    state->current_view = VIEW_BROWSE;
    state->selected_index = 0;
    state->scroll_offset = 0;

    /* Show loading indicator before blocking API call */
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(s_text_buf);
    C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_bottom);
    draw_text(115, 110, 0.6f, rgba(COLOR_PRIMARY), "Loading...");
    C3D_FrameEnd(0);

    /* Fetch into state->items directly (no stack-heavy temp copy) */
    jfin_get_items(session, saved_id, 0, JFIN_MAX_ITEMS, &state->items);

    log_write("NAV: into '%s' id=%s depth=%d items=%d",
              saved_name, saved_id, state->parent_depth, state->items.count);

    if (state->items.count == 0) {
        /* Empty folder — undo navigation */
        log_write("NAV: empty, undoing");
        state->parent_depth--;
        ui_navigate_back(state, session);
    }
}

void ui_navigate_back(ui_state_t *state, const jfin_session_t *session)
{
    if (state->parent_depth <= 0) {
        /* Already at top — go to libraries */
        state->current_view = VIEW_LIBRARIES;
        jfin_get_views(session, &state->items);
        state->selected_index = 0;
        state->scroll_offset = 0;
        return;
    }

    state->parent_depth--;
    state->selected_index = 0;
    state->scroll_offset = 0;

    if (state->parent_depth == 0) {
        state->current_view = VIEW_LIBRARIES;
        jfin_get_views(session, &state->items);
    } else {
        const char *parent_id = state->parent_stack_ids[state->parent_depth - 1];
        jfin_get_items(session, parent_id, 0, JFIN_MAX_ITEMS, &state->items);
    }
}

/* ── Renderers ─────────────────────────────────────────────────────── */

void ui_render_login(const ui_state_t *state)
{
    /* Bottom screen: login form */
    C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_bottom);

    draw_text(10, 10, 0.7f, rgba(COLOR_PRIMARY), "Connect to Jellyfin Server");

    const char *labels[] = {"Server URL:", "Username:", "Password:"};
    const char *values[] = {state->server_url, state->username, "********"};

    for (int i = 0; i < 3; i++) {
        float y = 50 + i * 50;
        u32 bg = (i == state->login_field) ? rgba(COLOR_HIGHLIGHT) : rgba(COLOR_BG_CARD);
        draw_rect(10, y, 300, 40, bg);
        draw_text(15, y + 2, 0.45f, rgba(COLOR_TEXT_SECONDARY), labels[i]);
        draw_text(15, y + 18, 0.5f, rgba(COLOR_TEXT_PRIMARY),
                  values[i][0] ? values[i] : "(tap A to enter)");
    }

    draw_text(10, 210, 0.45f, rgba(COLOR_TEXT_SECONDARY),
              "A: Edit field  R: Connect  START: Exit");
}

void ui_render_libraries(const ui_state_t *state)
{
    /* Bottom screen: library list */
    C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_bottom);

    draw_text(10, 5, 0.55f, rgba(COLOR_PRIMARY), "Libraries");

    for (int i = 0; i < state->items.count && i < UI_MAX_VISIBLE_ITEMS; i++) {
        int idx = state->scroll_offset + i;
        if (idx >= state->items.count) break;

        float y = 30 + i * UI_LIST_ITEM_HEIGHT;
        draw_list_item_bg(y, 310, UI_LIST_ITEM_HEIGHT - 4, idx == state->selected_index);

        draw_text(15, y + 10, 0.55f, rgba(COLOR_TEXT_PRIMARY),
                  state->items.items[idx].name);
    }

    draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY),
              "A:Enter B:Back Y:Playing SEL:Settings");
}

void ui_render_browse(const ui_state_t *state)
{
    /* Bottom screen: item list */
    C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_bottom);

    /* Breadcrumb */
    if (state->parent_depth > 0) {
        draw_text(10, 5, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  state->parent_stack_names[state->parent_depth - 1]);
    }

    for (int i = 0; i < UI_MAX_VISIBLE_ITEMS; i++) {
        int idx = state->scroll_offset + i;
        if (idx >= state->items.count) break;

        float y = 25 + i * UI_LIST_ITEM_HEIGHT;
        const jfin_item_t *item = &state->items.items[idx];

        draw_list_item_bg(y, 310, UI_LIST_ITEM_HEIGHT - 4, idx == state->selected_index);

        /* Item name + badges: [3D] stereoscopic, [SD] in the offline cache */
        const char *badge = (item->video_3d_format != JFIN_3D_NONE) ? " [3D]" : "";
        const char *sd_badge = (item_is_playable(item) &&
                                cache_has(item->id, item_cache_ext(item)))
                               ? " [SD]" : "";
        char label[160];
        if (item->type == JFIN_ITEM_AUDIO && item->index_number > 0) {
            snprintf(label, sizeof(label), "%d. %s%s%s",
                     item->index_number, item->name, badge, sd_badge);
        } else if (item->type == JFIN_ITEM_EPISODE && item->index_number > 0) {
            snprintf(label, sizeof(label), "E%d - %s%s%s",
                     item->index_number, item->name, badge, sd_badge);
        } else {
            snprintf(label, sizeof(label), "%s%s%s", item->name, badge, sd_badge);
        }

        draw_text(15, y + 4, 0.5f, rgba(COLOR_TEXT_PRIMARY), label);

        /* Subtitle: artist/year/duration */
        char sub[128] = {0};
        if (item->artist[0]) {
            snprintf(sub, sizeof(sub), "%s", item->artist);
        } else if (item->year > 0) {
            snprintf(sub, sizeof(sub), "%d", item->year);
        }
        if (item->runtime_ticks > 0) {
            char dur[16];
            format_ticks(item->runtime_ticks, dur, sizeof(dur));
            if (sub[0]) {
                strncat(sub, " - ", sizeof(sub) - strlen(sub) - 1);
                strncat(sub, dur, sizeof(sub) - strlen(sub) - 1);
            } else {
                snprintf(sub, sizeof(sub), "%s", dur);
            }
        }
        if (sub[0])
            draw_text(15, y + 22, 0.4f, rgba(COLOR_TEXT_SECONDARY), sub);
    }

    /* Scroll + page indicator */
    if (state->items.total_count > 0) {
        char info[48];
        if (state->items.total_count > JFIN_MAX_ITEMS) {
            int page = (state->items.start_index / JFIN_MAX_ITEMS) + 1;
            int total_pages = (state->items.total_count + JFIN_MAX_ITEMS - 1) / JFIN_MAX_ITEMS;
            snprintf(info, sizeof(info), "pg %d/%d (%d total)", page, total_pages, state->items.total_count);
        } else {
            snprintf(info, sizeof(info), "%d/%d",
                     state->selected_index + 1, state->items.count);
        }
        draw_text(220, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY), info);
    }

    if (state->items.total_count > JFIN_MAX_ITEMS)
        draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY), "A:Sel B:Back X:DL L/R:Pg SEL:Srch");
    else
        draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY), "A:Sel B:Back X:DL Y:Play SEL:Srch");
}

#define SUB_SCALE 0.55f
#define SUB_LINE_H 14.0f

static void draw_subtitles_top(void)
{
    vp_subtitle_t subs[4];
    int sub_count = video_player_get_subtitles(subs, 4);
    if (!sub_count) return;

    u32 outline = C2D_Color32(0, 0, 0, 200);

    for (int si = 0; si < sub_count; si++) {
        const vp_subtitle_t *s = &subs[si];
        u32 fg = s->color ? s->color : C2D_Color32(255, 255, 255, 255);

        int nlines = 1;
        for (const char *q = s->text; *q; q++) if (*q == '\n') nlines++;

        int row = (s->alignment - 1) / 3;
        float ax = (s->screen_x >= 0.0f) ? s->screen_x : 200.0f;
        float ay;
        if (s->screen_y >= 0.0f) ay = s->screen_y;
        else if (row == 2)        ay = 8.0f;
        else if (row == 1)        ay = 120.0f;
        else                      ay = 226.0f;

        float block_h = nlines * SUB_LINE_H;
        float line_y;
        if (row == 0)      line_y = ay - block_h;
        else if (row == 1) line_y = ay - block_h / 2.0f;
        else               line_y = ay;

        const char *lp = s->text;
        while (lp && *lp) {
            const char *nl = strchr(lp, '\n');
            int ll = nl ? (int)(nl - lp) : (int)strlen(lp);

            char lbuf[260];
            if (ll >= (int)sizeof(lbuf)) ll = sizeof(lbuf) - 1;
            memcpy(lbuf, lp, ll);
            lbuf[ll] = '\0';

            C2D_Text ct;
            C2D_TextParse(&ct, s_text_buf, lbuf);
            C2D_TextOptimize(&ct);

            float tw = 0, th = 0;
            C2D_TextGetDimensions(&ct, SUB_SCALE, SUB_SCALE, &tw, &th);
            (void)th;

            int col = (s->alignment - 1) % 3;
            float lx;
            if (col == 0)      lx = ax;
            else if (col == 1) lx = ax - tw / 2.0f;
            else               lx = ax - tw;

            if (lx < 2.0f) lx = 2.0f;
            if (lx + tw > 398.0f) lx = 398.0f - tw;

            C2D_DrawText(&ct, C2D_WithColor, lx-1, line_y-1, 0.5f, SUB_SCALE, SUB_SCALE, outline);
            C2D_DrawText(&ct, C2D_WithColor, lx+1, line_y-1, 0.5f, SUB_SCALE, SUB_SCALE, outline);
            C2D_DrawText(&ct, C2D_WithColor, lx-1, line_y+1, 0.5f, SUB_SCALE, SUB_SCALE, outline);
            C2D_DrawText(&ct, C2D_WithColor, lx+1, line_y+1, 0.5f, SUB_SCALE, SUB_SCALE, outline);
            C2D_DrawText(&ct, C2D_WithColor, lx,   line_y,   0.5f, SUB_SCALE, SUB_SCALE, fg);

            line_y += SUB_LINE_H;
            lp = nl ? nl + 1 : NULL;
        }
    }
}

void ui_render_now_playing(const ui_state_t *state, const player_status_t *player)
{
    video_status_t vstatus = video_player_get_status();
    bool is_video = (vstatus.state != VIDEO_STOPPED);

    /* Top screen */
    C2D_TargetClear(s_top, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_top);

    if (!state->has_now_playing) {
        draw_text(120, 110, 0.7f, rgba(COLOR_TEXT_SECONDARY), "Nothing playing");
        return;
    }

    if (is_video && vstatus.state == VIDEO_ERROR) {
        /* Show error prominently on top screen */
        draw_text(50, 60, 0.7f, rgba(0xFF4444FF), "Playback Error");
        draw_text(30, 100, 0.5f, rgba(COLOR_TEXT_PRIMARY),
                  vstatus.error_msg[0] ? vstatus.error_msg : "Cannot play this content");
        draw_text(30, 140, 0.5f, rgba(COLOR_TEXT_SECONDARY),
                  state->now_playing.name);
        draw_text(60, 190, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  "Press any button to go back");
    } else if (is_video && vstatus.state == VIDEO_LOADING) {
        /* Show buffering indicator while video is loading */
        draw_text(130, 100, 0.7f, rgba(COLOR_PRIMARY), "Buffering...");
        draw_text(80, 135, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  state->now_playing.name);
    } else if (is_video) {
        /* Render video frame on top screen */
        video_player_render_frame();
        draw_subtitles_top();

        /* Right eye for stereoscopic 3D (only when 3D slider is up) */
        if (vstatus.is_3d && osGet3DSliderState() > 0.0f) {
            C2D_TargetClear(s_top_right, rgba(0x000000FF));
            C2D_SceneBegin(s_top_right);
            video_player_render_frame_right();
            draw_subtitles_top();
        }
    } else if (player->state == PLAYER_LOADING) {
        /* Show buffering indicator while audio is loading */
        draw_text(130, 100, 0.7f, rgba(COLOR_PRIMARY), "Buffering...");
        draw_text(80, 135, 0.45f, rgba(COLOR_TEXT_SECONDARY),
                  state->now_playing.name);
    } else {
        /* Audio-only: show track info */
        const jfin_item_t *item = &state->now_playing;

        /* Album art or placeholder */
        draw_rect(125, 20, 150, 150, rgba(COLOR_BG_CARD));
        if (album_art_is_loaded())
            album_art_draw(125, 20, 150);
        else
            draw_text(165, 85, 0.6f, rgba(COLOR_TEXT_SECONDARY), "ART");

        draw_text(50, 180, 0.6f, rgba(COLOR_TEXT_PRIMARY), item->name);

        if (item->artist[0])
            draw_text(50, 200, 0.45f, rgba(COLOR_ACCENT), item->artist);

        if (item->album[0])
            draw_text(50, 215, 0.4f, rgba(COLOR_TEXT_SECONDARY), item->album);
    }

    /* Bottom screen: black when hidden (night mode), controls otherwise */
    C2D_TargetClear(s_bottom, rgba(0x000000FF));
    C2D_SceneBegin(s_bottom);

    if (state->bottom_hidden)
        return; /* black bottom screen — just the clear above */

    /* Use video position/state if video is playing, otherwise audio */
    int64_t pos_ticks, dur_ticks;
    int buf_pct;
    const char *state_str = "STOPPED";

    if (is_video) {
        pos_ticks = vstatus.position_ticks;
        dur_ticks = vstatus.duration_ticks;
        buf_pct = vstatus.buffer_percent;
        switch (vstatus.state) {
        case VIDEO_LOADING:  state_str = "BUFFERING..."; break;
        case VIDEO_PLAYING:  state_str = "PLAYING"; break;
        case VIDEO_PAUSED:   state_str = "PAUSED"; break;
        case VIDEO_ERROR:    state_str = vstatus.error_msg; break;
        default: break;
        }
    } else {
        pos_ticks = player->position_ticks;
        dur_ticks = player->duration_ticks;
        buf_pct = player->buffer_percent;
        switch (player->state) {
        case PLAYER_LOADING:  state_str = "BUFFERING..."; break;
        case PLAYER_PLAYING:  state_str = "PLAYING"; break;
        case PLAYER_PAUSED:   state_str = "PAUSED"; break;
        case PLAYER_ERROR:    state_str = player->error_msg; break;
        default: break;
        }
    }

    /* Progress bar */
    float progress = 0.0f;
    if (dur_ticks > 0)
        progress = (float)pos_ticks / (float)dur_ticks;
    if (progress > 1.0f) progress = 1.0f;

    draw_rect(20, 40, 280, 6, rgba(COLOR_BG_CARD));
    draw_rect(20, 40, 280 * progress, 6, rgba(COLOR_PRIMARY));

    /* Time labels */
    char pos_str[16], dur_str[16];
    format_ticks(pos_ticks, pos_str, sizeof(pos_str));
    format_ticks(dur_ticks, dur_str, sizeof(dur_str));
    draw_text(20, 50, 0.4f, rgba(COLOR_TEXT_SECONDARY), pos_str);
    draw_text(270, 50, 0.4f, rgba(COLOR_TEXT_SECONDARY), dur_str);

    draw_text(110, 80, 0.55f, rgba(COLOR_PRIMARY), state_str);

    /* Buffer indicator */
    char buf_str[64];
    snprintf(buf_str, sizeof(buf_str), "Buffer: %d%%", buf_pct);
    draw_text(115, 100, 0.4f, rgba(COLOR_TEXT_SECONDARY), buf_str);

    /* Diagnostics and subtitle status for video playback */
    if (is_video) {
        char diag[80];
        snprintf(diag, sizeof(diag), "%sDec: %.0f fps  Disp: %.0f fps  %dx%d",
                 vstatus.is_3d ? "3D  " : "",
                 vstatus.decode_fps, vstatus.display_fps,
                 vstatus.video_width, vstatus.video_height);
        draw_text(30, 125, 0.38f, rgba(COLOR_TEXT_SECONDARY), diag);

        char sub_str[48];
        if (state->subtitle_stream_index >= 0) {
            const char *label = state->subtitle_lang_pref[0] ? state->subtitle_lang_pref : "on";
            for (int i = 0; i < state->subtitle_list.count; i++) {
                if (state->subtitle_list.subs[i].index == state->subtitle_stream_index) {
                    label = state->subtitle_list.subs[i].language[0] ?
                            state->subtitle_list.subs[i].language :
                            state->subtitle_list.subs[i].title;
                    break;
                }
            }
            snprintf(sub_str, sizeof(sub_str), "Subs: %s", label);
        } else {
            snprintf(sub_str, sizeof(sub_str), "Subs: off");
        }
        u32 sub_color = (state->subtitle_stream_index >= 0) ?
                        rgba(COLOR_ACCENT) : rgba(COLOR_TEXT_SECONDARY);
        draw_text(30, 145, 0.4f, sub_color, sub_str);
    }

    /* Controls hint */
    if (is_video)
        draw_text(20, 180, 0.45f, rgba(COLOR_TEXT_PRIMARY),
                  "A:Pause X:Stop B:Back L/R:Seek Y:Subs");
    else
        draw_text(20, 180, 0.45f, rgba(COLOR_TEXT_PRIMARY),
                  "A:Pause X:Stop B:Back L/R:Seek");
}

void ui_render_settings(const ui_state_t *state, const jfin_session_t *session)
{
    C2D_TargetClear(s_bottom, rgba(COLOR_BG_DARK));
    C2D_SceneBegin(s_bottom);

    draw_text(10, 5, 0.55f, rgba(COLOR_PRIMARY), "Settings");

    for (int i = 0; i < UI_MAX_VISIBLE_ITEMS + 1 && i + state->settings_scroll < SET_COUNT; i++) {
        int idx = state->settings_scroll + i;
        float y = 30 + i * UI_LIST_ITEM_HEIGHT;

        if (settings_is_separator(idx)) {
            /* Separator: thin line + label */
            float line_y = y + UI_LIST_ITEM_HEIGHT / 2;
            draw_rect(10, line_y, 300, 1, rgba(COLOR_SEPARATOR));
            const char *sep_label = (idx == SET_SEPARATOR_ACCOUNT) ? "Account" : "About";
            draw_text(15, line_y + 4, 0.4f, rgba(COLOR_TEXT_SECONDARY), sep_label);
            continue;
        }

        bool selected = (idx == state->settings_index);
        draw_list_item_bg(y, 310, UI_LIST_ITEM_HEIGHT - 4, selected);

        /* Label + value per item */
        const char *label = "";
        char value[128] = {0};
        u32 value_color = rgba(COLOR_VALUE);

        switch (idx) {
        case SET_AUDIO_BITRATE:
            label = "Audio Bitrate";
            snprintf(value, sizeof(value), "%d kbps", g_config.audio_bitrate);
            break;
        case SET_VIDEO_BITRATE:
            label = "Video Bitrate";
            snprintf(value, sizeof(value), "%d kbps", g_config.video_bitrate);
            break;
        case SET_AUTO_ADVANCE:
            label = "Auto-advance";
            snprintf(value, sizeof(value), "%s", g_config.auto_advance ? "On" : "Off");
            break;
        case SET_CACHE_CLEAR:
            label = "Offline Cache";
            snprintf(value, sizeof(value), "%llu MB  A: clear",
                     (unsigned long long)(s_cache_bytes_ui / (1024 * 1024)));
            break;
        case SET_SERVER:
            label = "Server";
            snprintf(value, sizeof(value), "%.30s%s",
                     session->server_url,
                     strlen(session->server_url) > 30 ? "..." : "");
            value_color = rgba(COLOR_TEXT_SECONDARY);
            break;
        case SET_USERNAME:
            label = "User";
            snprintf(value, sizeof(value), "%s", g_config.username);
            value_color = rgba(COLOR_TEXT_SECONDARY);
            break;
        case SET_LOGOUT:
            label = "Logout";
            value_color = rgba(COLOR_DANGER);
            break;
        case SET_VERSION:
            label = "Version";
            snprintf(value, sizeof(value), "v" JFIN_VERSION);
            value_color = rgba(COLOR_TEXT_SECONDARY);
            break;
        case SET_DEVICE_ID:
            label = "Device";
            snprintf(value, sizeof(value), "%.18s%s",
                     g_config.device_id,
                     strlen(g_config.device_id) > 18 ? "..." : "");
            value_color = rgba(COLOR_TEXT_SECONDARY);
            break;
        }

        draw_text(15, y + 10, 0.5f,
                  idx == SET_LOGOUT ? rgba(COLOR_DANGER) : rgba(COLOR_TEXT_PRIMARY),
                  label);
        if (value[0])
            draw_text(200, y + 10, 0.45f, value_color, value);
    }

    draw_text(10, 220, 0.4f, rgba(COLOR_TEXT_SECONDARY),
              "A:Toggle L/R:Change B:Back");
}

/* ── Main render dispatch ──────────────────────────────────────────── */

void ui_render(const ui_state_t *state, const jfin_session_t *session,
               const player_status_t *player)
{
    /* Enable stereoscopic 3D only while a 3D frame is actually being
     * drawn (PLAYING/PAUSED) AND the slider is up. The right-eye render
     * is slider-gated, so without the slider term here a slider at 0
     * would present a stale right framebuffer; with it, slider 0 falls
     * back to clean 2D (left eye full-res) per the design doc. */
    video_status_t vs_3d = video_player_get_status();
    gfxSet3D(state->current_view == VIEW_NOW_PLAYING && vs_3d.is_3d &&
             (vs_3d.state == VIDEO_PLAYING || vs_3d.state == VIDEO_PAUSED) &&
             osGet3DSliderState() > 0.0f);

    C2D_TextBufClear(s_text_buf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    /* Top screen: branding or now-playing */
    if (state->current_view == VIEW_NOW_PLAYING) {
        ui_render_now_playing(state, player);
    } else {
        C2D_TargetClear(s_top, rgba(COLOR_BG_DARK));
        C2D_SceneBegin(s_top);

        draw_text(100, 80, 1.0f, rgba(COLOR_PRIMARY), "Jellyfin 3DS");
        draw_text(130, 120, 0.5f, rgba(COLOR_TEXT_SECONDARY), "v" JFIN_VERSION);

        /* Show mini now-playing bar if something is playing */
        if (state->has_now_playing && player->state == PLAYER_PLAYING) {
            draw_rect(0, 210, 400, 30, rgba(COLOR_BG_CARD));
            draw_text(10, 215, 0.45f, rgba(COLOR_TEXT_PRIMARY),
                      state->now_playing.name);
            draw_text(340, 215, 0.4f, rgba(COLOR_ACCENT), "Y: View");
        }
    }

    /* Bottom screen: view-specific */
    switch (state->current_view) {
    case VIEW_LOGIN:
        ui_render_login(state);
        break;
    case VIEW_LIBRARIES:
        ui_render_libraries(state);
        break;
    case VIEW_BROWSE:
        ui_render_browse(state);
        break;
    case VIEW_NOW_PLAYING:
        /* Already rendered above (both screens) */
        break;
    case VIEW_SETTINGS:
        ui_render_settings(state, session);
        break;
    }

    C3D_FrameEnd(0);
}
