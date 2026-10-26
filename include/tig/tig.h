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

TIG_API tig_status  tig_repository_create(const char* path, tig_repository** out);
TIG_API tig_status  tig_repository_open(const char* path, tig_repository** out);
TIG_API void        tig_repository_close(tig_repository* repository);

/* commits pending changes; succeeds without committing when there are none */
TIG_API tig_status  tig_repository_commit(tig_repository* repository, const char* message);

/* never goes backwards for a given repository */
TIG_API tig_status  tig_clock_now(tig_repository* repository, int64_t* unix_ms);

/* succeeds if the stream is already defined identically */
TIG_API tig_status  tig_telemetry_stream_define(tig_repository* repository, const char* stream,
                                                const tig_stream_definition* definition);
/* values are in column order; NULL or "" means no value */
TIG_API tig_status  tig_telemetry_append(tig_repository* repository, const char* stream, int64_t unix_ms,
                                         const char* const* values, size_t value_count);

#ifdef __cplusplus
}
#endif

#endif
