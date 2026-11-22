/* The console's firmware, and what it means for a download. Nothing here hides or blocks a
 * download: clients label the options that won't run on this console and list them last. */
#include "firmware.h"
#include "kraken.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* PS5 fake packages run on 11.60 or lower; above it they install but don't start. */
#define PS5_PACKAGE_LIMIT 0x11600000u
#define PS5_PACKAGE_LIMIT_TEXT "11.60"

/* The SDK reports 12.60 as 0x12600000: two BCD digits each for the major and minor version. */
static bool bcd_byte(unsigned value) { return (value >> 4) < 10 && (value & 15) < 10; }
static unsigned from_bcd(unsigned value) { return (value >> 4) * 10 + (value & 15); }

bool firmware_known(uint32_t raw) {
    unsigned major = raw >> 24, minor = (raw >> 16) & 0xff;
    return major && bcd_byte(major) && bcd_byte(minor);
}
void firmware_text(uint32_t raw, char *out, size_t cap) {
    if (!cap) return;
    *out = 0;
    if (firmware_known(raw))
        snprintf(out, cap, "%u.%02u", from_bcd(raw >> 24), from_bcd((raw >> 16) & 0xff));
}
const char *firmware_package_limit(void) { return PS5_PACKAGE_LIMIT_TEXT; }

/* "10.xx", "[11.xx+]" and "[BP4.xx]" in a release's filename name the oldest firmware it runs
 * on. Version numbers like v01.004 never end in "xx", so they don't match. */
int firmware_required(const char *name) {
    for (const char *p = name; p && *p; p++) {
        if (!isdigit((unsigned char)*p) || (p > name && (isdigit((unsigned char)p[-1]) || p[-1] == '.')))
            continue;
        int major = 0, digits = 0;
        const char *q = p;
        while (isdigit((unsigned char)*q) && digits < 3) major = major * 10 + (*q++ - '0'), digits++;
        if (digits > 2 || strncasecmp(q, ".xx", 3) || isalnum((unsigned char)q[3])) continue;
        if (major >= 1 && major <= 30) return major;
    }
    return -1;
}

static bool ps5_package(const cJSON *release) {
    const char *format = json_text(release, "format");
    return !*json_text(release, "addonId") && !strcmp(json_text(release, "platform"), "ps5") &&
           (!strcmp(format, "PKG") || !strcmp(format, "FPKG"));
}

void firmware_describe(cJSON *entry, uint32_t raw) {
    const char *issue = NULL;
    /* On a PS4 option, "N.xx" is a PS4 firmware: nothing to compare with the PS5's. */
    int required = strcmp(json_text(entry, "platform"), "ps5") ? -1 : firmware_required(json_text(entry, "filename"));
    if (required > 0) {
        char text[8];
        snprintf(text, sizeof text, "%d.xx", required);
        cJSON_AddStringToObject(entry, "requiredFirmware", text);
        if (firmware_known(raw) && from_bcd(raw >> 24) < (unsigned)required) issue = "newer-firmware";
    }
    /* Kraken Mount runs PS5 packages on any firmware it supports, without installing them. */
    if (ps5_package(entry) && kraken_active()) cJSON_AddStringToObject(entry, "mounter", "kraken");
    else if (ps5_package(entry)) {
        cJSON_AddStringToObject(entry, "maxFirmware", PS5_PACKAGE_LIMIT_TEXT);
        if (firmware_known(raw) && (raw & 0xffff0000u) > PS5_PACKAGE_LIMIT) issue = "ps5-package";
    }
    if (issue) cJSON_AddStringToObject(entry, "firmwareIssue", issue);
}

