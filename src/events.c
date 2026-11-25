#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

/* see tig-docs/EVENTS.md */

#define TIG_INITIAL_PATH "state/initial.yaml"
#define TIG_EVENT_ID_MIN 6
#define TIG_EVENT_ID_LENGTH 8
#define TIG_ITEM_ID_MAX 256
#define TIG_FILE_MAX (1024 * 1024)
/* a calendar date is where it happened, which can be up to 14 hours ahead of UTC */
#define TIG_DATE_AHEAD_MS (14 * INT64_C(3600000))

enum { TIG_PRECISION_YEAR, TIG_PRECISION_MONTH, TIG_PRECISION_DAY, TIG_PRECISION_INSTANT };

typedef struct tig_time {
  int64_t start_ms; /* start of the period the time covers, with dates taken as UTC */
  int precision;
  char prefix[11];  /* the time in an event's file name: no finer than a day */
} tig_time;

typedef struct tig_entry {
  char *path; /* relative to the working tree */
  char id[TIG_EVENT_ID_MAX + 1];
  tig_time time;
  tig_node *event;
} tig_entry;

/* initial state and events, as in the working tree or the index */
typedef struct tig_timeline {
  tig_node *initial;
  tig_entry *entries;
  size_t count;
  size_t capacity;
} tig_timeline;

static int tig_is_digit(char c) {
  return c >= '0' && c <= '9';
}

static int tig_is_lower(char c) {
  return c >= 'a' && c <= 'z';
}

static char *tig_copy(const char *text) {
  size_t length = strlen(text);
  char *copy = (char *)malloc(length + 1);

  if (copy != 0) {
    memcpy(copy, text, length + 1);
  }
  return copy;
}

/* --- times and IDs --- */

static int tig_digits(const char *text, size_t count) {
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

static int tig_days_in_month(int year, int month) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

  return month == 2 && leap ? 29 : days[month - 1];
}

/* YYYY, YYYY-MM, YYYY-MM-DD, or an RFC 3339 UTC instant with Z */
static int tig_parse_event_time(const char *text, tig_time *out) {
  size_t length = text != 0 ? strlen(text) : 0;
  int year;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int64_t fraction_ms = 0;

  if (length < 4 || (year = tig_digits(text, 4)) < 0) {
    return 0;
  }
  if (length == 4) {
    out->precision = TIG_PRECISION_YEAR;
  } else if (length == 7 && text[4] == '-') {
    out->precision = TIG_PRECISION_MONTH;
  } else if (length == 10 && text[4] == '-' && text[7] == '-') {
    out->precision = TIG_PRECISION_DAY;
  } else if (length >= 20 && text[4] == '-' && text[7] == '-' && text[10] == 'T' && text[13] == ':' &&
             text[16] == ':' && text[length - 1] == 'Z') {
    out->precision = TIG_PRECISION_INSTANT;
    hour = tig_digits(text + 11, 2);
    minute = tig_digits(text + 14, 2);
    second = tig_digits(text + 17, 2);
    if (length > 20) {
      size_t digits = length - 21;
      size_t i;
      int64_t scale = 100;

      if (text[19] != '.' || digits == 0 || digits > 9) {
        return 0;
      }
      for (i = 0; i < digits; ++i) {
        if (!tig_is_digit(text[20 + i])) {
          return 0;
        }
        fraction_ms += (text[20 + i] - '0') * scale;
        scale /= 10;
      }
    } else if (length != 20) {
      return 0;
    }
  } else {
    return 0;
  }

  if (out->precision >= TIG_PRECISION_MONTH) {
    month = tig_digits(text + 5, 2);
  }
  if (out->precision >= TIG_PRECISION_DAY) {
    day = tig_digits(text + 8, 2);
  }
  if (month < 1 || month > 12 || day < 1 || day > tig_days_in_month(year, month) || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 59) {
    return 0;
  }

  out->start_ms = tig__days_from_civil(year, month, day) * INT64_C(86400000) + hour * INT64_C(3600000) +
                  minute * INT64_C(60000) + second * INT64_C(1000) + fraction_ms;
  length = length < 10 ? length : 10;
  memcpy(out->prefix, text, length);
  out->prefix[length] = '\0';
  return 1;
}

static int tig_is_valid_event_id(const char *id, size_t length) {
  size_t i;

  if (length < TIG_EVENT_ID_MIN || length > TIG_EVENT_ID_MAX) {
    return 0;
  }
  for (i = 0; i < length; ++i) {
    if (!(tig_is_lower(id[i]) || tig_is_digit(id[i]))) {
      return 0;
    }
  }
  return 1;
}

static int tig_is_valid_item_id(const char *id) {
  size_t length = strlen(id);
  size_t i;

  if (length == 0 || length > TIG_ITEM_ID_MAX || !(tig_is_lower(id[0]) || tig_is_digit(id[0]))) {
    return 0;
  }
  for (i = 1; i < length; ++i) {
    if (!(tig_is_lower(id[i]) || tig_is_digit(id[i]) || id[i] == '-' || id[i] == '.' || id[i] == '/')) {
      return 0;
    }
  }
  return 1;
}

/* the ID in an event path, events/YYYY/<time>.<id>.yaml; the time is checked against the file's contents later */
static int tig_event_path_id(const char *path, char *id) {
  const char *name;
  const char *dot;
  size_t length;
  size_t prefix;

  if (strncmp(path, "events/", 7) != 0 || tig_digits(path + 7, 4) < 0 || path[11] != '/') {
    return 0;
  }
  name = path + 12;
  length = strlen(name);
  dot = strchr(name, '.');
  if (dot == 0 || length < 5 || strcmp(name + length - 5, ".yaml") != 0) {
    return 0;
  }
  prefix = (size_t)(dot - name);
  length -= prefix + 1 + 5;
  if ((prefix != 4 && prefix != 7 && prefix != 10) || !tig_is_valid_event_id(dot + 1, length)) {
    return 0;
  }
  memcpy(id, dot + 1, length);
  id[length] = '\0';
  return 1;
}

static int tig_is_placeholder(const char *path) {
  const char *slash = strrchr(path, '/');

  return strcmp(slash != 0 ? slash + 1 : path, ".gitkeep") == 0;
}

static void tig_event_path(char *out, size_t out_size, const tig_time *time, const char *id) {
  snprintf(out, out_size, "events/%.4s/%s.%s.yaml", time->prefix, time->prefix, id);
}

/* --- format --- */

