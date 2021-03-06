/*
* Copyright (c) 2014 Yubico AB
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*
* * Redistributions of source code must retain the above copyright
* notice, this list of conditions and the following disclaimer.
*
* * Redistributions in binary form must reproduce the above
* copyright notice, this list of conditions and the following
* disclaimer in the documentation and/or other materials provided
* with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
* A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
* OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
* THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "internal.h"

#include <string.h>
#include <unistd.h>
#include <json.h>
#include "crypto.h"
#include "b64/cencode.h"
#include "b64/cdecode.h"
#include "sha256.h"

#ifdef HAVE_JSON_OBJECT_OBJECT_GET_EX
#define u2fs_json_object_object_get(obj, key, value) json_object_object_get_ex(obj, key, &value)
#else
typedef int json_bool;
#define u2fs_json_object_object_get(obj, key, value) (value = json_object_object_get(obj, key)) == NULL ? (json_bool)FALSE : (json_bool)TRUE
#endif

static u2fs_rc encode_b64u(const char *data, size_t data_len, char *output)
{
  base64_encodestate b64;
  int cnt;

  if ((data_len * 4) >= (_B64_BUFSIZE * 3) || output == NULL)	//base64 is 75% efficient (4 characters encode 3 bytes)
    return U2FS_MEMORY_ERROR;

  base64_init_encodestate(&b64);
  cnt = base64_encode_block(data, data_len, output, &b64);
  cnt += base64_encode_blockend(output + cnt, &b64);

  output[cnt] = '\0';

  return U2FS_OK;
}

static u2fs_rc gen_challenge(u2fs_ctx_t *ctx)
{
  char buf[U2FS_CHALLENGE_RAW_LEN];
  u2fs_rc rc;

  if (ctx->challenge[0] != '\0')
    return U2FS_OK;

  rc = set_random_bytes(buf, U2FS_CHALLENGE_RAW_LEN);
  if (rc != U2FS_OK)
    return rc;

  return encode_b64u(buf, U2FS_CHALLENGE_RAW_LEN, ctx->challenge);
}

/**
 * u2fs_init:
 * @ctx: pointer to output variable holding a context handle.
 *
 * Initialize the U2F server context handle.
 *
 * Returns: On success %U2FS_OK (integer 0) is returned, and on errors
 * an #u2fs_rc error code.
 */
u2fs_rc u2fs_init(u2fs_ctx_t ** ctx)
{
  *ctx = calloc(1, sizeof(**ctx));
  if (*ctx == NULL)
    return U2FS_MEMORY_ERROR;

  return U2FS_OK;
}

/**
 * u2fs_done:
 * @ctx: a context handle, from u2fs_init()
 *
 * Deallocate resources associated with context @ctx.
 */
void u2fs_done(u2fs_ctx_t * ctx)
{
  if (ctx == NULL)
    return;

  free(ctx->keyHandle);
  ctx->keyHandle = NULL;
  free_key(ctx->key);
  ctx->key = NULL;
  free(ctx->origin);
  ctx->origin = NULL;
  free(ctx->appid);
  ctx->appid = NULL;
  free(ctx);
}

/**
 * u2fs_free_reg_res:
 * @result: a registration result as generated by u2fs_registration_verify()
 *
 * Deallocate resources associated with @result.
 */
void u2fs_free_reg_res(u2fs_reg_res_t * result)
{
  if (result != NULL) {
    if (result->keyHandle) {
      free(result->keyHandle);
      result->keyHandle = NULL;
    }
    if (result->publicKey) {
      free(result->publicKey);
      result->publicKey = NULL;
    }
    if (result->attestation_certificate_PEM) {
      free(result->attestation_certificate_PEM);
      result->attestation_certificate_PEM = NULL;
    }
    if (result->user_public_key) {
      free_key(result->user_public_key);
      result->user_public_key = NULL;
    }
    if (result->attestation_certificate) {
      free_cert(result->attestation_certificate);
      result->attestation_certificate = NULL;
    }
    free(result);
  }
}

/**
 * u2fs_free_auth_res:
 * @result: an authentication result as generated by u2fs_authentication_verify()
 *
 * Deallocate resources associated with @result.
 */
void u2fs_free_auth_res(u2fs_auth_res_t * result)
{
  if (result != NULL) {
    result->verified = -1;
    result->counter = 0;
    result->user_presence = 0;
  }
  free(result);
}

/**
 * u2fs_set_challenge:
 * @ctx: a context handle, from u2fs_init()
 * @challenge: a 43-byte long, websafe Base64 encoded challenge (viz RFC4648 Section 5)
 *
 * Stores a given @challenge within @ctx. If a value is already
 * present, it is cleared and the memory is released.
 *
 * Returns: On success %U2FS_OK (integer 0) is returned, and on errors
 * an #u2fs_rc error code.
 */
