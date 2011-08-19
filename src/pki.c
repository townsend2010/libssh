/*
 * known_hosts.c
 * This file is part of the SSH Library
 *
 * Copyright (c) 2010 by Aris Adamantiadis
 * Copyright (c) 2011-2011 Andreas Schneider <asn@cryptomilk.org>
 *
 * The SSH Library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * The SSH Library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the SSH Library; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

/**
 * @defgroup libssh_pki The SSH Public Key Infrastructure
 * @ingroup libssh
 *
 * Functions for the creation, importation and manipulation of public and
 * private keys in the context of the SSH protocol
 *
 * @{
 */

#include "config.h"

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "libssh/libssh.h"
#include "libssh/session.h"
#include "libssh/priv.h"
#include "libssh/pki.h"
#include "libssh/keys.h"
#include "libssh/buffer.h"
#include "libssh/misc.h"

void ssh_pki_log(const char *format, ...)
{
#ifdef DEBUG_CRYPTO
  char buffer[1024];
  va_list va;

    va_start(va, format);
    vsnprintf(buffer, sizeof(buffer), format, va);
    va_end(va);

    fprintf(stderr, "%s\n", buffer);
#else
    (void) format;
#endif
    return;
}

enum ssh_keytypes_e pki_privatekey_type_from_string(const char *privkey) {
    if (strncmp(privkey, DSA_HEADER_BEGIN, strlen(DSA_HEADER_BEGIN)) == 0) {
        return SSH_KEYTYPE_DSS;
    }

    if (strncmp(privkey, RSA_HEADER_BEGIN, strlen(RSA_HEADER_BEGIN)) == 0) {
        return SSH_KEYTYPE_RSA;
    }

    return SSH_KEYTYPE_UNKNOWN;
}

/**
 * @brief creates a new empty SSH key
 * @returns an empty ssh_key handle, or NULL on error.
 */
ssh_key ssh_key_new (void) {
  ssh_key ptr = malloc (sizeof (struct ssh_key_struct));
  if (ptr == NULL) {
      return NULL;
  }
  ZERO_STRUCTP(ptr);
  return ptr;
}

ssh_key ssh_key_dup(const ssh_key key)
{
    if (key == NULL) {
        return NULL;
    }

    return pki_key_dup(key, 0);
}

/**
 * @brief clean up the key and deallocate all existing keys
 * @param[in] key ssh_key to clean
 */
void ssh_key_clean (ssh_key key){
    if(key == NULL)
        return;
#ifdef HAVE_LIBGCRYPT
    if(key->dsa) gcry_sexp_release(key->dsa);
    if(key->rsa) gcry_sexp_release(key->rsa);
#elif defined HAVE_LIBCRYPTO
    if(key->dsa) DSA_free(key->dsa);
    if(key->rsa) RSA_free(key->rsa);
#endif
    key->flags=SSH_KEY_FLAG_EMPTY;
    key->type=SSH_KEYTYPE_UNKNOWN;
    key->type_c=NULL;
    key->dsa = NULL;
    key->rsa = NULL;
}

/**
 * @brief deallocate a SSH key
 * @param[in] key ssh_key handle to free
 */
void ssh_key_free (ssh_key key){
    if(key){
        ssh_key_clean(key);
        SAFE_FREE(key);
    }
}

/**
 * @brief returns the type of a ssh key
 * @param[in] key the ssh_key handle
 * @returns one of SSH_KEYTYPE_RSA,SSH_KEYTYPE_DSS,SSH_KEYTYPE_RSA1
 * @returns SSH_KEYTYPE_UNKNOWN if the type is unknown
 */
enum ssh_keytypes_e ssh_key_type(const ssh_key key){
    if (key == NULL) {
        return SSH_KEYTYPE_UNKNOWN;
    }
    return key->type;
}

/**
 * @brief Convert a key type to a string.
 *
 * @param[in]  type     The type to convert.
 *
 * @return              A string for the keytype or NULL if unknown.
 */
