#include "orbit/download_settings.hpp"
// Orbit Store TV app - The Orbit API's data, and the storefront rules built on it.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/model.hpp"

#include "orbit/i18n.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace orbit
{
JobProgress job_progress(const Job &job)
{
    const auto bytes = [](double value) { return std::isfinite(value) && value > 0.0 ? value : 0.0; };
    JobProgress p;
    const bool extracting = job.status == "extracting";
    p.done = bytes(extracting ? job.extracted : job.received);
    p.total = bytes(extracting ? job.extract_total : job.send_total > 0.0 ? job.send_total : job.total);
    p.remaining = std::max(0.0, p.total - p.done);
    const double speed = bytes(job.speed);
    if (speed > 0.0) p.minutes = std::ceil(p.remaining / speed / 60.0);
    return p;
}

namespace
{

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool contains_folded(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > haystack.size())
        return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        std::size_t j = 0;
        while (j < needle.size() && lower(haystack[i + j]) == lower(needle[j]))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}

int compare_folded(std::string_view a, std::string_view b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        const char x = lower(a[i]);
        const char y = lower(b[i]);
        if (x != y)
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
    }
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

std::string_view trim(std::string_view value)
{
    while (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);
    while (!value.empty() && value.back() == ' ')
        value.remove_suffix(1);
    return value;
}

// ISO catalogue timestamps include UTC or an explicit offset. Compare instants,
// not the spelling of the timezone; avoid depending on the console's local zone.
double addition_time(const std::string &date)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, end = 0;
    double second = 0;
    if (std::sscanf(date.c_str(), "%d-%d-%dT%d:%d:%lf%n", &year, &month, &day,
                    &hour, &minute, &second, &end) != 6 || end <= 0 ||
        year < 1900 || year > 9999 || month < 1 || month > 12 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || !(second >= 0 && second < 60))
        return 0;
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int months[] = {31, 28 + (leap ? 1 : 0), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (day < 1 || day > months[month - 1])
        return 0;
    const char *zone = date.c_str() + end;
    int offset = 0;
    if (std::string_view(zone) != "Z")
    {
        int hours = 0, minutes = 0, consumed = 0;
        if ((*zone != '+' && *zone != '-') ||
            std::sscanf(zone + 1, "%d:%d%n", &hours, &minutes, &consumed) != 2 ||
            zone[consumed + 1] != '\0' || hours < 0 || hours > 23 || minutes < 0 || minutes > 59)
            return 0;
        offset = (hours * 60 + minutes) * (*zone == '-' ? -1 : 1);
    }
    const long long y = year - 1;
    long long days = 365 * y + y / 4 - y / 100 + y / 400 + day - 1;
    for (int m = 0; m < month - 1; ++m)
        days += months[m];
    return static_cast<double>(days) * 86400 + hour * 3600 + minute * 60 + second - offset * 60;
}

int status_rank(std::string_view status)
{
    if (status == "downloading")
        return 0;
    if (status == "verifying" || status == "waiting-extraction" || status == "inspecting" || status == "extracting")
        return 1;
    if (status == "queued")
        return 2;
    if (status == "retrying")
        return 3;
    if (status == "paused")
        return 4;
    if (status == "error")
        return 5;
    if (status == "complete")
        return 6;
    return 7;
}

} // namespace

const BundleItem *Bundle::item(std::string_view release_id) const
{
    for (const BundleItem &it : items)
        if (it.release_id == release_id)
            return &it;
    return nullptr;
}

int Bundle::added() const
{
    return static_cast<int>(std::count_if(items.begin(), items.end(), [](const BundleItem &it) { return it.state == "added"; }));
}

const char *bundle_state_label(std::string_view state)
{
    if (state == "added")
        return tr("Added");
    if (state == "page")
        return tr("Open its page on your PS5");
    if (state == "failed" || state == "skipped")
        return tr("Not added");
    return tr("Waiting");
}

Bundle parse_bundle(const json::Value &value)
{
    Bundle bundle;
    bundle.id = value.text("id");
    bundle.state = value.text("state").empty() ? "idle" : value.text("state");
    bundle.message = value.text("message");
    for (const json::Value &entry : value["items"].items())
    {
        BundleItem item;
        item.release_id = entry.text("releaseId");
        if (item.release_id.empty())
            continue;
        item.title = entry.text("title");
        item.kind = entry.text("kind");
        item.state = entry.text("state");
        item.job_id = entry.text("jobId");
        item.message = entry.text("message");
        bundle.items.push_back(std::move(item));
    }
    return bundle;
}

BrowserSession parse_browser_session(const json::Value &value)
{
    BrowserSession session;
    session.native_dialog_available = value.flag("nativeDialogAvailable");
    session.dialog_opened = value.flag("dialogOpened");
    session.viking_account_available = value.flag("vikingAccountAvailable");
    session.viking_state = value.text("vikingNativeState");
    session.viking_pass = value.text("vikingPass");
    session.account_page = value.flag("accountPage");
    session.client_id = value.text("clientId");
    session.page_url = value.text("pageUrl");
    session.available = value.flag("available");
    session.active = value.flag("active");
    session.id = value.text("id");
    session.state = value.text("state");
    session.title = value.text("title");
    session.message = value.text("message");
    session.job_id = value.text("jobId");
    return session;
}

System parse_system(const json::Value &value)
{
    System system;
    system.name = value.text("name");
    system.version = value.text("version");
    system.instance_id = value.text("instanceId");
    system.stopping = value.flag("stopping");
    system.platform = value.text("platform");
    system.paired = value.flag("paired");
    system.local_session = value.flag("localSessionAvailable");
    system.state_healthy = value.flag("stateHealthy", true);
    system.catalogue_revision = value["catalogueRevision"].as_int();
    system.preferred_storage = value.text("preferredStorage");
    system.delete_archives = value.flag("deleteArchives", true);
    system.has_extract_archives = value["extractArchives"].is_bool();
    system.extract_archives = value.flag("extractArchives", true);
    system.bundle_protocol = static_cast<int>(value.number("bundleProtocol", 0));
    system.space_protocol = static_cast<int>(value.number("spaceProtocol", 0));
    system.has_download_notifications = value["downloadNotifications"].is_bool();
    system.download_notifications = value.flag("downloadNotifications", true);
    {
        const json::Value &events = value["notificationEvents"];
        system.has_notification_events = events.is_object();
        for (std::size_t i = 0; i < system.notification_events.size(); ++i)
            system.notification_events[i] = events.flag(kNotificationEvents[i].name, true);
    }
    system.has_delete_packages = value["deletePackages"].is_bool();
    system.delete_packages = value.flag("deletePackages", false);
    system.has_history_removal = value.flag("historyRemovalAvailable", false);
    system.history_files = value.text("historyFiles");
    system.has_power = value["power"].flag("available", false);
    system.power_enabled = value["power"].flag("enabled", false);
    system.power_seconds = static_cast<int>(value["power"]["seconds"].as_int());
    system.power_state = value["power"].text("state");
    system.power_job = value["power"].text("jobId");
    system.power_error = value["power"].text("error");
    system.ntfy_enabled = value.flag("ntfyEnabled", false);
    {
        const auto connections = value["downloadConnections"].as_int();
        system.has_download_connections = connections >= 1 && connections <= 64;
        system.download_connections = system.has_download_connections ? static_cast<int>(connections) : 8;
    }
    system.firmware = value.text("firmware");
    const auto limit = value["concurrentDownloads"].as_int();
    system.concurrent_downloads = limit >= 1 && limit <= 8 ? static_cast<int>(limit) : 1;
    for (std::size_t i = 0; i < download_settings.size(); ++i) {
        const auto &setting = download_settings[i];
        const auto &v = value[setting.key];
        const auto n = v.as_int();
        if (v.is_number() && n >= setting.min && n <= setting.max && v.as_number() == n)
            system.download_recovery[i] = static_cast<int>(n);
    }
    return system;
}

Sources parse_sources(const json::Value &value)
{
    Sources sources;
    for (const json::Value &id : value["enabled"].items())
    {
        if (id.is_string())
            sources.enabled.push_back(id.as_string());
    }
    sources.acknowledged = value.flag("acknowledged");
    sources.notice_version = static_cast<int>(value["noticeVersion"].as_int());
    for (const json::Value &item : value["options"].items())
    {
        SourceOption option;
        option.id = item.text("id");
        option.label = item.text("label");
        option.release_count = static_cast<int>(item["releaseCount"].as_int());
        option.download_ready = item.flag("downloadReady");
        sources.options.push_back(std::move(option));
    }
    return sources;
}

Debrid parse_debrid(const json::Value &value)
{
    Debrid status;
    if (value.text("provider") != "torbox")
        return status;
    status.available = value.flag("available");
    status.connected = value.flag("connected");
    status.busy = value.flag("busy");
    status.has_preferences = !value["alwaysUseTorBox"].is_null();
    status.always_use_torbox = value.flag("alwaysUseTorBox");
    status.last_delivery = value.text("lastDelivery");
    for (const json::Value &item : value["hosts"].items())
    {
        Debrid::Host host;
        host.source_id = item.text("sourceId");
        host.max_file_size = item.number("maxFileSize");
        host.available =
            item.flag("available");
        status.hosts.push_back(std::move(host));
    }
    return status;
}

