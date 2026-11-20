// AInputQueue / AInputEvent fed from vita buttons, sticks and front touch.
// buttons -> android gamepad keycodes, sticks -> joystick axes, touch -> touchscreen.
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include "android/android.h"
#include "bionic/bionic.h"
#include "loader/exports.h"
#include "util/abi.h"
#include "util/log.h"

#define AINPUT_EVENT_TYPE_KEY    1
#define AINPUT_EVENT_TYPE_MOTION 2
#define SRC_GAMEPAD     0x00000401
#define SRC_TOUCHSCREEN 0x00001002
#define SRC_JOYSTICK    0x01000010
#define KEY_DOWN 0
#define KEY_UP   1
#define MOTION_DOWN 0
#define MOTION_UP   1
#define MOTION_MOVE 2
#define MOTION_POINTER_DOWN 5
#define MOTION_POINTER_UP   6

// android axis ids
#define AXIS_X 0
#define AXIS_Y 1
#define AXIS_Z 11
#define AXIS_RZ 14
#define AXIS_HAT_X 15
#define AXIS_HAT_Y 16
#define AXIS_GAS 22     // r2, the game reads it (AMotionEvent_getAxisValue)
#define AXIS_BRAKE 23   // l2
#define N_AXES 24

#define MAX_PTRS 6

typedef struct {
  int type, source, action, keycode;
  int nptr;
  int ids[MAX_PTRS];
  float x[MAX_PTRS], y[MAX_PTRS];
  float axes[N_AXES];
} vevent;

#define QSIZE 128
typedef struct {
  int magic;
  vevent ev[QSIZE];
  int head, tail;
  SceKernelLwMutexWork lock;
} vqueue;

static vqueue q = { 0x1A9E7 };
static int q_ready;

void *input_queue(void) {
  if (!q_ready) {
    sceKernelCreateLwMutex(&q.lock, "inputq", 0, 0, NULL);
    q_ready = 1;
  }
  return &q;
}

int input_pending(void *queue) {
  vqueue *vq = queue;
  return vq->head != vq->tail;
}

static void push(const vevent *e) {
  sceKernelLockLwMutex(&q.lock, 1, NULL);
  int next = (q.head + 1) % QSIZE;
  if (next != q.tail) {
    q.ev[q.head] = *e;
    q.head = next;
  }
  sceKernelUnlockLwMutex(&q.lock, 1);
  looper_poke();
}

// --- the NDK api ---
extern int looper_attach_queue(void *lp, void *queue, int ident, void *cb, void *data);
extern void looper_detach_queue(void *queue);

static void b_AInputQueue_attachLooper(void *queue, void *lp, int ident, void *cb, void *data) {
  L("AInputQueue_attachLooper(ident %d)", ident);
  looper_attach_queue(lp, queue, ident, cb, data);
}
static void b_AInputQueue_detachLooper(void *queue) { looper_detach_queue(queue); }

// events get copied out so the game can hold onto them until finishEvent
static vevent held[QSIZE];
static int held_i;

static int b_AInputQueue_getEvent(vqueue *vq, vevent **out) {
  int r = -1;
  sceKernelLockLwMutex(&vq->lock, 1, NULL);
  if (vq->head != vq->tail) {
    held[held_i] = vq->ev[vq->tail];
    *out = &held[held_i];
    held_i = (held_i + 1) % QSIZE;
    vq->tail = (vq->tail + 1) % QSIZE;
    r = 0;
  }
  sceKernelUnlockLwMutex(&vq->lock, 1);
  return r;
}

static int b_AInputQueue_preDispatchEvent(void *q_, void *e) { (void)q_; (void)e; return 0; }
static void b_AInputQueue_finishEvent(void *q_, void *e, int handled) { (void)q_; (void)e; (void)handled; }

