/* Device keys held by the library (COMMITS.md, "Device keys"): Ed25519, kept as an unencrypted OpenSSH private key
   file (openssh-key-v1), signing SSH signatures (PROTOCOL.sshsig) as `ssh-keygen -Y sign` does. The arithmetic is
   Monocypher's. */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <monocypher-ed25519.h>

#define TIG_KEY_TYPE "ssh-ed25519"
#define TIG_KEY_COMMENT "tig-device"
#define TIG_KEY_BEGIN "-----BEGIN OPENSSH PRIVATE KEY-----"
#define TIG_KEY_END "-----END OPENSSH PRIVATE KEY-----"
#define TIG_SIGNATURE_BEGIN "-----BEGIN SSH SIGNATURE-----"
#define TIG_SIGNATURE_END "-----END SSH SIGNATURE-----"
/* "openssh-key-v1" and its NUL */
#define TIG_KEY_MAGIC "openssh-key-v1"
#define TIG_KEY_MAGIC_SIZE 15
/* OpenSSH wraps base64 at 70 characters, in keys and signatures */
#define TIG_BASE64_WIDTH 70
/* a public key blob: two strings, "ssh-ed25519" and the 32 byte key */
#define TIG_PUBLIC_BLOB_SIZE (4 + 11 + 4 + 32)
/* room for a decoded private key file with a long comment */
#define TIG_KEY_BLOB_MAX 4096

struct tig_key {
  unsigned char secret[64]; /* the seed, then the public key */
  unsigned char public_key[32];
  char public_line[96];      /* ssh-ed25519 AAAA... */
  char fingerprint[64];      /* SHA256:... */
  char allowed_signer[192];  /* fingerprint, namespaces="git", public_line */
  tig_device device;
};

/* --- the SSH wire format --- */

typedef struct tig_writer {
  unsigned char *data;
  size_t size;
  size_t length;
  int overflowed;
} tig_writer;

static void tig_put(tig_writer *writer, const void *data, size_t length) {
  if (writer->overflowed || length > writer->size - writer->length) {
    writer->overflowed = 1;
    return;
  }
  if (length > 0) {
    memcpy(writer->data + writer->length, data, length);
    writer->length += length;
  }
}

static void tig_put_u32(tig_writer *writer, uint32_t value) {
  unsigned char bytes[4];

  bytes[0] = (unsigned char)(value >> 24);
  bytes[1] = (unsigned char)(value >> 16);
  bytes[2] = (unsigned char)(value >> 8);
  bytes[3] = (unsigned char)value;
  tig_put(writer, bytes, sizeof(bytes));
}

static void tig_put_string(tig_writer *writer, const void *data, size_t length) {
  tig_put_u32(writer, (uint32_t)length);
  tig_put(writer, data, length);
}

static void tig_put_text(tig_writer *writer, const char *text) {
  tig_put_string(writer, text, strlen(text));
}

typedef struct tig_reader {
  const unsigned char *data;
  size_t length;
  size_t at;
  int failed;
} tig_reader;

static uint32_t tig_get_u32(tig_reader *reader) {
  const unsigned char *bytes;

  if (reader->failed || reader->length - reader->at < 4) {
    reader->failed = 1;
    return 0;
  }
  bytes = reader->data + reader->at;
  reader->at += 4;
  return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | (uint32_t)bytes[3];
}

/* a string's bytes, still inside the data; NULL if it runs past the end */
static const unsigned char *tig_get_string(tig_reader *reader, size_t *length) {
  uint32_t size = tig_get_u32(reader);
  const unsigned char *bytes;

  *length = 0;
  if (reader->failed || size > reader->length - reader->at) {
    reader->failed = 1;
    return 0;
  }
  bytes = reader->data + reader->at;
  reader->at += size;
  *length = size;
  return bytes;
}

static int tig_string_is(tig_reader *reader, const char *expected) {
  size_t length;
  const unsigned char *bytes = tig_get_string(reader, &length);

  return bytes != 0 && length == strlen(expected) && memcmp(bytes, expected, length) == 0;
}

/* --- base64 --- */

static const char tig_base64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* appends the padded base64 of data, with a newline after every width characters and after the last (width 0 adds
   none); the writer holds text */
static void tig_put_base64(tig_writer *writer, const unsigned char *data, size_t length, size_t width) {
  size_t column = 0;
  size_t i;

  for (i = 0; i < length; i += 3) {
    size_t left = length - i;
    uint32_t group = (uint32_t)data[i] << 16 | (left > 1 ? (uint32_t)data[i + 1] << 8 : 0) |
                     (left > 2 ? (uint32_t)data[i + 2] : 0);
    char quad[4];
    int j;

    quad[0] = tig_base64_alphabet[group >> 18 & 63];
    quad[1] = tig_base64_alphabet[group >> 12 & 63];
    quad[2] = left > 1 ? tig_base64_alphabet[group >> 6 & 63] : '=';
    quad[3] = left > 2 ? tig_base64_alphabet[group & 63] : '=';
    for (j = 0; j < 4; ++j) {
      tig_put(writer, &quad[j], 1);
      if (width != 0 && ++column == width) {
        tig_put(writer, "\n", 1);
        column = 0;
      }
    }
  }
  if (width != 0 && column != 0) {
    tig_put(writer, "\n", 1);
  }
}

