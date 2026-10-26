#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define TIG_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/time.h>
#define TIG_MKDIR(path) mkdir((path), 0777)
#endif

#ifndef TIG_VERSION_STRING
#define TIG_VERSION_STRING "0.0.0"
#endif

/* library-wide state; not thread safe */
static char *tig_storage_base_path;
static char tig_cloned_path[TIG_PATH_MAX];
static char tig_last_error[512];
static tig_clock_source tig_clock;
static void *tig_clock_context;

static void tig_set_error(const char *context, const char *detail) {
  if (detail != 0 && detail[0] != '\0') {
    snprintf(tig_last_error, sizeof(tig_last_error), "%s: %s", context, detail);
  } else {
    snprintf(tig_last_error, sizeof(tig_last_error), "%s", context);
  }
}

tig_status tig__fail(tig_status status, const char *context, const char *detail) {
  tig_set_error(context, detail);
  return status;
}

tig_status tig__fail_git(const char *context) {
  const git_error *error = git_error_last();
  return tig__fail(TIG_ERROR_GIT, context, error != 0 ? error->message : 0);
}

tig_status tig__fail_errno(const char *context) {
  return tig__fail(TIG_ERROR_GIT, context, strerror(errno));
}

static char *tig_strdup(const char *value) {
  size_t length = strlen(value);
  char *copy = (char *)malloc(length + 1);

  if (copy != 0) {
    memcpy(copy, value, length + 1);
  }
  return copy;
}

const char *tig_version(void) {
  return TIG_VERSION_STRING;
}

void tig_git_engine_version(int *major, int *minor, int *revision) {
  int git_major = 0;
  int git_minor = 0;
  int git_revision = 0;

  git_libgit2_version(&git_major, &git_minor, &git_revision);

  if (major != 0) {
    *major = git_major;
  }
  if (minor != 0) {
    *minor = git_minor;
  }
  if (revision != 0) {
    *revision = git_revision;
  }
}

const char *tig_get_last_error(void) {
  return tig_last_error;
}

tig_status tig_init(const tig_options *options) {
  char *base_path = 0;

  tig_last_error[0] = '\0';

  if (options != 0 && options->repository_storage_base_path != 0) {
    base_path = tig_strdup(options->repository_storage_base_path);
    if (base_path == 0) {
      return tig__fail(TIG_ERROR_GIT, "tig_init", "out of memory");
    }
  }

  if (git_libgit2_init() < 0) {
    free(base_path);
    return tig__fail_git("tig_init");
  }

  free(tig_storage_base_path);
  tig_storage_base_path = base_path;
  tig_clock = options != 0 ? options->clock : 0;
  tig_clock_context = options != 0 ? options->clock_context : 0;
  return TIG_OK;
}

void tig_shutdown(void) {
  git_libgit2_shutdown();

  free(tig_storage_base_path);
  tig_storage_base_path = 0;
  tig_cloned_path[0] = '\0';
  tig_clock = 0;
  tig_clock_context = 0;
}

