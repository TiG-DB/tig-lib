#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

/* see tig-docs/TELEMETRY.md */

#define TIG_STREAM_NAME_MAX 128
#define TIG_COLUMN_NAME_MAX 64
#define TIG_COLUMNS_MAX 64
#define TIG_ROW_MAX 4096
#define TIG_DEFINITION_MAX 8192
/* how far ahead of the TiG clock a reading may be */
#define TIG_CLOCK_SKEW_MS 5000
/* 9999-12-31T23:59:59.999Z */
#define TIG_TIME_MAX_MS INT64_C(253402300799999)

/* what writing needs from a parsed stream.yaml */
typedef struct tig_definition {
  char column_names[TIG_COLUMNS_MAX][TIG_COLUMN_NAME_MAX + 1];
  int column_types[TIG_COLUMNS_MAX];
  size_t column_count;
  int chunk;
  int64_t max_lag_ms;
} tig_definition;

typedef struct tig_writer {
  char *out;
  size_t size;
  size_t length;
  int overflow;
} tig_writer;

typedef struct tig_civil {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
  int millisecond;
} tig_civil;

static const char *const tig_type_names[] = {"integer", "decimal", "text"};
static const char *const tig_chunk_names[] = {"hour", "day", "month", "year"};

/* d: digit, anything else is literal */
static const char *const tig_chunk_patterns[] = {"dddd/dd/dddd-dd-ddTdd.csv", "dddd/dddd-dd-dd.csv",
                                                 "dddd/dddd-dd.csv", "dddd.csv"};
static const int tig_chunk_depths[] = {2, 1, 1, 0};

static void tig_write(tig_writer *writer, const char *text, size_t length) {
  if (writer->overflow || writer->length + length >= writer->size) {
    writer->overflow = 1;
    return;
  }
  memcpy(writer->out + writer->length, text, length);
  writer->length += length;
  writer->out[writer->length] = '\0';
}

static void tig_write_string(tig_writer *writer, const char *text) {
  tig_write(writer, text, strlen(text));
}

static int tig_is_digit(char c) {
  return c >= '0' && c <= '9';
}

static int tig_is_lower(char c) {
  return c >= 'a' && c <= 'z';
}

static int tig_is_letter(char c) {
  return tig_is_lower(c) || (c >= 'A' && c <= 'Z');
}

static int tig_is_valid_stream_name(const char *name) {
  size_t length = strlen(name);
  size_t i;

  if (length == 0 || length > TIG_STREAM_NAME_MAX || !(tig_is_lower(name[0]) || tig_is_digit(name[0]))) {
    return 0;
  }
  for (i = 1; i < length; ++i) {
    if (!(tig_is_lower(name[i]) || tig_is_digit(name[i]) || name[i] == '.' || name[i] == '-')) {
      return 0;
    }
  }
  return 1;
}

static int tig_is_valid_column_name(const char *name, size_t length) {
  size_t i;

  if (length == 0 || length > TIG_COLUMN_NAME_MAX || !(tig_is_lower(name[0]) || name[0] == '_')) {
    return 0;
  }
  for (i = 1; i < length; ++i) {
    if (!(tig_is_lower(name[i]) || tig_is_digit(name[i]) || name[i] == '_')) {
      return 0;
    }
  }
  return !(length == 4 && memcmp(name, "time", 4) == 0);
}

/* single-line text: no control characters */
static int tig_is_valid_text(const char *text) {
  const unsigned char *c;

  for (c = (const unsigned char *)text; *c != '\0'; ++c) {
    if (*c < 0x20 || *c == 0x7f) {
      return 0;
    }
  }
  return 1;
}

static int tig_is_valid_value(const char *value, int type) {
  const char *c = value;

  if (type == TIG_VALUE_TEXT) {
    return tig_is_valid_text(value);
  }

  if (*c == '-') {
    ++c;
  }
  if (!tig_is_digit(*c)) {
    return 0;
  }
  while (tig_is_digit(*c)) {
    ++c;
  }
  if (type == TIG_VALUE_DECIMAL && *c == '.') {
    ++c;
    if (!tig_is_digit(*c)) {
      return 0;
    }
    while (tig_is_digit(*c)) {
      ++c;
    }
  }
  return *c == '\0';
}

static int tig_find_name(const char *const *names, size_t count, const char *value, size_t length) {
  size_t i;

  for (i = 0; i < count; ++i) {
    if (strlen(names[i]) == length && memcmp(names[i], value, length) == 0) {
      return (int)i;
    }
  }
  return -1;
}