static tig_status tig_reject(const char *context, const char *what, const char *problem) {
  char detail[512];

  snprintf(detail, sizeof(detail), "%s %s", what, problem);
  return tig__fail(TIG_ERROR_REJECTED, context, detail);
}

static int tig_is_scalar(const tig_node *node) {
  return node != 0 && tig_node_type_of(node) == TIG_NODE_SCALAR;
}

static int tig_is_map(const tig_node *node) {
  return node != 0 && tig_node_type_of(node) == TIG_NODE_MAP;
}

static int tig_is_list(const tig_node *node) {
  return node != 0 && tig_node_type_of(node) == TIG_NODE_LIST;
}

static int tig_is_decimal(const tig_node *node) {
  const char *c = tig_node_text(node);

  if (c == 0) {
    return 0;
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
  if (*c == '.') {
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

/* fields that must be scalars when present */
static tig_status tig_check_scalars(const tig_node *map, const char *const *fields, size_t count, const char *context) {
  size_t i;

  for (i = 0; i < count; ++i) {
    const tig_node *value = tig_node_get(map, fields[i]);
    if (value != 0 && !tig_is_scalar(value)) {
      return tig_reject(context, fields[i], "must be text");
    }
  }
  return TIG_OK;
}

static tig_status tig_check_cost(const tig_node *cost, const char *context) {
  const char *currency;
  size_t i;

  if (cost == 0) {
    return TIG_OK;
  }
  currency = tig_node_text(tig_node_get(cost, "currency"));
  if (!tig_is_map(cost) || !tig_is_decimal(tig_node_get(cost, "amount")) || currency == 0 || strlen(currency) != 3 ||
      (tig_node_get(cost, "tax") != 0 && !tig_is_decimal(tig_node_get(cost, "tax")))) {
    return tig_reject(context, "cost", "needs a decimal amount, an ISO 4217 currency and an optional decimal tax");
  }
  for (i = 0; i < 3; ++i) {
    if (currency[i] < 'A' || currency[i] > 'Z') {
      return tig_reject(context, "cost", "currency must be an ISO 4217 code, e.g. GBP");
    }
  }
  return TIG_OK;
}

static tig_status tig_check_evidence(const tig_node *evidence, const char *context) {
  size_t i;

  if (evidence == 0) {
    return TIG_OK;
  }
  if (!tig_is_list(evidence)) {
    return tig_reject(context, "evidence", "must be a list of paths");
  }
  for (i = 0; i < tig_node_count(evidence); ++i) {
    const char *path = tig_node_text(tig_node_at(evidence, i));

    if (path == 0 || strncmp(path, "/evidence/", 10) != 0 || path[10] == '\0' || strstr(path, "//") != 0 ||
        strstr(path, "/./") != 0 || strstr(path, "/../") != 0 || strchr(path, '\\') != 0 ||
        path[strlen(path) - 1] == '/' || (strlen(path) >= 3 && strcmp(path + strlen(path) - 3, "/..") == 0)) {
      return tig_reject(context, "evidence", "paths must be files under /evidence");
    }
  }
  return TIG_OK;
}

static tig_status tig_check_change(const tig_node *change, const char *context) {
  static const char *const operations[] = {"add", "update", "remove"};
  const tig_node *since;
  const tig_node *set;
  const tig_node *unset;
  const char *id = 0;
  int operation = -1;
  size_t i;
  tig_time time;

  if (!tig_is_map(change)) {
    return tig_reject(context, "a change", "must be a map");
  }
  for (i = 0; i < 3; ++i) {
    const tig_node *value = tig_node_get(change, operations[i]);
    if (value != 0) {
      if (operation >= 0) {
        return tig_reject(context, "a change", "has more than one of add, update and remove");
      }
      operation = (int)i;
      id = tig_node_text(value);
    }
  }
  if (operation < 0) {
    return tig_reject(context, "a change", "needs add, update or remove");
  }
  if (id == 0 || !tig_is_valid_item_id(id)) {
    return tig_reject(context, "item ID", "must be lowercase letters, digits, '-', '.' and '/', starting with a letter or digit");
  }

  set = tig_node_get(change, "set");
  unset = tig_node_get(change, "unset");
  since = tig_node_get(change, "since");
  if (set != 0 && (operation == 2 || !tig_is_map(set))) {
    return tig_reject(context, "set", "is a map, for add and update only");
  }
  if (unset != 0) {
    if (operation != 1 || !tig_is_list(unset)) {
      return tig_reject(context, "unset", "is a list, for update only");
    }
    for (i = 0; i < tig_node_count(unset); ++i) {
      if (!tig_is_scalar(tig_node_at(unset, i))) {
        return tig_reject(context, "unset", "lists attribute names");
      }
    }
  }
  if (since != 0 && (operation != 0 || !tig_is_scalar(since) ||
                     (strcmp(tig_node_text(since), "unknown") != 0 && !tig_parse_event_time(tig_node_text(since), &time)))) {
    return tig_reject(context, "since", "is a time or unknown, for add only");
  }
  return TIG_OK;
}

static tig_status tig_check_event(const tig_node *event, const char *context, tig_time *time) {
  static const char *const event_scalars[] = {"type", "source", "notes"};
  static const char *const action_scalars[] = {"type", "description", "notes"};
  const tig_node *actions;
  const tig_node *readings;
  const tig_node *by;
  const tig_node *not_before;
  tig_time earliest;
  tig_status result;
  size_t i;
  size_t j;

  if (!tig_is_map(event)) {
    return tig__fail(TIG_ERROR_REJECTED, context, "an event must be a map");
  }
  if (!tig_parse_event_time(tig_node_text(tig_node_get(event, "time")), time)) {
    return tig_reject(context, "time", "is required: YYYY, YYYY-MM, YYYY-MM-DD or an RFC 3339 UTC instant ending in Z");
  }
  not_before = tig_node_get(event, "not_before");
  if (not_before != 0 && (!tig_parse_event_time(tig_node_text(not_before), &earliest) || earliest.start_ms > time->start_ms)) {
    return tig_reject(context, "not_before", "must be a time no later than time");
  }

  result = tig_check_scalars(event, event_scalars, 3, context);
  if (result != TIG_OK) {
    return result;
  }
  by = tig_node_get(event, "by");
  if (by != 0 && !tig_is_map(by)) {
    return tig_reject(context, "by", "must be a map");
  }
  readings = tig_node_get(event, "readings");
  if (readings != 0) {
    if (!tig_is_map(readings)) {
      return tig_reject(context, "readings", "must be a map of stream name to value");
    }
    for (i = 0; i < tig_node_count(readings); ++i) {
      const tig_node *value = tig_node_at(readings, i);

      if (!tig__is_valid_stream_name(tig_node_key_at(readings, i))) {
        return tig_reject(context, "readings", "are keyed by stream name");
      }
      for (j = 0; tig_is_map(value) && j < tig_node_count(value); ++j) {
        if (!tig_is_scalar(tig_node_at(value, j))) {
          return tig_reject(context, "readings", "are a value or a map of column to value");
        }
      }
      if (tig_is_list(value)) {
        return tig_reject(context, "readings", "are a value or a map of column to value");
      }
    }
  }
  result = tig_check_evidence(tig_node_get(event, "evidence"), context);
  if (result == TIG_OK) {
    result = tig_check_cost(tig_node_get(event, "cost"), context);
  }
  if (result != TIG_OK) {
    return result;
  }

  actions = tig_node_get(event, "actions");
  if (!tig_is_list(actions) || tig_node_count(actions) == 0) {
    return tig_reject(context, "actions", "are required: a list of at least one action");
  }
  for (i = 0; i < tig_node_count(actions); ++i) {
    const tig_node *action = tig_node_at(actions, i);
    const tig_node *changes = tig_node_get(action, "changes");

    if (!tig_is_map(action) || tig_node_get(action, "type") == 0) {
      return tig_reject(context, "an action", "must be a map with a type");
    }
    result = tig_check_scalars(action, action_scalars, 3, context);
    if (result == TIG_OK) {
      result = tig_check_evidence(tig_node_get(action, "evidence"), context);
    }
    if (result == TIG_OK) {
      result = tig_check_cost(tig_node_get(action, "cost"), context);
    }
    if (result == TIG_OK && changes != 0 && !tig_is_list(changes)) {
      result = tig_reject(context, "changes", "must be a list");
    }
    for (j = 0; result == TIG_OK && j < tig_node_count(changes); ++j) {
      result = tig_check_change(tig_node_at(changes, j), context);
    }
    if (result != TIG_OK) {
      return result;
    }
  }
  return TIG_OK;
}

static tig_status tig_check_initial(const tig_node *initial, const char *context) {
  static const char *const scalars[] = {"kind", "vocabulary"};
  const tig_node *items;
  tig_status result;
  size_t i;

  if (!tig_is_map(initial) || tig_node_get(initial, "kind") == 0) {
    return tig__fail(TIG_ERROR_REJECTED, context, "initial state must be a map with a kind");
  }
  result = tig_check_scalars(initial, scalars, 2, context);
  if (result != TIG_OK) {
    return result;
  }
  items = tig_node_get(initial, "items");
  if (items != 0 && !tig_is_map(items)) {
    return tig_reject(context, "items", "must be a map of item ID to attributes");
  }
  for (i = 0; i < tig_node_count(items); ++i) {
    if (!tig_is_valid_item_id(tig_node_key_at(items, i)) || !tig_is_map(tig_node_at(items, i))) {
      return tig_reject(context, tig_node_key_at(items, i), "is not a valid item: a lowercase ID with a map of attributes");
    }
  }
  return TIG_OK;
}

/* --- timelines --- */

static void tig_timeline_free(tig_timeline *timeline) {
  size_t i;

  for (i = 0; i < timeline->count; ++i) {
    free(timeline->entries[i].path);
    tig_node_free(timeline->entries[i].event);
  }
  free(timeline->entries);
  tig_node_free(timeline->initial);
  memset(timeline, 0, sizeof(*timeline));
}

static tig_entry *tig_timeline_find(const tig_timeline *timeline, const char *id) {
  size_t i;

  for (i = 0; i < timeline->count; ++i) {
    if (strcmp(timeline->entries[i].id, id) == 0) {
      return &timeline->entries[i];
    }
  }
  return 0;
}

static tig_entry *tig_timeline_find_path(const tig_timeline *timeline, const char *path) {
  size_t i;

  for (i = 0; i < timeline->count; ++i) {
    if (strcmp(timeline->entries[i].path, path) == 0) {
      return &timeline->entries[i];
    }
  }
  return 0;
}

static void tig_timeline_remove(tig_timeline *timeline, tig_entry *entry) {
  size_t index = (size_t)(entry - timeline->entries);

  free(entry->path);
  tig_node_free(entry->event);
  memmove(entry, entry + 1, (timeline->count - index - 1) * sizeof(*entry));
  --timeline->count;
}

/* takes event; checks its format, its path and that its ID is unique */
static tig_status tig_timeline_add(tig_timeline *timeline, const char *path, tig_node *event) {
  char expected[TIG_PATH_MAX];
  char id[TIG_EVENT_ID_MAX + 1];
  tig_entry entry;
  tig_status result;

  memset(&entry, 0, sizeof(entry));
  result = tig_check_event(event, path, &entry.time);
  if (result == TIG_OK && !tig_event_path_id(path, id)) {
    result = tig__fail(TIG_ERROR_REJECTED, path, "event files are events/YYYY/<time>.<id>.yaml");
  }
  if (result == TIG_OK) {
    tig_event_path(expected, sizeof(expected), &entry.time, id);
    if (strcmp(expected, path) != 0) {
      result = tig_reject(path, "path", "does not match the event's time");
    } else if (tig_timeline_find(timeline, id) != 0) {
      result = tig_reject(path, "event ID", "is already used by another event");
    }
  }
  if (result == TIG_OK && timeline->count == timeline->capacity) {
    size_t capacity = timeline->capacity == 0 ? 16 : timeline->capacity * 2;
    tig_entry *entries = (tig_entry *)realloc(timeline->entries, capacity * sizeof(*entries));

    if (entries == 0) {
      result = tig__fail(TIG_ERROR_GIT, path, "out of memory");
    } else {
      timeline->entries = entries;
      timeline->capacity = capacity;
    }
  }
  if (result == TIG_OK && (entry.path = tig_copy(path)) == 0) {
    result = tig__fail(TIG_ERROR_GIT, path, "out of memory");
  }
  if (result != TIG_OK) {
    tig_node_free(event);
    return result;
  }

  memcpy(entry.id, id, strlen(id) + 1);
  entry.event = event;
  timeline->entries[timeline->count++] = entry;
  return TIG_OK;
}

static tig_status tig_timeline_add_text(tig_timeline *timeline, const char *path, const char *text, size_t length) {
  tig_node *event;
  tig_status result = tig__node_parse(text, length, path, &event);

  return result == TIG_OK ? tig_timeline_add(timeline, path, event) : result;
}

static tig_status tig_timeline_set_initial_text(tig_timeline *timeline, const char *text, size_t length) {
  tig_node *initial;
  tig_status result = tig__node_parse(text, length, TIG_INITIAL_PATH, &initial);

  if (result == TIG_OK) {
    result = tig_check_initial(initial, TIG_INITIAL_PATH);
  }
  if (result != TIG_OK) {
    tig_node_free(initial);
    return result;
  }
  timeline->initial = initial;
  return TIG_OK;
}

static int tig_compare_entries(const void *left, const void *right) {
  const tig_entry *a = (const tig_entry *)left;
  const tig_entry *b = (const tig_entry *)right;

  if (a->time.start_ms != b->time.start_ms) {
    return a->time.start_ms < b->time.start_ms ? -1 : 1;
  }
  if (a->time.precision != b->time.precision) {
    return a->time.precision < b->time.precision ? -1 : 1;
  }
  return strcmp(a->id, b->id);
}

static void tig_timeline_sort(tig_timeline *timeline) {
  if (timeline->count > 1) {
    qsort(timeline->entries, timeline->count, sizeof(*timeline->entries), tig_compare_entries);
  }
}

static tig_status tig_apply_change(tig_node *items, const tig_node *change, const char *context) {
  static const char *const operations[] = {"add", "update", "remove"};
  const tig_node *set = tig_node_get(change, "set");
  const tig_node *unset = tig_node_get(change, "unset");
  const char *id = 0;
  char what[TIG_ITEM_ID_MAX + 16];
  tig_node *item;
  int operation;
  size_t i;

  for (operation = 0; operation < 3 && id == 0; ++operation) {
    id = tig_node_text(tig_node_get(change, operations[operation]));
  }
  --operation;
  item = tig_node_get(items, id);
  snprintf(what, sizeof(what), "%s %s:", operations[operation], id);

  switch (operation) {
    case 0:
      if (item != 0) {
        return tig_reject(context, what, "the item already exists");
      }
      return tig_node_set(items, id, set != 0 ? tig_node_copy(set) : tig_node_map());
    case 1:
      if (item == 0) {
        return tig_reject(context, what, "there is no such item");
      }
      for (i = 0; i < tig_node_count(set); ++i) {
        tig_status result = tig_node_set(item, tig_node_key_at(set, i), tig_node_copy(tig_node_at(set, i)));
        if (result != TIG_OK) {
          return result;
        }
      }
      for (i = 0; i < tig_node_count(unset); ++i) {
        tig_node_unset(item, tig_node_text(tig_node_at(unset, i)));
      }
      return TIG_OK;
    default:
      if (item == 0) {
        return tig_reject(context, what, "there is no such item");
      }
      tig_node_unset(items, id);
      return TIG_OK;
  }
}

/* replays the events whose time starts no later than until_ms; sorts the timeline */
static tig_status tig_timeline_replay(tig_timeline *timeline, int64_t until_ms, tig_node **out) {
  tig_node *state = timeline->initial != 0 ? tig_node_copy(timeline->initial) : tig_node_map();
  tig_node *items;
  tig_status result = TIG_OK;
  size_t i;
  size_t j;
  size_t k;

  *out = 0;
  if (state == 0) {
    return tig__fail(TIG_ERROR_GIT, "replay", "out of memory");
  }
  if (tig_node_get(state, "items") == 0) {
    result = tig_node_set(state, "items", tig_node_map());
  }
  items = tig_node_get(state, "items");

  tig_timeline_sort(timeline);
  for (i = 0; result == TIG_OK && i < timeline->count && timeline->entries[i].time.start_ms <= until_ms; ++i) {
    const tig_entry *entry = &timeline->entries[i];
    const tig_node *actions = tig_node_get(entry->event, "actions");

    for (j = 0; result == TIG_OK && j < tig_node_count(actions); ++j) {
      const tig_node *changes = tig_node_get(tig_node_at(actions, j), "changes");

      for (k = 0; result == TIG_OK && k < tig_node_count(changes); ++k) {
        result = tig_apply_change(items, tig_node_at(changes, k), entry->path);
      }
    }
  }

  if (result != TIG_OK) {
    tig_node_free(state);
    return result;
  }
  *out = state;
  return TIG_OK;
}

/* whether replaying everything succeeds */
static tig_status tig_timeline_check(tig_timeline *timeline) {
  tig_node *state;
  tig_status result = tig_timeline_replay(timeline, INT64_MAX, &state);

  tig_node_free(state);
  return result;
}

/* --- the working tree --- */

static tig_status tig_workdir(tig_repository *repository, const char **workdir) {
  *workdir = repository != 0 ? git_repository_workdir(repository->git) : 0;
  if (*workdir == 0) {
    return tig__fail(repository == 0 ? TIG_ERROR_INVALID_ARGUMENT : TIG_ERROR_GIT, "events",
                     repository == 0 ? "repository is required" : "repository is bare");
  }
  return TIG_OK;
}

/* reads a whole file into a NUL-terminated buffer; *text is 0 when it doesn't exist */
static tig_status tig_read_file(const char *path, char **text, size_t *length) {
  FILE *file = fopen(path, "rb");
  long size;

  *text = 0;
  *length = 0;
  if (file == 0) {
    return errno == ENOENT ? TIG_OK : tig__fail_errno(path);
  }
  if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return tig__fail_errno(path);
  }
  if (size > TIG_FILE_MAX) {
    fclose(file);
    return tig__fail(TIG_ERROR_REJECTED, path, "file too large");
  }
  *text = (char *)malloc((size_t)size + 1);
  if (*text == 0) {
    fclose(file);
    return tig__fail(TIG_ERROR_GIT, path, "out of memory");
  }
  *length = fread(*text, 1, (size_t)size, file);
  (*text)[*length] = '\0';
  if (ferror(file) || *length != (size_t)size) {
    fclose(file);
    free(*text);
    *text = 0;
    return tig__fail_errno(path);
  }
  fclose(file);
  return TIG_OK;
}

/* written beside the file, then renamed over it, so readers never see half a file */
static tig_status tig_write_file(const char *workdir, const char *relative, const char *text) {
  char path[TIG_PATH_MAX];
  char temporary[TIG_PATH_MAX];
  size_t base = strlen(workdir);
  size_t length = strlen(text);
  char *slash;
  FILE *file;
  tig_status result = tig__format_path(path, sizeof(path), workdir, relative, "");

  if (result == TIG_OK) {
    result = tig__format_path(temporary, sizeof(temporary), workdir, relative, ".tmp");
  }
  for (slash = strchr(path + base, '/'); result == TIG_OK && slash != 0; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    result = tig__make_directory(path);
    *slash = '/';
  }
  if (result != TIG_OK) {
    return result;
  }

  file = fopen(temporary, "wb");
  if (file == 0) {
    return tig__fail_errno(temporary);
  }
  if (fwrite(text, 1, length, file) != length) {
    fclose(file);
    remove(temporary);
    return tig__fail_errno(temporary);
  }
  if (fclose(file) != 0) {
    remove(temporary);
    return tig__fail_errno(temporary);
  }
#ifdef _WIN32
  remove(path);
#endif
  if (rename(temporary, path) != 0) {
    remove(temporary);
    return tig__fail_errno(path);
  }
  return TIG_OK;
}

static tig_status tig_remove_file(const char *workdir, const char *relative) {
  char path[TIG_PATH_MAX];
  tig_status result = tig__format_path(path, sizeof(path), workdir, relative, "");

  if (result == TIG_OK && remove(path) != 0 && errno != ENOENT) {
    result = tig__fail_errno(path);
  }
  return result;
}

/* calls add for each entry of dir that doesn't start with '.'; a missing dir has none */
static tig_status tig_each_name(const char *dir, tig_status (*add)(void *context, const char *name), void *context) {
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
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? TIG_OK
                                                                          : tig__fail(TIG_ERROR_GIT, dir, "cannot list folder");
  }
  do {
    if (entry.cFileName[0] != '.') {
      result = add(context, entry.cFileName);
    }
  } while (result == TIG_OK && FindNextFileA(find, &entry));
  FindClose(find);
#else
  DIR *handle = opendir(dir);
  struct dirent *entry;

  if (handle == 0) {
    return errno == ENOENT ? TIG_OK : tig__fail_errno(dir);
  }
  while (result == TIG_OK && (entry = readdir(handle)) != 0) {
    if (entry->d_name[0] != '.') {
      result = add(context, entry->d_name);
    }
  }
  closedir(handle);
#endif
  return result;
}

typedef struct tig_loader {
  const char *workdir;
  tig_timeline *timeline;
  char year[5];
} tig_loader;

static tig_status tig_load_event_file(void *context, const char *name) {
  tig_loader *loader = (tig_loader *)context;
  char relative[TIG_PATH_MAX];
  char path[TIG_PATH_MAX];
  char *text;
  size_t length;
  tig_status result;

  if (snprintf(relative, sizeof(relative), "events/%s/%s", loader->year, name) >= (int)sizeof(relative)) {
    return tig__fail(TIG_ERROR_BUFFER_TOO_SMALL, "path too long", name);
  }
  result = tig__format_path(path, sizeof(path), loader->workdir, relative, "");
  if (result == TIG_OK) {
    result = tig_read_file(path, &text, &length);
  }
  if (result == TIG_OK && text != 0) {
    result = tig_timeline_add_text(loader->timeline, relative, text, length);
    free(text);
  }
  return result;
}

static tig_status tig_load_year(void *context, const char *name) {
  tig_loader *loader = (tig_loader *)context;
  char dir[TIG_PATH_MAX];
  tig_status result;

  if (strlen(name) != 4 || tig_digits(name, 4) < 0) {
    return tig__fail(TIG_ERROR_REJECTED, name, "events/ holds only year folders");
  }
  memcpy(loader->year, name, 5);
  result = tig__format_path(dir, sizeof(dir), loader->workdir, "events/", name);
  return result == TIG_OK ? tig_each_name(dir, tig_load_event_file, loader) : result;
}

static tig_status tig_timeline_load_workdir(tig_repository *repository, tig_timeline *timeline) {
  tig_loader loader;
  char path[TIG_PATH_MAX];
  char *text;
  size_t length;
  tig_status result;

  memset(timeline, 0, sizeof(*timeline));
  result = tig_workdir(repository, &loader.workdir);
  if (result == TIG_OK) {
    result = tig__format_path(path, sizeof(path), loader.workdir, TIG_INITIAL_PATH, "");
  }
  if (result == TIG_OK) {
    result = tig_read_file(path, &text, &length);
  }
  if (result == TIG_OK && text != 0) {
    result = tig_timeline_set_initial_text(timeline, text, length);
    free(text);
  }
  if (result == TIG_OK) {
    loader.timeline = timeline;
    result = tig__format_path(path, sizeof(path), loader.workdir, "events", "");
  }
  if (result == TIG_OK) {
    result = tig_each_name(path, tig_load_year, &loader);
  }
  if (result != TIG_OK) {
    tig_timeline_free(timeline);
  }
  return result;
}

/* --- the index --- */

static tig_status tig_timeline_load_index(git_repository *git, git_index *index, tig_timeline *timeline) {
  size_t count = git_index_entrycount(index);
  size_t i;
  tig_status result = TIG_OK;

  memset(timeline, 0, sizeof(*timeline));
  for (i = 0; result == TIG_OK && i < count; ++i) {
    const git_index_entry *entry = git_index_get_byindex(index, i);
    int initial = strcmp(entry->path, TIG_INITIAL_PATH) == 0;
    git_blob *blob = 0;

    if (!initial && (strncmp(entry->path, "events/", 7) != 0 || tig_is_placeholder(entry->path))) {
      continue;
    }
    if (git_blob_lookup(&blob, git, &entry->id) != 0) {
      result = tig__fail_git(entry->path);
    } else if (git_blob_rawsize(blob) > TIG_FILE_MAX) {
      result = tig__fail(TIG_ERROR_REJECTED, entry->path, "file too large");
    } else if (initial) {
      result = tig_timeline_set_initial_text(timeline, (const char *)git_blob_rawcontent(blob), (size_t)git_blob_rawsize(blob));
    } else {
      result = tig_timeline_add_text(timeline, entry->path, (const char *)git_blob_rawcontent(blob),
                                     (size_t)git_blob_rawsize(blob));
    }
    git_blob_free(blob);
  }
  if (result != TIG_OK) {
    tig_timeline_free(timeline);
  }
  return result;
}

/* --- history --- */

typedef struct tig_oids {
  git_oid *items;
  size_t count;
  size_t capacity;
} tig_oids;

/* 1 if oid was already seen, 0 if it is new and now recorded, -1 when out of memory */
static int tig_oids_seen(tig_oids *oids, const git_oid *oid) {
  size_t i;

  for (i = 0; i < oids->count; ++i) {
    if (git_oid_equal(&oids->items[i], oid)) {
      return 1;
    }
  }
  if (oids->count == oids->capacity) {
    size_t capacity = oids->capacity == 0 ? 64 : oids->capacity * 2;
    git_oid *items = (git_oid *)realloc(oids->items, capacity * sizeof(*items));
    if (items == 0) {
      return -1;
    }
    oids->items = items;
    oids->capacity = capacity;
  }
  git_oid_cpy(&oids->items[oids->count++], oid);
  return 0;
}

/* the first of ids that an event in any commit reachable from HEAD has used, or -1 */
static tig_status tig_history_find_ids(git_repository *git, const char *const *ids, size_t count, long *found) {
  git_revwalk *walk = 0;
  tig_oids seen = {0, 0, 0};
  git_oid commit_id;
  tig_status result = TIG_OK;
  int error;

  *found = -1;
  if (count == 0 || git_repository_head_unborn(git) == 1) {
    return TIG_OK;
  }
  if (git_revwalk_new(&walk, git) != 0 || git_revwalk_push_head(walk) != 0) {
    git_revwalk_free(walk);
    return tig__fail_git("event history");
  }

  while (result == TIG_OK && *found < 0 && (error = git_revwalk_next(&commit_id, walk)) == 0) {
    git_commit *commit = 0;
    git_tree *root = 0;
    git_tree *events = 0;
    const git_tree_entry *entry;
    size_t i;
    size_t j;

    if (git_commit_lookup(&commit, git, &commit_id) != 0 || git_commit_tree(&root, commit) != 0) {
      result = tig__fail_git("event history");
    } else if ((entry = git_tree_entry_byname(root, "events")) != 0 && git_tree_entry_type(entry) == GIT_OBJECT_TREE) {
      int seen_events = tig_oids_seen(&seen, git_tree_entry_id(entry));

      if (seen_events < 0) {
        result = tig__fail(TIG_ERROR_GIT, "event history", "out of memory");
      } else if (seen_events == 0 && git_tree_lookup(&events, git, git_tree_entry_id(entry)) != 0) {
        result = tig__fail_git("event history");
      }
      for (i = 0; result == TIG_OK && events != 0 && *found < 0 && i < git_tree_entrycount(events); ++i) {
        const git_tree_entry *year = git_tree_entry_byindex(events, i);
        git_tree *files = 0;
        int seen_year;

        if (git_tree_entry_type(year) != GIT_OBJECT_TREE) {
          continue;
        }
        seen_year = tig_oids_seen(&seen, git_tree_entry_id(year));
        if (seen_year < 0) {
          result = tig__fail(TIG_ERROR_GIT, "event history", "out of memory");
        } else if (seen_year == 0 && git_tree_lookup(&files, git, git_tree_entry_id(year)) != 0) {
          result = tig__fail_git("event history");
        }
        for (j = 0; result == TIG_OK && files != 0 && *found < 0 && j < git_tree_entrycount(files); ++j) {
          char path[TIG_PATH_MAX];
          char id[TIG_EVENT_ID_MAX + 1];
          size_t k;

          snprintf(path, sizeof(path), "events/%s/%s", git_tree_entry_name(year),
                   git_tree_entry_name(git_tree_entry_byindex(files, j)));
          for (k = 0; tig_event_path_id(path, id) && k < count; ++k) {
            if (strcmp(id, ids[k]) == 0) {
              *found = (long)k;
              break;
            }
          }
        }
        git_tree_free(files);
      }
    }

    git_tree_free(events);
    git_tree_free(root);
    git_commit_free(commit);
  }
  if (result == TIG_OK && *found < 0 && error != GIT_ITEROVER) {
    result = tig__fail_git("event history");
  }

  free(seen.items);
  git_revwalk_free(walk);
  return result;
}

/* --- checks shared by the API and commits --- */

static tig_status tig_check_not_future(tig_repository *repository, const tig_entry *entry) {
  int64_t now;
  int64_t allowed;
  tig_status result = tig_clock_now(repository, &now);

  if (result != TIG_OK) {
    return result;
  }
  allowed = now + (entry->time.precision == TIG_PRECISION_INSTANT ? TIG_CLOCK_SKEW_MS : TIG_DATE_AHEAD_MS);
  if (entry->time.start_ms > allowed) {
    return tig_reject(entry->path, "time", "is in the future");
  }
  return TIG_OK;
}

static tig_status tig_check_evidence_list(git_index *index, const tig_node *evidence, const char *context) {
  size_t i;

  for (i = 0; i < tig_node_count(evidence); ++i) {
    const char *path = tig_node_text(tig_node_at(evidence, i));
    if (git_index_get_bypath(index, path + 1, 0) == 0) {
      return tig_reject(context, path, "is not in this commit or an earlier one");
    }
  }
  return TIG_OK;
}

/* every evidence file an event lists is committed, or staged for this commit */
static tig_status tig_check_evidence_present(git_index *index, const tig_entry *entry) {
  const tig_node *actions = tig_node_get(entry->event, "actions");
  tig_status result = tig_check_evidence_list(index, tig_node_get(entry->event, "evidence"), entry->path);
  size_t i;

  for (i = 0; result == TIG_OK && i < tig_node_count(actions); ++i) {
    result = tig_check_evidence_list(index, tig_node_get(tig_node_at(actions, i), "evidence"), entry->path);
  }
  return result;
}

static tig_status tig_check_written(tig_repository *repository, const tig_entry *entry) {
  git_index *index = 0;
  tig_status result = tig_check_not_future(repository, entry);

  if (result == TIG_OK && git_repository_index(&index, repository->git) != 0) {
    result = tig__fail_git(entry->path);
  }
  if (result == TIG_OK) {
    result = tig_check_evidence_present(index, entry);
  }
  git_index_free(index);
  return result;
}

static tig_status tig_render_to(const char *workdir, const char *relative, const tig_node *node) {
  char *text;
  tig_status result = tig_node_render(node, &text);

  if (result == TIG_OK) {
    result = tig_write_file(workdir, relative, text);
    tig_free(text);
  }
  return result;
}

/* --- public API --- */

tig_status tig_state_write_initial(tig_repository *repository, const tig_node *initial) {
  tig_timeline timeline;
  const char *workdir;
  tig_status result;

  if (initial == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_state_write_initial", "initial is required");
  }
  result = tig_check_initial(initial, TIG_INITIAL_PATH);
  if (result == TIG_OK) {
    result = tig_workdir(repository, &workdir);
  }
  if (result == TIG_OK) {
    result = tig_timeline_load_workdir(repository, &timeline);
  }
  if (result != TIG_OK) {
    return result;
  }

  tig_node_free(timeline.initial);
  timeline.initial = tig_node_copy(initial);
  if (timeline.initial == 0) {
    result = tig__fail(TIG_ERROR_GIT, "tig_state_write_initial", "out of memory");
  }
  if (result == TIG_OK) {
    result = tig_timeline_check(&timeline);
  }
  if (result == TIG_OK) {
    result = tig_render_to(workdir, TIG_INITIAL_PATH, initial);
  }
  tig_timeline_free(&timeline);
  return result;
}

tig_status tig_state_replay(tig_repository *repository, int64_t unix_ms, tig_node **state) {
  tig_timeline timeline;
  tig_status result;

  if (state == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_state_replay", "state is required");
  }
  *state = 0;
  result = tig_timeline_load_workdir(repository, &timeline);
  if (result == TIG_OK) {
    result = tig_timeline_replay(&timeline, unix_ms, state);
    tig_timeline_free(&timeline);
  }
  return result;
}

static tig_status tig_random_id(char *id) {
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  unsigned char bytes[64];
  size_t length = 0;
  size_t i;
  tig_status result = tig__random_bytes(bytes, sizeof(bytes));

  /* drops values past the largest multiple of 36, so every character is equally likely */
  for (i = 0; result == TIG_OK && i < sizeof(bytes) && length < TIG_EVENT_ID_LENGTH; ++i) {
    if (bytes[i] < 252) {
      id[length++] = alphabet[bytes[i] % 36];
    }
  }
  id[length] = '\0';
  if (result == TIG_OK && length < TIG_EVENT_ID_LENGTH) {
    result = tig__fail(TIG_ERROR_GIT, "event ID", "not enough random numbers");
  }
  return result;
}

/* checks and writes a new or corrected event; on success the timeline owns event */
static tig_status tig_write_event(tig_repository *repository, tig_timeline *timeline, const char *id,
                                  const tig_node *event, const char *previous_path) {
  const char *workdir;
  char path[TIG_PATH_MAX];
  tig_entry *entry;
  tig_node *copy;
  tig_time time;
  tig_status result = tig_workdir(repository, &workdir);

  if (result == TIG_OK) {
    result = tig_check_event(event, "event", &time);
  }
  if (result != TIG_OK) {
    return result;
  }
  tig_event_path(path, sizeof(path), &time, id);
  copy = tig_node_copy(event);
  if (copy == 0) {
    return tig__fail(TIG_ERROR_GIT, path, "out of memory");
  }

  result = tig_timeline_add(timeline, path, copy);
  if (result == TIG_OK) {
    entry = tig_timeline_find(timeline, id);
    result = tig_check_written(repository, entry);
  }
  if (result == TIG_OK) {
    result = tig_timeline_check(timeline);
  }
  if (result == TIG_OK) {
    result = tig_render_to(workdir, path, event);
  }
  if (result == TIG_OK && previous_path != 0 && strcmp(previous_path, path) != 0) {
    result = tig_remove_file(workdir, previous_path);
  }
  return result;
}

tig_status tig_event_add(tig_repository *repository, const tig_node *event, char *id, size_t id_size) {
  tig_timeline timeline;
  char chosen[TIG_EVENT_ID_MAX + 1];
  const char *candidates[1];
  long found = 0;
  int attempts;
  tig_status result;

  if (repository == 0 || event == 0 || id == 0 || id_size < TIG_EVENT_ID_LENGTH + 1) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_event_add", "repository, event and room for the ID are required");
  }
  id[0] = '\0';
  result = tig_timeline_load_workdir(repository, &timeline);
  if (result != TIG_OK) {
    return result;
  }

  /* a new ID must never have been used, in the working tree or in history */
  for (attempts = 0; result == TIG_OK && found >= 0 && attempts < 8; ++attempts) {
    result = tig_random_id(chosen);
    candidates[0] = chosen;
    found = tig_timeline_find(&timeline, chosen) != 0 ? 0 : -1;
    if (result == TIG_OK && found < 0) {
      result = tig_history_find_ids(repository->git, candidates, 1, &found);
    }
  }
  if (result == TIG_OK && found >= 0) {
    result = tig__fail(TIG_ERROR_GIT, "tig_event_add", "cannot choose an unused event ID");
  }
  if (result == TIG_OK) {
    result = tig_write_event(repository, &timeline, chosen, event, 0);
  }
  if (result == TIG_OK) {
    memcpy(id, chosen, strlen(chosen) + 1);
  }
  tig_timeline_free(&timeline);
  return result;
}

