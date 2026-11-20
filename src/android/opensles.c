// OpenSL ES 1.0.1 (android flavour) on top of sceAudioOut, from scratch.
//
// what nba2k16 does with it (checked against the .so):
//  - VCAudio_InitModule: engine, QueryNumSupportedInterfaces(LISTENER),
//    output mix + optional ENVIRONMENTALREVERB, then up to 8 voices
//  - voices: PCM buffer queue players (0x800007BD, 1/2/4 buffers, 8/16 bit,
//    mono/stereo, any rate) with BUFFERQUEUE + VOLUME, first time also
//    3DLOCATION/3DSOURCE (not required, game flips a flag on result 12)
//  - AAC streams: a decoder player (ANDROIDBUFFERQUEUE src with
//    "audio/vnd.android.aac-adts", simple BQ sink). no decoder here yet, we
//    refuse it and the game marks that voice failed.
//
// one mixer thread owns a sceAudioOut port, pulls from every playing player
// and runs the game's buffer queue callbacks itself (our lock not held).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include "loader/exports.h"
#include "util/log.h"
#include "android/opensles_types.h"

#define OUT_RATE    48000
#define GRAIN       1024                         // samples per output call, ~21ms
#define GRAIN_US    (GRAIN * 1000000 / OUT_RATE)
#define MAX_PLAYERS 64
#define MAX_QUEUE   64                           // buffers per player queue
#define CB_ROUNDS   64                           // callback trips per player per grain
#define MIXER_STACK (512 * 1024)                 // game decode code runs in our callbacks
#define MIXER_PRIO  64                           // top of the user range
#define ONE         0x10000u                     // resampler 16.16

// vtable slots the game binary was seen calling, by byte offset
_Static_assert(offsetof(struct SLObjectItf_, Realize) == 0, "obj slot");
_Static_assert(offsetof(struct SLObjectItf_, GetInterface) == 12, "obj slot");
_Static_assert(offsetof(struct SLObjectItf_, Destroy) == 24, "obj slot");
_Static_assert(sizeof(struct SLObjectItf_) == 40, "obj vtable size");
_Static_assert(offsetof(struct SLEngineItf_, CreateAudioPlayer) == 8, "engine slot");
_Static_assert(offsetof(struct SLEngineItf_, CreateListener) == 20, "engine slot");
_Static_assert(offsetof(struct SLEngineItf_, CreateOutputMix) == 28, "engine slot");
_Static_assert(offsetof(struct SLEngineItf_, QueryNumSupportedInterfaces) == 40, "engine slot");
_Static_assert(sizeof(struct SLEngineItf_) == 60, "engine vtable size");
_Static_assert(offsetof(struct SLPlayItf_, SetPlayState) == 0, "play slot");
_Static_assert(offsetof(struct SLPlayItf_, RegisterCallback) == 16, "play slot");
_Static_assert(offsetof(struct SLPlayItf_, SetCallbackEventsMask) == 20, "play slot");
_Static_assert(sizeof(struct SLPlayItf_) == 48, "play vtable size");
_Static_assert(offsetof(struct SLBufferQueueItf_, Enqueue) == 0, "bq slot");
_Static_assert(offsetof(struct SLBufferQueueItf_, Clear) == 4, "bq slot");
_Static_assert(offsetof(struct SLBufferQueueItf_, RegisterCallback) == 12, "bq slot");
_Static_assert(offsetof(struct SLVolumeItf_, SetVolumeLevel) == 0, "vol slot");
_Static_assert(offsetof(struct SLVolumeItf_, GetMaxVolumeLevel) == 8, "vol slot");
_Static_assert(offsetof(struct SLVolumeItf_, SetMute) == 12, "vol slot");
_Static_assert(sizeof(struct SLVolumeItf_) == 36, "vol vtable size");
_Static_assert(sizeof(struct SLInterfaceID_) == 16, "iid size");
_Static_assert(sizeof(SLDataFormat_PCM) == 28, "pcm format size");

// ---------------------------------------------------------------------------
// interface ids. not the khronos GUIDs, just unique blobs: the game only ever
// hands back the pointers we export. memcmp fallback catches copies.

#define IID_BLOB(n) { 0x56495441u, 0x534c, 0x3136, (n), { 'n', 'b', 'a', '2', 'k', (n) } }

static const struct SLInterfaceID_ iid_engine   = IID_BLOB(1);
static const struct SLInterfaceID_ iid_play     = IID_BLOB(2);
static const struct SLInterfaceID_ iid_bq       = IID_BLOB(3);
static const struct SLInterfaceID_ iid_abqsrc   = IID_BLOB(4);
static const struct SLInterfaceID_ iid_volume   = IID_BLOB(5);
static const struct SLInterfaceID_ iid_3dloc    = IID_BLOB(6);
static const struct SLInterfaceID_ iid_3dsrc    = IID_BLOB(7);
static const struct SLInterfaceID_ iid_envverb  = IID_BLOB(8);

const SLInterfaceID SL_IID_ENGINE                   = &iid_engine;
const SLInterfaceID SL_IID_PLAY                     = &iid_play;
const SLInterfaceID SL_IID_BUFFERQUEUE              = &iid_bq;
const SLInterfaceID SL_IID_ANDROIDBUFFERQUEUESOURCE = &iid_abqsrc;
const SLInterfaceID SL_IID_VOLUME                   = &iid_volume;
const SLInterfaceID SL_IID_3DLOCATION               = &iid_3dloc;
const SLInterfaceID SL_IID_3DSOURCE                 = &iid_3dsrc;
const SLInterfaceID SL_IID_ENVIRONMENTALREVERB      = &iid_envverb;

enum {
  IX_ENGINE, IX_PLAY, IX_BUFFERQUEUE, IX_ABQSOURCE, IX_VOLUME,
  IX_3DLOCATION, IX_3DSOURCE, IX_ENVREVERB,
  IX_COUNT, IX_UNKNOWN = IX_COUNT
};

static const struct {
  const struct SLInterfaceID_ *id;
  const char *name;
} iids[IX_COUNT] = {
  [IX_ENGINE]      = { &iid_engine,  "ENGINE" },
  [IX_PLAY]        = { &iid_play,    "PLAY" },
  [IX_BUFFERQUEUE] = { &iid_bq,      "BUFFERQUEUE" },
  [IX_ABQSOURCE]   = { &iid_abqsrc,  "ANDROIDBUFFERQUEUESOURCE" },
  [IX_VOLUME]      = { &iid_volume,  "VOLUME" },
  [IX_3DLOCATION]  = { &iid_3dloc,   "3DLOCATION" },
  [IX_3DSOURCE]    = { &iid_3dsrc,   "3DSOURCE" },
  [IX_ENVREVERB]   = { &iid_envverb, "ENVIRONMENTALREVERB" },
};