u2fs_rc u2fs_set_challenge(u2fs_ctx_t * ctx, const char *challenge)
{
  if (ctx == NULL || challenge == NULL)
    return U2FS_MEMORY_ERROR;

  if (strlen(challenge) != U2FS_CHALLENGE_B64U_LEN)
    return U2FS_CHALLENGE_ERROR;

  strncpy(ctx->challenge, challenge, U2FS_CHALLENGE_B64U_LEN);

  return U2FS_OK;
}

/**
 * u2fs_set_keyHandle:
 * @ctx: a context handle, from u2fs_init()
 * @keyHandle: a registered key-handle in websafe Base64 form, to use for signing, as returned by the U2F registration.
 *
 * Stores a given @keyHandle within @ctx. If a value is already present, it is cleared and the memory is released.
 *
 * Returns: On success %U2FS_OK (integer 0) is returned, and on errors
 * an #u2fs_rc error code.
 */
u2fs_rc u2fs_set_keyHandle(u2fs_ctx_t * ctx, const char *keyHandle)
{
  if (ctx == NULL || keyHandle == NULL)
    return U2FS_MEMORY_ERROR;

  if (ctx->keyHandle != NULL) {
    free(ctx->keyHandle);
    ctx->keyHandle = NULL;
  }

  ctx->keyHandle = strndup(keyHandle, strlen(keyHandle));

  if (ctx->keyHandle == NULL)
    return U2FS_MEMORY_ERROR;

  return U2FS_OK;
}

/**
 * u2fs_set_publicKey:
 * @ctx: a context handle, from u2fs_init()
 * @publicKey: a 65-byte raw EC public key as returned from registration.
 *
 * Decode @publicKey and store within @ctx. If a value is already
 * present, it is cleared and the memory is released.
 *
 * Returns: On success %U2FS_OK (integer 0) is returned, and on errors
 * a #u2fs_rc error code.
 */
u2fs_rc
u2fs_set_publicKey(u2fs_ctx_t * ctx, const unsigned char *publicKey)
{
  u2fs_EC_KEY_t *user_key;
  u2fs_rc rc;

  if (ctx == NULL || publicKey == NULL)
    return U2FS_MEMORY_ERROR;

  rc = decode_user_key(publicKey, &user_key);
  if (rc != U2FS_OK)
    return rc;

  if (ctx->key != NULL)
    free_key(ctx->key);

  ctx->key = user_key;

  return U2FS_OK;
}

/**
 * u2fs_get_registration_keyHandle:
 * @result: a registration result obtained from u2fs_registration_verify()
 *
 * Get the Base64 keyHandle obtained during the U2F registration
 * operation.  The memory is allocated by the library, and must not be
 * deallocated by the caller.
 *
 * Returns: On success the pointer to the buffer containing the keyHandle
 * is returned, and on errors NULL.
 */
const char *u2fs_get_registration_keyHandle(u2fs_reg_res_t * result)
{
  if (result == NULL)
    return NULL;

  return result->keyHandle;
}

/**
 * u2fs_get_registration_publicKey:
 * @result: a registration result obtained from u2fs_registration_verify()
 *
 * Extract the raw user public key obtained during the U2F
 * registration operation.  The memory is allocated by the library,
 * and must not be deallocated by the caller.  The returned buffer
 * pointer holds %U2FS_PUBLIC_KEY_LEN bytes.
 *
 * Returns: On success the pointer to the buffer containing the user public key
 * is returned, and on errors NULL.
 */
const char *u2fs_get_registration_publicKey(u2fs_reg_res_t * result)
{
  if (result == NULL)
    return NULL;

  return result->publicKey;
}

/**
 * u2fs_get_registration_attestation:
 * @result: a registration result obtained from u2fs_registration_verify()
 *
 * Extract the X509 attestation certificate (PEM format) obtained during the U2F
 * registration operation.  The memory is allocated by the library,
 * and must not be deallocated by the caller.
 *
 * Returns: On success the pointer to the buffer containing the attestation
 * certificate is returned, and on errors NULL.
 */
const char *u2fs_get_registration_attestation(u2fs_reg_res_t * result)
{
  if (result == NULL)
    return NULL;

  return (void*)result->attestation_certificate_PEM;
}

/**
 * u2fs_get_authentication_result:
 * @result: an authentication result obtained from u2fs_authentication_verify()
 * @verified: output parameter for the authentication result
 * @counter: output parameter for the counter value
 * @user_presence: output parameter for the user presence byte
 *
 * Unpack the authentication result obtained from a U2F authentication procedure
 * into its components. If any of the output parameters is set to NULL, that parameter
 * will be ignored.
 *
 * Returns: On success #U2FS_OK is returned, and on errors a #u2fs_rc error code.
 * The value @verified is set to #U2FS_OK on a successful authentication, and to 0 otherwise
 * @counter is filled with the value of the counter provided by the token.
 * A @user_presence value of 1 will determine the actual presence
 * of the user (yubikey touched) during the authentication.
 */
