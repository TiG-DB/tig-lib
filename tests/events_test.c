#include "tig/tig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPO "tig-events-repo"

/* 2026-11-10T12:00:00Z */
#define NOW INT64_C(1794312000000)
#define HOUR INT64_C(3600000)

#define CHECK(condition)                                                                          \
  do {                                                                                            \
    if (!(condition)) {                                                                           \
      fprintf(stderr, "%s:%d: CHECK(%s) failed; last error: %s\n", __FILE__, __LINE__, #condition, \
              tig_get_last_error());                                                              \
      ++failures;                                                                                 \
    }                                                                                             \
  } while (0)

static int failures;

static int64_t fake_clock(void* context) {
  (void)context;
  return NOW;
}

static void write_file(const char* path, const char* contents) {
  FILE* file = fopen(path, "wb");

  if (file != 0) {
    fputs(contents, file);
    fclose(file);
  }
}

static int file_exists(const char* path) {
  FILE* file = fopen(path, "rb");

  if (file != 0) {
    fclose(file);
    return 1;
  }
  return 0;
}

static tig_node* parse(const char* yaml) {
  tig_node* node = 0;

  if (tig_node_parse(yaml, strlen(yaml), &node) != TIG_OK) {
    fprintf(stderr, "cannot parse: %s\n%s\n", tig_get_last_error(), yaml);
    ++failures;
  }
  return node;
}

static int renders_as(const tig_node* node, const char* expected) {
  char* text = 0;
  int same = tig_node_render(node, &text) == TIG_OK && strcmp(text, expected) == 0;

  if (!same) {
    fprintf(stderr, "rendered:\n%s\nexpected:\n%s\n", text != 0 ? text : "(nothing)", expected);
  }
  tig_free(text);
  return same;
}

/* NULL-safe, so a missing value fails a check rather than crashing */
static int same(const char* text, const char* expected) {
  return text != 0 && strcmp(text, expected) == 0;
}

/* the item's attribute as text, in state replayed to unix_ms */
static const char* attribute(tig_node* state, const char* item, const char* name) {
  return tig_node_text(tig_node_get(tig_node_get(tig_node_get(state, "items"), item), name));
}

