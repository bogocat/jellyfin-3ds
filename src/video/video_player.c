/**
 * video_player.c — Video streaming player orchestrator
 *
 * Coordinates: network download → FFmpeg demux → MVD decode → display
 *              + FFmpeg AAC decode → swr_convert → NDSP audio
 *
 * Threading: network thread fills ring buffer, decode thread demuxes
 * and feeds both MVD (video) and NDSP (audio), main thread renders.
 */

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"
#include <malloc.h>
#include <curl/curl.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#include "video/video_player.h"
#include "video/ffmpeg_demux.h"
#include "video/mvd_decode.h"

/* ── Ring buffer for network data ──────────────────────────────────── */

#define RING_SIZE       (512 * 1024)  /* 512KB */
#define PREFETCH_BYTES  (32 * 1024)   /* 32KB before starting decode */
#define AUDIO_BUF_SIZE  4096          /* PCM samples per NDSP buffer */
#define NUM_AUDIO_BUFS  4

/* ── Video frame queue (decode → convert thread) ───────────────────── */

#define FRAME_QUEUE_SIZE 6  /* more headroom between decode and convert */

typedef struct {
    u8     *data;          /* BGR565 pixel data (allocated once) */
    double  pts;           /* presentation timestamp (seconds) */
    int     width, height; /* frame dimensions */
    bool    valid;
} queued_frame_t;

typedef struct {
    queued_frame_t frames[FRAME_QUEUE_SIZE];
    int            write_idx;
    int            read_idx;
    volatile int   count;  /* number of frames available to read */
    LightLock      lock;
} frame_queue_t;

/* ── Player state ──────────────────────────────────────────────────── */

static struct {
    /* Threads: network, decode, convert */
    Thread          net_thread;
    Thread          decode_thread;
    Thread          convert_thread;
    volatile bool   stop_requested;

    /* State */
    video_state_t   state;
    LightLock       state_lock;
    char            error_msg[128];

    /* Stream info */
    char            url[2048];
    int64_t         duration_ticks;
    int64_t         seek_offset_ticks; /* added to stream PTS for correct position after seek */
    volatile int64_t position_ticks;

    /* Demuxer */
    demux_ctx_t     demux;

    /* Video decode */
    mvd_ctx_t       mvd;
    bool            first_frame;

    /* Audio decode */
    AVCodecContext  *audio_dec_ctx;
    bool            audio_swr_ready;
    SwrContext      *swr_ctx;
    int             audio_sample_rate;

    /* A/V sync — audio is the master clock */
    double          audio_buf_pts[NUM_AUDIO_BUFS];
    volatile bool   audio_playing;

    /* NDSP audio output */
    int             ndsp_channel;
    ndspWaveBuf     wave_bufs[NUM_AUDIO_BUFS];
    s16            *pcm_bufs[NUM_AUDIO_BUFS];
    int             audio_buf_idx;

    /* Frame queue: decode thread → convert thread */
    frame_queue_t   fq;

    /* Frame display: convert thread → main thread */
    C3D_Tex         frame_tex[2];     /* double-buffered textures (left eye / 2D) */
    C2D_Image       frame_img;
    bool            tex_initialized;
    int             tex_write_idx;    /* convert writes to this */
    volatile int    tex_display_idx;  /* main thread displays this */
    volatile bool   new_tex_ready;
    LightLock       tex_lock;

    /* Stereoscopic 3D (SBS) */
    bool            is_3d;             /* mode_3d != VP_3D_NONE */
    vp_3d_mode_t    mode_3d;

    /* Offline cache: url is an sdmc:/ path, played without the network
     * thread or ring buffer (FFmpeg opens the file directly, seekable) */
    bool            is_local;
    C3D_Tex         frame_tex_right[2]; /* right eye double-buffered (3D only) */
    C2D_Image       frame_img_right;

    /* Display dimensions */
    int             display_width;
    int             display_height;

    /* Client-side subtitle track (main thread only) */
    vp_subtitle_t  *subtitle_entries;
    int             subtitle_count;

    /* Diagnostics */
    volatile int    frames_decoded;
    volatile int    frames_displayed;
    u64             last_fps_tick;
    float           decode_fps;
    float           display_fps;
} s_vp;

/* Morton tiling offset tables — declared early, used by convert thread */
static int s_inc_x[1024];
static int s_inc_y[1024];
static bool s_inc_tables_built = false;
static inline void tile_row_morton(u8 *tex, int dst_row,
                                   const u8 *src, int pixel_w);
static Tex3DS_SubTexture s_subtex;

/* ── Frame queue operations ────────────────────────────────────────── */

static void fq_init(frame_queue_t *fq, int frame_w, int frame_h)
{
    LightLock_Init(&fq->lock);
    fq->write_idx = 0;
    fq->read_idx = 0;
    fq->count = 0;
    int frame_size = frame_w * frame_h * 2; /* BGR565 */
    for (int i = 0; i < FRAME_QUEUE_SIZE; i++) {
        fq->frames[i].data = linearAlloc(frame_size);
        fq->frames[i].valid = false;
    }
}

static void fq_cleanup(frame_queue_t *fq)
{
    for (int i = 0; i < FRAME_QUEUE_SIZE; i++) {
        if (fq->frames[i].data) {
            linearFree(fq->frames[i].data);
            fq->frames[i].data = NULL;
        }
    }
    fq->count = 0;
}

/* Push a decoded frame. If queue is full, drop the oldest. */
static bool fq_push(frame_queue_t *fq, const u8 *data, int w, int h, double pts)
{
    LightLock_Lock(&fq->lock);

    if (fq->count >= FRAME_QUEUE_SIZE) {
        /* Drop oldest frame */
        fq->read_idx = (fq->read_idx + 1) % FRAME_QUEUE_SIZE;
        fq->count--;
    }

    queued_frame_t *f = &fq->frames[fq->write_idx];
    if (f->data) {
        memcpy(f->data, data, w * h * 2);
        f->pts = pts;
        f->width = w;
        f->height = h;
        f->valid = true;
        fq->write_idx = (fq->write_idx + 1) % FRAME_QUEUE_SIZE;
        fq->count++;
    }

    LightLock_Unlock(&fq->lock);
    return true;
}

/* Peek at the next frame's PTS without consuming it. Returns -1 if empty. */
static double fq_peek_pts(frame_queue_t *fq)
{
    LightLock_Lock(&fq->lock);
    double pts = -1.0;
    if (fq->count > 0)
        pts = fq->frames[fq->read_idx].pts;
    LightLock_Unlock(&fq->lock);
    return pts;
}

/* Pop the next frame. Returns NULL if empty. */
static queued_frame_t *fq_pop(frame_queue_t *fq)
{
    LightLock_Lock(&fq->lock);
    if (fq->count <= 0) {
        LightLock_Unlock(&fq->lock);
        return NULL;
    }
    queued_frame_t *f = &fq->frames[fq->read_idx];
    fq->read_idx = (fq->read_idx + 1) % FRAME_QUEUE_SIZE;
    fq->count--;
    LightLock_Unlock(&fq->lock);
    return f;
}

/* ── Audio clock (for A/V sync) ────────────────────────────────────── */

/**
 * Get current audio playback position in seconds.
 * Returns -1.0 if audio is not playing (video should display immediately).
 * Same approach as ThirdTube/FourthTube's Util_speaker_get_current_timestamp.
 */
