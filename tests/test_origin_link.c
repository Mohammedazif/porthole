/* The iPhone app's link, end to end over loopback: the code, clipboard sync, and
 * two files queued through the local Send to port arriving in order, with progress
 * and one result each. Requires the local companion ports to be available. */
#include <SDL.h>

#include "core/bonjour.h"
#include "core/metrics.h"
#include "core/sock.h"
#include "core/secure_link.h"
#include "broadcast/screen.h"
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <signal.h>
#include "core/upath.h"
#include "files/origin.h"
#include "files/inbox.h"
#include "files/places.h"
#include "session/devices.h"
#include "temp_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
static void nap(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void nap(int ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static struct {
    int fd;
    SSL *ssl;
} links[4];
static char ticket[65];

/* DNS-SD has separate platform tests. The TLS and file checks require only loopback. */
ph_bonjour *ph_bonjour_advertise(const char *name, const char *type, uint16_t port,
                                  const void *txt, uint16_t txt_len)
{
    (void)name;
    (void)type;
    (void)port;
    (void)txt;
    (void)txt_len;
    return NULL;
}

void ph_bonjour_stop(ph_bonjour *bonjour)
{
    (void)bonjour;
}

void ph_bonjour_host_name(char *out, size_t len)
{
    snprintf(out, len, "Porthole Test PC");
}

static SSL *peer_ssl(int fd)
{
    for (size_t i = 0; i < 4; i++)
        if (links[i].fd == fd && links[i].ssl)
            return links[i].ssl;
    return NULL;
}
static int read_peer(int fd, void *buf, int len, int flags)
{
    (void)flags;
    return SSL_read(peer_ssl(fd), buf, len);
}
static void close_peer(int fd)
{
    for (size_t i = 0; i < 4; i++)
        if (links[i].fd == fd && links[i].ssl) {
            SSL_free(links[i].ssl);
            links[i].ssl = NULL;
            break;
        }
    ph_sock_close(fd);
}
static int dial(unsigned short port)
{
    struct sockaddr_in addr;
    int fd = ph_sock_open();

    CHECK(fd >= 0);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    CHECK(connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    ph_sock_timeout(fd, 5);
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    CHECK(ctx && SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION));
    SSL *ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);
    CHECK(ssl && SSL_set_fd(ssl, fd) && SSL_connect(ssl) == 1);
    unsigned char digest[32];
    unsigned count = 0;
    char pin[65];
    X509 *cert = SSL_get_peer_certificate(ssl);
    CHECK(cert && X509_digest(cert, EVP_sha256(), digest, &count) && count == 32);
    X509_free(cert);
    for (size_t i = 0; i < 32; i++)
        snprintf(pin + 2 * i, 3, "%02x", digest[i]);
    CHECK(!strcmp(pin, ph_tls_fingerprint()));
    for (size_t i = 0; i < 4; i++)
        if (!links[i].ssl) {
            links[i].fd = fd;
            links[i].ssl = ssl;
            return fd;
        }
    CHECK(false);
    return -1;
}

static void line(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        CHECK(read_peer(fd, &c, 1, 0) == 1);
        if (c == '\n')
            break;
        buf[n++] = c;
    }
    buf[n] = 0;
}

static void say(int fd, const char *text)
{
    CHECK(SSL_write(peer_ssl(fd), text, (int)strlen(text)) == (int)strlen(text));
}

static void write_file(const char *path, size_t size)
{
    FILE *f = ph_fopen(path, "wb");
    CHECK(f);
    for (size_t i = 0; i < size; i++)
        fputc((int)(i * 7 % 251), f);
    CHECK(fclose(f) == 0);
}

/* Reads one PUSH and its bytes, checks them, and answers SAVED. */
static void take_push(int fd, const char *name, size_t size, bool update_controls)
{
    char head[256], want[256];
    size_t left = size, at = 0;

    line(fd, head, sizeof head);
    snprintf(want, sizeof want, "PUSH %zu %s", size, name);
    CHECK(!strcmp(head, want));
    while (left) {
        char chunk[4096];
        int got = (int)read_peer(fd, chunk, (int)(left < sizeof chunk ? left : sizeof chunk), 0);
        CHECK(got > 0);
        for (int i = 0; i < got; i++, at++)
            CHECK((unsigned char)chunk[i] == (unsigned char)(at * 7 % 251));
        left -= (size_t)got;
    }
    if (update_controls)
        say(fd, "INPUT 00000000-0000-0000-0000-000000000001 1 1\n");
    say(fd, "SAVED\n");
    if (update_controls) {
        line(fd, head, sizeof head);
        CHECK(!strcmp(head, "INPUT OK 00000000-0000-0000-0000-000000000001 1 1"));
    }
}

