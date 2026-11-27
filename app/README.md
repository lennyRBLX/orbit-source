# Orbit Store for TV (native app)

The native TV app **1.13.7** is included with **Orbit Store 1.9.7**. Its FFPKG and its folder ZIP (for Kraken Mount) bundle the matching **1.9.7 service**.

A full-screen PS5 app for Orbit's storefront, drawn by the GPU instead of
the console browser. It is a client of the Orbit backend payload. Downloads,
the queue, storage and the catalogue still live in the payload, which keeps
running when the app is closed. The app talks to it on `127.0.0.1:34177` and
opens the console's own session, so no pairing code is needed.

The app carries a copy of Orbit. When nothing answers on Orbit's port, it
starts Orbit through the ELF loader the jailbreak runs on `127.0.0.1:9021`,
so a separate payload launch is unnecessary when the loader is available.

It follows the browser storefront's design and rules (`src/`)
screen for screen:

- **Discover** (the opening page): the focused game's art behind the hero,
  then New on Orbit, Latest releases and an All games grid, largest downloads first,
  that scrolls through the whole catalogue. L2 / R2 jump 16 rows at a time.
- **Browse**: search, All games or Favourites, Source,
  Platform, Format, Region, Download size and Sort by (release date, newest first, by default),
  Reset filters, Saved filters (save the filters, sort and search under a name on the
  keyboard, apply one, or Square to delete it; shared with the browser and Orbit Zero),
  and a grid of covers with each game's state.
- **Add-ons (1.5.0)**: a separate page after Browse for DLC, activation
  packages and bundles. Search by add-on or base-game name; filter by Platform,
  Type, Source and Region. **Add-ons (count)** on a game’s details opens its
  extras. All four layouts share requirements and explicit package selection.
  See [Add-ons](../docs/addons.md).
- **Scores and playtimes (1.9.0)**: catalogue-supplied Metacritic scores and HowLongToBeat estimates on cards and details, with score/time filters, sorting and saved Browse presets. **Appearance → Scores and playtimes on game cards** hides card labels without removing details or filters.
- **Quick favourites**: press Square on the selected game in Browse or Discover
  to save it or remove it from your favourites.
- **Library**: the games ShadowMount reports on the console and its drives,
  with one counted All games / Available now / Missing source filter,
  independent Installed and Mounted badges, search, Location and Format; a game's details with
  Mount/Unmount and Copy/Move to drive (the same confirmations as the web);
  Scan for games (Library reads, scans and measures sizes on its own). **Files** (in Library) browses
  Orbit's drives: open, unpack, install, copy, move, rename and delete, with
  the operation's progress in a dialog. **Verify & Repair** inspects supported game folders and asks for approval before applying proposed fixes. See [Files](../docs/library.md#files).
- **Downloads**: Active, Finished, Failed and Cancelled, with pause, resume,
  retry, cancel (keep or delete the partial file) and remove from history.
- **Game page**: full-width artwork, the tagline and description, a primary
  download button, Favourite and About. The primary opens Download options;
  Source, Download using and Save to each open a choice panel. Back restores
  the previous control, and the enabled inner download button starts selected.
  View download / View in Library / Download another copy, storage estimates,
  metadata and Vikingfile instructions remain available.
- **App settings** (the gear in the top bar): controls grouped by task, with
  separate Downloads, Storage & cleanup, Sources & accounts and Devices
  sections. See the complete category list below.

Connect TorBox in the browser's **App settings → Sources & accounts → TorBox**, then select
**Download using → TorBox** on a supported game's native download page.
Without an account, **Set up TorBox** opens the browser with your game, source and destination preserved. The queue labels these jobs **via TorBox**. Account connection and host/file
limits still apply. Orbit remembers Direct/TorBox; **Always use TorBox** applies to new downloads without changing existing jobs. See [TorBox connection and recovery](../docs/debrid.md).

For Vikingfile, **Open Vikingfile account** and **Check sign-in** are available in settings. Orbit uses only the active user’s Vikingfile session to request a matching direct link before opening a download page. A bounded Premium download and pause/resume passed on the real PS5, and the owner confirmed native Premium downloads. Zero’s live Premium download and the issue reporter’s rate-limit case remain unconfirmed.