static double get_audio_clock(void)
{
    if (!s_vp.audio_playing || s_vp.audio_sample_rate <= 0)
        return -1.0;

    /* Find the currently playing buffer */
    for (int i = 0; i < NUM_AUDIO_BUFS; i++) {
        if (s_vp.wave_bufs[i].status == NDSP_WBUF_PLAYING) {
            double sample_offset = (double)ndspChnGetSamplePos(s_vp.ndsp_channel)
                                 / (double)s_vp.audio_sample_rate;
            return s_vp.audio_buf_pts[i] + sample_offset;
        }
    }

    /* No buffer playing — check if any are queued */
    double min_queued = 1e30;
    for (int i = 0; i < NUM_AUDIO_BUFS; i++) {
        if (s_vp.wave_bufs[i].status == NDSP_WBUF_QUEUED) {
            if (s_vp.audio_buf_pts[i] < min_queued)
                min_queued = s_vp.audio_buf_pts[i];
        }
    }
    if (min_queued < 1e29)
        return min_queued;

    return -1.0;
}

/* ── Network thread ────────────────────────────────────────────────── */

static int64_t s_net_bytes_rx; /* net thread only — gates retry safety */

static size_t net_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    demux_ctx_t *demux = (demux_ctx_t *)userdata;
    size_t total = size * nmemb;

    if (s_vp.stop_requested) return 0;
    s_net_bytes_rx += (int64_t)total;

    size_t written = 0;
    while (written < total && !s_vp.stop_requested) {
        int fill = __atomic_load_n(&demux->ring_fill, __ATOMIC_ACQUIRE);
        int space = demux->ring_size - fill;
        int chunk = (int)((total - written) < (size_t)space ? (total - written) : (size_t)space);

        if (chunk <= 0) {
            svcSleepThread(1000000LL); /* 1ms */
            continue;
        }

        for (int i = 0; i < chunk; i++) {
            demux->ring_data[demux->ring_write_pos] = ((uint8_t *)ptr)[written + i];
            demux->ring_write_pos = (demux->ring_write_pos + 1) % demux->ring_size;
        }
        __atomic_fetch_add(&demux->ring_fill, chunk, __ATOMIC_RELEASE);
        written += chunk;
    }

    return written;
}

static void net_thread_func(void *arg)
{
    (void)arg;
    log_write("NET: thread started");
    CURL *curl = curl_easy_init();
    if (!curl) {
        log_write("NET: curl_easy_init failed");
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg), "curl init failed");
        s_vp.state = VIDEO_ERROR;
        return;
    }
    log_write("NET: fetching URL (len=%d)", (int)strlen(s_vp.url));

    curl_easy_setopt(curl, CURLOPT_URL, s_vp.url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, net_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s_vp.demux);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 65536L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Jellyfin-3DS/" JFIN_VERSION);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L); /* server may need time to start transcoding */

    /* Retry on transient network failures (WiFi drops, peer resets) */
    #define NET_MAX_RETRIES 3
    CURLcode res = CURLE_OK;
    long http_code = 0;

    for (int attempt = 0; attempt <= NET_MAX_RETRIES && !s_vp.stop_requested; attempt++) {
        if (attempt > 0) {
            log_write("NET: retry %d/%d after transient failure", attempt, NET_MAX_RETRIES);
            svcSleepThread(2000000000LL); /* 2s backoff */
        }

        res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        log_write("NET: curl done, result=%d (%s), http=%ld, ring_fill=%d attempt=%d",
                  res, curl_easy_strerror(res), http_code,
                  __atomic_load_n(&s_vp.demux.ring_fill, __ATOMIC_ACQUIRE), attempt);

        /* Success or non-retryable: stop */
        if (res == CURLE_OK || res == CURLE_WRITE_ERROR /* stop_requested */)
            break;

        /* A retry restarts the transfer from byte 0 (Jellyfin's transcode
         * stream has no usable Range resume). If any bytes already reached
         * the ring the demuxer has consumed part of the stream, and a
         * restart would splice duplicate TS data into it — corruption.
         * Only retry failures that happened before first byte. */
        if (s_net_bytes_rx > 0) {
            log_write("NET: mid-stream failure after %lld bytes — not retrying",
                      (long long)s_net_bytes_rx);
            break;
        }

        /* Retryable: connection lost, timeout, recv error */
        if (res != CURLE_RECV_ERROR && res != CURLE_OPERATION_TIMEDOUT
            && res != CURLE_COULDNT_CONNECT && res != CURLE_GOT_NOTHING)
            break;
    }

    if (res != CURLE_OK && res != CURLE_WRITE_ERROR && !s_vp.stop_requested) {
        if (http_code > 0)
            snprintf(s_vp.error_msg, sizeof(s_vp.error_msg),
                     "HTTP %ld: %s", http_code, curl_easy_strerror(res));
        else
            snprintf(s_vp.error_msg, sizeof(s_vp.error_msg),
                     "Network error: %s", curl_easy_strerror(res));
        s_vp.state = VIDEO_ERROR;
    }

    __atomic_store_n(&s_vp.demux.ring_finished, true, __ATOMIC_RELEASE);
    curl_easy_cleanup(curl);
}

/* ── Audio decode + output ─────────────────────────────────────────── */

static bool init_audio_decoder(demux_ctx_t *demux)
{
    if (demux->audio_stream_idx < 0) {
        log_write("AUDIO: no audio stream found");
        return false;
    }

    AVFormatContext *fmt = (AVFormatContext *)demux->fmt_ctx;
    AVCodecParameters *par = fmt->streams[demux->audio_stream_idx]->codecpar;

    log_write("AUDIO: codec_id=%d sample_rate=%d channels=%d",
              par->codec_id, par->sample_rate, par->channels);

    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        log_write("AUDIO: avcodec_find_decoder FAILED for codec_id=%d", par->codec_id);
        return false;
    }
    log_write("AUDIO: found decoder '%s'", codec->name);

    s_vp.audio_dec_ctx = avcodec_alloc_context3(codec);
    if (!s_vp.audio_dec_ctx) {
        log_write("AUDIO: avcodec_alloc_context3 FAILED");
        return false;
    }

    avcodec_parameters_to_context(s_vp.audio_dec_ctx, par);

    /* TS demuxer may report 0 for sample_rate/channels until first decode.
     * Default to Jellyfin's transcode output: 48kHz stereo AAC. */
    if (s_vp.audio_dec_ctx->sample_rate == 0)
        s_vp.audio_dec_ctx->sample_rate = 48000;
    if (s_vp.audio_dec_ctx->channels == 0) {
        s_vp.audio_dec_ctx->channels = 2;
        s_vp.audio_dec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
    }

    int open_ret = avcodec_open2(s_vp.audio_dec_ctx, codec, NULL);
    if (open_ret < 0) {
        log_write("AUDIO: avcodec_open2 FAILED ret=%d", open_ret);
        avcodec_free_context(&s_vp.audio_dec_ctx);
        return false;
    }
    log_write("AUDIO: decoder opened (swr deferred to first frame)");

    /* swr init is deferred to decode_audio_packet — the actual sample format
     * isn't known until after the first frame is decoded (TS quirk). */
    s_vp.audio_swr_ready = false;
    s_vp.audio_sample_rate = 48000; /* default, updated on first frame */

    /* NDSP channel for video audio */
    s_vp.ndsp_channel = 1; /* channel 0 is used by audio player */
    ndspChnSetInterp(s_vp.ndsp_channel, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(s_vp.ndsp_channel, (float)s_vp.audio_sample_rate);
    ndspChnSetFormat(s_vp.ndsp_channel, NDSP_FORMAT_STEREO_PCM16);

    float mix[12] = {0};
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(s_vp.ndsp_channel, mix);

    /* Allocate PCM buffers in linear memory */
    for (int i = 0; i < NUM_AUDIO_BUFS; i++) {
        s_vp.pcm_bufs[i] = linearAlloc(AUDIO_BUF_SIZE * sizeof(s16) * 2);
        if (!s_vp.pcm_bufs[i]) return false;
        memset(&s_vp.wave_bufs[i], 0, sizeof(ndspWaveBuf));
        s_vp.wave_bufs[i].status = NDSP_WBUF_DONE;
    }

    return true;
}

