/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The server's TLS identity (server_tls.h).
 */

#include "server_tls.h"

#include "net.h"
#include "persist.h"
#include "tls_common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>

#define CERT_VALID_DAYS 3650

static SSL_CTX *g_ctx;
static char g_fingerprint[TLS_FINGERPRINT_LEN];
static char g_cert_path[STATE_DIR_MAX + 32];

SSL_CTX *server_tls_ctx(void) {
    return g_ctx;
}

const char *server_tls_fingerprint(void) {
    return g_fingerprint;
}

const char *server_tls_cert_path(void) {
    return g_cert_path;
}

void server_tls_cleanup(void) {
    SSL_CTX_free(g_ctx);
    g_ctx = NULL;
    g_fingerprint[0] = '\0';
    g_cert_path[0] = '\0';
}

/* --- Self-signed certificate ------------------------------------------------ */

/* PEM of `write` into a new file created with mode 0600 (key) or 0644
 * (certificate) next to `path`, then renamed over it. */
static bool write_pem(const char *path, mode_t mode, bool (*emit)(FILE *, void *), void *obj,
                      char *err, size_t err_len) {
    char tmp[STATE_DIR_MAX + 48];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    (void)unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0) {
        snprintf(err, err_len, "cannot create %s: %s", tmp, strerror(errno));
        return false;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        (void)unlink(tmp);
        snprintf(err, err_len, "cannot write %s: %s", tmp, strerror(errno));
        return false;
    }
    bool ok = emit(f, obj);
    ok = (fflush(f) == 0) && ok && (fsync(fileno(f)) == 0);
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        snprintf(err, err_len, "cannot write %s: %s", path, strerror(errno));
        (void)unlink(tmp);
        return false;
    }
    return true;
}

static bool write_key(FILE *f, void *key) {
    return PEM_write_PrivateKey(f, (EVP_PKEY *)key, NULL, NULL, 0, NULL, NULL) == 1;
}

static bool write_cert(FILE *f, void *cert) {
    return PEM_write_X509(f, (X509 *)cert) == 1;
}

static bool add_ext(X509 *cert, int nid, const char *value) {
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, cert, cert, NULL, NULL, 0);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &v3, nid, value);
    bool ok = ext && X509_add_ext(cert, ext, -1) == 1;
    X509_EXTENSION_free(ext);
    return ok;
}

/* A self-signed certificate for this host: CN and subjectAltName are the
 * host name (and localhost), a random 128-bit serial, 10 years. */
static X509 *make_certificate(EVP_PKEY *key) {
    char host[256] = "localhost";
    if (gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0') {
        copy_str(host, "localhost", sizeof(host));
    }
    host[sizeof(host) - 1] = '\0';
    X509 *cert = X509_new();
    BIGNUM *serial = BN_new();
    X509_NAME *name = X509_NAME_new();
    bool ok = cert && serial && name && X509_set_version(cert, 2) &&
              BN_rand(serial, 128, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) &&
              BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(cert)) &&
              X509_gmtime_adj(X509_getm_notBefore(cert), -3600) &&
              X509_gmtime_adj(X509_getm_notAfter(cert), (long)CERT_VALID_DAYS * 24 * 3600) &&
              X509_set_pubkey(cert, key);
    ok = ok &&
         X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)"Dragon GL", -1,
                                    -1, 0) &&
         X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)host, -1, -1,
                                    0) &&
         X509_set_subject_name(cert, name) &&
         X509_set_issuer_name(cert, name);
    X509_NAME_free(name);
    char san[300];
    snprintf(san, sizeof(san), strcmp(host, "localhost") ? "DNS:%s,DNS:localhost" : "DNS:%s", host);
    ok = ok && add_ext(cert, NID_basic_constraints, "critical,CA:FALSE") &&
         add_ext(cert, NID_key_usage, "critical,digitalSignature") &&
         add_ext(cert, NID_ext_key_usage, "serverAuth") &&
         add_ext(cert, NID_subject_alt_name, san) &&
         add_ext(cert, NID_subject_key_identifier, "hash") && X509_sign(cert, key, EVP_sha256());
    BN_free(serial);
    if (!ok) {
        X509_free(cert);
        return NULL;
    }
    return cert;
}

