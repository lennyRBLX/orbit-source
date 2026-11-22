# First-run home-screen icon

`orbit_store.elf` includes this installer. On its first successful run it adds the `ORBT00001` icon, which opens the running server. It does not start the backend; payload managers do that from the saved runtime. See the [startup contract](../docs/installation.md).

## Files and recovery

- App manifest and icon: `/user/app/ORBT00001/sce_sys/param.json` and `icon0.png`. The manifest uses `deeplinkUri`, like Payload Manager's own icon; nothing is written under `/system_ex`.
- Ownership and successful-registration records: `/data/orbit-store/launcher-owned-v1` and `launcher-ready-v1`. `launcher-files-v1` holds a fingerprint of the manifest and icon that were last registered.
- Runtime embedding: `runtime_image.c` places `build/orbit_runtime.elf` inside `orbit_store.elf`; the backend saves it as `/data/orbit-store/orbit_store.elf`.

First-time setup uses the platform application-registration API. On later runs Orbit checks for its system-created registration metadata and restores a missing Media entry. The installer never changes a pre-existing unknown title, and never writes through a symlink or over a directory. Inside a title Orbit owns, files left by an older build or by a failed attempt are replaced with complete copies (temporary file, then rename), so a registered icon is never left without its manifest or icon. If the ownership records were lost (for example after `/data/orbit-store` was reset), an existing `ORBT00001` is adopted only when it holds nothing but `sce_sys/param.json` and `icon0.png` and its manifest is Orbit's local-deeplink manifest; anything else is reported as `check-existing-title` with `EEXIST`.

A registration answer of `0x80990002` means the title is already registered. Orbit accepts it unless the registration metadata is known to be missing. If the metadata is missing, Orbit reports `check-registered-title` and retries on the next launch.

When available, the title-directory API registers `ORBT00001` alone. If the firmware
does not expose that API, Orbit stages its own shortcut and asks the system to refresh
application registrations with AppInstUtil. That operation can pick up other
applications already staged on the console. Orbit writes files only for the title
it owns. If neither API resolves, it reports `title-registration-unavailable`.
After a refresh, Orbit waits for registration metadata and checks the AppInst title
record if the metadata is still missing. If neither confirms registration, it
reports `check-refreshed-title` and retries on the next launch.

An already verified launcher with registration metadata skips registration.
Native installer resources are released on both successful and failed attempts.

A payload started while Orbit is already running can restore the shortcut. A launcher lock serializes installation across both processes.

The manifest URL is generated from `config/network.json`, alongside the server's port constant. When the manifest or icon of this build differs from the installed one (`launcher-files-v1`), Orbit replaces its owned files and registers the icon again; the record is written last, so a failed update is retried on the next start.

`orbit_core.elf` is available for server-only diagnostics and does not include this installer.

## Validation

`GET /api/v1/system` reports `launcherStatus`, `launcherError`, and the selected
`launcherRegistrationMethod` (`title-directory`, `system-refresh`, `unavailable`, or `not-needed`). On failure,
`launcherError` names the installer step and preserves the filesystem errno or native
registration error code. It contains no pairing code or saved-state contents.

Local tests cover first installation, concurrent launch, repair with intact files, and
preservation of unrelated artwork. Platform mocks cover title-directory registration,
the system refresh fallback, missing entry points, native errors and resource cleanup.
