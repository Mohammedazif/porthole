#include "core/mdns_name.h"

#include "core/sock.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

size_t ph_mdns_reverse_query(const char *ipv4, uint8_t *out, size_t cap)
{
    unsigned a, b, c, d;
    char extra, name[64];
    if (!ipv4 || !out || sscanf(ipv4, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 ||
        b > 255 || c > 255 || d > 255)
        return 0;
    snprintf(name, sizeof name, "%u.%u.%u.%u.in-addr.arpa", d, c, b, a);
    size_t n = 0;
    static const uint8_t header[12] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}; /* one question */
    if (cap < sizeof header + strlen(name) + 2 + 4)
        return 0;
    memcpy(out, header, sizeof header);
    n = sizeof header;
    for (const char *label = name; *label;) {
        const char *dot = strchr(label, '.');
        size_t length = dot ? (size_t)(dot - label) : strlen(label);
        out[n++] = (uint8_t)length;
        memcpy(out + n, label, length);
        n += length;
        label += length + (dot ? 1 : 0);
    }
    out[n++] = 0;
    out[n++] = 0, out[n++] = 12;   /* PTR */
    out[n++] = 0x80, out[n++] = 1; /* IN, and answer to this port directly */
    return n;
}

/* Reads a (possibly compressed) name at *at into out; advances *at past it. */
static bool read_name(const uint8_t *reply, size_t len, size_t *at, char *out, size_t cap)
{
    size_t pos = *at, used = 0;
    bool jumped = false;
    for (int hops = 0; hops < 32 && pos < len; hops++) {
        uint8_t length = reply[pos];
        if (length == 0) {
            if (!jumped)
                *at = pos + 1;
            if (out && cap)
                out[used < cap ? used : cap - 1] = 0;
            return true;
        }
        if ((length & 0xC0) == 0xC0) {
            if (pos + 1 >= len)
                return false;
            if (!jumped)
                *at = pos + 2;
            jumped = true;
            pos = (size_t)(length & 0x3F) << 8 | reply[pos + 1];
            continue;
        }
        if (length & 0xC0 || pos + 1 + length > len)
            return false;
        if (out) {
            if (used + length + 2 > cap)
                return false;
            if (used)
                out[used++] = '.';
            memcpy(out + used, reply + pos + 1, length);
            used += length;
        }
        pos += 1 + (size_t)length;
    }
    return false;
}

bool ph_mdns_reverse_answer(const uint8_t *reply, size_t len, char *host, size_t cap)
{
    if (!reply || len < 12 || !host || !cap || !(reply[2] & 0x80)) /* a response */
        return false;
    unsigned questions = (unsigned)reply[4] << 8 | reply[5];
    unsigned answers = (unsigned)reply[6] << 8 | reply[7];
    size_t at = 12;
    for (unsigned i = 0; i < questions; i++) {
        if (!read_name(reply, len, &at, NULL, 0) || at + 4 > len)
            return false;
        at += 4;
    }
    for (unsigned i = 0; i < answers; i++) {
        if (!read_name(reply, len, &at, NULL, 0) || at + 10 > len)
            return false;
        unsigned type = (unsigned)reply[at] << 8 | reply[at + 1];
        unsigned rdlength = (unsigned)reply[at + 8] << 8 | reply[at + 9];
        at += 10;
        if (at + rdlength > len)
            return false;
        if (type == 12) {
            size_t target = at;
            return read_name(reply, len, &target, host, cap) && host[0];
        }
        at += rdlength;
    }
    return false;
}

bool ph_mdns_device_name(const char *host, char *out, size_t cap)
{
    if (!host || !out || !cap)
        return false;
    size_t length = strlen(host);
    while (length && host[length - 1] == '.')
        length--;
    if (length > 6 && !strncmp(host + length - 6, ".local", 6))
        length -= 6;
    if (memchr(host, '.', length))
        return false; /* not a plain .local host name */
    /* "-2", "-3": iOS's counter when the name was taken on this network. */
    size_t digits = length;
    while (digits && isdigit((unsigned char)host[digits - 1]))
        digits--;
    if (digits < length && digits > 1 && host[digits - 1] == '-')
        length = digits - 1;
    if (!length || length >= cap)
        return false;
    for (size_t i = 0; i < length; i++)
        out[i] = host[i] == '-' ? ' ' : host[i];
    out[length] = 0;
    return true;
}

bool ph_mdns_host_form_of(const char *exact, const char *from_host)
{
    /* iOS keeps letters and digits, turns spaces and dashes into "-", and
     * drops the rest; ph_mdns_device_name turns "-" back into spaces. */
    char form[64];
    size_t n = 0;
    if (!exact || !from_host || !*from_host)
        return false;
    for (const unsigned char *c = (const unsigned char *)exact; *c && n + 1 < sizeof form; c++) {
        if (isalnum(*c))
            form[n++] = (char)*c;
        else if ((*c == ' ' || *c == '-') && n && form[n - 1] != ' ')
            form[n++] = ' ';
    }
    while (n && form[n - 1] == ' ')
        n--;
    form[n] = 0;
    return n && !strcmp(form, from_host);
}

bool ph_mdns_lookup_device_name(const char *ipv4, char *out, size_t cap, int timeout_ms)
{
    uint8_t query[96], reply[1500];
    struct sockaddr_in group = {0}, from;
    size_t query_len = ph_mdns_reverse_query(ipv4, query, sizeof query);
    if (!query_len || !out || !cap)
        return false;
#ifdef _WIN32
    if (!ph_sock_init())
        return false;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return false;
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        return false;
#endif
    group.sin_family = AF_INET;
    group.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);
    struct in_addr phone;
    bool ok = false;
    if (inet_pton(AF_INET, ipv4, &phone) == 1 &&
        sendto(s, (const char *)query, (int)query_len, 0, (struct sockaddr *)&group, sizeof group) > 0) {
        for (int waited = 0; waited < timeout_ms && !ok; waited += 100) {
            struct pollfd ready = {.fd = s, .events = POLLIN};
            if (ph_poll(&ready, 1, 100) <= 0)
                continue;
            ph_socklen from_len = sizeof from;
            int got = (int)recvfrom(s, (char *)reply, (int)sizeof reply, 0, (struct sockaddr *)&from, &from_len);
            char host[256];
            /* Only the phone's own answer about its own address counts. */
            if (got > 0 && from.sin_addr.s_addr == phone.s_addr &&
                ph_mdns_reverse_answer(reply, (size_t)got, host, sizeof host))
                ok = ph_mdns_device_name(host, out, cap);
        }
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return ok;
}
