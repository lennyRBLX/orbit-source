#include "catalog_view.h"
#include "firmware.h"
#include "orbit.h"
#include "packages.h"
#include "remote_sources.h"

#include <assert.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The fixture uses the real catalogue, settings, URL policies and queue
 * snapshots. Console firmware, receipts and browser sessions are absent. */
bool orbit_is_stopping(void) { return orbit.stop; }
bool firmware_known(uint32_t raw) { return raw != 0; }
void firmware_describe(cJSON *row, uint32_t raw) { (void)row; (void)raw; }
void firmware_describe_download(cJSON *row, const Job *job, uint32_t raw) {
    (void)row; (void)job; (void)raw;
}
cJSON *packages_for_download(const char *origin) { (void)origin; return NULL; }
bool job_active(const Job *j) { return !strcmp(j->status, "downloading"); }
bool job_can_refresh_link(const Job *j) { (void)j; return false; }
const char *notify_event_name(unsigned index) { (void)index; return NULL; }
void browser_source_changed_locked(void) {}
static atomic_llong scheduler_clock;
int64_t orbit_remote_test_clock(void) { return atomic_load(&scheduler_clock); }

static cJSON *find_row(const cJSON *snapshot, const char *id) {
    cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(snapshot, "sources"))
        if (!strcmp(json_text(row, "id"), id)) return row;
    return NULL;
}

static void print_json(cJSON *value) {
    assert(value);
    char *json = cJSON_PrintUnformatted(value);
    assert(json);
    puts(json);
    free(json);
    cJSON_Delete(value);
}

static bool equal_catalogue(const CatalogBody *a, const CatalogBody *b) {
    return catalog_body_size(a) == catalog_body_size(b) &&
           !memcmp(catalog_body_data(a), catalog_body_data(b), catalog_body_size(a));
}