u2fs_rc u2fs_get_authentication_result(u2fs_auth_res_t * result,
                                       u2fs_rc *verified,
                                       uint32_t * counter,
                                       uint8_t * user_presence)
{
  if (result == NULL)
    return U2FS_MEMORY_ERROR;

  if (verified)
    *verified = result->verified;

  if (counter)
    *counter = result->counter;

  if (user_presence)
    *user_presence = result->user_presence;

  return U2FS_OK;
}

/**
 * u2fs_set_origin:
 * @ctx: a context handle, from u2fs_init()
 * @origin: the origin of a registration request
 *
 * Stores @origin within @ctx. If a value is already present, it is cleared and the memory is released.
 *
 * Returns: On success %U2FS_OK (integer 0) is returned, and on errors
 * a #u2fs_rc error code.
 */
u2fs_rc u2fs_set_origin(u2fs_ctx_t * ctx, const char *origin)
{
  if (ctx == NULL || origin == NULL)
    return U2FS_MEMORY_ERROR;

  if (ctx->origin != NULL) {
    free(ctx->origin);
    ctx->origin = NULL;
  }

  ctx->origin = strdup(origin);
  if (ctx->origin == NULL)
    return U2FS_MEMORY_ERROR;

  return U2FS_OK;
}

/**
 * u2fs_set_appid:
 * @ctx: a context handle, from u2fs_init()
 * @appid: the appid of a registration request
 *
 * Stores @appid within @ctx. If a value is already present, it is cleared and the memory is released.
 *
 * Returns: On success %U2FS_OK (integer 0) is returned, and on errors
 * a #u2fs_rc error code.
 */
u2fs_rc u2fs_set_appid(u2fs_ctx_t * ctx, const char *appid)
{
  if (ctx == NULL || appid == NULL)
    return U2FS_MEMORY_ERROR;

  if (ctx->appid != NULL) {
    free(ctx->appid);
    ctx->appid = NULL;
  }

  ctx->appid = strdup(appid);
  if (ctx->appid == NULL)
    return U2FS_MEMORY_ERROR;

  return U2FS_OK;
}

static int registration_challenge_json(const char *challenge,
                                       const char *appid, char **output)
{
  u2fs_rc rc = U2FS_JSON_ERROR;
  struct json_object *json_challenge = NULL;
  struct json_object *json_version = NULL;
  struct json_object *json_appid = NULL;
  struct json_object *json_output = NULL;
  const char *json_string = NULL;

  rc = U2FS_JSON_ERROR;

  json_challenge = json_object_new_string(challenge);
  if (json_challenge == NULL)
    goto done;
  json_version = json_object_new_string(U2F_VERSION);
  if (json_version == NULL)
    goto done;
  json_appid = json_object_new_string(appid);
  if (json_appid == NULL)
    goto done;

  json_output = json_object_new_object();
  if (json_output == NULL)
    goto done;

  json_object_object_add(json_output, "challenge", json_object_get(json_challenge));
  json_object_object_add(json_output, "version", json_object_get(json_version));
  json_object_object_add(json_output, "appId", json_object_get(json_appid));

  json_string = json_object_to_json_string(json_output);
  if (json_string == NULL)
    rc = U2FS_JSON_ERROR;
  else if ((*output = strdup(json_string)) == NULL)
    rc = U2FS_MEMORY_ERROR;
  else
    rc = U2FS_OK;

done:
    json_object_put(json_output);
    json_object_put(json_challenge);
    json_object_put(json_version);
    json_object_put(json_appid);

  return rc;
}

/**
 * u2fs_registration_challenge:
 * @ctx: a context handle, from u2fs_init()
 * @output: pointer to output string with JSON data of RegistrationData.
 *
 * Get a U2F RegistrationData JSON structure, used as the challenge in
 * a U2F device registration.
 *
 * Returns: On success %U2FS_OK (integer 0) is returned, and on errors
 * an #u2fs_rc error code.
 */
u2fs_rc u2fs_registration_challenge(u2fs_ctx_t * ctx, char **output)
{
  u2fs_rc rc = gen_challenge(ctx);
  if (rc != U2FS_OK)
    return rc;

  return registration_challenge_json(ctx->challenge, ctx->appid, output);
}

static u2fs_rc
parse_clientData(const char *clientData, char **challenge, char **origin)
{
  struct json_object *jo = json_tokener_parse(clientData);
  struct json_object *k;
  const char *p;

  if (clientData == NULL || challenge == NULL || origin == NULL)
    return U2FS_MEMORY_ERROR;

  if (jo == NULL)
    return U2FS_JSON_ERROR;

  if (u2fs_json_object_object_get(jo, "challenge", k) == FALSE)
    return U2FS_JSON_ERROR;

  p = json_object_get_string(k);
  if (p == NULL)
    return U2FS_JSON_ERROR;

  *challenge = strdup(p);
  if (*challenge == NULL)
    return U2FS_MEMORY_ERROR;

  if (u2fs_json_object_object_get(jo, "origin", k) == FALSE)
    return U2FS_JSON_ERROR;

  p = json_object_get_string(k);
  if (p == NULL)
    return U2FS_JSON_ERROR;

  *origin = strdup(p);
  if (*origin == NULL)
    return U2FS_JSON_ERROR;

  json_object_put(jo);

  return U2FS_OK;

}

