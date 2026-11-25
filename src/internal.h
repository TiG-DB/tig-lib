#ifndef TIG_INTERNAL_H
#define TIG_INTERNAL_H

#include "tig/tig.h"

#include <git2.h>

#define TIG_PATH_MAX 4096

#define TIG_ALLOWED_SIGNERS ".tig/allowed_signers"

struct tig_repository {
  git_repository *git;
  int64_t clock_last;
  /* set by tig_repository_create or tig_repository_set_device; commits are unsigned without it */
  char *allowed_signer;
  tig_sign_callback sign;
  void *sign_context;
};

tig_status tig__fail(tig_status status, const char *context, const char *detail);
tig_status tig__fail_git(const char *context);
tig_status tig__fail_errno(const char *context);

tig_status tig__format_path(char *out, size_t out_size, const char *dir, const char *name,
                            const char *suffix);
tig_status tig__make_directory(const char *path);
/* cryptographically secure */
tig_status tig__random_bytes(unsigned char *out, size_t count);

/* sha256.c */
void tig__sha256(const void *data, size_t length, unsigned char digest[32]);

/* how far ahead of the TiG clock a reading or event may be */
#define TIG_CLOCK_SKEW_MS 5000

/* telemetry.c: whether a staged change follows the append-only rule */
tig_status tig__telemetry_check_change(git_repository *git, git_index *index,
                                       const git_diff_delta *delta);
int        tig__is_valid_stream_name(const char *name);
/* days since 1970-01-01 */
int64_t    tig__days_from_civil(int64_t year, int month, int day);

/* node.c: context names the source in errors */
tig_status tig__node_parse(const char *yaml, size_t length, const char *context, tig_node **out);

/* evidence.c: whether a path is under /evidence, and whether a staged change there follows EVIDENCE.md: files
   can be added, but never changed or removed */
int        tig__is_evidence_path(const char *path);
tig_status tig__evidence_check_change(const git_diff_delta *delta);

/* events.c: whether a path is under /events or /state, and whether the staged changes there follow
   EVENTS.md, "Writing"; *changed is set if there are any */
int        tig__is_event_path(const char *path);
tig_status tig__events_check_commit(tig_repository *repository, git_index *index, git_diff *diff, int *changed);

#endif
