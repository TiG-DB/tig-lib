#include "tig/tig.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  int major = 0;
  int minor = 0;
  int revision = 0;
  tig_repository* repository;
  const char* cloned_path;

  if (tig_init(0) != TIG_OK) {
    fputs("tig_init failed\n", stderr);
    return 1;
  }

  if (strcmp(tig_version(), "0.4.0") != 0) {
    fprintf(stderr, "unexpected tig version: %s\n", tig_version());
    tig_shutdown();
    return 1;
  }

  tig_git_engine_version(&major, &minor, &revision);
  if (major < 0 || minor < 0 || revision < 0) {
    fputs("invalid git engine version\n", stderr);
    tig_shutdown();
    return 1;
  }

  if (tig_repository_open(0, 0) != TIG_ERROR_INVALID_ARGUMENT) {
    fputs("tig_repository_open should reject null arguments\n", stderr);
    tig_shutdown();
    return 1;
  }

  if (tig_repository_clone("https://example.com/car.git", &cloned_path) != TIG_ERROR_INVALID_ARGUMENT ||
      tig_get_last_error()[0] == '\0') {
    fputs("tig_repository_clone should fail without a storage base path\n", stderr);
    tig_shutdown();
    return 1;
  }

  if (tig_repository_create("tig-smoke-repo", 0, &repository) != TIG_OK) {
    fprintf(stderr, "tig_repository_create failed: %s\n", tig_get_last_error());
    tig_shutdown();
    return 1;
  }
  tig_repository_close(repository);

  {
    static const char* const files[] = {"tig-smoke-repo/README.md", "tig-smoke-repo/AGENTS.md"};
    size_t i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
      FILE* file = fopen(files[i], "r");
      if (file == 0) {
        fprintf(stderr, "new repository is missing %s\n", files[i]);
        tig_shutdown();
        return 1;
      }
      fclose(file);
    }
  }

  if (tig_repository_open("tig-smoke-repo", &repository) != TIG_OK) {
    fprintf(stderr, "tig_repository_open failed: %s\n", tig_get_last_error());
    tig_shutdown();
    return 1;
  }
  tig_repository_close(repository);

  tig_shutdown();
  return 0;
}
