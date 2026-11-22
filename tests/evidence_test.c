#include "tig/tig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPO "tig-evidence-repo"
#define SOURCE "tig-evidence-source.bin"

/* 2026-11-10T12:00:00Z */
#define NOW INT64_C(1794312000000)

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

/* whether the file holds exactly this text */
static int file_is(const char* path, const char* expected) {
  char buffer[256] = {0};
  FILE* file = fopen(path, "rb");
  size_t length;

  if (file == 0) {
    return 0;
  }
  length = fread(buffer, 1, sizeof(buffer) - 1, file);
  fclose(file);
  buffer[length] = '\0';
  return strcmp(buffer, expected) == 0;
}

static tig_node* parse(const char* yaml) {
  tig_node* node = 0;

  if (tig_node_parse(yaml, strlen(yaml), &node) != TIG_OK) {
    fprintf(stderr, "cannot parse: %s\n%s\n", tig_get_last_error(), yaml);
    ++failures;
  }
  return node;
}

/* adds an event that lists the evidence, returning the status */
static tig_status add_event_listing(tig_repository* repository, const char* path) {
  char yaml[256];
  char id[TIG_EVENT_ID_MAX + 1];
  tig_node* event;
  tig_status status;

  snprintf(yaml, sizeof(yaml), "time: \"2026-11-10\"\nevidence: [%s]\nactions:\n  - type: fitted\n", path);
  event = parse(yaml);
  status = tig_event_add(repository, event, id, sizeof(id));
  tig_node_free(event);
  return status;
}

