#ifndef TIG_TIG_H
#define TIG_TIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(TIG_SHARED)
#  if defined(TIG_BUILDING_LIBRARY)
#    define TIG_API __declspec(dllexport)
#  else
#    define TIG_API __declspec(dllimport)
#  endif
#else
#  define TIG_API
#endif

typedef enum tig_status {
  TIG_OK = 0,
  TIG_ERROR_INVALID_ARGUMENT = -1,
  TIG_ERROR_GIT = -2,
  TIG_ERROR_BUFFER_TOO_SMALL = -3,
  TIG_ERROR_NOT_FOUND = -4,
  TIG_ERROR_REJECTED = -5
} tig_status;

/* milliseconds since the UNIX epoch */
typedef int64_t (*tig_clock_source)(void* context);

typedef struct tig_options {
  const char* repository_storage_base_path;
  /* optional: replaces the system clock, e.g. in tests */
  tig_clock_source clock;
  void* clock_context;
} tig_options;

typedef struct tig_repository tig_repository;

/* Writes an armored SSH signature ("-----BEGIN SSH SIGNATURE-----" ...) over content, made with the
   device key in the "git" namespace, e.g. by `ssh-keygen -Y sign -n git`. signature is NUL-terminated. */
typedef tig_status (*tig_sign_callback)(void* context, const char* content, size_t content_length,
                                        char* signature, size_t signature_size);

typedef struct tig_device {
  /* the key's line in .tig/allowed_signers, e.g. SHA256:<fingerprint> namespaces="git" ssh-ed25519 AAAA... */
  const char* allowed_signer;
  tig_sign_callback sign;
  void* sign_context;
} tig_device;

typedef enum tig_value_type {
  TIG_VALUE_INTEGER,
  TIG_VALUE_DECIMAL,
  TIG_VALUE_TEXT
} tig_value_type;

typedef enum tig_chunk_period {
  TIG_CHUNK_HOUR,
  TIG_CHUNK_DAY,
  TIG_CHUNK_MONTH,
  TIG_CHUNK_YEAR
} tig_chunk_period;

typedef struct tig_column {
  const char* name;
  tig_value_type type;
} tig_column;

typedef struct tig_stream_definition {
  const char* description; /* optional */
  const char* unit;        /* optional */
  const tig_column* columns;
  size_t column_count;
  tig_chunk_period chunk;
  int64_t max_lag_ms;
} tig_stream_definition;

TIG_API const char* tig_version(void);
TIG_API void        tig_git_engine_version(int* major, int* minor, int* revision);
TIG_API const char* tig_get_last_error(void);

TIG_API tig_status  tig_init(const tig_options* options);
TIG_API void        tig_shutdown(void);

TIG_API tig_status  tig_repository_clone(const char* url, const char** path);

/* with a device, the first commit adds .tig/allowed_signers and every commit is signed */
TIG_API tig_status  tig_repository_create(const char* path, const tig_device* device, tig_repository** out);
TIG_API tig_status  tig_repository_open(const char* path, tig_repository** out);
TIG_API void        tig_repository_close(tig_repository* repository);

/* Signs every later commit with the device's key. The key must be listed in .tig/allowed_signers;
   a repository without that file (unsigned legacy history) gains it in a signed commit. */
TIG_API tig_status  tig_repository_set_device(tig_repository* repository, const tig_device* device);

/* commits pending changes to /telemetry, /events, /state and /evidence; succeeds without committing when there are
   none. Rejected in a repository with .tig/allowed_signers until a device is set. */
TIG_API tig_status  tig_repository_commit(tig_repository* repository, const char* message);

/* never goes backwards for a given repository */
TIG_API tig_status  tig_clock_now(tig_repository* repository, int64_t* unix_ms);

/* succeeds if the stream is already defined identically */
TIG_API tig_status  tig_telemetry_stream_define(tig_repository* repository, const char* stream,
                                                const tig_stream_definition* definition);
/* values are in column order; NULL or "" means no value */
TIG_API tig_status  tig_telemetry_append(tig_repository* repository, const char* stream, int64_t unix_ms,
                                         const char* const* values, size_t value_count);

/* --- YAML values, for events and state --- */

typedef enum tig_node_type {
  TIG_NODE_SCALAR,
  TIG_NODE_LIST,
  TIG_NODE_MAP
} tig_node_type;

typedef struct tig_node tig_node;

/* Each returns NULL when out of memory. A string is always read back as a string (it is quoted where needed);
   tig_node_number takes a YAML integer or decimal, e.g. "57.5", and returns NULL for anything else. */
TIG_API tig_node*      tig_node_string(const char* text);
TIG_API tig_node*      tig_node_integer(int64_t value);
TIG_API tig_node*      tig_node_number(const char* text);
TIG_API tig_node*      tig_node_boolean(int value);
TIG_API tig_node*      tig_node_list(void);
TIG_API tig_node*      tig_node_map(void);
TIG_API tig_node*      tig_node_copy(const tig_node* node);
TIG_API void           tig_node_free(tig_node* node);