/* --- time --- */

/* days since 1970-01-01, from http://howardhinnant.github.io/date_algorithms.html */
static int64_t tig_days_from_civil(int64_t year, int month, int day) {
  int64_t era;
  int64_t year_of_era;
  int64_t day_of_year;
  int64_t day_of_era;

  year -= month <= 2;
  era = (year >= 0 ? year : year - 399) / 400;
  year_of_era = year - era * 400;
  day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

static void tig_civil_from_ms(int64_t unix_ms, tig_civil *civil) {
  int64_t days = unix_ms / 86400000;
  int64_t ms_of_day = unix_ms % 86400000;
  int64_t era;
  int64_t day_of_era;
  int64_t year_of_era;
  int64_t day_of_year;
  int64_t month_index;

  /* unix_ms is never negative here */
  days += 719468;
  era = days / 146097;
  day_of_era = days - era * 146097;
  year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  month_index = (5 * day_of_year + 2) / 153;

  civil->day = (int)(day_of_year - (153 * month_index + 2) / 5 + 1);
  civil->month = (int)(month_index < 10 ? month_index + 3 : month_index - 9);
  civil->year = (int)(year_of_era + era * 400 + (civil->month <= 2));
  civil->hour = (int)(ms_of_day / 3600000);
  civil->minute = (int)(ms_of_day / 60000 % 60);
  civil->second = (int)(ms_of_day / 1000 % 60);
  civil->millisecond = (int)(ms_of_day % 1000);
}

/* RFC 3339 UTC, with milliseconds only when there are any */
static void tig_format_time(int64_t unix_ms, char *out, size_t out_size) {
  tig_civil civil;

  tig_civil_from_ms(unix_ms, &civil);
  if (civil.millisecond != 0) {
    snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", civil.year, civil.month, civil.day,
             civil.hour, civil.minute, civil.second, civil.millisecond);
  } else {
    snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02dZ", civil.year, civil.month, civil.day,
             civil.hour, civil.minute, civil.second);
  }
}

static int tig_parse_digits(const char *text, size_t count) {
  int value = 0;
  size_t i;

  for (i = 0; i < count; ++i) {
    if (!tig_is_digit(text[i])) {
      return -1;
    }
    value = value * 10 + (text[i] - '0');
  }
  return value;
}

/* parses what tig_format_time writes: YYYY-MM-DDTHH:MM:SS[.f{1,3}]Z */
static int tig_parse_time(const char *text, size_t length, int64_t *unix_ms) {
  tig_civil civil;
  tig_civil check;
  size_t fraction = 0;
  int64_t value;

  if (length < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':' || text[length - 1] != 'Z') {
    return 0;
  }

  civil.year = tig_parse_digits(text, 4);
  civil.month = tig_parse_digits(text + 5, 2);
  civil.day = tig_parse_digits(text + 8, 2);
  civil.hour = tig_parse_digits(text + 11, 2);
  civil.minute = tig_parse_digits(text + 14, 2);
  civil.second = tig_parse_digits(text + 17, 2);
  civil.millisecond = 0;

  if (length > 20) {
    fraction = length - 21;
    if (text[19] != '.' || fraction == 0 || fraction > 3) {
      return 0;
    }
    civil.millisecond = tig_parse_digits(text + 20, fraction);
    if (civil.millisecond < 0) {
      return 0;
    }
    for (; fraction < 3; ++fraction) {
      civil.millisecond *= 10;
    }
  }

  if (civil.year < 1970 || civil.month < 1 || civil.month > 12 || civil.day < 1 || civil.day > 31 ||
      civil.hour < 0 || civil.hour > 23 || civil.minute < 0 || civil.minute > 59 || civil.second < 0 ||
      civil.second > 59) {
    return 0;
  }

  value = tig_days_from_civil(civil.year, civil.month, civil.day) * 86400000 +
          (int64_t)civil.hour * 3600000 + (int64_t)civil.minute * 60000 + (int64_t)civil.second * 1000 +
          civil.millisecond;

  /* rejects days past the end of the month, e.g. 2026-02-30 */
  tig_civil_from_ms(value, &check);
  if (check.day != civil.day || check.month != civil.month) {
    return 0;
  }

  *unix_ms = value;
  return 1;
}