ArchiveAccount parse_archive_account(const json::Value &value)
{
    ArchiveAccount account;
    account.known = value["signedIn"].is_bool();
    account.signed_in = value.flag("signedIn");
    account.name = value.text("name");
    return account;
}

NtfySettings parse_ntfy(const json::Value &value)
{
    NtfySettings ntfy;
    ntfy.known = value["enabled"].is_bool();
    ntfy.enabled = value.flag("enabled");
    ntfy.server = value.text("server");
    ntfy.topic = value.text("topic");
    ntfy.username = value.text("username");
    ntfy.has_password = value.flag("hasPassword");
    return ntfy;
}

std::string ntfy_address(const NtfySettings &ntfy)
{
    const std::size_t scheme = ntfy.server.find("://");
    return (scheme == std::string::npos ? ntfy.server : ntfy.server.substr(scheme + 3)) + "/" + ntfy.topic;
}

bool Debrid::supports(const Release &release) const
{
    if (!available || !connected || busy)
        return false;
    for (const Host &host : hosts)
        if (host.source_id == release.source_id && host.available)
            return true;
    return false;
}
bool Debrid::always_for(const Release &release) const
{
    if (!always_use_torbox || !available || !connected)
        return false;
    for (const Host &host : hosts)
        if (host.source_id == release.source_id)
            return host.available;
    return false;
}
bool Debrid::preferred(const Release &release) const
{
    if (always_for(release)) return true;
    if (last_delivery != "torbox" || !supports(release)) return false;
    for (const Host &host : hosts)
        if (host.source_id == release.source_id)
            return host.max_file_size <= 0 || release.size_bytes <= host.max_file_size;
    return false;
}

std::string Announcement::title_in(std::string_view language) const
{
    const std::string_view base = language.substr(0, language.find('-'));
    for (const std::string_view code : {language, base})
        for (const auto &text : translations)
            if (text[0] == code)
                return text[1];
    return title;
}

std::vector<Announcement> parse_announcements(const json::Value &value)
{
    std::vector<Announcement> out;
    for (const json::Value &item : value["announcements"].items())
    {
        Announcement a;
        a.id = item.text("id");
        a.title = item.text("title");
        a.message = item.text("message");
        const json::Value &translations = item["translations"];
        for (const std::string &code : translations.keys())
        {
            const json::Value &text = translations[code];
            if (!text.text("title").empty())
                a.translations.push_back({code, text.text("title"), text.text("message")});
        }
        if (!a.id.empty() && a.id.size() <= 64 && !a.title.empty() && a.title.size() <= 120 && out.size() < 10)
            out.push_back(std::move(a));
    }
    return out;
}

std::vector<std::string> Debrid::sources() const
{
    std::vector<std::string> ids;
    if (available && connected && !busy)
        for (const Host &host : hosts)
            if (host.available)
                ids.push_back(host.source_id);
    return ids;
}

static void parse_addon(const json::Value &item, AddonInfo &out)
{
    out.addon_id = item.text("addonId"); out.base_game_id = item.text("baseGameId");
    out.base_title = item.text("baseGameTitle"); out.addon_type = item.text("addonType");
    out.content_id = item.text("contentId");
    const auto &r = item["requirements"];
    out.required_edition = r.text("edition"); out.minimum_version = r.text("minimumVersion");
    out.requirement_notes = r.text("notes");
    for (const auto &id : r["titleIds"].items()) out.required_title_ids.push_back(id.as_string());
    for (const auto &entry : item["contents"].items()) out.contents.push_back(entry.as_string());
}
std::string addon_type_label(const std::string &type)
{
    return tr(type == "expansion" ? "Expansion" : type == "activation" ? "Activation package" : type == "bundle" ? "Bundle" : "Content pack");
}
std::string addon_requirements(const AddonInfo &i, const std::vector<LibraryGame> &library)
{
    if (i.addon_id.empty()) return {};
    std::string out = i.base_title + " · " + addon_type_label(i.addon_type) + "\n";
    if (i.addon_type == "activation") out += std::string(tr("This package activates content already included in the base game. It does not contain that content.")) + "\n";
    for (const auto &entry : i.contents) out += entry + "\n";
    std::string ids;
    for (const auto &id : i.required_title_ids) ids += (ids.empty() ? "" : ", ") + id;
    out += std::string(tr("Required title ID")) + ": " + (ids.empty() ? std::string(tr("Unknown")) : ids);
    out += "\n" + std::string(tr("Base-game edition")) + ": " + (i.required_edition.empty() ? tr("Unknown") : i.required_edition);
    out += "\n" + std::string(tr("Minimum game version")) + ": " + (i.minimum_version.empty() ? tr("Unknown") : i.minimum_version);
    if (!i.requirement_notes.empty()) out += "\n" + i.requirement_notes;
    const bool matching = std::any_of(library.begin(), library.end(), [&](const LibraryGame &g)
    {
        return (g.installed || g.on_drive) &&
               std::find(i.required_title_ids.begin(), i.required_title_ids.end(), g.title_id) != i.required_title_ids.end();
    });
    out += "\n" + std::string(matching ? tr("A matching base-game title ID was found. This does not confirm edition, version or DLC compatibility.")
                                       : tr("The required base game could not be confirmed. Check its title ID, edition and version before installing this add-on."));
    return out + "\n" + tr("You can still download this add-on. Unknown requirements are not a compatibility guarantee.");
}

std::vector<Release> parse_catalog(const json::Value &value)
{
    std::vector<Release> releases;
    releases.reserve(value.items().size());
    for (const json::Value &item : value.items())
    {
        Release release;
        parse_addon(item, release);
        release.id = item.text("id");
        release.game_id = item.text("gameId");
        if (release.id.empty() || release.game_id.empty())
            continue;
        release.title_id = item.text("titleId");
        release.platform = item.text("platform");
        if (release.platform.empty())
            release.platform = release.title_id.rfind("CUSA", 0) == 0 ? "ps4" : "ps5";
        release.title = item.text("title");
        release.genre = item.text("genre");
        release.tagline = item.text("tagline");
        release.description = item.text("description");
        release.provider = item.text("provider");
        release.source_id = item.text("sourceId");
        release.format = item.text("format");
        release.content_format = item.text("contentFormat") == "LZ4" ? "LZ4" : "";
        release.asset_compression = item.text("assetCompression") == "ZSTD" ? "ZSTD" : "";
        release.region = item.text("region");
        release.version = item.text("version");
        release.filename = item.text("filename");
        release.notes = item.text("notes");
        release.artwork_layout = item.text("artworkLayout");
        release.publisher = item.text("publisher");
        release.release_date = item.text("releaseDate");
        const auto &meta=item["insights"]; const auto &score=meta["metacritic"]; const auto &time=meta["howLongToBeat"];
        if (score["score"].is_number() && score.text("platform")==release.platform) { release.insights.score=static_cast<int>(score.number("score")); release.insights.score_url=score.text("url"); release.insights.score_checked=score.text("checkedAt"); }
        if (time["mainMinutes"].is_number()) release.insights.main_minutes=static_cast<int>(time.number("mainMinutes"));
        if (time["extrasMinutes"].is_number()) release.insights.extras_minutes=static_cast<int>(time.number("extrasMinutes"));
        if (time["completionistMinutes"].is_number()) release.insights.completionist_minutes=static_cast<int>(time.number("completionistMinutes"));
        release.insights.time_url=time.text("url");release.insights.time_checked=time.text("checkedAt");release.insights.edition=time.text("edition");
        release.added_at = item.text("addedAt");
        const double rank = item.number("newOnOrbitRank");
        if (rank >= 1 && rank <= 100 && rank == static_cast<int>(rank))
            release.new_on_orbit_rank = static_cast<int>(rank);
        for (const json::Value &tag : item["tags"].items())
            if (!tag.as_string().empty() && release.tags.size() < 8)
                release.tags.push_back(tag.as_string());
        release.size_bytes = item.number("sizeBytes");
        release.compression = item.text("compression");
        release.archive_unverified = item.text("archiveContents") == "unverified";
        release.extracted_bytes = item.number("extractedBytes");
        release.browser_available = item.flag("browserAvailable");
        release.direct_available = item.flag("directAvailable", true);
        release.required_firmware = item.text("requiredFirmware");
        release.max_firmware = item.text("maxFirmware");
        release.firmware_issue = item.text("firmwareIssue");
        release.mounter = item.text("mounter");
        releases.push_back(std::move(release));
    }
    return releases;
}

