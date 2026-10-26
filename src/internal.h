#ifndef TIG_INTERNAL_H
#define TIG_INTERNAL_H

#include "tig/tig.h"

#include <git2.h>

#define TIG_PATH_MAX 4096

struct tig_repository {
  git_repository *git;
  int64_t clock_last;
};

tig_status tig__fail(tig_status status, const char *context, const char *detail);
tig_status tig__fail_git(const char *context);
tig_status tig__fail_errno(const char *context);

tig_status tig__format_path(char *out, size_t out_size, const char *dir, const char *name,
                            const char *suffix);
tig_status tig__make_directory(const char *path);

/* telemetry.c: whether a staged change follows the append-only rule */
tig_status tig__telemetry_check_change(git_repository *git, git_index *index,
                                       const git_diff_delta *delta);

#endif