static int iid_lookup(SLInterfaceID id) {
  if (!id) return IX_UNKNOWN;
  for (int i = 0; i < IX_COUNT; i++)
    if (id == iids[i].id) return i;
  for (int i = 0; i < IX_COUNT; i++)
    if (!memcmp(id, iids[i].id, sizeof(*id))) return i;
  return IX_UNKNOWN;
}

// log each (who, iid) combo once so we see what the game really asks for
enum { WHO_SLCREATE, WHO_ENGINE_GET, WHO_MIX_CREATE, WHO_MIX_GET,
       WHO_PLAYER_CREATE, WHO_PLAYER_GET, WHO_COUNT };
static const char *const who_names[WHO_COUNT] = {
  "slCreateEngine", "engine GetInterface", "CreateOutputMix", "outputmix GetInterface",
  "CreateAudioPlayer", "player GetInterface",
};
static volatile uint32_t seen_iid[WHO_COUNT];

static void log_iid(int who, int ix, SLInterfaceID id, int required, const char *verdict) {
  uint32_t bit = 1u << ix;
  if (__sync_fetch_and_or(&seen_iid[who], bit) & bit) return;
  if (ix == IX_UNKNOWN)
    L("opensles: %s: unknown iid %p%s -> %s", who_names[who], (const void *)id,
      required ? " (required)" : "", verdict);
  else
    L("opensles: %s: SL_IID_%s%s -> %s", who_names[who], iids[ix].name,
      required ? " (required)" : "", verdict);
}

// ---------------------------------------------------------------------------
// objects

enum { K_ENGINE, K_MIX, K_PLAYER };

typedef struct {
  const struct SLObjectItf_ *itf; // SLObjectItf points here, keep it first
  int kind;
  SLuint32 state;
  slObjectCallback cb;
  void *cb_ctx;
  SLint32 prio;
  SLboolean preempt;
} sl_object;

typedef struct {
  sl_object obj;
  const struct SLEngineItf_ *eng;
} sl_engine;

typedef struct {
  sl_object obj;
} sl_mix;

typedef struct {
  const uint8_t *data;
  uint32_t size;
} sl_buf;

typedef struct {
  sl_object obj;
  const struct SLPlayItf_ *play;
  const struct SLBufferQueueItf_ *bq;
  const struct SLVolumeItf_ *vol;
  int slot;

  // source format, fixed at create
  uint32_t rate_mhz;
  int channels;
  int is8;
  uint32_t frame_bytes;
  uint32_t step; // source frames per output frame, 16.16

  // everything below is guarded by g_lock
  SLuint32 play_state;
  slPlayCallback play_cb;
  void *play_ctx;
  SLuint32 play_mask;
  SLmillisecond marker;
  int has_marker;
  SLmillisecond update_period;
  uint64_t frames; // source frames played since the last stop

  slBufferQueueCallback bq_cb;
  void *bq_ctx;
  uint32_t q_cap, q_head, q_count, q_pos;
  uint32_t play_index;
  uint32_t pending; // finished buffers whose callback hasn't run yet

  // resampler: interpolating s0 -> s1, frac in 16.16
  int32_t s0l, s0r, s1l, s1r;
  uint32_t frac;

  SLmillibel level;
  SLboolean mute, pan_on;
  SLpermille pan;
  int32_t gain_l, gain_r; // Q15, mute + pan folded in

  int cb_busy;    // mixer is inside this player's callback right now
  int dead;       // Destroy'd and unlinked
  int free_later; // Destroy'd from its own callback, mixer frees it

  sl_buf q[];
} sl_player;

#define OWNER(type, field, self) ((type *)((char *)(uintptr_t)(self) - offsetof(type, field)))

static const struct SLObjectItf_ obj_vtbl;
static const struct SLEngineItf_ engine_vtbl;
static const struct SLPlayItf_ play_vtbl;
static const struct SLBufferQueueItf_ bq_vtbl;
static const struct SLVolumeItf_ vol_vtbl;

// ---------------------------------------------------------------------------
// globals + lock

static SceKernelLwMutexWork g_lock __attribute__((aligned(8)));
static int g_lock_ok;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static sl_player *g_players[MAX_PLAYERS];
static volatile int g_mixer_thid = -1;
static volatile int g_mixer_up;
static volatile SceUInt64 g_heartbeat;

static int32_t g_acc[GRAIN * 2];
static int16_t g_pcm[2][GRAIN * 2] __attribute__((aligned(64)));

static inline void lock(void) {
  if (g_lock_ok) sceKernelLockLwMutex(&g_lock, 1, NULL);
}

static inline void unlock(void) {
  if (g_lock_ok) sceKernelUnlockLwMutex(&g_lock, 1);
}

static void check_mixer(void) {
  if (!g_mixer_up) {
    L_ONCE("opensles: no mixer thread, players will never advance");
    return;
  }
  SceUInt64 hb = g_heartbeat;
  if (hb && sceKernelGetProcessTimeWide() - hb > 2000000)
    L_ONCE("opensles: mixer hasn't ticked in >2s, stuck in a game callback?");
}

// ---------------------------------------------------------------------------
// mixer

static void reset_resampler(sl_player *p) {
  p->s0l = p->s0r = p->s1l = p->s1r = 0;
  p->frac = ONE; // first output ramps from 0 into the first frame
}

static void player_regain(sl_player *p) {
  // millibel -> linear, 0 mB is unity, we never boost
  float g = 0.f;
  if (!p->mute && p->level > SL_MILLIBEL_MIN) g = powf(10.f, (float)p->level / 2000.f);
  if (g > 1.f) g = 1.f;
  float gl = g, gr = g;
  if (p->pan_on) {
    if (p->pan > 0) gl *= (float)(1000 - p->pan) / 1000.f;
    else gr *= (float)(1000 + p->pan) / 1000.f;
  }
  p->gain_l = (int32_t)(gl * 32768.f + .5f);
  p->gain_r = (int32_t)(gr * 32768.f + .5f);
}

static void pop_buffer(sl_player *p) {
  p->q_head = (p->q_head + 1) % p->q_cap;
  p->q_count--;
  p->q_pos = 0;
  p->play_index++;
  p->pending++;
}