static uint32_t param_firmware(int parent, const char *name) {
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (fd < 0) return 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 2 || st.st_size > 131072) { close(fd); return 0; }
    char *data = calloc(1, (size_t)st.st_size + 1);
    size_t used = 0;
    if (data) while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, data + used, (size_t)st.st_size - used);
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);
    cJSON *p = data && used == (size_t)st.st_size ? cJSON_ParseWithLength(data, used + 1) : NULL;
    free(data);
    const char *text = json_text(p, "requiredSystemSoftwareVersion");
    uint32_t value = 0;
    if (strlen(text) == 18 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') &&
        strspn(text + 2, "0123456789abcdefABCDEF") == 16) {
        char first[5]; memcpy(first, text + 2, 4); first[4] = 0;
        value = (uint32_t)strtoul(first, NULL, 16) << 16;
        if (!firmware_known(value)) value = 0;
    }
    cJSON_Delete(p);
    return value;
}
static uint32_t walk_params(int parent, const char *name, unsigned depth, unsigned *visited, dev_t device) {
    if (depth > 8 || ++*visited > 4096) return 0;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) return 0;
    if (fstat(fd, &st) || st.st_dev != device) { close(fd); return 0; }
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return 0; }
    uint32_t highest = !strcmp(name, "sce_sys") ? param_firmware(fd, "param.json") : 0;
    struct dirent *entry;
    while (*visited < 4096 && (entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        uint32_t value = walk_params(fd, entry->d_name, depth + 1, visited, device);
        if (value > highest) highest = value;
    }
    closedir(d);
    return highest;
}
uint32_t firmware_extracted(int parent, const char *name) {
    struct stat st; unsigned visited = 0;
    if (fstat(parent, &st)) return 0;
    return walk_params(parent, name, 0, &visited, st.st_dev);
}
/* ShadowMount+ applies a backport from backports/<TITLE_ID>/ by its fakelib folder. Releases
 * name and nest the outer folder differently, so only that folder identifies one. */
static bool walk_backport(int parent, const char *name, unsigned depth, unsigned *visited, dev_t device) {
    if (depth > 8 || ++*visited > 4096) return false;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) return false;
    if (fstat(fd, &st) || st.st_dev != device) { close(fd); return false; }
    if (depth && (!strcasecmp(name, "fakelib") || !strcasecmp(name, "fakelib2"))) { close(fd); return true; }
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return false; }
    bool found = false;
    struct dirent *entry;
    while (!found && *visited < 4096 && (entry = readdir(d)))
        if (entry->d_name[0] != '.') found = walk_backport(fd, entry->d_name, depth + 1, visited, device);
    closedir(d);
    return found;
}
bool firmware_backport(int parent, const char *name) {
    struct stat st; unsigned visited = 0;
    if (fstat(parent, &st)) return false;
    return walk_backport(parent, name, 0, &visited, st.st_dev);
}
void firmware_describe_download(cJSON *entry, const Job *job, uint32_t raw) {
    cJSON *facts = release_json(&job->release);
    if (job->inspected_format[0])
        cJSON_ReplaceItemInObjectCaseSensitive(facts, "format", cJSON_CreateString(job->inspected_format));
    firmware_describe(facts, raw);
    if (firmware_known(job->extracted_firmware) && !strcmp(job->release.platform, "ps5")) {
        char required[16]; firmware_text(job->extracted_firmware, required, sizeof required);
        cJSON_DeleteItemFromObjectCaseSensitive(facts, "requiredFirmware");
        cJSON_AddStringToObject(facts, "requiredFirmware", required);
        cJSON_AddStringToObject(entry, "firmwareSource", "param");
        if (strcmp(json_text(facts, "firmwareIssue"), "ps5-package")) {
            cJSON_DeleteItemFromObjectCaseSensitive(facts, "firmwareIssue");
            if (firmware_known(raw) && (raw & 0xffff0000u) < job->extracted_firmware)
                cJSON_AddStringToObject(facts, "firmwareIssue", "newer-firmware");
        }
    }
    const char *keys[] = {"requiredFirmware", "maxFirmware", "firmwareIssue", "mounter"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(facts, keys[i]);
        if (v) cJSON_AddItemToObject(entry, keys[i], cJSON_Duplicate(v, true));
    }
    if (job->extracted_backport && !strcmp(job->release.platform, "ps5")) cJSON_AddTrueToObject(entry, "backport");
    char current[16]; firmware_text(raw, current, sizeof current);
    if (*current) cJSON_AddStringToObject(entry, "consoleFirmware", current);
    cJSON_Delete(facts);
}