tig_status tig_event_correct(tig_repository *repository, const char *id, const tig_node *event) {
  tig_timeline timeline;
  tig_entry *entry;
  char *previous_path = 0;
  tig_status result;

  if (repository == 0 || id == 0 || event == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_event_correct", "repository, id and event are required");
  }
  result = tig_timeline_load_workdir(repository, &timeline);
  if (result != TIG_OK) {
    return result;
  }
  entry = tig_timeline_find(&timeline, id);
  if (entry == 0) {
    result = tig__fail(TIG_ERROR_NOT_FOUND, id, "no such event");
  } else if ((previous_path = tig_copy(entry->path)) == 0) {
    result = tig__fail(TIG_ERROR_GIT, id, "out of memory");
  } else {
    /* keeps its ID, and moves if its time changed */
    tig_timeline_remove(&timeline, entry);
    result = tig_write_event(repository, &timeline, id, event, previous_path);
  }
  free(previous_path);
  tig_timeline_free(&timeline);
  return result;
}

tig_status tig_event_delete(tig_repository *repository, const char *id) {
  tig_timeline timeline;
  tig_entry *entry;
  const char *workdir;
  tig_status result;

  if (repository == 0 || id == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_event_delete", "repository and id are required");
  }
  result = tig_workdir(repository, &workdir);
  if (result == TIG_OK) {
    result = tig_timeline_load_workdir(repository, &timeline);
  }
  if (result != TIG_OK) {
    return result;
  }
  entry = tig_timeline_find(&timeline, id);
  if (entry == 0) {
    result = tig__fail(TIG_ERROR_NOT_FOUND, id, "no such event");
  } else {
    char path[TIG_PATH_MAX];

    snprintf(path, sizeof(path), "%s", entry->path);
    tig_timeline_remove(&timeline, entry);
    result = tig_timeline_check(&timeline);
    if (result == TIG_OK) {
      result = tig_remove_file(workdir, path);
    }
  }
  tig_timeline_free(&timeline);
  return result;
}