static int reconnect_phone(const char *token)
{
    int fd = dial(PH_PULL_PORT);
    char hello[64], response[256];
    snprintf(hello, sizeof hello, "HELLO %s\n", token);
    say(fd, hello);
    line(fd, response, sizeof response);
    CHECK(sscanf(response, "OK %64[0-9a-f]", ticket) == 1 && strlen(ticket) == 64);
    line(fd, response, sizeof response);
    CHECK(!strcmp(response, "SYNC READY"));
    line(fd, response, sizeof response);
    CHECK(!strcmp(response, "INPUT READY"));
    say(fd, "FILES 2\nRESUME?\n");
    line(fd, response, sizeof response);
    CHECK(!strcmp(response, "RESUME 2"));
    return fd;
}

static void test_control_verification(ph_origin *origin, int fd)
{
    const char id[] = "c9dea7da-5298-4b1f-a099-f23a47d3689a";
    const char code[] = "2021109678345219";
    char message[96], reply[96];
    say(fd, "IV?\n");
    line(fd, reply, sizeof reply);
    CHECK(!strcmp(reply, "IV READY"));
    CHECK(!ph_origin_controls_verified(origin));

    snprintf(message, sizeof message, "IV BEGIN %s\n", id);
    say(fd, message);
    for (int i = 0; i < 100 && !ph_origin_verifying_controls(origin); i++)
        nap(10);
    CHECK(ph_origin_issue_control_proof(origin, code));
    CHECK(ph_origin_control_proof_current(origin, code));
    CHECK(!ph_origin_issue_control_proof(origin, code));
    snprintf(message, sizeof message, "IV PROOF %s %s\n", id, code);
    say(fd, message);
    line(fd, reply, sizeof reply);
    snprintf(message, sizeof message, "IV OK %s", id);
    CHECK(!strcmp(reply, message));
    CHECK(ph_origin_controls_verified(origin));
    CHECK(!ph_origin_control_proof_current(origin, code));

    snprintf(message, sizeof message, "IV CANCEL %s\n", id);
    say(fd, message);
    line(fd, reply, sizeof reply);
    CHECK(!strcmp(reply, "IV RESET") && !ph_origin_controls_verified(origin));
    snprintf(message, sizeof message, "IV PROOF %s %s\n", id, code);
    say(fd, message);
    line(fd, reply, sizeof reply);
    snprintf(message, sizeof message, "IV FAILED %s", id);
    CHECK(!strcmp(reply, message) && !ph_origin_controls_verified(origin));
}

