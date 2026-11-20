// fake JNI env + VM, written from scratch. slot numbers follow jni.h's
// JNINativeInterface order (4 reserved, then GetVersion at 4, etc).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android/jni.h"
#include "game/patches.h"
#include "loader/so_loader.h"
#include "util/abi.h"
#include "util/log.h"
#include "util/paths.h"

typedef union {
  uint8_t z; int8_t b; uint16_t c; int16_t s;
  int32_t i; int64_t j; float f; double d; void *l;
} jvalue;

enum { K_CLASS = 1, K_OBJECT, K_STRING, K_ARRAY };
#define JOBJ_MAGIC 0x4A4F424A

typedef struct jobj {
  uint32_t magic;
  int kind;
  const char *cls;  // class name, e.g. "java/io/File"
  char *str;        // strings, and the path of File objects
  int len;          // string length / array length
  int elem;         // array element size
  void *data;       // array data / direct buffer
} jobj;

typedef struct jmeth jmeth;
typedef jvalue (*jhandler)(jobj *self, const jvalue *args, jmeth *m);

struct jmeth {
  const char *cls;
  char *name, *sig;
  int is_static;
  jhandler fn;
  int calls;
};

typedef struct {
  const char *cls;
  char *name, *sig;
  int is_static;
} jfield;

#define ACTIVITY_CLASS "com/t2ksports/nba2k16android/gameactivity"

static int is_jobj(const void *p) { return p && ((const jobj *)p)->magic == JOBJ_MAGIC; }

static jobj *new_obj(int kind, const char *cls) {
  jobj *o = calloc(1, sizeof(*o));
  o->magic = JOBJ_MAGIC;
  o->kind = kind;
  o->cls = cls;
  return o;
}

static jobj *new_string(const char *s) {
  jobj *o = new_obj(K_STRING, "java/lang/String");
  o->str = strdup(s ? s : "");
  o->len = strlen(o->str);
  return o;
}

static jobj *new_file(const char *path) {
  jobj *o = new_obj(K_OBJECT, "java/io/File");
  o->str = strdup(path);
  return o;
}

// --- classes, interned by name so the same jclass comes back every time ---
#define MAX_CLASSES 256
static jobj *classes[MAX_CLASSES];
static int n_classes;

static jobj *find_class(const char *name) {
  for (int i = 0; i < n_classes; i++)
    if (!strcmp(classes[i]->cls, name)) return classes[i];
  if (n_classes >= MAX_CLASSES) fatal("jni: too many classes");
  jobj *c = new_obj(K_CLASS, strdup(name));
  classes[n_classes++] = c;
  return c;
}

static jobj *activity_obj;

static const char *class_of(const void *p) {
  if (is_jobj(p)) return ((const jobj *)p)->cls;
  return "?";
}

// --- handlers for java methods the game calls ---
static jvalue h_null(jobj *self, const jvalue *a, jmeth *m) { (void)self; (void)a; (void)m; return (jvalue){ .l = NULL }; }
static jvalue h_package(jobj *self, const jvalue *a, jmeth *m) { (void)self; (void)a; (void)m; return (jvalue){ .l = new_string(PACKAGE_NAME) }; }
static jvalue h_files_dir(jobj *self, const jvalue *a, jmeth *m) { (void)self; (void)a; (void)m; return (jvalue){ .l = new_file(FILES_DIR) }; }
static jvalue h_cache_dir(jobj *self, const jvalue *a, jmeth *m) { (void)self; (void)a; (void)m; return (jvalue){ .l = new_file(CACHE_DIR) }; }
static jvalue h_obb_dir(jobj *self, const jvalue *a, jmeth *m) { (void)self; (void)a; (void)m; return (jvalue){ .l = new_file(OBB_DIR) }; }
static jvalue h_ext_dir(jobj *self, const jvalue *a, jmeth *m) { (void)self; (void)a; (void)m; return (jvalue){ .l = new_file(DATA_PATH) }; }
static jvalue h_path(jobj *self, const jvalue *a, jmeth *m) {
  (void)a; (void)m;
  return (jvalue){ .l = new_string(is_jobj(self) && self->str ? self->str : "") };
}
static jvalue h_self(jobj *self, const jvalue *a, jmeth *m) { (void)a; (void)m; return (jvalue){ .l = self }; }
static jvalue h_true(jobj *self, const jvalue *a, jmeth *m) { (void)self; (void)a; (void)m; return (jvalue){ .z = 1 }; }
static jobj *find_class(const char *name);
static jvalue h_class_loader(jobj *self, const jvalue *a, jmeth *m) {
  (void)self; (void)a; (void)m;
  static jobj *loader;
  if (!loader) loader = new_obj(K_OBJECT, "java/lang/ClassLoader");
  return (jvalue){ .l = loader };
}
// the obb file is named after this (main.<code>.<package>.obb), v0.0.29 is 29
static jvalue h_version_code(jobj *self, const jvalue *a, jmeth *m) {
  (void)self; (void)a; (void)m;
  return (jvalue){ .i = config_int("version_code", GAME_VERSION_CODE) };
}
static jvalue h_version_name(jobj *self, const jvalue *a, jmeth *m) {
  (void)self; (void)a; (void)m;
  return (jvalue){ .l = new_string(GAME_VERSION_NAME) };
}
// loadClass("com.foo.Bar") -> the jclass for com/foo/Bar
static jvalue h_load_class(jobj *self, const jvalue *a, jmeth *m) {
  (void)self; (void)m;
  const jobj *s = a[0].l;
  char name[256];
  snprintf(name, sizeof(name), "%s", is_jobj(s) && s->str ? s->str : "?");
  for (char *p = name; *p; p++) if (*p == '.') *p = '/';
  return (jvalue){ .l = find_class(name) };
}