/**
 * JSON decode
 */
static u2fs_rc
parse_registration_response(const char *response, char **registrationData,
                            char **clientData)
{
  struct json_object *jo;
  struct json_object *k;
  const char *p;

  jo = json_tokener_parse(response);
  if (jo == NULL)
    return U2FS_JSON_ERROR;

  if (u2fs_json_object_object_get(jo, "registrationData", k) == FALSE)
    return U2FS_JSON_ERROR;
  p = json_object_get_string(k);
  if (p == NULL)
    return U2FS_JSON_ERROR;
  *registrationData = strdup(p);
  if (*registrationData == NULL)
    return U2FS_MEMORY_ERROR;

  if (u2fs_json_object_object_get(jo, "clientData", k) == FALSE)
    return U2FS_JSON_ERROR;
  p = json_object_get_string(k);
  if (p == NULL)
    return U2FS_JSON_ERROR;
  *clientData = strdup(p);
  if (*clientData == NULL)
    return U2FS_MEMORY_ERROR;

  json_object_put(jo);

  return U2FS_OK;
}

static void dumpHex(const unsigned char *data, int offs, int len)
{
  int i;
  for (i = offs; i < len; i++) {
    if (i % 16 == 0)
      fprintf(stderr, "\n");
    fprintf(stderr, "%02x ", data[i] & 0xFF);
  }
  fprintf(stderr, "\n");
}

/**
 * Parse and validate the registration response.
 */
static u2fs_rc
parse_registrationData2(const unsigned char *data, size_t len,
                        unsigned char **user_public_key,
                        size_t * keyHandle_len, char **keyHandle,
                        u2fs_X509_t ** attestation_certificate,
                        u2fs_ECDSA_t ** signature)
{
  /*
     +-------------------------------------------------------------------+
     | 1 |     65    | 1 |    L    |    implied               | 64       |
     +-------------------------------------------------------------------+
     0x05
     public key
     key handle length
     key handle
     attestation cert
     signature
   */

  int offset = 0;
  size_t attestation_certificate_len;
  u2fs_rc rc;

  if (len <= 1 + 65 + 1 + 64) {
    if (debug)
      fprintf(stderr, "Length mismatch\n");
    return U2FS_FORMAT_ERROR;
  }

  if (data[offset++] != 0x05) {
    if (debug)
      fprintf(stderr, "Reserved byte mismatch\n");
    return U2FS_FORMAT_ERROR;
  }

  *user_public_key = calloc(sizeof(unsigned char), U2FS_PUBLIC_KEY_LEN);

  if (*user_public_key == NULL) {
    if (debug)
      fprintf(stderr, "Memory error\n");
    return U2FS_MEMORY_ERROR;
  }

  memcpy(*user_public_key, data + offset, U2FS_PUBLIC_KEY_LEN);

  offset += U2FS_PUBLIC_KEY_LEN;

  *keyHandle_len = data[offset++];

  *keyHandle = calloc(sizeof(char), *keyHandle_len);
  if (*keyHandle == NULL)
    return U2FS_MEMORY_ERROR;

  memcpy(*keyHandle, data + offset, *keyHandle_len);

  if (*keyHandle == NULL) {
    if (debug)
      fprintf(stderr, "Memory error\n");

    free(*user_public_key);

    keyHandle_len = 0;
    *user_public_key = NULL;

    return U2FS_MEMORY_ERROR;
  }

  if (debug)
    fprintf(stderr, "Key handle length: %d\n", (int) *keyHandle_len);

  offset += *keyHandle_len;

  // Skip over offset and offset+1 (0x30, 0x82 respecitvely)
  // Length is big-endian encoded in offset+3 and offset+4
  attestation_certificate_len =
      (data[offset + 2] << 8) + data[offset + 3] + 4;

  rc = decode_X509(data + offset, attestation_certificate_len,
                   attestation_certificate);

  if (rc != U2FS_OK) {
    return rc;
  }

  if (debug)
    dumpCert(*attestation_certificate);

  offset += attestation_certificate_len;

  size_t signature_len = len - offset;
  rc = decode_ECDSA(data + offset, signature_len, signature);

  if (rc != U2FS_OK) {
    free(*user_public_key);
    free(*keyHandle);

    *user_public_key = NULL;
    *keyHandle_len = 0;
    *keyHandle = NULL;

    if (debug)
      fprintf(stderr, "Unable to decode signature\n");

    return rc;
  }

  return U2FS_OK;
}

