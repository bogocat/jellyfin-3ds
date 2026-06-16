/**
 * player.c - Audio streaming player implementation
 *
 * Architecture:
 *   [Network thread] --ring buffer--> [Decode thread] --PCM16--> [NDSP]
 *
 * The network thread fetches audio data via libcurl into a ring buffer.
 * The decode thread reads from the ring buffer, decodes with libmpg123,
 * and feeds PCM16 samples to NDSP's double-buffered wave buffers.
 *
 * Reference: ctrmus (github.com/deltabeard/ctrmus)
 * Reference: ThirdTube audio pipeline
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>

#include "audio/player.h"

/* We use mpg123 for MP3 decoding as the primary path.
 * The Jellyfin API is configured to transcode to MP3. */
#include <mpg123.h>

/* ── Ring buffer for network data ──────────────────────────────────── */

#define RING_SIZE (512 * 1024) /* 512KB network buffer */

typedef struct {
    u8    *data;
    int    write_pos;
    int    read_pos;
    int    fill;       /* bytes available to read */
    bool   finished;   /* no more data coming from network */
    LightLock lock;
} ring_buffer_t;

static void ring_init(ring_buffer_t *rb)
{
    rb->data = linearAlloc(RING_SIZE);
    rb->write_pos = 0;
    rb->read_pos = 0;
    rb->fill = 0;
    rb->finished = false;
    LightLock_Init(&rb->lock);
}

static void ring_free(ring_buffer_t *rb)
{
    if (rb->data) {
        linearFree(rb->data);
        rb->data = NULL;
    }
}

/* fill and finished are read by the decode thread outside the lock
 * (prefetch/EOF checks), so they are updated with atomics — same
 * pattern as the video player's ring_fill fix. */

static int ring_write(ring_buffer_t *rb, const u8 *data, int len)
{
    LightLock_Lock(&rb->lock);
    int space = RING_SIZE - __atomic_load_n(&rb->fill, __ATOMIC_ACQUIRE);
    if (len > space) len = space;

    for (int i = 0; i < len; i++) {
        rb->data[rb->write_pos] = data[i];
        rb->write_pos = (rb->write_pos + 1) % RING_SIZE;
    }
    __atomic_fetch_add(&rb->fill, len, __ATOMIC_RELEASE);
    LightLock_Unlock(&rb->lock);
    return len;
}

static int ring_read(ring_buffer_t *rb, u8 *data, int len)
{
    LightLock_Lock(&rb->lock);
    int avail = __atomic_load_n(&rb->fill, __ATOMIC_ACQUIRE);
    if (len > avail) len = avail;

    for (int i = 0; i < len; i++) {
        data[i] = rb->data[rb->read_pos];
        rb->read_pos = (rb->read_pos + 1) % RING_SIZE;
    }
    __atomic_fetch_sub(&rb->fill, len, __ATOMIC_RELEASE);
    LightLock_Unlock(&rb->lock);
    return len;
}

/* NOTE: fill/finished must ONLY be accessed through atomics (or these
 * helpers) once threads are running — mixing in plain reads/writes is a
 * C11 data race. ACQUIRE is needed in the decode-thread spin loops; for
 * pure UI snapshots a RELAXED load suffices (see audio_player_get_status). */
static inline int ring_fill_level(const ring_buffer_t *rb)
{
    return __atomic_load_n(&rb->fill, __ATOMIC_ACQUIRE);
}

static inline bool ring_is_finished(const ring_buffer_t *rb)
{
    return __atomic_load_n(&rb->finished, __ATOMIC_ACQUIRE);
}

/* ── Player state ──────────────────────────────────────────────────── */

static struct {
    /* Threading */
    Thread          net_thread;
    Thread          decode_thread;
    volatile bool   running;
    volatile bool   stop_requested;

    /* State */
    player_state_t  state;
    LightLock       state_lock;

    /* Stream info */
    char            url[512];
    int64_t         duration_ticks;
    int64_t         position_ticks;

    /* Network */
    ring_buffer_t   ring;

    /* Decode */
    mpg123_handle  *mpg;
    long            sample_rate; /* actual rate from stream (44100 or 48000) */

    /* NDSP output */
    int             ndsp_channel;
    ndspWaveBuf     wave_bufs[AUDIO_NUM_BUFFERS];
    s16            *pcm_bufs[AUDIO_NUM_BUFFERS];
    int             active_buf;

    /* Error */
    char            error_msg[128];
} s_player;

/* ── cURL write callback (feeds ring buffer) ───────────────────────── */

