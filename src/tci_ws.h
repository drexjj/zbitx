// Ported from maxibitx (https://github.com/MikeJohnshoy/maxibitx, GPL-3.0).
// tci_ws.h
//
// A WebSocket server (RFC 6455), server side only: the transport under
// tci.c, knowing nothing of TCI. Accepts up to a fixed number of clients,
// each read by a thread of its own and written by another from a queue, so
// a slow client never blocks the thread that sends to it.
// docs/dsp_design_notes/tci_design_study.md §9.

#ifndef TCI_WS_H
#define TCI_WS_H

#include <stddef.h>
#include <stdint.h>

#define WS_MAX_CLIENTS 16

// A client is named by an id that stays unique across reconnects: a later
// client in the same slot gets a different id, so a send to a closed
// client's id is refused rather than delivered to its successor.
// ws_client_slot() gives the slot, 0..max_clients-1, for per-client arrays.
static inline int ws_client_slot(int id) { return id & (WS_MAX_CLIENTS - 1); }

// Called on the client's reader thread. on_open comes before any message
// and on_close after the last; no send to the id succeeds after on_close.
// A text message is NUL-terminated (len excludes the NUL).
struct ws_callbacks {
  void (*on_open)(int id, const char *peer);
  void (*on_text)(int id, const char *msg, size_t len);
  void (*on_binary)(int id, const uint8_t *data, size_t len);
  void (*on_close)(int id);
};

// Listens on bind_addr (dotted IPv4; NULL or "" for every interface) and
// port. Returns 0, or -1 if the socket can't be bound.
int ws_server_start(const char *bind_addr, int port, int max_clients,
                    const struct ws_callbacks *cb);

// Closes the listening socket and every client, and waits (up to about a
// second) for their threads to finish.
void ws_server_stop(void);

// Queue a message to a client; any thread. Return 0, or -1 if the client
// is gone or the frame was dropped. Binary frames are dropped while more
// than WS_QUEUE_BINARY_MAX bytes are waiting (a stream the client isn't
// keeping up with); a client with more than WS_QUEUE_MAX bytes waiting is
// disconnected.
#define WS_QUEUE_BINARY_MAX (2 * 1024 * 1024)
#define WS_QUEUE_MAX (8 * 1024 * 1024)
int ws_send_text(int id, const char *text);
int ws_send_binary(int id, const void *data, size_t len);

// Closes a client's connection (normal closure); its on_close follows.
void ws_close_client(int id);

// The Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key (28
// characters and a NUL). For the bench harness.
void ws_accept_key(const char *client_key, char out[29]);

#endif /* TCI_WS_H */