static const struct { const char *name; const char *sig; jhandler fn; } handlers[] = {
  { "getPackageName", NULL, h_package },
  { "getFilesDir", NULL, h_files_dir },
  { "getCacheDir", NULL, h_cache_dir },
  { "getExternalCacheDir", NULL, h_cache_dir },
  { "getObbDir", NULL, h_obb_dir },
  { "getExternalFilesDir", NULL, h_ext_dir },
  { "getExternalStorageDirectory", NULL, h_ext_dir },
  { "getAbsolutePath", NULL, h_path },
  { "getPath", NULL, h_path },
  { "toString", "()Ljava/lang/String;", h_path },
  { "exists", "()Z", h_true },
  { "getApplicationContext", NULL, h_self },
  { "getContext", NULL, h_self },
  { "getClassLoader", NULL, h_class_loader },
  { "loadClass", NULL, h_load_class },
  // android 6 runtime permission dance, we always have them
  { "isPermissionGranted", "()Z", h_true },
  { "isPermissionReceived", "()Z", h_true },
  { "getVersionCode", "()I", h_version_code },
  { "getVersionName", NULL, h_version_name },
};

static jhandler pick_handler(const char *name, const char *sig) {
  for (unsigned i = 0; i < ARRAY_LEN(handlers); i++)
    if (!strcmp(handlers[i].name, name) && (!handlers[i].sig || !strcmp(handlers[i].sig, sig)))
      return handlers[i].fn;
  return NULL;
}

// --- method / field ids, interned ---
#define MAX_METHODS 1024
static jmeth *methods[MAX_METHODS];
static int n_methods;

static jmeth *get_method(jobj *cls, const char *name, const char *sig, int is_static) {
  const char *cn = is_jobj(cls) ? cls->cls : "?";
  for (int i = 0; i < n_methods; i++) {
    jmeth *m = methods[i];
    if (m->is_static == is_static && !strcmp(m->cls, cn) && !strcmp(m->name, name) && !strcmp(m->sig, sig))
      return m;
  }
  if (n_methods >= MAX_METHODS) fatal("jni: too many methods");
  jmeth *m = calloc(1, sizeof(*m));
  m->cls = cn;
  m->name = strdup(name);
  m->sig = strdup(sig);
  m->is_static = is_static;
  m->fn = pick_handler(name, sig);
  methods[n_methods++] = m;
  L("JNI: Get%sMethodID %s.%s %s%s", is_static ? "Static" : "", cn, name, sig, m->fn ? "" : " (no handler)");
  return m;
}

static jfield *get_field(jobj *cls, const char *name, const char *sig, int is_static) {
  jfield *f = calloc(1, sizeof(*f));
  f->cls = is_jobj(cls) ? cls->cls : "?";
  f->name = strdup(name);
  f->sig = strdup(sig);
  f->is_static = is_static;
  L("JNI: Get%sFieldID %s.%s %s", is_static ? "Static" : "", f->cls, name, sig);
  return f;
}

// --- calling ---
// walks a signature like "(ILjava/lang/String;[BF)V" one param at a time
static const char *next_param(const char *p, char *type) {
  *type = *p;
  if (*p == ')' || !*p) return p;
  while (*p == '[') p++;
  if (*p == 'L') p = strchr(p, ';');
  if (*type == '[') *type = 'L';  // arrays are just object refs
  return p + 1;
}

