/* Evidence: the files that support events (EVIDENCE.md). */
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define TIG_EVIDENCE_DIR "evidence/"
#define TIG_EVIDENCE_DIR_LENGTH 9
#define TIG_COPY_CHUNK 65536

int tig__is_evidence_path(const char *path) {
  return strncmp(path, TIG_EVIDENCE_DIR, TIG_EVIDENCE_DIR_LENGTH) == 0;
}

tig_status tig__evidence_check_change(const git_diff_delta *delta) {
  const char *path = delta->status == GIT_DELTA_DELETED ? delta->old_file.path : delta->new_file.path;

  /* new files are fine however they got there; everything already committed is fixed */
  if (delta->status == GIT_DELTA_ADDED) {
    return TIG_OK;
  }
  return tig__fail(TIG_ERROR_REJECTED, path, "evidence is never changed or removed once committed; add a new file instead");
}

static int tig_is_alphanumeric(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int tig_evidence_name_is_valid(const char *name) {
  size_t length = strlen(name);
  size_t i;

  if (length == 0 || length > TIG_EVIDENCE_NAME_MAX || !tig_is_alphanumeric(name[0])) {
    return 0;
  }
  for (i = 1; i < length; ++i) {
    if (!tig_is_alphanumeric(name[i]) && name[i] != '.' && name[i] != '_' && name[i] != '-') {
      return 0;
    }
  }
  return 1;
}

/* the part of a path after its last separator */
static const char *tig_file_name(const char *path) {
  const char *name = path;
  const char *p;

  for (p = path; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      name = p + 1;
    }
  }
  return name;
}

/* copies source to destination, which is removed again if anything goes wrong or there was nothing to copy */
static tig_status tig_copy_file(const char *source, const char *destination) {
  char buffer[TIG_COPY_CHUNK];
  FILE *in = fopen(source, "rb");
  FILE *out;
  size_t total = 0;
  size_t count;
  tig_status result = TIG_OK;

  if (in == 0) {
    return errno == ENOENT ? tig__fail(TIG_ERROR_NOT_FOUND, source, "no such file") : tig__fail_errno(source);
  }
  out = fopen(destination, "wb");
  if (out == 0) {
    fclose(in);
    return tig__fail_errno(destination);
  }

  while (result == TIG_OK && (count = fread(buffer, 1, sizeof(buffer), in)) > 0) {
    if (fwrite(buffer, 1, count, out) != count) {
      result = tig__fail_errno(destination);
    }
    total += count;
  }
  if (result == TIG_OK && ferror(in)) {
    result = tig__fail_errno(source);
  }
  fclose(in);
  if (fclose(out) != 0 && result == TIG_OK) {
    result = tig__fail_errno(destination);
  }
  if (result == TIG_OK && total == 0) {
    result = tig__fail(TIG_ERROR_REJECTED, source, "is empty, so it can't be evidence");
  }
  if (result != TIG_OK) {
    remove(destination);
  }
  return result;
}

tig_status tig_evidence_add(tig_repository *repository, const char *source, const char *name, char *path,
                            size_t path_size) {
  char relative[TIG_EVIDENCE_NAME_MAX + TIG_EVIDENCE_DIR_LENGTH + 1];
  char destination[TIG_PATH_MAX];
  char context[TIG_EVIDENCE_PATH_MAX];
  const char *workdir;
  git_index *index = 0;
  FILE *probe;
  tig_status result = TIG_OK;

  if (repository == 0 || source == 0 || path == 0 || path_size < TIG_EVIDENCE_PATH_MAX) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_evidence_add",
                     "repository, source and room for the path are required");
  }
  path[0] = '\0';
  if (name == 0) {
    name = tig_file_name(source);
  }
  if (!tig_evidence_name_is_valid(name)) {
    return tig__fail(TIG_ERROR_REJECTED, name[0] != '\0' ? name : "evidence name",
                     "isn't allowed: use at most 100 letters, digits, '.', '_' and '-', starting with a letter or digit");
  }
  snprintf(context, sizeof(context), "/" TIG_EVIDENCE_DIR "%s", name);
  snprintf(relative, sizeof(relative), TIG_EVIDENCE_DIR "%s", name);

  workdir = git_repository_workdir(repository->git);
  if (workdir == 0) {
    return tig__fail(TIG_ERROR_GIT, "tig_evidence_add", "repository is bare");
  }
  result = tig__format_path(destination, sizeof(destination), workdir, "evidence", "");
  if (result == TIG_OK) {
    result = tig__make_directory(destination);
  }
  if (result == TIG_OK) {
    result = tig__format_path(destination, sizeof(destination), workdir, relative, "");
  }
  if (result != TIG_OK) {
    return result;
  }

  /* evidence is never replaced: not by a committed file, a staged one, or one that is only on disk */
  if (git_repository_index(&index, repository->git) != 0) {
    return tig__fail_git(context);
  }
  probe = fopen(destination, "rb");
  if (probe != 0) {
    fclose(probe);
  }
  if (probe != 0 || git_index_get_bypath(index, relative, 0) != 0) {
    git_index_free(index);
    return tig__fail(TIG_ERROR_REJECTED, context, "already exists, and evidence is never replaced");
  }

  result = tig_copy_file(source, destination);
  if (result == TIG_OK && (git_index_add_bypath(index, relative) != 0 || git_index_write(index) != 0)) {
    result = tig__fail_git(context);
    remove(destination);
  }
  git_index_free(index);
  if (result == TIG_OK) {
    memcpy(path, context, strlen(context) + 1);
  }
  return result;
}
