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

#define FRAME_QUEUE_SIZE 6

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
    C3D_Tex         frame_tex[2];     /* double-buffered textures */
    C2D_Image       frame_img;
    bool            tex_initialized;
    int             tex_write_idx;    /* convert writes to this */
    volatile int    tex_display_idx;  /* main thread displays this */
    volatile bool   new_tex_ready;
    LightLock       tex_lock;

    /* Display dimensions */
    int             display_width;
    int             display_height;

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

static size_t net_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    demux_ctx_t *demux = (demux_ctx_t *)userdata;
    size_t total = size * nmemb;

    if (s_vp.stop_requested) return 0;

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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Jellyfin-3DS/0.1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L); /* server may need time to start transcoding */

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    log_write("NET: curl done, result=%d (%s), http=%ld, ring_fill=%d",
              res, curl_easy_strerror(res), http_code,
              __atomic_load_n(&s_vp.demux.ring_fill, __ATOMIC_ACQUIRE));

    if (res != CURLE_OK && !s_vp.stop_requested) {
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg),
                 "HTTP %ld: %s", http_code, curl_easy_strerror(res));
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

    /* Init demuxer */
    if (!demux_init(&s_vp.demux)) {
        log_write("DEC: demux_init FAILED");
        snprintf(s_vp.error_msg, sizeof(s_vp.error_msg), "Demux init failed");
        s_vp.state = VIDEO_ERROR;
        return;
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

    s_vp.display_width = s_vp.demux.video_width;
    s_vp.display_height = s_vp.demux.video_height;

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

    while (!s_vp.stop_requested) {
        bool is_video = false;
        int ret = demux_read_packet(&s_vp.demux, pkt, &is_video);
        if (ret < 0) break; /* EOF or error */

        if (is_video) {
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
            decode_audio_packet(pkt);
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
        if (audio_pos >= 0 && next_pts - audio_pos > 0.003) {
            double sleep_sec = next_pts - audio_pos - 0.0015;
            if (sleep_sec > 0.1) sleep_sec = 0.1;
            if (sleep_sec > 0)
                svcSleepThread((s64)(sleep_sec * 1000000000.0));
            continue; /* re-check after sleep */
        }

        /* If video is way behind audio, skip frames to catch up */
        if (audio_pos >= 0 && audio_pos - next_pts > 0.1) {
            fq_pop(&s_vp.fq); /* discard late frame */
            continue;
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
            u8 *tex_data = (u8 *)s_vp.frame_tex[write_idx].data;
            int tex_w = s_vp.frame_tex[write_idx].width;

            int dst_row = 0, y_count = 0;
            for (int y = 0; y < f->height; y++) {
                const u8 *row = f->data + y * f->width * 2;
                int dst_pos = dst_row, x_count = 0;
                for (int x = 0; x < f->width; x += 2) {
                    *(u32 *)(tex_data + dst_pos) = *(const u32 *)(row + x * 2);
                    dst_pos += s_inc_x[x_count++];
                }
                dst_row += s_inc_y[y_count++];
            }

            C3D_TexFlush(&s_vp.frame_tex[write_idx]);

            /* Swap: tell main thread the new texture is ready */
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
    int tex_h = 256;

    /* Double-buffered textures: convert thread writes one while GPU reads the other */
    for (int i = 0; i < 2; i++) {
        if (!C3D_TexInit(&s_vp.frame_tex[i], tex_w, tex_h, GPU_RGB565)) {
            log_write("TEX: C3D_TexInit FAILED tex[%d] %dx%d", i, tex_w, tex_h);
            return;
        }
        C3D_TexSetFilter(&s_vp.frame_tex[i], GPU_LINEAR, GPU_LINEAR);
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
     * with all-zero offset tables. */
    if (!s_inc_tables_built)
        build_offset_tables(tex_w, tex_h);

    /* tex_lock is initialized once in video_player_init(); re-initializing
     * it here could corrupt a lock the convert thread already holds.
     * Publish tex_initialized last (release) — the convert thread gates on
     * it (acquire) before touching any of the state set up above. */
    __atomic_store_n(&s_vp.tex_initialized, true, __ATOMIC_RELEASE);

    log_write("TEX: init OK %dx%d in %dx%d tex (double buffered)", width, height, tex_w, tex_h);
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

bool video_player_play(const char *url, int64_t duration_ticks, int64_t seek_offset_ticks)
{
    log_write("PLAY: starting video, url_len=%d seek=%lld", (int)strlen(url), (long long)seek_offset_ticks);
    video_player_stop();

    snprintf(s_vp.url, sizeof(s_vp.url), "%s", url);
    s_vp.duration_ticks = duration_ticks;
    s_vp.seek_offset_ticks = seek_offset_ticks;
    s_vp.position_ticks = seek_offset_ticks;
    s_vp.error_msg[0] = '\0';
    s_vp.stop_requested = false;
    s_vp.state = VIDEO_LOADING;
    s_vp.new_tex_ready = false;
    s_vp.audio_buf_idx = 0;

    /* Allocate ring buffer */
    s_vp.demux.ring_data = malloc(RING_SIZE);
    if (!s_vp.demux.ring_data) return false;
    s_vp.demux.ring_size = RING_SIZE;
    s_vp.demux.ring_write_pos = 0;
    s_vp.demux.ring_read_pos = 0;
    s_vp.demux.ring_fill = 0;
    s_vp.demux.ring_finished = false;

    /* Reset NDSP channel. Set the channel field now rather than in
     * init_audio_decoder so a stop() before audio init doesn't clear
     * channel 0 (the music player's channel). */
    s_vp.ndsp_channel = 1;
    ndspChnReset(1);

    /* Launch threads (-1 = any available core) */
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    log_write("PLAY: creating threads, prio=%ld", (long)prio);

    s_vp.net_thread = threadCreate(net_thread_func, NULL,
                                    32 * 1024, prio - 1, -1, false);
    s_vp.decode_thread = threadCreate(decode_thread_func, NULL,
                                       64 * 1024, prio - 1, -1, false);
    s_vp.convert_thread = threadCreate(convert_thread_func, NULL,
                                        32 * 1024, prio, -1, false);

    if (!s_vp.net_thread || !s_vp.decode_thread || !s_vp.convert_thread) {
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
        /* RELAXED is sufficient (convert thread already joined) but keep
         * every cross-thread write to tex_initialized atomic for consistency */
        __atomic_store_n(&s_vp.tex_initialized, false, __ATOMIC_RELAXED);
    }
    /* Don't let the next playback render a frame from the previous video */
    s_vp.frame_img.tex = NULL;

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
    st.buffer_percent = (s_vp.demux.ring_size > 0)
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
    snprintf(st.error_msg, sizeof(st.error_msg), "%s", s_vp.error_msg);
    return st;
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
        s_vp.new_tex_ready = false;
        render_log_count++;
        if (render_log_count <= 3)
            log_write("RENDER: displaying tex[%d] frame#%d", s_vp.tex_display_idx, render_log_count);
    }
    LightLock_Unlock(&s_vp.tex_lock);

    /* Draw the frame texture on the top screen — zero upload cost */
    if (s_vp.tex_initialized && s_vp.frame_img.tex) {
        float x = (400 - s_vp.display_width) / 2.0f;
        float y = (240 - s_vp.display_height) / 2.0f;
        C2D_DrawImageAt(s_vp.frame_img, x, y, 0.5f, NULL, 1.0f, 1.0f);
    }
}