#define MAX_ARGS 16

static jvalue invoke(jobj *self, jmeth *m, const jvalue *args) {
  // first few calls, then every power of two so polling loops still show up
  int n = ++m->calls;
  if (n <= 4 || !(n & (n - 1)))
    L("JNI: call #%d %s.%s %s on %s", n, m->cls, m->name, m->sig, self ? class_of(self) : "static");
  if (m->fn) return m->fn(self, args, m);
  return (jvalue){ .j = 0 };
}

static jvalue invoke_v(jobj *self, jmeth *m, va_list ap) {
  jvalue args[MAX_ARGS] = { 0 };
  const char *p = strchr(m->sig, '(');
  p = p ? p + 1 : "";
  for (int i = 0; i < MAX_ARGS; i++) {
    char t;
    p = next_param(p, &t);
    if (t == ')' || !t) break;
    switch (t) {
      case 'J': args[i].j = va_arg(ap, int64_t); break;
      case 'F': args[i].f = (float)va_arg(ap, double); break;
      case 'D': args[i].d = va_arg(ap, double); break;
      case 'L': args[i].l = va_arg(ap, void *); break;
      default:  args[i].i = va_arg(ap, int32_t); break;
    }
  }
  return invoke(self, m, args);
}

// the game is softfp. everything it calls through the table gets SOFTFP,
// harmless for the int-only ones and required for the float ones.
#define J SOFTFP static

#define CALL_FAMILY(N, T, fld)                                                                    \
  J T Call##N##Method(void *e, jobj *o, jmeth *m, ...) {                                          \
    (void)e; va_list ap; va_start(ap, m); jvalue r = invoke_v(o, m, ap); va_end(ap); return r.fld; \
  }                                                                                               \
  J T Call##N##MethodV(void *e, jobj *o, jmeth *m, va_list ap) { (void)e; return invoke_v(o, m, ap).fld; } \
  J T Call##N##MethodA(void *e, jobj *o, jmeth *m, const jvalue *a) { (void)e; return invoke(o, m, a).fld; } \
  J T CallNonvirtual##N##Method(void *e, jobj *o, jobj *c, jmeth *m, ...) {                        \
    (void)e; (void)c; va_list ap; va_start(ap, m); jvalue r = invoke_v(o, m, ap); va_end(ap); return r.fld; \
  }                                                                                               \
  J T CallNonvirtual##N##MethodV(void *e, jobj *o, jobj *c, jmeth *m, va_list ap) { (void)e; (void)c; return invoke_v(o, m, ap).fld; } \
  J T CallNonvirtual##N##MethodA(void *e, jobj *o, jobj *c, jmeth *m, const jvalue *a) { (void)e; (void)c; return invoke(o, m, a).fld; } \
  J T CallStatic##N##Method(void *e, jobj *c, jmeth *m, ...) {                                    \
    (void)e; (void)c; va_list ap; va_start(ap, m); jvalue r = invoke_v(NULL, m, ap); va_end(ap); return r.fld; \
  }                                                                                               \
  J T CallStatic##N##MethodV(void *e, jobj *c, jmeth *m, va_list ap) { (void)e; (void)c; return invoke_v(NULL, m, ap).fld; } \
  J T CallStatic##N##MethodA(void *e, jobj *c, jmeth *m, const jvalue *a) { (void)e; (void)c; return invoke(NULL, m, a).fld; }

CALL_FAMILY(Object, void *, l)
CALL_FAMILY(Boolean, uint8_t, z)
CALL_FAMILY(Byte, int8_t, b)
CALL_FAMILY(Char, uint16_t, c)
CALL_FAMILY(Short, int16_t, s)
CALL_FAMILY(Int, int32_t, i)
CALL_FAMILY(Long, int64_t, j)
CALL_FAMILY(Float, float, f)
CALL_FAMILY(Double, double, d)

