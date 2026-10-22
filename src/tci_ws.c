// Ported from maxibitx (https://github.com/MikeJohnshoy/maxibitx, GPL-3.0).
// tci_ws.c
//
// The WebSocket server under tci.c (RFC 6455, server side). One thread
// accepts; each client then has a reader thread (handshake, then frames in:
// unmasking, 7/16/64-bit lengths, fragmented messages, ping, close) and a
// writer thread that sends its queue of frames out. Everything a caller
// sends is queued, so no caller ever waits on a client's network.
//
// The close handshake is kept short: a close is answered (or sent), the
// queue is written out, and the connection is shut. Client frames that
// arrive unmasked are accepted, where the RFC says to close: refusing them
// would gain nothing here. SHA-1 is included for the handshake alone.
// docs/dsp_design_notes/tci_design_study.md §9.

#define _GNU_SOURCE
#pragma GCC optimize("O2") // zbitx's default build is -O0; this masks and copies every byte

#include "tci_ws.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define WS_MAX_MESSAGE (256 * 1024) // larger closes the client (1009)
#define WS_HANDSHAKE_MAX 8192
#define WS_HANDSHAKE_TIMEOUT_S 5
#define WS_SEND_TIMEOUT_S 5
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

struct ws_frame {
  struct ws_frame *next;
  size_t len;
  int binary;
  uint8_t data[];
};

struct ws_client {
  int in_use;  // slot claimed - srv_lock
  int fd;      // srv_lock
  int id;      // qlock (written under srv_lock too)
  int open;    // sends accepted - qlock
  int draining; // writer: send the queue, then stop - qlock
  int failed;  // writer: stop now - qlock
  struct ws_frame *head, *tail;
  size_t queued, queued_binary;
  pthread_mutex_t qlock;
  pthread_cond_t qcond;
  pthread_t writer;
};

static struct ws_client clients[WS_MAX_CLIENTS];
static pthread_mutex_t srv_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ws_callbacks callbacks;
static int listen_fd = -1;
static int client_limit;
static unsigned generation;
static atomic_int running;
static atomic_int active_readers;
static pthread_t accept_thread;

/* ---- SHA-1 and base64, for Sec-WebSocket-Accept ---------------------- */

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// Messages up to 183 bytes, which covers any key this server accepts.
static void sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
  uint8_t m[192];
  size_t padded = ((len + 8) / 64 + 1) * 64;
  memset(m, 0, sizeof(m));
  memcpy(m, msg, len);
  m[len] = 0x80;
  uint64_t bits = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++)
    m[padded - 1 - i] = (uint8_t)(bits >> (8 * i));

  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  for (size_t blk = 0; blk < padded; blk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)m[blk + 4 * i] << 24 | (uint32_t)m[blk + 4 * i + 1] << 16 |
             (uint32_t)m[blk + 4 * i + 2] << 8 | m[blk + 4 * i + 3];
    for (int i = 16; i < 80; i++)
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  for (int i = 0; i < 5; i++) {
    out[4 * i] = (uint8_t)(h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(h[i] >> 8);
    out[4 * i + 3] = (uint8_t)h[i];
  }
}

