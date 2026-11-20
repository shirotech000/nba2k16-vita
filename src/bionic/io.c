// posix file io the bionic way. plus fake pipes since the native app
// glue talks to its own thread through one.
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include "bionic/bionic.h"
#include "loader/exports.h"
#include "util/abi.h"
#include "util/log.h"
#include "util/paths.h"

// --- pipes ---
#define PIPE_BASE 0x4000
#define MAX_PIPES 16
#define PIPE_BUF_SZ 4096

typedef struct {
  int used;
  uint8_t buf[PIPE_BUF_SZ];
  int head, tail;  // read at tail, write at head
  int closed_w;
  SceKernelLwMutexWork lock;
  SceUID readable;  // sema, count = bytes in buffer (roughly)
} vpipe;

static vpipe pipes[MAX_PIPES];

// fd layout: PIPE_BASE + idx*2 = read end, +1 = write end
static vpipe *pipe_of(int fd) {
  int i = (fd - PIPE_BASE) / 2;
  if (fd < PIPE_BASE || i >= MAX_PIPES || !pipes[i].used) return NULL;
  return &pipes[i];
}

int vpipe_is(int fd) { return pipe_of(fd) != NULL; }

int vpipe_available(int fd) {
  vpipe *p = pipe_of(fd);
  if (!p) return 0;
  int n = p->head - p->tail;
  return n < 0 ? n + PIPE_BUF_SZ : n;
}

static int b_pipe(int fds[2]) {
  for (int i = 0; i < MAX_PIPES; i++) {
    if (pipes[i].used) continue;
    vpipe *p = &pipes[i];
    memset(p, 0, sizeof(*p));
    p->used = 1;
    sceKernelCreateLwMutex(&p->lock, "vpipe", 0, 0, NULL);
    p->readable = sceKernelCreateSema("vpipe", 0, 0, 0x7FFFFFFF, NULL);
    fds[0] = PIPE_BASE + i * 2;
    fds[1] = PIPE_BASE + i * 2 + 1;
    L("pipe() -> %d/%d", fds[0], fds[1]);
    return 0;
  }
  errno = EMFILE;
  return -1;
}

static int pipe_read(vpipe *p, void *dst, size_t n) {
  uint8_t *d = dst;
  // block until there's at least something
  while (!vpipe_available(PIPE_BASE + (p - pipes) * 2)) {
    if (p->closed_w) return 0;
    sceKernelWaitSema(p->readable, 1, NULL);
  }
  sceKernelLockLwMutex(&p->lock, 1, NULL);
  size_t got = 0;
  while (got < n && p->tail != p->head) {
    d[got++] = p->buf[p->tail];
    p->tail = (p->tail + 1) % PIPE_BUF_SZ;
  }
  sceKernelUnlockLwMutex(&p->lock, 1);
  return got;
}

static int pipe_write(vpipe *p, const void *src, size_t n) {
  const uint8_t *s = src;
  sceKernelLockLwMutex(&p->lock, 1, NULL);
  size_t put = 0;
  while (put < n && (p->head + 1) % PIPE_BUF_SZ != p->tail) {
    p->buf[p->head] = s[put++];
    p->head = (p->head + 1) % PIPE_BUF_SZ;
  }
  sceKernelUnlockLwMutex(&p->lock, 1);
  if (put < n) L("pipe full, dropped %u bytes", (unsigned)(n - put));
  sceKernelSignalSema(p->readable, 1);
  looper_poke();
  return put;
}

// --- open & friends ---
static int fix_flags(int f) {
  int o = 0;
  switch (f & 3) {
    case 0: o = O_RDONLY; break;
    case 1: o = O_WRONLY; break;
    default: o = O_RDWR; break;
  }
  if (f & B_O_CREAT) o |= O_CREAT;
  if (f & B_O_EXCL) o |= O_EXCL;
  if (f & B_O_TRUNC) o |= O_TRUNC;
  if (f & B_O_APPEND) o |= O_APPEND;
  return o;
}

