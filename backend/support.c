/* Public funding configuration. Never fetch a user-supplied destination here. */
#define _POSIX_C_SOURCE 200809L
#include "orbit.h"
#include "support.h"
#include "qrcodegen.h"
#include "ca.h"
#include <curl/curl.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FUNDING_URL "https://raw.githubusercontent.com/saawant12/orbit-store-ps5/main/.github/FUNDING.yml"
#define FALLBACK "custom: [\"https://github.com/saawant12/orbit-store-ps5/issues/131#user-content-support-with-crypto\"]\n"
#define MAX_YAML 16384
static pthread_mutex_t support_lock = PTHREAD_MUTEX_INITIALIZER;
static char saved[MAX_YAML + 1] = FALLBACK;
static time_t checked;
static bool loaded;
static const struct { const char *key, *label, *base; } providers[] = {
    {"github", "GitHub Sponsors", "https://github.com/sponsors/"},
    {"ko_fi", "Ko-fi", "https://ko-fi.com/"},
    {"patreon", "Patreon", "https://www.patreon.com/"},
    {"open_collective", "Open Collective", "https://opencollective.com/"},
    {"liberapay", "Liberapay", "https://liberapay.com/"},
    {"issuehunt", "IssueHunt", "https://issuehunt.io/r/"},
    {"community_bridge", "LFX Mentorship", "https://mentorship.lfx.linuxfoundation.org/project/"},
    {"tidelift", "Tidelift", "https://tidelift.com/funding/github/"},
    {"polar", "Polar", "https://polar.sh/"},
    {"thanks_dev", "thanks.dev", "https://thanks.dev/"},
};
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}
static void option(cJSON *links, const char *key, char *raw) {
    char *s = trim(raw); size_t n = strlen(s);
    if (n >= 2 && ((*s == '\'' && s[n-1] == '\'') || (*s == '"' && s[n-1] == '"'))) {
        s[n-1] = 0; s++; n -= 2;
    }
    if (!n || n > 400 || cJSON_GetArraySize(links) >= 16 || !strcmp(s, "null") || !strcmp(s, "~")) return;
    char url[512], label[128]; url[0] = label[0] = 0;
    if (!strcmp(key, "custom")) {
        /* Literal HTTPS public DNS names only; no credentials, ports or ambiguous escapes. */
        if (strncmp(s, "https://", 8) || strpbrk(s, "\\\r\n\t <>\"'@")) return;
        const char *host = s + 8; size_t h = strcspn(host, "/?#");
        if (!h || h >= sizeof label || !memchr(host, '.', h)) return;
        bool letter = false;
        for (size_t i = 0; i < h; i++) {
            unsigned char c = (unsigned char)host[i];
            if (!isalnum(c) && c != '-' && c != '.') return;
            if (isalpha(c)) letter = true;
        }
        if (!letter || host[0] == '.' || host[h-1] == '.') return;
        snprintf(label, sizeof label, "%.*s", (int)h, host);
        if (strstr(label, ".local") || strstr(label, ".localhost") || strstr(label, ".internal")) return;
        snprintf(url, sizeof url, "%s", s);
        if (!strcmp(url, "https://github.com/saawant12/orbit-store-ps5/issues/131#user-content-support-with-crypto")) snprintf(label, sizeof label, "%s", "Support with crypto");
    } else {
        if (strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_/.") != n ||
            strstr(s, "..") || *s == '/' || s[n-1] == '/') return;
        for (size_t i = 0; i < sizeof providers / sizeof *providers; i++) if (!strcmp(key, providers[i].key)) {
            snprintf(url, sizeof url, "%s%s", providers[i].base, s);
            snprintf(label, sizeof label, "%s", providers[i].label); break;
        }
    }
    if (!*url) return;
    const cJSON *p; cJSON_ArrayForEach(p, links) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(p, "url");
        if (cJSON_IsString(u) && !strcmp(u->valuestring, url)) return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "label", label); cJSON_AddStringToObject(o, "url", url);
    /* Matrix includes a four-module quiet zone at render time, never remote QR services. */
    uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(15)], qr[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
    if (qrcodegen_encodeText(url, temp, qr, qrcodegen_Ecc_MEDIUM, 1, 15, qrcodegen_Mask_AUTO, true)) {
        cJSON *rows = cJSON_AddArrayToObject(o, "qr"); int size = qrcodegen_getSize(qr);
        for (int y = 0; y < size; y++) {
            char row[78]; for (int x = 0; x < size; x++) row[x] = qrcodegen_getModule(qr, x, y) ? '1' : '0';
            row[size] = 0; cJSON_AddItemToArray(rows, cJSON_CreateString(row));
        }
    }
    cJSON_AddItemToArray(links, o);
}
cJSON *support_parse(const char *yaml) {
    cJSON *links = cJSON_CreateArray();
    if (!yaml || strlen(yaml) > MAX_YAML) return links;
    char *copy = strdup(yaml), *save = NULL, key[48] = "";
    if (!copy) return links;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char quote = 0;
        for (char *p = line; *p; p++) {
            if ((*p == '\'' || *p == '"') && (!quote || quote == *p)) quote = quote ? 0 : *p;
            else if (*p == '#' && !quote && (p == line || isspace((unsigned char)p[-1]))) { *p = 0; break; }
        }
        char *s = trim(line); if (!*s || !strcmp(s, "---")) continue;
        if (*s == '-' && isspace((unsigned char)s[1])) { option(links, key, s+1); continue; }
        char *colon = strchr(s, ':'); if (!colon) { *key = 0; continue; }
        *colon = 0; if (strlen(trim(s)) >= sizeof key) { *key = 0; continue; }
        snprintf(key, sizeof key, "%s", trim(s)); s = trim(colon+1);
        if (*s != '[') { option(links, key, s); continue; }
        size_t n = strlen(s); if (n < 2 || s[n-1] != ']') continue;
        s[n-1] = 0; char *start = ++s; quote = 0;
        for (char *p = s;; p++) {
            if ((*p == '\'' || *p == '"') && (!quote || quote == *p)) quote = quote ? 0 : *p;
            if (!*p || (*p == ',' && !quote)) {
                bool end = !*p; *p = 0; option(links, key, start); if (end) break; start = p+1;
            }
        }
    }
    free(copy); return links;
}
typedef struct { char data[MAX_YAML + 1]; size_t size; } Buffer;
static size_t collect(void *data, size_t a, size_t b, void *ctx) {
    Buffer *buf = ctx; if (a && b > MAX_YAML / a) return 0;
    size_t n = a*b; if (n > MAX_YAML - buf->size) return 0;
    memcpy(buf->data + buf->size, data, n); buf->size += n; buf->data[buf->size] = 0; return n;
}
static bool valid(const char *yaml) { cJSON *p = support_parse(yaml); bool ok = cJSON_GetArraySize(p) > 0; cJSON_Delete(p); return ok; }
cJSON *support_status(void) {
    pthread_mutex_lock(&support_lock);
    if (!loaded) {
        loaded = true; int fd = openat(orbit.state_fd, "funding.yml", O_RDONLY | O_NOFOLLOW);
        if (fd >= 0) { char b[MAX_YAML + 2]; ssize_t n = read(fd, b, sizeof b - 1); close(fd);
            if (n > 0 && n <= MAX_YAML) { b[n] = 0; if (valid(b)) snprintf(saved, sizeof saved, "%s", b); }
        }
    }
    time_t now = time(NULL);
    if (!checked || now < checked || now - checked >= 21600) {
        checked = now; Buffer b = {0}; CURL *c = curl_easy_init(); long status = 0;
        if (c) {
            struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
            curl_easy_setopt(c, CURLOPT_URL, FUNDING_URL); curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
            curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https"); curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L); curl_easy_setopt(c, CURLOPT_TIMEOUT, 6L);
            curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &b); curl_easy_setopt(c, CURLOPT_USERAGENT, "Orbit-Store/" ORBIT_VERSION);
            CURLcode rc = curl_easy_perform(c); curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status); curl_easy_cleanup(c);
            if (rc == CURLE_OK && status == 200 && !memchr(b.data, 0, b.size) && valid(b.data)) {
                snprintf(saved, sizeof saved, "%s", b.data);
                int fd = openat(orbit.state_fd, "funding.yml.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
                if (fd >= 0) { bool ok = write(fd, saved, strlen(saved)) == (ssize_t)strlen(saved); close(fd);
                    if (ok) renameat(orbit.state_fd, "funding.yml.tmp", orbit.state_fd, "funding.yml"); }
            }
        }
    }
    cJSON *o = cJSON_CreateObject(); cJSON_AddItemToObject(o, "links", support_parse(saved));
    pthread_mutex_unlock(&support_lock); return o;
}