static u2fs_rc parse_registrationData(const char *registrationData,
                                      unsigned char **user_public_key,
                                      size_t * keyHandle_len,
                                      char **keyHandle,
                                      u2fs_X509_t **
                                      attestation_certificate,
                                      u2fs_ECDSA_t ** signature)
{
  base64_decodestate b64;
  size_t registrationData_len = strlen(registrationData);
  unsigned char *data;
  int data_len;
  u2fs_rc rc;

  data = malloc(registrationData_len + 1);
  if (data == NULL)
    return U2FS_MEMORY_ERROR;

  data[registrationData_len] = '\0';

  base64_init_decodestate(&b64);
  data_len =
      base64_decode_block(registrationData, registrationData_len,
                          (char *) data, &b64);

  if (debug) {
    fprintf(stderr, "registrationData Hex: ");
    dumpHex((unsigned char *) data, 0, data_len);
  }

  rc = parse_registrationData2(data, data_len,
                               user_public_key, keyHandle_len, keyHandle,
                               attestation_certificate, signature);

  free(data);
  data = NULL;

  return rc;
}

static u2fs_rc decode_clientData(const char *clientData, char **output)
{
  base64_decodestate b64;
  size_t clientData_len = strlen(clientData);
  char *data;
  u2fs_rc rc = 0;

  if (output == NULL)
    return U2FS_MEMORY_ERROR;

  data = calloc(sizeof(char), clientData_len);
  if (data == NULL)
    return U2FS_MEMORY_ERROR;

  base64_init_decodestate(&b64);
  base64_decode_block(clientData, clientData_len, data, &b64);

  if (debug) {
    fprintf(stderr, "clientData: %s\n", data);
  }

  *output = strndup(data, strlen(data));

  free(data);
  data = NULL;

  if (*output == NULL) {
    fprintf(stderr, "Memory Error\n");
    return U2FS_MEMORY_ERROR;
  }

  return rc;
}

/**
 * u2fs_registration_verify:
 * @ctx: a context handle, from u2fs_init().
 * @response: a U2F registration response message Base64 encoded.
 * @output: pointer to output structure containing the relevant data for a well formed request. Memory should be free'd.
 *
 * Get a U2F registration response and check its validity.
 *
 * Returns: On success %U2FS_OK (integer 0) is returned and @output is filled up with the user public key, the key handle and the attestation certificate. On errors
 * a #u2fs_rc error code.
 */
