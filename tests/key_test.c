/* Device keys, checked against what ssh-keygen made from the same key. Ed25519 signatures are deterministic, so
   the signature must match to the byte on every platform. */
#include "tig/tig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                          \
  do {                                                                                            \
    if (!(condition)) {                                                                           \
      fprintf(stderr, "%s:%d: CHECK(%s) failed; last error: %s\n", __FILE__, __LINE__, #condition, \
              tig_get_last_error());                                                              \
      exit(1);                                                                                    \
    }                                                                                             \
  } while (0)

/* a throwaway key made for this test with `ssh-keygen -t ed25519 -N ''` */
static const char vector_key[] =
    "-----BEGIN OPENSSH PRIVATE KEY-----\n"
    "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW\n"
    "QyNTUxOQAAACA4OpFVCHZOQNnn8pyUddJl3qiX4W3WYWYN8pgXT+V4OwAAAJhCa2IjQmti\n"
    "IwAAAAtzc2gtZWQyNTUxOQAAACA4OpFVCHZOQNnn8pyUddJl3qiX4W3WYWYN8pgXT+V4Ow\n"
    "AAAECx9Gcq+ZCuwWCwQ1U+22mnleuQo9BO6u3w8IHB/zzruTg6kVUIdk5A2efynJR10mXe\n"
    "qJfhbdZhZg3ymBdP5Xg7AAAAD3RpZy10ZXN0LXZlY3RvcgECAwQFBg==\n"
    "-----END OPENSSH PRIVATE KEY-----\n";

static const char vector_public[] = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIDg6kVUIdk5A2efynJR10mXeqJfhbdZhZg3ymBdP5Xg7";
static const char vector_fingerprint[] = "SHA256:eFHvbhceIuwXsnZdzW8VWmfe2OXB8FfzCB2Ep0C7tBI";

static const char vector_content[] =
    "tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904\n"
    "author TiG <tig@localhost> 1792504800 +0000\n"
    "\n"
    "Create TiG repository\n";

/* from `ssh-keygen -Y sign -n git` */
static const char vector_signature[] =
    "-----BEGIN SSH SIGNATURE-----\n"
    "U1NIU0lHAAAAAQAAADMAAAALc3NoLWVkMjU1MTkAAAAgODqRVQh2TkDZ5/KclHXSZd6ol+\n"
    "Ft1mFmDfKYF0/leDsAAAADZ2l0AAAAAAAAAAZzaGE1MTIAAABTAAAAC3NzaC1lZDI1NTE5\n"
    "AAAAQP2QhaN7PExkyKD3sKJXlxoavjz5cbP3xviqhWE0+8XbtyzUTvp83hSGXt3fdCjkjJ\n"
    "SQmT41rmGCX5jLyqAfow4=\n"
    "-----END SSH SIGNATURE-----\n";

/* the same text with CRLF line endings, as an editor on Windows might leave it */
static void to_crlf(const char* from, char* to) {
  for (; *from != '\0'; ++from) {
    if (*from == '\n') {
      *to++ = '\r';
    }
    *to++ = *from;
  }
  *to = '\0';
}

