#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
/* ShadowMountPlus owns installation and reports progress through its local API. */
#include "packages.h"
#include <curl/curl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STREAM_CHUNK_LIMIT (1024 * 1024)

bool package_platform_available(bool stream) {
    return shadowmount_package_support(stream);
}

static void stream_name(const char *name, char out[256]) {
    const char *suffix = strrchr(name, '.');
    /* ShadowMountPlus's stream API requires a .pkg name even for an FPKG. */
    snprintf(out, 256, "%.*s.pkg", suffix ? (int)(suffix - name) : (int)strlen(name), name);
}

int package_platform_submit(const char *source, const char *name, int64_t size,
                            int64_t *job, char token[33], char *error, size_t cap) {
    bool stream = size > 0;
    if (!package_platform_available(stream)) {
        copy_text(error, cap, "Enable the local API on a ShadowMountPlus version that supports package installation.");
        return -1;
    }
    char stream_file[256];
    if (stream) stream_name(name, stream_file);
    cJSON *input = cJSON_CreateObject();
    bool encoded = input && (stream ? cJSON_AddStringToObject(input, "name", stream_file) &&
        cJSON_AddNumberToObject(input, "size", (double)size)
        : cJSON_AddStringToObject(input, "path", source) != NULL);
    char *body = encoded ? cJSON_PrintUnformatted(input) : NULL;
    cJSON_Delete(input);
    if (!body) {
        copy_text(error, cap, "Could not prepare the ShadowMountPlus installation request.");
        return -1;
    }

    cJSON *result = shadowmount_request(shadowmount_port(), stream ? "packages/stream" : "packages/install", body, 0, error, cap);
    free(body);
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(result, "status");
    int64_t id = json_int(result, "job_id");
    const char *key = json_text(result, "token");
    int rc = 1; /* No confirmation. A mutation may have reached the installer. */
    if ((cJSON_IsNumber(status) && status->valuedouble != 0) ||
        (error && !strncmp(error, "ShadowMount: ", 13) && !strstr(error, "(HTTP 5"))) {
        if (result) copy_text(error, cap, json_text(result, "error"));
        rc = -1;
    } else if (cJSON_IsNumber(status) && id > 0 &&
               (!stream || (strlen(key) == 32 && strspn(key, "0123456789abcdefABCDEF") == 32))) {
        *job = id;
        if (stream) copy_text(token, 33, key);
        rc = 0;
    }
    cJSON_Delete(result);
    return rc;
}

int package_platform_poll(int64_t job, bool stream, const char *source, PackageProgress *result) {
    *result = (PackageProgress){0};
    if (job <= 0) return -1;
    char body[64];
    snprintf(body, sizeof body, "{\"job_id\":%lld}", (long long)job);
    cJSON *response = shadowmount_request(shadowmount_port(), "games/storage/status", body, 0, NULL, 0);
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(response, "status");
    const char *state = json_text(response, "state");
    const char *reported = json_text(response, "source");
    char normalized[PATH_MAX];
    bool same_source = !strcmp(reported, source);
    if (stream) {
        char stream_file[256];
        stream_name(source, stream_file);
        same_source = !strcmp(reported, stream_file);
    } else if (!same_source && realpath(source, normalized)) {
        same_source = !strcmp(reported, normalized);
    }
    /* A different job may have taken this slot, especially after a restart. */
    bool valid = cJSON_IsNumber(status) && status->valuedouble == 0 && json_int(response, "job_id") == job &&
        !strcmp(json_text(response, "operation"), "install") &&
        !strcmp(json_text(response, "source_type"), stream ? "stream" : "pkg") &&
        same_source &&
        (!strcmp(state, "preparing") || !strcmp(state, "installing") || !strcmp(state, "finalizing") ||
         !strcmp(state, "completed") || !strcmp(state, "failed") || !strcmp(state, "cancelled"));
    if (valid) {
        copy_text(result->state, sizeof result->state, state);
        copy_text(result->message, sizeof result->message, json_text(response, "result_error"));
        int64_t received = json_int(response, "processed_bytes"), total = json_int(response, "total_bytes");
        result->received = received > 0 ? received : 0;
        result->total = total > 0 ? total : 0;
        result->error = (int)json_int(response, "native_status");
        if (!result->error) result->error = (int)json_int(response, "result_status");
    }
    cJSON_Delete(response);
    return valid ? 0 : -1;
}

typedef struct { char *data; size_t size, used; } Range;
static size_t receive_range(char *data, size_t size, size_t count, void *opaque) {
    Range *r = opaque;
    if (size && count > (r->size - r->used) / size) return 0;
    size_t bytes = size * count;
    memcpy(r->data + r->used, data, bytes);
    r->used += bytes;
    return bytes;
}

static bool read_range(CURL *curl, const char *url, int64_t offset, int64_t total, Range *range) {
    char requested[64];
    snprintf(requested, sizeof requested, "%lld-%lld", (long long)offset, (long long)(offset + (int64_t)range->size - 1));
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_RANGE, requested);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_range);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, range);
    long status = 0;
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    struct curl_header *header = NULL;
    long long first, last, length;
    return rc == CURLE_OK && status == 206 && range->used == range->size &&
        curl_easy_header(curl, "Content-Range", 0, CURLH_HEADER, -1, &header) == CURLHE_OK &&
        sscanf(header->value, "bytes %lld-%lld/%lld", &first, &last, &length) == 3 &&
        first == offset && last == offset + (int64_t)range->size - 1 && length == total;
}

/* ShadowMountPlus requests ranges from Orbit Zero's existing peer-bound HTTP
 * stream. The payload relays them over the documented JSON/binary range API. */
void package_platform_stream(const char *url, int64_t total, const char token[33]) {
    int port = shadowmount_port();
    if (!port || !*token || total <= 0) return;
    CURL *curl = curl_easy_init();
    char *buffer = malloc(STREAM_CHUNK_LIMIT);
    if (!curl || !buffer) goto done;
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    char body[80];
    snprintf(body, sizeof body, "{\"token\":\"%s\",\"client\":1}", token);
    /* Bound each turn so the receipt worker can report progress and shut down. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    time_t until = now.tv_sec + 2;
    do {
        cJSON *request = shadowmount_request(port, "packages/stream/request", body, 0, NULL, 0);
        int64_t id = json_int(request, "id"), offset = json_int(request, "offset"), size = json_int(request, "size");
        cJSON_Delete(request);
        if (id <= 0) break;
        if (offset < 0 || size <= 0 || size > STREAM_CHUNK_LIMIT || size > total || offset > total - size) break;
        Range range = {buffer, (size_t)size, 0};
        if (!read_range(curl, url, offset, total, &range)) break;
        char route[120];
        snprintf(route, sizeof route, "packages/stream/chunk?token=%s&id=%lld", token, (long long)id);
        cJSON *reply = shadowmount_request(port, route, buffer, range.size, NULL, 0);
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(reply, "status");
        bool accepted = cJSON_IsNumber(status) && status->valuedouble == 0;
        cJSON_Delete(reply);
        if (!accepted) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec < until && !orbit_is_stopping());
done:
    if (curl) curl_easy_cleanup(curl);
    free(buffer);
}