#include <curl/curl.h>

static size_t stream_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    ring_buffer_t *rb = (ring_buffer_t *)userdata;
    size_t total = size * nmemb;

    if (s_player.stop_requested) return 0; /* abort transfer */

    /* Block briefly if buffer is full */
    size_t written = 0;
    while (written < total && !s_player.stop_requested) {
        int n = ring_write(rb, (u8 *)ptr + written, total - written);
        written += n;
        if (written < total)
            svcSleepThread(1000000LL); /* 1ms */
    }

    return written;
}

/* ── Network thread ────────────────────────────────────────────────── */

/* Cached file on SD: pump it into the ring buffer in place of curl.
 * Everything downstream (mpg123 feed, NDSP) is identical. */
static void local_file_to_ring(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(s_player.error_msg, sizeof(s_player.error_msg),
                 "Cache file open failed");
        s_player.state = PLAYER_ERROR;
        return;
    }

    u8 buf[8192];
    while (!s_player.stop_requested) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) {
            /* Distinguish a real SD read error (bad sector, card pulled)
             * from a clean EOF — otherwise a truncated read looks like a
             * normal end-of-track and the user sees no error. */
            if (ferror(f)) {
                snprintf(s_player.error_msg, sizeof(s_player.error_msg),
                         "SD read error");
                s_player.state = PLAYER_ERROR;
            }
            break;
        }
        size_t written = 0;
        while (written < n && !s_player.stop_requested) {
            written += ring_write(&s_player.ring, buf + written, n - written);
            if (written < n)
                svcSleepThread(1000000LL); /* 1ms — ring full */
        }
    }
    fclose(f);
}

static void net_thread_func(void *arg)
{
    (void)arg;

    if (strncmp(s_player.url, "http", 4) != 0) {
        local_file_to_ring(s_player.url);
        __atomic_store_n(&s_player.ring.finished, true, __ATOMIC_RELEASE);
        return;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(s_player.error_msg, sizeof(s_player.error_msg), "curl init failed");
        s_player.state = PLAYER_ERROR;
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, s_player.url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s_player.ring);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 32768L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Jellyfin-3DS/" JFIN_VERSION);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK && !s_player.stop_requested) {
        snprintf(s_player.error_msg, sizeof(s_player.error_msg),
                 "Stream error: %s", curl_easy_strerror(res));
        s_player.state = PLAYER_ERROR;
    }

    /* Release store: the decode thread must see all ring_write()s
     * before it sees finished=true, or it can exit with data unplayed */
    __atomic_store_n(&s_player.ring.finished, true, __ATOMIC_RELEASE);
    curl_easy_cleanup(curl);
}

/* ── Decode thread ─────────────────────────────────────────────────── */

static int feed_and_decode(s16 *out_buf, int max_samples)
{
    /* Feed data from ring buffer into mpg123 */
    u8 feed_buf[8192];
    int avail = ring_read(&s_player.ring, feed_buf, sizeof(feed_buf));
    if (avail > 0) {
        mpg123_feed(s_player.mpg, feed_buf, avail);
    }

    /* Try to decode PCM */
    size_t decoded = 0;
    int err = mpg123_read(s_player.mpg, (unsigned char *)out_buf,
                          max_samples * sizeof(s16) * AUDIO_CHANNELS, &decoded);

    if (err == MPG123_NEW_FORMAT) {
        long rate;
        int channels, encoding;
        mpg123_getformat(s_player.mpg, &rate, &channels, &encoding);

        /* Update NDSP sample rate to match the actual stream */
        if (rate != s_player.sample_rate && rate > 0) {
            s_player.sample_rate = rate;
            ndspChnSetRate(s_player.ndsp_channel, (float)rate);
        }

        /* Re-read after format change */
        err = mpg123_read(s_player.mpg, (unsigned char *)out_buf,
                          max_samples * sizeof(s16) * AUDIO_CHANNELS, &decoded);
    }

    return decoded / (sizeof(s16) * AUDIO_CHANNELS);
}