/* decodes the base64 in text, skipping white space; -1 if it isn't base64 or doesn't fit */
static int tig_decode_base64(const char *text, size_t length, unsigned char *out, size_t out_size, size_t *out_length) {
  uint32_t group = 0;
  int bits = 0;
  int padded = 0;
  size_t written = 0;
  size_t i;

  for (i = 0; i < length; ++i) {
    char c = text[i];
    const char *found;

    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      continue;
    }
    if (c == '=') {
      padded = 1;
      continue;
    }
    found = c != '\0' ? strchr(tig_base64_alphabet, c) : 0;
    if (found == 0 || padded) {
      return -1;
    }
    group = (group << 6 | (uint32_t)(found - tig_base64_alphabet)) & 0xffffff;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (written == out_size) {
        return -1;
      }
      out[written++] = (unsigned char)(group >> bits);
    }
  }
  *out_length = written;
  return 0;
}

/* --- keys --- */

static void tig_public_blob(const unsigned char public_key[32], unsigned char blob[TIG_PUBLIC_BLOB_SIZE]) {
  tig_writer writer = {0};

  writer.data = blob;
  writer.size = TIG_PUBLIC_BLOB_SIZE;
  tig_put_text(&writer, TIG_KEY_TYPE);
  tig_put_string(&writer, public_key, 32);
}

static tig_status tig_sign_commit(void *context, const char *content, size_t content_length, char *signature,
                                  size_t signature_size) {
  return tig_key_sign((const tig_key *)context, "git", content, content_length, signature, signature_size);
}

/* makes a key from its seed, which is wiped */
static tig_status tig_key_from_seed(unsigned char seed[32], tig_key **out) {
  tig_key *key = (tig_key *)calloc(1, sizeof(*key));
  unsigned char blob[TIG_PUBLIC_BLOB_SIZE];
  unsigned char digest[32];
  tig_writer line = {0};
  tig_writer fingerprint = {0};

  if (key == 0) {
    crypto_wipe(seed, 32);
    return tig__fail(TIG_ERROR_GIT, "device key", "out of memory");
  }
  crypto_ed25519_key_pair(key->secret, key->public_key, seed);
  tig_public_blob(key->public_key, blob);

  line.data = (unsigned char *)key->public_line;
  line.size = sizeof(key->public_line) - 1;
  tig_put(&line, TIG_KEY_TYPE " ", sizeof(TIG_KEY_TYPE));
  tig_put_base64(&line, blob, sizeof(blob), 0);

  tig__sha256(blob, sizeof(blob), digest);
  fingerprint.data = (unsigned char *)key->fingerprint;
  fingerprint.size = sizeof(key->fingerprint) - 1;
  tig_put(&fingerprint, "SHA256:", 7);
  tig_put_base64(&fingerprint, digest, sizeof(digest), 0);
  /* OpenSSH prints fingerprints without padding; 32 bytes leave one '=' */
  key->fingerprint[fingerprint.length - 1] = '\0';

  snprintf(key->allowed_signer, sizeof(key->allowed_signer), "%s namespaces=\"git\" %s", key->fingerprint,
           key->public_line);
  key->device.allowed_signer = key->allowed_signer;
  key->device.sign = tig_sign_commit;
  key->device.sign_context = key;
  *out = key;
  return TIG_OK;
}

tig_status tig_key_generate(tig_key **out) {
  unsigned char seed[32];
  tig_status result;

  if (out == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_key_generate", "out is required");
  }
  *out = 0;
  result = tig__random_bytes(seed, sizeof(seed));
  return result == TIG_OK ? tig_key_from_seed(seed, out) : result;
}