// shift one source frame into s1. 0 if the queue's dry. a buffer is popped
// as soon as its last frame is read (we've copied it, game can have it back).
static int fetch_frame(sl_player *p) {
  while (p->q_count) {
    const sl_buf *b = &p->q[p->q_head];
    if (p->q_pos + p->frame_bytes <= b->size) {
      const uint8_t *s = b->data + p->q_pos;
      int32_t l, r;
      if (p->is8) {
        l = ((int32_t)s[0] - 128) << 8;
        r = p->channels == 2 ? ((int32_t)s[1] - 128) << 8 : l;
      } else {
        int16_t a, c;
        memcpy(&a, s, 2);
        l = a;
        if (p->channels == 2) {
          memcpy(&c, s + 2, 2);
          r = c;
        } else {
          r = l;
        }
      }
      p->s0l = p->s1l;
      p->s0r = p->s1r;
      p->s1l = l;
      p->s1r = r;
      p->frames++;
      p->q_pos += p->frame_bytes;
      if (p->q_pos + p->frame_bytes > b->size) pop_buffer(p);
      return 1;
    }
    pop_buffer(p); // runt buffer, smaller than one frame
  }
  return 0;
}

// mix up to n frames into acc. bails early when a finished buffer wants its
// callback before we need the next frame (so the game can refill seamlessly)
// or when the queue runs dry.
static int mix_player(sl_player *p, int32_t *acc, int n) {
  const int32_t gl = p->gain_l, gr = p->gain_r;
  const uint32_t step = p->step;
  int i = 0;
  while (i < n) {
    while (p->frac >= ONE) {
      if (p->pending) return i;
      if (!fetch_frame(p)) return i;
      p->frac -= ONE;
    }
    int32_t f = (int32_t)(p->frac >> 2); // Q14 so the multiply can't overflow
    int32_t l = p->s0l + (((p->s1l - p->s0l) * f) >> 14);
    int32_t r = p->s0r + (((p->s1r - p->s0r) * f) >> 14);
    acc[i * 2]     += (l * gl) >> 15;
    acc[i * 2 + 1] += (r * gr) >> 15;
    p->frac += step;
    i++;
  }
  return i;
}

static void mix_grain(void) {
  int too_many_rounds = 0;
  memset(g_acc, 0, sizeof(g_acc));
  lock();
  for (int s = 0; s < MAX_PLAYERS; s++) {
    sl_player *p = g_players[s];
    if (!p || p->play_state != SL_PLAYSTATE_PLAYING) continue;
    int done = 0;
    for (int round = 0;; round++) {
      done += mix_player(p, g_acc + done * 2, GRAIN - done);
      if (!p->pending) break; // grain full or ran dry

      uint32_t n = p->pending;
      p->pending = 0;
      slBufferQueueCallback cb = p->bq_cb;
      void *ctx = p->bq_ctx;
      if (cb) {
        // hand buffers back outside our lock, the callback usually re-enqueues
        p->cb_busy = 1;
        unlock();
        while (n--) cb((SLBufferQueueItf)&p->bq, ctx);
        lock();
        p->cb_busy = 0;
        if (p->dead) {
          if (p->free_later) free(p);
          break; // don't touch p again
        }
        if (g_players[s] != p || p->play_state != SL_PLAYSTATE_PLAYING) break;
      }
      if (done >= GRAIN) break;
      if (round >= CB_ROUNDS) {
        too_many_rounds = 1;
        break;
      }
    }
  }
  unlock();
  if (too_many_rounds) L_ONCE("opensles: player burned through %d buffers in one grain, tiny buffers?", CB_ROUNDS);
}

extern int config_int(const char *key, int def);

static int open_port(int quiet) {
  // no_audio=1 in config.txt: never touch sceAudioOut, run on a silent
  // clock instead (callbacks still fire). handy for bisecting emulator crashes
  if (config_int("no_audio", 0)) {
    if (!quiet) L("opensles: no_audio set, running silent");
    return -1;
  }
  int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
  if (port >= 0) {
    // auto level control squashes the mix, turn it off (same as pspmania)
    sceAudioOutSetAlcMode(SCE_AUDIO_ALC_OFF);
    return port;
  }
  if (!quiet) L("opensles: BGM port open failed 0x%08X, trying MAIN", port);
  int p2 = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
  if (p2 >= 0) return p2;
  if (!quiet) L("opensles: MAIN port open failed 0x%08X too. running silent (callbacks still fire)", p2);
  return -1;
}

static void *mixer_main(void *arg) {
  (void)arg;
  g_mixer_thid = sceKernelGetThreadId();
  int r = sceKernelChangeThreadPriority(g_mixer_thid, MIXER_PRIO);
  if (r < 0) L("opensles: mixer priority change failed 0x%08X, carrying on", r);
  // park audio on core 2 so the game's main/render threads don't starve it
  sceKernelChangeThreadCpuAffinityMask(g_mixer_thid, SCE_KERNEL_CPU_MASK_USER_2);

  int port = open_port(0);
  SceUInt64 now = sceKernelGetProcessTimeWide();
  SceUInt64 next = now, retry_at = now + 2000000;
  L("opensles: mixer up, port %d, %d Hz stereo s16, grain %d", port, OUT_RATE, GRAIN);

  for (int cur = 0;; cur ^= 1) {
    mix_grain();

    int16_t *out = g_pcm[cur];
    for (int i = 0; i < GRAIN * 2; i++) {
      int32_t v = g_acc[i];
      out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }

    now = sceKernelGetProcessTimeWide();
    g_heartbeat = now;

    if (port >= 0) {
      r = sceAudioOutOutput(port, out); // blocks, this is our clock
      if (r < 0) {
        L("opensles: sceAudioOutOutput failed 0x%08X, dropping to a silent clock", r);
        sceAudioOutReleasePort(port);
        port = -1;
        next = now;
        retry_at = now + 2000000;
      }
      continue;
    }

    // no port: keep time ourselves so queues drain and callbacks keep coming
    next += GRAIN_US;
    if (next + 100000 < now) next = now; // way behind, resync
    if (next > now) sceKernelDelayThread((SceUInt)(next - now));
    if (now >= retry_at) {
      retry_at = now + 2000000;
      port = open_port(1);
      if (port >= 0) L("opensles: got an audio port back (%d)", port);
    }
  }
  return NULL;
}

