# Orbit Store source and licence bundle

The Community Fork source bundle contains the source for `orbit_store.elf`, the dependency sources and the build inputs. `SOURCES.json` records the application commit and the upstream archive hashes. Verify the bundle inputs with `shasum -a 256 -c SOURCE-SHA256SUMS`.

Unpack the application source and follow [BUILDING.md](BUILDING.md). Seed `.deps/cjson/` and `.deps/cacert.pem` from the supplied `build-inputs/` directory before bootstrapping. The PS5 payload SDK source and the pinned archive, HTTP, TLS, SQLite, 7-Zip and UnRAR sources are included. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and `licenses/` for their licences.

The browser and payload share the bundled catalogue snapshots in `catalog/`. The application source includes the catalogue tools and decoding material. Docker setup and installing the locked browser dependencies may still need network access.