std::vector<Drive> parse_storage(const json::Value &value)
{
    std::vector<Drive> drives;
    for (const json::Value &item : value.items())
    {
        Drive drive;
        drive.id = item.text("id");
        if (drive.id.empty())
            continue;
        drive.label = item.text("label");
        drive.path = item.text("path");
        drive.free_bytes = item.number("freeBytes");
        drive.total_bytes = item.number("totalBytes");
        drive.pending_bytes = item.number("pendingBytes");
        drive.projected_free_bytes = item.number("projectedFreeBytes");
        drive.external = item.flag("external");
        drives.push_back(std::move(drive));
    }
    return drives;
}

std::vector<Job> parse_jobs(const json::Value &value)
{
    std::vector<Job> jobs;
    for (const json::Value &item : value.items())
    {
        Job job;
        job.removing_files = item.flag("removingFiles", false);
        job.removal_error = item.text("removalError");
        parse_addon(item, job);
        job.id = item.text("id");
        if (job.id.empty())
            continue;
        job.release_id = item.text("releaseId");
        job.game_id = item.text("gameId");
        job.title_id = item.text("titleId");
        job.title = item.text("title");
        job.storage_id = item.text("storageId");
        job.filename = item.text("filename");
        job.path = item.text("path");
        job.format = item.text("format");
        job.status = item.text("status");
        job.error = item.text("error");
        job.verification = item.text("verification");
        Release facts;
        facts.required_firmware = item.text("requiredFirmware");
        facts.max_firmware = item.text("maxFirmware");
        facts.firmware_issue = item.text("firmwareIssue");
        facts.mounter = item.text("mounter");
        job.firmware_note = firmware_warning(facts, item.text("consoleFirmware"));
        // ShadowMount+ applies a backport only from backports/<TITLE_ID>/, not from inside the game's folder.
        if (item.flag("backport") && facts.firmware_issue == "newer-firmware")
            job.firmware_note = tr("This download needs firmware {version} or later. This PS5 is on {current}. It includes a backport for older firmware. To use it with ShadowMount+, move the folder that contains fakelib to /data/homebrew/backports/ and name it {titleId}. You can do this in {place}.",
                                   {{"version", facts.required_firmware}, {"current", item.text("consoleFirmware")}, {"titleId", job.title_id},
                                    {"place", std::string(tr("Library")) + " \xE2\x86\x92 " + std::string(tr("Files"))}});
        if (job.firmware_note.empty() && item.text("firmwareSource") == "param")
            job.firmware_note = tr("Game files declare firmware {version} or later. This does not confirm game compatibility.", {{"version", facts.required_firmware}});
        else if (job.firmware_note.empty() && (!facts.max_firmware.empty() || !facts.mounter.empty())) job.firmware_note = firmware_label(facts);
        job.delivery = item.text("delivery");
        const auto &installation = item["installation"];
        job.installation_available = installation.flag("available");
        job.installation_state = installation.text("state");
        job.installation_message = installation.text("message");
        job.shadowmount_state = installation.text("shadowMount");
        job.shadowmount_message = installation.text("shadowMountMessage");
        job.kraken_state = installation.text("kraken");
        job.kraken_message = installation.text("krakenMessage");
        job.kraken_error = installation.text("krakenError");
        job.installation_chain = installation.text("chain");
        job.installation_chain_title = installation.text("chainTitle");
        for (const auto &v:installation["verifications"].items()) job.verifications.push_back({v.text("storageId"),v.text("path"),static_cast<int>(v.number("fixCount")),static_cast<int>(v.number("warningCount")),v.flag("complete")});
        for (const auto &p : installation["packages"].items()) {
            PackageInstallation pkg;
            pkg.id = p.text("id"); pkg.filename = p.text("filename"); pkg.platform = p.text("platform");
            pkg.state = p.text("state"); pkg.message = p.text("message"); pkg.received = p.number("received"); pkg.total = p.number("total");
            job.packages.push_back(std::move(pkg));
        }
        job.can_refresh_link = item.flag("canRefreshLink");
        job.from_link = item.text("sourceId") == "link";
        job.link_host = item.text("linkHost");
        job.can_replace_link = item.flag("canReplaceLink");
        job.can_enter_archive_password = item.flag("canEnterArchivePassword");
        job.delivery_phase = item.text("deliveryPhase");
        job.received = item.number("received");
        job.total = item.number("total");
        job.speed = item.number("speed");
        job.computer_received = item.number("computerReceived");
        job.download_speed = item.number("downloadSpeed");
        job.extracted = item.number("extracted");
        job.extract_total = item.number("extractTotal");
        job.send_total = item.number("sendTotal");
        job.retry_at = item.number("retryAt");
        job.order = static_cast<int>(item["order"].as_int());
        jobs.push_back(std::move(job));
    }
    // The backend sorts by order; keep that even if a reply arrives unsorted.
    std::stable_sort(jobs.begin(), jobs.end(),
                     [](const Job &a, const Job &b) { return a.order < b.order; });
    return jobs;
}

namespace
{
constexpr const char *kSortIds[] = {"release", "added", "title", "title-desc", "size", "size-desc", "score", "score-desc", "time", "time-desc"};
constexpr const char *kSizeIds[] = {"", "small", "medium", "large", "huge"};

std::vector<std::string> strings(const json::Value &value)
{
    std::vector<std::string> out;
    for (const json::Value &item : value.items())
        if (item.is_string() && !item.as_string().empty() && !contains(out, item.as_string()))
            out.push_back(item.as_string());
    return out;
}

std::string string_list(const std::vector<std::string> &values)
{
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i)
        out += (i ? "," : "") + json::quote(values[i]);
    return out + "]";
}
} // namespace

std::vector<SavedFilter> parse_saved_filters(const json::Value &value)
{
    std::vector<SavedFilter> out;
    for (const json::Value &item : value.items())
    {
        const json::Value &f = item["filters"];
        if (!item.is_object() || item.text("id").empty() || item.text("name").empty() || !f.is_object())
            continue;
        SavedFilter saved;
        saved.id = item.text("id");
        saved.name = item.text("name");
        saved.addons = item.text("page") == "addons";
        BrowseFilters &b = saved.filters;
        b.query = f.text("query");
        b.sources = strings(f["source"]);
        b.formats = strings(f["format"]);
        b.tags = strings(f["tags"]);
        b.genres = strings(f["genres"]);
        b.minimum_score=f.text("minimumScore"); b.playtime_range=f.text("playtimeRange"); b.play_style=f.text("playStyle").empty()?"main":f.text("playStyle");
        b.region = f.text("region");
        b.platform = f.text("platform");
        b.addon_type = f.text("addonType");
        b.favourites = f.text("collection") == "favorites";
        b.hide_installed = f.flag("hideInstalled");
        for (int i = 0; i < static_cast<int>(SizeFilter::count); ++i)
            if (f.text("size") == kSizeIds[i])
                b.size = static_cast<SizeFilter>(i);
        for (int i = 0; i < static_cast<int>(Sort::count); ++i)
            if (f.text("sort") == kSortIds[i])
                b.sort = static_cast<Sort>(i);
        // "added" is the Add-ons page's newest first, Sort::release there.
        if (saved.addons && b.sort == Sort::added)
            b.sort = Sort::release;
        out.push_back(std::move(saved));
    }
    return out;
}

std::string saved_filter_body(std::string_view name, bool addons, const BrowseFilters &f)
{
    const char *sort = addons && f.sort == Sort::release ? "added" : kSortIds[static_cast<int>(f.sort)];
    return "{\"action\":\"save\",\"name\":" + json::quote(name) + ",\"page\":\"" + (addons ? "addons" : "games") +
           "\",\"filters\":{\"query\":" + json::quote(f.query) + ",\"minimumScore\":" + json::quote(f.minimum_score) + ",\"playtimeRange\":" + json::quote(f.playtime_range) + ",\"playStyle\":" + json::quote(f.play_style) + ",\"source\":" + string_list(f.sources) +
           ",\"format\":" + string_list(f.formats) + ",\"tags\":" + string_list(f.tags) + ",\"genres\":" + string_list(f.genres) +
           ",\"region\":" + json::quote(f.region) + ",\"platform\":" + json::quote(f.platform) +
           ",\"size\":\"" + kSizeIds[static_cast<int>(f.size)] + "\",\"collection\":\"" +
           (f.favourites ? "favorites" : "all") + "\",\"hideInstalled\":" + (f.hide_installed ? "true" : "false") +
           ",\"sort\":\"" + sort + "\",\"addonType\":" + json::quote(f.addon_type) + "}}";
}

std::vector<std::string> parse_favourites(const json::Value &value)
{
    std::vector<std::string> ids;
    for (const json::Value &item : value.items())
    {
        if (item.is_string() && !item.as_string().empty() && !contains(ids, item.as_string()))
            ids.push_back(item.as_string());
    }
    return ids;
}