const char *ssh_key_type_to_char(enum ssh_keytypes_e type) {
  switch (type) {
    case SSH_KEYTYPE_DSS:
      return "ssh-dss";
    case SSH_KEYTYPE_RSA:
      return "ssh-rsa";
    case SSH_KEYTYPE_RSA1:
      return "ssh-rsa1";
    case SSH_KEYTYPE_ECDSA:
      return "ssh-ecdsa";
    case SSH_KEYTYPE_UNKNOWN:
      return NULL;
  }

  /* We should never reach this */
  return NULL;
}

/**
 * @brief Convert a ssh key name to a ssh key type.
 *
 * @param[in] name      The name to convert.
 *
 * @return              The enum ssh key type.
 */
enum ssh_keytypes_e ssh_key_type_from_name(const char *name) {
  if (strcmp(name, "rsa1") == 0) {
    return SSH_KEYTYPE_RSA1;
  } else if (strcmp(name, "rsa") == 0) {
    return SSH_KEYTYPE_RSA;
  } else if (strcmp(name, "dsa") == 0) {
    return SSH_KEYTYPE_DSS;
  } else if (strcmp(name, "ssh-rsa1") == 0) {
    return SSH_KEYTYPE_RSA1;
  } else if (strcmp(name, "ssh-rsa") == 0) {
    return SSH_KEYTYPE_RSA;
  } else if (strcmp(name, "ssh-dss") == 0) {
    return SSH_KEYTYPE_DSS;
  } else if (strcmp(name, "ssh-ecdsa") == 0
             || strcmp(name, "ecdsa") == 0
             || strcmp(name, "ecdsa-sha2-nistp256") == 0
             || strcmp(name, "ecdsa-sha2-nistp384") == 0
             || strcmp(name, "ecdsa-sha2-nistp521") == 0) {
  }

  return SSH_KEYTYPE_UNKNOWN;
}

/**
 * @brief Check if the key has/is a public key.
 *
 * @param[in] k         The key to check.
 *
 * @return              1 if it is a public key, 0 if not.
 */
int ssh_key_is_public(const ssh_key k) {
    if (k == NULL) {
        return 0;
    }

    return (k->flags & SSH_KEY_FLAG_PUBLIC);
}

/**
 * @brief Check if the key is a private key.
 *
 * @param[in] k         The key to check.
 *
 * @return              1 if it is a private key, 0 if not.
 */
int ssh_key_is_private(const ssh_key k) {
    if (k == NULL) {
        return 0;
    }

    return (k->flags & SSH_KEY_FLAG_PRIVATE);
}

/**
 * @brief import a base64 formated key from a memory c-string
 *
 * @param[in]  b64_key  The c-string holding the base64 encoded key
 *
 * @param[in]  passphrase The passphrase to decrypt the key, or NULL
 *
 * @param[in]  auth_fn  An auth function you may want to use or NULL.
 *
 * @param[in]  auth_data Private data passed to the auth function.
 *
 * @param[out] pkey     A pointer where the key can be stored. You need
 *                      to free the memory.
 *
 * @return  SSH_ERROR in case of error, SSH_OK otherwise.
 *
 * @see ssh_key_free()
 */
int ssh_pki_import_privkey_base64(const char *b64_key,
                                  const char *passphrase,
                                  ssh_auth_callback auth_fn,
                                  void *auth_data,
                                  ssh_key *pkey)
{
    ssh_key key;

    if (b64_key == NULL || pkey == NULL) {
        return SSH_ERROR;
    }

    if (b64_key == NULL || !*b64_key) {
        return SSH_ERROR;
    }

    ssh_pki_log("Trying to decode privkey passphrase=%s",
                passphrase ? "true" : "false");

    key = pki_private_key_from_base64(b64_key, passphrase, auth_fn, auth_data);
    if (key == NULL) {
        return SSH_ERROR;
    }

    *pkey = key;

    return SSH_OK;
}

