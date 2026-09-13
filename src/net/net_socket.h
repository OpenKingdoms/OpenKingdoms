#ifndef TAK_NET_SOCKET_H
#define TAK_NET_SOCKET_H

/*
 * The few socket calls the relay host needs, spelled the same on
 * Windows and on everything else.
 *
 * Deliberately small. Everything interesting about a connection lives
 * in ws_conn.c and in the relay core, both of which are tested with no
 * network at all, so this layer only has to open a listener, accept,
 * read, write and select. Anything cleverer belongs above it.
 */

#include <stddef.h>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  /* select on Windows walks an array rather than a bitmask, and the
   * array is FD_SETSIZE long. The default is 64 and the relay holds up
   * to 64 clients plus its listener, so it has to be raised here,
   * before winsock2.h reads it. */
  #ifndef FD_SETSIZE
  #define FD_SETSIZE 256
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET TakSocket;
  #define TAK_INVALID_SOCKET INVALID_SOCKET
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>
  typedef int TakSocket;
  #define TAK_INVALID_SOCKET (-1)
#endif

/* Start and stop whatever the platform needs. 0 on success. */
int  TakNet_Start(void);
void TakNet_Stop(void);

/* A listening socket on `port`, non blocking, with the address
 * reusable so a restart does not wait out TIME_WAIT. */
TakSocket TakNet_Listen(unsigned short port);

/* Accept one, non blocking and with Nagle off, or TAK_INVALID_SOCKET
 * when there is nothing waiting. */
TakSocket TakNet_Accept(TakSocket listener);

void TakNet_Close(TakSocket s);

/* Bytes moved, 0 when the peer closed, -1 on a real error, and
 * -2 when the call would have blocked, which is not an error. */
int TakNet_Recv(TakSocket s, void *buf, size_t len);
int TakNet_Send(TakSocket s, const void *buf, size_t len);

/* Wait until one of the sockets is ready or `timeout_ms` passes.
 * `want_write` marks the ones with something queued. Returns the
 * number ready, or -1. `readable` and `writable` are filled in. */
int TakNet_Wait(const TakSocket *socks, const unsigned char *want_write,
                unsigned char *readable, unsigned char *writable,
                int count, int timeout_ms);

/* Milliseconds from an arbitrary start, for the relay's clock. */
unsigned long long TakNet_NowMs(void);

#endif /* TAK_NET_SOCKET_H */