J void CallVoidMethod(void *e, jobj *o, jmeth *m, ...) { (void)e; va_list ap; va_start(ap, m); invoke_v(o, m, ap); va_end(ap); }
J void CallVoidMethodV(void *e, jobj *o, jmeth *m, va_list ap) { (void)e; invoke_v(o, m, ap); }
J void CallVoidMethodA(void *e, jobj *o, jmeth *m, const jvalue *a) { (void)e; invoke(o, m, a); }
J void CallNonvirtualVoidMethod(void *e, jobj *o, jobj *c, jmeth *m, ...) { (void)e; (void)c; va_list ap; va_start(ap, m); invoke_v(o, m, ap); va_end(ap); }
J void CallNonvirtualVoidMethodV(void *e, jobj *o, jobj *c, jmeth *m, va_list ap) { (void)e; (void)c; invoke_v(o, m, ap); }
J void CallNonvirtualVoidMethodA(void *e, jobj *o, jobj *c, jmeth *m, const jvalue *a) { (void)e; (void)c; invoke(o, m, a); }
J void CallStaticVoidMethod(void *e, jobj *c, jmeth *m, ...) { (void)e; (void)c; va_list ap; va_start(ap, m); invoke_v(NULL, m, ap); va_end(ap); }
J void CallStaticVoidMethodV(void *e, jobj *c, jmeth *m, va_list ap) { (void)e; (void)c; invoke_v(NULL, m, ap); }
J void CallStaticVoidMethodA(void *e, jobj *c, jmeth *m, const jvalue *a) { (void)e; (void)c; invoke(NULL, m, a); }

// --- fields: nothing stored yet, just log + zero ---
static void field_log(const char *what, jfield *f) {
  L_ONCE("JNI: field access (first one shown)");
  L("JNI: %s %s.%s %s -> 0", what, f ? f->cls : "?", f ? f->name : "?", f ? f->sig : "?");
}

#define FIELD_FAMILY(N, T)                                                                        \
  J T Get##N##Field(void *e, jobj *o, jfield *f) { (void)e; (void)o; field_log("Get" #N "Field", f); return (T)0; } \
  J void Set##N##Field(void *e, jobj *o, jfield *f, T v) { (void)e; (void)o; (void)v; field_log("Set" #N "Field", f); } \
  J T GetStatic##N##Field(void *e, jobj *c, jfield *f) { (void)e; (void)c; field_log("GetStatic" #N "Field", f); return (T)0; } \
  J void SetStatic##N##Field(void *e, jobj *c, jfield *f, T v) { (void)e; (void)c; (void)v; field_log("SetStatic" #N "Field", f); }

FIELD_FAMILY(Object, void *)
FIELD_FAMILY(Boolean, uint8_t)
FIELD_FAMILY(Byte, int8_t)
FIELD_FAMILY(Char, uint16_t)
FIELD_FAMILY(Short, int16_t)
FIELD_FAMILY(Int, int32_t)
FIELD_FAMILY(Long, int64_t)
FIELD_FAMILY(Float, float)
FIELD_FAMILY(Double, double)

// --- arrays ---
static jobj *new_array(int len, int elem) {
  jobj *a = new_obj(K_ARRAY, "[");
  a->len = len;
  a->elem = elem;
  a->data = calloc(len ? len : 1, elem);
  return a;
}

#define ARRAY_FAMILY(N, T)                                                                        \
  J void *New##N##Array(void *e, int len) { (void)e; return new_array(len, sizeof(T)); }         \
  J void *Get##N##ArrayElements(void *e, jobj *a, uint8_t *copy) { (void)e; if (copy) *copy = 0; return is_jobj(a) ? a->data : NULL; } \
  J void Release##N##ArrayElements(void *e, jobj *a, void *p, int mode) { (void)e; (void)a; (void)p; (void)mode; } \
  J void Get##N##ArrayRegion(void *e, jobj *a, int s, int n, T *buf) { (void)e; if (is_jobj(a)) memcpy(buf, (T *)a->data + s, n * sizeof(T)); } \
  J void Set##N##ArrayRegion(void *e, jobj *a, int s, int n, const T *buf) { (void)e; if (is_jobj(a)) memcpy((T *)a->data + s, buf, n * sizeof(T)); }

ARRAY_FAMILY(Boolean, uint8_t)
ARRAY_FAMILY(Byte, int8_t)
ARRAY_FAMILY(Char, uint16_t)
ARRAY_FAMILY(Short, int16_t)
ARRAY_FAMILY(Int, int32_t)
ARRAY_FAMILY(Long, int64_t)
ARRAY_FAMILY(Float, float)
ARRAY_FAMILY(Double, double)

