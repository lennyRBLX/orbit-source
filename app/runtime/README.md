# Clean-room runtime shim

`libc.prx` is generated locally from the independently authored source in this
repository and is distributed under GPL-3.0-or-later. It contains no Sony
runtime implementation, proprietary SDK binary, or game file.

The release artifact has SHA-256:

```text
e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036
```

Generate it from the `app/` directory in the native build environment:

```sh
make libc
```

Then verify it from this directory with:

```sh
sha256sum -c libc.prx.sha256
```

The generated file is ignored by Git. The normal native application build also
creates it and includes it in the FFPKG; it is not a separate release download.

The complete source is in [`tooling/native`](../tooling/native). The
[rebuild script](../tools/rebuild-libc.sh) checks deterministic output against
the recorded hashes. See [the native build guide](../README.md#build) for the
required build environment.