static void decode_audio_packet(AVPacket *pkt)
{
    if (!s_vp.audio_dec_ctx) return;

    AVFrame *frame = av_frame_alloc();
    if (!frame) return;

    int ret = avcodec_send_packet(s_vp.audio_dec_ctx, pkt);
    if (ret < 0) { av_frame_free(&frame); return; }

    while (avcodec_receive_frame(s_vp.audio_dec_ctx, frame) >= 0) {
        /* Deferred swr init: now we know the real format from the decoded frame */
        if (!s_vp.audio_swr_ready) {
            if (s_vp.swr_ctx) swr_free(&s_vp.swr_ctx);
            s_vp.swr_ctx = swr_alloc();
            if (!s_vp.swr_ctx) break;

            int64_t in_layout = frame->channel_layout;
            if (!in_layout)
                in_layout = av_get_default_channel_layout(frame->channels);
            int in_rate = frame->sample_rate ? frame->sample_rate : 48000;

            av_opt_set_channel_layout(s_vp.swr_ctx, "in_channel_layout", in_layout, 0);
            av_opt_set_int(s_vp.swr_ctx, "in_sample_rate", in_rate, 0);
            av_opt_set_sample_fmt(s_vp.swr_ctx, "in_sample_fmt", frame->format, 0);

            av_opt_set_channel_layout(s_vp.swr_ctx, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
            av_opt_set_int(s_vp.swr_ctx, "out_sample_rate", in_rate, 0);
            av_opt_set_sample_fmt(s_vp.swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

            if (swr_init(s_vp.swr_ctx) < 0) {
                log_write("AUDIO: swr_init FAILED (deferred) fmt=%d rate=%d ch=%d",
                          frame->format, in_rate, frame->channels);
                swr_free(&s_vp.swr_ctx);
                break;
            }

            s_vp.audio_sample_rate = in_rate;
            ndspChnSetRate(s_vp.ndsp_channel, (float)in_rate);
            s_vp.audio_swr_ready = true;
            log_write("AUDIO: swr init OK (deferred) fmt=%d rate=%d ch=%d",
                      frame->format, in_rate, frame->channels);
        }

        /* Find a free NDSP buffer */
        ndspWaveBuf *wbuf = &s_vp.wave_bufs[s_vp.audio_buf_idx];

        /* Wait for buffer to be free. No timeout here: while paused this
         * legitimately takes arbitrarily long, and proceeding anyway would
         * resubmit a buffer NDSP still owns (queue corruption). Stop breaks
         * the wait. */
        while ((wbuf->status == NDSP_WBUF_QUEUED || wbuf->status == NDSP_WBUF_PLAYING)
               && !s_vp.stop_requested) {
            svcSleepThread(1000000LL);
        }
        if (s_vp.stop_requested) break;

        /* Convert to s16 stereo */
        int out_samples = swr_convert(s_vp.swr_ctx,
            (uint8_t **)&s_vp.pcm_bufs[s_vp.audio_buf_idx], AUDIO_BUF_SIZE,
            (const uint8_t **)frame->extended_data, frame->nb_samples);

        if (out_samples > 0) {
            /* Store PTS for A/V sync before queuing */
            double frame_pts = -1.0;
            if (frame->pts != AV_NOPTS_VALUE && s_vp.demux.fmt_ctx) {
                AVFormatContext *fmt = (AVFormatContext *)s_vp.demux.fmt_ctx;
                AVRational tb = fmt->streams[s_vp.demux.audio_stream_idx]->time_base;
                frame_pts = frame->pts * (double)tb.num / (double)tb.den;
            }
            s_vp.audio_buf_pts[s_vp.audio_buf_idx] = frame_pts;

            wbuf->data_vaddr = s_vp.pcm_bufs[s_vp.audio_buf_idx];
            wbuf->nsamples = out_samples;
            DSP_FlushDataCache(s_vp.pcm_bufs[s_vp.audio_buf_idx],
                               out_samples * sizeof(s16) * 2);
            ndspChnWaveBufAdd(s_vp.ndsp_channel, wbuf);
            s_vp.audio_playing = true;

            s_vp.audio_buf_idx = (s_vp.audio_buf_idx + 1) % NUM_AUDIO_BUFS;
        }
    }

    av_frame_free(&frame);
}

/* ── Decode thread ─────────────────────────────────────────────────── */

static void decode_thread_func(void *arg)
{
    (void)arg;

    if (!s_vp.is_local) {
        log_write("DEC: thread started, waiting for prefetch (%d bytes)", PREFETCH_BYTES);

        /* Wait for prefetch */
        while (!s_vp.stop_requested) {
            if (__atomic_load_n(&s_vp.demux.ring_fill, __ATOMIC_ACQUIRE) >= PREFETCH_BYTES
                || __atomic_load_n(&s_vp.demux.ring_finished, __ATOMIC_ACQUIRE))
                break;
            svcSleepThread(10000000LL); /* 10ms */
        }
        if (s_vp.stop_requested) { log_write("DEC: stop during prefetch"); return; }

        log_write("DEC: prefetch done, fill=%d finished=%d",
                  __atomic_load_n(&s_vp.demux.ring_fill, __ATOMIC_ACQUIRE),
                  __atomic_load_n(&s_vp.demux.ring_finished, __ATOMIC_ACQUIRE));
    } else {
        log_write("DEC: thread started (local file, no prefetch)");
    }

    /* Init demuxer */
    if (!demux_init(&s_vp.demux)) {
        log_write("DEC: demux_init FAILED");
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg), "Demux init failed");
        s_vp.state = VIDEO_ERROR;
        return;
    }

    /* Local files seek in-place instead of restarting the transcode.
     * After a successful seek the file's own PTS already reflect the
     * position, so the additive seek offset must be zeroed. */
    if (s_vp.is_local && s_vp.seek_offset_ticks > 0) {
        if (demux_seek(&s_vp.demux, s_vp.seek_offset_ticks)) {
            log_write("DEC: local seek to %lld ticks OK",
                      (long long)s_vp.seek_offset_ticks);
            s_vp.seek_offset_ticks = 0;
        } else {
            log_write("DEC: local seek FAILED, playing from start");
            s_vp.seek_offset_ticks = 0;
            s_vp.position_ticks = 0;
        }
    }
    log_write("DEC: demux OK video=%dx%d vidx=%d aidx=%d",
              s_vp.demux.video_width, s_vp.demux.video_height,
              s_vp.demux.video_stream_idx, s_vp.demux.audio_stream_idx);

    /* Init MVD */
    if (!mvd_init(&s_vp.mvd, s_vp.demux.video_width, s_vp.demux.video_height)) {
        log_write("DEC: mvd_init FAILED");
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg), "MVD init failed (New 3DS?)");
        s_vp.state = VIDEO_ERROR;
        demux_cleanup(&s_vp.demux);
        return;
    }
    log_write("DEC: MVD OK %dx%d", s_vp.mvd.width, s_vp.mvd.height);

    if (s_vp.is_3d) {
        /* SBS: each eye sees half the frame width */
        s_vp.display_width = s_vp.demux.video_width / 2;
        s_vp.display_height = s_vp.demux.video_height;
    } else {
        s_vp.display_width = s_vp.demux.video_width;
        s_vp.display_height = s_vp.demux.video_height;
    }

    /* Init frame queue between decode and convert threads */
    fq_init(&s_vp.fq, s_vp.mvd.width, s_vp.mvd.height);

    /* Send SPS/PPS (only for AVCC/MP4 — TS has them inline) */
    if (s_vp.demux.video_extradata && s_vp.demux.video_extradata_size > 0) {
        log_write("DEC: sending SPS/PPS from extradata (%d bytes)", s_vp.demux.video_extradata_size);
        mvd_send_sps_pps(&s_vp.mvd, s_vp.demux.video_extradata,
                          s_vp.demux.video_extradata_size);
    } else {
        log_write("DEC: no extradata — TS stream, SPS/PPS inline in NAL units");
        s_vp.mvd.sps_sent = true; /* skip the gate in decode_packet */
    }

    /* Init audio decoder */
    init_audio_decoder(&s_vp.demux);

    LightLock_Lock(&s_vp.state_lock);
    s_vp.state = VIDEO_PLAYING;
    LightLock_Unlock(&s_vp.state_lock);

    s_vp.first_frame = true;

    /* Main decode loop */
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return;

    bool first_video_pkt = true;
    bool got_video = false;
    int  audio_skipped = 0;
    while (!s_vp.stop_requested) {
        bool is_video = false;
        int ret = demux_read_packet(&s_vp.demux, pkt, &is_video);
        if (ret < 0) break; /* EOF or error */

        if (is_video) {
            if (first_video_pkt) {
                first_video_pkt = false;
                /* Jellyfin sends audio-only pre-roll before the first video IDR
                 * while the server-side filter chain initialises. Subtract the
                 * first video PTS from seek_offset so position_ticks doesn't
                 * double-count the seek, keeping subtitle timestamps in sync. */
                if (audio_skipped > 0 && pkt->pts != AV_NOPTS_VALUE) {
                    AVFormatContext *_fmt = (AVFormatContext *)s_vp.demux.fmt_ctx;
                    AVRational _tb = _fmt->streams[s_vp.demux.video_stream_idx]->time_base;
                    double first_video_pts = pkt->pts * (double)_tb.num / (double)_tb.den;
                    s_vp.seek_offset_ticks -= (int64_t)(first_video_pts * 10000000.0);
                }
            }
            got_video = true;
            bool got_frame = mvd_decode_packet(&s_vp.mvd, pkt->data, pkt->size);

            /* Compute video PTS in seconds */
            double video_pts = -1.0;
            if (pkt->pts != AV_NOPTS_VALUE) {
                AVFormatContext *fmt = (AVFormatContext *)s_vp.demux.fmt_ctx;
                AVRational tb = fmt->streams[s_vp.demux.video_stream_idx]->time_base;
                video_pts = pkt->pts * (double)tb.num / (double)tb.den;
                s_vp.position_ticks = s_vp.seek_offset_ticks + (int64_t)(video_pts * 10000000.0);
            }

            if (got_frame) {
                if (s_vp.first_frame) {
                    s_vp.first_frame = false;
                } else {
                    /* Push frame to queue for convert thread.
                     * If queue is full, oldest frame is dropped. */
                    u8 *frame_data = mvd_get_frame(&s_vp.mvd);
                    if (frame_data) {
                        fq_push(&s_vp.fq, frame_data,
                                s_vp.mvd.width, s_vp.mvd.height, video_pts);
                        s_vp.frames_decoded++;
                    }
                }
            }
        } else {
            if (got_video) {
                decode_audio_packet(pkt);
            } else {
                audio_skipped++;
            }
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);

    if (!s_vp.stop_requested) {
        LightLock_Lock(&s_vp.state_lock);
        s_vp.state = VIDEO_STOPPED;
        LightLock_Unlock(&s_vp.state_lock);
    }

    /* Frame queue is freed in video_player_stop() after the convert
     * thread is joined — freeing it here would yank buffers out from
     * under a convert thread that is still tiling a popped frame. */
}