J int GetArrayLength(void *e, jobj *a) { (void)e; return is_jobj(a) ? a->len : 0; }
J void *NewObjectArray(void *e, int len, jobj *cls, void *init) {
  (void)e; (void)cls;
  jobj *a = new_array(len, sizeof(void *));
  for (int i = 0; i < len; i++) ((void **)a->data)[i] = init;
  return a;
}
J void *GetObjectArrayElement(void *e, jobj *a, int i) {
  (void)e;
  return is_jobj(a) && i < a->len ? ((void **)a->data)[i] : NULL;
}
J void SetObjectArrayElement(void *e, jobj *a, int i, void *v) {
  (void)e;
  if (is_jobj(a) && i < a->len) ((void **)a->data)[i] = v;
}
J void *GetPrimitiveArrayCritical(void *e, jobj *a, uint8_t *copy) { (void)e; if (copy) *copy = 0; return is_jobj(a) ? a->data : NULL; }
J void ReleasePrimitiveArrayCritical(void *e, jobj *a, void *p, int mode) { (void)e; (void)a; (void)p; (void)mode; }

// --- strings ---
J void *NewStringUTF(void *e, const char *s) { (void)e; return new_string(s); }
J const char *GetStringUTFChars(void *e, jobj *s, uint8_t *copy) {
  (void)e;
  if (copy) *copy = 0;
  return is_jobj(s) && s->str ? s->str : "";
}
J void ReleaseStringUTFChars(void *e, jobj *s, const char *c) { (void)e; (void)s; (void)c; }
J int GetStringUTFLength(void *e, jobj *s) { (void)e; return is_jobj(s) && s->str ? (int)strlen(s->str) : 0; }
J int GetStringLength(void *e, jobj *s) { return GetStringUTFLength(e, s); }
J void *NewString(void *e, const uint16_t *u, int len) {
  (void)e;
  char *tmp = malloc(len + 1);
  for (int i = 0; i < len; i++) tmp[i] = u[i] < 0x80 ? (char)u[i] : '?';
  tmp[len] = 0;
  jobj *s = new_string(tmp);
  free(tmp);
  return s;
}
J const uint16_t *GetStringChars(void *e, jobj *s, uint8_t *copy) {
  (void)e;
  if (copy) *copy = 1;
  const char *c = is_jobj(s) && s->str ? s->str : "";
  int n = strlen(c);
  uint16_t *u = malloc((n + 1) * 2);
  for (int i = 0; i <= n; i++) u[i] = (uint8_t)c[i];
  return u;
}
J void ReleaseStringChars(void *e, jobj *s, const uint16_t *u) { (void)e; (void)s; free((void *)u); }
J void GetStringRegion(void *e, jobj *s, int st, int n, uint16_t *buf) {
  (void)e;
  const char *c = is_jobj(s) && s->str ? s->str : "";
  for (int i = 0; i < n; i++) buf[i] = (uint8_t)c[st + i];
}
J void GetStringUTFRegion(void *e, jobj *s, int st, int n, char *buf) {
  (void)e;
  const char *c = is_jobj(s) && s->str ? s->str : "";
  memcpy(buf, c + st, n);
  buf[n] = 0;
}
J const uint16_t *GetStringCritical(void *e, jobj *s, uint8_t *copy) { return GetStringChars(e, s, copy); }
J void ReleaseStringCritical(void *e, jobj *s, const uint16_t *u) { ReleaseStringChars(e, s, u); }

// --- classes / objects ---
J int GetVersion(void *e) { (void)e; return 0x00010006; }
J void *DefineClass(void *e, const char *n, void *l, const void *b, int len) { (void)e; (void)l; (void)b; (void)len; L("JNI: DefineClass(%s)", n); return NULL; }
J void *FindClass(void *e, const char *name) { (void)e; L("JNI: FindClass %s", name); return find_class(name); }
J void *GetSuperclass(void *e, jobj *c) { (void)e; (void)c; return NULL; }
J uint8_t IsAssignableFrom(void *e, jobj *a, jobj *b) { (void)e; (void)a; (void)b; return 1; }
J void *GetObjectClass(void *e, jobj *o) {
  (void)e;
  if (is_jobj(o)) return find_class(o->cls);
  return find_class(ACTIVITY_CLASS);
}
J uint8_t IsInstanceOf(void *e, jobj *o, jobj *c) { (void)e; (void)o; (void)c; return 1; }
J uint8_t IsSameObject(void *e, void *a, void *b) { (void)e; return a == b; }
J void *AllocObject(void *e, jobj *c) { (void)e; return new_obj(K_OBJECT, is_jobj(c) ? c->cls : "?"); }
J void *NewObject(void *e, jobj *c, jmeth *m, ...) {
  (void)e;
  L("JNI: NewObject %s %s", is_jobj(c) ? c->cls : "?", m ? m->sig : "?");
  return new_obj(K_OBJECT, is_jobj(c) ? c->cls : "?");
}
J void *NewObjectV(void *e, jobj *c, jmeth *m, va_list ap) { (void)ap; return NewObject(e, c, m); }
J void *NewObjectA(void *e, jobj *c, jmeth *m, const jvalue *a) { (void)a; return NewObject(e, c, m); }
J void *GetMethodID(void *e, jobj *c, const char *n, const char *s) { (void)e; return get_method(c, n, s, 0); }
J void *GetStaticMethodID(void *e, jobj *c, const char *n, const char *s) { (void)e; return get_method(c, n, s, 1); }
J void *GetFieldID(void *e, jobj *c, const char *n, const char *s) { (void)e; return get_field(c, n, s, 0); }
J void *GetStaticFieldID(void *e, jobj *c, const char *n, const char *s) { (void)e; return get_field(c, n, s, 1); }