For a Vikingfile or AkiraBox download that needs a provider page, choose the download option and save location, then
open the provider page on PS5. Complete any verification and wait without
pressing Download. Orbit checks the ready file link, closes its provider window
and adds the download to your chosen drive’s queue.
The native app shows verification progress, cancellation and **View download**
when the file is ready. Going back from the progress panel keeps verification
running; only **Cancel verification** stops it. Your game, source and drive stay
selected. A download is added only after the matching file link is validated.

Orbit’s download service runs in the background; you do not need to open its
browser interface first. If a source or drive is no longer available, choose an
available option explicitly. An older service without provider-browser support
shows an update message.
Auto-start and payload-manager setup remain in the browser version;
App settings → Updates & startup opens it. In 1.5.0, **Save diagnostic report** also
saves a report directly from the native app to the selected drive.

## App settings

Settings use the same categories in Store, Crossbar, Cinema and Panorama:

- **Downloads:** simultaneous downloads, connections, retry preferences and automatic shutdown. **Wait for** selects the whole queue or a download; other unfinished work still blocks shutdown.
- **Storage & cleanup:** default drive, archive/package cleanup and history file-removal preferences.
- **Sources & accounts:** enabled sources, Archive.org, Vikingfile and TorBox.
- **Devices:** pairing and Orbit Zero companion controls.
- **Appearance:** layout, motion, language and layout-specific options.
- **Notifications:** console and ntfy preferences.
- **Game catalogue:** catalogue details and refresh.
- **Updates & startup:** native/service updates and restart, with browser access for payload-manager and auto-start controls. The detected native installation path is used; saved and running versions are shown separately. Manual payload launch remains the fallback when no loader is available.
- **Diagnostics & about:** versions and diagnostic report export. Reports omit titles, filenames, paths, links, passwords and tokens.
- **Support Orbit:** optional support links and QR codes.

Existing preferences and entry-point shortcuts are retained. Menus trap controller focus, including the shutdown target selector. Arabic mirrors navigation; Japanese, Chinese and Korean load the selected language font. Language names are also baked into the primary fonts so the picker is readable before selection.

Download options show known firmware requirements and list incompatible choices
last with a warning; they remain selectable. Completed downloads offer explicit
package installation or ShadowMount registration status. See [installation](../docs/package-installation.md).

When the app opens it asks Orbit to look for updates of both parts (the
same automatic check the browser makes, with a shared six-hour cooldown).
A newer release puts a dot on the gear and on Updates and shows a toast
once. Nothing is downloaded or installed until it is chosen.

## What the console needs

- A jailbreak that loads **kstuff** (fake-signed apps) and **ShadowMountPlus**
  (mounts the app and adds its tile). The usual Relapse loader chain loads
  both. Without them the app does not start; the browser icon still works.
- An **ELF loader on port 9021**, as the jailbreak's loader chain provides,
  for the app to start Orbit. Without one, start Orbit from a payload
  manager; the app then waits for it and offers **Try again**.
- Orbit with the artwork endpoint (`/api/v1/art`, 0.6.0 and later), which the app
  brings with it. A running Orbit without it (0.5.1 and older releases) works
  too, but every cover shows its title's initial. To use the app's copy,
  stop the running Orbit (or restart the console) before opening the app.

### Updating Orbit from the native app

From native 1.12.2, **Update Orbit** uses the verified `serviceVersion` in the native update feed to avoid downloading a separate ELF when the new FFPKG already includes it. Select **Close and reopen** after the package is saved. The reopened app completes the pending update by saving the queue, stopping the older service and starting the newer bundled service through the local loader. Interrupted downloads continue. A manually replaced package offers **Update download service** when its bundled copy is newer. Payload Manager users without port 9021 retain the manual service-launch route.

### Starting Orbit

When the first status request is refused (nothing listens on port 34177),
the app sends Orbit to the loader once, shows **Starting Orbit…** and waits up
to 30 seconds for it to answer. It sends the copy Orbit saved in
`/data/orbit-store/orbit_store.elf`, which payload managers start and Orbit's
updater keeps current, unless the copy in the package (`/app0/orbit/`) is a
newer release or the saved one can't be read. A slow answer is not a refusal
and never triggers a start, and losing Orbit later is not answered with
another payload: the offline screen's **Try again** starts it on request.
Orbit never replaces a newer saved copy with an older one, so an older
package can't downgrade an Orbit you updated. The log records each
attempt (`[ORBIT] start: ...`).