/* ── Convert thread (Morton tiling + A/V sync) ─────────────────────── */

static void convert_thread_func(void *arg)
{
    (void)arg;
    log_write("CONV: thread started");

    /* A/V sync thresholds — wider margins reduce sleep churn */
    const double ahead_thr  = s_vp.is_3d ? 0.025 : 0.020;
    const double behind_thr = s_vp.is_3d ? 0.250 : 0.150;

    while (!s_vp.stop_requested) {
        /* Wait for a frame in the queue */
        double next_pts = fq_peek_pts(&s_vp.fq);
        if (next_pts < 0) {
            svcSleepThread(2000000LL); /* 2ms — no frame yet */
            continue;
        }

        /* A/V sync: wait for audio to catch up (ThirdTube approach).
         * This sleep doesn't block audio because audio is decoded
         * in the decode thread, not here. */
        double audio_pos = get_audio_clock();
        if (audio_pos >= 0 && next_pts - audio_pos > ahead_thr) {
            double sleep_sec = next_pts - audio_pos - (ahead_thr * 0.5);
            if (sleep_sec > 0.1) sleep_sec = 0.1;
            if (sleep_sec > 0)
                svcSleepThread((s64)(sleep_sec * 1000000000.0));
            continue; /* re-check after sleep */
        }

        /* If video is behind audio, batch-skip to latest frame.
         * Draining the queue in one lock avoids the peek→drop→peek
         * churn that causes visible stuttering on 3D.
         * Fall through to tile the surviving frame immediately. */
        if (audio_pos >= 0 && audio_pos - next_pts > behind_thr) {
            LightLock_Lock(&s_vp.fq.lock);
            while (s_vp.fq.count > 1) {
                s_vp.fq.read_idx = (s_vp.fq.read_idx + 1) % FRAME_QUEUE_SIZE;
                s_vp.fq.count--;
            }
            LightLock_Unlock(&s_vp.fq.lock);
            /* fall through to pop+tile the surviving frame */
        }

        /* Pop the frame and tile it into a texture */
        queued_frame_t *f = fq_pop(&s_vp.fq);
        if (!f || !f->data) continue;

        /* Morton tiling into the write texture.
         * tex_initialized is published with release semantics after the
         * main thread finishes init_frame_texture(); acquire here makes
         * the texture/index/table writes visible. After that point
         * tex_write_idx is only ever modified by this thread. */
        if (!__atomic_load_n(&s_vp.tex_initialized, __ATOMIC_ACQUIRE))
            continue;
        int write_idx = s_vp.tex_write_idx;
        if (s_vp.frame_tex[write_idx].data) {
            if (s_vp.is_3d) {
                /* SBS 3D: single-pass interleaved tiling.
                 * Tile both halves per row so each source row is only
                 * fetched once from memory (hot in L1 data cache). */
                int half_w = f->width / 2;
                int row_bytes = f->width * 2;
                u8 *tex_left = (u8 *)s_vp.frame_tex[write_idx].data;
                u8 *tex_right = (u8 *)s_vp.frame_tex_right[write_idx].data;

                int dst_row = 0, y_count = 0;
                for (int y = 0; y < f->height; y++) {
                    const u8 *row = f->data + y * row_bytes;

                    /* Prefetch next source row into L1 cache */
                    if (y + 1 < f->height)
                        __builtin_prefetch(row + row_bytes, 0, 1);

                    /* Left half then right half — same row, one cache fetch */
                    tile_row_morton(tex_left, dst_row, row, half_w);
                    tile_row_morton(tex_right, dst_row, row + half_w * 2, half_w);

                    dst_row += s_inc_y[y_count++];
                }
                C3D_TexFlush(&s_vp.frame_tex[write_idx]);
                C3D_TexFlush(&s_vp.frame_tex_right[write_idx]);
            } else {
                /* 2D: tile full frame with unrolled inner loop */
                u8 *tex_data = (u8 *)s_vp.frame_tex[write_idx].data;

                int dst_row = 0, y_count = 0;
                for (int y = 0; y < f->height; y++) {
                    const u8 *row = f->data + y * f->width * 2;
                    tile_row_morton(tex_data, dst_row, row, f->width);
                    dst_row += s_inc_y[y_count++];
                }
                C3D_TexFlush(&s_vp.frame_tex[write_idx]);
            }

            /* Swap: tell main thread the new texture(s) are ready */
            LightLock_Lock(&s_vp.tex_lock);
            s_vp.tex_display_idx = write_idx;
            s_vp.tex_write_idx = write_idx ^ 1;
            s_vp.new_tex_ready = true;
            s_vp.frames_displayed++;
            LightLock_Unlock(&s_vp.tex_lock);
        }
    }
    log_write("CONV: thread exiting");
}

