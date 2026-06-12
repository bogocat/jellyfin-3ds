/**
 * jellyfin.c - Jellyfin REST API client implementation
 *
 * Reference: Switchfin (github.com/dragonflylee/switchfin)
 * Reference: https://jmshrv.com/posts/jellyfin-api/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>
#include <curl/curl.h>

#include "api/jellyfin.h"
#include "api/cJSON.h"
#include "util/config.h"
#include "util/log.h"

extern jfin_config_t g_config;

/* ── Internal state ────────────────────────────────────────────────── */

static CURL *s_curl = NULL;
static char  s_user_agent[256];

/* ── cURL helpers ──────────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t size;
    size_t capacity;
} response_buf_t;

static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    response_buf_t *buf = (response_buf_t *)userdata;
    size_t total = size * nmemb;

    if (buf->size + total >= buf->capacity) {
        size_t new_cap = (buf->capacity + total) * 2;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->capacity = new_cap;
    }

    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static response_buf_t response_buf_new(void)
{
    response_buf_t buf = {0};
    buf.capacity = 4096;
    buf.data = malloc(buf.capacity);
    if (buf.data) buf.data[0] = '\0';
    return buf;
}

static void response_buf_free(response_buf_t *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

/**
 * Build the MediaBrowser authorization header.
 * Format: MediaBrowser Client="Jellyfin 3DS", Device="3DS", DeviceId="...", Version="0.1.0"[, Token="..."]
 */
static void build_auth_header(const jfin_session_t *session, char *out, int out_len)
{
    if (session && session->access_token[0] != '\0') {
        snprintf(out, out_len,
            "MediaBrowser Client=\"Jellyfin 3DS\", Device=\"Nintendo 3DS\", "
            "DeviceId=\"%s\", Version=\"" JFIN_VERSION "\", Token=\"%s\"",
            session->device_id, session->access_token);
    } else {
        snprintf(out, out_len,
            "MediaBrowser Client=\"Jellyfin 3DS\", Device=\"Nintendo 3DS\", "
            "DeviceId=\"%s\", Version=\"" JFIN_VERSION "\"",
            session ? session->device_id : "unknown");
    }
}

/**
 * Perform an HTTP GET request. Returns parsed cJSON object or NULL.
 * Caller must cJSON_Delete() the result.
 */