## Install downloaded games (1.5.0)

In **Downloads**, open a completed game’s **Downloaded files** or **Install on PS5**. Choose a PKG/FPKG and select **Install on PS5**. Bundles require an explicit choice for each package; installation progress is separate from the download. Files are kept unless deletion after successful installation is enabled. The native FFPKG includes the matching service; update a separately managed ELF copy only if your setup uses one.

New exFAT, FFPFSC and extracted game-folder downloads or transfers automatically request a scan when a compatible ShadowMount local API is running. If it was unavailable, start it and select **Scan with ShadowMount** in the download’s details when offered. This behavior is shared by Store, Crossbar, Cinema and Panorama. See [installing downloaded games](../docs/package-installation.md) for requirements and recovery.

## Install on the PS5

1. Stop a running Orbit first (App settings, Stop Orbit, in the browser
   version, or restart the console and run the jailbreak). A second copy of
   the payload does not replace a running one.
2. Optional: send `build/orbit_store.elf` yourself. The app starts its own
   copy of Orbit when none is running.
3. Copy **one** of these to the console:
   - `dist/PPSA99177.ffpkg` into `/data/homebrew/`, or
   - the folder `dist/PPSA99177/` to `/data/homebrew/PPSA99177/`.

   ShadowMountPlus registers it about ten seconds after the copy finishes.
4. Open **Orbit Store** from the Games row. The icon is the Solid Planet; the
   browser shortcut keeps its own icon in Media.
5. Discover opens first. If Orbit is not running you see
   **Starting Orbit…**, then Discover. If the app can't start it you see
   "Orbit isn't running" with the reason; start Orbit from a payload manager
   and the app reconnects by itself, or press **Try again**.

Title ID `PPSA99177`; the content version is defined in the package metadata. To update manually, replace the
image or folder and, as ShadowMountPlus stages `param.json` and the icon
once, also replace its staged copies if the tile text or icon changed.

Keep `downloadDataSize` in `sce_sys/param.json` at 256, the kit's package value.

The app writes only its own log (`/download0/hui/dev/app.log` inside its
sandbox; `/mnt/sandbox/PPSA99177_000/download0/hui/dev/app.log` over FTP
while it runs). Close it with the PS button and Close Application.

## Interface language

Orbit follows the PS5 system language when **Automatic** is selected, or the override in **App settings → Appearance → Language**. All 26 languages listed in the [project README](../README.md) are supported. Arabic mirrors the layout and controller navigation; Japanese, Chinese and Korean use their dedicated fonts. Unsupported system languages use English. Each app or browser keeps its own language preference.

