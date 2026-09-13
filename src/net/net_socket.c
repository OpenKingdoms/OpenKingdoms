/*
 * net_socket.c -- the few socket calls the relay host needs.
 *
 * See net_socket.h for why this is as small as it is.
 */

#include "net_socket.h"

#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <sys/select.h>
  #include <sys/time.h>
  #include <time.h>
#endif

int TakNet_Start(void) {
#if defined(_WIN32)
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void TakNet_Stop(void) {
#if defined(_WIN32)
    WSACleanup();
#endif
}

static int set_non_blocking(TakSocket s) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0 ? 0 : -1;
#else
    int f = fcntl(s, F_GETFL, 0);
    if (f < 0) return -1;
    return fcntl(s, F_SETFL, f | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

TakSocket TakNet_Listen(unsigned short port) {
    TakSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == TAK_INVALID_SOCKET) return TAK_INVALID_SOCKET;

    /* A restart should not have to wait out TIME_WAIT. On Windows
     * SO_REUSEADDR also lets a second process steal the port, so it is
     * SO_EXCLUSIVEADDRUSE there, which gives the restart without the
     * theft. */
#if defined(_WIN32)
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof on);
#else
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#endif

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        TakNet_Close(s);
        return TAK_INVALID_SOCKET;
    }
    if (listen(s, 64) != 0) {
        TakNet_Close(s);
        return TAK_INVALID_SOCKET;
    }
    if (set_non_blocking(s) != 0) {
        TakNet_Close(s);
        return TAK_INVALID_SOCKET;
    }
    return s;
}

TakSocket TakNet_Accept(TakSocket listener) {
    TakSocket s = accept(listener, NULL, NULL);
    if (s == TAK_INVALID_SOCKET) return TAK_INVALID_SOCKET;
    if (set_non_blocking(s) != 0) {
        TakNet_Close(s);
        return TAK_INVALID_SOCKET;
    }
    /* Turns are small and latency is the whole point, so no Nagle. */
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
    return s;
}

void TakNet_Close(TakSocket s) {
    if (s == TAK_INVALID_SOCKET) return;
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

#if defined(_WIN32)
static int would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
static int would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK
                                   || errno == EINTR; }
#endif

int TakNet_Recv(TakSocket s, void *buf, size_t len) {
    int n = (int)recv(s, (char *)buf, (int)len, 0);
    if (n > 0) return n;
    if (n == 0) return 0;
    return would_block() ? -2 : -1;
}

int TakNet_Send(TakSocket s, const void *buf, size_t len) {
    int n = (int)send(s, (const char *)buf, (int)len, 0);
    if (n >= 0) return n;
    return would_block() ? -2 : -1;
}

int TakNet_Wait(const TakSocket *socks, const unsigned char *want_write,
                unsigned char *readable, unsigned char *writable,
                int count, int timeout_ms) {
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    int nfds = 0;
    for (int i = 0; i < count; i++) {
        if (socks[i] == TAK_INVALID_SOCKET) continue;
        FD_SET(socks[i], &rd);
        if (want_write && want_write[i]) FD_SET(socks[i], &wr);
#if !defined(_WIN32)
        if ((int)socks[i] + 1 > nfds) nfds = (int)socks[i] + 1;
#endif
    }
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int n = select(nfds, &rd, &wr, NULL, &tv);
    if (n < 0) return -1;
    for (int i = 0; i < count; i++) {
        readable[i] = 0;
        writable[i] = 0;
        if (socks[i] == TAK_INVALID_SOCKET) continue;
        if (FD_ISSET(socks[i], &rd)) readable[i] = 1;
        if (FD_ISSET(socks[i], &wr)) writable[i] = 1;
    }
    return n;
}

unsigned long long TakNet_NowMs(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (unsigned long long)((now.QuadPart * 1000) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull
         + (unsigned long long)(ts.tv_nsec / 1000000);
#endif
}