void ws_accept_key(const char *client_key, char out[29]) {
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint8_t cat[128], digest[20];
  size_t klen = strlen(client_key);
  if (klen > 64)
    klen = 64;
  memcpy(cat, client_key, klen);
  memcpy(cat + klen, WS_GUID, strlen(WS_GUID));
  sha1(cat, klen + strlen(WS_GUID), digest);
  int o = 0;
  for (int i = 0; i < 21; i += 3) { // 20 bytes: six full groups, then 2 bytes
    uint32_t v = (uint32_t)digest[i] << 16 | (i + 1 < 20 ? (uint32_t)digest[i + 1] << 8 : 0) |
                 (i + 2 < 20 ? digest[i + 2] : 0);
    out[o++] = b64[(v >> 18) & 63];
    out[o++] = b64[(v >> 12) & 63];
    out[o++] = i + 1 < 20 ? b64[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < 20 ? b64[v & 63] : '=';
  }
  out[28] = '\0';
}

/* ---- Send queue -------------------------------------------------------- */

static struct ws_client *client_for(int id) {
  return id < 0 ? NULL : &clients[ws_client_slot(id)];
}

static struct ws_frame *frame_new(int opcode, const void *data, size_t len) {
  size_t hlen = len < 126 ? 2 : len < 65536 ? 4 : 10;
  struct ws_frame *f = malloc(sizeof(*f) + hlen + len);
  if (!f)
    return NULL;
  f->next = NULL;
  f->len = hlen + len;
  f->binary = (opcode == 2);
  f->data[0] = (uint8_t)(0x80 | opcode); // FIN: never fragmented
  if (hlen == 2) {
    f->data[1] = (uint8_t)len;
  } else if (hlen == 4) {
    f->data[1] = 126;
    f->data[2] = (uint8_t)(len >> 8);
    f->data[3] = (uint8_t)len;
  } else {
    f->data[1] = 127;
    for (int i = 0; i < 8; i++)
      f->data[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
  }
  if (len)
    memcpy(f->data + hlen, data, len);
  return f;
}

// Called with qlock held.
static void queue_append(struct ws_client *c, struct ws_frame *f) {
  if (c->tail)
    c->tail->next = f;
  else
    c->head = f;
  c->tail = f;
  c->queued += f->len;
  if (f->binary)
    c->queued_binary += f->len;
  pthread_cond_signal(&c->qcond);
}

// Called with qlock held: stop the writer and wake the reader.
static void fail_locked(struct ws_client *c) {
  c->open = 0;
  c->failed = 1;
  pthread_cond_broadcast(&c->qcond);
  shutdown(c->fd, SHUT_RDWR);
}

static int enqueue(int id, int opcode, const void *data, size_t len) {
  struct ws_client *c = client_for(id);
  if (!c)
    return -1;
  struct ws_frame *f = frame_new(opcode, data, len);
  if (!f)
    return -1;
  pthread_mutex_lock(&c->qlock);
  int ok = (c->id == id && c->open);
  if (ok && f->binary && c->queued_binary + f->len > WS_QUEUE_BINARY_MAX) {
    ok = 0;
  } else if (ok && c->queued + f->len > WS_QUEUE_MAX) {
    fprintf(stderr, "tci: client %d isn't reading - disconnecting it\n", ws_client_slot(id));
    fail_locked(c);
    ok = 0;
  }
  if (ok)
    queue_append(c, f);
  pthread_mutex_unlock(&c->qlock);
  if (!ok)
    free(f);
  return ok ? 0 : -1;
}

int ws_send_text(int id, const char *text) { return enqueue(id, 1, text, strlen(text)); }

int ws_send_binary(int id, const void *data, size_t len) { return enqueue(id, 2, data, len); }

// Queues a close frame (status code, or none if code is 0) and has the
// writer shut the connection once the queue is out.
static void close_id(int id, int code) {
  struct ws_client *c = client_for(id);
  if (!c)
    return;
  uint8_t payload[2] = {(uint8_t)(code >> 8), (uint8_t)code};
  struct ws_frame *f = frame_new(8, payload, code ? 2 : 0);
  pthread_mutex_lock(&c->qlock);
  if (c->id == id && c->open) {
    if (f) // without it (no memory) the connection still closes, unannounced
      queue_append(c, f);
    f = NULL;
    c->open = 0;
    c->draining = 1;
    pthread_cond_broadcast(&c->qcond);
  }
  pthread_mutex_unlock(&c->qlock);
  free(f);
}

void ws_close_client(int id) { close_id(id, 1000); }

static int send_all(int fd, const uint8_t *p, size_t n) {
  while (n) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static void *writer_main(void *arg) {
  struct ws_client *c = arg;
  for (;;) {
    pthread_mutex_lock(&c->qlock);
    while (!c->head && !c->draining && !c->failed)
      pthread_cond_wait(&c->qcond, &c->qlock);
    if (c->failed || !c->head) { // failed, or drained
      pthread_mutex_unlock(&c->qlock);
      break;
    }
    struct ws_frame *f = c->head;
    c->head = f->next;
    if (!c->head)
      c->tail = NULL;
    c->queued -= f->len;
    if (f->binary)
      c->queued_binary -= f->len;
    int fd = c->fd;
    pthread_mutex_unlock(&c->qlock);

    int err = send_all(fd, f->data, f->len);
    free(f);
    if (err) {
      pthread_mutex_lock(&c->qlock);
      fail_locked(c);
      pthread_mutex_unlock(&c->qlock);
      break;
    }
  }
  shutdown(c->fd, SHUT_RDWR); // ends the reader's recv(), if it is still in one
  return NULL;
}

/* ---- Receive --------------------------------------------------------- */

struct rbuf {
  int fd;
  size_t pos, len;
  uint8_t buf[16384];
};

static int rb_read(struct rbuf *r, void *dst, size_t n) {
  uint8_t *d = dst;
  while (n) {
    if (r->pos == r->len) {
      ssize_t got = recv(r->fd, r->buf, sizeof(r->buf), 0);
      if (got < 0 && errno == EINTR)
        continue;
      if (got <= 0)
        return -1;
      r->pos = 0;
      r->len = (size_t)got;
    }
    size_t take = r->len - r->pos < n ? r->len - r->pos : n;
    memcpy(d, r->buf + r->pos, take);
    r->pos += take;
    d += take;
    n -= take;
  }
  return 0;
}

static void reply_http(int fd, const char *status) {
  char msg[160];
  snprintf(msg, sizeof(msg), "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
  send_all(fd, (const uint8_t *)msg, strlen(msg));
}

// Reads the HTTP upgrade request and answers it. Bytes after the request
// (a client may send its first frame at once) stay in r for the frame loop.
static int handshake(struct rbuf *r) {
  size_t end = 0;
  for (;;) {
    uint8_t *hit = memmem(r->buf, r->len, "\r\n\r\n", 4);
    if (hit) {
      end = (size_t)(hit - r->buf) + 4;
      break;
    }
    if (r->len >= WS_HANDSHAKE_MAX)
      return -1;
    ssize_t got = recv(r->fd, r->buf + r->len, WS_HANDSHAKE_MAX - r->len, 0);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return -1;
    r->len += (size_t)got;
  }
  r->pos = end;

  char head[WS_HANDSHAKE_MAX + 1];
  memcpy(head, r->buf, end);
  head[end] = '\0';
  char key[65] = "";
  int upgrade = 0;
  char *save = NULL;
  char *line = strtok_r(head, "\r\n", &save);
  if (!line || strncmp(line, "GET ", 4) != 0) {
    reply_http(r->fd, "400 Bad Request");
    return -1;
  }
  while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
    char *colon = strchr(line, ':');
    if (!colon)
      continue;
    *colon = '\0';
    char *value = colon + 1;
    while (*value == ' ' || *value == '\t')
      value++;
    size_t vlen = strlen(value);
    while (vlen && (value[vlen - 1] == ' ' || value[vlen - 1] == '\t'))
      value[--vlen] = '\0';
    if (!strcasecmp(line, "Sec-WebSocket-Key") && vlen < sizeof(key))
      memcpy(key, value, vlen + 1);
    else if (!strcasecmp(line, "Upgrade") && strcasestr(value, "websocket"))
      upgrade = 1;
  }
  if (!key[0] || !upgrade) {
    reply_http(r->fd, "400 Bad Request");
    return -1;
  }

  char accept[29], reply[256];
  ws_accept_key(key, accept);
  snprintf(reply, sizeof(reply),
           "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
           "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
           accept);
  return send_all(r->fd, (const uint8_t *)reply, strlen(reply));
}

// Frames in until the connection ends or either side closes it.
static void frame_loop(int id, struct rbuf *r) {
  uint8_t *msg = NULL;
  size_t msg_len = 0, msg_cap = 0;
  int msg_op = 0; // opcode of the message being assembled, 0 = none

  for (;;) {
    uint8_t h[2], mask[4] = {0, 0, 0, 0};
    if (rb_read(r, h, 2))
      break;
    int fin = h[0] & 0x80, op = h[0] & 0x0F;
    uint64_t plen = h[1] & 0x7F;
    if (h[0] & 0x70) { // no extensions were negotiated
      close_id(id, 1002);
      break;
    }
    if (plen >= 126) {
      uint8_t e[8];
      int n = plen == 126 ? 2 : 8;
      if (rb_read(r, e, (size_t)n))
        break;
      plen = 0;
      for (int i = 0; i < n; i++)
        plen = plen << 8 | e[i];
    }
    if (plen > WS_MAX_MESSAGE) { // bounded here, so msg_len + plen can't wrap
      close_id(id, 1009);
      break;
    }
    if ((h[1] & 0x80) && rb_read(r, mask, 4))
      break;

    if (op >= 8) { // control: close, ping, pong
      uint8_t ctl[125];
      if (!fin || plen > sizeof(ctl) || (op != 8 && op != 9 && op != 10)) {
        close_id(id, 1002);
        break;
      }
      if (rb_read(r, ctl, (size_t)plen))
        break;
      for (uint64_t i = 0; i < plen; i++)
        ctl[i] ^= mask[i & 3];
      if (op == 8) {
        close_id(id, plen >= 2 ? (ctl[0] << 8 | ctl[1]) : 0);
        break;
      }
      if (op == 9)
        enqueue(id, 10, ctl, (size_t)plen);
      continue;
    }

    if (op == 0) {
      if (!msg_op) {
        close_id(id, 1002);
        break;
      }
    } else if (op == 1 || op == 2) {
      if (msg_op) {
        close_id(id, 1002);
        break;
      }
      msg_op = op;
      msg_len = 0;
    } else {
      close_id(id, 1002);
      break;
    }
    if (msg_len + plen > WS_MAX_MESSAGE) {
      close_id(id, 1009);
      break;
    }
    if (msg_len + plen + 1 > msg_cap) {
      size_t cap = msg_len + (size_t)plen + 1;
      uint8_t *grown = realloc(msg, cap);
      if (!grown)
        break;
      msg = grown;
      msg_cap = cap;
    }
    if (rb_read(r, msg + msg_len, (size_t)plen))
      break;
    for (uint64_t i = 0; i < plen; i++)
      msg[msg_len + i] ^= mask[i & 3];
    msg_len += (size_t)plen;

    if (fin) {
      if (msg_op == 1) {
        msg[msg_len] = '\0';
        if (callbacks.on_text)
          callbacks.on_text(id, (const char *)msg, msg_len);
      } else if (callbacks.on_binary) {
        callbacks.on_binary(id, msg, msg_len);
      }
      msg_op = 0;
    }
  }
  free(msg);
}

struct reader_start {
  int slot;
  char peer[64];
};

static void *reader_main(void *arg) {
  struct reader_start *rs = arg;
  struct ws_client *c = &clients[rs->slot];
  char peer[64];
  memcpy(peer, rs->peer, sizeof(peer));
  free(rs);

  pthread_mutex_lock(&c->qlock);
  int id = c->id;
  pthread_mutex_unlock(&c->qlock);

  struct rbuf *r = calloc(1, sizeof(*r));
  int fd = c->fd;
  struct timeval tv = {WS_HANDSHAKE_TIMEOUT_S, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  int writer_started = 0;
  if (r && (r->fd = fd, handshake(r) == 0)) {
    struct timeval none = {0, 0}, send_to = {WS_SEND_TIMEOUT_S, 0};
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_to, sizeof(send_to));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    pthread_mutex_lock(&c->qlock);
    c->open = 1;
    pthread_mutex_unlock(&c->qlock);
    if (pthread_create(&c->writer, NULL, writer_main, c) == 0) {
      writer_started = 1;
      if (callbacks.on_open)
        callbacks.on_open(id, peer);
      frame_loop(id, r);
    }
  }

  free(r);
  pthread_mutex_lock(&c->qlock);
  c->open = 0;
  if (!c->draining)
    fail_locked(c);
  pthread_cond_broadcast(&c->qcond);
  pthread_mutex_unlock(&c->qlock);
  if (writer_started) {
    pthread_join(c->writer, NULL);
    if (callbacks.on_close)
      callbacks.on_close(id);
  }

  pthread_mutex_lock(&c->qlock);
  while (c->head) {
    struct ws_frame *f = c->head;
    c->head = f->next;
    free(f);
  }
  c->tail = NULL;
  c->queued = c->queued_binary = 0;
  pthread_mutex_unlock(&c->qlock);

  pthread_mutex_lock(&srv_lock);
  close(c->fd);
  c->fd = -1;
  c->in_use = 0;
  pthread_mutex_unlock(&srv_lock);
  atomic_fetch_sub(&active_readers, 1);
  return NULL;
}

/* ---- Accept and lifetime --------------------------------------------- */

static void *accept_main(void *arg) {
  (void)arg;
  while (atomic_load(&running)) {
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &alen);
    if (fd < 0) {
      if (!atomic_load(&running))
        break;
      if (errno != EINTR && errno != ECONNABORTED)
        usleep(100000);
      continue;
    }
    char peer[64];
    snprintf(peer, sizeof(peer), "%s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

    pthread_mutex_lock(&srv_lock);
    int slot = -1;
    for (int i = 0; i < client_limit; i++) {
      if (!clients[i].in_use) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      pthread_mutex_unlock(&srv_lock);
      reply_http(fd, "503 Service Unavailable");
      close(fd);
      fprintf(stderr, "tci: connection from %s refused - %d clients already connected\n", peer,
              client_limit);
      continue;
    }
    struct ws_client *c = &clients[slot];
    generation = (generation + 1) & 0x3FFFFFF;
    c->in_use = 1;
    c->fd = fd;
    pthread_mutex_lock(&c->qlock);
    c->id = (int)(generation * WS_MAX_CLIENTS) | slot;
    c->open = c->draining = c->failed = 0;
    c->head = c->tail = NULL;
    c->queued = c->queued_binary = 0;
    pthread_mutex_unlock(&c->qlock);
    atomic_fetch_add(&active_readers, 1);
    pthread_mutex_unlock(&srv_lock);

    struct reader_start *rs = malloc(sizeof(*rs));
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (rs) {
      rs->slot = slot;
      memcpy(rs->peer, peer, sizeof(peer));
    }
    if (!rs || pthread_create(&t, &attr, reader_main, rs) != 0) {
      free(rs);
      pthread_mutex_lock(&srv_lock);
      close(fd);
      c->fd = -1;
      c->in_use = 0;
      pthread_mutex_unlock(&srv_lock);
      atomic_fetch_sub(&active_readers, 1);
    }
    pthread_attr_destroy(&attr);
  }
  return NULL;
}

int ws_server_start(const char *bind_addr, int port, int max_clients,
                    const struct ws_callbacks *cb) {
  static int initialized = 0;
  if (!initialized) {
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
      pthread_mutex_init(&clients[i].qlock, NULL);
      pthread_cond_init(&clients[i].qcond, NULL);
      clients[i].fd = -1;
    }
    initialized = 1;
  }
  callbacks = *cb;
  client_limit = max_clients < 1 ? 1 : max_clients > WS_MAX_CLIENTS ? WS_MAX_CLIENTS : max_clients;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind_addr && bind_addr[0] && inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
    fprintf(stderr, "tci: tci_bind '%s' isn't a dotted IPv4 address\n", bind_addr);
    return -1;
  }

  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0)
    return -1;
  int one = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listen_fd, 8) < 0) {
    fprintf(stderr, "tci: can't listen on TCP %d: %s\n", port, strerror(errno));
    close(listen_fd);
    listen_fd = -1;
    return -1;
  }
  atomic_store(&running, 1);
  if (pthread_create(&accept_thread, NULL, accept_main, NULL) != 0) {
    atomic_store(&running, 0);
    close(listen_fd);
    listen_fd = -1;
    return -1;
  }
  return 0;
}

void ws_server_stop(void) {
  if (!atomic_exchange(&running, 0))
    return;
  shutdown(listen_fd, SHUT_RDWR); // wakes accept()
  pthread_join(accept_thread, NULL);
  close(listen_fd);
  listen_fd = -1;

  pthread_mutex_lock(&srv_lock);
  for (int i = 0; i < WS_MAX_CLIENTS; i++) {
    if (clients[i].in_use) {
      pthread_mutex_lock(&clients[i].qlock);
      fail_locked(&clients[i]);
      pthread_mutex_unlock(&clients[i].qlock);
    }
  }
  pthread_mutex_unlock(&srv_lock);
  for (int waited = 0; atomic_load(&active_readers) > 0 && waited < 100; waited++)
    usleep(10000);
}