/* path of the chunk holding unix_ms, relative to the stream's folder */
static void tig_format_chunk(int chunk, int64_t unix_ms, char *out, size_t out_size) {
  tig_civil c;

  tig_civil_from_ms(unix_ms, &c);
  switch (chunk) {
    case TIG_CHUNK_HOUR:
      snprintf(out, out_size, "%04d/%02d/%04d-%02d-%02dT%02d.csv", c.year, c.month, c.year, c.month, c.day,
               c.hour);
      break;
    case TIG_CHUNK_DAY:
      snprintf(out, out_size, "%04d/%04d-%02d-%02d.csv", c.year, c.year, c.month, c.day);
      break;
    case TIG_CHUNK_MONTH:
      snprintf(out, out_size, "%04d/%04d-%02d.csv", c.year, c.year, c.month);
      break;
    default:
      snprintf(out, out_size, "%04d.csv", c.year);
      break;
  }
}

static int tig_is_chunk_name(const char *name, int chunk) {
  const char *pattern = tig_chunk_patterns[chunk];
  size_t length = strlen(pattern);
  size_t i;

  if (strlen(name) != length) {
    return 0;
  }
  for (i = 0; i < length; ++i) {
    if (pattern[i] == 'd' ? !tig_is_digit(name[i]) : name[i] != pattern[i]) {
      return 0;
    }
  }

  /* folders must match the file name, e.g. 2026/2026-10-23.csv */
  switch (chunk) {
    case TIG_CHUNK_HOUR:
      return memcmp(name, name + 8, 4) == 0 && memcmp(name + 5, name + 13, 2) == 0;
    case TIG_CHUNK_DAY:
    case TIG_CHUNK_MONTH:
      return memcmp(name, name + 5, 4) == 0;
    default:
      return 1;
  }
}

/* --- stream definitions --- */

static int tig_is_reserved_scalar(const char *text) {
  static const char *const reserved[] = {"true", "false", "yes", "no", "on", "off", "null", "y", "n"};
  size_t i;
  size_t j;

  for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
    for (j = 0; reserved[i][j] != '\0' && text[j] != '\0'; ++j) {
      char c = text[j] >= 'A' && text[j] <= 'Z' ? (char)(text[j] - 'A' + 'a') : text[j];
      if (c != reserved[i][j]) {
        break;
      }
    }
    if (reserved[i][j] == '\0' && text[j] == '\0') {
      return 1;
    }
  }
  return 0;
}

/* plain YAML scalar when unambiguous, otherwise double-quoted */
static void tig_write_scalar(tig_writer *writer, const char *text) {
  size_t length = strlen(text);
  int plain = length > 0 && tig_is_letter(text[0]) && text[length - 1] != ' ' && !tig_is_reserved_scalar(text);
  size_t i;

  for (i = 0; plain && i < length; ++i) {
    plain = tig_is_letter(text[i]) || tig_is_digit(text[i]) || strchr(" _./()+-", text[i]) != 0;
  }

  if (plain) {
    tig_write_string(writer, text);
    return;
  }

  tig_write_string(writer, "\"");
  for (i = 0; i < length; ++i) {
    if (text[i] == '"' || text[i] == '\\') {
      tig_write_string(writer, "\\");
    }
    tig_write(writer, text + i, 1);
  }
  tig_write_string(writer, "\"");
}

static void tig_write_duration(tig_writer *writer, int64_t ms) {
  char text[32];

  if (ms % 3600000 == 0) {
    snprintf(text, sizeof(text), "%lldh", (long long)(ms / 3600000));
  } else if (ms % 60000 == 0) {
    snprintf(text, sizeof(text), "%lldm", (long long)(ms / 60000));
  } else if (ms % 1000 == 0) {
    snprintf(text, sizeof(text), "%llds", (long long)(ms / 1000));
  } else {
    snprintf(text, sizeof(text), "%lldms", (long long)ms);
  }
  tig_write_string(writer, text);
}

static int tig_parse_duration(const char *text, size_t length, int64_t *ms) {
  int64_t value = 0;
  size_t i = 0;

  while (i < length && tig_is_digit(text[i])) {
    if (i >= 12) {
      return 0;
    }
    value = value * 10 + (text[i] - '0');
    ++i;
  }
  if (i == 0 || value == 0) {
    return 0;
  }

  if (length - i == 2 && memcmp(text + i, "ms", 2) == 0) {
    *ms = value;
  } else if (length - i == 1 && text[i] == 's') {
    *ms = value * 1000;
  } else if (length - i == 1 && text[i] == 'm') {
    *ms = value * 60000;
  } else if (length - i == 1 && text[i] == 'h') {
    *ms = value * 3600000;
  } else {
    return 0;
  }
  return 1;
}

