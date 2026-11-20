// fake NativeActivity. we play the part of android's framework: create the
// activity, walk it through the lifecycle, hand it a window + input queue.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>

#include "android/android.h"
#include "android/jni.h"
#include "loader/exports.h"
#include "loader/so_loader.h"
#include "util/log.h"
#include "util/paths.h"

ANativeActivity g_activity;
fake_window g_window = { 0x77111D0, SCREEN_W, SCREEN_H };
static ANativeActivityCallbacks callbacks;
static int fake_asset_manager = 0xA55E7;

typedef void (*onCreate_fn)(ANativeActivity *, void *saved, size_t saved_size);

#define CB(name, ...) do { \
    if (callbacks.name) { L("activity: " #name); callbacks.name(&g_activity, ##__VA_ARGS__); } \
  } while (0)

void activity_boot(void *onCreate) {
  memset(&g_activity, 0, sizeof(g_activity));
  g_activity.callbacks = &callbacks;
  g_activity.vm = jni_vm();
  g_activity.env = jni_env();
  g_activity.clazz = jni_activity_object();
  g_activity.internalDataPath = FILES_DIR;
  g_activity.externalDataPath = DATA_PATH;
  g_activity.sdkVersion = 19;
  g_activity.assetManager = &fake_asset_manager;
  g_activity.obbPath = OBB_DIR;

  L("activity: ANativeActivity_onCreate");
  ((onCreate_fn)onCreate)(&g_activity, NULL, 0);
  L("activity: onCreate returned");

  // same order android does it
  CB(onStart);
  CB(onResume);
  CB(onInputQueueCreated, input_queue());
  CB(onNativeWindowCreated, &g_window);
  CB(onWindowFocusChanged, 1);
}

void activity_loop(void) {
  // the game runs in its own thread (android_main), we just feed it input
  for (;;) {
    input_poll();
    sceKernelDelayThread(8 * 1000);
  }
}

// --- misc NDK bits ---
static void b_ANativeActivity_setWindowFlags(ANativeActivity *a, uint32_t add, uint32_t rem) {
  (void)a;
  L("ANativeActivity_setWindowFlags(+0x%X -0x%X)", add, rem);
}

static int b_ANativeWindow_setBuffersGeometry(void *w, int width, int height, int fmt) {
  (void)w;
  L("ANativeWindow_setBuffersGeometry(%d, %d, %d)", width, height, fmt);
  return 0;
}

static int fake_config = 0xC0F1;
static void *b_AConfiguration_new(void) { return &fake_config; }
static void b_AConfiguration_delete(void *c) { (void)c; }
static void b_AConfiguration_fromAssetManager(void *c, void *am) { (void)c; (void)am; }
static void b_AConfiguration_getLanguage(void *c, char *out) { (void)c; out[0] = 'e'; out[1] = 'n'; }

// --- android log ---
static const char prio_ch[] = "??VDIWEFS";

static int b_android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  L("%c/%s: %s", prio_ch[prio & 7], tag ? tag : "?", buf);
  return 0;
}

static int b_android_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  b_android_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return 0;
}

static int b_android_log_write(int prio, const char *tag, const char *msg) {
  L("%c/%s: %s", prio_ch[prio & 7], tag ? tag : "?", msg);
  return 0;
}

EXPORT_TABLE(android) = {
  EXP_AS("ANativeActivity_setWindowFlags", b_ANativeActivity_setWindowFlags),
  EXP_AS("ANativeWindow_setBuffersGeometry", b_ANativeWindow_setBuffersGeometry),
  EXP_AS("AConfiguration_new", b_AConfiguration_new),
  EXP_AS("AConfiguration_delete", b_AConfiguration_delete),
  EXP_AS("AConfiguration_fromAssetManager", b_AConfiguration_fromAssetManager),
  EXP_AS("AConfiguration_getLanguage", b_AConfiguration_getLanguage),
  EXP_AS("__android_log_print", b_android_log_print),
  EXP_AS("__android_log_vprint", b_android_log_vprint),
  EXP_AS("__android_log_write", b_android_log_write),
};
EXPORT_TABLE_END(android);
