// fake libcurl + the 4 openssl funcs the game wants. every transfer
// "finishes" right away with CURLE_COULDNT_CONNECT, so online stuff
// fails cleanly instead of hanging.
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "loader/exports.h"
#include "util/log.h"

#define CURLE_OK 0
#define CURLE_COULDNT_CONNECT 7
#define CURLM_OK 0
#define CURLMSG_DONE 1

typedef struct {
  int magic;
  int done_reported;
} fake_easy;

typedef struct {
  int msg;
  void *easy;
  union { void *whatever; int result; } data;
} curl_msg;

#define MAX_HANDLES 64
typedef struct {
  fake_easy *h[MAX_HANDLES];
  int n;
  curl_msg msg;
} fake_multi;

static int c_global_init_mem(long flags, void *m, void *f, void *r, void *s, void *c) {
  (void)flags; (void)m; (void)f; (void)r; (void)s; (void)c;
  L("curl: global init (offline stub)");
  return CURLE_OK;
}
static void c_global_cleanup(void) {}

static void *c_easy_init(void) {
  fake_easy *e = calloc(1, sizeof(*e));
  e->magic = 0xC0DE;
  return e;
}
static void c_easy_cleanup(void *e) { free(e); }

static int c_easy_setopt(void *e, int opt, ...) {
  (void)e;
  // log the url so we know what it wanted to talk to
  if (opt == 10002) {
    va_list ap;
    va_start(ap, opt);
    L("curl: url %s (not fetching)", va_arg(ap, const char *));
    va_end(ap);
  }
  return CURLE_OK;
}

static int c_easy_getinfo(void *e, int info, ...) {
  (void)e;
  va_list ap;
  va_start(ap, info);
  void *out = va_arg(ap, void *);
  va_end(ap);
  switch (info & 0xF00000) {
    case 0x100000: *(char **)out = NULL; break;   // string
    case 0x200000: *(long *)out = 0; break;       // long
    case 0x300000: *(double *)out = 0; break;     // double
    case 0x400000: *(void **)out = NULL; break;   // slist
  }
  return CURLE_OK;
}

static int c_easy_pause(void *e, int b) { (void)e; (void)b; return CURLE_OK; }

static void *c_slist_append(void *l, const char *s) { (void)s; return l ? l : (void *)1; }
static void c_slist_free_all(void *l) { (void)l; }

static void *c_multi_init(void) { return calloc(1, sizeof(fake_multi)); }
static int c_multi_cleanup(fake_multi *m) { free(m); return CURLM_OK; }

static int c_multi_add_handle(fake_multi *m, fake_easy *e) {
  if (m->n < MAX_HANDLES) m->h[m->n++] = e;
  e->done_reported = 0;
  return CURLM_OK;
}

static int c_multi_remove_handle(fake_multi *m, fake_easy *e) {
  for (int i = 0; i < m->n; i++) {
    if (m->h[i] == e) {
      m->h[i] = m->h[--m->n];
      break;
    }
  }
  return CURLM_OK;
}

static int c_multi_perform(fake_multi *m, int *running) {
  (void)m;
  *running = 0;
  return CURLM_OK;
}

static curl_msg *c_multi_info_read(fake_multi *m, int *left) {
  for (int i = 0; i < m->n; i++) {
    if (!m->h[i]->done_reported) {
      m->h[i]->done_reported = 1;
      m->msg.msg = CURLMSG_DONE;
      m->msg.easy = m->h[i];
      m->msg.data.result = CURLE_COULDNT_CONNECT;
      *left = 0;
      for (int j = i + 1; j < m->n; j++) *left += !m->h[j]->done_reported;
      return &m->msg;
    }
  }
  *left = 0;
  return NULL;
}

static int c_multi_fdset(void *m, void *r, void *w, void *x, int *maxfd) {
  (void)m; (void)r; (void)w; (void)x;
  *maxfd = -1;
  return CURLM_OK;
}

static int c_multi_timeout(void *m, long *t) { (void)m; *t = 100; return CURLM_OK; }

EXPORT_TABLE(curl) = {
  EXP_AS("curl_global_init_mem", c_global_init_mem),
  EXP_AS("curl_global_cleanup", c_global_cleanup),
  EXP_AS("curl_easy_init", c_easy_init),
  EXP_AS("curl_easy_cleanup", c_easy_cleanup),
  EXP_AS("curl_easy_setopt", c_easy_setopt),
  EXP_AS("curl_easy_getinfo", c_easy_getinfo),
  EXP_AS("curl_easy_pause", c_easy_pause),
  EXP_AS("curl_slist_append", c_slist_append),
  EXP_AS("curl_slist_free_all", c_slist_free_all),
  EXP_AS("curl_multi_init", c_multi_init),
  EXP_AS("curl_multi_cleanup", c_multi_cleanup),
  EXP_AS("curl_multi_add_handle", c_multi_add_handle),
  EXP_AS("curl_multi_remove_handle", c_multi_remove_handle),
  EXP_AS("curl_multi_perform", c_multi_perform),
  EXP_AS("curl_multi_info_read", c_multi_info_read),
  EXP_AS("curl_multi_fdset", c_multi_fdset),
  EXP_AS("curl_multi_timeout", c_multi_timeout),
};
EXPORT_TABLE_END(curl);

// openssl: just enough for the game to "install" its CA cert and move on
static int dummy_ssl_obj;
static void *s_bio_new_mem_buf(const void *b, int len) { (void)b; (void)len; return &dummy_ssl_obj; }
static void *s_pem_read_bio_x509(void *bio, void *x, void *cb, void *u) {
  (void)bio; (void)x; (void)cb; (void)u;
  return NULL;
}
static void *s_ssl_ctx_get_cert_store(void *ctx) { (void)ctx; return &dummy_ssl_obj; }
static int s_x509_store_add_cert(void *st, void *x) { (void)st; (void)x; return 1; }

EXPORT_TABLE(ssl) = {
  EXP_AS("BIO_new_mem_buf", s_bio_new_mem_buf),
  EXP_AS("PEM_read_bio_X509", s_pem_read_bio_x509),
  EXP_AS("SSL_CTX_get_cert_store", s_ssl_ctx_get_cert_store),
  EXP_AS("X509_STORE_add_cert", s_x509_store_add_cert),
};
EXPORT_TABLE_END(ssl);
