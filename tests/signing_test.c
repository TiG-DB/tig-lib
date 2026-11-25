/* Signs commits with a device key, through ssh-keygen and in-process, and checks them with git verify-commit.
   Needs a POSIX shell, ssh-keygen and git on PATH; run from an empty working directory. */
#include "tig/tig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORK "tig-signing-work"

/* 2026-10-23T10:15:00Z */
#define T0 INT64_C(1792750500000)
#define MINUTE INT64_C(60000)

#define CHECK(condition)                                                                          \
  do {                                                                                            \
    if (!(condition)) {                                                                           \
      fprintf(stderr, "%s:%d: CHECK(%s) failed; last error: %s\n", __FILE__, __LINE__, #condition, \
              tig_get_last_error());                                                              \
      ++failures;                                                                                 \
    }                                                                                             \
  } while (0)

static int failures;
static int64_t fake_now = T0;

static int64_t fake_clock(void* context) {
  (void)context;
  return fake_now;
}

/* reads a whole file into buffer; returns its length, or -1 */
static long read_into(const char* path, char* buffer, size_t size) {
  FILE* file = fopen(path, "rb");
  size_t length;

  if (file == 0) {
    return -1;
  }
  length = fread(buffer, 1, size - 1, file);
  buffer[length] = '\0';
  fclose(file);
  return (long)length;
}

static int run(const char* command) {
  int status = system(command);

  if (status != 0) {
    fprintf(stderr, "command failed (%d): %s\n", status, command);
  }
  return status;
}

/* signs with the key at context, as git does with gpg.format=ssh */
static tig_status ssh_sign(void* context, const char* content, size_t content_length, char* signature,
                           size_t signature_size) {
  char command[1024];
  FILE* payload = fopen(WORK "/payload", "wb");

  if (payload == 0 || fwrite(content, 1, content_length, payload) != content_length) {
    if (payload != 0) {
      fclose(payload);
    }
    return TIG_ERROR_INVALID_ARGUMENT;
  }
  fclose(payload);

  snprintf(command, sizeof(command), "rm -f " WORK "/payload.sig && ssh-keygen -q -Y sign -n git -f %s " WORK "/payload",
           (const char*)context);
  if (run(command) != 0 || read_into(WORK "/payload.sig", signature, signature_size) <= 0) {
    return TIG_ERROR_REJECTED;
  }
  return TIG_OK;
}

/* makes a key pair and its allowed signers line */
static int make_device(const char* key, char* line, size_t line_size) {
  char command[1024];
  long length;

  snprintf(command, sizeof(command),
           "ssh-keygen -q -t ed25519 -N '' -C test -f %s && "
           "printf '%%s namespaces=\"git\" %%s\\n' \"$(ssh-keygen -l -E sha256 -f %s.pub | cut -d' ' -f2)\" "
           "\"$(cut -d' ' -f1,2 %s.pub)\" > %s.line",
           key, key, key, key);
  if (run(command) != 0) {
    return -1;
  }
  snprintf(command, sizeof(command), "%s.line", key);
  length = read_into(command, line, line_size);
  if (length <= 0) {
    return -1;
  }
  while (length > 0 && line[length - 1] == '\n') {
    line[--length] = '\0';
  }
  return 0;
}

static tig_status record(tig_repository* repository, const char* value) {
  static const tig_column columns[] = {{"value", TIG_VALUE_DECIMAL}};
  static const tig_stream_definition load = {"Load", 0, columns, 1, TIG_CHUNK_DAY, 5 * MINUTE};
  tig_status status = tig_telemetry_stream_define(repository, "system.load", &load);

  fake_now += MINUTE;
  if (status == TIG_OK) {
    status = tig_telemetry_append(repository, "system.load", fake_now, &value, 1);
  }
  return status == TIG_OK ? tig_repository_commit(repository, "Record telemetry") : status;
}

/* the %G? verification status of each commit, newest first, e.g. "GGN" */
static int signatures(const char* repository, const char* expected) {
  char command[1024];
  char actual[256];

  snprintf(command, sizeof(command),
           "cd %s && git -c gpg.ssh.allowedSignersFile=\"$(pwd)/.tig/allowed_signers\" log --format=%%G? | "
           "tr -d '\\n' > ../signatures",
           repository);
  if (run(command) != 0 || read_into(WORK "/signatures", actual, sizeof(actual)) < 0) {
    return 0;
  }
  if (strcmp(actual, expected) != 0) {
    fprintf(stderr, "%s: signatures %s, expected %s\n", repository, actual, expected);
    return 0;
  }
  return 1;
}