// --- refs, exceptions, misc ---
J void *NewRef(void *e, void *o) { (void)e; return o; }
J void DeleteRef(void *e, void *o) { (void)e; (void)o; }
J int PushLocalFrame(void *e, int cap) { (void)e; (void)cap; return 0; }
J void *PopLocalFrame(void *e, void *res) { (void)e; return res; }
J int EnsureLocalCapacity(void *e, int cap) { (void)e; (void)cap; return 0; }
J int GetObjectRefType(void *e, void *o) { (void)e; (void)o; return 1; }
J int Throw(void *e, void *t) { (void)e; (void)t; L("JNI: Throw"); return 0; }
J int ThrowNew(void *e, jobj *c, const char *msg) { (void)e; L("JNI: ThrowNew %s: %s", class_of(c), msg); return 0; }
J void *ExceptionOccurred(void *e) { (void)e; return NULL; }
J void ExceptionDescribe(void *e) { (void)e; }
J void ExceptionClear(void *e) { (void)e; }
J uint8_t ExceptionCheck(void *e) { (void)e; return 0; }
J void FatalError(void *e, const char *msg) { (void)e; fatal("JNI FatalError: %s", msg); }
J int MonitorEnter(void *e, void *o) { (void)e; (void)o; return 0; }
J int MonitorExit(void *e, void *o) { (void)e; (void)o; return 0; }
J void *FromReflected(void *e, void *o) { (void)e; (void)o; return NULL; }
J void *ToReflected(void *e, void *c, void *id, uint8_t st) { (void)e; (void)c; (void)id; (void)st; return NULL; }

typedef struct { const char *name, *sig; void *fn; } native_method;
#define MAX_NATIVES 256
static native_method natives[MAX_NATIVES];
static int n_natives;

J int RegisterNatives(void *e, jobj *c, const native_method *m, int n) {
  (void)e;
  for (int i = 0; i < n && n_natives < MAX_NATIVES; i++) {
    natives[n_natives++] = m[i];
    L("JNI: RegisterNatives %s.%s %s -> %p", class_of(c), m[i].name, m[i].sig, m[i].fn);
  }
  return 0;
}
J int UnregisterNatives(void *e, jobj *c) { (void)e; (void)c; return 0; }

static void *vm_ptr;
J int GetJavaVM(void *e, void **vm) { (void)e; *vm = vm_ptr; return 0; }

J void *NewDirectByteBuffer(void *e, void *addr, int64_t cap) {
  (void)e;
  jobj *b = new_obj(K_OBJECT, "java/nio/ByteBuffer");
  b->data = addr;
  b->len = (int)cap;
  return b;
}
J void *GetDirectBufferAddress(void *e, jobj *b) { (void)e; return is_jobj(b) ? b->data : NULL; }
J int64_t GetDirectBufferCapacity(void *e, jobj *b) { (void)e; return is_jobj(b) ? b->len : -1; }

