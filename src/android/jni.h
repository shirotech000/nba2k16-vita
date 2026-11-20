#pragma once
#include <stdint.h>

// fake JNI. there's no java here, every lookup/call gets logged and
// answered by a little handler table in jni.c (or 0/NULL if unknown).

void jni_init(void);
void *jni_vm(void);              // JavaVM*
void *jni_env(void);             // JNIEnv*
void *jni_activity_object(void); // the "gameactivity" jobject

// natives the game registered with RegisterNatives, or exported Java_* syms
void *jni_find_native(const char *name);