The interface translations share `i18n/` with the browser. The build packages them under `assets/i18n/`. Game names and descriptions keep the catalogue’s original text. For translation checks, see [BUILDING.md](../BUILDING.md#interface-languages).

## Controls

| Button | Does |
| --- | --- |
| D-pad / left stick | Move the focus. Up from a screen reaches the top bar. |
| Cross | Select |
| Circle | Back: leaves a game page, closes the keyboard or a dialog |
| L1 / R1 | Previous / next tab |
| L2 / R2 | In the Browse and Discover game grids: jump 16 rows (96 games) up / down, keeping the column |
| Square | On a selected Browse or Discover game: toggle Favourite. In search: delete. In Browse's field: clear the search. In Library: cancel a running copy or move. On a game page: About. |
| Triangle | In search: space. In the Browse grid: search again. On a game page: toggle Favourite. |

Dropdowns (filters, Save to, a copy's destination) open with Cross; move with
the D-pad, choose with Cross, close with Circle.

## Build

Everything runs in the app's build image (Ubuntu 24.04 with Clang 18,
Mesa, .NET 8). On Apple silicon the image adds the x86-64 compiler builtins
from Ubuntu's archive at a pinned checksum.

Build Orbit first: the package carries `build/orbit_store.elf` (see
[BUILDING.md](../BUILDING.md)). The commands mount the repository root so the
app build can read it.

```sh
docker build -t orbit-app-build:0.1 app
docker run --rm -v "$PWD:/work" -w /work/app orbit-app-build:0.1 make ffpkg   # dist/PPSA99177{,.ffpkg,.zip}
docker run --rm -v "$PWD:/work" -w /work/app orbit-app-build:0.1 make preview # build/preview/pictures/*.png
docker run --rm -v "$PWD:/work" -w /work/app orbit-app-build:0.1 make test    # unit tests, sanitizers on
```

`ORBIT_PAYLOAD=<path>` bundles another build of Orbit, and
`ORBIT_PAYLOAD=none` builds an app without one, which only waits for Orbit.
The version written next to the copy comes from its `Orbit-Store/<version>`
user agent.

The first build fetches pinned dependencies into `app/.deps/` (ignored): the
PS5 payload SDK v0.42, the ps5-opengl 1.0.0 SDK, PacBrew v0.40.2 (for
libwebp), zlib 1.3.2, GoogleTest 1.17.0 and, for `.ffpkg`, UFS2Tool. Each is
checked against a SHA-256 or commit in `tools/`.


### Previews without a console

`make preview` runs the real app code on the PC (Mesa, surfaceless EGL) with
scripted controller input and writes a picture per step. It uses a built-in
stand-in backend with invented games and generated art (`host/fixture.cpp`).
To preview against a running Orbit backend instead, run the desktop backend
and give the preview its network:

```sh
docker run -d --name orbit-tv-backend -v "$PWD:/work" --entrypoint /work/build/orbit-host \
  orbit-build:0.1 --state /work/build/live-state
docker run --rm --network container:orbit-tv-backend -v "$PWD:/work" -w /work/app \
  -e ORBIT_BACKEND=127.0.0.1:34177 orbit-app-build:0.1 bash tools/preview.sh build/preview/live
```

Choose sources once in that backend (as on first run) or the app shows its
setup screen. Pictures are the PC's rendering; frame rate, sound and
controller feel are console checks.

## How it is built

```
src/orbit/       the app: API client, state sync, artwork, screens
src/main.cpp     console entry point
host/            PC preview and its stand-in backend
src/gfx, ui, core, audio, platform, runtime, third_party
                 the ps5-homebrew-ui kit (see Provenance)
tooling/, tools/ the native PS5 build: compile, link, FSELF signing, packaging
```

- **Starting Orbit.** `orbit/starter` picks the copy to send and streams it
  to the loader; `orbit/store` decides when (see Starting Orbit above).
- **Network.** `orbit/http` is a small HTTP/1.1 client for the loopback
  backend. `orbit/store` polls status, sources, the catalogue (only when its
  revision changes), the queue, storage, favourites and the Library (Orbit
  answers that from its cache and limits ShadowMount reads itself) on one
  worker thread (every second while Downloads is open, else every three)
  and carries out actions in order. Library drives are read only while the
  Library needs them. Screens read a snapshot; nothing on the frame waits
  for the network.
- **Rules.** `orbit/model` mirrors the browser's: grouping, Latest releases,
  Browse's filters and order (`src/browse.ts`), and where an option stands
  (`src/collection.ts`: an unfinished download, an exact Library copy, a
  finished download, or a related copy).
- **Artwork.** `GET /api/v1/art/<gameId>/<cover|hero|thumbnail>` on the backend fetches
  the catalogue's own HTTPS URL on its workers and keeps a bounded disk cache across service restarts. Tiles use smaller thumbnail artwork; cached fallbacks can be reused when a host fails (see
  [the API](../docs/api.md)). The app decodes WebP with libwebp (scaled while
  decoding) and PNG/JPEG with stb_image, at drawing size, on its own worker,
  then uploads at most two textures per frame. The heap is fixed at 128 MiB,
  so at most 64 covers and 4 banners stay resident.
- **Look.** The browser storefront's tokens and measurements (`src/styles.css`
  at 1920 x 1080, 1 rem = 20 px) in Inter, with the kit's SDF renderer: the
  same header, pill tabs, 3 px white focus ring with its glow behind the
  element, covers with names and dates, dropdowns, dialogs and veils over
  the art. The mark is rasterised on the CPU at start-up so its cut-outs
  work over art.

## Installing from Orbit

Install the TV app from the browser version:
**App settings → Updates & startup → TV app → Install on this PS5**, and update it there or in
the app itself (**App settings → Updates & startup**). Orbit downloads the image,
checks it and updates the detected installation location. A first installation
uses `/data/homebrew/`. Saved and running versions are separate; close and reopen
the app to activate its replacement (see the TV app installer in
[docs/api.md](../docs/api.md)). The same panel links to the latest GitHub
release for installing by hand.

