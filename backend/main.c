#include "orbit.h"
#include "remote_sources.h"
#include "bundle.h"
#include "packages.h"
#include "receive.h"
#include "zero_probe.h"
#include "zero_link.h"
#include "debrid.h"
#include "archive_account.h"
#include "viking_pass.h"
#include "ntfy.h"
#include "files.h"
#include "power.h"
#include "history.h"
#include "kraken.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef ORBIT_DESKTOP
#include <ps5/kernel.h>
#endif
static volatile sig_atomic_t stopping, signalled;
static atomic_bool stop_requested;
static atomic_int restart_loader = -1;
/* Written just before a restart Orbit started itself hands over; the new process waits a
 * little for the old one's lock and port instead of reporting a running copy. */
#define HANDOFF "restart-handoff"
#define HANDOFF_SECONDS 30
#ifdef ORBIT_INSTALL_LAUNCHER
#include "install.h"
#endif
#ifdef ORBIT_EMBED_RUNTIME
/* The runtime this payload saves on the console; see launcher/runtime_image.c. */
extern const unsigned char orbit_runtime_image[], orbit_runtime_image_end[];
#endif
int pairing_notify(void) {
#if defined(ORBIT_DESKTOP) && !defined(ORBIT_TEST)
    return -1;
#else
    char message[160];
    snprintf(message, sizeof message,
             "Orbit Store pairing code: %s\nOpen Pair devices in Orbit to keep the code on screen.",
             orbit.pair_code);
    return notify_console(message);
#endif
}
static void stop_signal(int n) {
    (void)n;
    signalled = 1;
    stopping = 1;
}
void orbit_restart_after_stop(int loader) {
    atomic_store(&restart_loader, loader);
}
static void handoff_wait(void) {
    struct timespec quarter = {.tv_nsec = 250000000};
    nanosleep(&quarter, NULL);
}
static bool handoff_pending(int state_fd) {
    struct stat st;
    time_t now = time(NULL);
    return !fstatat(state_fd, HANDOFF, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) &&
           st.st_mtime <= now && now - st.st_mtime < HANDOFF_SECONDS;
}
static void handoff_mark(int state_fd) {
    /* A fresh, empty file: a name linked to anything else is replaced, never written through. */
    unlinkat(state_fd, HANDOFF, 0);
    int fd = openat(state_fd, HANDOFF, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}
void orbit_request_stop(void) {
    atomic_store(&stop_requested, true);
}
bool orbit_is_stopping(void) {
    return stopping || atomic_load(&stop_requested);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    copy_text(orbit.state_dir, sizeof orbit.state_dir, "/data/orbit-store");
    const unsigned char *runtime = NULL;
    size_t runtime_size = 0;
#ifdef ORBIT_EMBED_RUNTIME
    runtime = orbit_runtime_image;
    runtime_size = (size_t)(orbit_runtime_image_end - orbit_runtime_image);
#endif
#ifdef ORBIT_DESKTOP
    orbit.desktop = true;
    copy_text(orbit.state_dir, sizeof orbit.state_dir, ".state");
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--state"))
            copy_text(orbit.state_dir, sizeof orbit.state_dir, argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--storage"))
            copy_text(orbit.desktop_storage, sizeof orbit.desktop_storage, argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--port"))
            orbit.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--listen-all"))
            orbit.listen_all = true;
        else if (i + 1 < argc && !strcmp(argv[i], "--system-root")) {
            copy_text(orbit.system_root, sizeof orbit.system_root, argv[++i]);
            orbit.autoboot = true;
        } else if (i + 1 < argc && !strcmp(argv[i], "--runtime-image")) {
            unsigned char *image;
            if (read_regular_file(argv[++i], 64 * 1024 * 1024, &image, &runtime_size)) {
                perror("runtime image");
                return 1;
            }
            runtime = image;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }
#else
    (void)argc;
    (void)argv;
    orbit.autoboot = true;
#endif
    if (orbit.port < 1024 || orbit.port > 65535 || catalog_init()) {
        fputs("Invalid configuration/catalogue.\n", stderr);
        return 1;
    }
#ifdef ORBIT_TEST
    /* The SDK's encoding, e.g. 0x13600000 for 13.60. */
    const char *test_firmware = getenv("ORBIT_TEST_FIRMWARE");
    if (test_firmware) orbit.firmware = (uint32_t)strtoul(test_firmware, NULL, 16);
    const char *url = getenv("ORBIT_TEST_URL"), *size = getenv("ORBIT_TEST_SIZE"),
               *sha = getenv("ORBIT_TEST_SHA256");
    if (!url || strncmp(url, "http://127.0.0.1:", 17) || !size) {
        fputs("Tests require a loopback fixture URL and size.\n", stderr);
        return 1;
    }
    const char *addon_fixture = getenv("ORBIT_TEST_ADDON");
    if (addon_fixture) {
        size_t selected = 0;
        while (selected < orbit.release_count && strcmp(orbit.releases[selected].id, addon_fixture)) selected++;
        if (selected == orbit.release_count || !orbit.releases[selected].addon_id[0]) return 1;
        Release saved = orbit.releases[0]; orbit.releases[0] = orbit.releases[selected]; orbit.releases[selected] = saved;
    }
    copy_text(orbit.releases[0].url, sizeof orbit.releases[0].url, url);
    orbit.releases[0].size = strtoll(size, NULL, 10);
    copy_text(orbit.releases[0].sha256, sizeof orbit.releases[0].sha256, sha ? sha : "");
    const char *test_source = getenv("ORBIT_TEST_SOURCE");
    if (test_source && !source_flag(test_source)) return 1;
    if (test_source)
        copy_text(orbit.releases[0].source, sizeof orbit.releases[0].source, test_source);
    const char *browser_url = getenv("ORBIT_TEST_BROWSER_URL");
    if (browser_url) copy_text(orbit.releases[0].browser_url, sizeof orbit.releases[0].browser_url, browser_url);
    /* More releases served by the same fixture file (Get everything lists). */
    const char *also = getenv("ORBIT_TEST_ALSO");
    for (const char *id = also; id && *id;) {
        size_t n = strcspn(id, ",");
        Release *other = NULL;
        for (size_t i = 1; i < orbit.release_count && !other; i++)
            if (strlen(orbit.releases[i].id) == n && !strncmp(orbit.releases[i].id, id, n)) other = &orbit.releases[i];
        if (!other) return 1;
        copy_text(other->url, sizeof other->url, url);
        copy_text(other->browser_url, sizeof other->browser_url, "");
        other->size = orbit.releases[0].size;
        copy_text(other->sha256, sizeof other->sha256, orbit.releases[0].sha256);
        if (test_source) copy_text(other->source, sizeof other->source, test_source);
        id += n + (id[n] == ',');
    }
    cJSON *fixture = cJSON_GetArrayItem(orbit.catalog, 0);
    if (addon_fixture) {
        cJSON_ArrayForEach(fixture, orbit.addons)
            if (!strcmp(json_text(fixture, "id"), addon_fixture)) break;
        if (!fixture) return 1;
    }
    const char *compression = getenv("ORBIT_TEST_COMPRESSION"), *extracted = getenv("ORBIT_TEST_EXTRACTED_SIZE");
    if (compression) {
        /* The fixture file becomes a compressed option with the same stem. */
        Release *r = &orbit.releases[0];
        char *dot = strrchr(r->filename, '.');
        if (!dot || !extracted || strlen(compression) >= sizeof r->compression) return 1;
        snprintf(dot, sizeof r->filename - (size_t)(dot - r->filename), ".%s", compression);
        copy_text(r->compression, sizeof r->compression, compression);
        r->extracted_size = strtoll(extracted, NULL, 10);
        if (getenv("ORBIT_TEST_UNVERIFIED")) {
            r->archive_unverified = true;
            r->extracted_size = 0;
            copy_text(r->format, sizeof r->format, "Unknown");
            cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "format"), "Unknown");
            cJSON_AddStringToObject(fixture, "archiveContents", "unverified");
        }
        const char *asset_compression = getenv("ORBIT_TEST_ASSET_COMPRESSION");
        if (asset_compression) {
            if (strcmp(asset_compression, "ZSTD") || r->archive_unverified) return 1;
            copy_text(r->format, sizeof r->format, "Game folder");
            r->content_format[0] = 0;
            copy_text(r->asset_compression, sizeof r->asset_compression, asset_compression);
            cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "format"), r->format);
            cJSON_DeleteItemFromObjectCaseSensitive(fixture, "contentFormat");
            cJSON_AddStringToObject(fixture, "assetCompression", asset_compression);
        }
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "filename"), r->filename);
        cJSON_AddStringToObject(fixture, "compression", compression);
        if (!r->archive_unverified) cJSON_AddNumberToObject(fixture, "extractedBytes", (double)r->extracted_size);
        const char *password = getenv("ORBIT_TEST_ARCHIVE_PASSWORD");
        if (password) {
            copy_text(r->archive_password, sizeof r->archive_password, password);
            cJSON_AddStringToObject(fixture, "archivePassword", password);
        }
    }
    if (test_source)
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "sourceId"), test_source);
    copy_text(orbit.releases[0].title, sizeof orbit.releases[0].title, "Orbit local test file");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "title"),
                         "Orbit local test file");
    cJSON_SetValuestring(
        cJSON_GetObjectItemCaseSensitive(fixture, "description"),
        "A disposable local HTTP fixture for testing the download controls. No PS5 is connected.");
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(fixture, "sizeBytes"),
                         orbit.releases[0].size);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "provider"),
                         "Local test server");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "cover"), "/orbit.svg");
