#pragma once
// bits of bionic (android libc, 32-bit arm) that differ from newlib
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

// linux errno values where newlib disagrees. the small common ones
// (ENOENT, EEXIST, EINVAL, EAGAIN, EBUSY...) are identical.
#define B_ENOSYS     38
#define B_ENOTEMPTY  39
#define B_ENETDOWN   100
#define B_ETIMEDOUT  110
#define B_EINPROGRESS 115

// open() flags on linux/arm
#define B_O_WRONLY    01
#define B_O_RDWR      02
#define B_O_CREAT     0100
#define B_O_EXCL      0200
#define B_O_TRUNC     01000
#define B_O_APPEND    02000
#define B_O_NONBLOCK  04000
#define B_O_DIRECTORY 040000

typedef struct {
  unsigned long long st_dev;
  unsigned char __pad0[4];
  unsigned long __st_ino;
  unsigned int st_mode;
  unsigned int st_nlink;
  unsigned long st_uid;
  unsigned long st_gid;
  unsigned long long st_rdev;
  unsigned char __pad3[4];
  long long st_size;
  unsigned long st_blksize;
  unsigned long long st_blocks;
  long st_atime_, st_atime_nsec;
  long st_mtime_, st_mtime_nsec;
  long st_ctime_, st_ctime_nsec;
  unsigned long long st_ino;
} b_stat;
_Static_assert(sizeof(b_stat) == 104, "bionic struct stat is 104 bytes on arm");
_Static_assert(offsetof(b_stat, st_size) == 48, "st_size @48");
_Static_assert(offsetof(b_stat, st_mtime_) == 80, "st_mtime @80");

typedef struct {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[256];
} b_dirent;
_Static_assert(offsetof(b_dirent, d_name) == 19, "d_name @19");

#define B_DT_DIR 4
#define B_DT_REG 8

typedef struct {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
  long tm_gmtoff;
  const char *tm_zone;
} b_tm;

// stdio.c: game FILE* -> real FILE*
FILE *b_file(FILE *f);

// io.c: our fake pipes (for the native app glue's looper)
int vpipe_is(int fd);
int vpipe_available(int fd);
// wakes up anybody sitting in looper_wait()
void looper_poke(void);