/**
 * @brief Import a key from a file.
 *
 * @param[in]  filename The filename of the the private key.
 *
 * @param[in]  passphrase The passphrase to decrypt the private key. Set to NULL
 *                        if none is needed or it is unknown.
 *
 * @param[in]  auth_fn  An auth function you may want to use or NULL.
 *
 * @param[in]  auth_data Private data passed to the auth function.
 *
 * @param[out] pkey     A pointer to store the ssh_key. You need to free the
 *                      key.
 *
 * @returns SSH_OK on success, SSH_ERROR otherwise.
 *
 * @see ssh_key_free()
 **/
int ssh_pki_import_privkey_file(const char *filename,
                                const char *passphrase,
                                ssh_auth_callback auth_fn,
                                void *auth_data,
                                ssh_key *pkey) {
    struct stat sb;
    char *key_buf;
    ssh_key key;
    FILE *file;
    off_t size;
    int rc;

    if (pkey == NULL || filename == NULL || *filename == '\0') {
        return SSH_ERROR;
    }

    rc = stat(filename, &sb);
    if (rc < 0) {
        ssh_pki_log("Error gettint stat of %s: %s",
                    filename, strerror(errno));
        return SSH_ERROR;
    }

    file = fopen(filename, "r");
    if (file == NULL) {
        ssh_pki_log("Error opening %s: %s",
                    filename, strerror(errno));
        return SSH_ERROR;
    }

    key_buf = malloc(sb.st_size + 1);
    if (key_buf == NULL) {
        fclose(file);
        ssh_pki_log("Out of memory!");
        return SSH_ERROR;
    }

    size = fread(key_buf, 1, sb.st_size, file);
    fclose(file);

    if (size != sb.st_size) {
        SAFE_FREE(key_buf);
        ssh_pki_log("Error reading %s: %s",
                    filename, strerror(errno));
        return SSH_ERROR;
    }

    key = pki_private_key_from_base64(key_buf, passphrase, auth_fn, auth_data);
    SAFE_FREE(key_buf);
    if (key == NULL) {
        return SSH_ERROR;
    }

    *pkey = key;
    return SSH_OK;
}

/* temporary function to migrate seemlessly to ssh_key */
ssh_public_key ssh_pki_convert_key_to_publickey(const ssh_key key) {
    ssh_public_key pub;

    if(key == NULL) {
        return NULL;
    }

    pub = malloc(sizeof(struct ssh_public_key_struct));
    if (pub == NULL) {
        return NULL;
    }
    ZERO_STRUCTP(pub);

    pub->dsa_pub = key->dsa;
    pub->rsa_pub = key->rsa;
    pub->type     = key->type;
    pub->type_c   = key->type_c;

    return pub;
}

ssh_private_key ssh_pki_convert_key_to_privatekey(const ssh_key key) {
    ssh_private_key privkey;

    privkey = malloc(sizeof(struct ssh_private_key_struct));
    if (privkey == NULL) {
        ssh_key_free(key);
        return NULL;
    }

    privkey->type = key->type;
    privkey->dsa_priv = key->dsa;
    privkey->rsa_priv = key->rsa;

    return privkey;
}