/* ── GPU texture setup ─────────────────────────────────────────────── */

/* Morton tiling offset tables (built once, used by convert thread) */
static void build_offset_tables(int tex_w, int tex_h);

/* Static subtex so it persists (compound literals on stack would dangle) */

static void init_frame_texture(int width, int height)
{
    int tex_w = 512;
    int tex_h = (height > 256) ? 512 : 256; /* 512 for 3D (tall per-eye frames) */

    /* Double-buffered textures: convert thread writes one while GPU reads the other */
    for (int i = 0; i < 2; i++) {
        if (!C3D_TexInit(&s_vp.frame_tex[i], tex_w, tex_h, GPU_RGB565)) {
            log_write("TEX: C3D_TexInit FAILED tex[%d] %dx%d", i, tex_w, tex_h);
            return;
        }
        C3D_TexSetFilter(&s_vp.frame_tex[i], GPU_LINEAR, GPU_LINEAR);
    }

    /* Right eye textures for stereoscopic 3D */
    if (s_vp.is_3d) {
        for (int i = 0; i < 2; i++) {
            if (!C3D_TexInit(&s_vp.frame_tex_right[i], tex_w, tex_h, GPU_RGB565)) {
                log_write("TEX: C3D_TexInit FAILED tex_right[%d] %dx%d", i, tex_w, tex_h);
                C3D_TexDelete(&s_vp.frame_tex[0]);
                C3D_TexDelete(&s_vp.frame_tex[1]);
                if (i == 1) C3D_TexDelete(&s_vp.frame_tex_right[0]);
                return;
            }
            C3D_TexSetFilter(&s_vp.frame_tex_right[i], GPU_LINEAR, GPU_LINEAR);
        }
    }

    s_subtex.width = (u16)width;
    s_subtex.height = (u16)height;
    s_subtex.left = 0.0f;
    s_subtex.top = 1.0f;
    s_subtex.right = (float)width / tex_w;
    s_subtex.bottom = 1.0f - ((float)height / tex_h);

    s_vp.tex_write_idx = 0;
    s_vp.tex_display_idx = 1;

    /* Build offset tables for Morton tiling — must happen before
     * tex_initialized is published or the convert thread can tile
     * with all-zero offset tables. 512x512 covers both texture sizes. */
    if (!s_inc_tables_built)
        build_offset_tables(512, 512);

    /* tex_lock is initialized once in video_player_init(); re-initializing
     * it here could corrupt a lock the convert thread already holds.
     * Publish tex_initialized last (release) — the convert thread gates on
     * it (acquire) before touching any of the state set up above. */
    __atomic_store_n(&s_vp.tex_initialized, true, __ATOMIC_RELEASE);

    log_write("TEX: init OK %dx%d in %dx%d tex%s", width, height, tex_w, tex_h,
              s_vp.is_3d ? " (3D L+R)" : " (double buffered)");
}

/* Precomputed-table Morton tiling (CPU, same approach as ThirdTube).
 * Faster than GX_DisplayTransfer which serializes with GPU pipeline. */


static void build_offset_tables(int tex_w, int tex_h)
{
    const int ps = 2;
    for (int i = 0; i + 3 < tex_w; i += 4) {
        s_inc_x[i]     = 4 * ps;
        s_inc_x[i + 1] = 12 * ps;
        s_inc_x[i + 2] = 4 * ps;
        s_inc_x[i + 3] = 44 * ps;
    }
    for (int i = 0; i + 7 < tex_h; i += 8) {
        s_inc_y[i]     = 2 * ps;
        s_inc_y[i + 1] = 6 * ps;
        s_inc_y[i + 2] = 2 * ps;
        s_inc_y[i + 3] = 22 * ps;
        s_inc_y[i + 4] = 2 * ps;
        s_inc_y[i + 5] = 6 * ps;
        s_inc_y[i + 6] = 2 * ps;
        s_inc_y[i + 7] = (tex_w * 8 - 42) * ps;
    }
    s_inc_tables_built = true;
}

/**
 * Tile one row of BGR565 pixels into a Morton-tiled texture.
 * Inner loop is unrolled by 4 pixel-pairs (8 pixels) using hardcoded
 * Morton offsets [+0, +8, +32, +40] with 128-byte group stride.
 * Eliminates s_inc_x[] table lookups in the hot path.
 */