static tig_status tig_render_definition(const char *stream, const tig_stream_definition *definition, char *out,
                                        size_t out_size) {
  tig_writer writer = {out, out_size, 0, 0};
  size_t i;
  size_t j;

  if (definition->columns == 0 || definition->column_count == 0 || definition->column_count > TIG_COLUMNS_MAX) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, stream, "a stream needs between 1 and 64 columns");
  }
  if ((int)definition->chunk < TIG_CHUNK_HOUR || (int)definition->chunk > TIG_CHUNK_YEAR) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, stream, "unknown chunk period");
  }
  if (definition->max_lag_ms <= 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, stream, "max_lag_ms must be positive");
  }
  if ((definition->description != 0 && !tig_is_valid_text(definition->description)) ||
      (definition->unit != 0 && !tig_is_valid_text(definition->unit))) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, stream, "description and unit must be single lines");
  }

  out[0] = '\0';
  if (definition->description != 0 && definition->description[0] != '\0') {
    tig_write_string(&writer, "description: ");
    tig_write_scalar(&writer, definition->description);
    tig_write_string(&writer, "\n");
  }
  if (definition->unit != 0 && definition->unit[0] != '\0') {
    tig_write_string(&writer, "unit: ");
    tig_write_scalar(&writer, definition->unit);
    tig_write_string(&writer, "\n");
  }

  tig_write_string(&writer, "columns:\n");
  for (i = 0; i < definition->column_count; ++i) {
    const tig_column *column = &definition->columns[i];

    if (column->name == 0 || !tig_is_valid_column_name(column->name, strlen(column->name))) {
      return tig__fail(TIG_ERROR_INVALID_ARGUMENT, stream,
                       "column names are lowercase letters, digits and _, and not \"time\"");
    }
    for (j = 0; j < i; ++j) {
      if (strcmp(definition->columns[j].name, column->name) == 0) {
        return tig__fail(TIG_ERROR_INVALID_ARGUMENT, column->name, "duplicate column name");
      }
    }
    if ((int)column->type < TIG_VALUE_INTEGER || (int)column->type > TIG_VALUE_TEXT) {
      return tig__fail(TIG_ERROR_INVALID_ARGUMENT, column->name, "unknown column type");
    }

    tig_write_string(&writer, "  - name: ");
    tig_write_string(&writer, column->name);
    tig_write_string(&writer, "\n    type: ");
    tig_write_string(&writer, tig_type_names[column->type]);
    tig_write_string(&writer, "\n");
  }

  tig_write_string(&writer, "chunk: ");
  tig_write_string(&writer, tig_chunk_names[definition->chunk]);
  tig_write_string(&writer, "\nmax_lag: ");
  tig_write_duration(&writer, definition->max_lag_ms);
  tig_write_string(&writer, "\n");

  return writer.overflow ? tig__fail(TIG_ERROR_INVALID_ARGUMENT, stream, "stream definition too long") : TIG_OK;
}

/* reads back what tig_render_definition writes; description and unit aren't needed for writing */
static tig_status tig_parse_definition(const char *text, size_t length, tig_definition *out, const char *path) {
  const char *line = text;
  const char *end = text + length;
  int in_columns = 0;
  size_t i;

  memset(out, 0, sizeof(*out));
  out->chunk = -1;

  while (line < end) {
    const char *line_end = memchr(line, '\n', (size_t)(end - line));
    size_t line_length;

    if (line_end == 0) {
      line_end = end;
    }
    line_length = (size_t)(line_end - line);
    if (line_length > 0 && line[line_length - 1] == '\r') {
      --line_length;
    }

#define TIG_KEY(key) (line_length >= sizeof(key) - 1 && memcmp(line, key, sizeof(key) - 1) == 0)
#define TIG_VALUE(key) line + sizeof(key) - 1, line_length - (sizeof(key) - 1)
    if (in_columns && TIG_KEY("  - name: ")) {
      if (out->column_count == TIG_COLUMNS_MAX || !tig_is_valid_column_name(TIG_VALUE("  - name: "))) {
        return tig__fail(TIG_ERROR_REJECTED, path, "invalid column");
      }
      memcpy(out->column_names[out->column_count], TIG_VALUE("  - name: "));
      out->column_types[out->column_count] = -1;
      ++out->column_count;
    } else if (in_columns && TIG_KEY("    type: ") && out->column_count > 0) {
      out->column_types[out->column_count - 1] = tig_find_name(tig_type_names, 3, TIG_VALUE("    type: "));
    } else if (TIG_KEY("columns:")) {
      in_columns = 1;
    } else if (TIG_KEY("chunk: ")) {
      in_columns = 0;
      out->chunk = tig_find_name(tig_chunk_names, 4, TIG_VALUE("chunk: "));
    } else if (TIG_KEY("max_lag: ")) {
      in_columns = 0;
      if (!tig_parse_duration(TIG_VALUE("max_lag: "), &out->max_lag_ms)) {
        return tig__fail(TIG_ERROR_REJECTED, path, "invalid max_lag");
      }
    } else if (line_length > 0 && line[0] != ' ') {
      in_columns = 0;
    }
#undef TIG_KEY
#undef TIG_VALUE

    line = line_end + 1;
  }

  for (i = 0; i < out->column_count; ++i) {
    if (out->column_types[i] < 0) {
      return tig__fail(TIG_ERROR_REJECTED, path, "column without a valid type");
    }
  }
  if (out->column_count == 0 || out->chunk < 0 || out->max_lag_ms <= 0) {
    return tig__fail(TIG_ERROR_REJECTED, path, "stream definition needs columns, chunk and max_lag");
  }
  return TIG_OK;
}

