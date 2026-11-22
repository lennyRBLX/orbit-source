# PEGASUS_DL_PORT: prompt

> Status: **implemented.** **Community catalogues** is selected during first-time setup.
> Orbit starts fetching after you acknowledge and save the download notice. The five default
> catalogues and any custom sources appear in the Community catalogues section in Sources.
> Orbit reads the individual links and labels in `downloadLinks`. Choosing a provider page checks
> the selected link for a PKG, FPKG, RAR or ZIP. An unlabeled RAR or ZIP goes through game-folder
> inspection and extraction. LZ4, ZSTD, exFAT, FFPFSC, PFS and 7z options are filtered.
> A page that cannot identify a single supported file, including an unresolved interactive
> link container, cannot add a download. Each catalogue is limited to 2,048 options and
> the importer takes one option per entry before filling in the remaining mirrors. It
> prefers direct files and known formats when choosing that first option.

---

## Prompt

You are working in the Orbit Store 1.9.6 source tree (GPL-3.0, PS5 payload in `backend/`, browser
interface in `src/`). Add **remote catalogue sources**, modelled on Pegasus DL+'s
`catalogs/default-sources.json`, **additively** alongside Orbit's built-in encrypted catalogue.
Nothing about the existing catalogue feed (`catalog/*.enc`, `backend/catalog.c`, the v3–v8 feeds,
the provider list in `backend/sources.c`) may change behaviour.

### Reference: the Pegasus DL+ source list

Pegasus DL+ ships a source manifest (`schemaVersion`, `revision`, `onboardingRevision`) with
`sources[]` entries shaped like:

```json
{
      "id": "dlps",
      "name": "DLPS Catalog",
      "description": "Games and DLCs from DLPSGAME in various formats",
      "url": "https://pegasus-dl.dev/catalogs/dlps.json",
      "defaultEnabled": true
    },
    {
      "id": "pippo-exfat",
      "name": "Pippo .exFAT",
      "description": "Games and DLCs in .exfat and .pkg format",
      "url": "https://pegasus-dl.dev/catalogs/pippo.json",
      "defaultEnabled": true
    },
    {
      "id": "pfs",
      "name": "PFS Catalog",
      "description": "Games and DLCs in .ffpfsc and .pkg format",
      "url": "https://pegasus-dl.dev/catalogs/pfs.json",
      "defaultEnabled": true
    },
    {
      "id": "evox-coreos",
      "name": "evoX-CoreOS",
      "description": "Homebrew apps and community utilities",
      "url": "https://raw.githubusercontent.com/nexgen999/evoX-CoreOS/refs/heads/main/json/pegasus-dl/catalog.json",
      "defaultEnabled": true
    },
    {
      "id": "zer0",
      "name": "ZER0GAME Catalog",
      "description": "Games and DLCs in .pkg format",
      "url": "https://pegasus-dl.dev/catalogs/zer0.json",
      "defaultEnabled": true
    }
```

Each `url` points at a plain-JSON catalogue. Before writing the adapter, fetch the sources
`catalog.json` once and record its actual field names in a test fixture
(`tests/fixtures/pegasus-catalog.json`). Don't guess the schema.

### 1. Backend: source manifest and settings (`backend/remote_sources.c/.h`, new)

- Ship a built-in manifest in the same shape as Pegasus's, with default entries for each Pegasus source like so:
  `evox-coreos` (above), `defaultEnabled: true`.
- Persist per-source state with the existing atomic state-file helpers (`backend/state_file.c`),
  in a new file such as `/data/orbit-store/remote-sources-v1.json`:
  `{ id, name, url, enabled, builtIn, lastFetchedAt, etag, lastError, entryCount }`.
- Let the user add a custom source by URL (HTTPS only, ≤2048 chars, `id` derived from the host +
  path hash, no duplicates) and remove or disable any source. Built-in entries can be disabled
  but not deleted. Bump a `revision` so a future manifest revision can add defaults without
  re-enabling ones the user turned off. That's how Pegasus uses `revision` /
  `onboardingRevision`.

### 2. Backend: fetching (`remote_sources.c`, worker thread)

- Use the existing libcurl patterns from `backend/catalog.c` (`collect`, `headers`, `progress`,
  stop checks, CA bundle, timeouts, `If-None-Match` with the stored ETag). Cap the response size
  (e.g. 8 MiB) and parse with cJSON.