// --- traps for every slot we didn't fill, numbered so the log says which ---
#define T(n) J int jni_trap_##n(void) { L("JNI: unimplemented env slot %d called", n); return 0; }
#define T10(p) T(p##0) T(p##1) T(p##2) T(p##3) T(p##4) T(p##5) T(p##6) T(p##7) T(p##8) T(p##9)
T(0) T(1) T(2) T(3) T(4) T(5) T(6) T(7) T(8) T(9)
T10(1) T10(2) T10(3) T10(4) T10(5) T10(6) T10(7) T10(8) T10(9) T10(10) T10(11)
T10(12) T10(13) T10(14) T10(15) T10(16) T10(17) T10(18) T10(19) T10(20) T10(21) T10(22)
T(230) T(231) T(232)
#undef T
#define T(n) [n] = (void *)jni_trap_##n,
static void *env_table[233] = {
  T(0) T(1) T(2) T(3) T(4) T(5) T(6) T(7) T(8) T(9)
  T10(1) T10(2) T10(3) T10(4) T10(5) T10(6) T10(7) T10(8) T10(9) T10(10) T10(11)
  T10(12) T10(13) T10(14) T10(15) T10(16) T10(17) T10(18) T10(19) T10(20) T10(21) T10(22)
  T(230) T(231) T(232)
};
#undef T
#undef T10

static void *env_ptr = env_table;  // JNIEnv is a pointer to the table

// --- JavaVM ---
J int vm_DestroyJavaVM(void *vm) { (void)vm; return 0; }
J int vm_AttachCurrentThread(void *vm, void **env, void *args) { (void)vm; (void)args; *env = &env_ptr; return 0; }
J int vm_DetachCurrentThread(void *vm) { (void)vm; return 0; }
J int vm_GetEnv(void *vm, void **env, int ver) { (void)vm; (void)ver; *env = &env_ptr; return 0; }

static void *vm_table[8] = {
  NULL, NULL, NULL,
  (void *)vm_DestroyJavaVM,
  (void *)vm_AttachCurrentThread,
  (void *)vm_DetachCurrentThread,
  (void *)vm_GetEnv,
  (void *)vm_AttachCurrentThread,  // AttachCurrentThreadAsDaemon
};
static void *vm_obj = vm_table;

void *jni_vm(void) { return &vm_obj; }
void *jni_env(void) { return &env_ptr; }
void *jni_activity_object(void) { return activity_obj; }

void *jni_find_native(const char *name) {
  for (int i = 0; i < n_natives; i++)
    if (!strcmp(natives[i].name, name)) return natives[i].fn;
  return NULL;
}

#define SET(i, f) env_table[i] = (void *)(f)
#define SET3(i, base) SET(i, base); SET(i + 1, base##V); SET(i + 2, base##A)

