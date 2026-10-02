/*
 * http_get.c -- one plain HTTP GET over a blocking socket.
 *
 * See tak_http_get.h. The relay answers with Content-Length and closes,
 * so this reads to the close and needs no chunked decoding.
 */

#include "tak_http_get.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __EMSCRIPTEN__

#include "net_socket.h"

static void say(char *why, size_t cap, const char *text) {
    if (why && cap) snprintf(why, cap, "%s", text);
}

static int split_address(const char *a, char *host, size_t hcap, unsigned short *port) {
    if (strncmp(a, "ws://", 5) == 0) a += 5;
    else if (strncmp(a, "http://", 7) == 0) a += 7;
    else if (strstr(a, "://")) return -1;
    size_t n = 0;
    while (*a && *a != ':' && *a != '/') {
        if (n + 1 >= hcap) return -1;
        host[n++] = *a++;
    }
    host[n] = '\0';
    *port = 80;
    if (*a == ':') {
        unsigned v = 0;
        for (a++; *a >= '0' && *a <= '9'; a++) v = v * 10 + (unsigned)(*a - '0');
        if (v == 0 || v > 65535) return -1;
        *port = (unsigned short)v;
    }
    return n ? 0 : -1;
}

int TAK_HttpGet(const char *address, const char *path, size_t cap,
                uint8_t **body, size_t *len, volatile size_t *progress,
                char *why, size_t why_cap) {
    *body = NULL;
    *len = 0;
    char host[128];
    unsigned short port = 80;
    if (!address || split_address(address, host, sizeof host, &port) != 0) {
        say(why, why_cap, "That is not a server address this build can reach.");
        return -1;
    }
    if (TakNet_Start() != 0) { say(why, why_cap, "The network would not start."); return -1; }
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        say(why, why_cap, "The server's name did not resolve.");
        return -1;
    }
    TakSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int ok = s != TAK_INVALID_SOCKET && connect(s, res->ai_addr, (int)res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) {
        if (s != TAK_INVALID_SOCKET) TakNet_Close(s);
        say(why, why_cap, "Nothing answered at the server's address.");
        return -1;
    }
#ifdef _WIN32
    DWORD tv = 30000;
#else
    struct timeval tv = { 30, 0 };
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    char req[512];
    int rn = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: OpenKingdoms\r\n"
                      "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (rn <= 0 || (size_t)rn >= sizeof req || send(s, req, rn, 0) != rn) {
        TakNet_Close(s);
        say(why, why_cap, "The request did not go out.");
        return -1;
    }
    size_t room = cap + 16384, got = 0, head = 0;
    uint8_t *buf = (uint8_t *)malloc(room + 1);
    int failed = buf == NULL;
    while (!failed) {
        if (got == room) { failed = 1; break; }
        int n = (int)recv(s, (char *)buf + got, (int)(room - got), 0);
        if (n == 0) break;
        if (n < 0) { failed = 2; break; }
        got += (size_t)n;
        buf[got] = '\0';
        if (!head) {
            char *end = strstr((char *)buf, "\r\n\r\n");
            if (end) head = (size_t)(end + 4 - (char *)buf);
        }
        if (head && progress) *progress = got - head;
    }
    TakNet_Close(s);
    if (failed) {
        free(buf);
        say(why, why_cap, failed == 1 ? "The answer is larger than it should be."
                                      : "The connection went while reading.");
        return -1;
    }
    int code = 0;
    if (!head || sscanf((char *)buf, "HTTP/1.%*d %d", &code) != 1) {
        free(buf);
        say(why, why_cap, "The server's answer was not HTTP.");
        return -1;
    }
    /* A body shorter than its length is a download cut off on the way. */
    const char *cl = strstr((char *)buf, "Content-Length:");
    if (!cl) cl = strstr((char *)buf, "content-length:");
    size_t body_len = got - head;
    if (cl && (size_t)(cl - (char *)buf) < head) {
        unsigned long long want = strtoull(cl + 15, NULL, 10);
        if (want != body_len) {
            free(buf);
            say(why, why_cap, "The download stopped short.");
            return -1;
        }
    }
    if (code != 200) {
        char text[256];
        const char *err = strstr((char *)buf + head, "\"error\":\"");
        if (err) {
            err += 9;
            size_t k = strcspn(err, "\"");
            if (k > 200) k = 200;
            snprintf(text, sizeof text, "The server said: %.*s.", (int)k, err);
        } else {
            snprintf(text, sizeof text, "The server answered %d.", code);
        }
        free(buf);
        say(why, why_cap, text);
        return -1;
    }
    memmove(buf, buf + head, body_len);
    buf[body_len] = '\0';
    *body = buf;
    *len = body_len;
    return 0;
}

#else

int TAK_HttpGet(const char *address, const char *path, size_t cap,
                uint8_t **body, size_t *len, volatile size_t *progress,
                char *why, size_t why_cap) {
    (void)address; (void)path; (void)cap; (void)progress;
    *body = NULL;
    *len = 0;
    if (why && why_cap) snprintf(why, why_cap, "The page fetches mods in the browser.");
    return -1;
}

#endif