#endif
    if (mkdir(orbit.state_dir, 0700) != 0 && access(orbit.state_dir, F_OK) != 0) {
        perror("state directory");
        return 1;
    }
    orbit.state_fd = open(orbit.state_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (orbit.state_fd < 0) {
        perror("state directory");
        return 1;
    }
    int lock = openat(orbit.state_fd, "instance.lock", O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    if (lock < 0) {
        perror("Orbit state lock");
        return 1;
    }
    int busy = flock(lock, LOCK_EX | LOCK_NB), lock_error = errno;
    bool handoff = handoff_pending(orbit.state_fd);
    for (int wait = 0; busy && handoff && (lock_error == EWOULDBLOCK || lock_error == EAGAIN) &&
                       wait < 60; wait++) {
        handoff_wait();
        busy = flock(lock, LOCK_EX | LOCK_NB);
        lock_error = errno;
    }
    if (!busy && handoff) unlinkat(orbit.state_fd, HANDOFF, 0);
    if (busy) {
        int error = lock_error;
        close(lock);
        if (error == EWOULDBLOCK || error == EAGAIN) {
            /* A newer payload still replaces the saved copy for the next start. */
            int synced = autoboot_sync(runtime, runtime_size);
#ifdef ORBIT_INSTALL_LAUNCHER
            orbit_launcher_ensure(orbit.state_fd);
#endif
            close(orbit.state_fd);
            if (runtime && synced >= 0) {
                char message[256];
                snprintf(message, sizeof message,
                         "Orbit %s saved. The previous session is still running. Stop Orbit, then "
                         "run the saved payload to apply it.",
                         ORBIT_VERSION);
                puts(message);
                notify_console(message);
            } else if (runtime && synced < 0) {
                notify_console("Orbit is running, but the replacement could not be fully saved. "
                               "Retry the update; the current session is unchanged.");
            } else {
                puts("Orbit is already running. Open the Orbit Store icon; the current queue is "
                     "unchanged.");
                notify_console("Orbit Store is already running. Open its home-screen icon.");
            }
            return 0;
        }
        close(orbit.state_fd);
        fputs("Orbit state cannot be locked.\n", stderr);
        return 1;
    }
    diagnostics_begin();
    random_hex(orbit.instance_id, 16);
    diagnostics_stage("startup", "started", 0);
    diagnostics_stage("catalogue-cache", "started", 0);
    catalog_load_cache();
    diagnostics_stage("catalogue-cache", "ok", 0);
    diagnostics_stage("https", "started", 0);
    int startup_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    diagnostics_stage("https", startup_result ? "error" : "ok", startup_result);
    if (!startup_result) {
        diagnostics_stage("saved-state", "started", 0);
        int remote_result = remote_sources_load();
        diagnostics_stage("remote-sources", remote_result ? "error" : "ok", remote_result);
        startup_result = state_load();
        diagnostics_stage("saved-state", startup_result ? "error" : "ok", startup_result);
    }
    if (!startup_result) {
        pthread_mutex_lock(&orbit.mutex);
        storage_mark_jobs_locked();
        pthread_mutex_unlock(&orbit.mutex);
    }
    if (startup_result) {
        fputs("Failed to initialize HTTPS or read saved state. Preserving state for recovery.\n",
              stderr);
        return 1;
    }
    diagnostics_stage("process-identity", "started", 0);
    orbit.process_identity = orbit_process_identity();
    diagnostics_stage("process-identity",
                      orbit.process_identity.state == ORBIT_PROCESS_UNCHECKED ? "skipped" : "ok", 0);
    diagnostics_stage("pairing", "started", 0);
    char random[9];
    random_hex(random, 4);
    unsigned long code = strtoul(random, NULL, 16) % 1000000;
    snprintf(orbit.pair_code, sizeof orbit.pair_code, "%06lu", code);
    printf("Orbit Store %s | %s | port %d\n", ORBIT_VERSION,
           orbit.desktop ? "DESKTOP PREVIEW (game transfers disabled)" : "PS5", orbit.port);
    printf("Pairing code: %s\n", orbit.pair_code);
    diagnostics_stage("pairing", "ok", 0);
#ifndef ORBIT_DESKTOP
    diagnostics_stage("firmware", "started", 0);
    orbit.firmware = kernel_get_fw_version();
    printf("Firmware raw: 0x%08x\n", orbit.firmware);
    diagnostics_stage("firmware", "ok", 0);
#endif
#ifdef ORBIT_INSTALL_LAUNCHER
    copy_text(orbit.launcher_status, sizeof orbit.launcher_status, "checking");
#else
    copy_text(orbit.launcher_status, sizeof orbit.launcher_status, "not-included");
#endif
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    signal(SIGPIPE, SIG_IGN);
    diagnostics_stage("listener", "started", 0);
    debrid_start();
    archive_account_load();
    viking_pass_load();
    ntfy_load();
    packages_init();
    receive_start();
    zero_probe_start();
    startup_result = zero_link_start();
    diagnostics_stage("zero-link", startup_result ? "error" : "ok", startup_result);
    /* Before the API answers, so a Get everything sent at once isn't refused. */
    if (bundle_start())
        puts("Orbit Get everything could not start; the server remains available.");
    startup_result = api_start();
    /* After a handoff the previous process may still be releasing the port. */
    for (int wait = 0; startup_result && handoff && wait < 40; wait++) {
        handoff_wait();
        startup_result = api_start();
    }
    diagnostics_stage("listener", startup_result ? "error" : "ok", startup_result);
    if (startup_result) {
        char message[200];
        snprintf(message, sizeof message,
                 "Orbit Store cannot listen on TCP port %d. Check for another service using this "
                 "port. No launcher changes were made.",
                 orbit.port);
        fprintf(stderr, "%s\n", message);
        notify_console(message);
        return 1;
    }
    /* Console threads may default to small stacks; curl and OpenSSL need headroom. */
    diagnostics_stage("download-worker", "started", 0);
    pthread_attr_t worker_attr;
    bool failed = pthread_attr_init(&worker_attr) != 0;
    if (!failed) {
        failed = pthread_attr_setstacksize(&worker_attr, ORBIT_THREAD_STACK) != 0;
        while (!failed && orbit.worker_count < ORBIT_MAX_DOWNLOADS) {
            failed = pthread_create(&orbit.workers[orbit.worker_count], &worker_attr,
                                    download_worker, NULL) != 0;
            if (!failed) orbit.worker_count++;
        }
        pthread_attr_destroy(&worker_attr);
    }
    diagnostics_stage("download-worker", failed ? "error" : "ok", failed ? -1 : 0);
    if (failed) {
        pthread_mutex_lock(&orbit.mutex);
        orbit.stop = true;
        pthread_cond_broadcast(&orbit.changed);
        pthread_mutex_unlock(&orbit.mutex);
        for (unsigned i = 0; i < orbit.worker_count; i++) pthread_join(orbit.workers[i], NULL);
        api_stop();
        return 1;
    }
#ifdef ORBIT_INSTALL_LAUNCHER
    diagnostics_stage("launcher", "started", 0);
    int launcher = orbit_launcher_ensure(orbit.state_fd);
    pthread_mutex_lock(&orbit.mutex);
    copy_text(orbit.launcher_status, sizeof orbit.launcher_status,
              launcher >= 0 ? "ready" : "error");
    copy_text(orbit.launcher_error, sizeof orbit.launcher_error, orbit_launcher_last_error());
    copy_text(orbit.launcher_registration_method, sizeof orbit.launcher_registration_method,
              orbit_launcher_registration_method());
    pthread_mutex_unlock(&orbit.mutex);
    diagnostics_stage("launcher", launcher < 0 ? "error" : "ok", launcher);
#endif
#if defined(ORBIT_INSTALL_LAUNCHER) || defined(ORBIT_DESKTOP)
    diagnostics_stage("integrations", "started", 0);
    startup_result = autoboot_sync(runtime, runtime_size);
    diagnostics_stage("integrations", startup_result < 0 ? "error" : "ok", startup_result);
#endif
    diagnostics_stage("catalogue", "started", 0);
    startup_result = catalog_start();
    diagnostics_stage("catalogue", startup_result ? "error" : "ok", startup_result);
    if (startup_result)
        puts("Catalogue refresh could not start; the saved catalogue remains available.");
    startup_result = remote_sources_start();
    diagnostics_stage("remote-refresh", startup_result ? "error" : "ok", startup_result);
    diagnostics_stage("updates", "started", 0);
    startup_result = updates_start();
    diagnostics_stage("updates", startup_result ? "error" : "ok", startup_result);
    if (startup_result)
        puts("Orbit update worker could not start; the server remains available.");
    diagnostics_stage("browser", "started", 0);
    startup_result = browser_start();
    diagnostics_stage("browser", startup_result ? "error" : "ok", startup_result);
    diagnostics_stage("kraken", "started", 0);
    startup_result = kraken_start();
    diagnostics_stage("kraken", startup_result ? "error" : "ok", startup_result);
    diagnostics_stage("library", "started", 0);
    startup_result = library_start();
    diagnostics_stage("library", startup_result ? "error" : "ok", startup_result);
    if (startup_result)
        puts("Library worker could not start; the server remains available.");
    startup_result = packages_start();
    diagnostics_stage("packages", startup_result ? "error" : "ok", startup_result);
    diagnostics_stage("artwork", "started", 0);
    startup_result = art_start();
    diagnostics_stage("artwork", startup_result ? "error" : "ok", startup_result);
    if (startup_result)
        puts("Artwork workers could not start.");
    char address[64], endpoint[96], ready_message[256];
    orbit_lan_address(address, sizeof address);
    if (address[0])
        snprintf(endpoint, sizeof endpoint, "http://%s:%d", address, orbit.port);
    else
        snprintf(endpoint, sizeof endpoint, "IP address unavailable; port %d", orbit.port);
    snprintf(ready_message, sizeof ready_message, "Orbit Store %s | Pair: %s | %s", endpoint,
             orbit.pair_code,
             !strcmp(orbit.launcher_status, "ready")   ? "Open the Orbit Store icon"
             : !strcmp(orbit.launcher_status, "error") ? "Icon setup failed; server is running"
                                                       : "Server ready");
    diagnostics_stage("notification", "started", 0);
    startup_result = notify_console(ready_message);
    diagnostics_stage("notification", startup_result ? "error" : "ok", startup_result);
    diagnostics_stage("ready", "ok", 0);
    while (!stopping) {
        sleep(1);
        power_tick();
        remote_sources_tick();
        if (atomic_load(&stop_requested)) {
            sleep(1); /* Let the stop acknowledgement reach the browser. */
            stopping = 1;
        }
    }
    diagnostics_stage("shutdown", "started", 0);
    /* Keep status reachable until workers have stopped and durable checkpoints
     * are saved. Clients must not treat a closed port as a released instance
     * lock while the old process is still shutting down. Mutations are refused
     * as soon as stopping is requested. */
    receive_stop();
    zero_probe_stop();
    zero_link_stop();
    pthread_mutex_lock(&orbit.mutex);
    orbit.stop = true;
    pthread_cond_broadcast(&orbit.changed);
    pthread_mutex_unlock(&orbit.mutex);
    art_stop();
    debrid_stop();
    archive_account_clear();
    viking_pass_clear();
    bundle_stop(); /* before the browser session it may be following */
    browser_stop();
    history_stop(); /* stop deletion before stopping Files/installer guards */
    files_stop(); /* before the installer it may be following */
    packages_stop();
    library_stop();
    kraken_stop();
    updates_stop();
    remote_sources_stop();
    catalog_stop();
    for (unsigned i = 0; i < orbit.worker_count; i++) pthread_join(orbit.workers[i], NULL);
    ntfy_stop(); /* after the workers, the last to queue a notification */
    api_stop();
    curl_global_cleanup();
    cJSON_Delete(orbit.catalog);
    cJSON_Delete(orbit.addons);
    diagnostics_stage("shutdown", "ok", 0);
    int loader = atomic_load(&restart_loader);
    /* Told to stop by a signal: that wins over a restart an update asked for around the same time. */
    if (loader >= 0 && signalled) {
        close(loader);
        loader = -1;
    }
    if (loader >= 0) handoff_mark(orbit.state_fd);
    close(lock);
    close(orbit.state_fd);
    if (loader >= 0) updates_restart_send(loader, runtime, runtime_size);
    if (!signalled && loader < 0) power_finish();
    return 0;
}