static void expect_result(ph_origin *o, int expected)
{
    for (int i = 0; i < 100; i++) {
        int result = ph_origin_take_push(o, NULL, 0);
        if (result) {
            CHECK(result == expected);
            return;
        }
        nap(20);
    }
    CHECK(false);
}
static void test_resume_send(ph_origin *o, const char *path, const char *token)
{
    char first[300], second[300], chunk[256];
    int fd = reconnect_phone(token);
    CHECK(ph_origin_put_file(o, path));
    line(fd, first, sizeof first);
    CHECK(!strncmp(first, "PUSH2 ", 6));
    char id[33], response[100];
    CHECK(sscanf(first, "PUSH2 %32s", id) == 1);
    ph_origin_send retry;
    CHECK(ph_origin_take_send(o, &retry) == PH_SEND_QUEUED);
    CHECK(ph_origin_take_send(o, &retry) == PH_SEND_ACTIVE);
    CHECK(!strcmp(retry.id, id) && strlen(retry.digest) == 64);
    snprintf(response, sizeof response, "PENDING %s\nOFFSET %s 0\n", id, id);
    say(fd, response);
    CHECK(read_peer(fd, chunk, 251, 0) > 0);
    close_peer(fd);
    expect_result(o, 2);
    char moved[1024];
    CHECK(snprintf(moved, sizeof moved, "%s.moved", path) < (int)sizeof moved);
    CHECK(ph_rename(path, moved) == 0);
    memcpy(retry.path, moved, strlen(moved) + 1);
    fd = reconnect_phone(token);
    CHECK(ph_origin_retry_file(o, &retry, false));
    line(fd, second, sizeof second);
    CHECK(!strcmp(first, second));
    snprintf(response, sizeof response, "OFFSET %s 251\n", id);
    say(fd, response);
    for (size_t i = 251; i < 1234;) {
        int n = (int)read_peer(fd, chunk, (int)SDL_min(sizeof chunk, 1234 - i), 0);
        CHECK(n > 0);
        for (int j = 0; j < n; j++, i++)
            CHECK((unsigned char)chunk[j] == i * 7 % 251);
    }
    close_peer(fd);
    expect_result(o, 2);
    fd = reconnect_phone(token);
    CHECK(ph_origin_retry_file(o, &retry, false));
    line(fd, second, sizeof second);
    CHECK(!strcmp(first, second));
    snprintf(response, sizeof response, "SAVED %s\n", id);
    say(fd, response);
    expect_result(o, 1);
    CHECK(ph_rename(moved, path) == 0);
    CHECK(ph_origin_put_file(o, path));
    line(fd, second, sizeof second);
    CHECK(strcmp(first, second));
    close_peer(fd);
    expect_result(o, 2);
    write_file(path, 1200);
    fd = reconnect_phone(token);
    CHECK(ph_origin_put_file(o, path));
    expect_result(o, 4);
    CHECK(ph_origin_restart_file(o, path));
    line(fd, first, sizeof first);
    CHECK(strcmp(first, second));
    close_peer(fd);
    expect_result(o, 2);
}
static int open_channel(int port, const char *credential)
{
    int fd = dial((unsigned short)port);
    char linebuf[96];
    snprintf(linebuf, sizeof linebuf, "AUTH %s\n", credential);
    say(fd, linebuf);
    return fd;
}
static int channel(int port)
{
    char answer[6];
    int fd = open_channel(port, ticket);
    line(fd, answer, sizeof answer);
    CHECK(!strcmp(answer, "OK"));
    return fd;
}
static bool verify_ticket(void *origin, const char *key)
{
    return ph_origin_authorize(origin, key);
}

static void test_screen_channel(ph_origin *origin)
{
    ph_screen_callbacks cb = {.user = origin, .authorize = verify_ticket};
    ph_screen *server = ph_screen_start(&cb);
    CHECK(server);
    int fd = open_channel(PH_SCREEN_PORT,
                          "0000000000000000000000000000000000000000000000000000000000000000");
    char ack;
    CHECK(read_peer(fd, &ack, 1, 0) <= 0);
    close_peer(fd);
    fd = channel(PH_SCREEN_PORT);
    say(fd, "PSCR 1\n");
    unsigned char frame[] = {0, 0, 0, 2, 2, 0x65, 0x07};
    CHECK(SSL_write(peer_ssl(fd), frame, sizeof frame) == sizeof frame);
    CHECK(read_peer(fd, &ack, 1, 0) == 1 && ack == 'A');
    /* Leave the authenticated connection open after a frame. The receiver
     * must close it before our own 16-second receive timeout. */
    ph_sock_timeout(fd, 16);
    ERR_clear_error();
    uint64_t started = ph_metrics_now_ms();
    int result = read_peer(fd, &ack, 1, 0);
    CHECK(result <= 0 && SSL_get_error(peer_ssl(fd), result) != SSL_ERROR_WANT_READ);
    CHECK(ph_metrics_now_ms() - started < 15500);
    close_peer(fd);
    ph_screen_stop(server);
}
static void test_chunked_send(ph_origin *origin, int fd, const char *path)
{
    char reply[256], transfer[33], command[128];
    say(fd, "FILES 3\nINBOX?\n");
    line(fd, reply, sizeof reply);
    CHECK(!strcmp(reply, "INBOX 3"));
    CHECK(ph_origin_put_file(origin, path));
    line(fd, reply, sizeof reply);
    CHECK(sscanf(reply, "PUSH3 %32s", transfer) == 1);
    snprintf(command, sizeof command,
             "OFFSET %s 0\nINPUT 00000000-0000-0000-0000-000000000009 0 1\n", transfer);
    say(fd, command);
    unsigned long long count = 0;
    bool acknowledged = false;
    while (count < 300000) {
        line(fd, reply, sizeof reply);
        if (!strcmp(reply, "INPUT OK 00000000-0000-0000-0000-000000000009 0 1")) {
            acknowledged = true;
            continue;
        }
        int length;
        CHECK(sscanf(reply, "CHUNK %d", &length) == 1 && length > 0 && length <= 65536);
        char chunk[4096];
        while (length > 0) {
            int got = (int)read_peer(fd, chunk, length < (int)sizeof chunk ? length : (int)sizeof chunk, 0);
            CHECK(got > 0);
            for (int i = 0; i < got; i++, count++)
                CHECK((unsigned char)chunk[i] == count * 7 % 251);
            length -= got;
        }
    }
    CHECK(count == 300000 && acknowledged);
    snprintf(command, sizeof command, "SAVED %s\n", transfer);
    say(fd, command);
    expect_result(origin, 1);
}