namespace
{

double optional_bytes(const json::Value &value)
{
    return value.is_number() ? value.as_number() : -1.0;
}

LibraryDrive parse_library_drive(const json::Value &item)
{
    LibraryDrive drive;
    drive.id = item.text("id");
    drive.path = item.text("path");
    drive.label = item.text("label");
    drive.filesystem = item.text("filesystem");
    drive.read_only = item.flag("readOnly");
    drive.total = item.number("totalBytes");
    drive.free = item.number("freeBytes");
    drive.used = item.number("usedBytes");
    return drive;
}

} // namespace

LibrarySnapshot parse_library(const json::Value &value)
{
    LibrarySnapshot snapshot;
    snapshot.status = value.text("status");
    snapshot.message = value.text("message");
    snapshot.provider = value.text("provider");
    snapshot.provider_version = value.text("providerVersion");
    snapshot.busy = value.flag("busy");
    snapshot.stale = value.flag("stale");
    snapshot.checked_at = value.number("checkedAt");
    snapshot.updated_at = value.number("updatedAt");
    snapshot.refresh_after = value.number("refreshAfter");
    for (const json::Value &item : value["games"].items())
    {
        LibraryGame game;
        game.title_id = item.text("titleId");
        if (game.title_id.empty())
            continue;
        game.title = item.text("title");
        game.platform = item.text("platform");
        game.format = item.text("format");
        game.path = item.text("path");
        game.runtime_path = item.text("runtimePath");
        game.source_key = item.text("sourceKey");
        game.location = item.text("location");
        game.installed = item.flag("installed");
        game.mounted = item.flag("mounted");
        game.on_drive = item.flag("onDrive");
        game.found_by_orbit = item.flag("foundByOrbit");
        game.managed = item.flag("managed");
        game.can_manage_source = item.flag("canManageSource");
        game.installed_size = optional_bytes(item["installedSizeBytes"]);
        game.size = optional_bytes(item["sizeBytes"]);
        game.size_status = item.text("sizeStatus");
        snapshot.games.push_back(std::move(game));
    }
    for (const json::Value &item : value["capabilities"].items())
    {
        if (item.is_string())
            snapshot.capabilities.push_back(item.as_string());
    }
    const json::Value &action = value["action"];
    if (action.is_object())
    {
        snapshot.has_action = true;
        snapshot.action.id = action.text("id");
        snapshot.action.action = action.text("action");
        snapshot.action.title_id = action.text("titleId");
        snapshot.action.state = action.text("state");
        snapshot.action.message = action.text("message");
    }
    const json::Value &job = value["storageJob"];
    if (job.is_object())
    {
        snapshot.has_storage_job = true;
        StorageJob &out = snapshot.storage_job;
        out.id = static_cast<int>(job["id"].as_int());
        out.operation = job.text("operation");
        out.state = job.text("state");
        out.title_id = job.text("titleId");
        out.source = job.text("source");
        out.destination = job.text("destination");
        out.error = job.text("error");
        out.total = job.number("totalBytes");
        out.processed = job.number("processedBytes");
        out.speed = job.number("speed");
        out.active = job.flag("active");
        out.cancellable = job.flag("cancellable");
        out.cancel_requested = job.flag("cancelRequested");
    }
    snapshot.storage_error = value.text("storageError");
    snapshot.storage_busy = value.flag("storageBusy");
    snapshot.scanning = value.flag("scanning");
    snapshot.measuring = value.flag("measuring");
    return snapshot;
}

LibraryStorage parse_library_storage(const json::Value &value)
{
    LibraryStorage storage;
    for (const json::Value &item : value["drives"].items())
        storage.drives.push_back(parse_library_drive(item));
    for (const json::Value &item : value["destinations"].items())
        storage.destinations.push_back(parse_library_drive(item));
    storage.busy = value.flag("busy");
    storage.stale = value.flag("stale");
    storage.updated_at = value.number("updatedAt");
    storage.error = value.text("error");
    return storage;
}

FileListing parse_file_listing(const json::Value &value)
{
    FileListing listing;
    listing.storage_id = value.text("storageId");
    listing.path = value.text("path");
    listing.truncated = value.flag("truncated");
    for (const json::Value &item : value["entries"].items())
    {
        FileEntry entry;
        entry.name = item.text("name");
        if (entry.name.empty())
            continue;
        entry.folder = item.flag("folder");
        entry.type = item.text("type");
        entry.size = optional_bytes(item["sizeBytes"]);
        entry.modified = item.number("modifiedAt");
        entry.locked = item.text("locked");
        listing.entries.push_back(std::move(entry));
    }
    return listing;
}

SpaceDeletion parse_space_deletion(const json::Value &value)
{
    SpaceDeletion deletion;
    if (!value.is_object())
        return deletion;
    deletion.state = value.text("state");
    if (deletion.state.empty())
        deletion.state = "idle";
    deletion.storage_id = value.text("storageId");
    deletion.message = value.text("message");
    deletion.current = value.text("current");
    deletion.total = static_cast<int>(value.number("total"));
    deletion.done = static_cast<int>(value.number("done"));
    deletion.failed = static_cast<int>(value.number("failed"));
    deletion.freed_bytes = value.number("freedBytes");
    return deletion;
}

SpaceListing parse_space_listing(const json::Value &value)
{
    SpaceListing listing;
    listing.storage_id = value.text("storageId");
    listing.label = value.text("label");
    listing.free_bytes = value.number("freeBytes");
    listing.total_bytes = value.number("totalBytes");
    listing.projected_free_bytes = value.number("projectedFreeBytes");
    const auto items = [](const json::Value &list, std::vector<SpaceItem> *out)
    {
        for (const json::Value &item : list.items())
        {
            SpaceItem row;
            row.id = item.text("id");
            row.kind = item.text("kind");
            row.title = item.text("title");
            row.name = item.text("name");
            row.format = item.text("format");
            row.size_bytes = item.number("sizeBytes");
            row.mounted = item.flag("mounted");
            if (!row.id.empty())
                out->push_back(std::move(row));
        }
    };
    items(value["leftovers"], &listing.leftovers);
    items(value["games"], &listing.games);
    listing.deletion = parse_space_deletion(value["deletion"]);
    return listing;
}

bool parse_file_operation(const json::Value &value, FileOperation *operation)
{
    const json::Value &item = value["operation"];
    *operation = FileOperation{};
    if (!item.is_object() || item.text("id").empty())
        return false;
    operation->id = item.text("id");
    operation->action = item.text("action");
    operation->state = item.text("state");
    operation->name = item.text("name");
    operation->storage_id = item.text("storageId");
    operation->path = item.text("path");
    operation->to_storage_id = item.text("toStorageId");
    operation->to_path = item.text("toPath");
    operation->processed = item.number("processedBytes");
    operation->total = item.number("totalBytes");
    operation->speed = item.number("speed");
    operation->size = optional_bytes(item["sizeBytes"]);
    operation->error = item.text("error");
    const auto &check = item["verification"];
    operation->verification.token = check.text("token"); operation->verification.fixes = static_cast<int>(check.number("fixCount"));
    operation->verification.warnings = static_cast<int>(check.number("warningCount")); operation->verification.checked = static_cast<int>(check.number("checkedFiles"));
    operation->verification.complete = check.flag("complete"); operation->verification.can_repair = check.flag("canRepair");
    for (const auto &row : check["issues"].items()) operation->verification.issues.push_back({row.text("kind"),row.text("path"),row.text("reference"),row.text("replacement"),row.text("mode"),row.flag("repairable")});
    return true;
}

Session parse_session(const json::Value &value)
{
    Session session;
    session.token = value.text("token");
    session.pair_code = value.text("pairCode");
    session.address = value.text("address");
    return session;
}

CatalogueStatus parse_catalogue_status(const json::Value &value)
{
    CatalogueStatus status;
    status.available = value.flag("available");
    status.busy = value.flag("busy");
    status.cached = value.flag("cached");
    status.revision = value["revision"].as_int();
    status.game_count = static_cast<int>(value["gameCount"].as_int());
    status.checked_at = value.number("checkedAt");
    status.check_after = value.number("checkAfter");
    status.error = value.text("error");
    return status;
}

ServiceUpdate parse_service_update(const json::Value &value)
{
    ServiceUpdate update;
    update.available = value.flag("available");
    update.update_available = value.flag("updateAvailable");
    update.busy = value.flag("busy");
    update.restart_required = value.flag("restartRequired");
    update.running_version = value.text("runningVersion");
    update.instance_id = value.text("instanceId");
    update.saved_version = value.text("savedVersion");
    update.latest_version = value.text("latestVersion");
    update.checksum = value.text("checksum");
    update.phase = value.text("phase");
    update.error = value.text("error");
    update.received = value.number("received");
    update.retry_at = value.number("retryAt");
    update.checked_at = value.number("checkedAt");
    update.check_after = value.number("checkAfter");
    update.self_restart = value.flag("selfRestart");
    update.update_all = value.flag("updateAll");
    update.tv_app_updated = value.text("tvAppUpdated");
    update.restart_notice = value.text("restartNotice");
    return update;
}