static int64_t tig_system_clock(void) {
#ifdef _WIN32
  FILETIME now;
  ULARGE_INTEGER ticks;

  /* 100ns ticks since 1601-01-01 */
  GetSystemTimeAsFileTime(&now);
  ticks.LowPart = now.dwLowDateTime;
  ticks.HighPart = now.dwHighDateTime;
  return (int64_t)((ticks.QuadPart - 116444736000000000ULL) / 10000);
#else
  struct timeval now;

  gettimeofday(&now, 0);
  return (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
#endif
}

/* TODO: correct by the offset measured against TiG Notary once signing is implemented */
tig_status tig_clock_now(tig_repository *repository, int64_t *unix_ms) {
  int64_t now;

  if (repository == 0 || unix_ms == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_clock_now", "repository and unix_ms are required");
  }

  now = tig_clock != 0 ? tig_clock(tig_clock_context) : tig_system_clock();
  if (now < repository->clock_last) {
    now = repository->clock_last;
  }
  repository->clock_last = now;

  *unix_ms = now;
  return TIG_OK;
}

tig_status tig__format_path(char *out, size_t out_size, const char *dir, const char *name,
                                  const char *suffix) {
  int written = snprintf(out, out_size, "%s%s%s", dir, name, suffix);

  if (written < 0 || (size_t)written >= out_size) {
    return tig__fail(TIG_ERROR_BUFFER_TOO_SMALL, "path too long", dir);
  }
  return TIG_OK;
}

tig_status tig__make_directory(const char *path) {
  if (TIG_MKDIR(path) != 0 && errno != EEXIST) {
    return tig__fail_errno(path);
  }
  return TIG_OK;
}

static tig_status tig_write_file_if_missing(const char *path, const char *contents) {
  FILE *file = fopen(path, "rb");

  if (file != 0) {
    fclose(file);
    return TIG_OK;
  }

  file = fopen(path, "wb");
  if (file == 0) {
    return tig__fail_errno(path);
  }

  if (fputs(contents, file) < 0) {
    fclose(file);
    return tig__fail_errno(path);
  }

  return fclose(file) == 0 ? TIG_OK : tig__fail_errno(path);
}

/* workdir always ends with a path separator */
static tig_status tig_create_layout(const char *workdir) {
  static const char *const entity_dirs[] = {"state", "events", "telemetry", "evidence"};
  char path[TIG_PATH_MAX];
  size_t i;
  tig_status result;

  for (i = 0; i < sizeof(entity_dirs) / sizeof(entity_dirs[0]); ++i) {
    result = tig__format_path(path, sizeof(path), workdir, entity_dirs[i], "");
    if (result == TIG_OK) {
      result = tig__make_directory(path);
    }

    /* git does not track empty directories */
    if (result == TIG_OK) {
      result = tig__format_path(path, sizeof(path), workdir, entity_dirs[i], "/.gitkeep");
    }
    if (result == TIG_OK) {
      result = tig_write_file_if_missing(path, "");
    }
    if (result != TIG_OK) {
      return result;
    }
  }

  result = tig__format_path(path, sizeof(path), workdir, ".gitignore", "");
  if (result == TIG_OK) {
    result = tig_write_file_if_missing(path, "/summaries/\n");
  }
  return result;
}

/* "https://host/owner/name.git/" -> "name" */
static tig_status tig_repository_name_from_url(const char *url, char *out, size_t out_size) {
  const char *end = url + strlen(url);
  const char *start;
  size_t length;

  while (end > url && (end[-1] == '/' || end[-1] == '\\')) {
    --end;
  }
  if (end - url >= 4 && memcmp(end - 4, ".git", 4) == 0) {
    end -= 4;
  }

  start = end;
  while (start > url && start[-1] != '/' && start[-1] != '\\' && start[-1] != ':') {
    --start;
  }

  length = (size_t)(end - start);
  if (length == 0 || (length == 1 && start[0] == '.') ||
      (length == 2 && start[0] == '.' && start[1] == '.')) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "cannot derive repository name from url", url);
  }
  if (length + 1 > out_size) {
    return tig__fail(TIG_ERROR_BUFFER_TOO_SMALL, "repository name too long", url);
  }

  memcpy(out, start, length);
  out[length] = '\0';
  return TIG_OK;
}

tig_status tig_repository_clone(const char *url, const char **path) {
  git_clone_options clone_options;
  git_repository *git = 0;
  char name[256];
  char destination[TIG_PATH_MAX];
  size_t base_length;
  tig_status result;

  if (url == 0 || path == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_repository_clone", "url and path are required");
  }

  *path = 0;
  if (tig_storage_base_path == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_repository_clone",
                    "repository_storage_base_path was not set in tig_init");
  }

  result = tig_repository_name_from_url(url, name, sizeof(name));
  if (result != TIG_OK) {
    return result;
  }

  base_length = strlen(tig_storage_base_path);
  while (base_length > 1 && (tig_storage_base_path[base_length - 1] == '/' ||
                             tig_storage_base_path[base_length - 1] == '\\')) {
    --base_length;
  }
  if (snprintf(destination, sizeof(destination), "%.*s/%s", (int)base_length,
               tig_storage_base_path, name) >= (int)sizeof(destination)) {
    return tig__fail(TIG_ERROR_BUFFER_TOO_SMALL, "path too long", tig_storage_base_path);
  }

  if (git_clone_options_init(&clone_options, GIT_CLONE_OPTIONS_VERSION) != 0 ||
      git_clone(&git, url, destination, &clone_options) != 0) {
    return tig__fail_git("tig_repository_clone");
  }
  git_repository_free(git);

  memcpy(tig_cloned_path, destination, strlen(destination) + 1);
  *path = tig_cloned_path;
  return TIG_OK;
}

/* commits the index on top of HEAD, then writes the index */
static tig_status tig_commit_index(tig_repository *repository, git_index *index, const char *message) {
  git_reference *head = 0;
  git_commit *parent = 0;
  git_tree *tree = 0;
  git_signature *signature = 0;
  git_oid tree_id;
  git_oid commit_id;
  int error;

  error = git_repository_head(&head, repository->git);
  if (error == 0) {
    error = git_reference_peel((git_object **)&parent, head, GIT_OBJECT_COMMIT);
  } else if (error == GIT_EUNBORNBRANCH || error == GIT_ENOTFOUND) {
    error = 0;
  }

  if (error == 0) {
    error = git_index_write_tree(&tree_id, index);
  }
  if (error == 0) {
    error = git_tree_lookup(&tree, repository->git, &tree_id);
  }
  if (error == 0 && git_signature_default(&signature, repository->git) != 0) {
    error = git_signature_now(&signature, "TiG", "tig@localhost");
  }
  if (error == 0) {
    error = git_commit_create_v(&commit_id, repository->git, "HEAD", signature, signature, 0, message,
                                tree, parent != 0 ? 1 : 0, parent);
  }
  if (error == 0) {
    error = git_index_write(index);
  }

  git_signature_free(signature);
  git_tree_free(tree);
  git_commit_free(parent);
  git_reference_free(head);
  return error == 0 ? TIG_OK : tig__fail_git("commit");
}