static bool receive_profile(void *user, const char *credential, char out[65])
{
    return ph_origin_receive_profile(user, credential, out);
}

static void test_resume_receive(const char *dir, ph_origin *origin)
{
    const char *header =
        "PSHR 3 1\nITEM 0123456789abcdef0123456789abcdef "
        "72399361da6a7754fec986dca5b7cbaf1c810a28ded4abaf56b2106d06cb78b0 10 inbox.txt\n";
    CHECK(ph_place_set_save_dir(dir));
    ph_inbox_callbacks callbacks = {
        .user = origin, .authorize = verify_ticket, .profile = receive_profile};
    ph_inbox *inbox = ph_inbox_start(dir, "Test", &callbacks);
    CHECK(inbox);
    char response[100], path[1200];
    int fd = channel(PH_INBOX_PORT);
    say(fd, header);
    line(fd, response, sizeof response);
    CHECK(!strcmp(response, "OFFSET 0"));
    say(fd, "abc");
    close_peer(fd);
    fd = channel(PH_INBOX_PORT);
    say(fd, header);
    line(fd, response, sizeof response);
    CHECK(!strcmp(response, "OFFSET 3"));
    say(fd, "defghij");
    line(fd, response, sizeof response);
    CHECK(!strcmp(response, "OK"));
    close_peer(fd);
    fd = channel(PH_INBOX_PORT);
    say(fd, header);
    line(fd, response, sizeof response);
    CHECK(!strcmp(response, "SAVED"));
    say(fd, "DONE\n");
    line(fd, response, sizeof response);
    CHECK(!strcmp(response, "BYE"));
    snprintf(path, sizeof path, "%s/inbox.txt", dir);
    FILE *f = ph_fopen(path, "rb");
    CHECK(f && fread(response, 1, 10, f) == 10 && !memcmp(response, "abcdefghij", 10));
    fclose(f);
    snprintf(path, sizeof path, "%s/2-inbox.txt", dir);
    CHECK(!ph_path_exists(path));
    close_peer(fd);
    ph_inbox_stop(inbox);
}

static void reject_credential(const char *credential)
{
    char hello[64], byte;
    int fd = dial(PH_PULL_PORT);
    snprintf(hello, sizeof hello, "HELLO %s\n", credential);
    say(fd, hello);
    CHECK(read_peer(fd, &byte, 1, 0) <= 0);
    close_peer(fd);
}