/* These take ownership of value, even when they fail, so a constructor can be passed straight in.
   tig_node_set replaces the value of an existing key, keeping its place; maps keep their keys in order. */
TIG_API tig_status     tig_node_append(tig_node* list, tig_node* value);
TIG_API tig_status     tig_node_set(tig_node* map, const char* key, tig_node* value);
/* returns 1 if the key was there */
TIG_API int            tig_node_unset(tig_node* map, const char* key);

TIG_API tig_node_type  tig_node_type_of(const tig_node* node);
/* a scalar's text, or NULL for a list or map */
TIG_API const char*    tig_node_text(const tig_node* node);
/* 0 for a scalar that YAML reads as a number, boolean or null */
TIG_API int            tig_node_is_string(const tig_node* node);
/* items in a list, or keys in a map */
TIG_API size_t         tig_node_count(const tig_node* node);
/* a list's item or a map's value; like strchr, these return a node that is as writable as the one given */
TIG_API tig_node*      tig_node_at(const tig_node* node, size_t index);
TIG_API const char*    tig_node_key_at(const tig_node* node, size_t index);
TIG_API tig_node*      tig_node_get(const tig_node* map, const char* key);
/* maps are equal whatever the order of their keys; a string never equals a number */
TIG_API int            tig_node_equal(const tig_node* left, const tig_node* right);

/* one YAML document; anchors and aliases, and tags other than !!str, are rejected */
TIG_API tig_status     tig_node_parse(const char* yaml, size_t length, tig_node** out);
/* NUL-terminated YAML, in the style of the TiG docs; free it with tig_free */
TIG_API tig_status     tig_node_render(const tig_node* node, char** out);
TIG_API void           tig_free(void* memory);

/* --- state and events (EVENTS.md) ---

   These work on the working tree, like telemetry, and tig_repository_commit commits them. Every write is
   rejected (TIG_ERROR_REJECTED) unless it passes the checks in EVENTS.md, "Writing", and replay of every event
   still succeeds; commits check the same again. */

#define TIG_EVENT_ID_MAX 16
#define TIG_TIME_LATEST INT64_MAX

/* writes state/initial.yaml: a map with kind, and optionally vocabulary and items */
TIG_API tig_status  tig_state_write_initial(tig_repository* repository, const tig_node* initial);
/* state/initial.yaml with the events whose time starts no later than unix_ms applied (TIG_TIME_LATEST for
   all), shaped like initial.yaml and always with items */
TIG_API tig_status  tig_state_replay(tig_repository* repository, int64_t unix_ms, tig_node** state);

/* writes a new event at events/YYYY/<time>.<id>.yaml, choosing an ID that has never been used;
   id needs room for TIG_EVENT_ID_MAX + 1 bytes */
TIG_API tig_status  tig_event_add(tig_repository* repository, const tig_node* event, char* id, size_t id_size);
/* replaces an event, which keeps its ID and moves if its time changes; TIG_ERROR_NOT_FOUND if there is none */
TIG_API tig_status  tig_event_correct(tig_repository* repository, const char* id, const tig_node* event);
TIG_API tig_status  tig_event_delete(tig_repository* repository, const char* id);
TIG_API tig_status  tig_event_read(tig_repository* repository, const char* id, tig_node** event);

/* return anything but TIG_OK to stop; tig_events_each then returns it */
typedef tig_status (*tig_event_visitor)(void* context, const char* id, const tig_node* event);
/* every event, in replay order */
TIG_API tig_status  tig_events_each(tig_repository* repository, tig_event_visitor visit, void* context);

/* --- evidence (EVIDENCE.md) ---

   Evidence is the files that support events: photos, receipts, certificates. Add one with tig_evidence_add, list its
   path in an event's `evidence`, and tig_repository_commit commits both, in one commit or two. Evidence is never
   changed or removed once committed, so a later commit that does either is rejected (TIG_ERROR_REJECTED). */

#define TIG_EVIDENCE_NAME_MAX 100
/* room for a path from tig_evidence_add, "/evidence/" and a name */
#define TIG_EVIDENCE_PATH_MAX (10 + TIG_EVIDENCE_NAME_MAX + 1)

/* Copies the file at source into /evidence as name and stages it, so that events can list it straight away. name is
   1 to TIG_EVIDENCE_NAME_MAX letters, digits, '.', '_' and '-', starting with a letter or digit; NULL uses the source's
   own file name. path receives what an event lists, e.g. /evidence/2024-03-11-invoice.pdf, and needs room for
   TIG_EVIDENCE_PATH_MAX bytes.
   TIG_ERROR_REJECTED if the name isn't allowed, a file of that name is already there (evidence is never replaced) or
   the source is empty; TIG_ERROR_NOT_FOUND if the source can't be found. The file is not committed until
   tig_repository_commit. */
TIG_API tig_status  tig_evidence_add(tig_repository* repository, const char* source, const char* name,
                                     char* path, size_t path_size);

#ifdef __cplusplus
}
#endif

#endif
