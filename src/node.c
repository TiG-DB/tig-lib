#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yaml.h>

/* YAML values for events and state (see tig-docs/EVENTS.md). Parsing goes through libyaml; rendering is done here, so
   that files look like the examples in the spec. */

#define TIG_NODE_DEPTH_MAX 64
#define TIG_LINE_WIDTH 120

struct tig_node {
  tig_node_type type;
  char *text;    /* scalars */
  int is_string; /* scalars: 0 for a plain scalar that YAML reads as a number, boolean or null */
  size_t count;
  size_t capacity;
  char **keys;   /* maps */
  tig_node **items;
};

static int tig_is_digit(char c) {
  return c >= '0' && c <= '9';
}

static int tig_is_letter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char *tig_copy_text(const char *text, size_t length) {
  char *copy = (char *)malloc(length + 1);

  if (copy != 0) {
    memcpy(copy, text, length);
    copy[length] = '\0';
  }
  return copy;
}

void tig_free(void *memory) {
  free(memory);
}

/* --- YAML 1.2 core schema --- */

static int tig_is_one_of(const char *text, const char *const *words, size_t count) {
  size_t i;

  for (i = 0; i < count; ++i) {
    if (strcmp(text, words[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int tig_is_integer_text(const char *text) {
  const char *c = text;

  if (c[0] == '0' && (c[1] == 'o' || c[1] == 'x') && c[2] != '\0') {
    for (c += 2; *c != '\0'; ++c) {
      int octal = *c >= '0' && *c <= '7';
      int hex = tig_is_digit(*c) || (*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F');
      if (text[1] == 'o' ? !octal : !hex) {
        return 0;
      }
    }
    return 1;
  }
  if (*c == '-' || *c == '+') {
    ++c;
  }
  if (!tig_is_digit(*c)) {
    return 0;
  }
  while (tig_is_digit(*c)) {
    ++c;
  }
  return *c == '\0';
}

static int tig_is_number_text(const char *text) {
  static const char *const special[] = {".inf", ".Inf", ".INF", "-.inf", "-.Inf", "-.INF", "+.inf",
                                        "+.Inf", "+.INF", ".nan", ".NaN", ".NAN"};
  const char *c = text;
  int digits = 0;

  if (tig_is_integer_text(text) || tig_is_one_of(text, special, sizeof(special) / sizeof(special[0]))) {
    return 1;
  }
  if (*c == '-' || *c == '+') {
    ++c;
  }
  while (tig_is_digit(*c)) {
    ++c;
    ++digits;
  }
  if (*c == '.') {
    ++c;
    while (tig_is_digit(*c)) {
      ++c;
      ++digits;
    }
  }
  if (digits == 0) {
    return 0;
  }
  if (*c == 'e' || *c == 'E') {
    ++c;
    if (*c == '-' || *c == '+') {
      ++c;
    }
    if (!tig_is_digit(*c)) {
      return 0;
    }
    while (tig_is_digit(*c)) {
      ++c;
    }
  }
  return *c == '\0';
}

/* whether a plain scalar is a string, rather than a null, boolean or number */
static int tig_plain_is_string(const char *text) {
  static const char *const values[] = {"", "~", "null", "Null", "NULL", "true", "True", "TRUE", "false", "False", "FALSE"};

  return !tig_is_one_of(text, values, sizeof(values) / sizeof(values[0])) && !tig_is_number_text(text);
}

/* --- construction --- */

static tig_node *tig_node_new(tig_node_type type) {
  tig_node *node = (tig_node *)calloc(1, sizeof(*node));

  if (node != 0) {
    node->type = type;
  }
  return node;
}

static tig_node *tig_node_scalar(const char *text, size_t length, int is_string) {
  tig_node *node = tig_node_new(TIG_NODE_SCALAR);

  if (node == 0) {
    return 0;
  }
  node->text = tig_copy_text(text, length);
  if (node->text == 0) {
    free(node);
    return 0;
  }
  node->is_string = is_string;
  return node;
}

tig_node *tig_node_string(const char *text) {
  return text != 0 ? tig_node_scalar(text, strlen(text), 1) : 0;
}

tig_node *tig_node_integer(int64_t value) {
  char text[32];

  snprintf(text, sizeof(text), "%lld", (long long)value);
  return tig_node_scalar(text, strlen(text), 0);
}

tig_node *tig_node_number(const char *text) {
  return text != 0 && tig_is_number_text(text) ? tig_node_scalar(text, strlen(text), 0) : 0;
}

tig_node *tig_node_boolean(int value) {
  return tig_node_scalar(value ? "true" : "false", value ? 4 : 5, 0);
}

tig_node *tig_node_list(void) {
  return tig_node_new(TIG_NODE_LIST);
}

tig_node *tig_node_map(void) {
  return tig_node_new(TIG_NODE_MAP);
}

void tig_node_free(tig_node *node) {
  size_t i;

  if (node == 0) {
    return;
  }
  for (i = 0; i < node->count; ++i) {
    if (node->keys != 0) {
      free(node->keys[i]);
    }
    tig_node_free(node->items[i]);
  }
  free(node->keys);
  free(node->items);
  free(node->text);
  free(node);
}

static int tig_node_reserve(tig_node *node) {
  size_t capacity;
  tig_node **items;

  if (node->count < node->capacity) {
    return 1;
  }
  capacity = node->capacity == 0 ? 4 : node->capacity * 2;
  items = (tig_node **)realloc(node->items, capacity * sizeof(*items));
  if (items == 0) {
    return 0;
  }
  node->items = items;
  if (node->type == TIG_NODE_MAP) {
    char **keys = (char **)realloc(node->keys, capacity * sizeof(*keys));
    if (keys == 0) {
      return 0;
    }
    node->keys = keys;
  }
  node->capacity = capacity;
  return 1;
}

tig_status tig_node_append(tig_node *list, tig_node *value) {
  if (list == 0 || list->type != TIG_NODE_LIST || value == 0) {
    tig_node_free(value);
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_node_append", "needs a list and a value");
  }
  if (!tig_node_reserve(list)) {
    tig_node_free(value);
    return tig__fail(TIG_ERROR_GIT, "tig_node_append", "out of memory");
  }
  list->items[list->count++] = value;
  return TIG_OK;
}

static long tig_node_find(const tig_node *map, const char *key) {
  size_t i;

  for (i = 0; i < map->count; ++i) {
    if (strcmp(map->keys[i], key) == 0) {
      return (long)i;
    }
  }
  return -1;
}

tig_status tig_node_set(tig_node *map, const char *key, tig_node *value) {
  long index;
  char *copy;

  if (map == 0 || map->type != TIG_NODE_MAP || key == 0 || value == 0) {
    tig_node_free(value);
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_node_set", "needs a map, a key and a value");
  }
  index = tig_node_find(map, key);
  if (index >= 0) {
    tig_node_free(map->items[index]);
    map->items[index] = value;
    return TIG_OK;
  }
  copy = tig_copy_text(key, strlen(key));
  if (copy == 0 || !tig_node_reserve(map)) {
    free(copy);
    tig_node_free(value);
    return tig__fail(TIG_ERROR_GIT, "tig_node_set", "out of memory");
  }
  map->keys[map->count] = copy;
  map->items[map->count] = value;
  ++map->count;
  return TIG_OK;
}

int tig_node_unset(tig_node *map, const char *key) {
  long index;

  if (map == 0 || map->type != TIG_NODE_MAP || key == 0 || (index = tig_node_find(map, key)) < 0) {
    return 0;
  }
  free(map->keys[index]);
  tig_node_free(map->items[index]);
  memmove(map->keys + index, map->keys + index + 1, (map->count - (size_t)index - 1) * sizeof(*map->keys));
  memmove(map->items + index, map->items + index + 1, (map->count - (size_t)index - 1) * sizeof(*map->items));
  --map->count;
  return 1;
}

tig_node *tig_node_copy(const tig_node *node) {
  tig_node *copy;
  size_t i;

  if (node == 0) {
    return 0;
  }
  if (node->type == TIG_NODE_SCALAR) {
    return tig_node_scalar(node->text, strlen(node->text), node->is_string);
  }
  copy = tig_node_new(node->type);
  for (i = 0; copy != 0 && i < node->count; ++i) {
    tig_node *item = tig_node_copy(node->items[i]);
    tig_status result = node->type == TIG_NODE_MAP ? tig_node_set(copy, node->keys[i], item) : tig_node_append(copy, item);

    if (item == 0 || result != TIG_OK) {
      tig_node_free(copy);
      copy = 0;
    }
  }
  return copy;
}

/* --- access --- */

tig_node_type tig_node_type_of(const tig_node *node) {
  return node != 0 ? node->type : TIG_NODE_SCALAR;
}

const char *tig_node_text(const tig_node *node) {
  return node != 0 && node->type == TIG_NODE_SCALAR ? node->text : 0;
}

int tig_node_is_string(const tig_node *node) {
  return node != 0 && node->type == TIG_NODE_SCALAR && node->is_string;
}

size_t tig_node_count(const tig_node *node) {
  return node != 0 ? node->count : 0;
}

tig_node *tig_node_at(const tig_node *node, size_t index) {
  return node != 0 && index < node->count ? node->items[index] : 0;
}

const char *tig_node_key_at(const tig_node *node, size_t index) {
  return node != 0 && node->type == TIG_NODE_MAP && index < node->count ? node->keys[index] : 0;
}

tig_node *tig_node_get(const tig_node *map, const char *key) {
  long index;

  if (map == 0 || map->type != TIG_NODE_MAP || key == 0) {
    return 0;
  }
  index = tig_node_find(map, key);
  return index >= 0 ? map->items[index] : 0;
}

int tig_node_equal(const tig_node *left, const tig_node *right) {
  size_t i;

  if (left == 0 || right == 0) {
    return left == right;
  }
  if (left->type != right->type || left->count != right->count) {
    return 0;
  }
  switch (left->type) {
    case TIG_NODE_SCALAR:
      return left->is_string == right->is_string && strcmp(left->text, right->text) == 0;
    case TIG_NODE_LIST:
      for (i = 0; i < left->count; ++i) {
        if (!tig_node_equal(left->items[i], right->items[i])) {
          return 0;
        }
      }
      return 1;
    default:
      for (i = 0; i < left->count; ++i) {
        if (!tig_node_equal(left->items[i], tig_node_get(right, left->keys[i]))) {
          return 0;
        }
      }
      return 1;
  }
}

/* --- parsing --- */

typedef struct tig_parser {
  yaml_parser_t yaml;
  const char *context;
} tig_parser;

static tig_status tig_parse_fail(tig_parser *parser, const char *problem, size_t line) {
  char detail[256];

  snprintf(detail, sizeof(detail), "%s (line %lu)", problem, (unsigned long)line + 1);
  return tig__fail(TIG_ERROR_REJECTED, parser->context, detail);
}

static tig_status tig_next_event(tig_parser *parser, yaml_event_t *event) {
  if (!yaml_parser_parse(&parser->yaml, event)) {
    return tig_parse_fail(parser, parser->yaml.problem != 0 ? parser->yaml.problem : "invalid YAML",
                          parser->yaml.problem_mark.line);
  }
  return TIG_OK;
}

static tig_status tig_parse_node(tig_parser *parser, yaml_event_t *event, int depth, tig_node **out);

/* the children of a sequence or mapping, up to its end event */
static tig_status tig_parse_children(tig_parser *parser, tig_node *node, int depth) {
  yaml_event_t event;
  tig_status result;

  for (;;) {
    tig_node *key = 0;
    tig_node *value = 0;

    result = tig_next_event(parser, &event);
    if (result != TIG_OK) {
      return result;
    }
    if (event.type == YAML_SEQUENCE_END_EVENT || event.type == YAML_MAPPING_END_EVENT) {
      yaml_event_delete(&event);
      return TIG_OK;
    }

    if (node->type == TIG_NODE_LIST) {
      result = tig_parse_node(parser, &event, depth + 1, &value);
      if (result == TIG_OK) {
        result = tig_node_append(node, value);
      }
      if (result != TIG_OK) {
        return result;
      }
      continue;
    }

    if (event.type != YAML_SCALAR_EVENT) {
      size_t line = event.start_mark.line;
      yaml_event_delete(&event);
      return tig_parse_fail(parser, "map keys must be scalars", line);
    }
    result = tig_parse_node(parser, &event, depth + 1, &key);
    if (result == TIG_OK && tig_node_find(node, key->text) >= 0) {
      result = tig_parse_fail(parser, "duplicate key", parser->yaml.mark.line);
    }
    if (result == TIG_OK) {
      result = tig_next_event(parser, &event);
      if (result == TIG_OK) {
        result = tig_parse_node(parser, &event, depth + 1, &value);
      }
    }
    if (result == TIG_OK) {
      result = tig_node_set(node, key->text, value);
    }
    tig_node_free(key);
    if (result != TIG_OK) {
      return result;
    }
  }
}

/* builds a node from event, which it deletes */
static tig_status tig_parse_node(tig_parser *parser, yaml_event_t *event, int depth, tig_node **out) {
  const char *tag = 0;
  size_t line = event->start_mark.line;
  tig_status result = TIG_OK;
  tig_node *node = 0;

  *out = 0;
  if (depth > TIG_NODE_DEPTH_MAX) {
    yaml_event_delete(event);
    return tig_parse_fail(parser, "nested too deeply", line);
  }

  switch (event->type) {
    case YAML_SCALAR_EVENT:
      tag = (const char *)event->data.scalar.tag;
      if (tag != 0 && strcmp(tag, YAML_STR_TAG) != 0) {
        result = tig_parse_fail(parser, "tags other than !!str are not supported", line);
        break;
      }
      node = tig_node_scalar((const char *)event->data.scalar.value, event->data.scalar.length,
                             tag != 0 || event->data.scalar.style != YAML_PLAIN_SCALAR_STYLE ||
                                 tig_plain_is_string((const char *)event->data.scalar.value));
      if (node != 0 && strlen(node->text) != event->data.scalar.length) {
        result = tig_parse_fail(parser, "scalars cannot contain NUL", line);
      }
      break;
    case YAML_SEQUENCE_START_EVENT:
    case YAML_MAPPING_START_EVENT:
      tag = event->type == YAML_SEQUENCE_START_EVENT ? (const char *)event->data.sequence_start.tag
                                                     : (const char *)event->data.mapping_start.tag;
      if (tag != 0) {
        result = tig_parse_fail(parser, "tags are not supported", line);
        break;
      }
      node = tig_node_new(event->type == YAML_SEQUENCE_START_EVENT ? TIG_NODE_LIST : TIG_NODE_MAP);
      break;
    case YAML_ALIAS_EVENT:
      result = tig_parse_fail(parser, "aliases are not supported", line);
      break;
    default:
      result = tig_parse_fail(parser, "unexpected YAML", line);
      break;
  }
  yaml_event_delete(event);

  if (result == TIG_OK && node == 0) {
    result = tig__fail(TIG_ERROR_GIT, parser->context, "out of memory");
  }
  if (result == TIG_OK && node->type != TIG_NODE_SCALAR) {
    result = tig_parse_children(parser, node, depth);
  }
  if (result != TIG_OK) {
    tig_node_free(node);
    return result;
  }
  *out = node;
  return TIG_OK;
}

tig_status tig__node_parse(const char *yaml, size_t length, const char *context, tig_node **out) {
  tig_parser parser;
  yaml_event_t event;
  tig_node *node = 0;
  tig_status result;
  int expected[] = {YAML_STREAM_START_EVENT, YAML_DOCUMENT_START_EVENT};
  size_t i;

  *out = 0;
  if (!yaml_parser_initialize(&parser.yaml)) {
    return tig__fail(TIG_ERROR_GIT, context, "out of memory");
  }
  parser.context = context;
  yaml_parser_set_input_string(&parser.yaml, (const unsigned char *)yaml, length);
  yaml_parser_set_encoding(&parser.yaml, YAML_UTF8_ENCODING);

  result = TIG_OK;
  for (i = 0; result == TIG_OK && i < 2; ++i) {
    result = tig_next_event(&parser, &event);
    if (result == TIG_OK && (int)event.type != expected[i]) {
      result = tig_parse_fail(&parser, event.type == YAML_STREAM_END_EVENT ? "empty document" : "expected a document",
                              event.start_mark.line);
    }
    if (result == TIG_OK) {
      yaml_event_delete(&event);
    }
  }
  if (result == TIG_OK) {
    result = tig_next_event(&parser, &event);
  }
  if (result == TIG_OK) {
    result = tig_parse_node(&parser, &event, 0, &node);
  }
  /* exactly one document */
  if (result == TIG_OK) {
    result = tig_next_event(&parser, &event);
    if (result == TIG_OK) {
      int ended = event.type == YAML_DOCUMENT_END_EVENT;
      yaml_event_delete(&event);
      if (ended) {
        result = tig_next_event(&parser, &event);
        if (result == TIG_OK) {
          ended = event.type == YAML_STREAM_END_EVENT;
          yaml_event_delete(&event);
        }
      }
      if (result == TIG_OK && !ended) {
        result = tig_parse_fail(&parser, "a file holds one YAML document", parser.yaml.mark.line);
      }
    }
  }

  yaml_parser_delete(&parser.yaml);
  if (result != TIG_OK) {
    tig_node_free(node);
    return result;
  }
  *out = node;
  return TIG_OK;
}

tig_status tig_node_parse(const char *yaml, size_t length, tig_node **out) {
  if (yaml == 0 || out == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_node_parse", "yaml and out are required");
  }
  return tig__node_parse(yaml, length, "YAML", out);
}

/* --- rendering --- */

typedef struct tig_buffer {
  char *data;
  size_t length;
  size_t capacity;
  int failed;
} tig_buffer;

static void tig_append(tig_buffer *buffer, const char *text, size_t length) {
  if (buffer->failed) {
    return;
  }
  if (buffer->length + length + 1 > buffer->capacity) {
    size_t capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
    char *data;

    while (buffer->length + length + 1 > capacity) {
      capacity *= 2;
    }
    data = (char *)realloc(buffer->data, capacity);
    if (data == 0) {
      buffer->failed = 1;
      return;
    }
    buffer->data = data;
    buffer->capacity = capacity;
  }
  memcpy(buffer->data + buffer->length, text, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
}

static void tig_append_string(tig_buffer *buffer, const char *text) {
  tig_append(buffer, text, strlen(text));
}

static void tig_append_indent(tig_buffer *buffer, int indent) {
  int i;

  for (i = 0; i < indent; ++i) {
    tig_append(buffer, " ", 1);
  }
}

/* YAML 1.1 readers also take these as booleans or null */
static int tig_is_reserved_word(const char *text) {
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

/* Starting with a digit, YAML 1.1 readers may take it as a number or a timestamp, e.g. 1.07, 2026-11-10 or 0x1f,
   unless it has a letter that no number has: so 6.8.0-48-generic can be plain. */
static int tig_is_plain_with_digit(const char *text) {
  size_t i;

  if (text[0] == '0' && strchr("xXoObB", text[1]) != 0 && text[1] != '\0') {
    return 0;
  }
  for (i = 0; text[i] != '\0'; ++i) {
    if (tig_is_letter(text[i]) && strchr("eE", text[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

/* a string that reads back as the same string without quotes, in block or flow context */
static int tig_can_be_plain(const char *text) {
  size_t length = strlen(text);
  size_t i;

  if (length == 0 || !(tig_is_letter(text[0]) || text[0] == '/' || (tig_is_digit(text[0]) && tig_is_plain_with_digit(text))) ||
      text[length - 1] == ' ' || tig_is_reserved_word(text) || !tig_plain_is_string(text)) {
    return 0;
  }
  for (i = 0; i < length; ++i) {
    if (!(tig_is_letter(text[i]) || tig_is_digit(text[i]) || strchr(" _./()+-@", text[i]) != 0)) {
      return 0;
    }
  }
  return 1;
}

static void tig_append_quoted(tig_buffer *buffer, const char *text) {
  const unsigned char *c;

  tig_append(buffer, "\"", 1);
  for (c = (const unsigned char *)text; *c != '\0'; ++c) {
    char escape[8];

    switch (*c) {
      case '"':
        tig_append(buffer, "\\\"", 2);
        break;
      case '\\':
        tig_append(buffer, "\\\\", 2);
        break;
      case '\n':
        tig_append(buffer, "\\n", 2);
        break;
      case '\t':
        tig_append(buffer, "\\t", 2);
        break;
      default:
        if (*c < 0x20 || *c == 0x7f) {
          snprintf(escape, sizeof(escape), "\\x%02x", *c);
          tig_append_string(buffer, escape);
        } else {
          tig_append(buffer, (const char *)c, 1);
        }
        break;
    }
  }
  tig_append(buffer, "\"", 1);
}

static void tig_append_text(tig_buffer *buffer, const char *text) {
  if (tig_can_be_plain(text)) {
    tig_append_string(buffer, text);
  } else {
    tig_append_quoted(buffer, text);
  }
}

static void tig_append_scalar(tig_buffer *buffer, const tig_node *node) {
  if (node->is_string) {
    tig_append_text(buffer, node->text);
  } else {
    tig_append_string(buffer, node->text[0] != '\0' ? node->text : "null");
  }
}

/* lists of maps, such as actions and changes, and maps of maps, such as items, are always written in block style */
static int tig_is_flowable(const tig_node *node) {
  size_t i;
  int all_maps = node->type == TIG_NODE_MAP && node->count > 0;

  for (i = 0; all_maps && i < node->count; ++i) {
    all_maps = node->items[i]->type == TIG_NODE_MAP && node->items[i]->count > 0;
  }
  if (all_maps) {
    return 0;
  }
  for (i = 0; i < node->count; ++i) {
    if ((node->type == TIG_NODE_LIST && node->items[i]->type == TIG_NODE_MAP) || !tig_is_flowable(node->items[i])) {
      return 0;
    }
  }
  return 1;
}

static void tig_append_flow(tig_buffer *buffer, const tig_node *node) {
  size_t i;

  if (node->type == TIG_NODE_SCALAR) {
    tig_append_scalar(buffer, node);
    return;
  }
  if (node->count == 0) {
    tig_append_string(buffer, node->type == TIG_NODE_MAP ? "{}" : "[]");
    return;
  }
  tig_append_string(buffer, node->type == TIG_NODE_MAP ? "{ " : "[");
  for (i = 0; i < node->count; ++i) {
    if (i > 0) {
      tig_append_string(buffer, ", ");
    }
    if (node->type == TIG_NODE_MAP) {
      tig_append_text(buffer, node->keys[i]);
      tig_append_string(buffer, ": ");
    }
    tig_append_flow(buffer, node->items[i]);
  }
  tig_append_string(buffer, node->type == TIG_NODE_MAP ? " }" : "]");
}

/* the flow form of node, if it fits on a line that already has used characters */
static int tig_fits_flow(const tig_node *node, size_t used, tig_buffer *flow) {
  if (node->type == TIG_NODE_SCALAR || node->count == 0) {
    tig_append_flow(flow, node);
    return 1;
  }
  if (!tig_is_flowable(node)) {
    return 0;
  }
  tig_append_flow(flow, node);
  return !flow->failed && used + flow->length <= TIG_LINE_WIDTH;
}

static void tig_append_block(tig_buffer *buffer, const tig_node *node, int indent, int first_indented);

/* after "key:" or "-": the value on the same line, or a block below */
static void tig_append_value(tig_buffer *buffer, const tig_node *value, int indent, size_t used) {
  tig_buffer flow = {0, 0, 0, 0};

  if (tig_fits_flow(value, used + 1, &flow)) {
    tig_append(buffer, " ", 1);
    tig_append(buffer, flow.data, flow.length);
    tig_append(buffer, "\n", 1);
  } else {
    tig_append(buffer, "\n", 1);
    tig_append_block(buffer, value, indent + 2, 1);
  }
  buffer->failed |= flow.failed;
  free(flow.data);
}

static void tig_append_block(tig_buffer *buffer, const tig_node *node, int indent, int first_indented) {
  size_t i;

  for (i = 0; i < node->count; ++i) {
    /* where the line would start if it had been indented here */
    size_t line_start = buffer->length - (i > 0 || first_indented ? 0 : (size_t)indent);

    if (i > 0 || first_indented) {
      tig_append_indent(buffer, indent);
    }
    if (node->type == TIG_NODE_MAP) {
      tig_append_text(buffer, node->keys[i]);
      tig_append(buffer, ":", 1);
      tig_append_value(buffer, node->items[i], indent, buffer->length - line_start);
    } else if (node->items[i]->type == TIG_NODE_MAP && node->items[i]->count > 0) {
      /* "- key: value", with the map's other keys lined up under the first */
      tig_append(buffer, "- ", 2);
      tig_append_block(buffer, node->items[i], indent + 2, 0);
    } else {
      tig_append(buffer, "-", 1);
      tig_append_value(buffer, node->items[i], indent, buffer->length - line_start);
    }
  }
}

tig_status tig_node_render(const tig_node *node, char **out) {
  tig_buffer buffer = {0, 0, 0, 0};

  if (node == 0 || out == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_node_render", "node and out are required");
  }
  *out = 0;
  if (node->type == TIG_NODE_SCALAR || node->count == 0) {
    tig_append_flow(&buffer, node);
    tig_append(&buffer, "\n", 1);
  } else {
    tig_append_block(&buffer, node, 0, 1);
  }
  if (buffer.failed) {
    free(buffer.data);
    return tig__fail(TIG_ERROR_GIT, "tig_node_render", "out of memory");
  }
  *out = buffer.data;
  return TIG_OK;
}
