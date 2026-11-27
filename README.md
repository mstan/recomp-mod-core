# recomp-mod-core

Current licensing and the grant for existing owner contributions are documented in [LICENSE-SCOPE.md](LICENSE-SCOPE.md).

Private extraction candidate for reusable native mod-package services. No system
consumer is integrated yet. This library builds without an emulator, launcher,
SDL, retail images, generated guest code or network service.

The implementation extracts working Virtual Boy/SNES package machinery: versioned
catalogs, strict manifest parsing, validated feature options, required resources,
stored/DEFLATE ZIP imports, containment and CRC checks, persistent selections,
exclusive plugin claims and content-derived committed identity. SNES feature
requirements use `requires = "package/feature, other/feature"`; commit rejects
unsatisfied requirements. Selection is explicit rather than automatically enabling
dependencies.

Opaque contexts own catalogs, selections and trusted-plugin registrations.
Different contexts can run on different threads; serialize calls on one context.
Only statically registered callbacks can be selected by a package. Host-side
activation callbacks can inspect the selected options/assets/resources.

`rmod_apply` is the engine installation seam. It accepts patches to named engine
memory spaces, expected bytes, replacement bytes and trusted hook identifiers.
It checks overlapping claims and every expected byte before mutation, copies
operation inputs, binds them and target identity into a fingerprint, and restores
bytes on failure. The engine transaction owns exclusion and hook/private-state
rollback. A failed byte restoration returns `-1`; the caller must halt guest
execution. `ram` and `cartridge` are both valid writable engine spaces. The core
never patches disk images. `rmod_commit` hashes the original image through a
bounded streaming buffer, without opening it for writing.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --timeout 0
```

Add this source package with `add_subdirectory` and link
`recomp-mod-core::core`. The C ABI is in `include/recomp_mod.h`; C++17 is required
to build the implementation. A CMake install exports the same target.

## Package profile and integration limits

This first profile uses the inherited format-version-1 manifest fields and the
`.rmod` ZIP extension. Archives are limited to 256 MiB compressed/expanded and
4,096 entries. Unknown fields fail explicitly. Existing console-specific
manifest extensions require consumer profile adapters; this is not a promise
that every existing console package can be imported unchanged.

The core does not define console memory maps, instruction hooks, asset conversion,
native save serialization or a launcher provider. The consuming system supplies
those and its original-image picker/extraction flow. Resource paths come from
that flow or programmatic host configuration; this library does not open dialogs.
The existing recomp-ui Mods page remains the user-facing owner of controls,
including default-enabled diagnostics/recording and saved player choices.

The component suite covers real manifests and archives, dependencies, options,
resources, target identity, exclusive claims, instance separation, persisted
selections, mutable loaded RAM/cartridge writes and transactional failures. It
does not establish gameplay acceptance, a performance improvement, or speaker
audio quality.

## License

Package runtime and extraction changes retain PolyForm Noncommercial 1.0.0
(`LICENSE`). The CRC helper retains its donor MIT notice in
`LICENSES/vbrecomp-MIT.txt`. SHA-256 is a public-domain reference implementation
adapted to incremental input. See `PROVENANCE.json` for exact donor identities.