static void test_nodes(void) {
  tig_node* map = tig_node_map();
  tig_node* list = tig_node_list();
  tig_node* parsed;
  tig_node* copy;

  /* strings are quoted when they would otherwise read back as something else */
  CHECK(tig_node_set(map, "model", tig_node_string("Example NVMe 1TB")) == TIG_OK);
  CHECK(tig_node_set(map, "version", tig_node_string("24.04")) == TIG_OK);
  CHECK(tig_node_set(map, "answer", tig_node_string("yes")) == TIG_OK);
  CHECK(tig_node_set(map, "mac", tig_node_string("00:00:5e:00:53:01")) == TIG_OK);
  CHECK(tig_node_set(map, "kernel", tig_node_string("6.8.0-48-generic")) == TIG_OK);
  CHECK(tig_node_set(map, "date", tig_node_string("2024-02-19")) == TIG_OK);
  CHECK(tig_node_set(map, "hex", tig_node_string("0x1f")) == TIG_OK);
  CHECK(tig_node_set(map, "exponent", tig_node_string("1e5")) == TIG_OK);
  CHECK(tig_node_set(map, "quote", tig_node_string("say \"hi\"\nbye")) == TIG_OK);
  CHECK(tig_node_set(map, "capacity", tig_node_integer(1000204886016)) == TIG_OK);
  CHECK(tig_node_set(map, "energy", tig_node_number("57.5")) == TIG_OK);
  CHECK(tig_node_set(map, "rotational", tig_node_boolean(0)) == TIG_OK);
  CHECK(tig_node_number("57.5.1") == 0);
  CHECK(tig_node_append(list, tig_node_string("disk/7a858f1b")) == TIG_OK);
  CHECK(tig_node_set(map, "disks", list) == TIG_OK);
  CHECK(tig_node_set(map, "nested", tig_node_map()) == TIG_OK);
  CHECK(tig_node_set(tig_node_get(map, "nested"), "system", tig_node_string("pci")) == TIG_OK);
  CHECK(tig_node_set(map, "model", tig_node_string("Example NVMe 2TB")) == TIG_OK); /* replaces, in place */
  CHECK(tig_node_count(map) == 14 && strcmp(tig_node_key_at(map, 0), "model") == 0);
  CHECK(tig_node_set(map, "bad", 0) == TIG_ERROR_INVALID_ARGUMENT);

  CHECK(renders_as(map,
                   "model: Example NVMe 2TB\n"
                   "version: \"24.04\"\n"
                   "answer: \"yes\"\n"
                   "mac: \"00:00:5e:00:53:01\"\n"
                   "kernel: 6.8.0-48-generic\n"
                   "date: \"2024-02-19\"\n"
                   "hex: \"0x1f\"\n"
                   "exponent: \"1e5\"\n"
                   "quote: \"say \\\"hi\\\"\\nbye\"\n"
                   "capacity: 1000204886016\n"
                   "energy: 57.5\n"
                   "rotational: false\n"
                   "disks: [disk/7a858f1b]\n"
                   "nested: { system: pci }\n"));

  /* and read back the same, types included */
  {
    char* text = 0;
    CHECK(tig_node_render(map, &text) == TIG_OK);
    parsed = parse(text);
    tig_free(text);
  }
  CHECK(tig_node_equal(map, parsed));
  CHECK(tig_node_is_string(tig_node_get(parsed, "version")) && !tig_node_is_string(tig_node_get(parsed, "capacity")));
  CHECK(strcmp(tig_node_text(tig_node_get(parsed, "quote")), "say \"hi\"\nbye") == 0);

  copy = tig_node_copy(parsed);
  CHECK(tig_node_equal(copy, map));
  CHECK(tig_node_set(copy, "version", tig_node_number("24.04")) == TIG_OK);
  CHECK(!tig_node_equal(copy, map)); /* a number is not the string "24.04" */
  CHECK(tig_node_unset(copy, "version") == 1 && tig_node_unset(copy, "version") == 0);
  CHECK(tig_node_count(copy) == 13);
  tig_node_free(copy);
  tig_node_free(parsed);
  tig_node_free(map);

  /* key order doesn't matter for equality */
  {
    tig_node* a = parse("{ a: 1, b: [x, y] }");
    tig_node* b = parse("b: [x, y]\na: 1\n");
    tig_node* c = parse("b: [y, x]\na: 1\n");
    CHECK(tig_node_equal(a, b) && !tig_node_equal(a, c));
    tig_node_free(a);
    tig_node_free(b);
    tig_node_free(c);
  }

  /* one plain document only */
  {
    static const char* const rejected[] = {"a: &x 1\nb: *x\n", "a: 1\n---\nb: 2\n", "a: 1\na: 2\n", "a: !!int 1\n",
                                           "a: [1\n", ""};
    size_t i;

    for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
      tig_node* node = (tig_node*)1;
      CHECK(tig_node_parse(rejected[i], strlen(rejected[i]), &node) == TIG_ERROR_REJECTED && node == 0);
    }
  }
}

static const char* const discovery =
    "time: \"2026-11-10T09:30:02Z\"\n"
    "type: discovery\n"
    "source: tig-agent 0.2.0\n"
    "actions:\n"
    "  - type: discovered\n"
    "    changes:\n"
    "      - add: memory\n"
    "        since: unknown\n"
    "        set: { capacity: 34359738368 }\n"
    "      - add: disk/7a858f1b\n"
    "        since: unknown\n"
    "        set: { model: Example NVMe 1TB, serial: S6EXAMPLE001, capacity: 1000204886016, transport: nvme, rotational: false }\n"
    "      - add: filesystem/root\n"
    "        since: \"2025-01-03T14:22:10Z\"\n"
    "        set: { type: ext4, size: 983349346304, disks: [disk/7a858f1b] }\n"
    "      - add: os\n"
    "        since: \"2025-01-03T14:22:10Z\"\n"
    "        set: { id: ubuntu, version: \"24.04\", name: Ubuntu 24.04.1 LTS, kernel: 6.8.0-48-generic }\n";

