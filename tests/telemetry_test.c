#include "tig/tig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPO "tig-telemetry-repo"

/* 2026-10-23T10:15:00Z */
#define T0 INT64_C(1792750500000)
#define MINUTE INT64_C(60000)

#define CHECK(condition)                                                                          \
  do {                                                                                            \
    if (!(condition)) {                                                                           \
      fprintf(stderr, "%s:%d: CHECK(%s) failed; last error: %s\n", __FILE__, __LINE__, #condition, \
              tig_get_last_error());                                                              \
      ++failures;                                                                                 \
    }                                                                                             \
  } while (0)

static int failures;
static int64_t fake_now;

static int64_t fake_clock(void* context) {
  (void)context;
  return fake_now;
}

/* returns a malloc'd copy, or NULL if missing */
static char* read_file(const char* path) {
  FILE* file = fopen(path, "rb");
  char* contents;
  long size;

  if (file == 0) {
    return 0;
  }
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  fseek(file, 0, SEEK_SET);
  contents = (char*)malloc((size_t)size + 1);
  if (fread(contents, 1, (size_t)size, file) != (size_t)size) {
    size = 0;
  }
  contents[size] = '\0';
  fclose(file);
  return contents;
}

static void write_file(const char* path, const char* contents) {
  FILE* file = fopen(path, "wb");

  if (file != 0) {
    fputs(contents, file);
    fclose(file);
  }
}

static int file_equals(const char* path, const char* expected) {
  char* contents = read_file(path);
  int equal = contents != 0 && strcmp(contents, expected) == 0;

  if (!equal) {
    fprintf(stderr, "%s contains:\n%s\nexpected:\n%s\n", path, contents != 0 ? contents : "(missing)", expected);
  }
  free(contents);
  return equal;
}

static tig_status append1(tig_repository* repository, const char* stream, int64_t unix_ms, const char* value) {
  return tig_telemetry_append(repository, stream, unix_ms, &value, 1);
}