/* --- files --- */

/* reads a whole small file; *found is 0 when it doesn't exist */
static tig_status tig_read_small_file(const char *path, char *out, size_t out_size, size_t *length, int *found) {
  FILE *file = fopen(path, "rb");

  *found = 0;
  *length = 0;
  if (file == 0) {
    return errno == ENOENT ? TIG_OK : tig__fail_errno(path);
  }

  *length = fread(out, 1, out_size, file);
  if (ferror(file)) {
    fclose(file);
    return tig__fail_errno(path);
  }
  if (*length == out_size) {
    fclose(file);
    return tig__fail(TIG_ERROR_REJECTED, path, "file too large");
  }

  fclose(file);
  out[*length] = '\0';
  *found = 1;
  return TIG_OK;
}

/* creates each folder between the end of base and the file name */
static tig_status tig_make_parents(char *path, size_t base_length) {
  char *slash;
  tig_status result;

  for (slash = strchr(path + base_length, '/'); slash != 0; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    result = tig__make_directory(path);
    *slash = '/';
    if (result != TIG_OK) {
      return result;
    }
  }
  return TIG_OK;
}

typedef struct tig_names {
  char **items;
  size_t count;
} tig_names;

static void tig_free_names(tig_names *names) {
  size_t i;

  for (i = 0; i < names->count; ++i) {
    free(names->items[i]);
  }
  free(names->items);
  names->items = 0;
  names->count = 0;
}

static tig_status tig_add_name(tig_names *names, const char *name) {
  char **items;
  size_t length = strlen(name);

  /* chunks and their folders always start with the year */
  if (!tig_is_digit(name[0])) {
    return TIG_OK;
  }

  items = (char **)realloc(names->items, (names->count + 1) * sizeof(*items));
  if (items == 0) {
    return tig__fail(TIG_ERROR_GIT, "telemetry", "out of memory");
  }
  names->items = items;
  names->items[names->count] = (char *)malloc(length + 1);
  if (names->items[names->count] == 0) {
    return tig__fail(TIG_ERROR_GIT, "telemetry", "out of memory");
  }
  memcpy(names->items[names->count], name, length + 1);
  ++names->count;
  return TIG_OK;
}

static int tig_compare_names_descending(const void *left, const void *right) {
  return strcmp(*(char *const *)right, *(char *const *)left);
}

/* entries in dir that start with a digit, newest first; a missing dir has none */
static tig_status tig_list_names(const char *dir, tig_names *names) {
  tig_status result = TIG_OK;
#ifdef _WIN32
  char pattern[TIG_PATH_MAX];
  WIN32_FIND_DATAA entry;
  HANDLE find;

  result = tig__format_path(pattern, sizeof(pattern), dir, "/*", "");
  if (result != TIG_OK) {
    return result;
  }
  find = FindFirstFileA(pattern, &entry);
  if (find == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
               ? TIG_OK
               : tig__fail(TIG_ERROR_GIT, dir, "cannot list folder");
  }
  do {
    result = tig_add_name(names, entry.cFileName);
  } while (result == TIG_OK && FindNextFileA(find, &entry));
  FindClose(find);
#else
  DIR *handle = opendir(dir);
  struct dirent *entry;

  if (handle == 0) {
    return errno == ENOENT ? TIG_OK : tig__fail_errno(dir);
  }
  while (result == TIG_OK && (entry = readdir(handle)) != 0) {
    result = tig_add_name(names, entry->d_name);
  }
  closedir(handle);
#endif

  if (result == TIG_OK && names->count > 1) {
    qsort(names->items, names->count, sizeof(*names->items), tig_compare_names_descending);
  }
  return result;
}