static bool generate_identity(const char *cert_path, const char *key_path, char *err,
                              size_t err_len) {
    EVP_PKEY *key = EVP_EC_gen("P-256");
    X509 *cert = key ? make_certificate(key) : NULL;
    if (!cert) {
        tls_error_text(err, err_len, "cannot create the TLS key and certificate");
        EVP_PKEY_free(key);
        return false;
    }
    bool ok = write_pem(key_path, 0600, write_key, key, err, err_len) &&
              write_pem(cert_path, 0644, write_cert, cert, err, err_len);
    X509_free(cert);
    EVP_PKEY_free(key);
    return ok;
}

/* --- Loading -------------------------------------------------------------- */

static void check_key_mode(const char *key_path, char *warn, size_t warn_len) {
    struct stat st;
    if (stat(key_path, &st) == 0 && (st.st_mode & (S_IRWXG | S_IRWXO))) {
        snprintf(warn, warn_len, "%s is readable by group/others (chmod 600 it)", key_path);
    }
}

bool server_tls_init(const char *cert_file, const char *key_file, char *err, size_t err_len,
                     char *warn, size_t warn_len) {
    err[0] = '\0';
    warn[0] = '\0';
    server_tls_cleanup();
    if ((cert_file == NULL) != (key_file == NULL)) {
        snprintf(err, err_len, "--tls-cert and --tls-key go together");
        return false;
    }
    char cert_path[STATE_DIR_MAX + 32], key_path[STATE_DIR_MAX + 32];
    if (cert_file) {
        copy_str(cert_path, cert_file, sizeof(cert_path));
        copy_str(key_path, key_file, sizeof(key_path));
    } else {
        char dir[STATE_DIR_MAX + 32];
        if (!state_path(dir, sizeof(dir), "tls") ||
            !state_path(cert_path, sizeof(cert_path), "tls/server.crt") ||
            !state_path(key_path, sizeof(key_path), "tls/server.key")) {
            snprintf(err, err_len, "state directory path too long");
            return false;
        }
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            snprintf(err, err_len, "cannot create %s: %s", dir, strerror(errno));
            return false;
        }
        bool have_cert = access(cert_path, F_OK) == 0, have_key = access(key_path, F_OK) == 0;
        if (have_cert != have_key) {
            snprintf(err, err_len, "%s without %s: restore it, or remove both for a new identity",
                     have_cert ? cert_path : key_path, have_cert ? key_path : cert_path);
            return false;
        }
        if (!have_cert && !generate_identity(cert_path, key_path, err, err_len)) {
            return false;
        }
    }
    check_key_mode(key_path, warn, warn_len);

    SSL_CTX *ctx = tls_new_context(true);
    if (!ctx) {
        tls_error_text(err, err_len, "cannot create the TLS context");
        return false;
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1) {
        char why[160];
        tls_error_text(why, sizeof(why), "unreadable");
        snprintf(err, err_len, "certificate %s: %s", cert_path, why);
        SSL_CTX_free(ctx);
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        char why[160];
        tls_error_text(why, sizeof(why), "unreadable");
        snprintf(err, err_len, "private key %s: %s", key_path, why);
        SSL_CTX_free(ctx);
        return false;
    }
    if (!tls_cert_fingerprint(SSL_CTX_get0_certificate(ctx), g_fingerprint,
                              sizeof(g_fingerprint))) {
        snprintf(err, err_len, "cannot compute the certificate fingerprint");
        SSL_CTX_free(ctx);
        return false;
    }
    g_ctx = ctx;
    copy_str(g_cert_path, cert_path, sizeof(g_cert_path));
    return true;
}
