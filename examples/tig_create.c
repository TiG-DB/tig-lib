#include "tig/tig.h"

#include <stdio.h>

int main(int argc, char** argv) {
  tig_options options = {0};
  tig_repository* repository;
  int result;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <path>\n", argv[0]);
    return 2;
  }

  if (tig_init(&options) != TIG_OK) {
    fputs("failed to initialize tig\n", stderr);
    return 1;
  }

  result = tig_repository_create(argv[1], &repository);
  if (result != TIG_OK) {
    fprintf(stderr, "failed to create tig repository at %s (%d)\n", argv[1], result);
    tig_shutdown();
    return 1;
  }

  printf("created tig repository at %s\n", argv[1]);

  tig_repository_close(repository);
  tig_shutdown();
  return 0;
}