static void decode_thread_func(void *arg)
{
    (void)arg;

    /* Wait for initial buffer fill before starting playback */
    while (!s_player.stop_requested) {
        if (ring_fill_level(&s_player.ring) >= (int)AUDIO_PREFETCH_BYTES ||
            ring_is_finished(&s_player.ring))
            break;
        svcSleepThread(10000000LL); /* 10ms */
    }

    if (s_player.stop_requested) return;

    /* Probe stream format before queuing any audio.
     * Feed data until mpg123 reports the format, then set NDSP rate. */
    while (!s_player.stop_requested) {
        u8 probe_buf[4096];
        int avail = ring_read(&s_player.ring, probe_buf, sizeof(probe_buf));
        if (avail > 0)
            mpg123_feed(s_player.mpg, probe_buf, avail);

        long rate;
        int channels, encoding;
        int ret = mpg123_getformat(s_player.mpg, &rate, &channels, &encoding);
        if (ret == MPG123_OK && rate > 0) {
            s_player.sample_rate = rate;
            ndspChnSetRate(s_player.ndsp_channel, (float)rate);
            break;
        }
        if (ring_is_finished(&s_player.ring) && ring_fill_level(&s_player.ring) == 0)
            break;
        svcSleepThread(5000000LL); /* 5ms */
    }

    if (s_player.stop_requested) return;

    LightLock_Lock(&s_player.state_lock);
    s_player.state = PLAYER_PLAYING;
    LightLock_Unlock(&s_player.state_lock);

    while (!s_player.stop_requested) {
        /* Find a free wave buffer */
        ndspWaveBuf *wbuf = &s_player.wave_bufs[s_player.active_buf];
        if (wbuf->status == NDSP_WBUF_QUEUED || wbuf->status == NDSP_WBUF_PLAYING) {
            svcSleepThread(4000000LL); /* 4ms */
            continue;
        }

        /* Decode samples into this buffer */
        int samples = feed_and_decode(s_player.pcm_bufs[s_player.active_buf],
                                      AUDIO_BUFFER_SAMPLES);

        if (samples <= 0) {
            if (ring_is_finished(&s_player.ring) && ring_fill_level(&s_player.ring) == 0)
                break; /* end of stream */
            svcSleepThread(4000000LL);
            continue;
        }

        /* Submit to NDSP */
        wbuf->data_vaddr = s_player.pcm_bufs[s_player.active_buf];
        wbuf->nsamples = samples;
        DSP_FlushDataCache(s_player.pcm_bufs[s_player.active_buf],
                           samples * sizeof(s16) * AUDIO_CHANNELS);
        ndspChnWaveBufAdd(s_player.ndsp_channel, wbuf);

        s_player.active_buf = (s_player.active_buf + 1) % AUDIO_NUM_BUFFERS;

        /* Update position estimate using actual sample rate */
        long rate = s_player.sample_rate ? s_player.sample_rate : AUDIO_SAMPLE_RATE;
        s_player.position_ticks += (int64_t)samples * 10000000LL / rate;
    }

    if (!s_player.stop_requested) {
        LightLock_Lock(&s_player.state_lock);
        s_player.state = PLAYER_STOPPED;
        LightLock_Unlock(&s_player.state_lock);
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

bool audio_player_init(void)
{
    memset(&s_player, 0, sizeof(s_player));
    LightLock_Init(&s_player.state_lock);

    /* Init mpg123 */
    if (mpg123_init() != MPG123_OK)
        return false;

    s_player.mpg = mpg123_new(NULL, NULL);
    if (!s_player.mpg) return false;

    mpg123_open_feed(s_player.mpg);
    mpg123_param(s_player.mpg, MPG123_FLAGS, MPG123_FORCE_STEREO, 0);

    /* NDSP channel setup */
    s_player.ndsp_channel = 0;
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, AUDIO_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    float mix[12] = {0};
    mix[0] = 1.0f; /* left */
    mix[1] = 1.0f; /* right */
    ndspChnSetMix(0, mix);

    /* Allocate PCM buffers in linear memory (required for NDSP DMA) */
    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        s_player.pcm_bufs[i] = linearAlloc(AUDIO_BUFFER_SAMPLES * sizeof(s16) * AUDIO_CHANNELS);
        if (!s_player.pcm_bufs[i]) return false;
        memset(&s_player.wave_bufs[i], 0, sizeof(ndspWaveBuf));
    }

    s_player.state = PLAYER_STOPPED;
    return true;
}

void audio_player_cleanup(void)
{
    audio_player_stop();

    if (s_player.mpg) {
        mpg123_close(s_player.mpg);
        mpg123_delete(s_player.mpg);
        s_player.mpg = NULL;
    }
    mpg123_exit();

    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        if (s_player.pcm_bufs[i]) {
            linearFree(s_player.pcm_bufs[i]);
            s_player.pcm_bufs[i] = NULL;
        }
    }
}