/* time of the last row of a chunk; *found is 0 if it only has a header */
static tig_status tig_read_last_time(const char *path, int *found, int64_t *unix_ms) {
  char tail[TIG_ROW_MAX * 2];
  FILE *file = fopen(path, "rb");
  long size;
  size_t length;
  size_t start;
  size_t field_end;

  *found = 0;
  if (file == 0) {
    return tig__fail_errno(path);
  }
  if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0) {
    fclose(file);
    return tig__fail_errno(path);
  }

  length = (size_t)size < sizeof(tail) ? (size_t)size : sizeof(tail);
  if (fseek(file, size - (long)length, SEEK_SET) != 0 || fread(tail, 1, length, file) != length) {
    fclose(file);
    return tig__fail_errno(path);
  }
  fclose(file);

  if (length == 0) {
    return TIG_OK;
  }
  if (tail[length - 1] != '\n') {
    return tig__fail(TIG_ERROR_REJECTED, path, "chunk ends with an incomplete row");
  }

  start = length - 1;
  while (start > 0 && tail[start - 1] != '\n') {
    --start;
  }
  if (start == 0 && length < (size_t)size) {
    return tig__fail(TIG_ERROR_REJECTED, path, "last row too long");
  }

  for (field_end = start; tail[field_end] != ',' && tail[field_end] != '\n'; ++field_end) {
  }
  if (field_end - start == 4 && memcmp(tail + start, "time", 4) == 0) {
    return TIG_OK;
  }
  if (!tig_parse_time(tail + start, field_end - start, unix_ms)) {
    return tig__fail(TIG_ERROR_REJECTED, path, "last row has an invalid time");
  }

  *found = 1;
  return TIG_OK;
}

/* time of the stream's latest reading, searching chunks newest first */
static tig_status tig_find_last_time(const char *dir, int depth, int *found, int64_t *unix_ms) {
  tig_names names = {0, 0};
  char path[TIG_PATH_MAX];
  size_t i;
  tig_status result;

  *found = 0;
  result = tig_list_names(dir, &names);
  for (i = 0; result == TIG_OK && !*found && i < names.count; ++i) {
    size_t length = strlen(names.items[i]);

    result = tig__format_path(path, sizeof(path), dir, "/", names.items[i]);
    if (result != TIG_OK) {
      break;
    }
    if (depth > 0) {
      result = tig_find_last_time(path, depth - 1, found, unix_ms);
    } else if (length > 4 && strcmp(names.items[i] + length - 4, ".csv") == 0) {
      result = tig_read_last_time(path, found, unix_ms);
    }
  }

  tig_free_names(&names);
  return result;
}

/* --- public API --- */

static tig_status tig_stream_path(tig_repository *repository, const char *stream, const char *suffix, char *out,
                                  size_t out_size, size_t *base_length) {
  const char *workdir;

  if (repository == 0 || stream == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "telemetry", "repository and stream are required");
  }
  if (!tig_is_valid_stream_name(stream)) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, stream,
                     "stream names are lowercase letters, digits, '.' and '-', starting with a letter or digit");
  }

  workdir = git_repository_workdir(repository->git);
  if (workdir == 0) {
    return tig__fail(TIG_ERROR_GIT, "telemetry", "repository is bare");
  }
  if (base_length != 0) {
    *base_length = strlen(workdir);
  }

  /* workdir always ends with a path separator */
  if (snprintf(out, out_size, "%stelemetry/%s/%s", workdir, stream, suffix) >= (int)out_size) {
    return tig__fail(TIG_ERROR_BUFFER_TOO_SMALL, "path too long", workdir);
  }
  return TIG_OK;
}