static cJSON *api_get(const jfin_session_t *session, const char *url)
{
    if (!s_curl) return NULL;

    response_buf_t resp = response_buf_new();
    if (!resp.data) return NULL;

    char auth_header[512];
    build_auth_header(session, auth_header, sizeof(auth_header));

    char auth_full[600];
    snprintf(auth_full, sizeof(auth_full), "Authorization: %s", auth_header);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_full);
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_reset(s_curl);
    curl_easy_setopt(s_curl, CURLOPT_URL, url);
    curl_easy_setopt(s_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(s_curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(s_curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(s_curl, CURLOPT_USERAGENT, s_user_agent);
    curl_easy_setopt(s_curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(s_curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(s_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s_curl, CURLOPT_SSL_VERIFYPEER, 0L); /* 3DS has no CA store */

    CURLcode res = curl_easy_perform(s_curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        printf("API GET error: %s\n", curl_easy_strerror(res));
        response_buf_free(&resp);
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(s_curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        printf("API GET %ld: %s\n", http_code, url);
        response_buf_free(&resp);
        return NULL;
    }

    cJSON *json = cJSON_Parse(resp.data);
    response_buf_free(&resp);
    return json;
}

/**
 * Perform an HTTP POST request with JSON body.
 * Returns parsed cJSON object or NULL.
 */
static cJSON *api_post(const jfin_session_t *session, const char *url,
                       const char *json_body)
{
    if (!s_curl) return NULL;

    response_buf_t resp = response_buf_new();
    if (!resp.data) return NULL;

    char auth_header[512];
    build_auth_header(session, auth_header, sizeof(auth_header));

    char auth_full[600];
    snprintf(auth_full, sizeof(auth_full), "Authorization: %s", auth_header);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_full);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_reset(s_curl);
    curl_easy_setopt(s_curl, CURLOPT_URL, url);
    curl_easy_setopt(s_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(s_curl, CURLOPT_POSTFIELDS, json_body ? json_body : "");
    curl_easy_setopt(s_curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(s_curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(s_curl, CURLOPT_USERAGENT, s_user_agent);
    curl_easy_setopt(s_curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(s_curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(s_curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(s_curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        printf("API POST error: %s\n", curl_easy_strerror(res));
        response_buf_free(&resp);
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(s_curl, CURLINFO_RESPONSE_CODE, &http_code);

    cJSON *json = NULL;
    if (resp.size > 0)
        json = cJSON_Parse(resp.data);

    response_buf_free(&resp);

    if (http_code < 200 || http_code >= 300) {
        printf("API POST %ld: %s\n", http_code, url);
        if (json) cJSON_Delete(json);
        return NULL;
    }

    return json;
}

/* ── JSON parsing helpers ──────────────────────────────────────────── */

static void json_get_string(const cJSON *obj, const char *key, char *out, int out_len)
{
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(val) && val->valuestring) {
        snprintf(out, out_len, "%s", val->valuestring);
    } else {
        out[0] = '\0';
    }
}

static int json_get_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(val)) return val->valueint;
    return fallback;
}

static int64_t json_get_int64(const cJSON *obj, const char *key, int64_t fallback)
{
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(val)) return (int64_t)val->valuedouble;
    return fallback;
}

static jfin_item_type_t parse_item_type(const char *type_str)
{
    if (!type_str) return JFIN_ITEM_UNKNOWN;
    if (strcmp(type_str, "CollectionFolder") == 0) return JFIN_ITEM_FOLDER;
    if (strcmp(type_str, "UserView") == 0) return JFIN_ITEM_FOLDER;
    if (strcmp(type_str, "Folder") == 0) return JFIN_ITEM_FOLDER;
    if (strcmp(type_str, "MusicAlbum") == 0) return JFIN_ITEM_MUSIC_ALBUM;
    if (strcmp(type_str, "MusicArtist") == 0) return JFIN_ITEM_MUSIC_ARTIST;
    if (strcmp(type_str, "Audio") == 0) return JFIN_ITEM_AUDIO;
    if (strcmp(type_str, "Movie") == 0) return JFIN_ITEM_MOVIE;
    /* "Home Videos and Photos" libraries return the generic Video type */
    if (strcmp(type_str, "Video") == 0) return JFIN_ITEM_MOVIE;
    if (strcmp(type_str, "Series") == 0) return JFIN_ITEM_SERIES;
    if (strcmp(type_str, "Season") == 0) return JFIN_ITEM_SEASON;
    if (strcmp(type_str, "Episode") == 0) return JFIN_ITEM_EPISODE;
    return JFIN_ITEM_UNKNOWN;
}

static void parse_item(const cJSON *obj, jfin_item_t *item)
{
    memset(item, 0, sizeof(*item));
    json_get_string(obj, "Id", item->id, sizeof(item->id));
    json_get_string(obj, "Name", item->name, sizeof(item->name));
    json_get_string(obj, "Album", item->album, sizeof(item->album));
    json_get_string(obj, "SeriesName", item->series_name, sizeof(item->series_name));
    item->year = json_get_int(obj, "ProductionYear", 0);
    item->index_number = json_get_int(obj, "IndexNumber", 0);
    item->runtime_ticks = json_get_int64(obj, "RunTimeTicks", 0);

    /* Artist can be in AlbumArtist or Artists array */
    json_get_string(obj, "AlbumArtist", item->artist, sizeof(item->artist));
    if (item->artist[0] == '\0') {
        const cJSON *artists = cJSON_GetObjectItemCaseSensitive(obj, "Artists");
        if (cJSON_IsArray(artists) && cJSON_GetArraySize(artists) > 0) {
            const cJSON *first = cJSON_GetArrayItem(artists, 0);
            if (cJSON_IsString(first) && first->valuestring)
                snprintf(item->artist, sizeof(item->artist), "%s", first->valuestring);
        }
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(obj, "Type");
    item->type = parse_item_type(cJSON_IsString(type) ? type->valuestring : NULL);

    const cJSON *image_tags = cJSON_GetObjectItemCaseSensitive(obj, "ImageTags");
    item->has_primary_image = (image_tags &&
        cJSON_GetObjectItemCaseSensitive(image_tags, "Primary") != NULL);

    /* Album art fallback for audio tracks (which often lack their own image) */
    json_get_string(obj, "AlbumId", item->album_id, sizeof(item->album_id));
    const cJSON *album_tag = cJSON_GetObjectItemCaseSensitive(obj, "AlbumPrimaryImageTag");
    item->has_album_image = (item->album_id[0] != '\0' &&
        cJSON_IsString(album_tag) && album_tag->valuestring != NULL);

    /* Stereoscopic 3D format. Half vs Full SBS matters: each HSBS eye is
     * anamorphic (needs a 2x horizontal stretch at display time), each
     * FSBS eye is already at native aspect. */
    const cJSON *v3d = cJSON_GetObjectItemCaseSensitive(obj, "Video3DFormat");
    if (cJSON_IsString(v3d) && v3d->valuestring) {
        if (strcmp(v3d->valuestring, "HalfSideBySide") == 0)
            item->video_3d_format = JFIN_3D_HSBS;
        else if (strcmp(v3d->valuestring, "FullSideBySide") == 0)
            item->video_3d_format = JFIN_3D_FSBS;
        else if (strstr(v3d->valuestring, "TopAndBottom"))
            item->video_3d_format = JFIN_3D_HTAB;
    }
}

static void parse_item_list(const cJSON *json, jfin_item_list_t *list)
{
    memset(list, 0, sizeof(*list));

    const cJSON *items = cJSON_GetObjectItemCaseSensitive(json, "Items");
    if (!cJSON_IsArray(items)) return;

    list->total_count = json_get_int(json, "TotalRecordCount", 0);
    list->start_index = json_get_int(json, "StartIndex", 0);

    int count = cJSON_GetArraySize(items);
    if (count > JFIN_MAX_ITEMS) count = JFIN_MAX_ITEMS;

    for (int i = 0; i < count; i++) {
        parse_item(cJSON_GetArrayItem(items, i), &list->items[i]);
    }
    list->count = count;
}

/* ── Public API ────────────────────────────────────────────────────── */

bool jfin_init(void)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return false;

    s_curl = curl_easy_init();
    if (!s_curl) return false;

    snprintf(s_user_agent, sizeof(s_user_agent),
             "Jellyfin-3DS/" JFIN_VERSION " (Nintendo 3DS; ARM11)");

    return true;
}

void jfin_cleanup(void)
{
    if (s_curl) {
        curl_easy_cleanup(s_curl);
        s_curl = NULL;
    }
    curl_global_cleanup();
}

bool jfin_login(jfin_session_t *session, const char *server_url,
                const char *username, const char *password,
                const char *device_id)
{
    memset(session, 0, sizeof(*session));
    snprintf(session->server_url, sizeof(session->server_url), "%s", server_url);
    /* Use the persistent per-console device id so the token is bound to
     * the same DeviceId that restored sessions (main.c) present later */
    snprintf(session->device_id, sizeof(session->device_id), "%s",
             (device_id && device_id[0]) ? device_id : "3ds-jellyfin-001");

    /* Remove trailing slash */
    int len = strlen(session->server_url);
    if (len > 0 && session->server_url[len - 1] == '/')
        session->server_url[len - 1] = '\0';

    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url), "%s/Users/AuthenticateByName", session->server_url);

    /* Build login JSON */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "Username", username);
    cJSON_AddStringToObject(body, "Pw", password);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return false; /* OOM — don't POST an empty body */

    cJSON *resp = api_post(session, url, body_str);
    free(body_str);

    if (!resp) return false;

    json_get_string(resp, "AccessToken", session->access_token, sizeof(session->access_token));

    const cJSON *user = cJSON_GetObjectItemCaseSensitive(resp, "User");
    if (user) {
        json_get_string(user, "Id", session->user_id, sizeof(session->user_id));
    }

    /* Get server name */
    json_get_string(resp, "ServerId", session->server_name, sizeof(session->server_name));

    cJSON_Delete(resp);

    session->authenticated = (session->access_token[0] != '\0' &&
                              session->user_id[0] != '\0');

    if (session->authenticated)
        printf("Logged in as %s\n", username);
    else
        printf("Login failed for %s\n", username);

    return session->authenticated;
}

void jfin_logout(jfin_session_t *session)
{
    if (!session->authenticated) return;

    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url), "%s/Sessions/Logout", session->server_url);
    cJSON *resp = api_post(session, url, NULL);
    if (resp) cJSON_Delete(resp);

    memset(session->access_token, 0, sizeof(session->access_token));
    session->authenticated = false;
}

bool jfin_get_views(const jfin_session_t *session, jfin_item_list_t *out)
{
    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url), "%s/Users/%s/Views",
             session->server_url, session->user_id);

    cJSON *json = api_get(session, url);
    if (!json) return false;

    parse_item_list(json, out);
    cJSON_Delete(json);
    return true;
}