static CatalogBody *view(bool addons) {
    bool gzip = false;
    CatalogBody *body = catalog_view_get(addons, &gzip);
    assert(body && !gzip);
    return body;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    orbit.state_fd = open(argv[1], O_RDONLY | O_DIRECTORY);
    assert(orbit.state_fd >= 0);
    assert(!curl_global_init(CURL_GLOBAL_DEFAULT));
    assert(!catalog_init());
    assert(!remote_sources_load());
    assert(!state_load());

    SourceSettings loaded = orbit.sources;
    SourceSettings enabled = {ORBIT_SOURCE_ARCHIVE | ORBIT_SOURCE_VIKINGFILE | ORBIT_SOURCE_AKIRABOX |
                              ORBIT_SOURCE_DATANODES, ORBIT_SOURCE_NOTICE_VERSION, 1};
    orbit.sources = enabled;
    CatalogBody *base = view(false), *base_addons = view(true);
    orbit.sources = loaded;
    const cJSON *first = cJSON_GetArrayItem(orbit.catalog, 0);
    printf("{\"titleId\":\"%s\",\"gameId\":\"%s\"}\n", json_text(first, "titleId"), json_text(first, "gameId"));
    fflush(stdout);
    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        cJSON *request = cJSON_Parse(line);
        assert(request);
        const char *op = json_text(request, "op");
        if (!strcmp(op, "view")) {
            CatalogBody *body = view(false), *addons = view(true);
            const unsigned char *data = catalog_body_data(body);
            cJSON *result = cJSON_CreateObject(), *rows = cJSON_AddArrayToObject(result, "remote");
            cJSON_AddBoolToObject(result, "sameGames", equal_catalogue(base, body));
            cJSON_AddBoolToObject(result, "sameAddons", equal_catalogue(base_addons, addons));
            cJSON *catalogue = cJSON_ParseWithLength((const char *)data, catalog_body_size(body)), *row;
            assert(cJSON_IsArray(catalogue));
            cJSON_ArrayForEach(row, catalogue)
                if (!strcmp(json_text(row, "sourceId"), "remote"))
                    cJSON_AddItemToArray(rows, cJSON_Duplicate(row, true));
            print_json(result);
            cJSON_Delete(catalogue);
            catalog_body_release(addons);
            catalog_body_release(body);
        } else if (!strcmp(op, "start")) {
            printf("%d\n", remote_sources_start());
        } else if (!strcmp(op, "advance")) {
            atomic_fetch_add(&scheduler_clock, json_int(request, "milliseconds"));
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "tick"))) remote_sources_tick();
            puts("true");
        } else if (!strcmp(op, "map") || !strcmp(op, "resolve")) {
            Release release, saved;
            const cJSON *row = cJSON_GetObjectItemCaseSensitive(request, "entry");
            int code = pegasus_entry_to_release(row, "evox-coreos", &release);
            if (!code && !strcmp(op, "resolve")) {
                Release locator;
                char error[256] = "";
                assert(remote_page_pending(&release));
                code = remote_download_resolve(&release, &locator, NULL, error, sizeof error);
            }
            if (code) printf("%d\n", code);
            else {
                cJSON *snapshot = release_json(&release);
                if (!remote_page_pending(&release)) {
                    assert(!remote_release_parse(snapshot, &saved));
                    assert(!memcmp(&release, &saved, sizeof release));
                }
                print_json(snapshot);
            }
        } else if (!strcmp(op, "save-job")) {
            pthread_mutex_lock(&orbit.mutex);
            const Release *release = release_find(json_text(request, "id"));
            assert(release);
            Job *j = &orbit.jobs[orbit.job_count++];
            j->release = *release;
            copy_text(j->id, sizeof j->id, "0123456789abcdef");
            copy_text(j->release_id, sizeof j->release_id, release->id);
            copy_text(j->status, sizeof j->status, "paused");
            copy_text(j->delivery, sizeof j->delivery, "direct");
            copy_text(j->filename, sizeof j->filename, release->filename);
            j->inspected_size = 41;
            copy_text(j->inspected_format, sizeof j->inspected_format, "Game folder");
            printf("%d\n", state_save_locked());
            pthread_mutex_unlock(&orbit.mutex);
        } else {
            pthread_mutex_lock(&orbit.mutex);
            if (!strcmp(op, "policy")) {
                orbit.sources = enabled;
                if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "remote")))
                    orbit.sources.enabled |= ORBIT_SOURCE_REMOTE;
                if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "notice"))) {
                    orbit.sources.notice_version = 0;
                    orbit.sources.acknowledged_at = 0;
                }
                remote_sources_changed_locked();
                print_json(remote_sources_json_locked());
            } else if (!strcmp(op, "selection")) {
                print_json(sources_json_locked());
            } else if (!strcmp(op, "save-selection")) {
                char message[128] = "";
                printf("%d\n", sources_set_locked(request, message, sizeof message));
            } else if (!strcmp(op, "show")) {
                print_json(remote_sources_json_locked());
            } else if (!strcmp(op, "jobs")) {
                print_json(jobs_json_locked());
            } else if (!strcmp(op, "metadata")) {
                print_json(remote_metadata_json_locked(json_text(request, "ids")));
            } else if (!strcmp(op, "diagnostics")) {
                print_json(remote_sources_diagnostics_locked());
            } else {
                char message[128] = "";
                const char *id = json_text(request, "id");
                int code = remote_sources_change_locked(op, id, request, message, sizeof message);
                cJSON *response = remote_sources_json_locked();
                cJSON_AddNumberToObject(response, "status", code);
                const cJSON *row = find_row(response, id);
                if (row) cJSON_AddBoolToObject(response, "found", true);
                print_json(response);
            }
            pthread_mutex_unlock(&orbit.mutex);
        }
        cJSON_Delete(request);
        fflush(stdout);
    }
    orbit.stop = true;
    remote_sources_stop();
    catalog_body_release(base);
    catalog_body_release(base_addons);
    catalog_view_clear();
    cJSON_Delete(orbit.catalog);
    cJSON_Delete(orbit.addons);
    close(orbit.state_fd);
    curl_global_cleanup();
    return 0;
}