int main(void) {
  static const tig_column temperature_columns[] = {{"value", TIG_VALUE_DECIMAL}};
  static const tig_column release_columns[] = {{"name", TIG_VALUE_TEXT}, {"build", TIG_VALUE_INTEGER}};
  tig_stream_definition temperature = {"CPU package temperature", "celsius", temperature_columns, 1,
                                       TIG_CHUNK_DAY, 5 * MINUTE};
  tig_stream_definition definition;
  tig_options options = {0};
  tig_repository* repository;
  const char* values[2];
  char* original;
  int64_t now;

  options.clock = fake_clock;
  fake_now = T0;

  if (tig_init(&options) != TIG_OK || tig_repository_create(REPO, 0, &repository) != TIG_OK) {
    fprintf(stderr, "setup failed: %s\n", tig_get_last_error());
    return 1;
  }

  /* defining streams */
  CHECK(tig_telemetry_stream_define(repository, "cpu.temp", &temperature) == TIG_OK);
  CHECK(tig_telemetry_stream_define(repository, "cpu.temp", &temperature) == TIG_OK);
  CHECK(file_equals(REPO "/telemetry/cpu.temp/stream.yaml",
                    "description: CPU package temperature\n"
                    "unit: celsius\n"
                    "columns:\n"
                    "  - name: value\n"
                    "    type: decimal\n"
                    "chunk: day\n"
                    "max_lag: 5m\n"));

  definition = temperature;
  definition.max_lag_ms = 10 * MINUTE;
  CHECK(tig_telemetry_stream_define(repository, "cpu.temp", &definition) == TIG_ERROR_REJECTED);
  CHECK(tig_telemetry_stream_define(repository, "CPU", &temperature) == TIG_ERROR_INVALID_ARGUMENT);
  CHECK(tig_telemetry_stream_define(repository, ".hidden", &temperature) == TIG_ERROR_INVALID_ARGUMENT);
  CHECK(tig_telemetry_stream_define(repository, "", &temperature) == TIG_ERROR_INVALID_ARGUMENT);
  definition = temperature;
  definition.max_lag_ms = 0;
  CHECK(tig_telemetry_stream_define(repository, "cpu.other", &definition) == TIG_ERROR_INVALID_ARGUMENT);
  {
    static const tig_column bad_columns[] = {{"time", TIG_VALUE_DECIMAL}};
    definition = temperature;
    definition.columns = bad_columns;
    CHECK(tig_telemetry_stream_define(repository, "cpu.other", &definition) == TIG_ERROR_INVALID_ARGUMENT);
  }

  /* appending */
  CHECK(append1(repository, "odometer", T0, "1000") == TIG_ERROR_NOT_FOUND);
  CHECK(append1(repository, "cpu.temp", T0, "47.5") == TIG_OK);
  CHECK(append1(repository, "cpu.temp", T0, "48") == TIG_ERROR_REJECTED);
  CHECK(append1(repository, "cpu.temp", T0 + MINUTE, "warm") == TIG_ERROR_INVALID_ARGUMENT);
  CHECK(append1(repository, "cpu.temp", T0 + MINUTE, "48.") == TIG_ERROR_INVALID_ARGUMENT);
  CHECK(tig_telemetry_append(repository, "cpu.temp", T0 + MINUTE, values, 2) == TIG_ERROR_INVALID_ARGUMENT);
  fake_now = T0 + MINUTE + 250;
  CHECK(append1(repository, "cpu.temp", T0 + MINUTE + 250, "48.0") == TIG_OK);

  /* freshness against the TiG clock */
  fake_now = T0 + 10 * MINUTE;
  CHECK(append1(repository, "cpu.temp", T0 + 2 * MINUTE, "49.0") == TIG_ERROR_REJECTED);
  CHECK(append1(repository, "cpu.temp", fake_now + MINUTE, "49.0") == TIG_ERROR_REJECTED);
  CHECK(append1(repository, "cpu.temp", fake_now, "") == TIG_OK);

  CHECK(file_equals(REPO "/telemetry/cpu.temp/2026/2026-10-23.csv",
                    "time,value\n"
                    "2026-10-23T10:15:00Z,47.5\n"
                    "2026-10-23T10:16:00.250Z,48.0\n"
                    "2026-10-23T10:25:00Z,\n"));

  /* text needs quoting; other chunk periods */
  definition.description = 0;
  definition.unit = 0;
  definition.columns = release_columns;
  definition.column_count = 2;
  definition.chunk = TIG_CHUNK_YEAR;
  definition.max_lag_ms = 30 * 1000;
  CHECK(tig_telemetry_stream_define(repository, "os.release", &definition) == TIG_OK);
  values[0] = "Ubuntu \"Noble\", 24.04";
  values[1] = "2404";
  CHECK(tig_telemetry_append(repository, "os.release", fake_now, values, 2) == TIG_OK);
  values[0] = "two\nlines";
  CHECK(tig_telemetry_append(repository, "os.release", fake_now + 1000, values, 2) == TIG_ERROR_INVALID_ARGUMENT);
  CHECK(file_equals(REPO "/telemetry/os.release/2026.csv",
                    "time,name,build\n"
                    "2026-10-23T10:25:00Z,\"Ubuntu \"\"Noble\"\", 24.04\",2404\n"));

  definition = temperature;
  definition.chunk = TIG_CHUNK_HOUR;
  CHECK(tig_telemetry_stream_define(repository, "cpu.load", &definition) == TIG_OK);
  CHECK(append1(repository, "cpu.load", fake_now, "0.5") == TIG_OK);
  CHECK(file_equals(REPO "/telemetry/cpu.load/2026/10/2026-10-23T10.csv", "time,value\n2026-10-23T10:25:00Z,0.5\n"));

  CHECK(tig_repository_commit(repository, "Record telemetry") == TIG_OK);
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  /* a new chunk each day; order holds across chunks */
  fake_now = INT64_C(1792800030000); /* 2026-10-24T00:00:30Z */
  CHECK(append1(repository, "cpu.temp", fake_now, "45.0") == TIG_OK);
  CHECK(append1(repository, "cpu.temp", fake_now - MINUTE, "45.0") == TIG_ERROR_REJECTED);
  CHECK(file_equals(REPO "/telemetry/cpu.temp/2026/2026-10-24.csv", "time,value\n2026-10-24T00:00:30Z,45.0\n"));

  /* reopening reads everything back from the files */
  tig_repository_close(repository);
  CHECK(tig_repository_open(REPO, &repository) == TIG_OK);
  CHECK(append1(repository, "cpu.temp", fake_now, "45.5") == TIG_ERROR_REJECTED);
  CHECK(append1(repository, "cpu.temp", fake_now + 1000, "45.5") == TIG_OK);
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  /* appending to a committed chunk */
  CHECK(append1(repository, "cpu.temp", fake_now + 2000, "46.0") == TIG_OK);
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  /* the append-only rule */
  original = read_file(REPO "/telemetry/cpu.temp/2026/2026-10-23.csv");
  write_file(REPO "/telemetry/cpu.temp/2026/2026-10-23.csv",
             "time,value\n2026-10-23T10:15:00Z,40.5\n2026-10-23T10:16:00.250Z,48.0\n2026-10-23T10:25:00Z,\n");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  remove(REPO "/telemetry/cpu.temp/2026/2026-10-23.csv");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  write_file(REPO "/telemetry/cpu.temp/2026/2026-10-23.csv", original);
  free(original);
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  write_file(REPO "/telemetry/cpu.temp/notes.txt", "hello\n");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  remove(REPO "/telemetry/cpu.temp/notes.txt");

  original = read_file(REPO "/telemetry/cpu.temp/stream.yaml");
  write_file(REPO "/telemetry/cpu.temp/stream.yaml", "columns:\n  - name: value\n    type: text\nchunk: day\nmax_lag: 5m\n");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  write_file(REPO "/telemetry/cpu.temp/stream.yaml", original);
  free(original);

  write_file(REPO "/state/device.yaml", "cpu: arm64\n");
  CHECK(tig_repository_commit(repository, 0) == TIG_OK); /* untracked files outside /telemetry are left alone */
  remove(REPO "/state/device.yaml");

  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  /* the clock never goes backwards */
  fake_now = T0;
  CHECK(tig_clock_now(repository, &now) == TIG_OK && now == INT64_C(1792800030000));

  tig_repository_close(repository);
  tig_shutdown();

  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
  }
  return failures == 0 ? 0 : 1;
}