int main(void) {
  tig_key* key;
  tig_key* generated;
  tig_key* again;
  const tig_device* device;
  tig_repository* repository;
  char signature[TIG_KEY_SIGNATURE_MAX];
  char other[TIG_KEY_SIGNATURE_MAX];
  char exported[TIG_KEY_EXPORT_MAX];
  char crlf[2 * sizeof(vector_key)];
  char line[512];
  char small[32];

  CHECK(tig_init(0) == TIG_OK);

  CHECK(tig_key_parse(vector_key, strlen(vector_key), &key) == TIG_OK);
  CHECK(strcmp(tig_key_public(key), vector_public) == 0);
  CHECK(strcmp(tig_key_fingerprint(key), vector_fingerprint) == 0);
  CHECK(tig_key_sign(key, "git", vector_content, strlen(vector_content), signature, sizeof(signature)) == TIG_OK);
  CHECK(strcmp(signature, vector_signature) == 0);

  /* the namespace and every byte of the content are signed */
  CHECK(tig_key_sign(key, "tig-notary", vector_content, strlen(vector_content), other, sizeof(other)) == TIG_OK);
  CHECK(strcmp(other, signature) != 0);
  CHECK(tig_key_sign(key, "git", vector_content, strlen(vector_content) - 1, other, sizeof(other)) == TIG_OK);
  CHECK(strcmp(other, signature) != 0);

  /* the device for a repository: its allowed signers line, and signing in the git namespace */
  device = tig_key_device(key);
  snprintf(line, sizeof(line), "%s namespaces=\"git\" %s", vector_fingerprint, vector_public);
  CHECK(strcmp(device->allowed_signer, line) == 0);
  CHECK(device->sign(device->sign_context, vector_content, strlen(vector_content), other, sizeof(other)) == TIG_OK);
  CHECK(strcmp(other, vector_signature) == 0);

  CHECK(tig_key_sign(key, "", vector_content, 1, other, sizeof(other)) == TIG_ERROR_INVALID_ARGUMENT);
  CHECK(tig_key_sign(key, "git", vector_content, 1, small, sizeof(small)) == TIG_ERROR_BUFFER_TOO_SMALL);
  CHECK(small[0] == '\0');
  CHECK(tig_key_export(key, small, sizeof(small)) == TIG_ERROR_BUFFER_TOO_SMALL);
  tig_key_free(key);

  to_crlf(vector_key, crlf);
  CHECK(tig_key_parse(crlf, strlen(crlf), &key) == TIG_OK);
  CHECK(strcmp(tig_key_fingerprint(key), vector_fingerprint) == 0);
  tig_key_free(key);

  /* nothing that isn't a whole key is accepted */
  CHECK(tig_key_parse("", 0, &key) == TIG_ERROR_INVALID_ARGUMENT && key == 0);
  CHECK(tig_key_parse(vector_public, strlen(vector_public), &key) == TIG_ERROR_INVALID_ARGUMENT);
  CHECK(tig_key_parse(vector_key, strlen(vector_key) / 2, &key) == TIG_ERROR_INVALID_ARGUMENT);
  /* a changed seed no longer makes the stored public key */
  strcpy(crlf, vector_key);
  {
    char* at = strstr(crlf, "AAAECx9Gcq");
    CHECK(at != 0);
    at[9] = 'r';
  }
  CHECK(tig_key_parse(crlf, strlen(crlf), &key) == TIG_ERROR_INVALID_ARGUMENT && key == 0);

  /* a new key survives export and parse, and differs from the next */
  CHECK(tig_key_generate(&generated) == TIG_OK);
  CHECK(tig_key_generate(&again) == TIG_OK);
  CHECK(strcmp(tig_key_fingerprint(generated), tig_key_fingerprint(again)) != 0);
  tig_key_free(again);
  CHECK(tig_key_export(generated, exported, sizeof(exported)) == TIG_OK);
  CHECK(tig_key_parse(exported, strlen(exported), &again) == TIG_OK);
  CHECK(strcmp(tig_key_public(generated), tig_key_public(again)) == 0);
  CHECK(tig_key_sign(generated, "git", "x", 1, signature, sizeof(signature)) == TIG_OK);
  CHECK(tig_key_sign(again, "git", "x", 1, other, sizeof(other)) == TIG_OK);
  CHECK(strcmp(signature, other) == 0);
  tig_key_free(again);

  /* signs a repository's commits, which then needs a device to commit */
  CHECK(tig_repository_create("tig-key-repo", tig_key_device(generated), &repository) == TIG_OK);
  tig_repository_close(repository);
  CHECK(tig_repository_open("tig-key-repo", &repository) == TIG_OK);
  CHECK(tig_repository_set_device(repository, tig_key_device(generated)) == TIG_OK);
  tig_repository_close(repository);
  tig_key_free(generated);

  tig_shutdown();
  return 0;
}
