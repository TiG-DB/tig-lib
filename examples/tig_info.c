#include "tig/tig.h"

#include <stdio.h>

int main(void) {
  int major = 0;
  int minor = 0;
  int revision = 0;

  if (tig_init(0) != TIG_OK) {
    fputs("failed to initialize tig\n", stderr);
    return 1;
  }

  tig_git_engine_version(&major, &minor, &revision);
  printf("tig %s\n", tig_version());
  printf("git engine %d.%d.%d\n", major, minor, revision);

  tig_shutdown();
  return 0;
}
