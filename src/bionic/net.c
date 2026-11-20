// no networking, on purpose. every socket call just fails like the
// network is down, so the game falls back to offline.
#include <errno.h>
#include <stddef.h>
#include <zlib.h>
#include "bionic/bionic.h"
#include "loader/exports.h"
#include "util/log.h"

static int net_down(const char *what) {
  L_ONCE("net: %s -> offline", what);
  errno = ENETDOWN;
  return -1;
}

static int b_socket(int d, int t, int p) { (void)d; (void)t; (void)p; return net_down("socket"); }
static int b_connect(int s, const void *a, int l) { (void)s; (void)a; (void)l; return net_down("connect"); }
static int b_bind(int s, const void *a, int l) { (void)s; (void)a; (void)l; return net_down("bind"); }
static int b_listen(int s, int n) { (void)s; (void)n; return net_down("listen"); }
static int b_accept(int s, void *a, void *l) { (void)s; (void)a; (void)l; return net_down("accept"); }
static int b_send(int s, const void *b, size_t n, int f) { (void)s; (void)b; (void)n; (void)f; return net_down("send"); }
static int b_sendto(int s, const void *b, size_t n, int f, const void *a, int l) {
  (void)s; (void)b; (void)n; (void)f; (void)a; (void)l;
  return net_down("sendto");
}
static int b_recv(int s, void *b, size_t n, int f) { (void)s; (void)b; (void)n; (void)f; return net_down("recv"); }
static int b_recvfrom(int s, void *b, size_t n, int f, void *a, void *l) {
  (void)s; (void)b; (void)n; (void)f; (void)a; (void)l;
  return net_down("recvfrom");
}
static int b_setsockopt(int s, int l, int o, const void *v, int n) {
  (void)s; (void)l; (void)o; (void)v; (void)n;
  return net_down("setsockopt");
}
static int b_getsockname(int s, void *a, void *l) { (void)s; (void)a; (void)l; return net_down("getsockname"); }
static int b_shutdown(int s, int h) { (void)s; (void)h; return net_down("shutdown"); }
static void *b_gethostbyname(const char *n) { L_ONCE("net: gethostbyname(%s) -> NULL", n); return NULL; }

EXPORT_TABLE(net) = {
  EXP_AS("socket", b_socket),
  EXP_AS("connect", b_connect),
  EXP_AS("bind", b_bind),
  EXP_AS("listen", b_listen),
  EXP_AS("accept", b_accept),
  EXP_AS("send", b_send),
  EXP_AS("sendto", b_sendto),
  EXP_AS("recv", b_recv),
  EXP_AS("recvfrom", b_recvfrom),
  EXP_AS("setsockopt", b_setsockopt),
  EXP_AS("getsockname", b_getsockname),
  EXP_AS("shutdown", b_shutdown),
  EXP_AS("gethostbyname", b_gethostbyname),
};
EXPORT_TABLE_END(net);

// zlib is ABI compatible (z_stream is the same 56 bytes), pass straight through
EXPORT_TABLE(zlib) = {
  EXP(deflate), EXP(deflateEnd), EXP(deflateInit2_),
  EXP(inflate), EXP(inflateEnd), EXP(inflateInit2_),
};
EXPORT_TABLE_END(zlib);