tig_status tig_telemetry_stream_define(tig_repository *repository, const char *stream,
                                       const tig_stream_definition *definition) {
  char path[TIG_PATH_MAX];
  char rendered[TIG_DEFINITION_MAX];
  char existing[TIG_DEFINITION_MAX];
  size_t base_length;
  size_t length;
  int found;
  FILE *file;
  tig_status result;

  if (definition == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_telemetry_stream_define", "definition is required");
  }

  result = tig_stream_path(repository, stream, "stream.yaml", path, sizeof(path), &base_length);
  if (result == TIG_OK) {
    result = tig_render_definition(stream, definition, rendered, sizeof(rendered));
  }
  if (result == TIG_OK) {
    result = tig_read_small_file(path, existing, sizeof(existing), &length, &found);
  }
  if (result != TIG_OK) {
    return result;
  }

  /* a definition never changes once written */
  if (found) {
    return length == strlen(rendered) && memcmp(existing, rendered, length) == 0
               ? TIG_OK
               : tig__fail(TIG_ERROR_REJECTED, stream, "stream is already defined differently");
  }

  result = tig_make_parents(path, base_length);
  if (result != TIG_OK) {
    return result;
  }

  file = fopen(path, "wb");
  if (file == 0) {
    return tig__fail_errno(path);
  }
  if (fputs(rendered, file) < 0) {
    fclose(file);
    return tig__fail_errno(path);
  }
  return fclose(file) == 0 ? TIG_OK : tig__fail_errno(path);
}

tig_status tig_telemetry_append(tig_repository *repository, const char *stream, int64_t unix_ms,
                                const char *const *values, size_t value_count) {
  char path[TIG_PATH_MAX];
  char text[TIG_DEFINITION_MAX];
  char chunk[64];
  char row[TIG_ROW_MAX];
  tig_writer writer = {row, sizeof(row), 0, 0};
  tig_definition definition;
  size_t base_length;
  size_t stream_dir_length;
  size_t length;
  size_t i;
  int found;
  int exists;
  int64_t now;
  int64_t last;
  FILE *file;
  tig_status result;

  result = tig_stream_path(repository, stream, "stream.yaml", path, sizeof(path), &base_length);
  if (result == TIG_OK) {
    result = tig_read_small_file(path, text, sizeof(text), &length, &found);
  }
  if (result == TIG_OK && !found) {
    result = tig__fail(TIG_ERROR_NOT_FOUND, stream, "stream is not defined");
  }
  if (result == TIG_OK) {
    result = tig_parse_definition(text, length, &definition, path);
  }
  if (result != TIG_OK) {
    return result;
  }

  if (value_count != definition.column_count || (value_count > 0 && values == 0)) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, stream, "one value is needed for each column");
  }
  if (unix_ms < 0 || unix_ms > TIG_TIME_MAX_MS) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, stream, "time out of range");
  }

  tig_format_time(unix_ms, chunk, sizeof(chunk));
  tig_write_string(&writer, chunk);
  for (i = 0; i < value_count; ++i) {
    const char *value = values[i] != 0 ? values[i] : "";

    if (value[0] != '\0' && !tig_is_valid_value(value, definition.column_types[i])) {
      return tig__fail(TIG_ERROR_INVALID_ARGUMENT, definition.column_names[i],
                       definition.column_types[i] == TIG_VALUE_TEXT ? "text must be a single line"
                                                                    : "not a valid number");
    }

    tig_write_string(&writer, ",");
    if (strchr(value, ',') != 0 || strchr(value, '"') != 0) {
      tig_write_string(&writer, "\"");
      for (; *value != '\0'; ++value) {
        tig_write(&writer, *value == '"' ? "\"\"" : value, *value == '"' ? 2 : 1);
      }
      tig_write_string(&writer, "\"");
    } else {
      tig_write_string(&writer, value);
    }
  }
  tig_write_string(&writer, "\n");
  if (writer.overflow) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, stream, "reading too long");
  }

  /* readings are written as soon as they are sampled */
  result = tig_clock_now(repository, &now);
  if (result != TIG_OK) {
    return result;
  }
  if (unix_ms < now - definition.max_lag_ms) {
    return tig__fail(TIG_ERROR_REJECTED, stream, "reading is older than max_lag");
  }
  if (unix_ms > now + TIG_CLOCK_SKEW_MS) {
    return tig__fail(TIG_ERROR_REJECTED, stream, "reading is in the future");
  }

  stream_dir_length = strlen(path) - strlen("/stream.yaml");
  path[stream_dir_length] = '\0';
  result = tig_find_last_time(path, tig_chunk_depths[definition.chunk], &found, &last);
  if (result != TIG_OK) {
    return result;
  }
  if (found && unix_ms <= last) {
    return tig__fail(TIG_ERROR_REJECTED, stream, "reading is not later than the stream's last reading");
  }

  tig_format_chunk(definition.chunk, unix_ms, chunk, sizeof(chunk));
  result = tig__format_path(path + stream_dir_length, sizeof(path) - stream_dir_length, "/", chunk, "");
  if (result == TIG_OK) {
    result = tig_make_parents(path, base_length);
  }
  if (result != TIG_OK) {
    return result;
  }

  file = fopen(path, "rb");
  exists = file != 0;
  if (file != 0) {
    fclose(file);
  }

  file = fopen(path, "ab");
  if (file == 0) {
    return tig__fail_errno(path);
  }
  if (!exists) {
    fputs("time", file);
    for (i = 0; i < definition.column_count; ++i) {
      fputc(',', file);
      fputs(definition.column_names[i], file);
    }
    fputc('\n', file);
  }
  if (fwrite(row, 1, writer.length, file) != writer.length) {
    fclose(file);
    return tig__fail_errno(path);
  }
  return fclose(file) == 0 ? TIG_OK : tig__fail_errno(path);
}

