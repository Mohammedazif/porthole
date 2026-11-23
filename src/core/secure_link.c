#include "core/secure_link.h"
#include "core/config.h"
#include "core/metrics.h"
#include "core/sock.h"
#include "core/upath.h"
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <pthread.h>
#ifndef _WIN32
#include <signal.h>
#endif

static pthread_once_t once = PTHREAD_ONCE_INIT;
static SSL_CTX *identity;
static char fingerprint[65];
static _Thread_local SSL *connection;
static _Thread_local struct {
    bool (*verify)(void *, const char *);
    void *user;
    char ticket[65];
} channel;

static bool install_identity(const char *path)
{
    EVP_PKEY_CTX *gen = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    EVP_PKEY *key = NULL;
    X509 *cert = X509_new();
    unsigned char serial[16];
    X509_NAME *name = X509_NAME_new();
    bool ok = false;
    if (!name || !gen || !cert || EVP_PKEY_keygen_init(gen) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(gen, NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(gen, &key) <= 0 || RAND_bytes(serial, sizeof serial) != 1)
        goto done;
    BIGNUM *number = BN_bin2bn(serial, sizeof serial, NULL);
    bool configured = number && BN_to_ASN1_INTEGER(number, X509_get_serialNumber(cert));
    BN_free(number);
    if (!configured || !X509_set_version(cert, 2) ||
        !X509_gmtime_adj(X509_getm_notBefore(cert), -300) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert), 315360000L) || !X509_set_pubkey(cert, key))
        goto done;
    if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"Porthole", -1,
                                    -1, 0) ||
        !X509_set_subject_name(cert, name) || !X509_set_issuer_name(cert, name) ||
        !X509_sign(cert, key, EVP_sha256()))
        goto done;
    char temporary[1200];
    if (snprintf(temporary, sizeof temporary, "%s-%02x%02x%02x%02x.tmp", path, serial[0], serial[1],
                 serial[2], serial[3]) >= (int)sizeof temporary)
        goto done;
    FILE *out = ph_fopen(temporary, "wbx");
    if (!out)
        goto done;
    bool written = true;
#ifndef _WIN32
    written = fchmod(fileno(out), 0600) == 0;
#endif
    written = written && PEM_write_PrivateKey(out, key, NULL, NULL, 0, NULL, NULL) == 1 &&
              PEM_write_X509(out, cert) == 1;
    if (fclose(out) != 0)
        written = false;
    if (written) {
#ifdef _WIN32
        wchar_t *from = ph_wide(temporary), *to = ph_wide(path);
        ok = from && to && MoveFileW(from, to);
        free(from);
        free(to);
#else
        ok = link(temporary, path) == 0;
#endif
    }
    ph_remove(temporary);
    if (written && ph_path_exists(path))
        ok = true;
done:
    X509_NAME_free(name);
    X509_free(cert);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(gen);
    return ok;
}
static void load_identity(void)
{
    char path[1024];
    if (!ph_config_path("desktop-identity.pem", path, sizeof path))
        return;
    if (!ph_path_exists(path) && !install_identity(path))
        return;
    FILE *file = ph_fopen(path, "rb");
    if (!file)
        return;
    EVP_PKEY *key = PEM_read_PrivateKey(file, NULL, NULL, NULL);
    X509 *cert = PEM_read_X509(file, NULL, NULL, NULL);
    fclose(file);
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    unsigned char digest[32];
    unsigned size = 0;
    if (ctx && key && cert && SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) &&
        SSL_CTX_use_certificate(ctx, cert) == 1 && SSL_CTX_use_PrivateKey(ctx, key) == 1 &&
        SSL_CTX_check_private_key(ctx) == 1 && X509_digest(cert, EVP_sha256(), digest, &size) &&
        size == 32) {
        for (size_t i = 0; i < size; i++)
            snprintf(fingerprint + 2 * i, 3, "%02x", digest[i]);
        SSL_CTX_set_num_tickets(ctx, 0);
        identity = ctx;
    } else
        SSL_CTX_free(ctx);
    X509_free(cert);
    EVP_PKEY_free(key);
}
const char *ph_tls_fingerprint(void)
{
    pthread_once(&once, load_identity);
    return fingerprint;
}