bool jfin_get_items(const jfin_session_t *session, const char *parent_id,
                    int start_index, int limit, jfin_item_list_t *out)
{
    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url),
             "%s/Users/%s/Items?ParentId=%s&StartIndex=%d&Limit=%d"
             "&SortBy=SortName&SortOrder=Ascending"
             "&Fields=PrimaryImageAspectRatio,BasicSyncInfo",
             session->server_url, session->user_id, parent_id,
             start_index, limit);

    cJSON *json = api_get(session, url);
    if (!json) return false;

    parse_item_list(json, out);
    out->start_index = start_index;
    cJSON_Delete(json);
    return true;
}

bool jfin_get_resume(const jfin_session_t *session, jfin_item_list_t *out)
{
    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url),
             "%s/Users/%s/Items/Resume?Limit=%d&Fields=PrimaryImageAspectRatio",
             session->server_url, session->user_id, JFIN_MAX_ITEMS);

    cJSON *json = api_get(session, url);
    if (!json) return false;

    parse_item_list(json, out);
    cJSON_Delete(json);
    return true;
}

bool jfin_get_latest(const jfin_session_t *session, const char *parent_id,
                     int limit, jfin_item_list_t *out)
{
    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url),
             "%s/Users/%s/Items/Latest?ParentId=%s&Limit=%d"
             "&Fields=PrimaryImageAspectRatio",
             session->server_url, session->user_id, parent_id, limit);

    /* /Latest returns a flat array, not {Items: [...]} */
    cJSON *json = api_get(session, url);
    if (!json) return false;

    memset(out, 0, sizeof(*out));

    if (cJSON_IsArray(json)) {
        int count = cJSON_GetArraySize(json);
        if (count > JFIN_MAX_ITEMS) count = JFIN_MAX_ITEMS;
        for (int i = 0; i < count; i++) {
            parse_item(cJSON_GetArrayItem(json, i), &out->items[i]);
        }
        out->count = count;
        out->total_count = count;
    }

    cJSON_Delete(json);
    return true;
}