u2fs_rc u2fs_registration_verify(u2fs_ctx_t * ctx, const char *response,
                                 u2fs_reg_res_t ** output)
{
  char *registrationData;
  char *clientData;
  char *clientData_decoded;
  unsigned char *user_public_key;
  size_t keyHandle_len;
  char *keyHandle;
  char *origin;
  char *challenge;
  char buf[_B64_BUFSIZE];
  unsigned char c = 0;
  u2fs_X509_t *attestation_certificate;
  u2fs_ECDSA_t *signature;
  u2fs_EC_KEY_t *key;
  u2fs_rc rc;

  if (ctx == NULL || response == NULL || output == NULL)
    return U2FS_MEMORY_ERROR;

  key = NULL;
  clientData_decoded = NULL;
  challenge = NULL;
  origin = NULL;
  attestation_certificate = NULL;
  user_public_key = NULL;
  signature = NULL;
  registrationData = NULL;
  clientData = NULL;
  keyHandle = NULL;
  *output = NULL;

  rc = parse_registration_response(response, &registrationData,
                                   &clientData);
  if (rc != U2FS_OK)
    goto failure;

  if (debug) {
    fprintf(stderr, "registrationData: %s\n", registrationData);
    fprintf(stderr, "clientData: %s\n", clientData);
  }

  rc = parse_registrationData(registrationData, &user_public_key,
                              &keyHandle_len, &keyHandle,
                              &attestation_certificate, &signature);
  if (rc != U2FS_OK)
    goto failure;

  rc = extract_EC_KEY_from_X509(attestation_certificate, &key);

  if (rc != U2FS_OK)
    goto failure;

  //TODO Add certificate validation

  rc = decode_clientData(clientData, &clientData_decoded);

  if (rc != U2FS_OK)
    goto failure;

  rc = parse_clientData(clientData_decoded, &challenge, &origin);

  if (rc != U2FS_OK)
    goto failure;


  rc = gen_challenge(ctx);
  if (rc != U2FS_OK)
    goto failure;

  if (strcmp(ctx->challenge, challenge) != 0) {
    rc = U2FS_CHALLENGE_ERROR;
    goto failure;
  }

  if (strcmp(ctx->origin, origin) != 0) {
    rc = U2FS_ORIGIN_ERROR;
    goto failure;
  }

  struct sha256_state sha_ctx;
  char challenge_parameter[U2FS_HASH_LEN],
      application_parameter[U2FS_HASH_LEN];

  sha256_init(&sha_ctx);
  sha256_process(&sha_ctx, (unsigned char *) ctx->appid,
                 strlen(ctx->appid));
  sha256_done(&sha_ctx, (unsigned char *) application_parameter);

  sha256_init(&sha_ctx);
  sha256_process(&sha_ctx, (unsigned char *) clientData_decoded,
                 strlen(clientData_decoded));
  sha256_done(&sha_ctx, (unsigned char *) challenge_parameter);

  unsigned char dgst[U2FS_HASH_LEN];
  sha256_init(&sha_ctx);
  sha256_process(&sha_ctx, &c, 1);
  sha256_process(&sha_ctx, (unsigned char *) application_parameter,
                 U2FS_HASH_LEN);
  sha256_process(&sha_ctx, (unsigned char *) challenge_parameter,
                 U2FS_HASH_LEN);
  sha256_process(&sha_ctx, (unsigned char *) keyHandle, keyHandle_len);
  sha256_process(&sha_ctx, user_public_key, U2FS_PUBLIC_KEY_LEN);
  sha256_done(&sha_ctx, dgst);

  rc = verify_ECDSA(dgst, U2FS_HASH_LEN, signature, key);

  if (rc != U2FS_OK)
    goto failure;

  free_sig(signature);
  signature = NULL;

  *output = calloc(1, sizeof(**output));
  if (*output == NULL) {
    rc = U2FS_MEMORY_ERROR;
    goto failure;
  }

  rc = encode_b64u(keyHandle, keyHandle_len, buf);
  if (rc != U2FS_OK)
    goto failure;

  u2fs_EC_KEY_t *key_ptr;
  (*output)->keyHandle = strndup(buf, strlen(buf));

  rc = decode_user_key(user_public_key, &key_ptr);
  if (rc != U2FS_OK)
    goto failure;

  (*output)->attestation_certificate = dup_cert(attestation_certificate);

  rc = dump_user_key(key_ptr, &(*output)->publicKey);
  if (rc != U2FS_OK)
    goto failure;

  rc = dump_X509_cert(attestation_certificate, &(*output)->attestation_certificate_PEM);
  if (rc != U2FS_OK)
    goto failure;

  if ((*output)->keyHandle == NULL
      || (*output)->publicKey == NULL
      || (*output)->attestation_certificate == NULL) {
    rc = U2FS_MEMORY_ERROR;
    goto failure;
  }

  free_key(key);
  key = NULL;

  free_cert(attestation_certificate);
  attestation_certificate = NULL;

  free(clientData_decoded);
  clientData_decoded = NULL;

  free(challenge);
  challenge = NULL;

  free(origin);
  origin = NULL;

  free(user_public_key);
  user_public_key = NULL;

  free(registrationData);
  registrationData = NULL;

  free(clientData);
  clientData = NULL;

  free(keyHandle);
  keyHandle = NULL;

  return U2FS_OK;

failure:
  if (key) {
    free_key(key);
    key = NULL;
  }

  if (clientData_decoded) {
    free(clientData_decoded);
    clientData_decoded = NULL;
  }

  if (challenge) {
    free(challenge);
    challenge = NULL;
  }

  if (origin) {
    free(origin);
    origin = NULL;
  }

  if (attestation_certificate) {
    free_cert(attestation_certificate);
    attestation_certificate = NULL;
  }

  if (user_public_key) {
    free(user_public_key);
    user_public_key = NULL;
  }

  if (signature) {
    free_sig(signature);
    signature = NULL;
  }

  if (registrationData) {
    free(registrationData);
    registrationData = NULL;
  }

  if (clientData) {
    free(clientData);
    clientData = NULL;
  }

  if (keyHandle) {
    free(keyHandle);
    keyHandle = NULL;
  }

  return rc;
}

static int authentication_challenge_json(const char *challenge,
                                         const char *keyHandle,
                                         const char *appid, char **output)
{
  u2fs_rc rc = U2FS_JSON_ERROR;
  struct json_object *json_challenge = NULL;
  struct json_object *json_key = NULL;
  struct json_object *json_version = NULL;
  struct json_object *json_appid = NULL;
  struct json_object *json_output = NULL;
  const char *json_string = NULL;

  rc = U2FS_JSON_ERROR;

  json_key = json_object_new_string(keyHandle);
  if (json_key == NULL)
    goto done;
  json_version = json_object_new_string(U2F_VERSION);
  if (json_version == NULL)
    goto done;
  json_challenge = json_object_new_string(challenge);
  if (json_challenge == NULL)
    goto done;
  json_appid = json_object_new_string(appid);
  if (json_appid == NULL)
    goto done;

  json_output = json_object_new_object();
  if (json_output == NULL)
    goto done;

  json_object_object_add(json_output, "keyHandle", json_object_get(json_key));
  json_object_object_add(json_output, "version", json_object_get(json_version));
  json_object_object_add(json_output, "challenge", json_object_get(json_challenge));
  json_object_object_add(json_output, "appId", json_object_get(json_appid));

  json_string = json_object_to_json_string(json_output);

  if (json_string == NULL)
    rc = U2FS_JSON_ERROR;
  else if ((*output = strdup(json_string)) == NULL)
    rc = U2FS_MEMORY_ERROR;
  else
    rc = U2FS_OK;

done:
    json_object_put(json_output);
    json_object_put(json_challenge);
    json_object_put(json_key);
    json_object_put(json_version);
    json_object_put(json_appid);

  return rc;
}