TvAppStatus parse_tv_app(const json::Value &value)
{
    TvAppStatus status;
    status.available = value.flag("available");
    status.installed = value.text("installed");
    status.installed_version = value.text("installedVersion");
    status.installed_path = value.text("installedPath");
    if (status.installed_path.empty())
        status.installed_path = status.installed == "folder" ? "/data/homebrew/PPSA99177" : "/data/homebrew/PPSA99177.ffpkg";
    status.latest_version = value.text("latestVersion");
    status.service_version = value.text("serviceVersion");
    status.checksum = value.text("checksum");
    status.size = value.number("size");
    status.phase = value.text("phase");
    status.error = value.text("error");
    status.busy = value.flag("busy");
    status.update_available = value.flag("updateAvailable");
    status.received = value.number("received");
    status.retry_at = value.number("retryAt");
    status.checked_at = value.number("checkedAt");
    status.check_after = value.number("checkAfter");
    return status;
}

bool LibrarySnapshot::can(std::string_view capability) const
{
    return contains(capabilities, capability);
}

bool LibrarySnapshot::working() const
{
    return (has_action && (action.state == "queued" || action.state == "running")) ||
           (has_storage_job && storage_job.active) || storage_busy;
}

const LibraryGame *LibrarySnapshot::game(std::string_view title_id,
                                         std::string_view source_key) const
{
    for (const LibraryGame &candidate : games)
    {
        if (candidate.title_id == title_id && candidate.source_key == source_key)
            return &candidate;
    }
    return nullptr;
}

std::vector<Game> group_games(const std::vector<Release> &releases)
{
    std::vector<Game> games;
    for (const Release &release : releases)
    {
        Game *game = nullptr;
        for (Game &existing : games)
        {
            if (existing.id == release.game_id)
            {
                game = &existing;
                break;
            }
        }
        if (game == nullptr)
        {
            games.emplace_back();
            game = &games.back();
            static_cast<AddonInfo &>(*game) = release;
            game->id = release.game_id;
            game->platform = release.platform;
            game->title = release.title;
            game->genre = release.genre;
            game->tagline = release.tagline;
            game->description = release.description;
            game->artwork_layout = release.artwork_layout;
            game->publisher = release.publisher;
            game->release_date = release.release_date;
            game->insights = release.insights;
        }
        game->releases.push_back(release);
    }
    // Options that run on this PS5 first, so the first one shown is one that will.
    for (Game &game : games)
        std::stable_partition(game.releases.begin(), game.releases.end(),
                              [](const Release &r) { return r.firmware_issue.empty(); });
    return games;
}

std::string firmware_label(const Release &r)
{
    if (r.mounter == "kraken" && r.firmware_issue.empty())
        return tr("Runs with Kraken Mount");
    if (r.firmware_issue == "ps5-package")
        return tr("Needs firmware {version} or lower", {{"version", r.max_firmware}});
    if (r.firmware_issue == "newer-firmware")
        return tr("Needs firmware {version} or later", {{"version", r.required_firmware}});
    std::string text;
    if (!r.required_firmware.empty())
        text = tr("Firmware {version} or later", {{"version", r.required_firmware}});
    if (!r.max_firmware.empty())
        text += (text.empty() ? "" : " \xC2\xB7 ") + tr("Firmware {version} or lower", {{"version", r.max_firmware}});
    return text;
}

std::string firmware_warning(const Release &r, const std::string &current)
{
    if (r.firmware_issue == "ps5-package")
        return tr("This download needs firmware {version} or lower. This PS5 is on {current}.",
                  {{"version", r.max_firmware}, {"current", current}});
    if (r.firmware_issue == "newer-firmware")
        return tr("This download needs firmware {version} or later. This PS5 is on {current}.",
                  {{"version", r.required_firmware}, {"current", current}});
    return {};
}

static std::vector<int> new_on_orbit_picks(const std::vector<Game> &games)
{
    std::vector<int> result;
    for (int i = 0; i < static_cast<int>(games.size()); ++i)
    {
        const auto &releases = games[static_cast<std::size_t>(i)].releases;
        const int rank = releases.empty() ? 0 : releases.front().new_on_orbit_rank;
        if (rank >= 1 && rank <= 100) result.push_back(i);
    }
    std::stable_sort(result.begin(), result.end(), [&](int a, int b)
    {
        const Game &x = games[static_cast<std::size_t>(a)];
        const Game &y = games[static_cast<std::size_t>(b)];
        const int xr = x.releases.front().new_on_orbit_rank;
        const int yr = y.releases.front().new_on_orbit_rank;
        return xr != yr ? xr < yr : compare_folded(x.title, y.title) < 0;
    });
    if (result.size() > 40) result.resize(40);
    return result;
}

std::vector<int> latest_games(const std::vector<Game> &games, std::string_view today)
{
    const auto pinned = new_on_orbit_picks(games);
    std::vector<int> result;
    for (int i = 0; i < static_cast<int>(games.size()); ++i)
    {
        const std::string &date = games[static_cast<std::size_t>(i)].release_date;
        if (!date.empty() && std::string_view(date) <= today &&
            std::find(pinned.begin(), pinned.end(), i) == pinned.end())
            result.push_back(i);
    }
    std::stable_sort(result.begin(), result.end(),
                     [&](int a, int b)
                     {
                         const Game &x = games[static_cast<std::size_t>(a)];
                         const Game &y = games[static_cast<std::size_t>(b)];
                         if (x.release_date != y.release_date)
                             return x.release_date > y.release_date;
                         return compare_folded(x.title, y.title) < 0;
                     });
    if (result.size() > 20)
        result.resize(20);
    return result;
}

int grid_jump(int index, int count, int columns, int rows)
{
    if (count <= 0 || columns <= 0)
        return index;
    const int row = std::clamp(index / columns + rows, 0, (count - 1) / columns);
    return std::min(row * columns + index % columns, count - 1);
}

std::vector<int> newly_added_games(const std::vector<Game> &games, const std::vector<int> &latest)
{
    std::vector<int> result;
    std::vector<std::pair<int, double>> dated;
    for (int i = 0; i < static_cast<int>(games.size()); ++i)
    {
        if (std::find(latest.begin(), latest.end(), i) != latest.end())
            continue;
        const Game &game = games[static_cast<std::size_t>(i)];
        const int rank = game.releases.empty() ? 0 : game.releases.front().new_on_orbit_rank;
        if (rank >= 1 && rank <= 100)
        {
            result.push_back(i);
            continue;
        }
        const double added = game.releases.empty() ? 0 : addition_time(game.releases.front().added_at);
        if (added > 0)
            dated.emplace_back(i, added);
    }
    std::stable_sort(result.begin(), result.end(), [&](int a, int b)
    {
        const Game &x = games[static_cast<std::size_t>(a)];
        const Game &y = games[static_cast<std::size_t>(b)];
        const int xr = x.releases.front().new_on_orbit_rank;
        const int yr = y.releases.front().new_on_orbit_rank;
        return xr != yr ? xr < yr : compare_folded(x.title, y.title) < 0;
    });
    if (result.size() > 40)
        result.resize(40);
    std::stable_sort(dated.begin(), dated.end(), [&](const auto &a, const auto &b)
    {
        const Game &x = games[static_cast<std::size_t>(a.first)];
        const Game &y = games[static_cast<std::size_t>(b.first)];
        return a.second != b.second ? a.second > b.second
            : x.release_date != y.release_date ? x.release_date > y.release_date
            : compare_folded(x.title, y.title) < 0;
    });
    if (dated.size() > 40 - result.size())
        dated.resize(40 - result.size());
    std::stable_sort(dated.begin(), dated.end(), [&](const auto &a, const auto &b)
    {
        const Game &x = games[static_cast<std::size_t>(a.first)];
        const Game &y = games[static_cast<std::size_t>(b.first)];
        return x.release_date != y.release_date ? x.release_date > y.release_date
                                               : compare_folded(x.title, y.title) < 0;
    });
    for (const auto &entry : dated)
        result.push_back(entry.first);
    return result;
}

int playtime_minutes(const GameInsights &i, std::string_view style) { return style=="extras"?i.extras_minutes:style=="completionist"?i.completionist_minutes:i.main_minutes; }
std::string insight_summary(const GameInsights &i) {
    std::string out=i.score>=0?std::string(tr("Metacritic"))+" "+i18n::count(i.score):"";
    if (i.main_minutes>0) out+=(out.empty()?"":" · ")+tr("About {hours} h",{{"hours",i18n::decimal(i.main_minutes/60.0)}});
    return out;
}
std::vector<std::pair<std::string,std::string>> insight_facts(const GameInsights &i) {
    std::vector<std::pair<std::string,std::string>> out;
    if(i.score>=0)out.emplace_back("Metacritic",i18n::count(i.score)+" / 100");
    const int times[]={i.main_minutes,i.extras_minutes,i.completionist_minutes}; const char *labels[]={"Main Story","Main + Extras","Completionist"};
    for(int n=0;n<3;++n)if(times[n]>0)out.emplace_back(std::string("HowLongToBeat · ")+tr(labels[n]),tr("About {hours} h",{{"hours",i18n::decimal(times[n]/60.0)}}));
    return out;
}
bool matches_query(const Game &game, std::string_view query)
{
    query = trim(query);
    if (query.empty())
        return true;
    if (contains_folded(game.title, query) || contains_folded(game.base_title, query))
        return true;
    for (const Release &release : game.releases)
    {
        if (contains_folded(release.title_id, query))
            return true;
    }
    return false;
}

