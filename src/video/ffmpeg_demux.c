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

#define AVIO_BUF_SIZE (64 * 1024)  /* 64KB AVIO read buffer */

/* ── Ring buffer read (called by FFmpeg's AVIO) ────────────────────── */

static int ring_read_for_avio(void *opaque, uint8_t *buf, int buf_size)
{
    demux_ctx_t *ctx = (demux_ctx_t *)opaque;

    /* Wait for data (block with sleep, not spin) */
    int waited = 0;
    while (__atomic_load_n(&ctx->ring_fill, __ATOMIC_ACQUIRE) < buf_size
           && !__atomic_load_n(&ctx->ring_finished, __ATOMIC_ACQUIRE)) {
        if (waited > 15000) /* 15 seconds — allows net thread retries on WiFi drops */
            return AVERROR_EOF;
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

    /* Allocate AVIO buffer */
    ctx->avio_buf = av_malloc(AVIO_BUF_SIZE);
    if (!ctx->avio_buf)
        return false;

    /* Create custom AVIO context that reads from our ring buffer */
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
        return false;
    }

    /* Allocate format context */
    fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        avio_context_free(&avio_ctx);
        return false;
    }
    fmt_ctx->pb = avio_ctx;
    fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    /* Probe and open the TS stream */
    const AVInputFormat *input_fmt = av_find_input_format("mpegts");

    /* Speed up format detection: 32KB probe + 0.5s analysis is enough for TS */
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "probesize", "32768", 0);
    av_dict_set(&opts, "analyzeduration", "500000", 0);

    int ret = avformat_open_input(&fmt_ctx, NULL, input_fmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        avio_context_free(&avio_ctx);
        return false;
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
            ctx->video_stream_idx = i;
            ctx->video_width = par->width;
            ctx->video_height = par->height;
            ctx->video_extradata = par->extradata;
            ctx->video_extradata_size = par->extradata_size;
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