static ph_origin *test_pairing(ph_origin *origin, const char *token, const char *key)
{
    char response[128], hint[96];
    int fd = reconnect_phone(key);
    snprintf(response, sizeof response, "PAIR %s\n", key);
    say(fd, response);
    line(fd, response, sizeof response);
    CHECK(!strcmp(response, "PAIRED") && !ph_origin_code_open(origin));
    close_peer(fd);
    reject_credential(token);

    char first_id[65], next_id[65], second_code[16];
    CHECK(ph_origin_selected_id(origin, first_id));
    CHECK(ph_origin_add_phone(origin, hint, sizeof hint));
    CHECK(sscanf(hint, "%*s %15s", second_code) == 1 && strlen(second_code) == 8);
    const char *second_key = "3d3a8b4897277353ff91bfda11fc4d5d";
    fd = reconnect_phone(second_code);
    snprintf(response, sizeof response, "PAIR %s\n", second_key);
    say(fd, response);
    /* A durable pairing also survives losing its acknowledgement. */
    for (int attempt = 0; attempt < 100 && !ph_origin_selected_id(origin, next_id); attempt++)
        nap(10);
    CHECK(ph_origin_selected_id(origin, next_id));
    close_peer(fd);
    reject_credential(second_code);
    fd = reconnect_phone(second_key);
    CHECK(strcmp(first_id, next_id));
    CHECK(ph_origin_rename_phone(origin, first_id, "Saved test phone"));
    CHECK(ph_origin_choose_phone(origin, first_id));
    CHECK(read_peer(fd, response, 1, 0) <= 0);
    close_peer(fd);
    reject_credential(second_key);

    ph_origin_stop(origin);
    origin = ph_origin_start(hint, sizeof hint, NULL);
    CHECK(origin && !ph_origin_code_open(origin));
    ph_session_state state = {0};
    ph_origin_sessions(origin, &state);
    CHECK(state.count == 2 && state.devices[0].active && !state.devices[1].active);
    CHECK(!strcmp(state.devices[0].name, "Saved test phone"));
    CHECK(ph_origin_choose_phone(origin, next_id));
    fd = reconnect_phone(second_key);
    close_peer(fd);
    CHECK(ph_origin_choose_phone(origin, first_id));
    CHECK(ph_origin_remove_saved_phone(origin, next_id));
    reject_credential(second_key);

    fd = reconnect_phone(key);
    ph_origin_forget(origin);
    CHECK(read_peer(fd, response, 1, 0) <= 0);
    close_peer(fd);
    fd = dial(PH_PULL_PORT);
    snprintf(response, sizeof response, "HELLO %s\n", key);
    say(fd, response);
    line(fd, response, sizeof response);
    CHECK(!strcmp(response, "DISCONNECTED"));
    close_peer(fd);
    fd = reconnect_phone(key);
    CHECK(ph_origin_renew(origin, hint, sizeof hint) && ph_origin_code_open(origin));
    close_peer(fd);
    reject_credential(key);
    reject_credential(token);
    CHECK(sscanf(hint, "%*s %15s", second_code) == 1 && strlen(second_code) == 8);
    fd = reconnect_phone(second_code);
    snprintf(response, sizeof response, "PAIR %s\n", key);
    say(fd, response);
    CHECK(read_peer(fd, response, 1, 0) <= 0);
    close_peer(fd);
    reject_credential(key);
    return origin;
}