tig_status tig_event_read(tig_repository *repository, const char *id, tig_node **event) {
  tig_timeline timeline;
  tig_entry *entry;
  tig_status result;

  if (id == 0 || event == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_event_read", "id and event are required");
  }
  *event = 0;
  result = tig_timeline_load_workdir(repository, &timeline);
  if (result != TIG_OK) {
    return result;
  }
  entry = tig_timeline_find(&timeline, id);
  if (entry == 0) {
    result = tig__fail(TIG_ERROR_NOT_FOUND, id, "no such event");
  } else if ((*event = tig_node_copy(entry->event)) == 0) {
    result = tig__fail(TIG_ERROR_GIT, id, "out of memory");
  }
  tig_timeline_free(&timeline);
  return result;
}

tig_status tig_events_each(tig_repository *repository, tig_event_visitor visit, void *context) {
  tig_timeline timeline;
  tig_status result;
  size_t i;

  if (visit == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_events_each", "visit is required");
  }
  result = tig_timeline_load_workdir(repository, &timeline);
  if (result != TIG_OK) {
    return result;
  }
  tig_timeline_sort(&timeline);
  for (i = 0; result == TIG_OK && i < timeline.count; ++i) {
    result = visit(context, timeline.entries[i].id, timeline.entries[i].event);
  }
  tig_timeline_free(&timeline);
  return result;
}