static int pki_import_pubkey_buffer(ssh_buffer buffer,
                                    enum ssh_keytypes_e type,
                                    ssh_key *pkey) {
    ssh_key key;
    int rc;

    key = ssh_key_new();
    if (key == NULL) {
        return SSH_ERROR;
    }

    key->type = type;
    key->type_c = ssh_key_type_to_char(type);
    key->flags = SSH_KEY_FLAG_PUBLIC;

    switch (type) {
        case SSH_KEYTYPE_DSS:
            {
                ssh_string p;
                ssh_string q;
                ssh_string g;
                ssh_string pubkey;

                p = buffer_get_ssh_string(buffer);
                if (p == NULL) {
                    goto fail;
                }
                q = buffer_get_ssh_string(buffer);
                if (q == NULL) {
                    ssh_string_burn(p);
                    ssh_string_free(p);

                    goto fail;
                }
                g = buffer_get_ssh_string(buffer);
                if (g == NULL) {
                    ssh_string_burn(p);
                    ssh_string_free(p);
                    ssh_string_burn(q);
                    ssh_string_free(q);

                    goto fail;
                }
                pubkey = buffer_get_ssh_string(buffer);
                if (g == NULL) {
                    ssh_string_burn(p);
                    ssh_string_free(p);
                    ssh_string_burn(q);
                    ssh_string_free(q);
                    ssh_string_burn(g);
                    ssh_string_free(g);

                    goto fail;
                }

                rc = pki_pubkey_build_dss(key, p, q, g, pubkey);
#ifdef DEBUG_CRYPTO
                ssh_print_hexa("p", ssh_string_data(p), ssh_string_len(p));
                ssh_print_hexa("q", ssh_string_data(q), ssh_string_len(q));
                ssh_print_hexa("g", ssh_string_data(g), ssh_string_len(g));
#endif
                ssh_string_burn(p);
                ssh_string_free(p);
                ssh_string_burn(q);
                ssh_string_free(q);
                ssh_string_burn(g);
                ssh_string_free(g);
                ssh_string_burn(pubkey);
                ssh_string_free(pubkey);
                if (rc == SSH_ERROR) {
                    goto fail;
                }
            }
            break;
        case SSH_KEYTYPE_RSA:
        case SSH_KEYTYPE_RSA1:
            {
                ssh_string e;
                ssh_string n;

                e = buffer_get_ssh_string(buffer);
                if (e == NULL) {
                    goto fail;
                }
                n = buffer_get_ssh_string(buffer);
                if (n == NULL) {
                    ssh_string_burn(e);
                    ssh_string_free(e);

                    goto fail;
                }

                rc = pki_pubkey_build_rsa(key, e, n);
#ifdef DEBUG_CRYPTO
                ssh_print_hexa("e", ssh_string_data(e), ssh_string_len(e));
                ssh_print_hexa("n", ssh_string_data(n), ssh_string_len(n));
#endif
                ssh_string_burn(e);
                ssh_string_free(e);
                ssh_string_burn(n);
                ssh_string_free(n);
                if (rc == SSH_ERROR) {
                    goto fail;
                }
            }
            break;
        case SSH_KEYTYPE_ECDSA:
        case SSH_KEYTYPE_UNKNOWN:
            ssh_pki_log("Unknown public key protocol %d", type);
            goto fail;
    }

    *pkey = key;
    return SSH_OK;
fail:
    ssh_key_free(key);

    return SSH_ERROR;
}

/**
 * @brief Import a base64 formated public key from a memory c-string.
 *
 * @param[in]  b64_key  The base64 key to format.
 *
 * @param[in]  type     The type of the key to format.
 *
 * @param[out] pkey     A pointer where the key can be stored. You need
 *                      to free the memory.
 *
 * @return              SSH_OK on success, SSH_ERROR on error.
 *
 * @see ssh_key_free()
 */
int ssh_pki_import_pubkey_base64(const char *b64_key,
                                 enum ssh_keytypes_e type,
                                 ssh_key *pkey) {
    ssh_buffer buffer;
    ssh_string type_s;
    int rc;

    if (b64_key == NULL || pkey == NULL) {
        return SSH_ERROR;
    }

    buffer = base64_to_bin(b64_key);
    if (buffer == NULL) {
        return SSH_ERROR;
    }

    type_s = buffer_get_ssh_string(buffer);
    if (type_s == NULL) {
        ssh_buffer_free(buffer);
        return SSH_ERROR;
    }
    ssh_string_free(type_s);

    rc = pki_import_pubkey_buffer(buffer, type, pkey);
    ssh_buffer_free(buffer);

    return rc;
}

/**
 * @internal
 *
 * @brief Import a public key from a ssh string.
 *
 * @param[in]  key_blob The key blob to import as specified in RFC 4253 section
 *                      6.6 "Public Key Algorithms".
 *
 * @param[out] pkey     A pointer where the key can be stored. You need
 *                      to free the memory.
 *
 * @return              SSH_OK on success, SSH_ERROR on error.
 *
 * @see ssh_key_free()
 */
