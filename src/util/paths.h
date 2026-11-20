#pragma once
// where stuff lives on the vita
#define DATA_PATH     "ux0:data/nba2k16"
#define GAME_SO_PATH  DATA_PATH "/libnba2k16_clean_opt.so"
#define STL_SO_PATH   DATA_PATH "/libgnustl_shared.so"
#define OBB_DIR       DATA_PATH "/obb"
// the game gets its own subfolders. it cleans out android's data dirs on
// boot, and that used to take our .so files + config.txt with it.
#define INTERNAL_DIR  DATA_PATH "/internal"   // /data/data/<pkg>
#define EXTERNAL_DIR  DATA_PATH "/external"   // /sdcard/Android/data/<pkg>
#define FILES_DIR     INTERNAL_DIR "/files"
#define CACHE_DIR     INTERNAL_DIR "/cache"
#define SHADER_CACHE  DATA_PATH "/shadercache"
#define LOG_PATH      DATA_PATH "/port.log"
#define SHACCCG_PATH  "ur0:data/libshacccg.suprx"

#define PACKAGE_NAME  "com.t2ksports.nba2k16android"
#define GAME_VERSION_CODE 29
#define GAME_VERSION_NAME "0.0.29"
#define OBB_FILE      OBB_DIR "/main.29." PACKAGE_NAME ".obb"

// what the game gets told (activity paths, File objects from jni). has to
// be the android spelling: the game's file layer treats anything that
// doesn't start with '/' as relative and glues its current folder in
// front ("files/ux0:data/..."). path_fix maps these back to the dirs above.
#define ANDROID_FILES_DIR    "/data/data/" PACKAGE_NAME "/files"
#define ANDROID_CACHE_DIR    "/data/data/" PACKAGE_NAME "/cache"
#define ANDROID_EXTERNAL_DIR "/sdcard/Android/data/" PACKAGE_NAME
#define ANDROID_OBB_DIR      "/sdcard/Android/obb/" PACKAGE_NAME
#define ANDROID_SDCARD       "/sdcard"

// turns an android path into a vita one. returns out.
char *path_fix(const char *in, char *out, int outlen);
// 1 if a fixed path has a ':' anywhere but right after the device name
int path_is_bad(const char *p);
// path_fix, but bad paths get logged + refused: -1 with errno = ENOENT.
// every path-taking shim goes through this, never hand junk to sceIo.
int path_fix_checked(const char *what, const char *in, char *out, int outlen);

// 1 if ux0:data/nba2k16/vita3k.flag exists (tools/vita3k/run.ps1 drops it
// into the emulator's profile only). checked once, turns on emu-only stuff.
int emu_is_vita3k(void);
