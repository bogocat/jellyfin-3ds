/**
 * video_player.h — Video streaming player (New 3DS only)
 *
 * Streams video from a Jellyfin TS URL, demuxes with FFmpeg,
 * decodes H.264 via MVD hardware, renders on top screen.
 * Audio is decoded via FFmpeg AAC and played through NDSP.
 */

#ifndef JFIN_VIDEO_PLAYER_H
#define JFIN_VIDEO_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VIDEO_STOPPED,
    VIDEO_LOADING,
    VIDEO_PLAYING,
    VIDEO_PAUSED,
    VIDEO_ERROR
} video_state_t;

typedef struct {
    video_state_t state;
    int64_t  position_ticks;
    int64_t  duration_ticks;
    int      buffer_percent;
    int      video_width;
    int      video_height;
    float    decode_fps;
    float    display_fps;
    int      frames_decoded;
    int      frames_displayed;
    char     error_msg[128];
    bool     is_3d;
} video_status_t;

/* Stereoscopic mode for side-by-side content */
typedef enum {
    VP_3D_NONE = 0,
    VP_3D_HSBS,  /* half-width SBS: stretch each eye 2x horizontally */
    VP_3D_FSBS,  /* full-width SBS: each eye already at native aspect */
} vp_3d_mode_t;

/**
 * Check if the current hardware supports video playback (New 3DS).
 */
bool video_player_is_supported(void);

/**
 * Initialize the video player subsystem.
 * Returns false on Old 3DS or if MVD init fails.
 */
bool video_player_init(void);

/**
 * Shut down the video player. Stops playback and frees all resources.
 */
void video_player_cleanup(void);

/**
 * Start video playback from a TS stream URL.
 * seek_offset_ticks: position in the original media this stream starts from.
 */
bool video_player_play(const char *url, int64_t duration_ticks,
                       int64_t seek_offset_ticks, vp_3d_mode_t mode_3d);

/**
 * Stop playback.
 */
void video_player_stop(void);

/**
 * Toggle pause/resume.
 */
void video_player_pause(void);

/**
 * Get current status. Safe to call from any thread.
 */
video_status_t video_player_get_status(void);

/**
 * Must be called each frame from the main loop.
 * Handles frame display on the top screen.
 */
void video_player_render_frame(void);

/**
 * Render the right-eye frame for stereoscopic 3D.
 * Call after video_player_render_frame() within the right-eye scene.
 */
void video_player_render_frame_right(void);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_VIDEO_PLAYER_H */
