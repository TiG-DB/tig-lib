# tig-lib

Portable C library scaffold built on [libgit2](https://libgit2.org/), without
exposing libgit2 to libtig users.

## Requirements

- CMake 3.16 or newer
- A C99 compiler
- libgit2 1.5 or newer at build time
- libyaml 0.2.5 and Monocypher 4.0.3, which CMake downloads and compiles into libtig (set `TIG_LIBYAML_SOURCE_DIR`
  and `TIG_MONOCYPHER_SOURCE_DIR` to build offline)

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
- `TIG_LIBYAML_SOURCE_DIR`: optional path to a libyaml 0.2.5 source tree, instead of downloading it
- `TIG_MONOCYPHER_SOURCE_DIR`: optional path to a Monocypher 4.0.3 source tree, instead of downloading it
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
helpers, repository create/open/clone/close/commit wrappers, device keys and commit
signing, the TiG clock, telemetry, and state and events. libgit2, libyaml and Monocypher
stay internal implementation details.

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
changed in any way other than appending, or if anything outside `/telemetry`,
`/events`, `/state` and `/evidence` is staged. Set `options.clock` to replace the
system clock, e.g. in tests.

### State and events

State and events follow `EVENTS.md`. They are YAML, held as `tig_node` values:
build them with `tig_node_map`, `tig_node_set` and the scalar constructors, or
parse them with `tig_node_parse`. Like telemetry, they are written to the
working tree and committed by `tig_repository_commit`:

```c
tig_node* event = tig_node_map();
tig_node* actions = tig_node_list();
tig_node* action = tig_node_map();
char id[TIG_EVENT_ID_MAX + 1];

tig_node_set(event, "time", tig_node_string("2026-11-04T07:12:31Z"));
tig_node_set(action, "type", tig_node_string("booted"));
tig_node_append(actions, action);
tig_node_set(event, "actions", actions);

tig_event_add(repository, event, id, sizeof(id)); /* events/2026/2026-11-04.<id>.yaml */
tig_node_free(event);
```

`tig_event_add` chooses a random ID that no event has used, in the working tree
or in history. `tig_event_correct` replaces an event, keeping its ID and moving
its file if its time changed, and `tig_event_delete` removes one.
`tig_state_write_initial` writes `state/initial.yaml`. `tig_state_replay`
rebuilds state at a time, and `tig_events_each` visits events in replay order.

Each write returns `TIG_ERROR_REJECTED` unless it follows "Writing" in
`EVENTS.md`: the event is well formed, its time isn't in the future (instants
may be 5 seconds ahead of the TiG clock, and dates 14 hours, since a date is
where it happened), its evidence files are already in the index, and every
event still replays, including those dated after it. `tig_repository_commit`
makes the same checks on whatever is in `/events` and `/state`, so files edited
by hand are checked too, and it rejects an event that reuses an ID from history
unless the event is moving to a new path in the same commit.

### Evidence

Evidence is the files that support events: photos, receipts, certificates
(`EVIDENCE.md`). `tig_evidence_add` copies a file into `/evidence` and stages
it, so an event can list it straight away. List it by the path it returns:

```c
char path[TIG_EVIDENCE_PATH_MAX];
tig_node* paths = tig_node_list();

tig_evidence_add(repository, "scan0001.pdf", "2024-03-11-invoice.pdf", path, sizeof(path));
/* path is now "/evidence/2024-03-11-invoice.pdf" */
tig_node_append(paths, tig_node_string(path));
tig_node_set(event, "evidence", paths); /* then tig_event_add, as above */

tig_repository_commit(repository, 0); /* commits the event and its evidence together */
```

A name is up to `TIG_EVIDENCE_NAME_MAX` letters, digits, `.`, `_` and `-`,
starting with a letter or digit; pass `NULL` to use the source's own file name.
Evidence is never replaced, so adding a name that is already there is
`TIG_ERROR_REJECTED`, and so is an empty file. Once evidence is committed it is
fixed: `tig_repository_commit` returns `TIG_ERROR_REJECTED` for a later commit
that changes or removes it. It only takes evidence that came in through
`tig_evidence_add` (or was staged with Git), so a file dropped into `/evidence`
by hand is left alone, and an event can't list it.

Evidence may be committed before the event that lists it, or in the same
commit. `tig_repository_commit` with no message calls an evidence-only commit
"Add evidence".

### Signed commits

`COMMITS.md` requires every commit to be signed with the device's key. The
library can hold the key and sign itself:

```c
tig_key *key;
char text[TIG_KEY_EXPORT_MAX];

/* the first run: make a key, and store the text somewhere only you can read */
tig_key_generate(&key);
tig_key_export(key, text, sizeof(text));
/* later runs */
tig_key_parse(text, length, &key);

tig_repository_create(path, tig_key_device(key), &repository);
```

A key is Ed25519, and its text is an unencrypted OpenSSH private key file, so
`ssh-keygen` reads keys the library makes and the library reads keys
`ssh-keygen` makes without a passphrase. Encrypting that text at rest is the
caller's job. `tig_key_sign` signs in any namespace, e.g. `tig-notary` for a
notary request. Free the key with `tig_key_free` after closing the repositories
that use it. The library doesn't verify signatures yet.

A caller can instead sign elsewhere, e.g. with a hardware key or a program: pass
a `tig_device` with the key's `.tig/allowed_signers` line and a callback that
returns an SSH signature in the `git` namespace, e.g. from
`ssh-keygen -Y sign -n git`:

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

Git, and so `libtig`, refuses to open a repository that another user owns. A
caller that has checked who can write to its repositories, such as a service
whose data an administrator also reads, can set `allow_other_owners` in
`tig_options`.

The TiG clock does not yet correct for the offset measured against TiG Notary.

## Licence

Licensed under the [Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE).

The names "TiG" and "TiG Notary" and the TiG logos are not covered by that
licence and may not be used without permission, except to say accurately that your
work is compatible with TiG.