void jni_init(void) {
  vm_ptr = &vm_obj;
  activity_obj = new_obj(K_OBJECT, ACTIVITY_CLASS);

  SET(4, GetVersion); SET(5, DefineClass); SET(6, FindClass);
  SET(7, FromReflected); SET(8, FromReflected); SET(9, ToReflected);
  SET(10, GetSuperclass); SET(11, IsAssignableFrom); SET(12, ToReflected);
  SET(13, Throw); SET(14, ThrowNew); SET(15, ExceptionOccurred);
  SET(16, ExceptionDescribe); SET(17, ExceptionClear); SET(18, FatalError);
  SET(19, PushLocalFrame); SET(20, PopLocalFrame);
  SET(21, NewRef); SET(22, DeleteRef); SET(23, DeleteRef);
  SET(24, IsSameObject); SET(25, NewRef); SET(26, EnsureLocalCapacity);
  SET(27, AllocObject); SET3(28, NewObject);
  SET(31, GetObjectClass); SET(32, IsInstanceOf); SET(33, GetMethodID);

  SET3(34, CallObjectMethod);  SET3(37, CallBooleanMethod); SET3(40, CallByteMethod);
  SET3(43, CallCharMethod);    SET3(46, CallShortMethod);   SET3(49, CallIntMethod);
  SET3(52, CallLongMethod);    SET3(55, CallFloatMethod);   SET3(58, CallDoubleMethod);
  SET3(61, CallVoidMethod);

  SET3(64, CallNonvirtualObjectMethod);  SET3(67, CallNonvirtualBooleanMethod);
  SET3(70, CallNonvirtualByteMethod);    SET3(73, CallNonvirtualCharMethod);
  SET3(76, CallNonvirtualShortMethod);   SET3(79, CallNonvirtualIntMethod);
  SET3(82, CallNonvirtualLongMethod);    SET3(85, CallNonvirtualFloatMethod);
  SET3(88, CallNonvirtualDoubleMethod);  SET3(91, CallNonvirtualVoidMethod);

  SET(94, GetFieldID);
  SET(95, GetObjectField);  SET(96, GetBooleanField); SET(97, GetByteField);
  SET(98, GetCharField);    SET(99, GetShortField);   SET(100, GetIntField);
  SET(101, GetLongField);   SET(102, GetFloatField);  SET(103, GetDoubleField);
  SET(104, SetObjectField); SET(105, SetBooleanField); SET(106, SetByteField);
  SET(107, SetCharField);   SET(108, SetShortField);  SET(109, SetIntField);
  SET(110, SetLongField);   SET(111, SetFloatField);  SET(112, SetDoubleField);

  SET(113, GetStaticMethodID);
  SET3(114, CallStaticObjectMethod);  SET3(117, CallStaticBooleanMethod);
  SET3(120, CallStaticByteMethod);    SET3(123, CallStaticCharMethod);
  SET3(126, CallStaticShortMethod);   SET3(129, CallStaticIntMethod);
  SET3(132, CallStaticLongMethod);    SET3(135, CallStaticFloatMethod);
  SET3(138, CallStaticDoubleMethod);  SET3(141, CallStaticVoidMethod);

  SET(144, GetStaticFieldID);
  SET(145, GetStaticObjectField);  SET(146, GetStaticBooleanField); SET(147, GetStaticByteField);
  SET(148, GetStaticCharField);    SET(149, GetStaticShortField);   SET(150, GetStaticIntField);
  SET(151, GetStaticLongField);    SET(152, GetStaticFloatField);   SET(153, GetStaticDoubleField);
  SET(154, SetStaticObjectField);  SET(155, SetStaticBooleanField); SET(156, SetStaticByteField);
  SET(157, SetStaticCharField);    SET(158, SetStaticShortField);   SET(159, SetStaticIntField);
  SET(160, SetStaticLongField);    SET(161, SetStaticFloatField);   SET(162, SetStaticDoubleField);

  SET(163, NewString); SET(164, GetStringLength); SET(165, GetStringChars); SET(166, ReleaseStringChars);
  SET(167, NewStringUTF); SET(168, GetStringUTFLength); SET(169, GetStringUTFChars); SET(170, ReleaseStringUTFChars);
  SET(171, GetArrayLength); SET(172, NewObjectArray); SET(173, GetObjectArrayElement); SET(174, SetObjectArrayElement);

  SET(175, NewBooleanArray); SET(176, NewByteArray); SET(177, NewCharArray); SET(178, NewShortArray);
  SET(179, NewIntArray); SET(180, NewLongArray); SET(181, NewFloatArray); SET(182, NewDoubleArray);
  SET(183, GetBooleanArrayElements); SET(184, GetByteArrayElements); SET(185, GetCharArrayElements);
  SET(186, GetShortArrayElements); SET(187, GetIntArrayElements); SET(188, GetLongArrayElements);
  SET(189, GetFloatArrayElements); SET(190, GetDoubleArrayElements);
  SET(191, ReleaseBooleanArrayElements); SET(192, ReleaseByteArrayElements); SET(193, ReleaseCharArrayElements);
  SET(194, ReleaseShortArrayElements); SET(195, ReleaseIntArrayElements); SET(196, ReleaseLongArrayElements);
  SET(197, ReleaseFloatArrayElements); SET(198, ReleaseDoubleArrayElements);
  SET(199, GetBooleanArrayRegion); SET(200, GetByteArrayRegion); SET(201, GetCharArrayRegion);
  SET(202, GetShortArrayRegion); SET(203, GetIntArrayRegion); SET(204, GetLongArrayRegion);
  SET(205, GetFloatArrayRegion); SET(206, GetDoubleArrayRegion);
  SET(207, SetBooleanArrayRegion); SET(208, SetByteArrayRegion); SET(209, SetCharArrayRegion);
  SET(210, SetShortArrayRegion); SET(211, SetIntArrayRegion); SET(212, SetLongArrayRegion);
  SET(213, SetFloatArrayRegion); SET(214, SetDoubleArrayRegion);

  SET(215, RegisterNatives); SET(216, UnregisterNatives);
  SET(217, MonitorEnter); SET(218, MonitorExit); SET(219, GetJavaVM);
  SET(220, GetStringRegion); SET(221, GetStringUTFRegion);
  SET(222, GetPrimitiveArrayCritical); SET(223, ReleasePrimitiveArrayCritical);
  SET(224, GetStringCritical); SET(225, ReleaseStringCritical);
  SET(226, NewRef); SET(227, DeleteRef); SET(228, ExceptionCheck);
  SET(229, NewDirectByteBuffer); SET(230, GetDirectBufferAddress); SET(231, GetDirectBufferCapacity);
  SET(232, GetObjectRefType);

  (void)h_null;
  L("jni: fake env ready");
}