static u2fs_rc
parse_signatureData2(const unsigned char *data, size_t len,
                     uint8_t * user_presence, uint32_t * counter,
                     u2fs_ECDSA_t ** signature)
{
  /*
     +-----------------------------------+
     | 1 |     4     |      implied      |
     +-----------------------------------+
     user presence
     counter
     signature
   */

  int offset = 0;
  u2fs_rc rc;

  if (len <= 1 + U2FS_COUNTER_LEN) {
    if (debug)
      fprintf(stderr, "Length mismatch\n");
    return U2FS_FORMAT_ERROR;
  }

  *user_presence = data[offset++] & 0x01;

  if (*user_presence == 0) {
    if (debug)
      fprintf(stderr, "User presence byte mismatch\n");
    return U2FS_FORMAT_ERROR;
  }

  memcpy((char *) counter, data + offset, U2FS_COUNTER_LEN);

  offset += U2FS_COUNTER_LEN;

  size_t signature_len = len - offset;
  rc = decode_ECDSA(data + offset, signature_len, signature);

  if (rc != U2FS_OK) {
    return rc;
  }

  return U2FS_OK;
}

static u2fs_rc
parse_signatureData(const char *signatureData, uint8_t * user_presence,
                    uint32_t * counter, u2fs_ECDSA_t ** signature)
{

  base64_decodestate b64;
  size_t signatureData_len = strlen(signatureData);
  unsigned char *data;
  int data_len;
  u2fs_rc rc;

  data = malloc(signatureData_len + 1);
  if (data == NULL)
    return U2FS_MEMORY_ERROR;

  data[signatureData_len] = '\0';

  base64_init_decodestate(&b64);
  data_len =
      base64_decode_block(signatureData, signatureData_len, (char *) data,
                          &b64);

  if (debug) {
    fprintf(stderr, "signatureData Hex: ");
    dumpHex((unsigned char *) data, 0, data_len);
  }

  rc = parse_signatureData2(data, data_len, user_presence, counter,
                            signature);

  free(data);
  data = NULL;

  return rc;
}

static u2fs_rc
parse_authentication_response(const char *response, char **signatureData,
                              char **clientData, char **keyHandle)
{
  struct json_object *jo;
  struct json_object *k;
  const char *p;

  jo = json_tokener_parse(response);
  if (jo == NULL)
    return U2FS_JSON_ERROR;

  if (u2fs_json_object_object_get(jo, "signatureData", k) == FALSE)
    return U2FS_JSON_ERROR;
  p = json_object_get_string(k);
  if (p == NULL)
    return U2FS_JSON_ERROR;
  *signatureData = strdup(p);
  if (*signatureData == NULL)
    return U2FS_MEMORY_ERROR;

  if (u2fs_json_object_object_get(jo, "clientData", k) == FALSE)
    return U2FS_JSON_ERROR;
  p = json_object_get_string(k);
  if (p == NULL)
    return U2FS_JSON_ERROR;
  *clientData = strdup(p);
  if (*clientData == NULL)
    return U2FS_MEMORY_ERROR;

  if (u2fs_json_object_object_get(jo, "keyHandle", k) == FALSE)
    return U2FS_JSON_ERROR;
  p = json_object_get_string(k);
  if (p == NULL)
    return U2FS_JSON_ERROR;
  *keyHandle = strdup(p);
  if (*keyHandle == NULL)
    return U2FS_MEMORY_ERROR;

  json_object_put(jo);

  return U2FS_OK;
}

/**
 * u2fs_authentication_verify:
 * @ctx: a context handle, from u2fs_init()
 * @response: pointer to output string with JSON data.
 * @output: pointer to output structure containing the relevant data for a well formed request. Memory should be free'd.
 *
 * Get a U2F authentication response and check its validity.
 *
 * Returns: On a successful verification %U2FS_OK (integer 0) is returned and @output is filled with the authentication result (same as the returned value), the counter received from the token and the user presence information. On errors
 * a #u2fs_rc error code is returned.
 */