/* reads a decoded openssh-key-v1 file */
static tig_status tig_key_read(const unsigned char *blob, size_t length, tig_key **out) {
  tig_reader reader = {0};
  tig_reader inner = {0};
  unsigned char seed[32];
  const unsigned char *private_part;
  const unsigned char *public_key;
  const unsigned char *secret;
  size_t private_length;
  size_t public_length;
  size_t secret_length;
  size_t ignored;
  uint32_t keys;
  uint32_t first_check;
  uint32_t second_check;
  tig_status result;

  if (length < TIG_KEY_MAGIC_SIZE || memcmp(blob, TIG_KEY_MAGIC, TIG_KEY_MAGIC_SIZE) != 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "device key", "not an OpenSSH private key");
  }
  reader.data = blob;
  reader.length = length;
  reader.at = TIG_KEY_MAGIC_SIZE;
  if (!tig_string_is(&reader, "none") || !tig_string_is(&reader, "none")) {
    return tig__fail(TIG_ERROR_REJECTED, "device key", "the key is encrypted with a passphrase");
  }
  tig_get_string(&reader, &ignored); /* the KDF options */
  keys = tig_get_u32(&reader);
  tig_get_string(&reader, &ignored); /* the public key, repeated below */
  private_part = tig_get_string(&reader, &private_length);
  if (reader.failed || keys != 1) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "device key", "damaged, or more than one key");
  }

  inner.data = private_part;
  inner.length = private_length;
  first_check = tig_get_u32(&inner);
  second_check = tig_get_u32(&inner);
  if (!tig_string_is(&inner, TIG_KEY_TYPE)) {
    return tig__fail(TIG_ERROR_REJECTED, "device key", "not an Ed25519 key");
  }
  public_key = tig_get_string(&inner, &public_length);
  secret = tig_get_string(&inner, &secret_length);
  if (inner.failed || first_check != second_check || public_length != 32 || secret_length != 64 ||
      memcmp(secret + 32, public_key, 32) != 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "device key", "damaged");
  }

  memcpy(seed, secret, sizeof(seed));
  result = tig_key_from_seed(seed, out);
  if (result == TIG_OK && memcmp((*out)->public_key, public_key, 32) != 0) {
    tig_key_free(*out);
    *out = 0;
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "device key", "the public key doesn't match the private key");
  }
  return result;
}

/* the first occurrence of marker in the length bytes at text, which needn't be NUL-terminated */
static const char *tig_find(const char *text, size_t length, const char *marker) {
  size_t marker_length = strlen(marker);
  size_t i;

  for (i = 0; marker_length <= length && i <= length - marker_length; ++i) {
    if (memcmp(text + i, marker, marker_length) == 0) {
      return text + i;
    }
  }
  return 0;
}

tig_status tig_key_parse(const char *text, size_t length, tig_key **out) {
  const char *begin;
  const char *end = 0;
  unsigned char *blob;
  size_t blob_length = 0;
  tig_status result;

  if (text == 0 || out == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_key_parse", "text and out are required");
  }
  *out = 0;

  begin = tig_find(text, length, TIG_KEY_BEGIN);
  if (begin != 0) {
    begin += strlen(TIG_KEY_BEGIN);
    end = tig_find(begin, length - (size_t)(begin - text), TIG_KEY_END);
  }
  if (end == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "device key", "not an OpenSSH private key");
  }

  blob = (unsigned char *)malloc(TIG_KEY_BLOB_MAX);
  if (blob == 0) {
    return tig__fail(TIG_ERROR_GIT, "device key", "out of memory");
  }
  if (tig_decode_base64(begin, (size_t)(end - begin), blob, TIG_KEY_BLOB_MAX, &blob_length) != 0) {
    result = tig__fail(TIG_ERROR_INVALID_ARGUMENT, "device key", "damaged");
  } else {
    result = tig_key_read(blob, blob_length, out);
  }
  crypto_wipe(blob, TIG_KEY_BLOB_MAX);
  free(blob);
  return result;
}

tig_status tig_key_export(const tig_key *key, char *out, size_t out_size) {
  unsigned char public_blob[TIG_PUBLIC_BLOB_SIZE];
  unsigned char inner[256];
  unsigned char blob[512];
  unsigned char check[4];
  tig_writer private_part = {0};
  tig_writer file = {0};
  tig_writer text = {0};
  unsigned char padding;
  tig_status result;

  if (key == 0 || out == 0 || out_size == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_key_export", "key and out are required");
  }
  out[0] = '\0';
  /* repeated, so that a reader can tell it decrypted the key correctly; this one isn't encrypted */
  result = tig__random_bytes(check, sizeof(check));
  if (result != TIG_OK) {
    return result;
  }
  tig_public_blob(key->public_key, public_blob);

  private_part.data = inner;
  private_part.size = sizeof(inner);
  tig_put(&private_part, check, sizeof(check));
  tig_put(&private_part, check, sizeof(check));
  tig_put_text(&private_part, TIG_KEY_TYPE);
  tig_put_string(&private_part, key->public_key, sizeof(key->public_key));
  tig_put_string(&private_part, key->secret, sizeof(key->secret));
  tig_put_text(&private_part, TIG_KEY_COMMENT);
  /* padded with 1, 2, 3 ... to the cipher's block size, which is 8 for none */
  for (padding = 1; private_part.length % 8 != 0; ++padding) {
    tig_put(&private_part, &padding, 1);
  }

  file.data = blob;
  file.size = sizeof(blob);
  tig_put(&file, TIG_KEY_MAGIC, TIG_KEY_MAGIC_SIZE);
  tig_put_text(&file, "none"); /* cipher */
  tig_put_text(&file, "none"); /* KDF */
  tig_put_text(&file, "");     /* KDF options */
  tig_put_u32(&file, 1);
  tig_put_string(&file, public_blob, sizeof(public_blob));
  tig_put_string(&file, inner, private_part.length);

  text.data = (unsigned char *)out;
  text.size = out_size - 1;
  tig_put(&text, TIG_KEY_BEGIN "\n", strlen(TIG_KEY_BEGIN "\n"));
  tig_put_base64(&text, blob, file.length, TIG_BASE64_WIDTH);
  tig_put(&text, TIG_KEY_END "\n", strlen(TIG_KEY_END "\n"));

  crypto_wipe(inner, sizeof(inner));
  crypto_wipe(blob, sizeof(blob));
  if (private_part.overflowed || file.overflowed || text.overflowed) {
    crypto_wipe(out, out_size);
    return tig__fail(TIG_ERROR_BUFFER_TOO_SMALL, "tig_key_export", "out needs TIG_KEY_EXPORT_MAX bytes");
  }
  out[text.length] = '\0';
  return TIG_OK;
}

