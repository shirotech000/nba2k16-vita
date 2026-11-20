#pragma once
#include <stdint.h>
#include <stddef.h>

// NDK structs the game actually reads. field order has to match the NDK.
typedef struct ANativeActivity ANativeActivity;

typedef struct {
  void (*onStart)(ANativeActivity *);
  void (*onResume)(ANativeActivity *);
  void *(*onSaveInstanceState)(ANativeActivity *, size_t *outSize);
  void (*onPause)(ANativeActivity *);
  void (*onStop)(ANativeActivity *);
  void (*onDestroy)(ANativeActivity *);
  void (*onWindowFocusChanged)(ANativeActivity *, int hasFocus);
  void (*onNativeWindowCreated)(ANativeActivity *, void *window);
  void (*onNativeWindowResized)(ANativeActivity *, void *window);
  void (*onNativeWindowRedrawNeeded)(ANativeActivity *, void *window);
  void (*onNativeWindowDestroyed)(ANativeActivity *, void *window);
  void (*onInputQueueCreated)(ANativeActivity *, void *queue);
  void (*onInputQueueDestroyed)(ANativeActivity *, void *queue);
  void (*onContentRectChanged)(ANativeActivity *, const void *rect);
  void (*onConfigurationChanged)(ANativeActivity *);
  void (*onLowMemory)(ANativeActivity *);
} ANativeActivityCallbacks;

struct ANativeActivity {
  ANativeActivityCallbacks *callbacks;
  void *vm;          // JavaVM*
  void *env;         // JNIEnv*
  void *clazz;       // jobject, the activity
  const char *internalDataPath;
  const char *externalDataPath;
  int32_t sdkVersion;
  void *instance;
  void *assetManager;
  const char *obbPath;
};

typedef struct {
  int magic;
  int width, height;
} fake_window;

#define SCREEN_W 960
#define SCREEN_H 544

// activity.c
void activity_boot(void *onCreate);
void activity_loop(void);           // never returns, pumps input
extern ANativeActivity g_activity;
extern fake_window g_window;

// input.c
void *input_queue(void);
int input_pending(void *queue);
void input_poll(void);             // read ctrl/touch, push events

// looper.c
// (looper_poke lives in bionic.h since the pipes call it)
