# tig-lib

Portable C library scaffold built on [libgit2](https://libgit2.org/), without
exposing libgit2 to libtig users.

## Requirements

- CMake 3.16 or newer
- A C99 compiler
- libgit2 1.5 or newer at build time

On macOS with Homebrew:

```sh
brew install cmake libgit2
```

On Debian or Ubuntu:

```sh
sudo apt-get install cmake libgit2-dev
```

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Build options:

- `TIG_BUILD_EXAMPLES`: build the example programs, defaults to `ON`
- `TIG_BUILD_TESTS`: build the test programs, defaults to `ON`
- `TIG_STANDALONE`: hide and embed libgit2 so consumers only link libtig, defaults to `ON`
- `TIG_REQUIRE_STATIC_LIBGIT2`: require a static libgit2 when building a standalone static libtig, defaults to `TIG_STANDALONE`
- `TIG_LIBGIT2_SOURCE_DIR`: optional path to a libgit2 source checkout to build into libtig
- `TIG_LIBGIT2_STATIC_LIBRARY`: optional path to a prebuilt static libgit2 archive
- `TIG_LIBGIT2_INCLUDE_DIR`: include path required with `TIG_LIBGIT2_STATIC_LIBRARY`
- `BUILD_SHARED_LIBS`: build shared libraries instead of static libraries, defaults to CMake's standard behavior

For a standalone static libtig, point CMake at either a libgit2 source checkout
or a prebuilt static libgit2 archive:

```sh
cmake -S . -B build -DTIG_LIBGIT2_SOURCE_DIR=/path/to/libgit2
cmake --build build
```

or:

```sh
cmake -S . -B build \
  -DTIG_LIBGIT2_STATIC_LIBRARY=/path/to/libgit2.a \
  -DTIG_LIBGIT2_INCLUDE_DIR=/path/to/libgit2/include
cmake --build build
```

When `TIG_STANDALONE` is enabled, installed CMake consumers get only `tig::tig`;
the installed package does not require or export a separate libgit2 target.

## Use

```c
#include <tig/tig.h>

int main(void) {
  tig_options options = {0};

  if (tig_init(&options) != TIG_OK) {
    return 1;
  }

  /* call tig APIs */

  tig_shutdown();
  return 0;
}
```

Set `options.repository_storage_base_path` to the directory that
`tig_repository_clone` should clone into. After a failed call,
`tig_get_last_error` describes what went wrong.

The public API intentionally stays small for now: version helpers, lifecycle
helpers, repository create/open/clone/close/commit wrappers, commit signing through a
device callback, the TiG clock and telemetry. libgit2 stays an internal implementation detail.

### Telemetry

Telemetry follows `TELEMETRY.md` in the TiG docs. Define a stream once, then
append readings as soon as they are sampled. Readings are written to the
working tree straight away, and committed in batches:

```c
static const tig_column columns[] = {{"value", TIG_VALUE_DECIMAL}};
tig_stream_definition cpu_temp = {"CPU package temperature", "celsius", columns, 1,
                                  TIG_CHUNK_DAY, 5 * 60 * 1000};
const char* value = "47.5";
int64_t now;

tig_telemetry_stream_define(repository, "cpu.temp", &cpu_temp);
tig_clock_now(repository, &now);
tig_telemetry_append(repository, "cpu.temp", now, &value, 1);
/* ... more readings ... */
tig_repository_commit(repository, "Record telemetry");
```

`tig_telemetry_append` returns `TIG_ERROR_REJECTED` for a reading older than the
stream's `max_lag`, in the future, or not later than the stream's last reading.
`tig_repository_commit` returns `TIG_ERROR_REJECTED` if `/telemetry` has
changed in any way other than appending, or if anything outside `/telemetry` is
staged. Set `options.clock` to replace the system clock, e.g. in tests.

### Signed commits

`COMMITS.md` requires every commit to be signed with the device's key. The
library does no cryptography itself: pass a `tig_device` with the key's
`.tig/allowed_signers` line and a callback that returns an SSH signature in the
`git` namespace, e.g. from `ssh-keygen -Y sign -n git`:

```c
tig_device device = {
  "SHA256:<fingerprint> namespaces=\"git\" ssh-ed25519 AAAA...", /* allowed_signer */
  sign_with_ssh_keygen,                                           /* sign */
  key_path                                                        /* sign_context */
};

tig_repository_create(path, &device, &repository);  /* first commit adds the key */
/* or, for an existing repository: */
tig_repository_open(path, &repository);
tig_repository_set_device(repository, &device);
```

`tig_repository_set_device` returns `TIG_ERROR_REJECTED` if the repository's
`.tig/allowed_signers` doesn't list the key. A repository with unsigned history
and no such file gains it in a signed "Add device key" commit, which becomes the
repository's key commit. Once a repository has the file, `tig_repository_commit`
is rejected until a device is set. Without a device, a repository that has never
had a key still commits unsigned, e.g. in tests.

The TiG clock does not yet correct for the offset measured against TiG Notary.