static void test_evidence(void) {
  tig_repository* repository = 0;
  tig_repository* reopened = 0;
  char path[TIG_EVIDENCE_PATH_MAX];
  char tiny[TIG_EVIDENCE_PATH_MAX - 1];
  char long_name[TIG_EVIDENCE_NAME_MAX + 2];
  tig_options options = {0};

  options.clock = fake_clock;
  CHECK(tig_init(&options) == TIG_OK);
  CHECK(tig_repository_create(REPO, 0, &repository) == TIG_OK);
  write_file(SOURCE, "receipt");

  /* a file is copied into /evidence under the name it is given, and an event can list it straight away */
  CHECK(tig_evidence_add(repository, SOURCE, "2026-11-10-invoice.pdf", path, sizeof(path)) == TIG_OK);
  CHECK(strcmp(path, "/evidence/2026-11-10-invoice.pdf") == 0);
  CHECK(file_is(REPO "/evidence/2026-11-10-invoice.pdf", "receipt"));
  CHECK(add_event_listing(repository, path) == TIG_OK);

  /* evidence is not replaced, whether it is staged or committed */
  CHECK(tig_evidence_add(repository, SOURCE, "2026-11-10-invoice.pdf", path, sizeof(path)) == TIG_ERROR_REJECTED);
  CHECK(path[0] == '\0');
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);
  CHECK(tig_evidence_add(repository, SOURCE, "2026-11-10-invoice.pdf", path, sizeof(path)) == TIG_ERROR_REJECTED);
  CHECK(tig_repository_open(REPO, &reopened) == TIG_OK);
  CHECK(tig_evidence_add(reopened, SOURCE, "2026-11-10-invoice.pdf", path, sizeof(path)) == TIG_ERROR_REJECTED);
  tig_repository_close(reopened);
  CHECK(file_is(REPO "/evidence/2026-11-10-invoice.pdf", "receipt"));

  /* a later event can list evidence that is already committed */
  CHECK(add_event_listing(repository, "/evidence/2026-11-10-invoice.pdf") == TIG_OK);
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  /* without a name, the source's own file name is used, whatever directory it is in */
  CHECK(tig_evidence_add(repository, "./" SOURCE, 0, path, sizeof(path)) == TIG_OK);
  CHECK(strcmp(path, "/evidence/" SOURCE) == 0 && file_exists(REPO "/evidence/" SOURCE));
  CHECK(tig_repository_commit(repository, 0) == TIG_OK); /* evidence on its own is a commit */

  /* names are plain file names */
  memset(long_name, 'a', TIG_EVIDENCE_NAME_MAX + 1);
  long_name[TIG_EVIDENCE_NAME_MAX + 1] = '\0';
  {
    static const char* const rejected[] = {"",         "has space.jpg", "../escape.jpg", "a/b.jpg",    "a\\b.jpg",
                                           ".hidden",  "-dash.jpg",     "_under.jpg",    "caf\xc3\xa9.jpg"};
    size_t i;

    for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
      if (tig_evidence_add(repository, SOURCE, rejected[i], path, sizeof(path)) != TIG_ERROR_REJECTED) {
        fprintf(stderr, "name not rejected: \"%s\"\n", rejected[i]);
        ++failures;
      }
    }
    CHECK(tig_evidence_add(repository, SOURCE, long_name, path, sizeof(path)) == TIG_ERROR_REJECTED);
    long_name[TIG_EVIDENCE_NAME_MAX] = '\0';
    CHECK(tig_evidence_add(repository, SOURCE, long_name, path, sizeof(path)) == TIG_OK);
    CHECK(strlen(path) == TIG_EVIDENCE_PATH_MAX - 1);
    CHECK(tig_repository_commit(repository, 0) == TIG_OK);
  }

  /* a source that is missing or empty, and arguments that are not usable, add nothing */
  CHECK(tig_evidence_add(repository, "tig-evidence-nothing.bin", "missing.jpg", path, sizeof(path)) == TIG_ERROR_NOT_FOUND);
  CHECK(!file_exists(REPO "/evidence/missing.jpg"));
  write_file("tig-evidence-empty.bin", "");
  CHECK(tig_evidence_add(repository, "tig-evidence-empty.bin", "empty.jpg", path, sizeof(path)) == TIG_ERROR_REJECTED);
  CHECK(!file_exists(REPO "/evidence/empty.jpg"));
  remove("tig-evidence-empty.bin");
  CHECK(tig_evidence_add(repository, SOURCE, "small.jpg", tiny, sizeof(tiny)) == TIG_ERROR_INVALID_ARGUMENT);
  CHECK(tig_evidence_add(repository, 0, "small.jpg", path, sizeof(path)) == TIG_ERROR_INVALID_ARGUMENT);
  CHECK(tig_evidence_add(0, SOURCE, "small.jpg", path, sizeof(path)) == TIG_ERROR_INVALID_ARGUMENT);
  CHECK(!file_exists(REPO "/evidence/small.jpg"));

  /* a file that is only on disk isn't evidence, so an event can't list it and a commit doesn't take it */
  write_file(REPO "/evidence/dropped.pdf", "by hand");
  CHECK(add_event_listing(repository, "/evidence/dropped.pdf") == TIG_ERROR_REJECTED);
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);
  CHECK(add_event_listing(repository, "/evidence/dropped.pdf") == TIG_ERROR_REJECTED);
  CHECK(tig_evidence_add(repository, SOURCE, "dropped.pdf", path, sizeof(path)) == TIG_ERROR_REJECTED);
  remove(REPO "/evidence/dropped.pdf");

  /* an event can't list evidence that isn't there, or paths outside /evidence */
  CHECK(add_event_listing(repository, "/evidence/never-added.jpg") == TIG_ERROR_REJECTED);
  CHECK(add_event_listing(repository, "/elsewhere/2026-11-10-invoice.pdf") == TIG_ERROR_REJECTED);

  /* once committed, evidence is never changed... */
  write_file(REPO "/evidence/2026-11-10-invoice.pdf", "tampered");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  CHECK(file_is(REPO "/evidence/2026-11-10-invoice.pdf", "tampered")); /* the file is left for its owner to put back */
  write_file(REPO "/evidence/2026-11-10-invoice.pdf", "receipt");
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  /* ...or removed */
  remove(REPO "/evidence/2026-11-10-invoice.pdf");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  write_file(REPO "/evidence/2026-11-10-invoice.pdf", "receipt");
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  /* rejected commits leave the evidence that was staged before them staged */
  CHECK(tig_evidence_add(repository, SOURCE, "later.jpg", path, sizeof(path)) == TIG_OK);
  write_file(REPO "/evidence/2026-11-10-invoice.pdf", "tampered");
  CHECK(tig_repository_commit(repository, 0) == TIG_ERROR_REJECTED);
  write_file(REPO "/evidence/2026-11-10-invoice.pdf", "receipt");
  CHECK(add_event_listing(repository, "/evidence/later.jpg") == TIG_OK);
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  tig_repository_close(repository);
  remove(SOURCE);
  tig_shutdown();
}

int main(void) {
  test_evidence();

  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
  }
  return failures == 0 ? 0 : 1;
}