static int b_open(const char *path, int flags, ...) {
  int mode = 0;
  if (flags & B_O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  char fixed[512];
  path_fix(path, fixed, sizeof(fixed));
  int fd = open(fixed, fix_flags(flags), mode);
  L("open(%s, 0x%X) -> %d", fixed, flags, fd);
  return fd;
}

static int b_read(int fd, void *buf, size_t n) {
  vpipe *p = pipe_of(fd);
  if (p) return pipe_read(p, buf, n);
  return read(fd, buf, n);
}

static int b_write(int fd, const void *buf, size_t n) {
  vpipe *p = pipe_of(fd);
  if (p) return pipe_write(p, buf, n);
  if (fd == 1 || fd == 2) {
    L("[fd%d] %.*s", fd, (int)n, (const char *)buf);
    return n;
  }
  return write(fd, buf, n);
}

typedef struct { void *base; size_t len; } b_iovec;

static int b_writev(int fd, const b_iovec *iov, int cnt) {
  int total = 0;
  for (int i = 0; i < cnt; i++) {
    int r = b_write(fd, iov[i].base, iov[i].len);
    if (r < 0) return r;
    total += r;
  }
  return total;
}

static int b_close(int fd) {
  vpipe *p = pipe_of(fd);
  if (p) {
    if (fd & 1) {
      p->closed_w = 1;
      sceKernelSignalSema(p->readable, 1);
    }
    return 0;
  }
  return close(fd);
}

static long b_lseek(int fd, long off, int whence) { return lseek(fd, off, whence); }
static long long b_lseek64(int fd, long long off, int whence) {
  return lseek(fd, (off_t)off, whence);
}

static void to_bstat(const struct stat *s, b_stat *b) {
  memset(b, 0, sizeof(*b));
  b->st_mode = s->st_mode;  // S_IF* bits are the same octal values
  b->st_nlink = 1;
  b->st_size = s->st_size;
  b->st_blksize = 4096;
  b->st_blocks = (s->st_size + 511) / 512;
  b->st_atime_ = s->st_atime;
  b->st_mtime_ = s->st_mtime;
  b->st_ctime_ = s->st_ctime;
}

static int b_stat_fn(const char *path, b_stat *b) {
  char fixed[512];
  struct stat s;
  path_fix(path, fixed, sizeof(fixed));
  int r = stat(fixed, &s);
  if (r == 0) to_bstat(&s, b);
  L("stat(%s) -> %d", fixed, r);
  return r;
}

static int b_fstat(int fd, b_stat *b) {
  if (vpipe_is(fd)) {
    memset(b, 0, sizeof(*b));
    b->st_mode = 0010000 | 0600;  // fifo
    return 0;
  }
  struct stat s;
  int r = fstat(fd, &s);
  if (r == 0) to_bstat(&s, b);
  return r;
}

typedef struct {
  uint32_t f_type, f_bsize;
  uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int32_t f_fsid[2];
  uint32_t f_namelen, f_frsize, f_flags, f_spare[4];
} b_statfs;

static int b_statfs_fn(const char *path, b_statfs *s) {
  // just say there's ~4GB free, the game only checks it's enough
  memset(s, 0, sizeof(*s));
  s->f_bsize = s->f_frsize = 4096;
  s->f_blocks = 2 * 1024 * 1024;
  s->f_bfree = s->f_bavail = 1024 * 1024;
  s->f_namelen = 255;
  L("statfs(%s)", path);
  return 0;
}

static int b_mkdir(const char *path, int mode) {
  char fixed[512];
  path_fix(path, fixed, sizeof(fixed));
  int r = sceIoMkdir(fixed, 0777);
  if (r < 0) {
    errno = (r & 0xFF) == 0x11 ? EEXIST : ENOENT;  // SCE_ERROR_ERRNO_EEXIST is 0x80010011
    return -1;
  }
  return 0;
}

static int b_rmdir(const char *path) {
  char fixed[512];
  return sceIoRmdir(path_fix(path, fixed, sizeof(fixed))) < 0 ? -1 : 0;
}

static int b_unlink(const char *path) {
  char fixed[512];
  return sceIoRemove(path_fix(path, fixed, sizeof(fixed))) < 0 ? -1 : 0;
}

static int b_rename(const char *a, const char *b) {
  char fa[512], fb[512];
  return sceIoRename(path_fix(a, fa, sizeof(fa)), path_fix(b, fb, sizeof(fb))) < 0 ? -1 : 0;
}

// --- dirs, straight on top of sceIoDopen ---
typedef struct {
  SceUID dfd;
  b_dirent ent;
} b_dir;

static b_dir *b_opendir(const char *path) {
  char fixed[512];
  path_fix(path, fixed, sizeof(fixed));
  SceUID d = sceIoDopen(fixed);
  L("opendir(%s) -> 0x%X", fixed, d);
  if (d < 0) {
    errno = ENOENT;
    return NULL;
  }
  b_dir *dir = calloc(1, sizeof(*dir));
  dir->dfd = d;
  return dir;
}

static b_dirent *b_readdir(b_dir *dir) {
  SceIoDirent e;
  memset(&e, 0, sizeof(e));
  if (sceIoDread(dir->dfd, &e) <= 0) return NULL;
  memset(&dir->ent, 0, sizeof(dir->ent));
  dir->ent.d_reclen = sizeof(b_dirent);
  dir->ent.d_type = SCE_S_ISDIR(e.d_stat.st_mode) ? B_DT_DIR : B_DT_REG;
  strncpy(dir->ent.d_name, e.d_name, sizeof(dir->ent.d_name) - 1);
  return &dir->ent;
}

static int b_closedir(b_dir *dir) {
  if (!dir) return -1;
  sceIoDclose(dir->dfd);
  free(dir);
  return 0;
}

static int b_alphasort(const b_dirent **a, const b_dirent **b) {
  return strcmp((*a)->d_name, (*b)->d_name);
}

static int b_scandir(const char *path, b_dirent ***list,
                     int (*filter)(const b_dirent *),
                     int (*cmp)(const b_dirent **, const b_dirent **)) {
  b_dir *d = b_opendir(path);
  if (!d) return -1;
  int n = 0, cap = 16;
  // malloc here goes through newlib directly, the game frees with our
  // free() which is the same allocator, so that's fine
  b_dirent **out = malloc(cap * sizeof(*out));
  b_dirent *e;
  while ((e = b_readdir(d))) {
    if (filter && !filter(e)) continue;
    if (n == cap) out = realloc(out, (cap *= 2) * sizeof(*out));
    out[n] = malloc(sizeof(b_dirent));
    memcpy(out[n], e, sizeof(b_dirent));
    n++;
  }
  b_closedir(d);
  if (cmp) qsort(out, n, sizeof(*out), (int (*)(const void *, const void *))cmp);
  *list = out;
  return n;
}

// select/ioctl only matter for sockets, which we don't have
static int b_select(int n, void *r, void *w, void *e, struct timeval *tv) {
  (void)n; (void)r; (void)w; (void)e;
  if (tv) sceKernelDelayThread(tv->tv_sec * 1000000 + tv->tv_usec);
  return 0;
}

static int b_ioctl(int fd, int req, ...) {
  L_ONCE("ioctl(%d, 0x%X) unsupported", fd, req);
  errno = EINVAL;
  return -1;
}

static int b_poll(void *fds, int n, int timeout) {
  (void)fds; (void)n;
  if (timeout > 0) sceKernelDelayThread(timeout * 1000);
  return 0;
}

EXPORT_TABLE(io) = {
  EXP_AS("pipe", b_pipe),
  EXP_AS("open", b_open),
  EXP_AS("read", b_read),
  EXP_AS("write", b_write),
  EXP_AS("writev", b_writev),
  EXP_AS("close", b_close),
  EXP_AS("lseek", b_lseek),
  EXP_AS("lseek64", b_lseek64),
  EXP_AS("stat", b_stat_fn),
  EXP_AS("fstat", b_fstat),
  EXP_AS("statfs", b_statfs_fn),
  EXP_AS("mkdir", b_mkdir),
  EXP_AS("rmdir", b_rmdir),
  EXP_AS("unlink", b_unlink),
  EXP_AS("rename", b_rename),
  EXP_AS("opendir", b_opendir),
  EXP_AS("readdir", b_readdir),
  EXP_AS("closedir", b_closedir),
  EXP_AS("scandir", b_scandir),
  EXP_AS("alphasort", b_alphasort),
  EXP_AS("select", b_select),
  EXP_AS("ioctl", b_ioctl),
  EXP_AS("poll", b_poll),
};
EXPORT_TABLE_END(io);