static bool wait_tls(SSL *session, int result, uint64_t deadline)
{
    int error = SSL_get_error(session, result);
    if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
        return false;
    uint64_t now = ph_metrics_now_ms();
    if (now >= deadline)
        return false;
    struct pollfd ready = {.fd = SSL_get_fd(session),
                           .events = error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT};
    return ph_poll(&ready, 1, (int)(deadline - now)) > 0 &&
           (ready.revents & ready.events) != 0;
}

bool ph_tls_accept(int fd)
{
    if (connection || !*ph_tls_fingerprint())
        return false;
#ifndef _WIN32
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);
#endif
#ifdef _WIN32
    u_long enabled = 1;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &enabled) != 0)
        return false;
#else
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return false;
#endif
    SSL *session = SSL_new(identity);
    if (session && SSL_set_fd(session, fd)) {
        uint64_t deadline = ph_metrics_now_ms() + 5000;
        do {
            ERR_clear_error();
            int rc = SSL_accept(session);
            if (rc == 1) {
                connection = session;
                return true;
            }
            if (!wait_tls(session, rc, deadline))
                break;
        } while (ph_metrics_now_ms() < deadline);
    }
    SSL_free(session);
    ERR_clear_error();
    return false;
}
void ph_tls_end(void)
{
    if (connection)
        SSL_shutdown(connection);
    SSL_free(connection);
    connection = NULL;
    OPENSSL_cleanse(&channel, sizeof channel);
    ERR_clear_error();
}
int ph_tls_read(int fd, void *data, int len)
{
    return ph_tls_read_until(fd, data, len, ph_metrics_now_ms() + 5000);
}
int ph_tls_read_until(int fd, void *data, int len, uint64_t deadline)
{
    if (!connection || SSL_get_fd(connection) != fd)
        return -1;
    while (ph_metrics_now_ms() < deadline) {
        ERR_clear_error();
        int rc = SSL_read(connection, data, len);
        if (rc > 0 || !wait_tls(connection, rc, deadline))
            return rc;
    }
    return -1;
}
int ph_tls_write(int fd, const void *data, int len)
{
    if (!connection || SSL_get_fd(connection) != fd)
        return -1;
    uint64_t deadline = ph_metrics_now_ms() + 5000;
    while (ph_metrics_now_ms() < deadline) {
        ERR_clear_error();
        int rc = SSL_write(connection, data, len);
        if (rc > 0 || !wait_tls(connection, rc, deadline))
            return rc;
    }
    return -1;
}
bool ph_tls_pending(int fd)
{
    return connection && SSL_get_fd(connection) == fd && SSL_pending(connection) > 0;
}
bool ph_tls_authorize(int fd, bool (*verify)(void *, const char *), void *user)
{
    char header[71];
    uint64_t deadline = ph_metrics_now_ms() + 5000;
    for (size_t i = 0; i < 70; i++)
        if (ph_tls_read_until(fd, header + i, 1, deadline) != 1)
            return false;
    if (memcmp(header, "AUTH ", 5) || header[69] != '\n')
        return false;
    header[69] = 0;
    if (!verify || !verify(user, header + 5) || ph_tls_write(fd, "OK\n", 3) != 3)
        return false;
    channel.verify = verify;
    channel.user = user;
    memcpy(channel.ticket, header + 5, 65);
    return true;
}

bool ph_tls_authorized(void)
{
    return connection && channel.verify && channel.verify(channel.user, channel.ticket);
}

const char *ph_tls_ticket(void)
{
    return connection && channel.verify ? channel.ticket : NULL;
}
