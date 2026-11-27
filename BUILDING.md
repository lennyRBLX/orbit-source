# Building Orbit Store 1.9.7

Orbit 1.9.7 adds free-space management and downloads from pasted links. This source bundle builds the PS5 service and browser interface. Orbit Zero is maintained and packaged separately. See [release notes](https://github.com/saawant12/orbit-store-ps5/releases/tag/v1.9.7) for the user-facing changes.

This source builds the Orbit **1.9.7** service/browser payload. Game metadata and external artwork URLs come from the included catalogue snapshot; builds do not fetch game artwork.

Requirements: Node.js 22+, Python 3, a host C compiler, OpenSSL development files and Docker. On macOS use the Command Line Tools and `brew install openssl@3`; the build container already supplies Clang and OpenSSL. Run host Python tools through a project virtual environment. Catalogue tools compile a small host helper against OpenSSL; no extra Python packages are required.

The source archive includes `catalog/games.enc`, `catalog/releases.enc`, `catalog/addons.enc` and `catalog/catalog.enc`, together with all readers, build tools and required decoding material. `catalog:check` and `build` read these inputs directly. After editing catalogue inputs, run `npm run catalog:build` and commit the updated snapshots. Ordinary rebuilds reuse unchanged snapshot bytes.

Padrino Store is a separate built-in Community catalogue. It ships as `catalog/padrino.json.gz`, with its source commit recorded inside the snapshot. To update it, check out the desired [Padrino revision](https://github.com/B4ckd0orl/Padrino-Store) and run `python3 tools/import_padrino.py --checkout /path/to/Padrino-Store`. The importer uses the game and alternate edition metadata with the associated single-file downloads. It excludes ZSTD, unavailable and multipart options, and provider pages Orbit cannot open. A title needs at least one supported option to appear. Publish the regenerated snapshot with a new Orbit build. The native Orbit feed retains its own update schedule.

The Padrino importer prints skip totals by reason. Add `--show-skips` to write one JSON record per skipped row to stderr with the title ID, release ID, provider, URLs and reason code.

The shared service supplies catalogue data to both interfaces. Existing catalogue caches are migrated automatically. Queues with pasted-link jobs use schema 8. Earlier queue files still load, and pairing state keeps its existing format. TorBox job compatibility is described in [TorBox setup](https://github.com/saawant12/orbit-store-ps5/blob/main/guides/downloads.md#optional-download-through-torbox). Published Orbit 1.1.0 uses the v5 catalogue feed for PS4 and PS5 games. Version 1.5.0 uses v8 with separate game and add-on collections. v7 remains a compatible game-only feed for earlier clients. The v6 feed is pinned to its last catalogue below the older 4,096-option limit. 1fichier remains deferred. The v6 compatibility feed retains ZIP, 7z, RAR, AkiraBox and DataNodes options; v5 excludes archives and those providers. The v4 and v3 feeds also retain PS5-only compatibility. Catalogue updates cannot add these capabilities to an older app.

Package installation and game registration use the ShadowMountPlus v1 local API on the console. Use a version that advertises package installation for PKG/FPKG and browser streaming for Orbit Zero’s **Stream & install**.

## Payload and browser interface

```sh
python3 -m venv .venv
.venv/bin/python tools/bootstrap.py
npm ci
npm run build
docker build -f tools/Dockerfile.sdk -t orbit-sdk:0.43 .
docker build -t orbit-build:0.1 .
docker run --rm --cpus 2 --memory 6g -v "$PWD:/work" orbit-build:0.1 payload
```

The encrypted-archive implementation compiles 7-Zip 26.03 from pinned source through `tools/build-7zip.sh` and `tools/7zip-orbit.mak`. Its C++ bridge only reads archives; file validation and writes stay in Orbit. To build without network access, copy the source bundle’s `7z2603-src.7z` into `.deps/` first. It is built without 7-Zip's RAR code. RAR archives use RARLAB's UnRAR 7.23, compiled from pinned source through `tools/build-unrar.sh` with the changes in `tools/patch-unrar.py`; copy the source bundle's `unrarsrc-7.2.7.tar.gz` into `.deps/` to build it without network access. Distribution clearance is pending for the RAR component; see `THIRD-PARTY-NOTICES.md`.

The result is `build/orbit_store.elf`. It carries `build/orbit_runtime.elf`, the runtime saved on the console. Run `npm run build` before rebuilding the payload whenever the browser interface or catalogue changes.

The source bundle includes the exact cJSON and CA files used for the published build. To reuse them, copy its `build-inputs/cjson/` and `build-inputs/cacert.pem` into `.deps/` before bootstrap.

To rebuild with modified libmicrohttpd, curl, OpenSSL, libarchive, liblzma, bzip2 or zlib, unpack the included source archive, adapt `tools/build-deps.sh` to use it, rebuild `orbit-build:0.1`, and rebuild the payload. Rebuild the image after pulling these archive-extraction changes too; host tests link the same libarchive through `tools/build-host-archive.sh`. The same source permits relinking the statically linked libmicrohttpd.

## Interface languages

The browser interface uses the catalogues in `i18n/`. English text is the key, so a string without a translation shows in English. After adding or changing interface text (`t()`, `tn()` or `rich()` in `src/`, or a fixed message in `backend/`), refresh the list and see what each language is missing:

```sh
node tools/i18n.mjs extract   # rewrites i18n/en.json
node tools/i18n.mjs check     # lists missing, unused and mismatched translations
```

`npm test` fails until every language has every string with the same `{placeholders}`. The browser follows the browser's language unless one is chosen in App settings.

For a new language, register its code and native name in `src/i18n.ts` and include it in `tools/i18n.mjs`.

The Hungarian catalogue was contributed by [isty1974 in issue #13](https://github.com/saawant12/orbit-store-ps5/issues/13#issuecomment-6065940183), with newer interface messages filled in for this version.

## Checks and release packaging

```sh
npm test
npm run test:catalog
docker run --rm --cpus 2 --memory 6g --network none -v "$PWD:/work" orbit-build:0.1 test-links test-launcher
```

Run tests in a checkout; root development tests are omitted from the release source archive. None of these commands contact a console.

From a clean release commit, rebuild the payload, then `npm run release:package`. The command checks release evidence, version agreement, artifact freshness and dependency source hashes. It stages the payload, its checksum, exact source and licence bundle, release notes and the official payload feed under `build/release-1.9.7/`. It never uploads or installs anything. Publish feed changes only after their binary URLs are live and their hashes verified.

For local preparation before the console smoke check, use `npm run release:package -- --review`. This creates `build/review-1.9.7/` with the remaining release gates recorded; it does not publish or waive them.