- Refresh on start (after `catalog_start()`), on a long interval (6 h), and on demand from the API.
  The service tick retries enabled catalogues with zero download options after a one-minute wait.
  Manual refresh requests share a separate one-minute cooldown across paired clients. Refreshing
  requires the download notice and an enabled catalogue. The Community catalogues download
  toggle controls visibility in Browse.
- Cache the last good body per source under `/data/orbit-store/remote-cache/<id>.json` and
  load it at startup, so sources work offline.
- A failed fetch keeps the previous cache and records `lastError`. It never empties the source.

### 3. Backend: mapping entries to `Release`

- Write `pegasus_entry_to_release(const cJSON *, const char *source_id, Release *)` that maps a
  catalogue entry to Orbit's `Release` (`backend/orbit.h`): `id` = `rs-<source>-<hash>` (fits
  `char id[24]`), `title`, `title_id`, `platform` ("PS5"/"PS4"), `format`, `filename`, `url`,
  `size`, `sha256` if present, `compression` from the file extension (zip/7z/rar), and
  `source` = a new provider id `remote` (with `browser_url` keeping the catalogue's page link if any).
- Validate it like `release_parse()` does (identifier charset, lengths, HTTPS URLs). Drop invalid
  entries and count them in `lastError` rather than failing the whole source.
- **Respect the format filter:** only keep entries whose format is one Orbit currently offers
  (PKG, FPKG, game folder). Skip the rest.

### 4. Backend: merging, additively

- Keep remote releases in their own array (`orbit.remote_releases`, `remote_release_count`), guarded by
  the same lock as `orbit.releases`. Have `catalog_view.c` / `browser_match.c` iterate both
  arrays, so the built-in catalogue path is untouched when no remote sources are enabled.
- Add `ORBIT_SOURCE_REMOTE` (32U) to `orbit.h`, `source_flag("remote")` in `sources.c`, and a
  `{"remote", "Community catalogues", true}` row in `sources_json_locked()`, so the existing
  Source filter, counts and "acknowledged" notice cover remote entries too. Keep
  `ORBIT_SOURCE_NOTICE_VERSION` at 1, but require the source notice to be acknowledged before any remote
  source is fetched.
- Remote entries that match a catalogue game by `title_id` attach to that game as extra download
  options. Unmatched entries appear as standalone titles under a **Homebrew** platform tag.
- Downloads of remote releases go through the normal queue (`queue.c`), extraction and Kraken Mount
  handoff. No new download path.

### 5. API (`backend/api.c`)

- `GET /api/v1/remote-sources` → list with state.
- `POST /api/v1/remote-sources` `{url, name}` → add. `PATCH /api/v1/remote-sources/<id>` `{enabled}`.
  `DELETE /api/v1/remote-sources/<id>` (custom only). `POST /api/v1/remote-sources/refresh`.
- Mutations require the existing paired-client auth, and are refused while shutting down, like
  the other mutating routes.
- Add the sources' summary (count, last error codes, no URLs) to `diagnostics.c`.

### 6. Browser interface (`src/`)

- Extend `src/Sources.tsx` with a **Community catalogues** section: a list with an enabled toggle,
  entry count, last refresh and error, an *Add catalogue URL* field, *Remove* for custom entries,
  and *Refresh now*. Add API calls in `src/api.ts` and types in `src/types.ts`.
- Show a one-time notice when adding a custom URL. It should say that the catalogue is
  third-party, that Orbit doesn't vet it, and that the user is responsible for having
  permission to download what it lists.
- All new text goes through `t()`. Then run `node tools/i18n.mjs extract` and `check`.

### 7. Tests and done criteria

- Host tests: manifest revision merge (user-disabled defaults stay off), entry mapping against
  the recorded fixture, format filtering, invalid-entry handling, ETag/304 reuse, cache load
  offline, and confirming the built-in catalogue results are byte-identical with remote sources
  disabled.
- `npm test`, `npm run test:catalog`, then rebuild `build/orbit_store.elf` per `BUILDING.md`.
- Done when: all source entries appear under Source → Community catalogues, download and hand off
  through the normal queue, and the built-in catalogue still behaves exactly as before.