int main(void) {
  char line[1024];
  char other_line[1024];
  tig_options options = {0};
  tig_device device = {0};
  tig_device other = {0};
  tig_repository* repository;

  if (run("rm -rf " WORK " && mkdir " WORK) != 0 || make_device(WORK "/device", line, sizeof(line)) != 0 ||
      make_device(WORK "/other", other_line, sizeof(other_line)) != 0) {
    return 1;
  }
  device.allowed_signer = line;
  device.sign = ssh_sign;
  device.sign_context = WORK "/device";
  other.allowed_signer = other_line;
  other.sign = ssh_sign;
  other.sign_context = WORK "/other";

  options.clock = fake_clock;
  if (tig_init(&options) != TIG_OK) {
    return 1;
  }

  /* a new repository lists the key in its first commit, and signs every commit */
  CHECK(tig_repository_create(WORK "/signed", &device, &repository) == TIG_OK);
  CHECK(record(repository, "0.5") == TIG_OK);
  tig_repository_close(repository);
  CHECK(signatures(WORK "/signed", "GG"));
  CHECK(run("cd " WORK "/signed && git show --format= --name-only $(git rev-list --max-parents=0 HEAD) | "
            "grep -qx .tig/allowed_signers") == 0);

  /* without a device it can't commit, and another key isn't accepted */
  CHECK(tig_repository_open(WORK "/signed", &repository) == TIG_OK);
  CHECK(record(repository, "0.6") == TIG_ERROR_REJECTED);
  CHECK(tig_repository_set_device(repository, &other) == TIG_ERROR_REJECTED);
  CHECK(record(repository, "0.6") == TIG_ERROR_REJECTED);
  CHECK(tig_repository_set_device(repository, &device) == TIG_OK);
  CHECK(record(repository, "0.6") == TIG_OK);
  tig_repository_close(repository);
  CHECK(signatures(WORK "/signed", "GGG"));

  /* unsigned legacy history gains the key in a signed commit */
  CHECK(tig_repository_create(WORK "/legacy", 0, &repository) == TIG_OK);
  CHECK(record(repository, "1.5") == TIG_OK);
  CHECK(tig_repository_set_device(repository, &device) == TIG_OK);
  CHECK(record(repository, "1.6") == TIG_OK);
  tig_repository_close(repository);
  CHECK(signatures(WORK "/legacy", "GGNN"));
  CHECK(run("cd " WORK "/legacy && test \"$(git log -1 --skip=1 --format=%s)\" = 'Add device key' && "
            "git show --format= --name-only HEAD~1 | grep -qx .tig/allowed_signers") == 0);

  /* setting the same device again changes nothing */
  CHECK(tig_repository_open(WORK "/legacy", &repository) == TIG_OK);
  CHECK(tig_repository_set_device(repository, &device) == TIG_OK);
  tig_repository_close(repository);
  CHECK(signatures(WORK "/legacy", "GGNN"));

  /* a sign callback that returns no signature commits nothing */
  {
    tig_device broken = device;
    broken.sign_context = WORK "/missing-key";
    CHECK(tig_repository_open(WORK "/signed", &repository) == TIG_OK);
    CHECK(tig_repository_set_device(repository, &broken) == TIG_OK);
    CHECK(record(repository, "0.7") != TIG_OK);
    tig_repository_close(repository);
    CHECK(signatures(WORK "/signed", "GGG"));
  }

  /* the library signs in-process, with a key it made or one that ssh-keygen made */
  {
    char text[TIG_KEY_EXPORT_MAX];
    char expected[1024];
    long length;
    tig_key* generated = 0;
    tig_key* loaded = 0;

    CHECK(tig_key_generate(&generated) == TIG_OK);
    CHECK(tig_repository_create(WORK "/in-process", tig_key_device(generated), &repository) == TIG_OK);
    CHECK(record(repository, "2.5") == TIG_OK);
    tig_repository_close(repository);
    CHECK(signatures(WORK "/in-process", "GG"));

    /* and ssh-keygen reads the key it exports */
    CHECK(tig_key_export(generated, text, sizeof(text)) == TIG_OK);
    {
      FILE* file = fopen(WORK "/exported", "wb");
      CHECK(file != 0 && fputs(text, file) >= 0 && fclose(file) == 0);
    }
    CHECK(run("chmod 600 " WORK "/exported && ssh-keygen -y -f " WORK "/exported | cut -d' ' -f1,2 | tr -d '\\n' > "
              WORK "/exported.pub") == 0);
    CHECK(read_into(WORK "/exported.pub", expected, sizeof(expected)) > 0);
    CHECK(strcmp(expected, tig_key_public(generated)) == 0);

    length = read_into(WORK "/device", text, sizeof(text));
    CHECK(length > 0 && tig_key_parse(text, (size_t)length, &loaded) == TIG_OK);
    CHECK(strcmp(tig_key_device(loaded)->allowed_signer, line) == 0);
    CHECK(tig_repository_open(WORK "/signed", &repository) == TIG_OK);
    CHECK(tig_repository_set_device(repository, tig_key_device(loaded)) == TIG_OK);
    CHECK(record(repository, "0.8") == TIG_OK);
    tig_repository_close(repository);
    CHECK(signatures(WORK "/signed", "GGGG"));

    tig_key_free(generated);
    tig_key_free(loaded);
  }

  tig_shutdown();
  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  return 0;
}