tig_status tig_key_sign(const tig_key *key, const char *name_space, const void *content, size_t content_length,
                        char *signature, size_t signature_size) {
  unsigned char public_blob[TIG_PUBLIC_BLOB_SIZE];
  unsigned char digest[64];
  unsigned char signed_data[256];
  unsigned char raw[64];
  unsigned char inner[128];
  unsigned char blob[384];
  tig_writer to_sign = {0};
  tig_writer wrapped = {0};
  tig_writer file = {0};
  tig_writer text = {0};

  if (key == 0 || name_space == 0 || name_space[0] == '\0' || strlen(name_space) > 64 ||
      (content == 0 && content_length != 0) || signature == 0 || signature_size == 0) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_key_sign",
                     "key, a namespace of 1 to 64 characters, content and signature are required");
  }
  signature[0] = '\0';
  crypto_sha512(digest, (const uint8_t *)content, content_length);

  /* what is signed: the namespace and the content's hash, so a signature can't be reused for another purpose */
  to_sign.data = signed_data;
  to_sign.size = sizeof(signed_data);
  tig_put(&to_sign, "SSHSIG", 6);
  tig_put_text(&to_sign, name_space);
  tig_put_text(&to_sign, ""); /* reserved */
  tig_put_text(&to_sign, "sha512");
  tig_put_string(&to_sign, digest, sizeof(digest));
  if (to_sign.overflowed) {
    return tig__fail(TIG_ERROR_INVALID_ARGUMENT, "tig_key_sign", "the namespace is too long");
  }
  crypto_ed25519_sign(raw, key->secret, signed_data, to_sign.length);

  wrapped.data = inner;
  wrapped.size = sizeof(inner);
  tig_put_text(&wrapped, TIG_KEY_TYPE);
  tig_put_string(&wrapped, raw, sizeof(raw));

  tig_public_blob(key->public_key, public_blob);
  file.data = blob;
  file.size = sizeof(blob);
  tig_put(&file, "SSHSIG", 6);
  tig_put_u32(&file, 1); /* version */
  tig_put_string(&file, public_blob, sizeof(public_blob));
  tig_put_text(&file, name_space);
  tig_put_text(&file, ""); /* reserved */
  tig_put_text(&file, "sha512");
  tig_put_string(&file, inner, wrapped.length);

  text.data = (unsigned char *)signature;
  text.size = signature_size - 1;
  tig_put(&text, TIG_SIGNATURE_BEGIN "\n", strlen(TIG_SIGNATURE_BEGIN "\n"));
  tig_put_base64(&text, blob, file.length, TIG_BASE64_WIDTH);
  tig_put(&text, TIG_SIGNATURE_END "\n", strlen(TIG_SIGNATURE_END "\n"));
  if (wrapped.overflowed || file.overflowed || text.overflowed) {
    signature[0] = '\0';
    return tig__fail(TIG_ERROR_BUFFER_TOO_SMALL, "tig_key_sign", "the signature doesn't fit");
  }
  signature[text.length] = '\0';
  return TIG_OK;
}

const tig_device *tig_key_device(const tig_key *key) {
  return key != 0 ? &key->device : 0;
}

const char *tig_key_public(const tig_key *key) {
  return key != 0 ? key->public_line : 0;
}

const char *tig_key_fingerprint(const tig_key *key) {
  return key != 0 ? key->fingerprint : 0;
}

void tig_key_free(tig_key *key) {
  if (key != 0) {
    crypto_wipe(key, sizeof(*key));
    free(key);
  }
}