static const char* const replacement =
    "time: \"2026-11-10T11:02:14Z\"\n"
    "not_before: \"2026-11-10T10:41:00Z\"\n"
    "source: tig-agent 0.2.0\n"
    "actions:\n"
    "  - type: replaced\n"
    "    changes:\n"
    "      - remove: disk/7a858f1b\n"
    "      - add: disk/eb3f1f45\n"
    "        set: { model: Example NVMe 2TB, capacity: 2000398934016 }\n"
    "      - update: filesystem/root\n"
    "        set: { disks: [disk/eb3f1f45] }\n"
    "  - type: resized\n"
    "    changes:\n"
    "      - update: filesystem/root\n"
    "        set: { size: 1967925690368 }\n";

static tig_status count_events(void* context, const char* id, const tig_node* event) {
  (void)id;
  (void)event;
  ++*(int*)context;
  return TIG_OK;
}

typedef struct visit_order {
  char times[8][32];
  int count;
} visit_order;

static tig_status record_order(void* context, const char* id, const tig_node* event) {
  visit_order* order = (visit_order*)context;

  (void)id;
  if (order->count < 8) {
    snprintf(order->times[order->count++], 32, "%s", tig_node_text(tig_node_get(event, "time")));
  }
  return TIG_OK;
}

static void test_events(void) {
  tig_options options = {0};
  tig_repository* repository;
  tig_node* initial;
  tig_node* event;
  tig_node* state;
  char discovery_id[TIG_EVENT_ID_MAX + 1];
  char replacement_id[TIG_EVENT_ID_MAX + 1];
  char id[TIG_EVENT_ID_MAX + 1];
  char path[512];
  int count = 0;

  options.clock = fake_clock;
  CHECK(tig_init(&options) == TIG_OK);
  CHECK(tig_repository_create(REPO, 0, &repository) == TIG_OK);

  /* initial state */
  initial = parse("kind: device\nvocabulary: tig-device/1\nitems:\n  entity: { vendor: Example, serial: EXC1234567 }\n");
  CHECK(tig_state_write_initial(repository, initial) == TIG_OK);
  CHECK(file_exists(REPO "/state/initial.yaml"));
  tig_node_free(initial);
  initial = parse("items: {}\n");
  CHECK(tig_state_write_initial(repository, initial) == TIG_ERROR_REJECTED); /* needs a kind */
  tig_node_free(initial);

  /* events go at events/YYYY/<date>.<id>.yaml */
  event = parse(discovery);
  CHECK(tig_event_add(repository, event, discovery_id, sizeof(discovery_id)) == TIG_OK);
  CHECK(strlen(discovery_id) >= 6 && strlen(discovery_id) <= 16);
  snprintf(path, sizeof(path), REPO "/events/2026/2026-11-10.%s.yaml", discovery_id);
  CHECK(file_exists(path));
  tig_node_free(event);

  event = parse(replacement);
  CHECK(tig_event_add(repository, event, replacement_id, sizeof(replacement_id)) == TIG_OK);
  CHECK(strcmp(discovery_id, replacement_id) != 0);
  tig_node_free(event);

  /* replay to a time */
  CHECK(tig_state_replay(repository, TIG_TIME_LATEST, &state) == TIG_OK);
  CHECK(same(tig_node_text(tig_node_get(state, "kind")), "device"));
  CHECK(same(attribute(state, "entity", "serial"), "EXC1234567"));
  CHECK(tig_node_get(tig_node_get(state, "items"), "disk/7a858f1b") == 0);
  CHECK(same(attribute(state, "disk/eb3f1f45", "model"), "Example NVMe 2TB"));
  CHECK(same(attribute(state, "filesystem/root", "size"), "1967925690368"));
  CHECK(same(attribute(state, "filesystem/root", "type"), "ext4"));
  tig_node_free(state);

  CHECK(tig_state_replay(repository, NOW - 2 * HOUR, &state) == TIG_OK); /* 10:00, between the two */
  CHECK(same(attribute(state, "disk/7a858f1b", "model"), "Example NVMe 1TB"));
  CHECK(same(attribute(state, "filesystem/root", "size"), "983349346304"));
  tig_node_free(state);

  CHECK(tig_state_replay(repository, 0, &state) == TIG_OK); /* before any event: the initial state */
  CHECK(tig_node_count(tig_node_get(state, "items")) == 1);
  tig_node_free(state);

  /* writes that would break replay, now or for events already recorded, are rejected */
  {
    static const char* const rejected[] = {
        /* the disk is already there */
        "time: \"2026-11-10T11:30:00Z\"\nactions:\n  - type: installed\n    changes:\n      - add: disk/eb3f1f45\n",
        /* backdated, before the discovery added it */
        "time: \"2026-11-10\"\nactions:\n  - type: removed\n    changes:\n      - remove: memory\n",
        /* backdated: the later replacement removes the same disk */
        "time: \"2026-11-10T10:00:00Z\"\nactions:\n  - type: removed\n    changes:\n      - remove: disk/7a858f1b\n",
        /* in the future */
        "time: \"2026-11-10T13:00:00Z\"\nactions:\n  - type: booted\n",
        "time: \"2026-11-12\"\nactions:\n  - type: booted\n",
        /* malformed */
        "time: \"2026-11-10T11:00:00\"\nactions:\n  - type: booted\n",
        "time: \"2026-02-30\"\nactions:\n  - type: booted\n",
        "time: \"2026-11-10\"\nactions: []\n",
        "time: \"2026-11-10\"\nactions:\n  - description: no type\n",
        "time: \"2026-11-10\"\nnot_before: \"2026-11-11\"\nactions:\n  - type: booted\n",
        "time: \"2026-11-10\"\nactions:\n  - type: fitted\n    changes:\n      - add: Disk/A\n",
        "time: \"2026-11-10\"\nactions:\n  - type: fitted\n    changes:\n      - add: x\n        remove: y\n",
        "time: \"2026-11-10\"\nactions:\n  - type: fitted\n    changes:\n      - remove: memory\n        set: { a: 1 }\n",
        "time: \"2026-11-10\"\nactions:\n  - type: fitted\n    changes:\n      - add: x\n        since: soon\n",
        "time: \"2026-11-10\"\ncost: { amount: 10, currency: pounds }\nactions:\n  - type: fitted\n",
        "time: \"2026-11-10\"\nevidence: [/evidence/../secret]\nactions:\n  - type: fitted\n",
        /* evidence must be committed first */
        "time: \"2026-11-10\"\nevidence: [/evidence/invoice.pdf]\nactions:\n  - type: fitted\n",
    };
    size_t i;

    for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
      tig_status status;

      event = parse(rejected[i]);
      status = tig_event_add(repository, event, id, sizeof(id));
      if (status != TIG_ERROR_REJECTED) {
        fprintf(stderr, "not rejected (%d):\n%s\n", (int)status, rejected[i]);
        ++failures;
      }
      tig_node_free(event);
    }
  }
  CHECK(tig_events_each(repository, count_events, &count) == TIG_OK && count == 2);

  /* events that keep replay working are fine at any time, with unknown fields kept */
  event = parse("time: \"1998\"\ntype: purchase\nby: { name: Example Shop }\nactions:\n  - type: acquired\n    colour: blue\n");
  CHECK(tig_event_add(repository, event, id, sizeof(id)) == TIG_OK);
  tig_node_free(event);
  CHECK(tig_event_read(repository, id, &event) == TIG_OK);
  CHECK(same(tig_node_text(tig_node_get(tig_node_at(tig_node_get(event, "actions"), 0), "colour")), "blue"));
  tig_node_free(event);
  snprintf(path, sizeof(path), REPO "/events/1998/1998.%s.yaml", id);
  CHECK(file_exists(path));

  /* corrections keep the ID and move the file when the time changes */
  event = parse("time: \"1998-06-14\"\ntype: purchase\nactions:\n  - type: acquired\n");
  CHECK(tig_event_correct(repository, id, event) == TIG_OK);
  tig_node_free(event);
  CHECK(!file_exists(path));
  snprintf(path, sizeof(path), REPO "/events/1998/1998-06-14.%s.yaml", id);
  CHECK(file_exists(path));
  event = parse("time: \"1998\"\nactions:\n  - type: acquired\n");
  CHECK(tig_event_correct(repository, "nosuchid", event) == TIG_ERROR_NOT_FOUND);
  tig_node_free(event);

  /* replay order: by start, then coarser before finer */
  event = parse("time: \"1998-06\"\nactions:\n  - type: recorded\n");
  CHECK(tig_event_add(repository, event, id, sizeof(id)) == TIG_OK);
  tig_node_free(event);
  event = parse("time: \"1998-06-01T00:00:00Z\"\nactions:\n  - type: recorded\n");
  CHECK(tig_event_add(repository, event, id, sizeof(id)) == TIG_OK);
  tig_node_free(event);
  event = parse("time: \"1998-06-01\"\nactions:\n  - type: recorded\n");
  CHECK(tig_event_add(repository, event, id, sizeof(id)) == TIG_OK);
  tig_node_free(event);
  {
    visit_order order;
    memset(&order, 0, sizeof(order));
    CHECK(tig_events_each(repository, record_order, &order) == TIG_OK && order.count == 6);
    CHECK(same(order.times[0], "1998-06"));
    CHECK(same(order.times[1], "1998-06-01"));
    CHECK(same(order.times[2], "1998-06-01T00:00:00Z"));
    CHECK(same(order.times[3], "1998-06-14"));
    CHECK(same(order.times[4], "2026-11-10T09:30:02Z"));
  }
  CHECK(tig_event_delete(repository, id) == TIG_OK);
  CHECK(tig_event_delete(repository, id) == TIG_ERROR_NOT_FOUND);

  /* the discovery is needed by the replacement after it */
  CHECK(tig_event_delete(repository, discovery_id) == TIG_ERROR_REJECTED);

  /* commits stage events and state */
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  /* and check what was written by hand */
  snprintf(path, sizeof(path), REPO "/events/2026/2026-11-10.%s.yaml", discovery_id);
  remove(path);
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  write_file(path, discovery);
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  write_file(REPO "/events/2026/2026-11-09.handmade.yaml", "time: \"2026-11-10\"\nactions:\n  - type: booted\n");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED); /* path doesn't match the time */
  remove(REPO "/events/2026/2026-11-09.handmade.yaml");
  write_file(REPO "/events/2026/2026-11-10.handmade.yaml", "time: [2026]\n");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  write_file(REPO "/events/2026/2026-11-10.handmade.yaml", "time: \"2026-11-10\"\nactions:\n  - type: booted\n");
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);
  write_file(REPO "/events/notes.txt", "hello\n");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  remove(REPO "/events/notes.txt");

  /* an ID is never reused, even after its event is deleted */
  remove(REPO "/events/2026/2026-11-10.handmade.yaml");
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);
  write_file(REPO "/events/2026/2026-11-10.handmade.yaml", "time: \"2026-11-10\"\nactions:\n  - type: booted\n");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  remove(REPO "/events/2026/2026-11-10.handmade.yaml");

  /* but a committed event may move when its time is corrected */
  event = parse("time: \"2026-11-09T09:00:00Z\"\ntype: discovery\nactions:\n  - type: discovered\n    changes:\n      - add: memory\n      - add: disk/7a858f1b\n      - add: filesystem/root\n");
  CHECK(tig_event_correct(repository, discovery_id, event) == TIG_OK);
  tig_node_free(event);
  snprintf(path, sizeof(path), REPO "/events/2026/2026-11-09.%s.yaml", discovery_id);
  CHECK(file_exists(path));
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  /* a correction to the initial state is checked against the events */
  write_file(REPO "/state/initial.yaml", "kind: device\nitems:\n  memory: { capacity: 1 }\n");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  write_file(REPO "/state/initial.yaml", "kind: device\nvocabulary: tig-device/1\n");
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);
  CHECK(tig_state_replay(repository, TIG_TIME_LATEST, &state) == TIG_OK);
  CHECK(tig_node_get(tig_node_get(state, "items"), "entity") == 0);
  tig_node_free(state);

  tig_repository_close(repository);
  tig_shutdown();
}

int main(void) {
  test_nodes();
  test_events();

  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
  }
  return failures == 0 ? 0 : 1;
}