int ssh_pki_import_pubkey_blob(const ssh_string key_blob,
                               ssh_key *pkey) {
    ssh_buffer buffer;
    ssh_string type_s = NULL;
    char *type_c = NULL;
    int rc;

    if (key_blob == NULL || pkey == NULL) {
        return SSH_ERROR;
    }

    buffer = ssh_buffer_new();
    if (buffer == NULL) {
        ssh_pki_log("Out of memory!");
        return SSH_ERROR;
    }

    rc = buffer_add_data(buffer, ssh_string_data(key_blob),
            ssh_string_len(key_blob));
    if (rc < 0) {
        ssh_pki_log("Out of memory!");
        goto fail;
    }

    type_s = buffer_get_ssh_string(buffer);
    if (type_s == NULL) {
        ssh_pki_log("Out of memory!");
        goto fail;
    }

    type_c = ssh_string_to_char(type_s);
    if (type_c == NULL) {
        ssh_pki_log("Out of memory!");
        goto fail;
    }
    ssh_string_free(type_s);

    rc = pki_import_pubkey_buffer(buffer, ssh_key_type_from_name(type_c), pkey);

    ssh_buffer_free(buffer);
    free(type_c);

    return rc;
fail:
    ssh_buffer_free(buffer);
    ssh_string_free(type_s);
    free(type_c);

    return SSH_ERROR;
}

int ssh_pki_import_pubkey_file(const char *filename, ssh_key *pkey)
{
    enum ssh_keytypes_e type;
    struct stat sb;
    char *key_buf, *p;
    const char *q;
    FILE *file;
    off_t size;
    int rc;

    if (pkey == NULL || filename == NULL || *filename == '\0') {
        return SSH_ERROR;
    }

    rc = stat(filename, &sb);
    if (rc < 0) {
        ssh_pki_log("Error gettint stat of %s: %s",
                    filename, strerror(errno));
        return SSH_ERROR;
    }

    file = fopen(filename, "r");
    if (file == NULL) {
        ssh_pki_log("Error opening %s: %s",
                    filename, strerror(errno));
        return SSH_ERROR;
    }

    key_buf = malloc(sb.st_size + 1);
    if (key_buf == NULL) {
        fclose(file);
        ssh_pki_log("Out of memory!");
        return SSH_ERROR;
    }

    size = fread(key_buf, 1, sb.st_size, file);
    fclose(file);

    if (size != sb.st_size) {
        SAFE_FREE(key_buf);
        ssh_pki_log("Error reading %s: %s",
                    filename, strerror(errno));
        return SSH_ERROR;
    }

    q = p = key_buf;
    while (!isspace((int)*p)) p++;
    *p = '\0';

    type = ssh_key_type_from_name(q);
    if (type == SSH_KEYTYPE_UNKNOWN) {
        SAFE_FREE(key_buf);
        return SSH_ERROR;
    }
    q = ++p;
    while (!isspace((int)*p)) p++;
    *p = '\0';

    rc = ssh_pki_import_pubkey_base64(q, type, pkey);
    SAFE_FREE(key_buf);

    return rc;
}

/**
 * @brief Generate and duplicate a public key from a private key.
 *
 * @param[in] privkey   The private key to get the public key from.
 *
 * @return              A public key, NULL on error.
 */
ssh_key ssh_pki_publickey_from_privatekey(const ssh_key privkey) {

    if (privkey == NULL || !ssh_key_is_private(privkey)) {
        return NULL;
    }

    return pki_key_dup(privkey, 1);
}

/**
 * @brief Create a key_blob from a public key.
 *
 * The "key_blob" is encoded as per RFC 4253 section 6.6 "Public Key
 * Algorithms" for any of the supported protocol 2 key types.
 *
 * @param[in] key       A public or private key to create the public ssh_string
 *                      from.
 *
 * @return              The key_blob or NULL on error.
 */
ssh_string ssh_pki_export_pubkey_blob(const ssh_key key)
{
    if (key == NULL) {
        return NULL;
    }

    return pki_publickey_to_blob(key);
}