static inline void tile_row_morton(u8 *tex, int dst_row,
                                   const u8 *src, int pixel_w)
{
    int groups = pixel_w >> 3; /* pixel_w / 8 */
    int dst_pos = dst_row;

    for (int g = 0; g < groups; g++) {
        const u32 *sp = (const u32 *)(src + g * 16);
        *(u32 *)(tex + dst_pos)      = sp[0];
        *(u32 *)(tex + dst_pos + 8)  = sp[1];
        *(u32 *)(tex + dst_pos + 32) = sp[2];
        *(u32 *)(tex + dst_pos + 40) = sp[3];
        dst_pos += 128;
    }

    /* Remainder (0-6 pixels) — fall back to table for non-8-aligned widths */
    int x_base = groups * 8;
    int x_count = groups * 4;
    for (int x = x_base; x < pixel_w; x += 2) {
        *(u32 *)(tex + dst_pos) = *(const u32 *)(src + x * 2);
        dst_pos += s_inc_x[x_count++];
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

bool video_player_is_supported(void)
{
    return mvd_is_available();
}

bool video_player_init(void)
{
    memset(&s_vp, 0, sizeof(s_vp));
    LightLock_Init(&s_vp.state_lock);
    LightLock_Init(&s_vp.tex_lock);
    s_vp.state = VIDEO_STOPPED;
    return true;
}

void video_player_cleanup(void)
{
    video_player_stop();
}

/* ── Client-side ASS subtitle loader ───────────────────────────────── */

typedef struct { char *data; size_t size; size_t cap; } ass_buf_t;

static size_t ass_write_cb(void *ptr, size_t sz, size_t nmemb, void *ud)
{
    ass_buf_t *b = (ass_buf_t *)ud;
    size_t n = sz * nmemb;
    if (b->size + n >= b->cap) {
        size_t nc = (b->cap + n) * 2;
        char *nd = realloc(b->data, nc + 1);
        if (!nd) return 0;
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->size, ptr, n);
    b->size += n;
    b->data[b->size] = '\0';
    return n;
}

static int64_t ass_parse_time(const char *s)
{
    int h = 0, m = 0, sec = 0, cs = 0;
    sscanf(s, "%d:%d:%d.%d", &h, &m, &sec, &cs);
    return ((int64_t)h * 3600 + m * 60 + sec) * 10000000LL + cs * 100000LL;
}

static void ass_strip_tags(const char *src, char *dst, int dst_max,
                           float *px, float *py, int *align, uint32_t *color)
{
    int di = 0;
    while (*src && di < dst_max - 1) {
        if (*src == '{') {
            const char *end = strchr(src, '}');
            if (!end) { src++; continue; }
            const char *p = src + 1;
            while (p < end) {
                if (*p == '\\') {
                    p++;
                    if (strncmp(p, "pos(", 4) == 0) {
                        sscanf(p + 4, "%f,%f", px, py);
                    } else if (strncmp(p, "an", 2) == 0 &&
                               p[2] >= '1' && p[2] <= '9') {
                        *align = p[2] - '0';
                    } else if (strncmp(p, "c&H", 3) == 0 ||
                               strncmp(p, "1c&H", 4) == 0) {
                        const char *hstart = (p[0] == '1') ? p + 4 : p + 3;
                        unsigned int bgr = 0;
                        sscanf(hstart, "%x", &bgr);
                        u8 r = bgr & 0xFF;
                        u8 g = (bgr >> 8) & 0xFF;
                        u8 b = (bgr >> 16) & 0xFF;
                        *color = C2D_Color32(r, g, b, 255);
                    } else if (*p == 'N' || *p == 'n') {
                        if (di < dst_max - 1) dst[di++] = '\n';
                    }
                }
                p++;
            }
            src = end + 1;
        } else if (*src == '\\' && (*(src+1) == 'N' || *(src+1) == 'n')) {
            if (di < dst_max - 1) dst[di++] = '\n';
            src += 2;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

static void subtitle_parse_ass(const char *text)
{
    float play_res_x = 640.0f, play_res_y = 480.0f;
    const char *p;

    p = strstr(text, "PlayResX:");
    if (p) play_res_x = (float)atoi(p + 9);
    p = strstr(text, "PlayResY:");
    if (p) play_res_y = (float)atoi(p + 9);

    typedef struct { char name[64]; int align; } style_entry_t;
    style_entry_t styles[32];
    int style_count = 0;
    int align_col = 18;

    const char *fmt = strstr(text, "\nFormat:");
    if (fmt) {
        fmt += 8;
        int col = 0;
        const char *fp = fmt;
        while (*fp && *fp != '\n') {
            while (*fp == ' ') fp++;
            if (strncmp(fp, "Alignment", 9) == 0 &&
                (fp[9] == ',' || fp[9] == '\n' || fp[9] == '\r' || fp[9] == ' '))
                align_col = col;
            col++;
            while (*fp && *fp != ',' && *fp != '\n') fp++;
            if (*fp == ',') fp++;
        }
    }
    p = text;
    while ((p = strstr(p, "\nStyle:")) != NULL && style_count < 32) {
        p += 7;
        while (*p == ' ') p++;
        const char *eol = strchr(p, '\n');
        const char *comma = strchr(p, ',');
        if (!comma || (eol && comma > eol)) { p++; continue; }
        int nlen = (int)(comma - p);
        if (nlen >= 64) nlen = 63;
        style_entry_t *se = &styles[style_count];
        memcpy(se->name, p, nlen); se->name[nlen] = '\0';
        se->align = 2;
        const char *sp = comma + 1;
        for (int c = 1; sp && *sp; c++) {
            if (c == align_col) { se->align = atoi(sp); break; }
            sp = strchr(sp, ',');
            if (sp) sp++;
        }
        style_count++;
        p++;
    }

    int count = 0;
    p = text;
    while ((p = strstr(p, "\nDialogue:")) != NULL) { count++; p++; }
    if (count == 0) { log_write("SUB: no Dialogue lines"); return; }
    if (count > 4096) count = 4096;
    s_vp.subtitle_entries = malloc(count * sizeof(vp_subtitle_t));
    if (!s_vp.subtitle_entries) return;
    int idx = 0;
    p = text;
    while ((p = strstr(p, "\nDialogue:")) != NULL && idx < count) {
        p++;
        const char *eol = strchr(p, '\n');
        const char *fp = p + 10;
        char start_ts[16] = {0}, end_ts[16] = {0}, style_name[64] = {0};
        int fi;
        for (fi = 0; fi < 9 && fp && *fp; fi++) {
            const char *comma = strchr(fp, ',');
            if (!comma || (eol && comma > eol)) break;
            int n = (int)(comma - fp);
            if (fi == 1 && n < 16) { memcpy(start_ts,   fp, n); start_ts[n]   = '\0'; }
            if (fi == 2 && n < 16) { memcpy(end_ts,     fp, n); end_ts[n]     = '\0'; }
            if (fi == 3 && n < 64) { memcpy(style_name, fp, n); style_name[n] = '\0'; }
            fp = comma + 1;
        }
        if (fi < 9 || !fp) continue;
        int tlen = eol ? (int)(eol - fp) : (int)strlen(fp);
        while (tlen > 0 && (fp[tlen-1] == '\r' || fp[tlen-1] == '\n')) tlen--;
        char tmp[512];
        if (tlen >= (int)sizeof(tmp)) tlen = sizeof(tmp) - 1;
        memcpy(tmp, fp, tlen);
        tmp[tlen] = '\0';
        vp_subtitle_t *e = &s_vp.subtitle_entries[idx];
        e->start_ticks = ass_parse_time(start_ts);
        e->end_ticks   = ass_parse_time(end_ts);
        e->screen_x    = -1.0f;
        e->screen_y    = -1.0f;
        e->alignment   = 2;
        e->color       = 0;
        for (int si = 0; si < style_count; si++) {
            if (strcmp(styles[si].name, style_name) == 0) {
                e->alignment = styles[si].align;
                break;
            }
        }
        float raw_x = -1.0f, raw_y = -1.0f;
        ass_strip_tags(tmp, e->text, sizeof(e->text), &raw_x, &raw_y,
                       &e->alignment, &e->color);
        if (raw_x >= 0.0f) {
            e->screen_x = raw_x * 400.0f / play_res_x;
            e->screen_y = raw_y * 240.0f / play_res_y;
        }
        if (e->text[0]) idx++;
    }
    s_vp.subtitle_count = idx;
    log_write("SUB: parsed %d cues (PlayRes %.0fx%.0f)", idx, play_res_x, play_res_y);
}

static void subtitle_load(const char *url)
{
    free(s_vp.subtitle_entries);
    s_vp.subtitle_entries = NULL;
    s_vp.subtitle_count = 0;
    if (!url || !url[0]) return;

    /* Local file path (offline downloaded .ass) */
    if (strncmp(url, "sdmc:/", 6) == 0) {
        FILE *fp = fopen(url, "rb");
        if (!fp) { log_write("SUB: local open failed: %s", url); return; }
        fseek(fp, 0, SEEK_END);
        long fsz = ftell(fp);
        rewind(fp);
        if (fsz <= 0 || fsz > 2 * 1024 * 1024) { fclose(fp); return; }
        char *data = malloc((size_t)fsz + 1);
        if (!data) { fclose(fp); return; }
        size_t rd = fread(data, 1, (size_t)fsz, fp);
        fclose(fp);
        data[rd] = '\0';
        log_write("SUB: local read %zu bytes", rd);
        subtitle_parse_ass(data);
        free(data);
        return;
    }

    CURL *curl = curl_easy_init();
    if (!curl) return;
    ass_buf_t buf = {0};
    buf.cap = 65536;
    buf.data = malloc(buf.cap + 1);
    if (!buf.data) { curl_easy_cleanup(curl); return; }
    buf.data[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ass_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Jellyfin-3DS/" JFIN_VERSION);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || !buf.size) {
        log_write("SUB: fetch failed: %s", curl_easy_strerror(res));
        free(buf.data);
        return;
    }
    log_write("SUB: fetched %zu bytes", buf.size);
    subtitle_parse_ass(buf.data);
    free(buf.data);
}

void video_player_clear_subtitles(void)
{
    free(s_vp.subtitle_entries);
    s_vp.subtitle_entries = NULL;
    s_vp.subtitle_count = 0;
}

void video_player_load_subtitles(const char *url)
{
    if (!url || !url[0]) return;
    video_player_clear_subtitles();
    subtitle_load(url);
}

/* ─────────────────────────────────────────────────────────────────── */

bool video_player_play(const char *url, const char *subtitle_url,
                       int64_t duration_ticks,
                       int64_t seek_offset_ticks, vp_3d_mode_t mode_3d)
{
    log_write("PLAY: starting video, url_len=%d seek=%lld 3d=%d",
              (int)strlen(url), (long long)seek_offset_ticks, (int)mode_3d);
    video_player_stop();

    snprintf(s_vp.url, sizeof(s_vp.url), "%s", url);
    s_vp.duration_ticks = duration_ticks;
    s_vp.seek_offset_ticks = seek_offset_ticks;
    s_vp.mode_3d = mode_3d;
    s_vp.is_3d = (mode_3d != VP_3D_NONE);
    s_net_bytes_rx = 0;
    s_vp.position_ticks = seek_offset_ticks;
    s_vp.error_msg[0] = '\0';
    s_vp.stop_requested = false;
    s_vp.state = VIDEO_LOADING;
    s_vp.new_tex_ready = false;
    s_vp.audio_buf_idx = 0;

    /* Anything that isn't http(s) is a cached file on the SD card */
    s_vp.is_local = (strncmp(url, "http", 4) != 0);
    s_vp.demux.local_path = s_vp.is_local ? s_vp.url : NULL;

    if (!s_vp.is_local) {
        /* Allocate ring buffer (network playback only) */
        s_vp.demux.ring_data = malloc(RING_SIZE);
        if (!s_vp.demux.ring_data) return false;
        s_vp.demux.ring_size = RING_SIZE;
        s_vp.demux.ring_write_pos = 0;
        s_vp.demux.ring_read_pos = 0;
        s_vp.demux.ring_fill = 0;
        s_vp.demux.ring_finished = false;
    }

    /* Reset NDSP channel. Set the channel field now rather than in
     * init_audio_decoder so a stop() before audio init doesn't clear
     * channel 0 (the music player's channel). */
    s_vp.ndsp_channel = 1;
    ndspChnReset(1);

    /* Fetch and parse subtitle track before starting threads */
    subtitle_load(subtitle_url);

    /* Launch threads (-1 = any available core) */
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    log_write("PLAY: creating threads, prio=%ld", (long)prio);

    if (!s_vp.is_local)
        s_vp.net_thread = threadCreate(net_thread_func, NULL,
                                        32 * 1024, prio - 1, -1, false);
    s_vp.decode_thread = threadCreate(decode_thread_func, NULL,
                                       64 * 1024, prio - 1, -1, false);
    s_vp.convert_thread = threadCreate(convert_thread_func, NULL,
                                        32 * 1024, prio, -1, false);

    if ((!s_vp.net_thread && !s_vp.is_local) ||
        !s_vp.decode_thread || !s_vp.convert_thread) {
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg),
                 "Thread create failed (net=%p dec=%p)",
                 (void*)s_vp.net_thread, (void*)s_vp.decode_thread);
        /* Clean up any thread that DID start */
        s_vp.stop_requested = true;
        if (s_vp.net_thread) {
            threadJoin(s_vp.net_thread, U64_MAX);
            threadFree(s_vp.net_thread);
            s_vp.net_thread = NULL;
        }
        if (s_vp.decode_thread) {
            threadJoin(s_vp.decode_thread, U64_MAX);
            threadFree(s_vp.decode_thread);
            s_vp.decode_thread = NULL;
        }
        free(s_vp.demux.ring_data);
        s_vp.demux.ring_data = NULL;
        s_vp.state = VIDEO_ERROR;
        return false;
    }

    return true;
}

void video_player_stop(void)
{
    /* Guard on thread handles, not state: the decode thread sets
     * VIDEO_STOPPED at EOF while all three threads are still live. */
    if (!s_vp.net_thread && !s_vp.decode_thread && !s_vp.convert_thread)
        return;

    s_vp.stop_requested = true;

    if (s_vp.net_thread) {
        threadJoin(s_vp.net_thread, U64_MAX);
        threadFree(s_vp.net_thread);
        s_vp.net_thread = NULL;
    }
    if (s_vp.decode_thread) {
        threadJoin(s_vp.decode_thread, U64_MAX);
        threadFree(s_vp.decode_thread);
        s_vp.decode_thread = NULL;
    }
    if (s_vp.convert_thread) {
        threadJoin(s_vp.convert_thread, U64_MAX);
        threadFree(s_vp.convert_thread);
        s_vp.convert_thread = NULL;
    }

    /* All threads joined — now it is safe to free the frame queue */
    fq_cleanup(&s_vp.fq);

    ndspChnWaveBufClear(s_vp.ndsp_channel);

    /* Clean up audio decoder */
    if (s_vp.swr_ctx) {
        swr_free(&s_vp.swr_ctx);
        s_vp.swr_ctx = NULL;
    }
    if (s_vp.audio_dec_ctx) {
        avcodec_free_context(&s_vp.audio_dec_ctx);
        s_vp.audio_dec_ctx = NULL;
    }
    for (int i = 0; i < NUM_AUDIO_BUFS; i++) {
        if (s_vp.pcm_bufs[i]) {
            linearFree(s_vp.pcm_bufs[i]);
            s_vp.pcm_bufs[i] = NULL;
        }
    }

    mvd_cleanup(&s_vp.mvd);
    demux_cleanup(&s_vp.demux);

    if (s_vp.demux.ring_data) {
        free(s_vp.demux.ring_data);
        s_vp.demux.ring_data = NULL;
    }

    if (s_vp.tex_initialized) {
        C3D_TexDelete(&s_vp.frame_tex[0]);
        C3D_TexDelete(&s_vp.frame_tex[1]);
        if (s_vp.is_3d) {
            C3D_TexDelete(&s_vp.frame_tex_right[0]);
            C3D_TexDelete(&s_vp.frame_tex_right[1]);
        }
        /* RELAXED is sufficient (convert thread already joined) but keep
         * every cross-thread write to tex_initialized atomic for consistency */
        __atomic_store_n(&s_vp.tex_initialized, false, __ATOMIC_RELAXED);
    }
    /* Don't let the next playback render a frame from the previous video */
    s_vp.frame_img.tex = NULL;
    s_vp.frame_img_right.tex = NULL;
    s_vp.is_3d = false;
    s_vp.mode_3d = VP_3D_NONE;
    s_vp.is_local = false;
    s_vp.demux.local_path = NULL;

    free(s_vp.subtitle_entries);
    s_vp.subtitle_entries = NULL;
    s_vp.subtitle_count = 0;

    s_vp.state = VIDEO_STOPPED;
}

void video_player_pause(void)
{
    LightLock_Lock(&s_vp.state_lock);
    if (s_vp.state == VIDEO_PLAYING) {
        ndspChnSetPaused(s_vp.ndsp_channel, true);
        s_vp.state = VIDEO_PAUSED;
    } else if (s_vp.state == VIDEO_PAUSED) {
        ndspChnSetPaused(s_vp.ndsp_channel, false);
        s_vp.state = VIDEO_PLAYING;
    }
    LightLock_Unlock(&s_vp.state_lock);
}

video_status_t video_player_get_status(void)
{
    video_status_t st;
    st.state = s_vp.state;
    st.position_ticks = s_vp.position_ticks;
    st.duration_ticks = s_vp.duration_ticks;
    st.buffer_percent = s_vp.is_local ? 100
        : (s_vp.demux.ring_size > 0)
        ? (__atomic_load_n(&s_vp.demux.ring_fill, __ATOMIC_RELAXED) * 100 / s_vp.demux.ring_size) : 0;
    st.video_width = s_vp.display_width;
    st.video_height = s_vp.display_height;
    /* Compute FPS every second */
    u64 now = svcGetSystemTick();
    u64 elapsed = now - s_vp.last_fps_tick;
    if (elapsed > SYSCLOCK_ARM11) { /* 1 second */
        double sec = (double)elapsed / (double)SYSCLOCK_ARM11;
        s_vp.decode_fps = s_vp.frames_decoded / sec;
        s_vp.display_fps = s_vp.frames_displayed / sec;
        s_vp.frames_decoded = 0;
        s_vp.frames_displayed = 0;
        s_vp.last_fps_tick = now;
    }
    st.decode_fps = s_vp.decode_fps;
    st.display_fps = s_vp.display_fps;
    st.frames_decoded = s_vp.frames_decoded;
    st.frames_displayed = s_vp.frames_displayed;
    st.is_3d = s_vp.is_3d;
    snprintf(st.error_msg, sizeof(st.error_msg), "%s", s_vp.error_msg);
    return st;
}

/**
 * Per-eye draw placement for SBS content, fitted to the 400x240 top screen.
 * HSBS eyes are anamorphic half-width, so they get a 2x horizontal stretch
 * to restore aspect; FSBS eyes are already at native aspect (no stretch).
 */
static void compute_3d_draw_rect(float *x, float *y, float *sx, float *sy)
{
    float eye_stretch = (s_vp.mode_3d == VP_3D_HSBS) ? 2.0f : 1.0f;
    float logical_w = s_vp.display_width * eye_stretch;
    float logical_h = (float)s_vp.display_height;
    float scale = 400.0f / logical_w;
    if (logical_h * scale > 240.0f)
        scale = 240.0f / logical_h;
    *sx = eye_stretch * scale;
    *sy = scale;
    *x = (400.0f - s_vp.display_width * (*sx)) / 2.0f;
    *y = (240.0f - s_vp.display_height * (*sy)) / 2.0f;
}

void video_player_render_frame(void)
{
    static int render_log_count = 0;

    if (s_vp.state != VIDEO_PLAYING && s_vp.state != VIDEO_PAUSED)
        return;

    /* Init textures on first call (must be on main thread) */
    if (!s_vp.tex_initialized && s_vp.display_width > 0)
        init_frame_texture(s_vp.display_width, s_vp.display_height);

    /* Check if convert thread prepared a new texture */
    LightLock_Lock(&s_vp.tex_lock);
    if (s_vp.new_tex_ready) {
        /* Point the C2D image at the newly completed texture */
        s_vp.frame_img.tex = &s_vp.frame_tex[s_vp.tex_display_idx];
        s_vp.frame_img.subtex = &s_subtex;
        if (s_vp.is_3d) {
            s_vp.frame_img_right.tex = &s_vp.frame_tex_right[s_vp.tex_display_idx];
            s_vp.frame_img_right.subtex = &s_subtex;
        }
        s_vp.new_tex_ready = false;
        render_log_count++;
        if (render_log_count <= 3)
            log_write("RENDER: displaying tex[%d] frame#%d%s",
                      s_vp.tex_display_idx, render_log_count,
                      s_vp.is_3d ? " (3D)" : "");
    }
    LightLock_Unlock(&s_vp.tex_lock);

    /* Draw the frame texture on the top screen */
    if (s_vp.tex_initialized && s_vp.frame_img.tex) {
        if (s_vp.is_3d) {
            float x, y, sx, sy;
            compute_3d_draw_rect(&x, &y, &sx, &sy);
            C2D_DrawImageAt(s_vp.frame_img, x, y, 0.5f, NULL, sx, sy);
        } else {
            float x = (400 - s_vp.display_width) / 2.0f;
            float y = (240 - s_vp.display_height) / 2.0f;
            C2D_DrawImageAt(s_vp.frame_img, x, y, 0.5f, NULL, 1.0f, 1.0f);
        }
    }
}

void video_player_render_frame_right(void)
{
    if (!s_vp.is_3d) return;
    if (s_vp.state != VIDEO_PLAYING && s_vp.state != VIDEO_PAUSED) return;
    if (!s_vp.tex_initialized || !s_vp.frame_img_right.tex) return;

    float x, y, sx, sy;
    compute_3d_draw_rect(&x, &y, &sx, &sy);
    C2D_DrawImageAt(s_vp.frame_img_right, x, y, 0.5f, NULL, sx, sy);
}

int video_player_get_subtitles(vp_subtitle_t *out, int max_count)
{
    if (!s_vp.subtitle_entries || s_vp.subtitle_count == 0 || max_count <= 0)
        return 0;
    int64_t pos = s_vp.position_ticks;
    int count = 0;
    int lo = 0, hi = s_vp.subtitle_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s_vp.subtitle_entries[mid].start_ticks <= pos)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (int i = lo - 1; i >= 0 && count < max_count; i--) {
        if (s_vp.subtitle_entries[i].end_ticks > pos)
            out[count++] = s_vp.subtitle_entries[i];
        if (pos - s_vp.subtitle_entries[i].start_ticks > 600000000LL)
            break;
    }
    return count;
}