static void sl_init_once(void) {
  int r = sceKernelCreateLwMutex(&g_lock, "opensles", 0, 0, NULL);
  if (r < 0) L("opensles: lwmutex create failed 0x%08X, running UNLOCKED (racy)", r);
  else g_lock_ok = 1;

  pthread_attr_t a;
  pthread_t th;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, MIXER_STACK);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  r = pthread_create(&th, &a, mixer_main, NULL);
  pthread_attr_destroy(&a);
  if (r != 0) {
    L("opensles: MIXER THREAD FAILED TO START (%d). no audio, no buffer callbacks", r);
    return;
  }
  g_mixer_up = 1;
}

// ---------------------------------------------------------------------------
// SLObjectItf (shared by engine, output mix, player)

static void player_destroy(sl_player *p);

static inline sl_object *OBJ(SLObjectItf self) {
  return (sl_object *)(uintptr_t)self;
}

static SLresult obj_Realize(SLObjectItf self, SLboolean async) {
  sl_object *o = OBJ(self);
  if (o->state == SL_OBJECT_STATE_REALIZED) return SL_RESULT_PRECONDITIONS_VIOLATED;
  if (async) L_ONCE("opensles: async Realize asked for, doing it synchronously");
  o->state = SL_OBJECT_STATE_REALIZED;
  return SL_RESULT_SUCCESS;
}

static SLresult obj_Resume(SLObjectItf self, SLboolean async) {
  (void)self;
  (void)async;
  return SL_RESULT_PRECONDITIONS_VIOLATED; // nothing ever gets suspended
}

static SLresult obj_GetState(SLObjectItf self, SLuint32 *pState) {
  if (!pState) return SL_RESULT_PARAMETER_INVALID;
  *pState = OBJ(self)->state;
  return SL_RESULT_SUCCESS;
}

static SLresult obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *pInterface) {
  if (!pInterface) return SL_RESULT_PARAMETER_INVALID;
  *(void **)pInterface = NULL;

  sl_object *o = OBJ(self);
  int ix = iid_lookup(iid);
  void *itf = NULL;
  int who;

  switch (o->kind) {
  case K_ENGINE:
    who = WHO_ENGINE_GET;
    if (ix == IX_ENGINE) itf = &((sl_engine *)o)->eng;
    break;
  case K_MIX:
    who = WHO_MIX_GET; // env reverb etc land here: unsupported, game copes
    break;
  default: {
    sl_player *p = (sl_player *)o;
    who = WHO_PLAYER_GET;
    if (ix == IX_PLAY) itf = &p->play;
    else if (ix == IX_BUFFERQUEUE) itf = &p->bq;
    else if (ix == IX_VOLUME) itf = &p->vol;
    break; // 3D* -> 12, the game checks for exactly that
  }
  }

  log_iid(who, ix, iid, 0, itf ? "ok" : "FEATURE_UNSUPPORTED");
  if (!itf) return SL_RESULT_FEATURE_UNSUPPORTED;
  if (o->state != SL_OBJECT_STATE_REALIZED) L_ONCE("opensles: GetInterface before Realize, allowing it");
  *(void **)pInterface = itf;
  return SL_RESULT_SUCCESS;
}

static SLresult obj_RegisterCallback(SLObjectItf self, slObjectCallback callback, void *pContext) {
  sl_object *o = OBJ(self);
  o->cb = callback;
  o->cb_ctx = pContext;
  return SL_RESULT_SUCCESS;
}

static void obj_AbortAsyncOperation(SLObjectItf self) {
  (void)self;
}

static void obj_Destroy(SLObjectItf self) {
  sl_object *o = OBJ(self);
  if (!o) return;
  if (o->kind == K_PLAYER) {
    player_destroy((sl_player *)o);
    return;
  }
  free(o);
}

static SLresult obj_SetPriority(SLObjectItf self, SLint32 priority, SLboolean preemptable) {
  sl_object *o = OBJ(self);
  o->prio = priority;
  o->preempt = preemptable;
  return SL_RESULT_SUCCESS;
}

static SLresult obj_GetPriority(SLObjectItf self, SLint32 *pPriority, SLboolean *pPreemptable) {
  if (!pPriority || !pPreemptable) return SL_RESULT_PARAMETER_INVALID;
  sl_object *o = OBJ(self);
  *pPriority = o->prio;
  *pPreemptable = o->preempt;
  return SL_RESULT_SUCCESS;
}

static SLresult obj_SetLossOfControlInterfaces(SLObjectItf self, SLint16 numInterfaces,
                                               SLInterfaceID *pInterfaceIDs, SLboolean enabled) {
  (void)self;
  (void)numInterfaces;
  (void)pInterfaceIDs;
  (void)enabled;
  return SL_RESULT_SUCCESS;
}

static const struct SLObjectItf_ obj_vtbl = {
  obj_Realize,
  obj_Resume,
  obj_GetState,
  obj_GetInterface,
  obj_RegisterCallback,
  obj_AbortAsyncOperation,
  obj_Destroy,
  obj_SetPriority,
  obj_GetPriority,
  obj_SetLossOfControlInterfaces,
};

static void obj_init(sl_object *o, int kind) {
  o->itf = &obj_vtbl;
  o->kind = kind;
  o->state = SL_OBJECT_STATE_UNREALIZED;
}

// ---------------------------------------------------------------------------
// SLPlayItf

#define PLAYER_OF_PLAY(self) OWNER(sl_player, play, self)