const char *sort_label(Sort sort)
{
    switch (sort)
    {
    case Sort::added:
        return tr("Recently added");
    case Sort::title:
        return tr("Title A\xE2\x80\x93Z");
    case Sort::title_desc:
        return tr("Title Z\xE2\x80\x93"
               "A");
    case Sort::score: return tr("Lowest score");
    case Sort::score_desc: return tr("Highest score");
    case Sort::time: return tr("Shortest playtime");
    case Sort::time_desc: return tr("Longest playtime");
    case Sort::size:
        return tr("Smallest download");
    case Sort::size_desc:
        return tr("Largest download");
    default:
        return tr("Release date (newest first)");
    }
}

const char *size_label(SizeFilter size)
{
    switch (size)
    {
    case SizeFilter::small:
        return tr("Under 10 GB");
    case SizeFilter::medium:
        return tr("10\xE2\x80\x93"
                  "50 GB");
    case SizeFilter::large:
        return tr("50\xE2\x80\x93"
                  "100 GB");
    case SizeFilter::huge:
        return tr("100 GB or more");
    default:
        return tr("Any size");
    }
}

bool contains(const std::vector<std::string> &values, std::string_view value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

namespace
{
constexpr std::pair<const char *, const char *> kTags[] = {
    {"exclusive", /* i18n */ "PlayStation exclusive"},
    {"local-coop", /* i18n */ "Local co-op"},
    {"split-screen", /* i18n */ "Split-screen"},
    {"open-world", /* i18n */ "Open world"},
    {"story-driven", /* i18n */ "Story-driven"},
    {"soulslike", /* i18n */ "Soulslike"},
    {"psvr-supported", /* i18n */ "PSVR supported"},
    {"psvr2-supported", /* i18n */ "PSVR2 supported"},
    {"turn-based", /* i18n */ "Turn-based"},
};
}

const char *tag_label(std::string_view tag)
{
    for (const auto &known : kTags)
        if (tag == known.first)
            return known.second;
    return nullptr;
}

std::vector<std::string> catalogue_tags(const std::vector<Game> &games)
{
    std::vector<std::string> out;
    for (const auto &known : kTags)
    {
        const bool present = std::any_of(games.begin(), games.end(), [&](const Game &game) {
            return std::any_of(game.releases.begin(), game.releases.end(), [&](const Release &release) {
                return std::find(release.tags.begin(), release.tags.end(), known.first) != release.tags.end();
            });
        });
        if (present)
            out.push_back(known.first);
    }
    return out;
}

std::vector<std::string> game_genres(std::string_view genre)
{
    // " / " or a comma ("Action,Horror" in a few rows) separates genres. Keep Driving/Racing
    // and Music/Rhythm intact, matching the web clients.
    std::vector<std::string> out;
    std::size_t begin = 0;
    while (begin < genre.size())
    {
        const std::size_t slash = genre.find(" / ", begin), comma = genre.find(',', begin);
        const std::size_t end = std::min(slash, comma);
        std::string value(genre.substr(begin, end == std::string_view::npos ? genre.size() - begin : end - begin));
        const auto first = value.find_first_not_of(" \t\r\n");
        value = first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
        if (value == "Sports") value = "Sport";
        else if (value == "Simulator" || value == "シミュレーター") value = "Simulation";
        else if (value == "Racing" || value == "Driving" || value == "Racing/Driving") value = "Driving/Racing";
        else if (value == "RPG" || value == "Role-playing" || value == "Role Playing Game (RPG)") value = "Role Playing Games";
        else if (value == "ストラテジー") value = "Strategy";
        if (!value.empty() && !contains(out, value)) out.push_back(value);
        if (end == std::string_view::npos) break;
        begin = end + (end == comma ? 1 : 3);
    }
    return out;
}

std::vector<std::string> catalogue_genres(const std::vector<Game> &games)
{
    // Use the full catalogue, so search and other filters never remove choices.
    std::map<std::string, unsigned> counts;
    for (const Game &game : games)
        for (const std::string &genre : game_genres(game.genre))
            ++counts[genre];
    std::vector<std::string> out;
    for (const auto &[genre, count] : counts)
        if (count >= 10) out.push_back(genre);
    return out;
}

bool in_library(const Game &game, const std::vector<LibraryGame> &library)
{
    for (const Release &release : game.releases)
        for (const LibraryGame &copy : library)
            if (release.addon_id.empty() && copy.title_id == release.title_id && copy.platform == release.platform &&
                (copy.on_drive || copy.installed))
                return true;
    return false;
}

std::string release_format(const Release &release)
{
    if (release.asset_compression == "ZSTD") return "ZSTD";
    return release.content_format.empty() ? release.format : release.content_format;
}

bool release_matches(const Release &release, const BrowseFilters &filters)
{
    if (!filters.sources.empty() && !contains(filters.sources, release.source_id) &&
        !(contains(filters.sources, kTorboxSource) && contains(filters.torbox_sources, release.source_id)))
        return false;
    const std::string packaging = release.compression == "7z" ? "7z" : release.compression == "zip" ? "ZIP" : release.compression == "rar" ? "RAR" : "";
    if (!filters.formats.empty() && !contains(filters.formats, release_format(release)) && !contains(filters.formats, packaging))
        return false;
    if (!filters.region.empty() && (release.region.empty() ? "unknown" : release.region) != filters.region)
        return false;
    const double gb = release.size_bytes / 1e9;
    return filters.size == SizeFilter::small    ? gb < 10.0
           : filters.size == SizeFilter::medium ? gb >= 10.0 && gb < 50.0
           : filters.size == SizeFilter::large  ? gb >= 50.0 && gb < 100.0
           : filters.size == SizeFilter::huge   ? gb >= 100.0
                                                : true;
}

int preferred_release(const Game &game, const BrowseFilters *filters, std::string_view last_source,
                      std::string_view last_format)
{
    const bool filtered = filters && (!filters->sources.empty() || !filters->formats.empty() ||
                                      !filters->region.empty() || filters->size != SizeFilter::any);
    int best = 0;
    std::array<int, 3> best_rank{3, 3, 3};
    for (std::size_t i = 0; i < game.releases.size(); ++i)
    {
        const Release &r = game.releases[i];
        const std::array<int, 3> rank{
            filtered && release_matches(r, *filters) ? 0 : 1,
            !last_source.empty() && r.source_id == last_source ? (release_format(r) == last_format ? 0 : 1) : 2,
            r.browser_available ? 1 : 0,
        };
        if (rank < best_rank)
        {
            best_rank = rank;
            best = static_cast<int>(i);
        }
    }
    return best;
}

namespace
{
// The game's smallest option matching every filter, or -1 when it doesn't show in Browse.
double smallest_match(const Game &game, const BrowseFilters &filters, const std::vector<std::string> &favourites,
                      const std::vector<LibraryGame> &library)
{
    if ((!filters.addon_base.empty() && game.base_game_id != filters.addon_base) ||
        (!filters.addon_type.empty() && game.addon_type != filters.addon_type))
        return -1.0;
    if (!matches_query(game, filters.query))
        return -1.0;
    if (!filters.platform.empty() && game.platform != filters.platform)
        return -1.0;
    if (!filters.minimum_score.empty() && game.insights.score < std::atoi(filters.minimum_score.c_str())) return -1.0;
    if (!filters.playtime_range.empty()) {
        const int m=playtime_minutes(game.insights,filters.play_style);const auto &r=filters.playtime_range;
        if (m<0 || !(r=="under5"?m<300:r=="5to10"?m>=300&&m<600:r=="10to20"?m>=600&&m<1200:r=="20to40"?m>=1200&&m<2400:r=="40plus"&&m>=2400)) return -1.0;
    }
    if (filters.favourites && !contains(favourites, game.id))
        return -1.0;
    if (filters.hide_installed && in_library(game, library))
        return -1.0;
    if (!filters.tags.empty() &&
        std::none_of(game.releases.begin(), game.releases.end(), [&](const Release &release) {
            return std::any_of(release.tags.begin(), release.tags.end(),
                               [&](const std::string &tag) { return contains(filters.tags, tag); });
        }))
        return -1.0;
    if (!filters.genres.empty())
    {
        const auto genres = game_genres(game.genre);
        if (std::none_of(genres.begin(), genres.end(), [&](const std::string &genre) { return contains(filters.genres, genre); }))
            return -1.0;
    }
    double smallest = -1.0;
    for (const Release &release : game.releases)
    {
        if (!release_matches(release, filters))
            continue;
        if (smallest < 0.0 || release.size_bytes < smallest)
            smallest = release.size_bytes;
    }
    return smallest;
}
} // namespace

long browse_count(const std::vector<Game> &games, const BrowseFilters &filters,
                  const std::vector<std::string> &favourites, const std::vector<LibraryGame> &library)
{
    return static_cast<long>(std::count_if(games.begin(), games.end(), [&](const Game &game)
                                           { return smallest_match(game, filters, favourites, library) >= 0.0; }));
}

std::vector<int> browse(const std::vector<Game> &games, const BrowseFilters &filters,
                        const std::vector<std::string> &favourites, const std::vector<LibraryGame> &library)
{
    struct Match
    {
        int index;
        double size; // the smallest matching option
    };
    std::vector<Match> matches;
    for (int i = 0; i < static_cast<int>(games.size()); ++i)
    {
        const double smallest = smallest_match(games[static_cast<std::size_t>(i)], filters, favourites, library);
        if (smallest >= 0.0)
            matches.push_back({i, smallest});
    }
    const Sort sort = filters.sort;
    std::stable_sort(matches.begin(), matches.end(),
                     [&](const Match &a, const Match &b)
                     {
                         const Game &x = games[static_cast<std::size_t>(a.index)];
                         const Game &y = games[static_cast<std::size_t>(b.index)];
                         const int title = compare_folded(x.title, y.title);
                         if (sort==Sort::score || sort==Sort::score_desc || sort==Sort::time || sort==Sort::time_desc) {
                             const bool score=sort==Sort::score || sort==Sort::score_desc;
                             const int av=score?x.insights.score:playtime_minutes(x.insights,filters.play_style);
                             const int bv=score?y.insights.score:playtime_minutes(y.insights,filters.play_style);
                             if ((av<0)!=(bv<0)) return av>=0;
                             if (av!=bv) return sort==Sort::score_desc || sort==Sort::time_desc ? av>bv:av<bv;
                             return title<0;
                         }
                         switch (sort)
                         {
                         case Sort::title_desc:
                             return title > 0;
                         case Sort::release:
                             // Newest first; an undated game sorts as "" (last).
                             if (x.release_date != y.release_date)
                                 return x.release_date > y.release_date;
                             return title < 0;
                         case Sort::added:
                         {
                             // Newest on Orbit first, as the browser's "added" sort.
                             static const std::string undated;
                             const std::string &xa = x.releases.empty() ? undated : x.releases.front().added_at;
                             const std::string &ya = y.releases.empty() ? undated : y.releases.front().added_at;
                             if (xa != ya)
                                 return xa > ya;
                             return title < 0;
                         }
                         case Sort::size:
                             if (a.size != b.size)
                                 return a.size < b.size;
                             return title < 0;
                         case Sort::size_desc:
                             if (a.size != b.size)
                                 return a.size > b.size;
                             return title < 0;
                         default:
                             return title < 0;
                         }
                     });
    std::vector<int> result;
    result.reserve(matches.size());
    for (const Match &match : matches)
        result.push_back(match.index);
    return result;
}

std::vector<std::pair<std::string, std::string>> source_choices(const std::vector<Game> &games)
{
    std::vector<std::pair<std::string, std::string>> choices;
    for (const Game &game : games)
    {
        for (const Release &release : game.releases)
        {
            bool known = false;
            for (const auto &choice : choices)
                known = known || choice.first == release.source_id;
            if (!known && !release.source_id.empty())
                choices.emplace_back(release.source_id, release.provider);
        }
    }
    return choices;
}

std::vector<std::string> format_choices(const std::vector<Game> &games)
{
    std::vector<std::string> choices;
    for (const Game &game : games)
    {
        for (const Release &release : game.releases)
        {
            const std::string format = release_format(release);
            if (!format.empty() && format != "Unknown" && !contains(choices, format))
                choices.push_back(format);
            const std::string packaging = release.compression == "7z" ? "7z" : release.compression == "zip" ? "ZIP" : release.compression == "rar" ? "RAR" : "";
            if (!packaging.empty() && !contains(choices, packaging)) choices.push_back(packaging);
        }
    }
    return choices;
}

bool job_active(const Job &job)
{
    return job.status == "queued" || job.status == "downloading" || job.status == "retrying" ||
           job.status == "verifying" || job.status == "waiting-extraction" || job.status == "inspecting" || job.status == "extracting";
}

bool job_inactive(const Job &job)
{
    return job.status == "complete" || job.status == "cancelled";
}

const char *status_label(std::string_view status)
{
    if (status == "downloading")
        return tr("Downloading");
    if (status == "verifying")
        return tr("Verifying");
    if (status == "waiting-extraction") return tr("Waiting to unpack");
    if (status == "inspecting") return tr("Inspecting archive");
    if (status == "extracting")
        return tr("Extracting");
    if (status == "queued")
        return tr("Queued");
    if (status == "retrying")
        return tr("Retrying");
    if (status == "paused")
        return tr("Paused");
    if (status == "error")
        return tr("Needs attention");
    if (status == "complete")
        return tr("Downloaded");
    if (status == "cancelled")
        return tr("Cancelled");
    return "";
}

const Job *release_job(const Release &release, const std::vector<Job> &jobs)
{
    const Job *best = nullptr;
    for (const Job &job : jobs)
    {
        if (job.release_id != release.id || job.title_id != release.title_id ||
            job.format != release.format || job.filename != release.filename ||
            job.total != release.size_bytes || job.status == "cancelled")
            continue;
        if (best == nullptr || status_rank(job.status) < status_rank(best->status))
            best = &job;
    }
    return best;
}

CollectionState release_state(const Release &release, const std::vector<Job> &jobs,
                              const std::vector<LibraryGame> &library)
{
    CollectionState state;
    const Job *job = release_job(release, jobs);
    if (job != nullptr && job->status != "complete")
    {
        state.kind = CollectionState::Kind::download;
        state.label = status_label(job->status);
        state.job = job;
        return state;
    }
    // ShadowMount reports no trustworthy version, so a versioned option is
    // never treated as already in the Library on the strength of its title ID.
    const LibraryGame *related = nullptr;
    for (const LibraryGame &copy : library)
    {
        if (!release.addon_id.empty() || copy.title_id != release.title_id || copy.platform != release.platform ||
            !(copy.on_drive || copy.installed))
            continue;
        if (related == nullptr)
            related = &copy;
        const std::size_t slash = copy.path.rfind('/');
        const std::string_view name = slash == std::string::npos
                                          ? std::string_view(copy.path)
                                          : std::string_view(copy.path).substr(slash + 1);
        if (copy.on_drive && copy.format == release.format && release.version.empty() &&
            name == release.filename && (copy.size < 0.0 || copy.size == release.size_bytes))
        {
            state.kind = CollectionState::Kind::library;
            state.label = tr("In Library");
            state.library = &copy;
            return state;
        }
    }
    if (job != nullptr)
    {
        state.kind = CollectionState::Kind::download;
        state.label = tr("Downloaded");
        state.job = job;
        return state;
    }
    if (related != nullptr)
    {
        state.kind = CollectionState::Kind::related;
        state.label = tr("Related copy in Library");
        state.library = related;
    }
    return state;
}

CollectionState game_state(const Game &game, const std::vector<Job> &jobs,
                           const std::vector<LibraryGame> &library)
{
    // An unfinished download first, then a Library copy, then a finished
    // download, then anything else (gameState in src/collection.ts).
    std::vector<CollectionState> states;
    for (const Release &release : game.releases)
    {
        CollectionState state = release_state(release, jobs, library);
        if (state.kind != CollectionState::Kind::none)
            states.push_back(state);
    }
    for (const CollectionState &state : states)
    {
        if (state.job != nullptr && state.job->status != "complete")
            return state;
    }
    for (const CollectionState &state : states)
    {
        if (state.kind == CollectionState::Kind::library)
            return state;
    }
    for (const CollectionState &state : states)
    {
        if (state.kind == CollectionState::Kind::download)
            return state;
    }
    return states.empty() ? CollectionState{} : states.front();
}

int default_drive(const std::vector<Drive> &drives, std::string_view preferred)
{
    for (int i = 0; i < static_cast<int>(drives.size()); ++i)
    {
        if (!preferred.empty() && drives[static_cast<std::size_t>(i)].id == preferred)
            return i;
    }
    for (int i = 0; i < static_cast<int>(drives.size()); ++i)
    {
        if (drives[static_cast<std::size_t>(i)].external)
            return i;
    }
    return drives.empty() ? -1 : 0;
}

SpacePlan plan_space(const Release &release, const Drive &drive, const std::vector<Job> &jobs)
{
    SpacePlan plan;
    for (const Job &job : jobs)
    {
        if (job.release_id == release.id && job.storage_id == drive.id &&
            job.status != "complete" && job.status != "cancelled")
        {
            plan.planned = true;
            break;
        }
    }
    // Download again starts from zero. Cancelled history may describe a deleted
    // partial or a disconnected drive and cannot reduce a fresh download's size.
    // Only the download: the console checks for room to unpack an archive before unpacking it.
    double needed = plan.planned ? 0.0 : release.size_bytes;
    plan.after = drive.projected_free_bytes - needed;
    plan.enough = plan.after >= kSpaceMargin;
    return plan;
}

int default_drive(const std::vector<Drive> &drives, std::string_view preferred,
                  const Release &release, const std::vector<Job> &jobs)
{
    std::vector<SpacePlan> plans;
    plans.reserve(drives.size());
    for (const Drive &drive : drives) plans.push_back(plan_space(release, drive, jobs));
    for (std::size_t i = 0; i < drives.size(); ++i)
        if (plans[i].planned) return static_cast<int>(i);
    int best = -1;
    for (std::size_t i = 0; i < drives.size(); ++i)
    {
        if (!plans[i].enough) continue;
        if (!preferred.empty() && drives[i].id == preferred) return static_cast<int>(i);
        if (best < 0 || plans[i].after > plans[static_cast<std::size_t>(best)].after) best = static_cast<int>(i);
    }
    return best >= 0 ? best : default_drive(drives, preferred);
}

std::string format_label(const Release &release)
{
    const std::string format = release_format(release);
    if (release.compression.empty())
        return format;
    const std::string kind = release.compression == "7z" ? "7z"
                             : release.compression == "zip" ? "ZIP" : "RAR";
    return format == "Unknown" ? kind : kind + " \xC2\xB7 " + format;
}

JobView job_view(const Job &job)
{
    if (job.status == "complete")
        return JobView::finished;
    if (job.status == "cancelled")
        return JobView::cancelled;
    if (job.status == "error")
        return JobView::failed;
    return JobView::active;
}

const char *job_view_label(JobView view)
{
    switch (view)
    {
    case JobView::finished:
        return tr("Finished");
    case JobView::failed:
        return tr("Failed");
    case JobView::cancelled:
        return tr("Cancelled");
    default:
        return tr("Active");
    }
}

std::string format_bytes(double bytes)
{
    // 49.4 GB in English, 49,4 GB in German, 49,4 ГБ in Russian.
    const bool giga = bytes >= 1e9;
    return i18n::decimal(giga ? bytes / 1e9 : bytes / 1e6) + std::string(i18n::unit_space()) +
           std::string(i18n::unit(giga));
}

std::string format_time_left(double speed, double minutes)
{
    const long whole = static_cast<long>(std::max(0.0, minutes));
    if (whole < 60)
        return tr("{speed}/s \xC2\xB7 {minutes} min remaining",
                  {{"speed", format_bytes(speed)}, {"minutes", i18n::count(whole)}});
    return tr("{speed}/s \xC2\xB7 {hours} h {minutes} min remaining",
              {{"speed", format_bytes(speed)}, {"hours", i18n::count(whole / 60)}, {"minutes", i18n::count(whole % 60)}});
}

std::string format_minutes_left(double minutes)
{
    const long whole = static_cast<long>(std::max(0.0, minutes));
    if (whole < 60)
        return tr("{minutes} min left", {{"minutes", i18n::count(whole)}});
    return tr("{hours} h {minutes} min left", {{"hours", i18n::count(whole / 60)}, {"minutes", i18n::count(whole % 60)}});
}

std::string format_retry_wait(double seconds)
{
    const long whole = static_cast<long>(std::max(0.0, seconds));
    if (whole < 120)
        return tr("Retrying in {seconds} s", {{"seconds", i18n::count(whole)}});
    return tr("Retrying in {minutes} min", {{"minutes", i18n::count((whole + 59) / 60)}});
}

std::string format_age(double unix_time, double now)
{
    if (unix_time <= 0.0)
        return tr("never");
    const double age = std::max(0.0, now - unix_time);
    if (age < 60.0)
        return tr("just now");
    if (age < 3600.0)
        return trn(static_cast<long>(age / 60.0), "{count} min ago", "{count} min ago");
    if (age < 86400.0)
        return trn(static_cast<long>(age / 3600.0), "{count} h ago", "{count} h ago");
    return trn(static_cast<long>(age / 86400.0), "{count} day ago", "{count} days ago");
}

std::string app_release_version(std::string_view content_version)
{
    // Two digits, three and three, as the package's param.json writes it.
    if (content_version.size() != 10 || content_version[2] != '.' || content_version[6] != '.')
        return {};
    unsigned parts[3] = {};
    const std::size_t starts[3] = {0, 3, 7};
    const std::size_t lengths[3] = {2, 3, 3};
    for (int i = 0; i < 3; ++i)
    {
        for (std::size_t j = 0; j < lengths[i]; ++j)
        {
            const char c = content_version[starts[i] + j];
            if (c < '0' || c > '9')
                return {};
            parts[i] = parts[i] * 10 + static_cast<unsigned>(c - '0');
        }
    }
    return std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." +
           std::to_string(parts[2]);
}

bool check_fresh(double checked_at, double now)
{
    return checked_at > 0.0 && now - checked_at <= 900.0;
}

std::string format_date(std::string_view iso)
{
    if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-')
        return std::string(iso);
    const int year = std::atoi(std::string(iso.substr(0, 4)).c_str());
    const int month = (iso[5] - '0') * 10 + (iso[6] - '0');
    const int day = (iso[8] - '0') * 10 + (iso[9] - '0');
    if (month < 1 || month > 12 || day < 1 || day > 31)
        return std::string(iso);
    // As the browser writes it (Intl, medium date): "Jul 18, 2026", "18.07.2026".
    return i18n::date(year, month, day);
}

// ---- Get everything ----

const Game *find_game(const std::vector<Game> &games, std::string_view id)
{
    for (const Game &game : games)
        if (game.id == id)
            return &game;
    return nullptr;
}

const Release *preferred_option(const Game &item, std::string_view title_id, std::string_view last_source,
                                std::string_view last_format)
{
    const Release *best = nullptr;
    std::array<int, 3> best_rank{9, 9, 9};
    for (const Release &r : item.releases)
    {
        if (!title_id.empty() && r.title_id != title_id)
            continue;
        // As preferred_release() without Browse's filters, ones that run here first.
        const std::array<int, 3> rank{
            r.firmware_issue.empty() ? 0 : 1,
            !last_source.empty() && r.source_id == last_source ? (release_format(r) == last_format ? 0 : 1) : 2,
            r.browser_available ? 1 : 0,
        };
        if (rank < best_rank)
        {
            best_rank = rank;
            best = &r;
        }
    }
    return best;
}

std::vector<EverythingRow> everything_rows(const Game &game, const Release &option, const std::vector<Game> &addons,
                                           const std::vector<LibraryGame> &library, std::string_view last_source,
                                           std::string_view last_format)
{
    std::vector<EverythingRow> rows;
    if (option.title_id.empty() || !game.addon_id.empty())
        return rows;
    bool installed = false;
    for (const LibraryGame &g : library)
        if (g.title_id == option.title_id && g.installed && (g.platform == option.platform || g.platform == "unknown"))
            installed = true;
    rows.push_back({EverythingRow::Kind::game, &game, &option, installed});
    for (const Game &addon : addons)
    {
        if (addon.addon_id.empty())
            continue;
        // Only the add-on's options for this title ID on this platform.
        Game same = addon;
        same.releases.clear();
        for (const Release &r : addon.releases)
            if (r.platform == option.platform)
                same.releases.push_back(r);
        const Release *pick = preferred_option(same, option.title_id, last_source, last_format);
        if (pick == nullptr)
            continue;
        for (const Release &r : addon.releases)
            if (r.id == pick->id)
                rows.push_back({EverythingRow::Kind::dlc, &addon, &r, false});
    }
    return rows;
}

bool everything_offered(const std::vector<EverythingRow> &rows)
{
    return std::any_of(rows.begin(), rows.end(), [](const EverythingRow &row) { return row.kind != EverythingRow::Kind::game; });
}

std::string everything_label(const EverythingRow &row)
{
    if (row.kind == EverythingRow::Kind::dlc)
        return tr("{title} (DLC)", {{"title", row.item ? row.item->title : std::string()}});
    return tr("Game");
}

std::string everything_summary(const std::vector<EverythingRow> &rows)
{
    long dlc = 0;
    bool installed = false;
    for (const EverythingRow &row : rows)
    {
        if (row.kind == EverythingRow::Kind::dlc)
            ++dlc;
        else
            installed = row.installed;
    }
    // An installed game isn't part of it: just the DLC (as src/GetEverything.tsx).
    if (installed)
        return tr("{count} DLC", {{"count", i18n::count(dlc)}});
    if (dlc > 0)
        return tr("Game and {count} DLC", {{"count", i18n::count(dlc)}});
    return tr("Game");
}

} // namespace orbit
