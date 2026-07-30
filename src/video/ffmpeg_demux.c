/**
 * ffmpeg_demux.c — MPEG-TS demuxer via FFmpeg avformat
 *
 * Uses a custom AVIO context that reads from a ring buffer.
 * The ring buffer is filled by a separate network thread (curl).
 */

#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

#include "video/ffmpeg_demux.h"
#include "util/log.h"

#define AVIO_BUF_SIZE (64 * 1024)  /* 64KB AVIO read buffer */

/* Last valid video dimensions seen across any session.
 * When avformat_find_stream_info returns 0x0 (SPS not in probe window — common
 * after seeks on both local and online streams), we fall back to these so the
 * MVD is initialised with real dimensions.  Without this fallback every NAL
 * fails with 0xD96170CA because MVD rejects all input when width/height = 0. */
static int s_cached_width  = 0;
static int s_cached_height = 0;

/* ── Ring buffer read (called by FFmpeg's AVIO) ────────────────────── */

static int ring_read_for_avio(void *opaque, uint8_t *buf, int buf_size)
{
    demux_ctx_t *ctx = (demux_ctx_t *)opaque;

    /* Wait for data (block with sleep, not spin) */
    int waited = 0;
    while (__atomic_load_n(&ctx->ring_fill, __ATOMIC_ACQUIRE) < buf_size
           && !__atomic_load_n(&ctx->ring_finished, __ATOMIC_ACQUIRE)) {
        if (waited > 15000) { /* 15 seconds — allows net thread retries on WiFi drops */
            log_write("DEC: ring_read_for_avio TIMEOUT (15s), fill=%d finished=%d",
                      __atomic_load_n(&ctx->ring_fill, __ATOMIC_ACQUIRE),
                      __atomic_load_n(&ctx->ring_finished, __ATOMIC_ACQUIRE));
            return AVERROR_EOF;
        }
        svcSleepThread(1000000LL); /* 1ms */
        waited++;
    }

    int avail = __atomic_load_n(&ctx->ring_fill, __ATOMIC_ACQUIRE);
    if (avail == 0 && __atomic_load_n(&ctx->ring_finished, __ATOMIC_ACQUIRE))
        return AVERROR_EOF;

    int to_read = (buf_size < avail) ? buf_size : avail;
    if (to_read > ctx->ring_size) to_read = ctx->ring_size; /* defensive */

    /* Copy from ring buffer */
    for (int i = 0; i < to_read; i++) {
        buf[i] = ctx->ring_data[ctx->ring_read_pos];
        ctx->ring_read_pos = (ctx->ring_read_pos + 1) % ctx->ring_size;
    }
    __atomic_fetch_sub(&ctx->ring_fill, to_read, __ATOMIC_RELEASE);

    return to_read;
}

/* ── Public API ────────────────────────────────────────────────────── */

