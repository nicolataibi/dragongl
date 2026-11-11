/*
 * DRAGON GL - Unit tests: TLS helpers and the known_servers file
 * (include/tls_common.h, src/client/known_servers.h).
 *
 * Fingerprint spelling and normalization, the ALPN wire form, and the
 * trust-on-first-use store: lookup, replacement of a server's line,
 * comments and other servers kept, file and directory modes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/x509.h>

#include "known_servers.h"
#include "protocol.h"
#include "tls_common.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

#define FP_A "0e0daDF641be57431a7ac70669b74ac3fbc5d833d0699e09c183b9d93a8236c9"

static void test_alpn(void) {
    char want[16];
    snprintf(want, sizeof(want), "dragongl/%d", PROTOCOL_VERSION);
    CHECK(TLS_ALPN_LEN == 1 + strlen(want));
    CHECK(TLS_ALPN[0] == strlen(want));
    CHECK(memcmp(TLS_ALPN + 1, want, strlen(want)) == 0);
}

static void test_normalize(void) {
    char hex[TLS_FINGERPRINT_HEX_LEN];
    CHECK(tls_fingerprint_normalize(FP_A, hex));
    CHECK(strcmp(hex, "0e0dadf641be57431a7ac70669b74ac3fbc5d833d0699e09c183b9d93a8236c9") == 0);
    CHECK(tls_fingerprint_normalize("SHA256:0E:0D:AD:F6:41:BE:57:43:1A:7A:C7:06:69:B7:4A:C3:FB:"
                                    "C5:D8:33:D0:69:9E:09:C1:83:B9:D9:3A:82:36:C9",
                                    hex));
    CHECK(strcmp(hex, "0e0dadf641be57431a7ac70669b74ac3fbc5d833d0699e09c183b9d93a8236c9") == 0);
    CHECK(tls_fingerprint_normalize("sha256:" FP_A, hex));
    CHECK(!tls_fingerprint_normalize(FP_A "00", hex)); /* 33 bytes */
    CHECK(hex[0] == '\0');
    CHECK(!tls_fingerprint_normalize("0e0d", hex));
    CHECK(!tls_fingerprint_normalize(
        "zz0dadf641be57431a7ac70669b74ac3fbc5d833d0699e09c183b9d93a8236c9", hex));
    CHECK(!tls_fingerprint_normalize("", hex));
    CHECK(!tls_fingerprint_normalize(NULL, hex));
}

/* The fingerprint is SHA-256 over the DER certificate, "SHA256:AB:..". */
static void test_cert_fingerprint(void) {
    EVP_PKEY *key = EVP_EC_gen("P-256");
    X509 *cert = X509_new();
    CHECK(key && cert);
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 7);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
    X509_set_pubkey(cert, key);
    X509_NAME *name = X509_NAME_new();
    CHECK(name);
    CHECK(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"test", -1,
                                     -1, 0));
    CHECK(X509_set_subject_name(cert, name));
    CHECK(X509_set_issuer_name(cert, name));
    X509_NAME_free(name);
    CHECK(X509_sign(cert, key, EVP_sha256()) > 0);

    unsigned char *der = NULL;
    int der_len = i2d_X509(cert, &der);
    unsigned char md[32];
    unsigned int md_len = 0;
    CHECK(der_len > 0 && EVP_Digest(der, (size_t)der_len, md, &md_len, EVP_sha256(), NULL));
    char want[TLS_FINGERPRINT_LEN];
    size_t at = (size_t)snprintf(want, sizeof(want), "SHA256:");
    for (unsigned int i = 0; i < md_len; i++) {
        at += (size_t)snprintf(want + at, sizeof(want) - at, i ? ":%02X" : "%02X", md[i]);
    }
    char got[TLS_FINGERPRINT_LEN];
    CHECK(tls_cert_fingerprint(cert, got, sizeof(got)));
    CHECK(strcmp(got, want) == 0);
    CHECK(strlen(got) == TLS_FINGERPRINT_LEN - 1);
    CHECK(!tls_cert_fingerprint(cert, got, 10)); /* too small */
    CHECK(!tls_cert_fingerprint(NULL, got, sizeof(got)));
    OPENSSL_free(der);
    X509_free(cert);
    EVP_PKEY_free(key);
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *read_file(const char *path) {
    static char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f) {
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

static void test_known_servers(void) {
    char tmpl[] = "/tmp/dragongl-tls-test-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (!dir) {
        return;
    }
    setenv("XDG_CONFIG_HOME", dir, 1);
    char path[512];
    CHECK(known_servers_path(path, sizeof(path)));
    char want_path[512];
    snprintf(want_path, sizeof(want_path), "%s/dragongl/known_servers", dir);
    CHECK(strcmp(path, want_path) == 0);
    setenv("XDG_CONFIG_HOME", "relative/ignored", 1);
    setenv("HOME", dir, 1);
    char home_path[512];
    CHECK(known_servers_path(home_path, sizeof(home_path)));
    CHECK(strstr(home_path, "/.config/dragongl/known_servers") != NULL);

    const char *a = "0e0dadf641be57431a7ac70669b74ac3fbc5d833d0699e09c183b9d93a8236c9";
    const char *b = "1111111111111111111111111111111111111111111111111111111111111111";
    int line = -1;
    CHECK(known_servers_check(path, "game.example", 8080, a, &line) == KS_UNKNOWN);
    char why[256];
    CHECK(known_servers_set(path, "game.example", 8080, a, why, sizeof(why)));
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    char parent[512];
    snprintf(parent, sizeof(parent), "%s/dragongl", dir);
    CHECK(stat(parent, &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK(known_servers_check(path, "GAME.example", 8080, a, &line) == KS_MATCH);
    CHECK(known_servers_check(path, "game.example", 8081, a, &line) == KS_UNKNOWN);
    CHECK(known_servers_check(path, "game.example", 8080, b, &line) == KS_MISMATCH);
    CHECK(line == 3); /* after the two comment lines of a new file */

    /* Other servers and comments stay; the server's line is replaced. */
    char text[1024];
    snprintf(text, sizeof(text),
             "# mine\n"
             "other.example 9000 %s\n"
             "game.example 8080 %s   # old\n"
             "\n"
             "::1 8080 %s\n",
             b, a, a);
    write_file(path, text);
    CHECK(known_servers_check(path, "::1", 8080, a, &line) == KS_MATCH && line == 5);
    CHECK(known_servers_set(path, "game.example", 8080, b, why, sizeof(why)));
    const char *now = read_file(path);
    CHECK(strstr(now, "# mine\n") != NULL);
    CHECK(strstr(now, "other.example 9000") != NULL);
    CHECK(strstr(now, "::1 8080") != NULL);
    CHECK(strstr(now, "# old") == NULL);
    CHECK(known_servers_check(path, "game.example", 8080, b, &line) == KS_MATCH);

    /* Malformed lines are ignored, not taken for a server. */
    write_file(path, "game.example\ngame.example 8080\ngame.example x " FP_A "\n");
    CHECK(known_servers_check(path, "game.example", 8080, a, &line) == KS_UNKNOWN);

    CHECK(unlink(path) == 0);
    CHECK(rmdir(parent) == 0);
    CHECK(rmdir(dir) == 0);
}

int main(void) {
    test_alpn();
    test_normalize();
    test_cert_fingerprint();
    test_known_servers();
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("tls: all checks passed\n");
    return 0;
}
