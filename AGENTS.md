# AGENTS.md

`tig-lib` is the reference implementation of the TiG (Timeline in Git) standard: a portable C99 library (`libtig`) on libgit2. libgit2 is embedded and hidden, so consumers link only `libtig`.

## Spec

The specification lives in `tig-docs`, checked out here as the `docs/` submodule. It is the source of truth: when the implementation and the spec disagree, raise it rather than silently picking one.

- `docs/WHITEPAPER.md`: rationale, entities and repo layout. Read this first.
- `docs/EVENTS.md`: state and events, replay, and the library API for them (`src/events.c`).
- `docs/TELEMETRY.md`: telemetry readings (`src/telemetry.c`).
- `docs/COMMITS.md`: commit signing.
- `docs/vocabularies/`: shared vocabularies such as `tig-device/1`.
- `docs/ROADMAP.md`: where the prototypes are heading.

Keep the `docs/` submodule pointing at a `tig-docs` commit that contains the spec the code implements, and update it when the spec changes.

## Layout

- `include/tig/tig.h`: the public API. Keep libgit2 types out of it.
- `src/`: implementation (`tig.c`, `node.c`, `events.c`, `telemetry.c`, `internal.h`).
- `tests/`, `examples/`: tests and example programs.

Build and test:

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## Versioning

The library is semantically versioned (`MAJOR.MINOR.PATCH`). The version is set in one place, `project(tig VERSION ...)` in `CMakeLists.txt`, and surfaces through `tig_version()` and the CMake package config.

Update it whenever fixes or features land, in the same change:

- PATCH for bug fixes with no API change.
- MINOR for new, backwards-compatible features or API additions.
- MAJOR for incompatible API or format changes. While the major version is 0, breaking changes may go in a MINOR bump, but call them out in the PR.

Update the expected version in `tests/smoke_test.c` too. `tig-agent` pins this repo as a submodule, so after a release, bump its `external/tig-lib` pin to a commit on `main`.