int main(int argc, char **argv)
{
    char dir[1024], hint[96], token[16] = "", reply[256], name[96], path_a[1200], path_b[1200];
    char *paths[2] = {path_a, path_b};
    ph_origin *o;
    int phone, results[2], percent_seen = -1;

    (void)argc;
    (void)argv;

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    CHECK(ph_test_temp_dir(dir, sizeof dir));
    char blocked[1200];
    snprintf(blocked, sizeof blocked, "%s/blocked-app-data", dir);
    FILE *file = ph_fopen(blocked, "wb");
    CHECK(file && fclose(file) == 0);
#ifdef _WIN32
    _putenv_s("APPDATA", blocked);
#else
    setenv("XDG_CONFIG_HOME", blocked, 1);
#endif
    ph_origin_start_status status;
    CHECK(!ph_origin_start(hint, sizeof hint, &status) && !hint[0] &&
          status == PH_ORIGIN_START_IDENTITY);
    CHECK(ph_remove(blocked) == 0);
#ifdef _WIN32
    _putenv_s("APPDATA", dir);
#else
    setenv("XDG_CONFIG_HOME", dir, 1);
#endif
    int occupied = ph_sock_listen(PH_PULL_PORT, false, 1);
    CHECK(occupied >= 0);
    CHECK(!ph_origin_start(hint, sizeof hint, &status) && !hint[0] &&
          status == PH_ORIGIN_START_LISTENER);
    ph_sock_close(occupied);
    o = ph_origin_start(hint, sizeof hint, &status);
    CHECK(o && status == PH_ORIGIN_START_OK);
    CHECK(sscanf(hint, "%*s %15s", token) == 1 && strlen(token) == 8);
    CHECK(!ph_origin_put_file(o, "/does/not/exist"));

    /* A wrong code is turned away; the right one is welcomed with OK and SYNC. */
    phone = dial(PH_PULL_PORT);
    say(phone, "HELLO 00000000\n");
    CHECK(read_peer(phone, reply, 1, 0) <= 0);
    close_peer(phone);

    /* Losing the connection after OK must leave the advertised code usable. */
    phone = dial(PH_PULL_PORT);
    snprintf(reply, sizeof reply, "HELLO %s\n", token);
    say(phone, reply);
    line(phone, reply, sizeof reply);
    CHECK(!strncmp(reply, "OK ", 3));
    CHECK(ph_origin_code_open(o));
    close_peer(phone);

    phone = dial(PH_PULL_PORT);
    snprintf(reply, sizeof reply, "HELLO %s\n", token);
    say(phone, reply);
    line(phone, reply, sizeof reply);
    CHECK(sscanf(reply, "OK %64[0-9a-f]", ticket) == 1 && strlen(ticket) == 64);
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "SYNC READY"));
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "INPUT READY"));
    const char *key = "9b60dadcd5a15f46fd9183e0b144db65";
    snprintf(reply, sizeof reply, "PAIR %s\n", key);
    say(phone, reply);
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "PAIRED") && !ph_origin_code_open(o));
    say(phone, "NAME Zoë’s iPhone\nNAME iPhone\n");
    CHECK(!ph_origin_put_clip(o, 'T', "disabled", 8));
    const char *request = "1eecc050-8818-4079-9fb0-2e0143e55d96";
    snprintf(reply, sizeof reply, "SYNC %s 1\nFILES\n", request);
    say(phone, reply);
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "SYNC OK 1eecc050-8818-4079-9fb0-2e0143e55d96 1"));
    char device_name[64];
    ph_origin_phone_name(o, device_name, sizeof device_name);
    CHECK(!strcmp(device_name, "Zoë’s iPhone"));
    nap(400);
    CHECK(ph_origin_clip_flags(o) & PH_ORIGIN_CLIP_READY);
    CHECK(ph_origin_clip_flags(o) & PH_ORIGIN_CLIP_READY);

    /* Authenticated input preferences are acknowledged and can change live. */
    say(phone, "NAME  iPhone\nCLIP T 5\nphone");
    say(phone, "INPUT 00000000-0000-0000-0000-000000000001 1 1\n");
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "INPUT OK 00000000-0000-0000-0000-000000000001 1 1"));
    ph_origin_phone_name(o, device_name, sizeof device_name);
    CHECK(!strcmp(device_name, "iPhone"));
    unsigned clipboard_flags = ph_origin_clip_flags(o);
    CHECK((clipboard_flags & (PH_ORIGIN_CLIP_READY | PH_ORIGIN_CLIP_GOT)) ==
          (PH_ORIGIN_CLIP_READY | PH_ORIGIN_CLIP_GOT));
    CHECK(ph_origin_clip_flags(o) == PH_ORIGIN_CLIP_READY);
    char kind;
    void *clipboard;
    size_t clipboard_size;
    CHECK(ph_origin_take_clip(o, &kind, &clipboard, &clipboard_size));
    CHECK(kind == 'T' && clipboard_size == 5 && !memcmp(clipboard, "phone", 5));
    free(clipboard);
    ph_control_policy policy = ph_origin_control_policy(o);
    CHECK(policy.managed && policy.connected && policy.enabled && policy.screen_only);
    CHECK(!ph_control_connection_wanted(policy, false, false, false));
    test_control_verification(o, phone);
    say(phone, "INPUT 00000000-0000-0000-0000-000000000001 1 0\n");
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "INPUT OK 00000000-0000-0000-0000-000000000001 1 0"));
    CHECK(ph_control_connection_wanted(ph_origin_control_policy(o), false, false, false));
    say(phone, "INPUT 00000000-0000-0000-0000-000000000001 0 0\n");
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "INPUT OK 00000000-0000-0000-0000-000000000001 0 0"));
    CHECK(!ph_control_connection_wanted(ph_origin_control_policy(o), true, true, false));

    /* Two files through the local port, as Explorer's Send to hands them over. */
    snprintf(path_a, sizeof path_a, "%s/Q3 report.pdf", dir);
    snprintf(path_b, sizeof path_b, "%s/Résumé.txt", dir);
    write_file(path_a, 300000);
    write_file(path_b, 1234);
    CHECK(ph_place_send_to_running(2, paths) == 2);
    CHECK(ph_origin_push_busy(o, name, sizeof name));
    take_push(phone, "Q3 report.pdf", 300000, true);
    CHECK(ph_origin_control_policy(o).enabled);
    take_push(phone, "Résumé.txt", 1234, false);
    for (int i = 0, got = 0; i < 50 && got < 2; i++) {
        int note = ph_origin_take_push(o, name, sizeof name);
        int pct = ph_origin_push_percent(o);
        if (pct > percent_seen)
            percent_seen = pct;
        if (note)
            results[got++] = note;
        else
            nap(50);
        if (got == 2)
            break;
    }
    CHECK(results[0] == 1 && results[1] == 1);
    CHECK(!ph_origin_push_busy(o, NULL, 0));

    /* Clipboard text the PC copies reaches the phone on the same link. */
    CHECK(ph_origin_put_clip(o, 'T', "hello", 5));
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "CLIP T 5"));
    CHECK(read_peer(phone, reply, 5, 0) == 5 && !memcmp(reply, "hello", 5));
    CHECK(ph_origin_clip_flags(o) & PH_ORIGIN_CLIP_READY);
    say(phone, "SYNC 1eecc050-8818-4079-9fb0-2e0143e55d96 0\n");
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "SYNC OK 1eecc050-8818-4079-9fb0-2e0143e55d96 0"));
    CHECK(!(ph_origin_clip_flags(o) & PH_ORIGIN_CLIP_READY));
    CHECK(!ph_origin_put_clip(o, 'T', "disabled", 8));
    say(phone, "NAME  Zoë’s iPhone\nSYNC 1eecc050-8818-4079-9fb0-2e0143e55d96 1\n");
    line(phone, reply, sizeof reply);
    CHECK(!strcmp(reply, "SYNC OK 1eecc050-8818-4079-9fb0-2e0143e55d96 1"));
    CHECK(ph_origin_clip_flags(o) & PH_ORIGIN_CLIP_READY);
    ph_origin_phone_name(o, device_name, sizeof device_name);
    CHECK(!strcmp(device_name, "Zoë’s iPhone"));

    /* The phone leaving fails anything still queued instead of showing it forever. */
    close_peer(phone);
    nap(400);
    CHECK(!(ph_origin_clip_flags(o) & PH_ORIGIN_CLIP_READY));
    ph_origin_phone_name(o, device_name, sizeof device_name);
    CHECK(!strcmp(device_name, "Zoë’s iPhone"));
    policy = ph_origin_control_policy(o);
    CHECK(policy.managed && !policy.connected);
    CHECK(!ph_control_connection_wanted(policy, true, true, false));
    test_resume_send(o, path_b, key);
    phone = reconnect_phone(key);
    test_screen_channel(o);
    test_resume_receive(dir, o);
    test_chunked_send(o, phone, path_a);
    CHECK(ph_origin_ask(o, 0x1234, 1.0, NULL, NULL, NULL, dir));
    char original[256];
    line(phone, original, sizeof original);
    CHECK(!strncmp(original, "PULL ", 5));
    say(phone, "FILE2 6c37299b-1234-43b0-acde-000000000001 5 shot.jpg\nimage");
    line(phone, original, sizeof original);
    CHECK(!strcmp(original, "RECEIVED 6c37299b-1234-43b0-acde-000000000001"));
    CHECK(ph_origin_take(o, original, sizeof original) == PH_ORIGIN_FILE &&
          ph_file_size(original) == 5);
    CHECK(ph_origin_ask(o, 0x1234, 1.0, NULL, NULL, NULL, dir));
    line(phone, original, sizeof original);
    CHECK(!strncmp(original, "PULL ", 5));
    ph_origin_cancel_pull(o);
    line(phone, original, sizeof original);
    CHECK(!strcmp(original, "CANCEL PULL"));
    say(phone, "MISS cancelled\n");
    close_peer(phone);
    o = test_pairing(o, token, key);
    ph_origin_stop(o);
    ph_remove(path_a);
    ph_remove(path_b);
    ph_rmdir(dir);
    puts("origin link: code, sync, queued Send to files with results, clipboard passed");
    return 0;
}