static SLresult play_SetPlayState(SLPlayItf self, SLuint32 state) {
  if (state < SL_PLAYSTATE_STOPPED || state > SL_PLAYSTATE_PLAYING) return SL_RESULT_PARAMETER_INVALID;
  sl_player *p = PLAYER_OF_PLAY(self);
  lock();
  if (state != p->play_state) {
    // android keeps the queue on stop (docs say Clear() after STOPPED),
    // we just reset position + resampler
    if (state == SL_PLAYSTATE_STOPPED) {
      p->frames = 0;
      reset_resampler(p);
    }
    p->play_state = state;
  }
  unlock();
  if (state == SL_PLAYSTATE_PLAYING) check_mixer();
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPlayState(SLPlayItf self, SLuint32 *pState) {
  if (!pState) return SL_RESULT_PARAMETER_INVALID;
  sl_player *p = PLAYER_OF_PLAY(self);
  lock();
  *pState = p->play_state;
  unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetDuration(SLPlayItf self, SLmillisecond *pMsec) {
  (void)self;
  if (!pMsec) return SL_RESULT_PARAMETER_INVALID;
  *pMsec = SL_TIME_UNKNOWN; // buffer queues have no duration
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPosition(SLPlayItf self, SLmillisecond *pMsec) {
  if (!pMsec) return SL_RESULT_PARAMETER_INVALID;
  sl_player *p = PLAYER_OF_PLAY(self);
  lock();
  uint64_t f = p->frames;
  unlock();
  *pMsec = (SLmillisecond)(f * 1000000ull / p->rate_mhz);
  return SL_RESULT_SUCCESS;
}

// play events (marker/pos update/head at end) are stored, never fired.
// android doesn't send HEADATEND for buffer queues either.
static SLresult play_RegisterCallback(SLPlayItf self, slPlayCallback callback, void *pContext) {
  sl_player *p = PLAYER_OF_PLAY(self);
  lock();
  p->play_cb = callback;
  p->play_ctx = pContext;
  unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetCallbackEventsMask(SLPlayItf self, SLuint32 eventFlags) {
  sl_player *p = PLAYER_OF_PLAY(self);
  p->play_mask = eventFlags;
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetCallbackEventsMask(SLPlayItf self, SLuint32 *pEventFlags) {
  if (!pEventFlags) return SL_RESULT_PARAMETER_INVALID;
  *pEventFlags = PLAYER_OF_PLAY(self)->play_mask;
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetMarkerPosition(SLPlayItf self, SLmillisecond mSec) {
  sl_player *p = PLAYER_OF_PLAY(self);
  p->marker = mSec;
  p->has_marker = 1;
  return SL_RESULT_SUCCESS;
}

static SLresult play_ClearMarkerPosition(SLPlayItf self) {
  PLAYER_OF_PLAY(self)->has_marker = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetMarkerPosition(SLPlayItf self, SLmillisecond *pMsec) {
  if (!pMsec) return SL_RESULT_PARAMETER_INVALID;
  sl_player *p = PLAYER_OF_PLAY(self);
  if (!p->has_marker) return SL_RESULT_PRECONDITIONS_VIOLATED;
  *pMsec = p->marker;
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetPositionUpdatePeriod(SLPlayItf self, SLmillisecond mSec) {
  PLAYER_OF_PLAY(self)->update_period = mSec;
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPositionUpdatePeriod(SLPlayItf self, SLmillisecond *pMsec) {
  if (!pMsec) return SL_RESULT_PARAMETER_INVALID;
  *pMsec = PLAYER_OF_PLAY(self)->update_period;
  return SL_RESULT_SUCCESS;
}

static const struct SLPlayItf_ play_vtbl = {
  play_SetPlayState,
  play_GetPlayState,
  play_GetDuration,
  play_GetPosition,
  play_RegisterCallback,
  play_SetCallbackEventsMask,
  play_GetCallbackEventsMask,
  play_SetMarkerPosition,
  play_ClearMarkerPosition,
  play_GetMarkerPosition,
  play_SetPositionUpdatePeriod,
  play_GetPositionUpdatePeriod,
};

// ---------------------------------------------------------------------------
// SLBufferQueueItf / SLAndroidSimpleBufferQueueItf (same thing)

#define PLAYER_OF_BQ(self) OWNER(sl_player, bq, self)

static SLresult bq_Enqueue(SLBufferQueueItf self, const void *pBuffer, SLuint32 size) {
  if (!pBuffer || !size) return SL_RESULT_PARAMETER_INVALID;
  sl_player *p = PLAYER_OF_BQ(self);
  SLresult r = SL_RESULT_SUCCESS;
  lock();
  if (p->q_count >= p->q_cap) {
    r = SL_RESULT_BUFFER_INSUFFICIENT;
  } else {
    // just the pointer, the game owns the memory until its callback
    sl_buf *b = &p->q[(p->q_head + p->q_count) % p->q_cap];
    b->data = pBuffer;
    b->size = size;
    p->q_count++;
  }
  unlock();
  return r;
}

static SLresult bq_Clear(SLBufferQueueItf self) {
  sl_player *p = PLAYER_OF_BQ(self);
  lock();
  p->q_head = p->q_count = p->q_pos = 0;
  p->pending = 0; // no callbacks for cleared buffers
  unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState(SLBufferQueueItf self, SLBufferQueueState *pState) {
  if (!pState) return SL_RESULT_PARAMETER_INVALID;
  sl_player *p = PLAYER_OF_BQ(self);
  lock();
  pState->count = p->q_count;
  pState->playIndex = p->play_index;
  unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(SLBufferQueueItf self, slBufferQueueCallback callback, void *pContext) {
  sl_player *p = PLAYER_OF_BQ(self);
  lock();
  p->bq_cb = callback;
  p->bq_ctx = pContext;
  unlock();
  return SL_RESULT_SUCCESS;
}

static const struct SLBufferQueueItf_ bq_vtbl = {
  bq_Enqueue,
  bq_Clear,
  bq_GetState,
  bq_RegisterCallback,
};

// ---------------------------------------------------------------------------
// SLVolumeItf

#define PLAYER_OF_VOL(self) OWNER(sl_player, vol, self)

static SLresult vol_SetVolumeLevel(SLVolumeItf self, SLmillibel level) {
  if (level > 0) return SL_RESULT_PARAMETER_INVALID; // max is 0 mB
  sl_player *p = PLAYER_OF_VOL(self);
  lock();
  p->level = level;
  player_regain(p);
  unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult vol_GetVolumeLevel(SLVolumeItf self, SLmillibel *pLevel) {
  if (!pLevel) return SL_RESULT_PARAMETER_INVALID;
  *pLevel = PLAYER_OF_VOL(self)->level;
  return SL_RESULT_SUCCESS;
}

static SLresult vol_GetMaxVolumeLevel(SLVolumeItf self, SLmillibel *pMaxLevel) {
  (void)self;
  if (!pMaxLevel) return SL_RESULT_PARAMETER_INVALID;
  *pMaxLevel = 0; // game pre-fills -32768 and clamps to this, so it must be written
  return SL_RESULT_SUCCESS;
}

static SLresult vol_SetMute(SLVolumeItf self, SLboolean mute) {
  sl_player *p = PLAYER_OF_VOL(self);
  lock();
  p->mute = mute ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
  player_regain(p);
  unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult vol_GetMute(SLVolumeItf self, SLboolean *pMute) {
  if (!pMute) return SL_RESULT_PARAMETER_INVALID;
  *pMute = PLAYER_OF_VOL(self)->mute;
  return SL_RESULT_SUCCESS;
}

static SLresult vol_EnableStereoPosition(SLVolumeItf self, SLboolean enable) {
  sl_player *p = PLAYER_OF_VOL(self);
  lock();
  p->pan_on = enable ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE;
  player_regain(p);
  unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult vol_IsEnabledStereoPosition(SLVolumeItf self, SLboolean *pEnable) {
  if (!pEnable) return SL_RESULT_PARAMETER_INVALID;
  *pEnable = PLAYER_OF_VOL(self)->pan_on;
  return SL_RESULT_SUCCESS;
}

static SLresult vol_SetStereoPosition(SLVolumeItf self, SLpermille stereoPosition) {
  if (stereoPosition < -1000 || stereoPosition > 1000) return SL_RESULT_PARAMETER_INVALID;
  sl_player *p = PLAYER_OF_VOL(self);
  lock();
  p->pan = stereoPosition;
  player_regain(p);
  unlock();
  return SL_RESULT_SUCCESS;
}

static SLresult vol_GetStereoPosition(SLVolumeItf self, SLpermille *pStereoPosition) {
  if (!pStereoPosition) return SL_RESULT_PARAMETER_INVALID;
  *pStereoPosition = PLAYER_OF_VOL(self)->pan;
  return SL_RESULT_SUCCESS;
}

static const struct SLVolumeItf_ vol_vtbl = {
  vol_SetVolumeLevel,
  vol_GetVolumeLevel,
  vol_GetMaxVolumeLevel,
  vol_SetMute,
  vol_GetMute,
  vol_EnableStereoPosition,
  vol_IsEnabledStereoPosition,
  vol_SetStereoPosition,
  vol_GetStereoPosition,
};

// ---------------------------------------------------------------------------
// player lifetime

static void player_destroy(sl_player *p) {
  lock();
  if (p->slot >= 0 && p->slot < MAX_PLAYERS && g_players[p->slot] == p) g_players[p->slot] = NULL;
  p->dead = 1;
  p->play_state = SL_PLAYSTATE_STOPPED;
  p->q_count = 0;
  if (p->cb_busy && sceKernelGetThreadId() == g_mixer_thid) {
    // destroyed from inside its own callback, mixer frees it on the way out
    p->free_later = 1;
    unlock();
    return;
  }
  // like android: wait out a callback that's already running. (so the game
  // must not hold a lock its callback wants while it calls Destroy, same rule)
  while (p->cb_busy) {
    unlock();
    sceKernelDelayThread(100);
    lock();
  }
  unlock();
  free(p);
}

// formats seen so far, so the log gets one line per distinct format
static struct {
  uint32_t rate_mhz, ch, bits, nbuf;
} g_fmt_seen[32];
static int g_fmt_n;

static int fmt_is_new(const SLDataFormat_PCM *f, uint32_t nbuf) {
  for (int i = 0; i < g_fmt_n; i++)
    if (g_fmt_seen[i].rate_mhz == f->samplesPerSec && g_fmt_seen[i].ch == f->numChannels &&
        g_fmt_seen[i].bits == f->bitsPerSample && g_fmt_seen[i].nbuf == nbuf)
      return 0;
  if (g_fmt_n >= (int)(sizeof(g_fmt_seen) / sizeof(g_fmt_seen[0]))) return 0;
  g_fmt_seen[g_fmt_n].rate_mhz = f->samplesPerSec;
  g_fmt_seen[g_fmt_n].ch = f->numChannels;
  g_fmt_seen[g_fmt_n].bits = f->bitsPerSample;
  g_fmt_seen[g_fmt_n].nbuf = nbuf;
  g_fmt_n++;
  return 1;
}

// ---------------------------------------------------------------------------
// SLEngineItf

static SLresult eng_CreateAudioPlayer(SLEngineItf self, SLObjectItf *pPlayer, SLDataSource *pAudioSrc,
                                      SLDataSink *pAudioSnk, SLuint32 numInterfaces,
                                      const SLInterfaceID *pInterfaceIds,
                                      const SLboolean *pInterfaceRequired) {
  (void)self;
  static unsigned refused_decoders;

  if (!pPlayer) return SL_RESULT_PARAMETER_INVALID;
  *pPlayer = NULL; // game only looks at this, never at our return code
  if (!pAudioSrc || !pAudioSrc->pLocator || !pAudioSnk || !pAudioSnk->pLocator)
    return SL_RESULT_PARAMETER_INVALID;
  if (numInterfaces && !pInterfaceIds) return SL_RESULT_PARAMETER_INVALID;

  // interfaces: log everything, only fail on something required we can't fake
  int want_abq = 0;
  for (SLuint32 i = 0; i < numInterfaces; i++) {
    int ix = iid_lookup(pInterfaceIds[i]);
    int req = pInterfaceRequired && pInterfaceRequired[i];
    switch (ix) {
    case IX_PLAY:
    case IX_BUFFERQUEUE:
    case IX_VOLUME:
      log_iid(WHO_PLAYER_CREATE, ix, pInterfaceIds[i], req, "ok");
      break;
    case IX_3DLOCATION:
    case IX_3DSOURCE:
    case IX_ENVREVERB:
      log_iid(WHO_PLAYER_CREATE, ix, pInterfaceIds[i], req, "accepted, GetInterface says unsupported");
      break;
    case IX_ABQSOURCE:
      // decoder players get their own louder refusal below
      log_iid(WHO_PLAYER_CREATE, ix, pInterfaceIds[i], req, req ? "no decoder, refusing player" : "ignored");
      if (req) want_abq = 1;
      break;
    default:
      log_iid(WHO_PLAYER_CREATE, ix, pInterfaceIds[i], req, req ? "unsupported, refusing player" : "ignored");
      if (req) return SL_RESULT_FEATURE_UNSUPPORTED;
      break;
    }
  }

  SLuint32 loc = *(const SLuint32 *)pAudioSrc->pLocator;

  if (loc == SL_DATALOCATOR_ANDROIDBUFFERQUEUE) {
    // the AAC decode-to-PCM trick. would need an ADTS decoder (SceAudiodec)
    const SLDataFormat_MIME *m = pAudioSrc->pFormat;
    unsigned n = __sync_add_and_fetch(&refused_decoders, 1);
    if (n == 1 || n == 10 || n == 100 || n % 1000 == 0)
      L("opensles: refusing decoder player #%u (ANDROIDBUFFERQUEUE src, mime '%s', container %u). "
        "no AAC decode yet, that voice stays silent",
        n, m && m->formatType == SL_DATAFORMAT_MIME && m->mimeType ? (const char *)m->mimeType : "?",
        m ? m->containerType : 0);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }

  if (loc != SL_DATALOCATOR_BUFFERQUEUE && loc != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE) {
    L("opensles: CreateAudioPlayer with source locator 0x%X, only buffer queues are done", loc);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }
  if (want_abq) {
    L_ONCE("opensles: PCM player with ANDROIDBUFFERQUEUESOURCE required, refusing");
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }

  uint32_t nbuf = ((const SLDataLocator_BufferQueue *)pAudioSrc->pLocator)->numBuffers;
  if (!nbuf) return SL_RESULT_PARAMETER_INVALID;
  if (nbuf > MAX_QUEUE) {
    L("opensles: player wants %u buffers, capping at %d", nbuf, MAX_QUEUE);
    nbuf = MAX_QUEUE;
  }

  const SLDataFormat_PCM *f = pAudioSrc->pFormat;
  if (!f) return SL_RESULT_PARAMETER_INVALID;
  if (f->formatType != SL_DATAFORMAT_PCM) {
    L("opensles: buffer queue player with format type %u, only PCM is done", f->formatType);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }
  if (f->numChannels < 1 || f->numChannels > 2 || (f->bitsPerSample != 8 && f->bitsPerSample != 16) ||
      (f->containerSize && f->containerSize != f->bitsPerSample) || f->endianness == SL_BYTEORDER_BIGENDIAN ||
      f->samplesPerSec < 1000 * 1000 || f->samplesPerSec > 192000 * 1000) {
    L("opensles: unsupported PCM: %u ch, %u mHz, %u bit (container %u), endian %u",
      f->numChannels, f->samplesPerSec, f->bitsPerSample, f->containerSize, f->endianness);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }

  SLuint32 sink = *(const SLuint32 *)pAudioSnk->pLocator;
  if (sink != SL_DATALOCATOR_OUTPUTMIX) {
    L("opensles: PCM player with sink locator 0x%X, only output mix is done", sink);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }

  sl_player *p = calloc(1, sizeof(*p) + nbuf * sizeof(sl_buf));
  if (!p) return SL_RESULT_MEMORY_FAILURE;
  obj_init(&p->obj, K_PLAYER);
  p->play = &play_vtbl;
  p->bq = &bq_vtbl;
  p->vol = &vol_vtbl;
  p->slot = -1;
  p->rate_mhz = f->samplesPerSec;
  p->channels = (int)f->numChannels;
  p->is8 = f->bitsPerSample == 8;
  p->frame_bytes = f->numChannels * (f->bitsPerSample / 8);
  p->step = (uint32_t)(((uint64_t)f->samplesPerSec << 16) / (OUT_RATE * 1000ull));
  p->play_state = SL_PLAYSTATE_STOPPED;
  p->q_cap = nbuf;
  reset_resampler(p);
  player_regain(p); // 0 mB, unmuted, centred

  int fmt_new = 0;
  lock();
  for (int s = 0; s < MAX_PLAYERS; s++) {
    if (!g_players[s]) {
      p->slot = s;
      g_players[s] = p;
      break;
    }
  }
  if (p->slot >= 0) fmt_new = fmt_is_new(f, nbuf);
  unlock();

  if (p->slot < 0) {
    L_ONCE("opensles: out of player slots (%d), refusing", MAX_PLAYERS);
    free(p);
    return SL_RESULT_MEMORY_FAILURE;
  }
  if (fmt_new)
    L("opensles: new PCM format: %u Hz, %u ch, %u bit, %u buffers",
      f->samplesPerSec / 1000, f->numChannels, f->bitsPerSample, nbuf);

  check_mixer();
  *pPlayer = (SLObjectItf)&p->obj.itf;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(SLEngineItf self, SLObjectItf *pMix, SLuint32 numInterfaces,
                                    const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
  (void)self;
  if (!pMix) return SL_RESULT_PARAMETER_INVALID;
  *pMix = NULL;
  if (numInterfaces && !pInterfaceIds) return SL_RESULT_PARAMETER_INVALID;
  // take whatever it asks for (env reverb etc), GetInterface says no later
  for (SLuint32 i = 0; i < numInterfaces; i++)
    log_iid(WHO_MIX_CREATE, iid_lookup(pInterfaceIds[i]), pInterfaceIds[i],
            pInterfaceRequired && pInterfaceRequired[i], "accepted, not implemented");

  sl_mix *m = calloc(1, sizeof(*m));
  if (!m) return SL_RESULT_MEMORY_FAILURE;
  obj_init(&m->obj, K_MIX);
  *pMix = (SLObjectItf)&m->obj.itf;
  L("opensles: output mix created");
  return SL_RESULT_SUCCESS;
}

static SLresult eng_QueryNumSupportedInterfaces(SLEngineItf self, SLuint32 objectID,
                                                SLuint32 *pNumSupportedInterfaces) {
  (void)self;
  if (!pNumSupportedInterfaces) return SL_RESULT_PARAMETER_INVALID;
  *pNumSupportedInterfaces = 0;
  L_ONCE("opensles: QueryNumSupportedInterfaces(0x%X)", objectID);
  switch (objectID) {
  case SL_OBJECTID_ENGINE: *pNumSupportedInterfaces = 1; return SL_RESULT_SUCCESS;
  case SL_OBJECTID_AUDIOPLAYER: *pNumSupportedInterfaces = 3; return SL_RESULT_SUCCESS;
  case SL_OBJECTID_OUTPUTMIX: return SL_RESULT_SUCCESS;
  // LISTENER lands here: count stays 0 so the game skips CreateListener
  default: return SL_RESULT_FEATURE_UNSUPPORTED;
  }
}

static SLresult eng_QuerySupportedInterfaces(SLEngineItf self, SLuint32 objectID, SLuint32 index,
                                             SLInterfaceID *pInterfaceId) {
  (void)self;
  static const SLInterfaceID *const player_itfs[] = { &SL_IID_PLAY, &SL_IID_BUFFERQUEUE, &SL_IID_VOLUME };
  if (!pInterfaceId) return SL_RESULT_PARAMETER_INVALID;
  *pInterfaceId = NULL;
  switch (objectID) {
  case SL_OBJECTID_ENGINE:
    if (index != 0) return SL_RESULT_PARAMETER_INVALID;
    *pInterfaceId = SL_IID_ENGINE;
    return SL_RESULT_SUCCESS;
  case SL_OBJECTID_AUDIOPLAYER:
    if (index >= 3) return SL_RESULT_PARAMETER_INVALID;
    *pInterfaceId = *player_itfs[index];
    return SL_RESULT_SUCCESS;
  case SL_OBJECTID_OUTPUTMIX:
    return SL_RESULT_PARAMETER_INVALID;
  default:
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
}

static SLresult eng_QueryNumSupportedExtensions(SLEngineItf self, SLuint32 *pNumExtensions) {
  (void)self;
  if (!pNumExtensions) return SL_RESULT_PARAMETER_INVALID;
  *pNumExtensions = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_QuerySupportedExtension(SLEngineItf self, SLuint32 index, SLchar *pExtensionName,
                                            SLint16 *pNameLength) {
  (void)self;
  (void)index;
  (void)pExtensionName;
  (void)pNameLength;
  return SL_RESULT_PARAMETER_INVALID; // there are none
}

static SLresult eng_IsExtensionSupported(SLEngineItf self, const SLchar *pExtensionName, SLboolean *pSupported) {
  (void)self;
  if (!pExtensionName || !pSupported) return SL_RESULT_PARAMETER_INVALID;
  *pSupported = SL_BOOLEAN_FALSE;
  return SL_RESULT_SUCCESS;
}

// creators we don't do: null the out pointer, say so once, return 12
static SLresult eng_CreateLEDDevice(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID,
                                    SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
                                    const SLboolean *pInterfaceRequired) {
  (void)self; (void)deviceID; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  if (pDevice) *pDevice = NULL;
  L_ONCE("opensles: stub CreateLEDDevice");
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_CreateVibraDevice(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID,
                                      SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
                                      const SLboolean *pInterfaceRequired) {
  (void)self; (void)deviceID; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  if (pDevice) *pDevice = NULL;
  L_ONCE("opensles: stub CreateVibraDevice");
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_CreateAudioRecorder(SLEngineItf self, SLObjectItf *pRecorder, SLDataSource *pAudioSrc,
                                        SLDataSink *pAudioSnk, SLuint32 numInterfaces,
                                        const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
  (void)self; (void)pAudioSrc; (void)pAudioSnk; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  if (pRecorder) *pRecorder = NULL;
  L_ONCE("opensles: stub CreateAudioRecorder");
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_CreateMidiPlayer(SLEngineItf self, SLObjectItf *pPlayer, SLDataSource *pMIDISrc,
                                     SLDataSource *pBankSrc, SLDataSink *pAudioOutput, SLDataSink *pVibra,
                                     SLDataSink *pLEDArray, SLuint32 numInterfaces,
                                     const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
  (void)self; (void)pMIDISrc; (void)pBankSrc; (void)pAudioOutput; (void)pVibra; (void)pLEDArray;
  (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  if (pPlayer) *pPlayer = NULL;
  L_ONCE("opensles: stub CreateMidiPlayer");
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_CreateListener(SLEngineItf self, SLObjectItf *pListener, SLuint32 numInterfaces,
                                   const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
  (void)self; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  if (pListener) *pListener = NULL;
  L_ONCE("opensles: stub CreateListener");
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_Create3DGroup(SLEngineItf self, SLObjectItf *pGroup, SLuint32 numInterfaces,
                                  const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
  (void)self; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  if (pGroup) *pGroup = NULL;
  L_ONCE("opensles: stub Create3DGroup");
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_CreateMetadataExtractor(SLEngineItf self, SLObjectItf *pMetadataExtractor,
                                            SLDataSource *pDataSource, SLuint32 numInterfaces,
                                            const SLInterfaceID *pInterfaceIds,
                                            const SLboolean *pInterfaceRequired) {
  (void)self; (void)pDataSource; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  if (pMetadataExtractor) *pMetadataExtractor = NULL;
  L_ONCE("opensles: stub CreateMetadataExtractor");
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_CreateExtensionObject(SLEngineItf self, SLObjectItf *pObject, void *pParameters,
                                          SLuint32 objectID, SLuint32 numInterfaces,
                                          const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
  (void)self; (void)pParameters; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  if (pObject) *pObject = NULL;
  L_ONCE("opensles: stub CreateExtensionObject(0x%X)", objectID);
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const struct SLEngineItf_ engine_vtbl = {
  eng_CreateLEDDevice,
  eng_CreateVibraDevice,
  eng_CreateAudioPlayer,
  eng_CreateAudioRecorder,
  eng_CreateMidiPlayer,
  eng_CreateListener,
  eng_Create3DGroup,
  eng_CreateOutputMix,
  eng_CreateMetadataExtractor,
  eng_CreateExtensionObject,
  eng_QueryNumSupportedInterfaces,
  eng_QuerySupportedInterfaces,
  eng_QueryNumSupportedExtensions,
  eng_QuerySupportedExtension,
  eng_IsExtensionSupported,
};

// ---------------------------------------------------------------------------
// entry point

SLresult slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions, const SLEngineOption *pEngineOptions,
                        SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
                        const SLboolean *pInterfaceRequired) {
  if (!pEngine) return SL_RESULT_PARAMETER_INVALID;
  *pEngine = NULL;
  if ((numOptions && !pEngineOptions) || (numInterfaces && !pInterfaceIds)) return SL_RESULT_PARAMETER_INVALID;

  pthread_once(&g_once, sl_init_once);

  for (SLuint32 i = 0; i < numOptions; i++)
    L("opensles: engine option 0x%X = %u (ignored)", pEngineOptions[i].feature, pEngineOptions[i].data);

  for (SLuint32 i = 0; i < numInterfaces; i++) {
    int ix = iid_lookup(pInterfaceIds[i]);
    int req = pInterfaceRequired && pInterfaceRequired[i];
    int ok = ix == IX_ENGINE;
    log_iid(WHO_SLCREATE, ix, pInterfaceIds[i], req, ok ? "ok" : req ? "unsupported, failing" : "ignored");
    if (!ok && req) return SL_RESULT_FEATURE_UNSUPPORTED;
  }

  sl_engine *e = calloc(1, sizeof(*e));
  if (!e) return SL_RESULT_MEMORY_FAILURE;
  obj_init(&e->obj, K_ENGINE);
  e->eng = &engine_vtbl;
  *pEngine = (SLObjectItf)&e->obj.itf;
  L("opensles: engine created");
  return SL_RESULT_SUCCESS;
}

EXPORT_TABLE(opensles) = {
  EXP(slCreateEngine),
  EXP(SL_IID_ENGINE),
  EXP(SL_IID_PLAY),
  EXP(SL_IID_BUFFERQUEUE),
  EXP(SL_IID_ANDROIDBUFFERQUEUESOURCE),
  EXP(SL_IID_VOLUME),
  EXP(SL_IID_3DLOCATION),
  EXP(SL_IID_3DSOURCE),
  EXP(SL_IID_ENVIRONMENTALREVERB),
};
EXPORT_TABLE_END(opensles);