static tig_status tig_commit_layout(tig_repository *repository) {
  static const char *const layout_files[] = {".gitignore", "state/.gitkeep", "events/.gitkeep",
                                             "telemetry/.gitkeep", "evidence/.gitkeep"};
  git_index *index = 0;
  size_t i;
  tig_status result = TIG_OK;

  if (git_repository_index(&index, repository->git) != 0) {
    return tig__fail_git("tig_repository_create");
  }

  for (i = 0; i < sizeof(layout_files) / sizeof(layout_files[0]) && result == TIG_OK; ++i) {
    if (git_index_add_bypath(index, layout_files[i]) != 0) {
      result = tig__fail_git(layout_files[i]);
    }
  }
  if (result == TIG_OK) {
    result = tig_commit_index(repository, index, "Create TiG repository");
  }

  git_index_free(index);
  return result;
}

tig_status tig_repository_commit(tig_repository *repository, const char *message) {
  char telemetry_dir[] = "telemetry";
  char *paths[] = {telemetry_dir};
  git_strarray pathspec = {paths, 1};
  git_index *index = 0;
  git_reference *head = 0;
  git_tree *head_tree = 0;
  git_diff *diff = 0;
  size_t i;
  size_t count = 0;
  int error;
  tig_status result = TIG_OK;

  if (repository == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_repository_commit", "repository is required");
  }

  error = git_repository_index(&index, repository->git);
  if (error == 0) {
    error = git_repository_head(&head, repository->git);
    if (error == 0) {
      error = git_reference_peel((git_object **)&head_tree, head, GIT_OBJECT_TREE);
    } else if (error == GIT_EUNBORNBRANCH || error == GIT_ENOTFOUND) {
      error = 0;
    }
  }

  /* stage new, changed and deleted files, so deletions are caught below rather than ignored */
  if (error == 0) {
    error = git_index_add_all(index, &pathspec, GIT_INDEX_ADD_DEFAULT, 0, 0);
  }
  if (error == 0) {
    error = git_index_update_all(index, &pathspec, 0, 0);
  }
  if (error == 0) {
    error = git_diff_tree_to_index(&diff, repository->git, head_tree, index, 0);
  }
  if (error != 0) {
    result = tig__fail_git("tig_repository_commit");
  }

  if (result == TIG_OK) {
    count = git_diff_num_deltas(diff);
    for (i = 0; i < count && result == TIG_OK; ++i) {
      result = tig__telemetry_check_change(repository->git, index, git_diff_get_delta(diff, i));
    }
  }

  if (result == TIG_OK && count > 0) {
    result = tig_commit_index(repository, index, message != 0 ? message : "Record telemetry");
  }
  if (result != TIG_OK && index != 0) {
    /* drop what was staged here, keeping the index on disk as it was */
    git_index_read(index, 1);
  }

  git_diff_free(diff);
  git_tree_free(head_tree);
  git_reference_free(head);
  git_index_free(index);
  return result;
}

tig_status tig_repository_create(const char *path, tig_repository **out) {
  tig_repository *repository;
  const char *workdir;
  tig_status result;

  if (path == 0 || out == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_repository_create", "path and out are required");
  }

  *out = 0;
  repository = (tig_repository *)calloc(1, sizeof(*repository));
  if (repository == 0) {
    return tig__fail(TIG_ERROR_GIT, "tig_repository_create", "out of memory");
  }

  if (git_repository_init(&repository->git, path, 0) != 0) {
    free(repository);
    return tig__fail_git("tig_repository_create");
  }

  workdir = git_repository_workdir(repository->git);
  result = workdir == 0 ? tig__fail(TIG_ERROR_GIT, "tig_repository_create", "repository is bare")
                        : tig_create_layout(workdir);
  if (result == TIG_OK && git_repository_head_unborn(repository->git) == 1) {
    result = tig_commit_layout(repository);
  }
  if (result != TIG_OK) {
    tig_repository_close(repository);
    return result;
  }

  *out = repository;
  return TIG_OK;
}

tig_status tig_repository_open(const char *path, tig_repository **out) {
  tig_repository *repository;

  if (path == 0 || out == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_repository_open", "path and out are required");
  }

  *out = 0;
  repository = (tig_repository *)calloc(1, sizeof(*repository));
  if (repository == 0) {
    return tig__fail(TIG_ERROR_GIT, "tig_repository_open", "out of memory");
  }

  if (git_repository_open(&repository->git, path) != 0) {
    free(repository);
    return tig__fail_git("tig_repository_open");
  }

  *out = repository;
  return TIG_OK;
}

void tig_repository_close(tig_repository *repository) {
  if (repository == 0) {
    return;
  }

  git_repository_free(repository->git);
  free(repository);
}
