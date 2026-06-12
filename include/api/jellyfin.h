/**
 * jellyfin.h - Jellyfin REST API client
 *
 * Handles authentication, library browsing, and stream URL negotiation.
 * Uses libcurl + cJSON. All functions are synchronous (blocking).
 */

#ifndef JFIN_API_H
#define JFIN_API_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Limits tuned for 3DS memory constraints */
#define JFIN_MAX_URL          1024
#define JFIN_URL_BUF          2048  /* local buffer for URL composition */
#define JFIN_MAX_TOKEN        256
#define JFIN_MAX_NAME         128
#define JFIN_MAX_ID           64
#define JFIN_MAX_ITEMS        50   /* max items per page */
#define JFIN_IMAGE_MAX_WIDTH  400  /* top screen width */
#define JFIN_IMAGE_MAX_HEIGHT 240  /* top screen height */

/* ── Types ─────────────────────────────────────────────────────────── */

typedef enum {
    JFIN_3D_NONE = 0,
    JFIN_3D_HSBS,  /* HalfSideBySide — each eye anamorphic half-width */
    JFIN_3D_FSBS,  /* FullSideBySide — each eye at native aspect */
    JFIN_3D_HTAB,  /* HalfTopAndBottom (not rendered in 3D yet) */
} jfin_3d_format_t;

typedef enum {
    JFIN_ITEM_FOLDER,
    JFIN_ITEM_MUSIC_ALBUM,
    JFIN_ITEM_MUSIC_ARTIST,
    JFIN_ITEM_AUDIO,
    JFIN_ITEM_MOVIE,
    JFIN_ITEM_SERIES,
    JFIN_ITEM_SEASON,
    JFIN_ITEM_EPISODE,
    JFIN_ITEM_UNKNOWN
} jfin_item_type_t;

typedef struct {
    char id[JFIN_MAX_ID];
    char name[JFIN_MAX_NAME];
    char album[JFIN_MAX_NAME];       /* for audio tracks */
    char artist[JFIN_MAX_NAME];      /* for audio tracks / albums */
    char series_name[JFIN_MAX_NAME]; /* for episodes */
    jfin_item_type_t type;
    int  year;
    int  index_number;               /* track/episode number */
    char album_id[JFIN_MAX_ID];      /* for album art fallback on audio tracks */
    int64_t runtime_ticks;           /* duration in 10M ticks */
    bool has_primary_image;
    bool has_album_image;            /* album has art (for audio track fallback) */
    jfin_3d_format_t video_3d_format; /* stereoscopic 3D format (SBS/TAB) */
} jfin_item_t;

typedef struct {
    jfin_item_t items[JFIN_MAX_ITEMS];
    int         count;
    int         total_count;         /* total matching items on server */
    int         start_index;         /* pagination offset */
} jfin_item_list_t;

typedef struct {
    char url[JFIN_URL_BUF];          /* ready-to-fetch stream URL */
    char container[32];              /* "mp3", "opus", "ts", etc. */
    bool is_transcoding;
    
} jfin_stream_t;

typedef struct {
    char server_url[JFIN_MAX_URL];   /* e.g. "http://your-server:8096" */
    char access_token[JFIN_MAX_TOKEN];
    char user_id[JFIN_MAX_ID];
    char device_id[JFIN_MAX_ID];
    char server_name[JFIN_MAX_NAME];
    bool authenticated;
} jfin_session_t;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/**
 * Initialize the API client. Call once at startup.
 * Initializes libcurl and generates a persistent device ID.
 */
bool jfin_init(void);

/**
 * Shut down the API client. Call once at exit.
 */
void jfin_cleanup(void);

/* ── Authentication ────────────────────────────────────────────────── */

/**
 * Authenticate with username and password.
 * device_id is the persistent per-console id (config); pass NULL/empty
 * to fall back to the legacy fixed id.
 * On success, session is populated and session->authenticated == true.
 */
bool jfin_login(jfin_session_t *session, const char *server_url,
                const char *username, const char *password,
                const char *device_id);

/**
 * Start QuickConnect flow. Returns a code the user enters in the web UI.
 * Poll jfin_quickconnect_poll() until it returns true.
 */
bool jfin_quickconnect_start(jfin_session_t *session, const char *server_url,
                             char *code_out, int code_out_len);

/**
 * Poll QuickConnect status. Returns true when the user has approved.
 */
bool jfin_quickconnect_poll(jfin_session_t *session);

/**
 * Log out and invalidate the access token.
 */
void jfin_logout(jfin_session_t *session);

/* ── Library Browsing ──────────────────────────────────────────────── */

/**
 * Get the user's top-level library views (Music, Movies, Shows, etc.)
 */
bool jfin_get_views(const jfin_session_t *session, jfin_item_list_t *out);

/**
 * Get child items of a parent (folder, library, album, series, etc.)
 * start_index and limit control pagination.
 */
bool jfin_get_items(const jfin_session_t *session, const char *parent_id,
                    int start_index, int limit, jfin_item_list_t *out);

/**
 * Get "Continue Listening/Watching" items.
 */
bool jfin_get_resume(const jfin_session_t *session, jfin_item_list_t *out);

/**
 * Get recently added items for a library.
 */
bool jfin_get_latest(const jfin_session_t *session, const char *parent_id,
                     int limit, jfin_item_list_t *out);

/**
 * Search across all libraries.
 */
bool jfin_search(const jfin_session_t *session, const char *query,
                 int limit, jfin_item_list_t *out);

/* ── Streaming ─────────────────────────────────────────────────────── */

/**
 * Get an audio stream URL. start_ticks = 0 for beginning, or seek position.
 */
bool jfin_get_audio_stream(const jfin_session_t *session, const char *item_id,
                           int64_t start_ticks, jfin_stream_t *out);

/**
 * Get a video stream URL. start_ticks = 0 for beginning, or seek position.
 */
bool jfin_get_video_stream(const jfin_session_t *session, const char *item_id,
                           int64_t start_ticks, bool is_3d,
                           jfin_stream_t *out);



/* ── Images ────────────────────────────────────────────────────────── */

/**
 * Build a URL for an item's image, pre-scaled for 3DS.
 * Falls back to album art for audio tracks without their own image.
 * Does not fetch the image — caller uses the URL with their own loader.
 */
void jfin_get_image_url_for_item(const jfin_session_t *session,
                                 const jfin_item_t *item,
                                 int max_width, int max_height,
                                 char *url_out, int url_out_len);

/* ── Playback Reporting ────────────────────────────────────────────── */

/**
 * Report playback start to the server (updates "Now Playing" on dashboard).
 */
bool jfin_report_start(const jfin_session_t *session, const char *item_id);

/**
 * Report playback progress (position in ticks).
 */
bool jfin_report_progress(const jfin_session_t *session, const char *item_id,
                          int64_t position_ticks, bool is_paused);

/**
 * Report playback stopped.
 */
bool jfin_report_stop(const jfin_session_t *session, const char *item_id,
                      int64_t position_ticks);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_API_H */