/**
 * @brief Convert a public key to a base64 hased key.
 *
 * @param[in] key       The key to hash
 *
 * @param[out] b64_key  A pointer to store the base64 hased key.
 *
 * @return              SSH_OK on success, SSH_ERROR on error.
 *
 * @see ssh_string_free_char()
 */
int ssh_pki_export_pubkey_base64(const ssh_key key,
                                 char **b64_key)
{
    ssh_string key_blob;
    unsigned char *b64;

    if (key == NULL || b64_key == NULL) {
        return SSH_ERROR;
    }

    key_blob = pki_publickey_to_blob(key);
    if (key_blob == NULL) {
        return SSH_ERROR;
    }

    b64 = bin_to_base64(ssh_string_data(key_blob), ssh_string_len(key_blob));
    ssh_string_free(key_blob);
    if (b64 == NULL) {
        return SSH_ERROR;
    }

    *b64_key = (char *)b64;

    return SSH_OK;
}

int ssh_pki_export_pubkey_file(const ssh_key key,
                               const char *filename)
{
    char key_buf[4096];
    char host[256];
    char *b64_key;
    char *user;
    FILE *fp;
    int rc;

    if (key == NULL || filename == NULL || *filename == '\0') {
        return SSH_ERROR;
    }

    user = ssh_get_local_username();
    if (user == NULL) {
        return SSH_ERROR;
    }

    rc = gethostname(host, sizeof(host));
    if (rc < 0) {
        free(user);
        return SSH_ERROR;
    }

    rc = ssh_pki_export_pubkey_base64(key, &b64_key);
    if (rc < 0) {
        free(user);
        return SSH_ERROR;
    }

    rc = snprintf(key_buf, sizeof(key_buf),
                  "%s %s %s@%s\n",
                  key->type_c,
                  b64_key,
                  user,
                  host);
    free(user);
    free(b64_key);
    if (rc < 0) {
        return SSH_ERROR;
    }

    fp = fopen(filename, "w+");
    if (fp == NULL) {
        return SSH_ERROR;
    }
    rc = fwrite(key_buf, strlen(key_buf), 1, fp);
    if (rc != 1 || ferror(fp)) {
        fclose(fp);
        unlink(filename);
        return SSH_ERROR;
    }
    fclose(fp);

    return SSH_OK;
}

/*
 * This function signs the session id (known as H) as a string then
 * the content of sigbuf */
ssh_string ssh_pki_do_sign(ssh_session session, ssh_buffer sigbuf,
        const ssh_key privatekey) {
    struct ssh_crypto_struct *crypto = session->current_crypto ? session->current_crypto :
        session->next_crypto;
    unsigned char hash[SHA_DIGEST_LEN + 1] = {0};
    ssh_string session_str = NULL;
    ssh_string signature = NULL;
    struct signature_struct *sign = NULL;
    SHACTX ctx = NULL;

    if (privatekey == NULL || !ssh_key_is_private(privatekey)) {
        return NULL;
    }

    session_str = ssh_string_new(SHA_DIGEST_LEN);
    if (session_str == NULL) {
        return NULL;
    }
    ssh_string_fill(session_str, crypto->session_id, SHA_DIGEST_LEN);

    ctx = sha1_init();
    if (ctx == NULL) {
        ssh_string_free(session_str);
        return NULL;
    }

    sha1_update(ctx, session_str, ssh_string_len(session_str) + 4);
    ssh_string_free(session_str);
    sha1_update(ctx, buffer_get_rest(sigbuf), buffer_get_rest_len(sigbuf));
    sha1_final(hash + 1,ctx);
    hash[0] = 0;

#ifdef DEBUG_CRYPTO
    ssh_print_hexa("Hash being signed with dsa", hash + 1, SHA_DIGEST_LEN);
#endif

    sign = pki_do_sign(privatekey, hash);
    if (sign == NULL) {
        return NULL;
    }

    signature = signature_to_string(sign);
    signature_free(sign);

    return signature;
}


/**
 * @}
 */
