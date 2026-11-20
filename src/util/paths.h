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

// turns an android path into a vita one. returns out.
char *path_fix(const char *in, char *out, int outlen);
