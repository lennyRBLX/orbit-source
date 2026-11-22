# Native host tooling

All repository-owned format tooling is C or C++. Make and Bash orchestrate the
native build. No C# or .NET runtime is required.

The root build performs four native stages:

1. `prospero-clang18` compiles application sources and `native/app_crt.cpp`.
2. LLVM lld resolves objects, static archives, public SDK import stubs,
   constructors, TLS, and unwind information into an intermediate PIE.
3. `build/host/ps5-native-tool` converts that PIE to the PS5 ELF layout and wraps
   it in a deterministic development FSELF.
4. The requested folder or optional filesystem image is assembled.

`native/libc_builder.cpp` independently reproduces the generated clean-room
runtime from the two manifests under `native/runtime/`. Run `make libc` from
`app/` in the native build environment to verify both hashes.

`make deps` fetches the pinned public payload SDK and static zlib archive into
`.deps/native/`; neither is installed globally.

See [How it is built](../README.md#how-it-is-built) for the format boundary and
[the host-tool build script](../tools/build-host-tools.sh) for the low-level commands.