/* --- commit checks --- */

static tig_status tig_load_blob(git_repository *git, const git_oid *id, git_blob **blob, const char *path) {
  return git_blob_lookup(blob, git, id) == 0 ? TIG_OK : tig__fail_git(path);
}

tig_status tig__telemetry_check_change(git_repository *git, git_index *index, const git_diff_delta *delta) {
  const char *path = delta->status == GIT_DELTA_DELETED ? delta->old_file.path : delta->new_file.path;
  const char *stream;
  const char *file;
  char stream_name[TIG_STREAM_NAME_MAX + 1];
  char definition_path[TIG_PATH_MAX];
  const git_index_entry *entry;
  git_blob *old_blob = 0;
  git_blob *new_blob = 0;
  tig_definition definition;
  tig_status result;

  if (strncmp(path, "telemetry/", strlen("telemetry/")) != 0) {
    return tig__fail(TIG_ERROR_REJECTED, path, "only telemetry can be committed");
  }
  stream = path + strlen("telemetry/");
  file = strchr(stream, '/');
  if (file == 0 || (size_t)(file - stream) > TIG_STREAM_NAME_MAX) {
    return tig__fail(TIG_ERROR_REJECTED, path, "not part of a telemetry stream");
  }
  memcpy(stream_name, stream, (size_t)(file - stream));
  stream_name[file - stream] = '\0';
  ++file;
  if (!tig_is_valid_stream_name(stream_name)) {
    return tig__fail(TIG_ERROR_REJECTED, path, "invalid stream name");
  }

  if (delta->status == GIT_DELTA_ADDED) {
    /* the stream's definition, as it will be committed */
    snprintf(definition_path, sizeof(definition_path), "telemetry/%s/stream.yaml", stream_name);
    entry = git_index_get_bypath(index, definition_path, 0);
    if (entry == 0) {
      return tig__fail(TIG_ERROR_REJECTED, path, "stream has no stream.yaml");
    }
    result = tig_load_blob(git, &entry->id, &new_blob, definition_path);
    if (result == TIG_OK) {
      size_t length = (size_t)git_blob_rawsize(new_blob);
      result = tig_parse_definition((const char *)git_blob_rawcontent(new_blob), length, &definition,
                                    definition_path);
    }
    git_blob_free(new_blob);

    if (result == TIG_OK && strcmp(file, "stream.yaml") != 0 && !tig_is_chunk_name(file, definition.chunk)) {
      result = tig__fail(TIG_ERROR_REJECTED, path, "not a chunk of this stream");
    }
    return result;
  }

  if (delta->status != GIT_DELTA_MODIFIED) {
    return tig__fail(TIG_ERROR_REJECTED, path, "telemetry cannot be deleted, renamed or replaced");
  }
  if (strcmp(file, "stream.yaml") == 0) {
    return tig__fail(TIG_ERROR_REJECTED, path, "stream definitions cannot change");
  }

  result = tig_load_blob(git, &delta->old_file.id, &old_blob, path);
  if (result == TIG_OK) {
    result = tig_load_blob(git, &delta->new_file.id, &new_blob, path);
  }
  if (result == TIG_OK) {
    git_object_size_t old_size = git_blob_rawsize(old_blob);
    git_object_size_t new_size = git_blob_rawsize(new_blob);
    const char *new_content = (const char *)git_blob_rawcontent(new_blob);

    if (new_size <= old_size || memcmp(git_blob_rawcontent(old_blob), new_content, (size_t)old_size) != 0 ||
        new_content[new_size - 1] != '\n') {
      result = tig__fail(TIG_ERROR_REJECTED, path, "telemetry can only be appended to");
    }
  }

  git_blob_free(new_blob);
  git_blob_free(old_blob);
  return result;
}