Orbit finds the app through `tv-app.json` at the root of the public
repository's `main` branch:

`npm run release:package` generates the exact feed. It contains `name`,
`filename`, the app's own semantic `version`, the official FFPKG
release `url`, its SHA-256 `checksum` and byte `size`. The app version comes
from its content version and is separate from the release/service version.
Generate these values from the final package; do not copy example versions,
hashes or sizes into the live feed.

Upload `dist/PPSA99177.ffpkg` to the release first, then publish the feed.
Until the feed exists, the panel says the app hasn't been published yet.
The package command generates the feed with the exact image size and checksum.

## Provenance and licences

The app is GPL-3.0-or-later, like Orbit.

- `src/gfx`, `src/ui` (with `components`), `src/core`, `src/audio`,
  `src/platform`, `src/runtime`, `src/third_party/stb/{vorbis.c,stb_vorbis.inc}`,
  `tooling/`, `runtime/`, `host/platform_host.cpp`, `third_party/stb/` and
  most of `tools/` are from
  [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) at
  commit `4bd942579dd981b3df9c740438489ca6a614ddc0` (GPL-3.0-or-later;
  BlackBearReloaded; its native tooling derives from SharpProspero, GPL-3.0).
  They are unmodified except `tools/bake-fonts.sh` (Orbit's font table),
  `tools/build-tests.sh` (links libwebp, adds `host/`), `tools/build.sh`
  (copies the staged Orbit payload into the package) and the new
  `tools/preview.sh` and `tools/stage-payload.sh`.
- `assets/audio/sfx/glass` are the kit's interface sounds (BlackBearReloaded,
  distributed under the kit's licence).
- Inter 4.1 (`third_party/fonts`, SIL Open Font License 1.1); the baked atlases
  in `assets/fonts` carry the licence beside them.
- `src/third_party/stb/stb_image.inc` is stb_image (public domain or MIT) at
  the kit's pinned stb commit.
- Linked at build time, not stored here: the ps5-opengl SDK
  (GPL-3.0-or-later, with Mesa and OpenGNM PSBC under their own licences),
  libwebp 1.4.0 from PacBrew (BSD-3-Clause, `../licenses/libwebp-*`), and the
  kit's clean-room `libc.prx` shim generated from `tooling/`.
- Carried in the package: Orbit's own `orbit_store.elf`, with the components
  listed in [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md).

`npm run release:package` includes the exact app and payload source, the full
ps5-opengl release with its source archives and patches, both payload SDK
versions, the linked WebP and C++ runtime sources, and their notices. See
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md) and
[BUILDING.md](../BUILDING.md) for the inventory and rebuild instructions.

### Orbit Zero

Use native app **1.13.7**, running service **1.9.7** and the shared Orbit Zero **1.9.7** for the current controls and 26 interface languages. The FFPKG and Zero’s installers include service 1.9.7, and Zero fetches newer compatible services from the official feed; an explicit ELF can also be selected for an offline update. Zero integration originally required service 1.0.0, and PS4 browsing required service 1.1.0/native app 1.4.0; those are historical minimums, not the current versions.

Pair Orbit Zero on the computer with the running, matching Orbit ELF. In the TV app, open **App settings → Devices → Orbit Zero → Enable Orbit Zero**. The native storefront then uses the computer's catalogue and artwork; Download follows the computer's saved preference. Downloads shows the internet rate and the transfer rate to the PS5 separately. Keep the computer and Orbit Zero running while it transfers. Vikingfile verification opens on the computer, with a prompt to continue there. Turning this mode off affects new downloads; existing computer transfers retain their original route.

**Computer** is the default preference. **Prefer PS5** probes the selected source before starting a new direct console download and falls back to the computer when the console cannot fetch it. The computer and PS5 may use Wi-Fi or Ethernet on the same reachable local network; only direct console downloads require the PS5 to have internet access. See the [companion API](../docs/api.md#native-orbit-zero-controls) for registration, routing and token boundaries.

## Files navigation in 1.7.0

Select a folder to open it directly. In Store and Cinema, move right to the separate **…** control for folder actions. Files open their actions. Crossbar and Panorama retain their own selected-item controls. **Circle** closes a dialog first, then returns to the parent folder; from the drive list it returns to Library.