bool jfin_search(const jfin_session_t *session, const char *query,
                 int limit, jfin_item_list_t *out)
{
    /* URL-encode the query using libcurl */
    char *encoded = curl_easy_escape(s_curl, query, 0);
    if (!encoded) return false;

    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url),
             "%s/Users/%s/Items?SearchTerm=%s&Limit=%d&Recursive=true"
             "&Fields=PrimaryImageAspectRatio",
             session->server_url, session->user_id, encoded, limit);

    cJSON *json = api_get(session, url);
    curl_free(encoded);
    if (!json) return false;

    parse_item_list(json, out);
    cJSON_Delete(json);
    return true;
}

bool jfin_get_audio_stream(const jfin_session_t *session, const char *item_id,
                           int64_t start_ticks, jfin_stream_t *out)
{
    memset(out, 0, sizeof(*out));

    int len = snprintf(out->url, sizeof(out->url),
             "%s/Audio/%s/universal?UserId=%s&DeviceId=%s"
             "&MaxStreamingBitrate=%d"
             "&Container=mp3,opus,ogg,aac"
             "&AudioCodec=mp3"
             "&TranscodingContainer=mp3"
             "&TranscodingProtocol=http"
             "&api_key=%s",
             session->server_url, item_id, session->user_id,
             session->device_id, g_config.audio_bitrate * 1000,
             session->access_token);

    if (start_ticks > 0 && len < (int)sizeof(out->url) - 40)
        snprintf(out->url + len, sizeof(out->url) - len,
                 "&startTimeTicks=%lld", (long long)start_ticks);

    snprintf(out->container, sizeof(out->container), "%s", "mp3");
    out->is_transcoding = true;

    return true;
}