bool demux_init(demux_ctx_t *ctx)
{
    AVFormatContext *fmt_ctx = NULL;
    AVIOContext *avio_ctx = NULL;
    int ret;

    const AVInputFormat *input_fmt = av_find_input_format("mpegts");

    /* Large probe budget: subtitle burn-in transcodes delay video output by
     * 2-5s while the server-side filter chain initialises, so we must wait
     * for enough data to include the first video packet with SPS/PPS.
     * For normal streams these limits are never reached (probe exits early
     * once all stream info is found). */
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "probesize", "524288", 0);
    av_dict_set(&opts, "analyzeduration", "10000000", 0);

    if (ctx->local_path) {
        /* Cached file on SD: open via FFmpeg's file protocol (newlib
         * routes open/read/lseek to the sdmc devoptab). Seekable —
         * no ring buffer, no custom AVIO. */
        ret = avformat_open_input(&fmt_ctx, ctx->local_path, input_fmt, &opts);
        av_dict_free(&opts);
        if (ret < 0)
            return false;
    } else {
        /* Network stream: custom AVIO over the curl-fed ring buffer */
        ctx->avio_buf = av_malloc(AVIO_BUF_SIZE);
        if (!ctx->avio_buf) {
            av_dict_free(&opts);
            return false;
        }

        avio_ctx = avio_alloc_context(
            ctx->avio_buf, AVIO_BUF_SIZE,
            0,            /* write_flag = 0 (read-only) */
            ctx,          /* opaque */
            ring_read_for_avio,
            NULL,         /* write callback */
            NULL          /* seek callback — no seeking for live TS */
        );
        if (!avio_ctx) {
            av_free(ctx->avio_buf);
            av_dict_free(&opts);
            return false;
        }

        fmt_ctx = avformat_alloc_context();
        if (!fmt_ctx) {
            avio_context_free(&avio_ctx);
            av_dict_free(&opts);
            return false;
        }
        fmt_ctx->pb = avio_ctx;
        fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

        ret = avformat_open_input(&fmt_ctx, NULL, input_fmt, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            avio_context_free(&avio_ctx);
            return false;
        }
    }

    /* Find stream info */
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        avformat_close_input(&fmt_ctx);
        avio_context_free(&avio_ctx);
        return false;
    }

    /* Find video and audio streams */
    ctx->video_stream_idx = -1;
    ctx->audio_stream_idx = -1;

    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVCodecParameters *par = fmt_ctx->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && ctx->video_stream_idx < 0) {
            ctx->video_stream_idx     = i;
            ctx->video_width          = par->width;
            ctx->video_height         = par->height;
            ctx->video_extradata      = par->extradata;
            ctx->video_extradata_size = par->extradata_size;
            if (ctx->video_width > 0 && ctx->video_height > 0) {
                s_cached_width  = ctx->video_width;
                s_cached_height = ctx->video_height;
            } else if (s_cached_width > 0) {
                ctx->video_width  = s_cached_width;
                ctx->video_height = s_cached_height;
                log_write("DEC: dimensions 0x0 from probe — using cached %dx%d",
                          s_cached_width, s_cached_height);
            }
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && ctx->audio_stream_idx < 0) {
            ctx->audio_stream_idx = i;
            ctx->audio_sample_rate = par->sample_rate;
#if LIBAVCODEC_VERSION_MAJOR >= 60
            ctx->audio_channels = par->ch_layout.nb_channels;
#else
            ctx->audio_channels = par->channels;
#endif
        }
    }

    if (ctx->video_stream_idx < 0)
        return false; /* no video stream found */

    ctx->fmt_ctx = fmt_ctx;
    ctx->avio_ctx = avio_ctx;
    return true;
}

int demux_read_packet(demux_ctx_t *ctx, AVPacket *pkt, bool *is_video)
{
    AVFormatContext *fmt_ctx = (AVFormatContext *)ctx->fmt_ctx;

    int ret = av_read_frame(fmt_ctx, pkt);
    if (ret < 0)
        return ret;

    *is_video = (pkt->stream_index == ctx->video_stream_idx);
    return 0;
}

bool demux_seek(demux_ctx_t *ctx, int64_t position_ticks)
{
    if (!ctx->fmt_ctx || !ctx->local_path)
        return false; /* network streams seek via StartTimeTicks restart */

    AVFormatContext *fmt_ctx = (AVFormatContext *)ctx->fmt_ctx;
    /* Jellyfin ticks are 100ns; av_seek_frame with stream -1 wants
     * AV_TIME_BASE (microseconds). BACKWARD lands on the prior keyframe
     * so MVD starts from a clean IDR (TS carries SPS/PPS inline). */
    int64_t ts_us = position_ticks / 10;
    int ret = av_seek_frame(fmt_ctx, -1, ts_us, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        /* Some TS files only resolve timestamps approximately */
        ret = av_seek_frame(fmt_ctx, -1, ts_us,
                            AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    }
    return ret >= 0;
}

void demux_cleanup(demux_ctx_t *ctx)
{
    if (ctx->fmt_ctx) {
        AVFormatContext *fmt_ctx = (AVFormatContext *)ctx->fmt_ctx;
        avformat_close_input(&fmt_ctx);
        ctx->fmt_ctx = NULL;
    }
    if (ctx->avio_ctx) {
        AVIOContext *avio_ctx = (AVIOContext *)ctx->avio_ctx;
        /* avio_buf is freed by avio_context_free if allocated by av_malloc */
        avio_context_free(&avio_ctx);
        ctx->avio_ctx = NULL;
        ctx->avio_buf = NULL;
    }
}