static int b_AInputEvent_getType(const vevent *e) { return e->type; }
static int b_AInputEvent_getSource(const vevent *e) { return e->source; }
static int b_AKeyEvent_getAction(const vevent *e) { return e->action; }
static int b_AKeyEvent_getKeyCode(const vevent *e) { return e->keycode; }
static int b_AMotionEvent_getAction(const vevent *e) { return e->action; }
static size_t b_AMotionEvent_getPointerCount(const vevent *e) { return e->nptr; }
static int b_AMotionEvent_getPointerId(const vevent *e, size_t i) { return i < MAX_PTRS ? e->ids[i] : 0; }
// float returns -> softfp
static SOFTFP float b_AMotionEvent_getX(const vevent *e, size_t i) { return i < MAX_PTRS ? e->x[i] : 0; }
static SOFTFP float b_AMotionEvent_getY(const vevent *e, size_t i) { return i < MAX_PTRS ? e->y[i] : 0; }
static SOFTFP float b_AMotionEvent_getAxisValue(const vevent *e, int axis, size_t i) {
  (void)i;
  return axis >= 0 && axis < N_AXES ? e->axes[axis] : 0;
}

// --- pumping the vita side ---
static const struct { unsigned btn; int key; } keymap[] = {
  { SCE_CTRL_CROSS,    96 },  // BUTTON_A
  { SCE_CTRL_CIRCLE,   97 },  // BUTTON_B
  { SCE_CTRL_SQUARE,   99 },  // BUTTON_X
  { SCE_CTRL_TRIANGLE, 100 }, // BUTTON_Y
  { SCE_CTRL_LTRIGGER, 102 }, // BUTTON_L1
  { SCE_CTRL_RTRIGGER, 103 }, // BUTTON_R1
  { SCE_CTRL_START,    108 }, // BUTTON_START
  { SCE_CTRL_SELECT,   4 },   // KEYCODE_BACK: android back, the game's menus have no on screen one
  { SCE_CTRL_UP,       19 },
  { SCE_CTRL_DOWN,     20 },
  { SCE_CTRL_LEFT,     21 },
  { SCE_CTRL_RIGHT,    22 },
};

static unsigned prev_btn;
static float prev_axes[N_AXES];
static int prev_touch_n;
static int prev_touch_id[MAX_PTRS];
static int touch_inited;

static float stick(unsigned char v) {
  float f = ((int)v - 128) / 127.0f;
  if (f > -0.15f && f < 0.15f) f = 0;  // deadzone
  if (f > 1) f = 1;
  if (f < -1) f = -1;
  return f;
}