bool jfin_get_video_stream(const jfin_session_t *session, const char *item_id,
                           int64_t start_ticks, bool is_3d,
                           jfin_stream_t *out)
{
    memset(out, 0, sizeof(*out));

    /* 3D SBS: request a double-width frame so each eye half is ~400px
     * (native top-screen width). No MaxHeight — let Jellyfin preserve
     * the aspect ratio. If hardware decode/tiling can't keep up at 800,
     * 640 is the fallback knob (each eye 320, upscaled). */
    const char *res_params = is_3d
        ? "&MaxWidth=800"
        : "&MaxWidth=400&MaxHeight=240";

    /* Unique PlaySessionId per request prevents stale transcode conflicts */
    u64 tick = svcGetSystemTick();
    int len = snprintf(out->url, sizeof(out->url),
             "%s/Videos/%s/stream?UserId=%s&DeviceId=%s"
             "&VideoCodec=h264"
             "&AudioCodec=aac"
             "&Container=ts"
             "%s"
             "&VideoBitRate=%d"
             "&AudioBitRate=%d"
             "&MaxAudioChannels=2"
             "&TranscodingMaxAudioChannels=2"
             "&Profile=Baseline"
             "&Level=31"
             "&MaxRefFrames=2"
             "&MediaSourceId=%s"
             "&PlaySessionId=3ds%08lx"
             "&api_key=%s",
             session->server_url, item_id, session->user_id,
             session->device_id, res_params,
             g_config.video_bitrate * 1000, g_config.audio_bitrate * 1000,
             item_id, (unsigned long)(tick & 0xFFFFFFFF), session->access_token);

    if (start_ticks > 0 && len > 0 && len < (int)sizeof(out->url) - 40) {
        /* The base URL above already carries a PlaySessionId that is
         * unique per call (fresh tick), which is what busts Jellyfin's
         * per-item+device transcode cache on seek. Appending a second
         * PlaySessionId here (the old behavior) sent two different
         * session ids in one request. */
        snprintf(out->url + len, sizeof(out->url) - len,
                 "&StartTimeTicks=%lld", (long long)start_ticks);
        log_write("SEEK: StartTimeTicks=%lld session=3ds%08lx",
                  (long long)start_ticks, (unsigned long)(tick & 0xFFFFFFFF));
    }

    snprintf(out->container, sizeof(out->container), "%s", "ts");
    out->is_transcoding = true;

    return true;
}

void jfin_get_image_url_for_item(const jfin_session_t *session,
                                 const jfin_item_t *item,
                                 int max_width, int max_height,
                                 char *url_out, int url_out_len)
{
    /* Use the item's own image, or fall back to album art for audio tracks */
    const char *image_item_id = item->id;
    if (!item->has_primary_image && item->has_album_image)
        image_item_id = item->album_id;

    snprintf(url_out, url_out_len,
             "%s/Items/%s/Images/Primary?maxWidth=%d&maxHeight=%d&format=Jpg&quality=80",
             session->server_url, image_item_id, max_width, max_height);
}

bool jfin_report_start(const jfin_session_t *session, const char *item_id)
{
    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url), "%s/Sessions/Playing", session->server_url);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "ItemId", item_id);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return true; /* OOM — skip rather than POST empty body */

    cJSON *resp = api_post(session, url, body_str);
    free(body_str);
    if (resp) cJSON_Delete(resp);

    return true; /* reporting is best-effort */
}

bool jfin_report_progress(const jfin_session_t *session, const char *item_id,
                          int64_t position_ticks, bool is_paused)
{
    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url), "%s/Sessions/Playing/Progress", session->server_url);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "ItemId", item_id);
    cJSON_AddNumberToObject(body, "PositionTicks", (double)position_ticks);
    cJSON_AddBoolToObject(body, "IsPaused", is_paused);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return true; /* OOM — skip rather than POST empty body */

    cJSON *resp = api_post(session, url, body_str);
    free(body_str);
    if (resp) cJSON_Delete(resp);

    return true;
}

bool jfin_report_stop(const jfin_session_t *session, const char *item_id,
                      int64_t position_ticks)
{
    char url[JFIN_URL_BUF];
    snprintf(url, sizeof(url), "%s/Sessions/Playing/Stopped", session->server_url);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "ItemId", item_id);
    cJSON_AddNumberToObject(body, "PositionTicks", (double)position_ticks);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return true; /* OOM — skip rather than POST empty body */

    cJSON *resp = api_post(session, url, body_str);
    free(body_str);
    if (resp) cJSON_Delete(resp);

    return true;
}
