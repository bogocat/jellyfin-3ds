/**
 * config.h - Persistent configuration (saved to SD card)
 *
 * Stores server URL, credentials, and preferences in a simple
 * INI-style file at sdmc:/3ds/jellyfin-3ds/config.ini
 */

#ifndef JFIN_CONFIG_H
#define JFIN_CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_PATH "sdmc:/3ds/jellyfin-3ds/config.ini"
#define CACHE_PATH  "sdmc:/3ds/jellyfin-3ds/cache/"

typedef struct {
    char server_url[512];
    char username[64];
    char access_token[256];
    char user_id[64];
    char device_id[64];
    int  audio_bitrate;      /* kbps, default 128 */
    int  video_bitrate;      /* kbps, default 472 */
    bool prefer_transcoding; /* always transcode vs direct play */
    bool auto_advance;       /* auto-play next track, default true */
} jfin_config_t;

/**
 * Load config from SD card. Returns false if no config file exists.
 * Missing fields get sensible defaults.
 */
bool config_load(jfin_config_t *config);

/**
 * Save config to SD card. Creates directories if needed.
 */
bool config_save(const jfin_config_t *config);

/**
 * Generate a unique device ID (persisted across sessions).
 * Uses 3DS serial if available, otherwise random UUID.
 */
void config_ensure_device_id(jfin_config_t *config);

/**
 * Save session credentials from a live session into config and write to disk.
 * Call immediately after successful login so credentials persist even on crash.
 */
void config_save_session(jfin_config_t *config, const char *server_url,
                         const char *access_token, const char *user_id,
                         const char *username);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_CONFIG_H */
