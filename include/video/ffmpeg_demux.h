/**
 * ffmpeg_demux.h — MPEG-TS demuxer via FFmpeg
 *
 * Opens a TS stream from a ring buffer (fed by curl), demuxes into
 * separate audio and video packet queues.
 */

#ifndef JFIN_FFMPEG_DEMUX_H
#define JFIN_FFMPEG_DEMUX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations — avoid exposing FFmpeg headers globally */
typedef struct AVPacket AVPacket;
typedef struct AVCodecParameters AVCodecParameters;

typedef struct {
    /* Local file playback (offline cache): when set, demux_init opens
     * this path directly via FFmpeg's file protocol — seekable, and the
     * ring buffer / network thread are not used at all. */
    const char *local_path;

    /* Ring buffer fed by network thread */
    uint8_t *ring_data;
    int      ring_size;
    int      ring_write_pos;
    int      ring_read_pos;
    volatile int ring_fill;
    volatile bool ring_finished;

    /* Internal FFmpeg state (opaque) */
    void *fmt_ctx;    /* AVFormatContext* */
    void *avio_ctx;   /* AVIOContext* */
    uint8_t *avio_buf;

    /* Stream indices */
    int video_stream_idx;
    int audio_stream_idx;

    /* Stream info */
    int video_width;
    int video_height;
    int audio_sample_rate;
    int audio_channels;

    /* Codec extradata (for SPS/PPS) */
    uint8_t *video_extradata;
    int      video_extradata_size;
} demux_ctx_t;

/**
 * Initialize the demuxer. Call after the ring buffer has enough data
 * for FFmpeg to probe the stream format (~64KB).
 */
bool demux_init(demux_ctx_t *ctx);

/**
 * Read the next packet. Returns 0 on success, negative on error/EOF.
 * Caller must free the packet with av_packet_unref after use.
 * Sets *is_video to true for video packets, false for audio.
 */
int demux_read_packet(demux_ctx_t *ctx, AVPacket *pkt, bool *is_video);

/**
 * Seek to a position (Jellyfin 10MHz ticks). Only works for seekable
 * inputs (local files); returns false for live network streams.
 */
bool demux_seek(demux_ctx_t *ctx, int64_t position_ticks);

/**
 * Clean up demuxer resources.
 */
void demux_cleanup(demux_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_FFMPEG_DEMUX_H */