/* --- commits --- */

int tig__is_event_path(const char *path) {
  return strncmp(path, "events/", 7) == 0 || strncmp(path, "state/", 6) == 0;
}

typedef struct tig_ids {
  char (*items)[TIG_EVENT_ID_MAX + 1];
  size_t count;
} tig_ids;

static tig_status tig_ids_add(tig_ids *ids, const char *id) {
  char (*items)[TIG_EVENT_ID_MAX + 1] = realloc(ids->items, (ids->count + 1) * sizeof(*items));

  if (items == 0) {
    return tig__fail(TIG_ERROR_GIT, "commit", "out of memory");
  }
  ids->items = items;
  memcpy(ids->items[ids->count++], id, strlen(id) + 1);
  return TIG_OK;
}

static int tig_ids_contain(const tig_ids *ids, const char *id) {
  size_t i;

  for (i = 0; i < ids->count; ++i) {
    if (strcmp(ids->items[i], id) == 0) {
      return 1;
    }
  }
  return 0;
}

tig_status tig__events_check_commit(tig_repository *repository, git_index *index, git_diff *diff, int *changed) {
  size_t count = git_diff_num_deltas(diff);
  tig_ids added = {0, 0};
  tig_ids deleted = {0, 0};
  const char **unmoved = 0;
  size_t unmoved_count = 0;
  tig_timeline timeline;
  tig_status result = TIG_OK;
  long found;
  size_t i;

  *changed = 0;
  memset(&timeline, 0, sizeof(timeline));

  for (i = 0; result == TIG_OK && i < count; ++i) {
    const git_diff_delta *delta = git_diff_get_delta(diff, i);
    const char *path = delta->status == GIT_DELTA_DELETED ? delta->old_file.path : delta->new_file.path;
    char id[TIG_EVENT_ID_MAX + 1];

    if (!tig__is_event_path(path)) {
      continue;
    }
    *changed = 1;
    if (delta->status != GIT_DELTA_ADDED && delta->status != GIT_DELTA_MODIFIED && delta->status != GIT_DELTA_DELETED) {
      result = tig__fail(TIG_ERROR_REJECTED, path, "events and state can be added, changed or deleted, but not replaced");
    } else if (tig_is_placeholder(path) || strcmp(path, TIG_INITIAL_PATH) == 0) {
      /* checked with the rest of the timeline below */
    } else if (strncmp(path, "state/", 6) == 0) {
      result = tig__fail(TIG_ERROR_REJECTED, path, "state/ holds only initial.yaml");
    } else if (!tig_event_path_id(path, id)) {
      result = tig__fail(TIG_ERROR_REJECTED, path, "event files are events/YYYY/<time>.<id>.yaml");
    } else if (delta->status == GIT_DELTA_ADDED) {
      result = tig_ids_add(&added, id);
    } else if (delta->status == GIT_DELTA_DELETED) {
      result = tig_ids_add(&deleted, id);
    }
  }
  if (result != TIG_OK || !*changed) {
    goto done;
  }

  /* every event is well formed, at its own path, with a unique ID, and replay succeeds */
  result = tig_timeline_load_index(repository->git, index, &timeline);
  if (result == TIG_OK) {
    result = tig_timeline_check(&timeline);
  }

  for (i = 0; result == TIG_OK && i < count; ++i) {
    const git_diff_delta *delta = git_diff_get_delta(diff, i);
    const tig_entry *entry;

    if (delta->status == GIT_DELTA_DELETED || !tig__is_event_path(delta->new_file.path) ||
        (entry = tig_timeline_find_path(&timeline, delta->new_file.path)) == 0) {
      continue;
    }
    result = tig_check_not_future(repository, entry);
    if (result == TIG_OK) {
      result = tig_check_evidence_present(index, entry);
    }
  }

  /* a new ID must never have been used before, unless the event is moving to a new time */
  if (result == TIG_OK && added.count > 0) {
    unmoved = (const char **)malloc(added.count * sizeof(*unmoved));
    if (unmoved == 0) {
      result = tig__fail(TIG_ERROR_GIT, "commit", "out of memory");
    }
    for (i = 0; result == TIG_OK && i < added.count; ++i) {
      if (!tig_ids_contain(&deleted, added.items[i])) {
        unmoved[unmoved_count++] = added.items[i];
      }
    }
  }
  if (result == TIG_OK) {
    result = tig_history_find_ids(repository->git, unmoved, unmoved_count, &found);
    if (result == TIG_OK && found >= 0) {
      result = tig_reject(unmoved[found], "event ID", "was used by an earlier event, and IDs are never reused");
    }
  }

done:
  free(unmoved);
  free(added.items);
  free(deleted.items);
  tig_timeline_free(&timeline);
  return result;
}