bool audio_player_play(const char *url, int64_t duration_ticks, int64_t seek_offset_ticks)
{
    audio_player_stop(); /* stop any current playback */

    snprintf(s_player.url, sizeof(s_player.url), "%s", url);
    s_player.duration_ticks = duration_ticks;
    s_player.position_ticks = seek_offset_ticks;
    s_player.error_msg[0] = '\0';
    s_player.stop_requested = false;
    s_player.active_buf = 0;
    s_player.sample_rate = AUDIO_SAMPLE_RATE;
    s_player.state = PLAYER_LOADING;

    /* Reset decoder */
    mpg123_open_feed(s_player.mpg);

    /* Init ring buffer */
    ring_init(&s_player.ring);

    /* Reset wave buffers */
    ndspChnReset(s_player.ndsp_channel);
    ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(0, AUDIO_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    for (int i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        memset(&s_player.wave_bufs[i], 0, sizeof(ndspWaveBuf));
        s_player.wave_bufs[i].status = NDSP_WBUF_DONE;
    }

    /* Launch threads */
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

    s_player.net_thread = threadCreate(net_thread_func, NULL,
                                        16 * 1024, prio - 1, -1, false);
    s_player.decode_thread = threadCreate(decode_thread_func, NULL,
                                           16 * 1024, prio - 1, -1, false);

    if (!s_player.net_thread || !s_player.decode_thread) {
        snprintf(s_player.error_msg, sizeof(s_player.error_msg), "Thread creation failed");
        /* Wind down whichever thread DID start and free the ring buffer,
         * otherwise a lone net thread keeps streaming into a ring nobody
         * drains and the 512KB ring leaks on every failed play. */
        s_player.stop_requested = true;
        if (s_player.net_thread) {
            threadJoin(s_player.net_thread, U64_MAX);
            threadFree(s_player.net_thread);
            s_player.net_thread = NULL;
        }
        if (s_player.decode_thread) {
            threadJoin(s_player.decode_thread, U64_MAX);
            threadFree(s_player.decode_thread);
            s_player.decode_thread = NULL;
        }
        ring_free(&s_player.ring);
        s_player.state = PLAYER_ERROR;
        return false;
    }

    return true;
}

void audio_player_stop(void)
{
    if (s_player.state == PLAYER_STOPPED && !s_player.net_thread)
        return;

    s_player.stop_requested = true;

    /* Wait for threads to finish */
    if (s_player.net_thread) {
        threadJoin(s_player.net_thread, U64_MAX);
        threadFree(s_player.net_thread);
        s_player.net_thread = NULL;
    }
    if (s_player.decode_thread) {
        threadJoin(s_player.decode_thread, U64_MAX);
        threadFree(s_player.decode_thread);
        s_player.decode_thread = NULL;
    }

    ndspChnWaveBufClear(s_player.ndsp_channel);
    ring_free(&s_player.ring);

    s_player.state = PLAYER_STOPPED;
}

void audio_player_pause(void)
{
    LightLock_Lock(&s_player.state_lock);
    if (s_player.state == PLAYER_PLAYING) {
        ndspChnSetPaused(s_player.ndsp_channel, true);
        s_player.state = PLAYER_PAUSED;
    } else if (s_player.state == PLAYER_PAUSED) {
        ndspChnSetPaused(s_player.ndsp_channel, false);
        s_player.state = PLAYER_PLAYING;
    }
    LightLock_Unlock(&s_player.state_lock);
}

bool audio_player_seek(int64_t position_ticks)
{
    /* Seeking requires re-requesting the stream with a startTimeTicks param.
     * For MVP, we don't implement seeking — just restart from the beginning
     * or skip to next track. */
    (void)position_ticks;
    return false;
}

player_status_t audio_player_get_status(void)
{
    player_status_t status;
    LightLock_Lock(&s_player.state_lock);
    status.state = s_player.state;
    status.position_ticks = s_player.position_ticks;
    status.duration_ticks = s_player.duration_ticks;
    /* RELAXED: UI status snapshot, nothing is gated on this read
     * (matches video_player_get_status) */
    status.buffer_percent = (s_player.ring.data && RING_SIZE > 0)
        ? (__atomic_load_n(&s_player.ring.fill, __ATOMIC_RELAXED) * 100 / RING_SIZE) : 0;
    snprintf(status.error_msg, sizeof(status.error_msg), "%s", s_player.error_msg);
    LightLock_Unlock(&s_player.state_lock);
    return status;
}

void audio_player_update(void)
{
    /* Currently a no-op — NDSP buffer pumping is handled in the decode thread.
     * This hook exists for future use (e.g., main-thread UI sync, reporting). */
}