void input_poll(void) {
  if (!touch_inited) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    touch_inited = 1;
  }

  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  sceCtrlPeekBufferPositive(0, &pad, 1);

  // buttons
  unsigned changed = pad.buttons ^ prev_btn;
  for (unsigned i = 0; i < ARRAY_LEN(keymap); i++) {
    if (!(changed & keymap[i].btn)) continue;
    vevent e = { 0 };
    e.type = AINPUT_EVENT_TYPE_KEY;
    e.source = SRC_GAMEPAD;
    e.keycode = keymap[i].key;
    e.action = (pad.buttons & keymap[i].btn) ? KEY_DOWN : KEY_UP;
    push(&e);
  }
  prev_btn = pad.buttons;

  // sticks + dpad as hat
  vevent m = { 0 };
  m.type = AINPUT_EVENT_TYPE_MOTION;
  m.source = SRC_JOYSTICK;
  m.action = MOTION_MOVE;
  m.nptr = 1;
  m.axes[AXIS_X] = stick(pad.lx);
  m.axes[AXIS_Y] = stick(pad.ly);
  m.axes[AXIS_Z] = stick(pad.rx);
  m.axes[AXIS_RZ] = stick(pad.ry);
  m.axes[AXIS_HAT_X] = (pad.buttons & SCE_CTRL_RIGHT) ? 1 : (pad.buttons & SCE_CTRL_LEFT) ? -1 : 0;
  m.axes[AXIS_HAT_Y] = (pad.buttons & SCE_CTRL_DOWN) ? 1 : (pad.buttons & SCE_CTRL_UP) ? -1 : 0;
  // the vita has no l2/r2: rear touch, left half = l2 (brake), right half =
  // r2 (gas). the game reads both trigger axes. also sent as key presses
  // (BUTTON_L2 104 / BUTTON_R2 105) in case a menu wants buttons
  // opt-in: fingers resting on the back held l2/r2 down and the game stopped
  // taking any other input (hw)
  static int rear = -1;
  if (rear < 0) {
    extern int config_int(const char *key, int def);
    rear = config_int("rear_triggers", 0) != 0;
  }
  if (rear) {
    SceTouchData bt;
    memset(&bt, 0, sizeof(bt));
    sceTouchPeek(SCE_TOUCH_PORT_BACK, &bt, 1);
    int l2 = 0, r2 = 0;
    for (unsigned i = 0; i < bt.reportNum && i < 8; i++) {
      if (bt.report[i].x < 960) l2 = 1; else r2 = 1;
    }
    m.axes[AXIS_BRAKE] = (float)l2;
    m.axes[AXIS_GAS] = (float)r2;
    static int prev_l2, prev_r2;
    for (int k = 0; k < 2; k++) {
      int now = k ? r2 : l2, *prev = k ? &prev_r2 : &prev_l2;
      if (now == *prev) continue;
      vevent e = { 0 };
      e.type = AINPUT_EVENT_TYPE_KEY;
      e.source = SRC_GAMEPAD;
      e.keycode = k ? 105 : 104;
      e.action = now ? KEY_DOWN : KEY_UP;
      push(&e);
      *prev = now;
    }
  }
  if (memcmp(m.axes, prev_axes, sizeof(prev_axes))) {
    push(&m);
    memcpy(prev_axes, m.axes, sizeof(prev_axes));
  }

  // front touch. panel is 1920x1088, screen is 960x544
  SceTouchData t;
  memset(&t, 0, sizeof(t));
  sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1);
  int n = t.reportNum > MAX_PTRS ? MAX_PTRS : t.reportNum;

  vevent te = { 0 };
  te.type = AINPUT_EVENT_TYPE_MOTION;
  te.source = SRC_TOUCHSCREEN;
  te.nptr = n;
  for (int i = 0; i < n; i++) {
    te.ids[i] = t.report[i].id;
    te.x[i] = t.report[i].x / 2.0f;
    te.y[i] = t.report[i].y / 2.0f;
  }

  if (n > 0 && prev_touch_n == 0) {
    te.action = MOTION_DOWN;
    push(&te);
  } else if (n == 0 && prev_touch_n > 0) {
    // lift, report the last known pointer
    te.action = MOTION_UP;
    te.nptr = 1;
    te.ids[0] = prev_touch_id[0];
    push(&te);
  } else if (n > prev_touch_n) {
    te.action = MOTION_POINTER_DOWN | ((n - 1) << 8);
    push(&te);
  } else if (n < prev_touch_n) {
    te.action = MOTION_POINTER_UP | ((n) << 8);
    push(&te);
  } else if (n > 0) {
    te.action = MOTION_MOVE;
    push(&te);
  }
  prev_touch_n = n;
  for (int i = 0; i < n; i++) prev_touch_id[i] = te.ids[i];
}

EXPORT_TABLE(input) = {
  EXP_AS("AInputQueue_attachLooper", b_AInputQueue_attachLooper),
  EXP_AS("AInputQueue_detachLooper", b_AInputQueue_detachLooper),
  EXP_AS("AInputQueue_getEvent", b_AInputQueue_getEvent),
  EXP_AS("AInputQueue_preDispatchEvent", b_AInputQueue_preDispatchEvent),
  EXP_AS("AInputQueue_finishEvent", b_AInputQueue_finishEvent),
  EXP_AS("AInputEvent_getType", b_AInputEvent_getType),
  EXP_AS("AInputEvent_getSource", b_AInputEvent_getSource),
  EXP_AS("AKeyEvent_getAction", b_AKeyEvent_getAction),
  EXP_AS("AKeyEvent_getKeyCode", b_AKeyEvent_getKeyCode),
  EXP_AS("AMotionEvent_getAction", b_AMotionEvent_getAction),
  EXP_AS("AMotionEvent_getPointerCount", b_AMotionEvent_getPointerCount),
  EXP_AS("AMotionEvent_getPointerId", b_AMotionEvent_getPointerId),
  EXP_AS("AMotionEvent_getX", b_AMotionEvent_getX),
  EXP_AS("AMotionEvent_getY", b_AMotionEvent_getY),
  EXP_AS("AMotionEvent_getAxisValue", b_AMotionEvent_getAxisValue),
};
EXPORT_TABLE_END(input);