u2fs_rc u2fs_authentication_verify(u2fs_ctx_t * ctx, const char *response,
                                   u2fs_auth_res_t ** output)
{
  char *signatureData;
  char *clientData;
  char *clientData_decoded;
  char *keyHandle;
  char *challenge;
  char *origin;
  uint8_t user_presence;
  uint32_t counter_num;
  uint32_t counter;
  u2fs_ECDSA_t *signature;
  u2fs_rc rc;

  if (ctx == NULL || response == NULL || output == NULL)
    return U2FS_MEMORY_ERROR;

  signatureData = NULL;
  clientData = NULL;
  clientData_decoded = NULL;
  keyHandle = NULL;
  challenge = NULL;
  origin = NULL;
  signature = NULL;
  *output = NULL;

  rc = parse_authentication_response(response, &signatureData,
                                     &clientData, &keyHandle);
  if (rc != U2FS_OK)
    goto failure;

  if (debug) {
    fprintf(stderr, "signatureData: %s\n", signatureData);
    fprintf(stderr, "clientData: %s\n", clientData);
    fprintf(stderr, "keyHandle: %s\n", keyHandle);
  }

  rc = parse_signatureData(signatureData, &user_presence,
                           &counter, &signature);
  if (rc != U2FS_OK)
    goto failure;

  rc = decode_clientData(clientData, &clientData_decoded);

  if (rc != U2FS_OK)
    goto failure;

  rc = parse_clientData(clientData_decoded, &challenge, &origin);

  if (rc != U2FS_OK)
    goto failure;

  if (strcmp(ctx->challenge, challenge) != 0) {
    rc = U2FS_CHALLENGE_ERROR;
    goto failure;
  }

  if (strcmp(ctx->origin, origin) != 0) {
    rc = U2FS_ORIGIN_ERROR;
    goto failure;
  }

  struct sha256_state sha_ctx;
  char challenge_parameter[U2FS_HASH_LEN],
      application_parameter[U2FS_HASH_LEN];

  sha256_init(&sha_ctx);
  sha256_process(&sha_ctx, (unsigned char *) ctx->appid,
                 strlen(ctx->appid));
  sha256_done(&sha_ctx, (unsigned char *) application_parameter);

  sha256_init(&sha_ctx);
  sha256_process(&sha_ctx, (unsigned char *) clientData_decoded,
                 strlen(clientData_decoded));
  sha256_done(&sha_ctx, (unsigned char *) challenge_parameter);

  unsigned char dgst[U2FS_HASH_LEN];
  sha256_init(&sha_ctx);
  sha256_process(&sha_ctx, (unsigned char *) application_parameter,
                 U2FS_HASH_LEN);
  sha256_process(&sha_ctx, (unsigned char *) &user_presence, 1);
  sha256_process(&sha_ctx, (unsigned char *) &counter, U2FS_COUNTER_LEN);
  sha256_process(&sha_ctx, (unsigned char *) challenge_parameter,
                 U2FS_HASH_LEN);
  sha256_done(&sha_ctx, dgst);

  rc = verify_ECDSA(dgst, U2FS_HASH_LEN, signature, ctx->key);

  if (rc != U2FS_OK)
    goto failure;

  free_sig(signature);
  signature = NULL;

  *output = calloc(1, sizeof(**output));
  if (*output == NULL) {
    rc = U2FS_MEMORY_ERROR;
    goto failure;
  }

  counter_num = 0;
  counter_num |= (counter & 0xFF000000) >> 24;
  counter_num |= (counter & 0x00FF0000) >> 8;
  counter_num |= (counter & 0x0000FF00) << 8;
  counter_num |= (counter & 0x000000FF) << 24;

  (*output)->verified = U2FS_OK;
  (*output)->user_presence = user_presence;
  (*output)->counter = counter_num;

  free(origin);
  origin = NULL;

  free(challenge);
  challenge = NULL;

  free(keyHandle);
  keyHandle = NULL;

  free(signatureData);
  signatureData = NULL;

  free(clientData);
  clientData = NULL;

  free(clientData_decoded);
  clientData_decoded = NULL;

  return U2FS_OK;

failure:
  if (clientData_decoded) {
    free(clientData_decoded);
    clientData_decoded = NULL;
  }

  if (challenge) {
    free(challenge);
    challenge = NULL;
  }

  if (origin) {
    free(origin);
    origin = NULL;
  }

  if (signature) {
    free_sig(signature);
    signature = NULL;
  }

  if (signatureData) {
    free(signatureData);
    signatureData = NULL;
  }

  if (clientData) {
    free(clientData);
    clientData = NULL;
  }

  if (keyHandle) {
    free(keyHandle);
    keyHandle = NULL;
  }

  return rc;
}

/**
 * u2fs_authentication_challenge:
 * @ctx: a context handle, from u2fs_init()
 * @output: pointer to output string with JSON data of AuthenticationData.
 *
 * Get a U2F AuthenticationData JSON structure, used as the challenge in
 * a U2F authentication procedure.
 *
 * Returns: On success %U2FS_OK (integer 0) is returned, and on errors
 * a #u2fs_rc error code.
 */
u2fs_rc u2fs_authentication_challenge(u2fs_ctx_t * ctx, char **output)
{
  u2fs_rc rc;

  if (ctx->keyHandle == NULL)
    return U2FS_MEMORY_ERROR;

  rc = gen_challenge(ctx);
  if (rc != U2FS_OK)
    return rc;

  return authentication_challenge_json(ctx->challenge,
                                       ctx->keyHandle, ctx->appid, output);
}
