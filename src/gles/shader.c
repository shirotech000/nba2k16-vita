// shaders: glsl is stashed, at link time it goes glsl -> cg (glsl2cg.c) ->
// gxp (SceShaccCg, cached on disk) -> shader patcher.
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <psp2/shacccg.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "gles/gl_internal.h"
#include "gles/gl_api.h"
#include "util/paths.h"
#include "util/cpu.h"

// say "linked" even when we failed, so the game keeps going and just
// skips those draws. off = tell the truth.
#define LIE_ABOUT_LINK_FAILURES 0

// glsl2cg entry point name (assumed, glsl2cg.h doesn't say)
#define G2C_ENTRY "main"

#define SHADERFAIL_DIR DATA_PATH "/shaderfail"

static name_table g_shaders, g_progs;
clear_prog_t CLR;

gl_program *prog_get(GLuint name) { return nt_get(&g_progs, name); }
gl_program *prog_current(void) { return G.program ? prog_get(G.program) : NULL; }
static gl_shader *sh_get(GLuint name) { return nt_get(&g_shaders, name); }

// ============================================================ SceShaccCg

static int g_shacc = 0;  // 0 untried, 1 ok, -1 missing
static SceShaccCgSourceFile g_src;

// ---- fallback arena for the compiler ----
// mid-game newlib's heap is full, so the Cg compiler died with "out of
// memory" and those programs never drew (126 of 245 draws skipped in one
// log). when malloc says no, the compiler gets its own memblock with a tiny
// first-fit allocator. it's handed back once the compile queue goes idle.
typedef struct ablk {
  uint32_t size;          // payload bytes
  uint32_t free;
  struct ablk *next, *prev;
} ablk;                   // 16 bytes, keeps payloads 16-aligned

static uint8_t *arena;
static uint32_t arena_size;
static SceUID arena_uid = -1;
static ablk *arena_head;
static int arena_live;    // bytes handed out from the arena

static int in_arena(const void *p) {
  return arena && (const uint8_t *)p >= arena && (const uint8_t *)p < arena + arena_size;
}

static int arena_open(void) {
  if (arena) return 1;
  static const uint32_t sizes[] = { 8 * 1024 * 1024, 4 * 1024 * 1024 };
  for (unsigned i = 0; i < 2 && arena_uid < 0; i++) {
    arena_uid = sceKernelAllocMemBlock("nba_cgheap", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_L1WBWA_RW, sizes[i], NULL);
    if (arena_uid < 0) arena_uid = sceKernelAllocMemBlock("nba_cgheap", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, sizes[i], NULL);
    if (arena_uid >= 0) arena_size = sizes[i];
  }
  if (arena_uid < 0) {
    L_ONCE("shader: no fallback arena for the Cg compiler either (0x%08X)", arena_uid);
    return 0;
  }
  void *base = NULL;
  sceKernelGetMemBlockBase(arena_uid, &base);
  arena = base;
  arena_head = (ablk *)arena;
  arena_head->size = arena_size - sizeof(ablk);
  arena_head->free = 1;
  arena_head->next = arena_head->prev = NULL;
  arena_live = 0;
  L("shader: Cg compiler fallback arena up (%uK)", arena_size / 1024);
  return 1;
}

static void arena_close_if_idle(void) {
  if (!arena || arena_live) return;
  sceKernelFreeMemBlock(arena_uid);
  arena_uid = -1;
  arena = NULL;
  arena_head = NULL;
  L("shader: Cg compiler fallback arena handed back");
}

static void *arena_alloc(uint32_t n) {
  n = ALIGN_UP(n ? n : 1, 16);
  for (ablk *b = arena_head; b; b = b->next) {
    if (!b->free || b->size < n) continue;
    if (b->size >= n + sizeof(ablk) + 64) {
      // split, the tail stays free
      ablk *t = (ablk *)((uint8_t *)(b + 1) + n);
      t->size = b->size - n - sizeof(ablk);
      t->free = 1;
      t->prev = b;
      t->next = b->next;
      if (b->next) b->next->prev = t;
      b->next = t;
      b->size = n;
    }
    b->free = 0;
    arena_live += b->size;
    return b + 1;
  }
  return NULL;
}

static void arena_free(void *p) {
  ablk *b = (ablk *)p - 1;
  b->free = 1;
  arena_live -= b->size;
  // merge with free neighbours (blocks are in address order)
  if (b->next && b->next->free) {
    ablk *n = b->next;
    b->size += sizeof(ablk) + n->size;
    b->next = n->next;
    if (n->next) n->next->prev = b;
  }
  if (b->prev && b->prev->free) {
    ablk *pv = b->prev;
    pv->size += sizeof(ablk) + b->size;
    pv->next = b->next;
    if (b->next) b->next->prev = pv;
  }
}

// the compiler's own memory, tracked so we can tell if it's what eats the heap.
// only one compile runs at a time (the worker, or init before it exists), so
// the arena needs no lock
static volatile int shacc_live, shacc_peak;
static void *shacc_malloc(unsigned int n) {
  void *p = malloc(n);
  size_t got = p ? malloc_usable_size(p) : 0;
  if (!p && arena_open()) {
    p = arena_alloc(n);
    if (p) got = ((ablk *)p - 1)->size;
  }
  if (!p) {
    L("shader: Cg compiler malloc(%u) failed, live %uK", n, shacc_live / 1024);
    return NULL;
  }
  int l = __sync_add_and_fetch(&shacc_live, (int)got);
  if (l > shacc_peak + 4 * 1024 * 1024) {
    shacc_peak = l;
    L("shader: Cg compiler memory peak %uK", l / 1024);
  }
  return p;
}
unsigned shacc_mem_live(void) { return (unsigned)shacc_live; }

static void shacc_free(void *p) {
  if (!p) return;
  if (in_arena(p)) {
    __sync_sub_and_fetch(&shacc_live, (int)((ablk *)p - 1)->size);
    arena_free(p);
    return;
  }
  __sync_sub_and_fetch(&shacc_live, (int)malloc_usable_size(p));
  free(p);
}

int shacc_ready(void)
{
  if (g_shacc) return g_shacc > 0;
  SceUID id = sceKernelLoadStartModule(SHACCCG_PATH, 0, NULL, 0, NULL, NULL);
  if (id < 0) {
    g_shacc = -1;
    L("=====================================================================");
    L("shader: can't load %s (0x%08X)", SHACCCG_PATH, id);
    L("shader: put libshacccg.suprx in ur0:data/ (dump it from your vita),");
    L("shader: without it NOTHING renders, not even glClear");
    L("=====================================================================");
    return 0;
  }
  sceShaccCgSetDefaultAllocator(shacc_malloc, shacc_free);
  g_shacc = 1;
  L("shader: libshacccg loaded (%s)", sceShaccCgGetVersionString());
  return 1;
}

// all includes resolve to our one in-memory file
static SceShaccCgSourceFile *cb_open(const char *name, const SceShaccCgSourceLocation *from,
                                     const SceShaccCgCompileOptions *opt, const char **err)
{
  (void)opt;
  if (!from || !strcmp(name, g_src.fileName)) return &g_src;
  *err = "includes not supported";
  return NULL;
}
static void cb_release(const SceShaccCgSourceFile *f, const SceShaccCgCompileOptions *opt) { (void)f; (void)opt; }
static const char *cb_locate(const char *name, const SceShaccCgSourceLocation *from, SceUInt32 n,
                             const char *const *paths, const SceShaccCgCompileOptions *opt, const char **err)
{
  (void)from; (void)n; (void)paths; (void)opt; (void)err;
  return name;
}
static const char *cb_abs(const char *name, const SceShaccCgSourceLocation *from, const SceShaccCgCompileOptions *opt)
{
  (void)from; (void)opt;
  return name;
}
static void cb_release_name(const char *name, const SceShaccCgCompileOptions *opt) { (void)name; (void)opt; }

// ---- disk cache ----
typedef struct {
  uint32_t magic, version, cg_len, cg_hash2, gxp_size;
} cache_hdr;
#define CACHE_MAGIC 0x50584E4E  // "NNXP"
#define CACHE_VER   1

static uint64_t hash64(const char *s, size_t n)
{
  uint64_t h = 1469598103934665603ull;
  while (n--) { h ^= (uint8_t)*s++; h *= 1099511628211ull; }
  return h;
}

static void cache_path(char *out, int n, uint64_t h, int frag)
{
  snprintf(out, n, "%s/%c%016llx.gxp", SHADER_CACHE, frag ? 'f' : 'v', (unsigned long long)h);
}

static SceGxmProgram *cache_load(const char *path, uint32_t cg_len, uint32_t h2)
{
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) return NULL;
  cache_hdr hd;
  SceGxmProgram *p = NULL;
  if (sceIoRead(fd, &hd, sizeof(hd)) == sizeof(hd) && hd.magic == CACHE_MAGIC &&
      hd.version == CACHE_VER && hd.cg_len == cg_len && hd.cg_hash2 == h2 &&
      hd.gxp_size > 0 && hd.gxp_size < 4 * 1024 * 1024) {
    p = malloc(hd.gxp_size);
    if (sceIoRead(fd, p, hd.gxp_size) != (SceSSize)hd.gxp_size || sceGxmProgramCheck(p) < 0) {
      L("shader: bad cache file %s, ignoring", path);
      free(p);
      p = NULL;
    }
  }
  sceIoClose(fd);
  return p;
}

static void cache_store(const char *path, const SceGxmProgram *p, uint32_t size, uint32_t cg_len, uint32_t h2)
{
  SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0) { L_ONCE("shader: can't write cache %s (0x%08X)", path, fd); return; }
  cache_hdr hd = { CACHE_MAGIC, CACHE_VER, cg_len, h2, size };
  sceIoWrite(fd, &hd, sizeof(hd));
  sceIoWrite(fd, p, size);
  sceIoClose(fd);
}

static void log_append(char **log, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_append(char **log, const char *fmt, ...)
{
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  size_t old = *log ? strlen(*log) : 0, add = strlen(tmp);
  *log = realloc(*log, old + add + 2);
  memcpy(*log + old, tmp, add);
  (*log)[old + add] = '\n';
  (*log)[old + add + 1] = 0;
}

// cache only, never compiles. NULL = not cached (yet)
static SceGxmProgram *shacc_lookup(const char *cg, int frag)
{
  size_t len = strlen(cg);
  uint64_t h = hash64(cg, len) ^ (frag ? 0x9E3779B97F4A7C15ull : 0);
  uint32_t h2 = fnv1a(cg, len, 2166136261u);
  char path[128];
  cache_path(path, sizeof(path), h, frag);
  return cache_load(path, (uint32_t)len, h2);
}

SceGxmProgram *shacc_compile(const char *cg, int frag, char **log)
{
  size_t len = strlen(cg);
  uint64_t h = hash64(cg, len) ^ (frag ? 0x9E3779B97F4A7C15ull : 0);
  uint32_t h2 = fnv1a(cg, len, 2166136261u);
  char path[128];
  cache_path(path, sizeof(path), h, frag);

  SceGxmProgram *p = cache_load(path, (uint32_t)len, h2);
  if (p) return p;

  if (!shacc_ready()) {
    log_append(log, "libshacccg not available");
    return NULL;
  }

  // crash guard: libshacccg can take the whole process down on some
  // inputs (seen in vita3k). we leave a marker while compiling, so if it's
  // still there next boot this exact source killed us -> skip it for good.
  char base[128], mark[140], bad[140], srcf[140];
  snprintf(base, sizeof(base), "%.*s", (int)(strlen(path) - 4), path);  // drop ".gxp"
  snprintf(mark, sizeof(mark), "%s.compiling", base);
  snprintf(bad, sizeof(bad), "%s.bad", base);
  snprintf(srcf, sizeof(srcf), "%s.cg", base);
  SceIoStat st;
  if (sceIoGetstat(bad, &st) >= 0) {
    log_append(log, "skipped: this source crashed the Cg compiler before (%s)", srcf);
    return NULL;
  }
  if (sceIoGetstat(mark, &st) >= 0) {
    sceIoRename(mark, bad);
    L("shader: %s crashed the Cg compiler last run, marking it bad (source kept in %s)", base, srcf);
    log_append(log, "skipped: crashed the Cg compiler last run (%s)", srcf);
    return NULL;
  }
  SceUID mfd = sceIoOpen(srcf, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (mfd >= 0) { sceIoWrite(mfd, cg, len); sceIoClose(mfd); }
  mfd = sceIoOpen(mark, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (mfd >= 0) sceIoClose(mfd);

  g_src.fileName = frag ? "<nba_fs>" : "<nba_vs>";
  g_src.text = cg;
  g_src.size = (SceUInt32)len;

  SceShaccCgCompileOptions opt;
  sceShaccCgInitializeCompileOptions(&opt);
  opt.mainSourceFile = g_src.fileName;
  opt.targetProfile = frag ? SCE_SHACCCG_PROFILE_FP : SCE_SHACCCG_PROFILE_VP;
  opt.entryFunctionName = G2C_ENTRY;
  opt.searchPathCount = 0;
  opt.macroDefinitionCount = 0;
  opt.includeFileCount = 0;
  opt.locale = SCE_SHACCCG_ENGLISH;
  opt.optimizationLevel = 3;
  opt.useFastmath = 1;
  opt.useFastprecision = 0;
  opt.useFastint = 1;
  opt.warningsAsErrors = 0;
  opt.performanceWarnings = 0;
  opt.pedantic = 0;

  SceShaccCgCallbackList cb;
  sceShaccCgInitializeCallbackList(&cb, SCE_SHACCCG_TRIVIAL);
  cb.openFile = cb_open;
  cb.releaseFile = cb_release;
  cb.locateFile = cb_locate;
  cb.absolutePath = cb_abs;
  cb.releaseFileName = cb_release_name;

  const SceShaccCgCompileOutput *out = sceShaccCgCompileProgram(&opt, &cb, 0);
  sceIoRemove(mark);  // made it out alive
  if (!out) {
    log_append(log, "sceShaccCgCompileProgram returned NULL");
    return NULL;
  }
  for (int i = 0; i < out->diagnosticCount; i++) {
    const SceShaccCgDiagnosticMessage *d = &out->diagnostics[i];
    static const char *lv[] = { "info", "warning", "error" };
    unsigned line = d->location ? d->location->lineNumber : 0;
    unsigned col = d->location ? d->location->columnNumber : 0;
    if (d->level >= SCE_SHACCCG_DIAGNOSTIC_LEVEL_WARNING || !out->programData)
      log_append(log, "%s:%u:%u: %s: %s", frag ? "fs" : "vs", line, col,
                 lv[d->level <= 2 ? d->level : 2], d->message ? d->message : "?");
  }
  if (out->programData && out->programSize) {
    // copy it out, the patcher keeps pointing at whatever we register
    p = malloc(out->programSize);
    memcpy(p, out->programData, out->programSize);
    if (sceGxmProgramCheck(p) < 0) {
      log_append(log, "sceGxmProgramCheck rejected the output");
      free(p);
      p = NULL;
    } else {
      cache_store(path, p, out->programSize, (uint32_t)len, h2);
    }
  }
  sceShaccCgDestroyCompileOutput(out);
  return p;
}

static void dump_fail(gl_program *pr, const char *vs, const char *fs, const char *vcg, const char *fcg,
                      const char *why)
{
  static unsigned n;
  char path[128];
  snprintf(path, sizeof(path), "%s/prog%04u_%u.txt", SHADERFAIL_DIR, n++, pr->name);
  SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0) { L("shader: can't write %s", path); return; }
  const char *parts[][2] = {
    { "==== error\n", why }, { "\n==== glsl vs\n", vs }, { "\n==== glsl fs\n", fs },
    { "\n==== cg vs\n", vcg }, { "\n==== cg fs\n", fcg },
  };
  for (unsigned i = 0; i < ARRAY_LEN(parts); i++) {
    sceIoWrite(fd, parts[i][0], strlen(parts[i][0]));
    const char *s = parts[i][1] ? parts[i][1] : "(none)";
    sceIoWrite(fd, s, strlen(s));
  }
  sceIoClose(fd);
  L("shader: dumped failing program %u to %s", pr->name, path);
}

// ============================================================ big stack runner
// the cg compiler + glsl2cg recursion want a lot of stack, and glLinkProgram
// runs on whatever thread the game uses. do the heavy part on our own thread.
#define COMPILE_STACK (1024 * 1024)

typedef struct { void (*fn)(void *); void *arg; } stack_job;

static int stack_job_entry(SceSize args, void *argp)
{
  (void)args;
  stack_job *j = *(stack_job **)argp;
  j->fn(j->arg);
  return 0;
}

static void run_on_big_stack(void (*fn)(void *), void *arg)
{
  stack_job j = { fn, arg };
  stack_job *jp = &j;
  SceUID th = sceKernelCreateThread("nba_shaderc", stack_job_entry, 0x10000100, COMPILE_STACK, 0, 0, NULL);
  if (th < 0) {
    L_ONCE("shader: can't create compile thread (0x%08X), compiling inline", th);
    fn(arg);
    return;
  }
  sceKernelStartThread(th, sizeof(jp), &jp);
  sceKernelWaitThreadEnd(th, NULL, NULL);
  sceKernelDeleteThread(th);
}

// ============================================================ clear program

static const char *clear_vs_cg =
  "void main(float2 aPosition, uniform float uDepth, out float4 oPos : POSITION)\n"
  "{\n"
  "  oPos = float4(aPosition, uDepth, 1.0f);\n"
  "}\n";

static const char *clear_fs_cg =
  "float4 main(uniform float4 uColor) : COLOR\n"
  "{\n"
  "  return uColor;\n"
  "}\n";

static void clear_init_job(void *unused)
{
  (void)unused;
  char *log = NULL;
  CLR.vgxp = shacc_compile(clear_vs_cg, 0, &log);
  CLR.fgxp = shacc_compile(clear_fs_cg, 1, &log);
  if (!CLR.vgxp || !CLR.fgxp) {
    L("shader: clear shader failed to compile:\n%s", log ? log : "");
    free(log);
    return;
  }
  free(log);
  sceGxmShaderPatcherRegisterProgram(gxm_patcher, CLR.vgxp, &CLR.vid);
  sceGxmShaderPatcherRegisterProgram(gxm_patcher, CLR.fgxp, &CLR.fid);
  const SceGxmProgramParameter *pos = sceGxmProgramFindParameterByName(CLR.vgxp, "aPosition");
  CLR.u_depth = sceGxmProgramFindParameterByName(CLR.vgxp, "uDepth");
  CLR.u_color = sceGxmProgramFindParameterByName(CLR.fgxp, "uColor");
  if (!pos || !CLR.u_depth || !CLR.u_color) {
    L("shader: clear shader params missing (%p %p %p)", pos, CLR.u_depth, CLR.u_color);
    return;
  }
  SceGxmVertexAttribute a;
  a.streamIndex = 0;
  a.offset = 0;
  a.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
  a.componentCount = 2;
  a.regIndex = sceGxmProgramParameterGetResourceIndex(pos);
  SceGxmVertexStream s;
  s.stride = 8;
  s.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
  int r = sceGxmShaderPatcherCreateVertexProgram(gxm_patcher, CLR.vid, &a, 1, &s, 1, &CLR.vp);
  if (r < 0) { L("shader: clear vertex program failed 0x%08X", r); return; }
  CLR.ok = 1;
  L("shader: clear shader ready");
}

SceGxmFragmentProgram *clear_get_fp(uint8_t cmask, SceGxmOutputRegisterFormat fmt)
{
  SceGxmMultisampleMode ms = gxm_scene_msaa();
  uint64_t key = cmask | ((uint64_t)fmt << 8) | ((uint64_t)ms << 40);
  for (fp_variant *v = CLR.fps; v; v = v->next)
    if (v->key == key) return v->fp;
  SceGxmBlendInfo bi;
  memset(&bi, 0, sizeof(bi));
  bi.colorMask = cmask;
  bi.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
  bi.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
  bi.colorSrc = bi.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
  bi.colorDst = bi.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
  fp_variant *v = calloc(1, sizeof(*v));
  int r = sceGxmShaderPatcherCreateFragmentProgram(gxm_patcher, CLR.fid, fmt, ms,
                                                   &bi, CLR.vgxp, &v->fp);
  if (r < 0) {
    L("shader: clear fragment program (mask %x fmt %d) failed 0x%08X", cmask, fmt, r);
    free(v);
    return NULL;
  }
  v->key = key;
  v->next = CLR.fps;
  CLR.fps = v;
  return v->fp;
}

void shader_init(void)
{
  static int done;
  if (done) return;
  done = 1;
  sceIoMkdir(DATA_PATH, 0777);
  sceIoMkdir(SHADER_CACHE, 0777);
  sceIoMkdir(SHADERFAIL_DIR, 0777);
  shacc_ready();
  if (gxm_ready()) run_on_big_stack(clear_init_job, NULL);
}

// ============================================================ shader objects

GLuint glCreateShader(GLenum type)
{
  gl_shader *s = calloc(1, sizeof(*s));
  s->name = nt_gen(&g_shaders);
  s->type = type;
  nt_set(&g_shaders, s->name, s);
  TRACE("glCreateShader(0x%X) = %u", type, s->name);
  return s->name;
}

void glShaderSource(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len)
{
  gl_shader *s = sh_get(sh);
  if (!s) { GL_ERR(GL_INVALID_VALUE); return; }
  size_t total = 0;
  for (int i = 0; i < count; i++)
    total += (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]);
  free(s->src);
  s->src = malloc(total + 1);
  size_t o = 0;
  for (int i = 0; i < count; i++) {
    size_t n = (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]);
    memcpy(s->src + o, str[i], n);
    o += n;
  }
  s->src[o] = 0;
  s->compiled = 0;
}

void glCompileShader(GLuint sh)
{
  // real work happens at link, when both stages are known
  gl_shader *s = sh_get(sh);
  if (!s) { GL_ERR(GL_INVALID_VALUE); return; }
  s->compiled = s->src != NULL;
}

static void sh_free(gl_shader *s)
{
  nt_set(&g_shaders, s->name, NULL);
  free(s->src);
  free(s);
}

void glDeleteShader(GLuint sh)
{
  gl_shader *s = sh_get(sh);
  if (!s) return;
  if (s->attached) s->delete_pending = 1;
  else sh_free(s);
}

void glGetShaderiv(GLuint sh, GLenum pname, GLint *out)
{
  gl_shader *s = sh_get(sh);
  if (!s) { GL_ERR(GL_INVALID_VALUE); return; }
  switch (pname) {
  case GL_SHADER_TYPE: *out = s->type; break;
  case GL_DELETE_STATUS: *out = s->delete_pending; break;
  case GL_COMPILE_STATUS: *out = s->compiled; break;   // real errors show at link
  case GL_INFO_LOG_LENGTH: *out = 0; break;
  case GL_SHADER_SOURCE_LENGTH: *out = s->src ? (GLint)strlen(s->src) + 1 : 0; break;
  default: L_ONCE("gl: glGetShaderiv 0x%X unhandled", pname); *out = 0; break;
  }
}

void glGetShaderInfoLog(GLuint sh, GLsizei max, GLsizei *len, GLchar *log)
{
  (void)sh;
  if (len) *len = 0;
  if (log && max > 0) log[0] = 0;
}

// ============================================================ programs

GLuint glCreateProgram(void)
{
  gl_program *p = calloc(1, sizeof(*p));
  p->name = nt_gen(&g_progs);
  nt_set(&g_progs, p->name, p);
  return p->name;
}

void glAttachShader(GLuint prog, GLuint sh)
{
  gl_program *p = prog_get(prog);
  gl_shader *s = sh_get(sh);
  if (!p || !s) { GL_ERR(GL_INVALID_VALUE); return; }
  if (s->type == GL_VERTEX_SHADER) p->vs = sh;
  else p->fs = sh;
  s->attached++;
}

void glBindAttribLocation(GLuint prog, GLuint idx, const GLchar *name)
{
  gl_program *p = prog_get(prog);
  if (!p || idx >= MAX_ATTRIBS) { GL_ERR(GL_INVALID_VALUE); return; }
  for (int i = 0; i < p->n_binds; i++) {
    if (!strcmp(p->binds[i].name, name)) { p->binds[i].loc = idx; return; }
  }
  if (p->n_binds == (int)ARRAY_LEN(p->binds)) return;
  snprintf(p->binds[p->n_binds].name, GLSL2CG_NAME_LEN, "%s", name);
  p->binds[p->n_binds].loc = idx;
  p->n_binds++;
}

// tear down everything link produced. deferred so the gpu is done with it.
typedef struct {
  SceGxmProgram *vgxp, *fgxp;
  SceGxmShaderPatcherId vid, fid;
  vp_variant *vps;
  fp_variant *fps;
} prog_garbage;

static void job_detach(struct gl_program *p);

static void prog_garbage_free(void *arg)
{
  prog_garbage *g = arg;
  for (vp_variant *v = g->vps, *n; v; v = n) {
    n = v->next;
    if (v->vp) sceGxmShaderPatcherReleaseVertexProgram(gxm_patcher, v->vp);
    free(v);
  }
  for (fp_variant *v = g->fps, *n; v; v = n) {
    n = v->next;
    if (v->fp) sceGxmShaderPatcherReleaseFragmentProgram(gxm_patcher, v->fp);
    free(v);
  }
  if (g->vid) sceGxmShaderPatcherUnregisterProgram(gxm_patcher, g->vid);
  if (g->fid) sceGxmShaderPatcherUnregisterProgram(gxm_patcher, g->fid);
  free(g->vgxp);
  free(g->fgxp);
  free(g);
}

static void prog_reset(gl_program *p)
{
  job_detach(p);
  if (p->vgxp || p->fgxp || p->vps || p->fps) {
    prog_garbage *g = calloc(1, sizeof(*g));
    g->vgxp = p->vgxp; g->fgxp = p->fgxp;
    g->vid = p->vid; g->fid = p->fid;
    g->vps = p->vps; g->fps = p->fps;
    gxm_defer(prog_garbage_free, g);
  }
  p->vgxp = p->fgxp = NULL;
  p->vid = p->fid = NULL;
  p->vps = NULL;
  p->fps = NULL;
  for (int i = 0; i < p->n_uniforms; i++) free(p->uniforms[i].data);
  free(p->uniforms);
  p->uniforms = NULL;
  p->n_uniforms = 0;
  free(p->loc_map);
  p->loc_map = NULL;
  p->n_locs = 0;
  p->n_attribs = 0;
  p->n_samplers = 0;
  p->uses_iid = 0;
  p->linked = 0;
  free(p->log);
  p->log = NULL;
}

static void sh_detach(GLuint name)
{
  gl_shader *s = sh_get(name);
  if (!s) return;
  if (s->attached) s->attached--;
  if (!s->attached && s->delete_pending) sh_free(s);
}

void glDeleteProgram(GLuint prog)
{
  gl_program *p = prog_get(prog);
  if (!p) return;
  if (G.program == prog) {
    // GL keeps it alive while current, we just keep it until unbound
    p->delete_pending = 1;
    return;
  }
  prog_reset(p);
  sh_detach(p->vs);
  sh_detach(p->fs);
  nt_set(&g_progs, prog, NULL);
  free(p);
}

static int type_cols(g2c_type t)
{
  switch (t) {
  case G2C_MAT2: return 2;
  case G2C_MAT3: return 3;
  case G2C_MAT4: return 4;
  default: return 1;
  }
}

static int type_comps(g2c_type t)
{
  switch (t) {
  case G2C_FLOAT: case G2C_INT: case G2C_BOOL: return 1;
  case G2C_VEC2: case G2C_IVEC2: case G2C_BVEC2: case G2C_MAT2: return 2;
  case G2C_VEC3: case G2C_IVEC3: case G2C_BVEC3: case G2C_MAT3: return 3;
  default: return 4;
  }
}

static int type_is_int(g2c_type t)
{
  return (t >= G2C_INT && t <= G2C_BVEC4);
}

static int g_link_logs;
static unsigned uni_words(const prog_uniform *u, int stage);

// sanity check for the upload layout: res is taken as a 32-bit word offset.
// if that's ever wrong (vec4 units etc), uniforms overlap and we say so.
// the first few programs also dump res/comps/arr per uniform.
static void check_uniform_layout(gl_program *p, int stage, uint32_t bufsize)
{
  static int overlaps;
  int dump = g_link_logs < 8;
  for (int i = 0; i < p->n_uniforms; i++) {
    prog_uniform *a = &p->uniforms[i];
    if (a->is_sampler || !a->st[stage].p) continue;
    unsigned a0 = a->st[stage].res, a1 = a0 + uni_words(a, stage);
    if (dump)
      L("shader:   %s uniform %s res %u comps %u arr %u type %d -> words [%u,%u) of %u", stage ? "fs" : "vs",
        a->name, a->st[stage].res, a->st[stage].comps, a->st[stage].arr, a->st[stage].type, a0, a1, bufsize / 4);
    if (a->st[stage].type != SCE_GXM_PARAMETER_TYPE_F32) continue;
    for (int k = i + 1; k < p->n_uniforms && overlaps < 16; k++) {
      prog_uniform *b = &p->uniforms[k];
      if (b->is_sampler || !b->st[stage].p || b->st[stage].type != SCE_GXM_PARAMETER_TYPE_F32) continue;
      unsigned b0 = b->st[stage].res, b1 = b0 + uni_words(b, stage);
      if (a0 < b1 && b0 < a1) {
        overlaps++;
        L("shader: !! prog %u %s: uniform %s words [%u,%u) overlaps %s [%u,%u), upload layout is wrong",
          p->name, stage ? "fs" : "vs", a->name, a0, a1, b->name, b0, b1);
      }
    }
  }
}

static void setup_uniform_stage(gl_program *p, prog_uniform *u, int stage)
{
  const SceGxmProgram *gxp = stage ? p->fgxp : p->vgxp;
  const SceGxmProgramParameter *prm = sceGxmProgramFindParameterByName(gxp, u->cg_name);
  if (!prm) return;
  SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(prm);
  if (u->is_sampler) {
    if (cat != SCE_GXM_PARAMETER_CATEGORY_SAMPLER) return;
  } else if (cat != SCE_GXM_PARAMETER_CATEGORY_UNIFORM) {
    L("shader: uniform %s has category %d in %s", u->name, cat, stage ? "fs" : "vs");
    return;
  }
  u->st[stage].p = prm;
  u->st[stage].res = sceGxmProgramParameterGetResourceIndex(prm);
  u->st[stage].comps = sceGxmProgramParameterGetComponentCount(prm);
  u->st[stage].arr = sceGxmProgramParameterGetArraySize(prm);
  u->st[stage].type = sceGxmProgramParameterGetType(prm);
  if (u->is_sampler) return;
  // float4 slot layout for matrices/arrays (glsl2cg convention), or when the
  // cg side is a float4 anyway. otherwise native.
  int slot_layout = u->slots > 1 || (u->st[stage].comps == 4 && u->comps < 4);
  u->st[stage].stride = slot_layout ? 4 : u->comps;
  if (slot_layout && u->st[stage].comps != 4)
    L("shader: %s: expected float4 slots, gxp says %u comps x %u (prog %u)", u->name,
      u->st[stage].comps, u->st[stage].arr, p->name);
  if (u->st[stage].type != SCE_GXM_PARAMETER_TYPE_F32)
    L_ONCE("shader: non-f32 uniform seen (%s type %d), using slow path", u->name, u->st[stage].type);
}

static void collect_samplers(gl_program *p, int stage)
{
  const SceGxmProgram *gxp = stage ? p->fgxp : p->vgxp;
  unsigned n = sceGxmProgramGetParameterCount(gxp);
  for (unsigned i = 0; i < n; i++) {
    const SceGxmProgramParameter *prm = sceGxmProgramGetParameter(gxp, i);
    if (sceGxmProgramParameterGetCategory(prm) != SCE_GXM_PARAMETER_CATEGORY_SAMPLER) continue;
    if (p->n_samplers == (int)ARRAY_LEN(p->samplers)) break;
    prog_sampler *s = &p->samplers[p->n_samplers++];
    const char *name = sceGxmProgramParameterGetName(prm);
    s->stage = stage;
    s->res = sceGxmProgramParameterGetResourceIndex(prm);
    s->cube = sceGxmProgramParameterIsSamplerCube(prm);
    s->uniform = -1;
    for (int k = 0; k < p->n_uniforms; k++)
      if (p->uniforms[k].is_sampler && !strcmp(p->uniforms[k].cg_name, name)) { s->uniform = k; break; }
    if (g_link_logs < 40)
      L("shader: prog %u %s sampler '%s' -> slot %u%s%s", p->name, stage ? "fs" : "vs", name, s->res,
        s->cube ? " (cube)" : "", s->uniform < 0 ? " (no gl uniform!)" : "");
  }
}

// ============================================================ link
// split in two so a slow Cg compile never freezes the game:
//   link_front: translate + attribute/uniform locations (no gxp needed),
//               so glGetUniformLocation / glUniform* work right away
//   link_back:  register with the patcher + resolve gxp params. runs on the
//               render thread once both gxps exist (cache hit or worker done)
// draws with a program that's still compiling just get skipped.

static void link_front(gl_program *p, const g2c_result *r)
{
  // ---- attributes: explicit binds > layout(location) > first free ----
  unsigned used = 0;
  int loc_of[GLSL2CG_MAX_ATTRIBS];
  for (int i = 0; i < r->n_attribs; i++) {
    loc_of[i] = -1;
    for (int b = 0; b < p->n_binds; b++)
      if (!strcmp(p->binds[b].name, r->attribs[i].name)) loc_of[i] = p->binds[b].loc;
    if (loc_of[i] < 0 && r->attribs[i].location >= 0) loc_of[i] = r->attribs[i].location;
    if (loc_of[i] >= 0 && loc_of[i] < MAX_ATTRIBS) used |= 1u << loc_of[i];
  }
  for (int i = 0; i < r->n_attribs && p->n_attribs < MAX_ATTRIBS; i++) {
    if (loc_of[i] < 0 || loc_of[i] >= MAX_ATTRIBS) {
      int l = 0;
      while (l < MAX_ATTRIBS && (used & (1u << l))) l++;
      if (l == MAX_ATTRIBS) break;
      loc_of[i] = l;
      used |= 1u << l;
    }
    prog_attrib *a = &p->attribs[p->n_attribs++];
    snprintf(a->name, sizeof(a->name), "%s", r->attribs[i].name);
    snprintf(a->cg_name, sizeof(a->cg_name), "%s", r->attribs[i].cg_name[0] ? r->attribs[i].cg_name : r->attribs[i].name);
    a->loc = loc_of[i];
    a->reg = -1;
  }

  // ---- uniforms ----
  p->uniforms = calloc(r->n_uniforms ? r->n_uniforms : 1, sizeof(prog_uniform));
  p->n_uniforms = r->n_uniforms;
  int nloc = 0;
  for (int i = 0; i < r->n_uniforms; i++) {
    prog_uniform *u = &p->uniforms[i];
    const g2c_uniform *g = &r->uniforms[i];
    snprintf(u->name, sizeof(u->name), "%s", g->name);
    snprintf(u->cg_name, sizeof(u->cg_name), "%s", g->cg_name[0] ? g->cg_name : g->name);
    u->type = g->type;
    u->elems = g->array_size > 0 ? g->array_size : 1;
    u->cols = type_cols(g->type);
    if (g->type == G2C_OTHER && g->vec4_slots > u->elems) u->cols = g->vec4_slots / u->elems;
    u->comps = type_comps(g->type);
    u->slots = u->elems * u->cols;
    u->is_sampler = g->type == G2C_SAMPLER2D || g->type == G2C_SAMPLERCUBE;
    u->is_cube = g->type == G2C_SAMPLERCUBE;
    u->is_int = type_is_int(g->type);
    u->in_vs = g->in_vs;
    u->in_fs = g->in_fs;
    u->data = calloc(u->slots * 4, sizeof(float));
    u->loc_base = nloc;
    nloc += u->elems;
    if (!u->is_sampler && u->slots > 1 && g->vec4_slots != u->slots)
      L("shader: %s: glsl2cg says %d slots, we think %d", u->name, g->vec4_slots, u->slots);
  }
  p->n_locs = nloc;
  p->loc_map = malloc((nloc ? nloc : 1) * sizeof(int16_t));
  for (int i = 0; i < r->n_uniforms; i++)
    for (int e = 0; e < p->uniforms[i].elems; e++)
      p->loc_map[p->uniforms[i].loc_base + e] = (int16_t)i;

  // gl_InstanceID is an INSTANCE param the hardware fills, nothing to bind
  p->uses_iid = r->uses_instance_id;
}

// p->vgxp / p->fgxp are set. patcher + gxp lookups, render thread only
static int link_back(gl_program *p, char **log)
{
  int e1 = sceGxmShaderPatcherRegisterProgram(gxm_patcher, p->vgxp, &p->vid);
  int e2 = sceGxmShaderPatcherRegisterProgram(gxm_patcher, p->fgxp, &p->fid);
  if (e1 < 0 || e2 < 0) {
    log_append(log, "sceGxmShaderPatcherRegisterProgram failed 0x%08X 0x%08X", e1, e2);
    if (e1 >= 0) sceGxmShaderPatcherUnregisterProgram(gxm_patcher, p->vid);
    if (e2 >= 0) sceGxmShaderPatcherUnregisterProgram(gxm_patcher, p->fid);
    p->vid = p->fid = NULL;
    return 0;
  }
  p->vbuf_size = sceGxmProgramGetDefaultUniformBufferSize(p->vgxp);
  p->fbuf_size = sceGxmProgramGetDefaultUniformBufferSize(p->fgxp);

  for (int i = 0; i < p->n_attribs; i++) {
    prog_attrib *a = &p->attribs[i];
    const SceGxmProgramParameter *prm = sceGxmProgramFindParameterByName(p->vgxp, a->cg_name);
    if (prm && sceGxmProgramParameterGetCategory(prm) == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) {
      a->reg = sceGxmProgramParameterGetResourceIndex(prm);
      a->comps = sceGxmProgramParameterGetComponentCount(prm);
    }
  }
  for (int i = 0; i < p->n_uniforms; i++) {
    prog_uniform *u = &p->uniforms[i];
    if (u->in_vs) setup_uniform_stage(p, u, 0);
    if (u->in_fs) setup_uniform_stage(p, u, 1);
  }
  collect_samplers(p, 0);
  collect_samplers(p, 1);
  check_uniform_layout(p, 0, p->vbuf_size);
  check_uniform_layout(p, 1, p->fbuf_size);

  if (g_link_logs < 40) {
    g_link_logs++;
    L("shader: linked prog %u: %d attribs, %d uniforms, %d samplers, ubuf vs %u fs %u", p->name,
      p->n_attribs, p->n_uniforms, p->n_samplers, p->vbuf_size, p->fbuf_size);
    for (int i = 0; i < p->n_attribs; i++)
      L("shader:   attrib %s loc %d reg %d comps %d", p->attribs[i].name, p->attribs[i].loc,
        p->attribs[i].reg, p->attribs[i].comps);
  }
  return 1;
}

// ---- background compile worker ----
typedef struct compile_job {
  GLuint prog;
  char *vcg, *fcg, *vsrc, *fsrc;  // owned copies (glsl kept for the failure dump)
  SceGxmProgram *vgxp, *fgxp;     // whichever came from the cache are already set
  char *log;
  int done, orphan;               // guarded by q_lock
  int attempts;                   // compiles tried so far
  SceUInt64 not_before;           // retry backoff (process time, us)
  struct compile_job *next;
} compile_job;

static SceKernelLwMutexWork q_lock;
static SceUID q_sema = -1;
static compile_job *q_head, *q_tail;
static int g_async = -1;          // -1 untried, 0 off (sync compiles), 1 worker running
static volatile int q_pending;

static void job_free(compile_job *j)
{
  free(j->vcg); free(j->fcg); free(j->vsrc); free(j->fsrc);
  free(j->log);
  free(j->vgxp);
  free(j->fgxp);
  free(j);
}

static int compile_worker(SceSize args, void *argp)
{
  (void)args; (void)argp;
  cpu_place_game_thread();  // core 3 too with CapUnlocker, that's what it's for
  for (;;) {
    sceKernelWaitSema(q_sema, 1, NULL);
    sceKernelLockLwMutex(&q_lock, 1, NULL);
    compile_job *j = q_head;
    if (j) { q_head = j->next; if (!q_head) q_tail = NULL; }
    sceKernelUnlockLwMutex(&q_lock, 1);
    if (!j) continue;

    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (j->not_before > now) sceKernelDelayThread((SceUInt32)(j->not_before - now));
    j->attempts++;
    if (!j->vgxp) j->vgxp = shacc_compile(j->vcg, 0, &j->log);
    if (j->vgxp && !j->fgxp) j->fgxp = shacc_compile(j->fcg, 1, &j->log);

    sceKernelLockLwMutex(&q_lock, 1, NULL);
    int orphan = j->orphan;
    j->done = 1;
    sceKernelUnlockLwMutex(&q_lock, 1);
    if (!__sync_sub_and_fetch(&q_pending, 1)) arena_close_if_idle();
    if (orphan) job_free(j);  // program got relinked/deleted meanwhile
  }
  return 0;
}

static int async_ready(void)
{
  if (g_async >= 0) return g_async;
  g_async = 0;
  extern int config_int(const char *key, int def);
  if (!config_int("async_shaders", 1)) {
    L("shader: async_shaders=0, compiling inline");
    return 0;
  }
  sceKernelCreateLwMutex(&q_lock, "shader_q", 0, 0, NULL);
  q_sema = sceKernelCreateSema("shader_q", 0, 0, 0x7FFFFFFF, NULL);
  // just under the game's threads (160): uses idle cpu, doesn't starve gameplay
  SceUID th = sceKernelCreateThread("nba_shaderq", compile_worker, 0xA8, COMPILE_STACK, 0, 0, NULL);
  if (q_sema < 0 || th < 0) {
    L("shader: no compile worker (0x%08X), compiling inline", th);
    return 0;
  }
  sceKernelStartThread(th, 0, NULL);
  g_async = 1;
  L("shader: background compile worker up");
  return 1;
}

static void queue_job(compile_job *j)
{
  sceKernelLockLwMutex(&q_lock, 1, NULL);
  if (q_tail) q_tail->next = j; else q_head = j;
  q_tail = j;
  sceKernelUnlockLwMutex(&q_lock, 1);
  int n = __sync_add_and_fetch(&q_pending, 1);
  if (n == 1 || !(n & 7)) L("shader: %d compiles queued", n);
  sceKernelSignalSema(q_sema, 1);
}

// detach a pending job from its program (relink/delete). the worker frees it
// when it's done, or we free it now if it already is
static void job_detach(gl_program *p)
{
  compile_job *j = p->job;
  if (!j) return;
  p->job = NULL;
  p->pending = 0;
  sceKernelLockLwMutex(&q_lock, 1, NULL);
  int done = j->done;
  if (!done) j->orphan = 1;
  sceKernelUnlockLwMutex(&q_lock, 1);
  if (done) job_free(j);
}

// called before drawing with p. 1 = ready to draw
int prog_try_finish(gl_program *p)
{
  if (!p->pending) return p->linked;
  compile_job *j = p->job;
  sceKernelLockLwMutex(&q_lock, 1, NULL);
  int done = j->done;
  sceKernelUnlockLwMutex(&q_lock, 1);
  if (!done) return 0;

  p->job = NULL;
  p->pending = 0;
  p->vgxp = j->vgxp; j->vgxp = NULL;
  p->fgxp = j->fgxp; j->fgxp = NULL;
  char *log = j->log; j->log = NULL;
  int ok = p->vgxp && p->fgxp && link_back(p, &log);
  // out of memory (and the compiler's "fatal internal error", which is
  // usually the same thing) isn't the shader's fault: try again later
  if (!ok && log && j->attempts < 3 &&
      (strstr(log, "out of memory") || strstr(log, "fatal internal error"))) {
    L("shader: program %u failed to compile (try %d), retrying in %ds", p->name, j->attempts, j->attempts * 2);
    if (p->vgxp && p->vid == NULL) { j->vgxp = p->vgxp; p->vgxp = NULL; }
    if (p->fgxp && p->fid == NULL) { j->fgxp = p->fgxp; p->fgxp = NULL; }
    free(log);
    j->done = 0;
    j->next = NULL;
    j->not_before = sceKernelGetProcessTimeWide() + (SceUInt64)j->attempts * 2000000;
    p->job = j;
    p->pending = 1;
    queue_job(j);
    return 0;
  }
  if (!ok) {
    dump_fail(p, j->vsrc, j->fsrc, j->vcg, j->fcg, log ? log : "?");
    L("!!!! shader: LINK FAILED for program %u (background compile):\n%s", p->name, log ? log : "?");
    p->log = NULL;
    prog_reset(p);
    p->log = log;
    p->linked = 0;
  } else {
    free(log);
  }
  job_free(j);
  return ok;
}

typedef struct { gl_program *p; char *log; int ok; } link_job;

// ---- translation cache ----
// the game links its whole shader set during the intro, and translating
// every pair again on each boot made the logo fade crawl (~45ms a link in
// vita3k). so glsl2cg's output gets cached on disk next to the gxps, keyed
// by the GLSL pair and the translator's build stamp.
#define TCACHE_MAGIC 0x54433247u  // "G2CT"

typedef struct {
  uint32_t magic, build, src_hash2, vs_len, fs_len, res_size;
} tcache_hdr;

static void tcache_path(char *out, int n, const char *vs, const char *fs)
{
  uint64_t h = hash64(vs, strlen(vs)) * 31 + hash64(fs, strlen(fs));
  snprintf(out, n, "%s/t%016llx.g2c", SHADER_CACHE, (unsigned long long)h);
}

static uint32_t tcache_check(const char *vs, const char *fs)
{
  uint32_t h = fnv1a(vs, strlen(vs), 2166136261u);
  return fnv1a(fs, strlen(fs), h);
}

static uint32_t tcache_build(void)
{
  return fnv1a(g2c_build_id, strlen(g2c_build_id), 2166136261u);
}

// 1 = r is filled in (vs_cg/fs_cg malloc'd, g2c_free works on it)
static int tcache_load(const char *vs, const char *fs, g2c_result *r)
{
  char path[128];
  tcache_path(path, sizeof(path), vs, fs);
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) return 0;
  tcache_hdr h;
  int ok = 0;
  char *v = NULL, *f = NULL;
  if (sceIoRead(fd, &h, sizeof(h)) == sizeof(h) && h.magic == TCACHE_MAGIC && h.build == tcache_build() &&
      h.src_hash2 == tcache_check(vs, fs) && h.res_size == sizeof(*r) &&
      h.vs_len < 1024 * 1024 && h.fs_len < 1024 * 1024 &&
      sceIoRead(fd, r, sizeof(*r)) == sizeof(*r)) {
    v = malloc(h.vs_len + 1);
    f = malloc(h.fs_len + 1);
    if (v && f && sceIoRead(fd, v, h.vs_len) == (SceSSize)h.vs_len && sceIoRead(fd, f, h.fs_len) == (SceSSize)h.fs_len) {
      v[h.vs_len] = 0;
      f[h.fs_len] = 0;
      r->vs_cg = v;
      r->fs_cg = f;
      ok = 1;
    }
  }
  sceIoClose(fd);
  if (!ok) {
    free(v);
    free(f);
    memset(r, 0, sizeof(*r));
  }
  return ok;
}

static void tcache_store(const char *vs, const char *fs, const g2c_result *r)
{
  char path[128];
  tcache_path(path, sizeof(path), vs, fs);
  SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0) return;
  g2c_result copy = *r;
  copy.vs_cg = copy.fs_cg = NULL;  // pointers mean nothing on disk
  tcache_hdr h = { TCACHE_MAGIC, tcache_build(), tcache_check(vs, fs),
                   (uint32_t)strlen(r->vs_cg), (uint32_t)strlen(r->fs_cg), sizeof(*r) };
  sceIoWrite(fd, &h, sizeof(h));
  sceIoWrite(fd, &copy, sizeof(copy));
  sceIoWrite(fd, r->vs_cg, h.vs_len);
  sceIoWrite(fd, r->fs_cg, h.fs_len);
  sceIoClose(fd);
}

// everything after translation. small stack use unless it has to compile
// inline (async off), so the cached path can run right on the game's thread
static void link_with(link_job *lj, gl_program *p, gl_shader *vs, gl_shader *fs, g2c_result *r)
{
  char **log = &lj->log;
  link_front(p, r);

  SceGxmProgram *vg = shacc_lookup(r->vs_cg, 0);
  SceGxmProgram *fg = shacc_lookup(r->fs_cg, 1);
  if (vg && fg) {
    // cached: finish right here like before
    p->vgxp = vg;
    p->fgxp = fg;
    lj->ok = link_back(p, log);
  } else if (async_ready()) {
    compile_job *j = calloc(1, sizeof(*j));
    j->prog = p->name;
    j->vcg = strdup(r->vs_cg); j->fcg = strdup(r->fs_cg);
    j->vsrc = strdup(vs->src); j->fsrc = strdup(fs->src);
    j->vgxp = vg; j->fgxp = fg;
    p->job = j;
    p->pending = 1;
    queue_job(j);
    lj->ok = 1;  // locations are valid, draws wait for the gxps
  } else {
    free(vg); free(fg);
    p->vgxp = shacc_compile(r->vs_cg, 0, log);
    p->fgxp = p->vgxp ? shacc_compile(r->fs_cg, 1, log) : NULL;
    if (!p->vgxp || !p->fgxp) dump_fail(p, vs->src, fs->src, r->vs_cg, r->fs_cg, *log);
    else lj->ok = link_back(p, log);
  }
}

// slow path, on the big stack: translate (and cache it), then link
static void link_job_fn(void *arg)
{
  link_job *lj = arg;
  gl_program *p = lj->p;
  char **log = &lj->log;
  lj->ok = 0;

  gl_shader *vs = sh_get(p->vs), *fs = sh_get(p->fs);
  if (!vs || !fs || !vs->src || !fs->src) { log_append(log, "missing vertex or fragment shader"); return; }
  if (!gxm_ready()) { log_append(log, "gxm not initialized"); return; }

  // ~12KB, keep it off the game thread's stack
  g2c_result *r = calloc(1, sizeof(*r));
  if (!r) { log_append(log, "out of memory"); return; }
  if (g2c_translate(vs->src, fs->src, r) != 0) {
    log_append(log, "glsl2cg: %s", r->error);
    dump_fail(p, vs->src, fs->src, r->vs_cg, r->fs_cg, r->error);
    g2c_free(r);
    free(r);
    return;
  }
  tcache_store(vs->src, fs->src, r);
  link_with(lj, p, vs, fs, r);
  g2c_free(r);
  free(r);
}

void glLinkProgram(GLuint prog)
{
  gl_program *p = prog_get(prog);
  if (!p) { GL_ERR(GL_INVALID_VALUE); return; }
  prog_reset(p);
  link_job j = { p, NULL, 0 };
  p->link_tried = 1;

  // fast path: translation cached and compiles go to the worker, so nothing
  // here needs the big stack or a fresh thread
  int done = 0;
  gl_shader *vs = sh_get(p->vs), *fs = sh_get(p->fs);
  if (vs && fs && vs->src && fs->src && gxm_ready() && async_ready()) {
    g2c_result *r = calloc(1, sizeof(*r));
    if (r && tcache_load(vs->src, fs->src, r)) {
      link_with(&j, p, vs, fs, r);
      g2c_free(r);
      done = 1;
    }
    free(r);
  }
  if (!done) run_on_big_stack(link_job_fn, &j);

  char *log = j.log;
  p->linked = j.ok;
  p->log = log;
  if (!p->linked) {
    L("!!!! shader: LINK FAILED for program %u (vs %u fs %u):\n%s", prog, p->vs, p->fs, log ? log : "?");
    // drop any half-built state, keep the log
    p->log = NULL;
    prog_reset(p);
    p->log = log;
  }
}

void glUseProgram(GLuint prog)
{
  GLuint old = G.program;
  if (prog && !prog_get(prog)) { GL_ERR(GL_INVALID_VALUE); return; }
  G.program = prog;
  if (old && old != prog) {
    gl_program *o = prog_get(old);
    if (o && o->delete_pending) glDeleteProgram(old);
  }
}

void glGetProgramiv(GLuint prog, GLenum pname, GLint *out)
{
  gl_program *p = prog_get(prog);
  if (!p) { GL_ERR(GL_INVALID_VALUE); return; }
  switch (pname) {
  case GL_LINK_STATUS:
    *out = p->linked || (LIE_ABOUT_LINK_FAILURES && p->link_tried);
    break;
  case GL_VALIDATE_STATUS: *out = p->linked; break;
  case GL_DELETE_STATUS: *out = p->delete_pending; break;
  case GL_INFO_LOG_LENGTH: *out = p->log ? (GLint)strlen(p->log) + 1 : 0; break;
  case GL_ATTACHED_SHADERS: *out = (p->vs != 0) + (p->fs != 0); break;
  case GL_ACTIVE_ATTRIBUTES: *out = p->n_attribs; break;
  case GL_ACTIVE_UNIFORMS: *out = p->n_uniforms; break;
  case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
  case GL_ACTIVE_UNIFORM_MAX_LENGTH: *out = GLSL2CG_NAME_LEN; break;
  case GL_ACTIVE_UNIFORM_BLOCKS: *out = 0; break;
  case GL_PROGRAM_BINARY_LENGTH: *out = 0; break;
  default: L_ONCE("gl: glGetProgramiv 0x%X unhandled", pname); *out = 0; break;
  }
}

void glGetProgramInfoLog(GLuint prog, GLsizei max, GLsizei *len, GLchar *log)
{
  gl_program *p = prog_get(prog);
  const char *s = (p && p->log) ? p->log : "";
  GLsizei n = (GLsizei)strlen(s);
  if (max <= 0) { if (len) *len = 0; return; }
  if (n > max - 1) n = max - 1;
  if (log) { memcpy(log, s, n); log[n] = 0; }
  if (len) *len = n;
}

GLint glGetAttribLocation(GLuint prog, const GLchar *name)
{
  gl_program *p = prog_get(prog);
  if (!p || !p->linked) return -1;
  for (int i = 0; i < p->n_attribs; i++)
    if (!strcmp(p->attribs[i].name, name)) return p->attribs[i].loc;
  return -1;
}

GLint glGetUniformLocation(GLuint prog, const GLchar *name)
{
  gl_program *p = prog_get(prog);
  if (!p || !p->linked || !name) return -1;
  char base[GLSL2CG_NAME_LEN];
  int idx = 0;
  const char *br = strchr(name, '[');
  size_t n = br ? (size_t)(br - name) : strlen(name);
  if (n >= sizeof(base)) return -1;
  memcpy(base, name, n);
  base[n] = 0;
  if (br) idx = atoi(br + 1);
  for (int i = 0; i < p->n_uniforms; i++) {
    prog_uniform *u = &p->uniforms[i];
    if (strcmp(u->name, base)) continue;
    if (idx < 0 || idx >= u->elems) return -1;
    return u->loc_base + idx;
  }
  return -1;
}

// ============================================================ glUniform*

static prog_uniform *uni_lookup(GLint loc, int *elem)
{
  gl_program *p = prog_current();
  if (!p || !p->linked || loc < 0) return NULL;
  if (loc >= p->n_locs) { GL_ERR(GL_INVALID_OPERATION); return NULL; }
  prog_uniform *u = &p->uniforms[p->loc_map[loc]];
  *elem = loc - u->loc_base;
  return u;
}

static void uni_set_f(GLint loc, GLsizei count, int ncomp, const GLfloat *v)
{
  int e0;
  prog_uniform *u = uni_lookup(loc, &e0);
  if (!u) return;
  if (ncomp > 4) ncomp = 4;
  for (int i = 0; i < count && e0 + i < u->elems; i++) {
    float *d = u->data + (e0 + i) * u->cols * 4;
    for (int c = 0; c < ncomp; c++) d[c] = v[i * ncomp + c];
  }
}

static void uni_set_i(GLint loc, GLsizei count, int ncomp, const GLint *v)
{
  int e0;
  prog_uniform *u = uni_lookup(loc, &e0);
  if (!u) return;
  for (int i = 0; i < count && e0 + i < u->elems; i++) {
    float *d = u->data + (e0 + i) * u->cols * 4;
    for (int c = 0; c < ncomp; c++) d[c] = (float)v[i * ncomp + c];
  }
}

static void uni_set_mat(GLint loc, GLsizei count, int n, GLboolean tr, const GLfloat *v)
{
  int e0;
  prog_uniform *u = uni_lookup(loc, &e0);
  if (!u) return;
  if (u->cols != n) L_ONCE("gl: glUniformMatrix%dfv on '%s' (cols %d)", n, u->name, u->cols);
  for (int i = 0; i < count && e0 + i < u->elems; i++) {
    const float *m = v + i * n * n;
    for (int c = 0; c < n && c < u->cols; c++) {
      float *d = u->data + ((e0 + i) * u->cols + c) * 4;
      for (int r = 0; r < n; r++) d[r] = tr ? m[r * n + c] : m[c * n + r];
    }
  }
}

SOFTFP void glUniform1f(GLint loc, GLfloat x) { GLfloat v[1] = { x }; uni_set_f(loc, 1, 1, v); }
SOFTFP void glUniform2f(GLint loc, GLfloat x, GLfloat y) { GLfloat v[2] = { x, y }; uni_set_f(loc, 1, 2, v); }
SOFTFP void glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z) { GLfloat v[3] = { x, y, z }; uni_set_f(loc, 1, 3, v); }
SOFTFP void glUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { GLfloat v[4] = { x, y, z, w }; uni_set_f(loc, 1, 4, v); }
void glUniform1fv(GLint loc, GLsizei n, const GLfloat *v) { uni_set_f(loc, n, 1, v); }
void glUniform2fv(GLint loc, GLsizei n, const GLfloat *v) { uni_set_f(loc, n, 2, v); }
void glUniform3fv(GLint loc, GLsizei n, const GLfloat *v) { uni_set_f(loc, n, 3, v); }
void glUniform4fv(GLint loc, GLsizei n, const GLfloat *v) { uni_set_f(loc, n, 4, v); }
void glUniform1i(GLint loc, GLint x) { GLint v[1] = { x }; uni_set_i(loc, 1, 1, v); }
void glUniform2i(GLint loc, GLint x, GLint y) { GLint v[2] = { x, y }; uni_set_i(loc, 1, 2, v); }
void glUniform3i(GLint loc, GLint x, GLint y, GLint z) { GLint v[3] = { x, y, z }; uni_set_i(loc, 1, 3, v); }
void glUniform4i(GLint loc, GLint x, GLint y, GLint z, GLint w) { GLint v[4] = { x, y, z, w }; uni_set_i(loc, 1, 4, v); }
void glUniform1iv(GLint loc, GLsizei n, const GLint *v) { uni_set_i(loc, n, 1, v); }
void glUniform2iv(GLint loc, GLsizei n, const GLint *v) { uni_set_i(loc, n, 2, v); }
void glUniform3iv(GLint loc, GLsizei n, const GLint *v) { uni_set_i(loc, n, 3, v); }
void glUniform4iv(GLint loc, GLsizei n, const GLint *v) { uni_set_i(loc, n, 4, v); }
void glUniformMatrix2fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { uni_set_mat(loc, n, 2, tr, v); }
void glUniformMatrix3fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { uni_set_mat(loc, n, 3, tr, v); }
void glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) { uni_set_mat(loc, n, 4, tr, v); }

// ============================================================ upload per draw

// 32-bit words write_uniform puts at res. the cg compiler may shrink a partly
// used array, so never go past what the gxp says it has
static unsigned uni_words(const prog_uniform *u, int stage)
{
  int stride = u->st[stage].stride;
  int slots = u->slots;
  int arr = u->st[stage].arr ? (int)u->st[stage].arr : 1;
  if (stride == 4 && slots > arr) slots = arr;
  return (unsigned)((slots - 1) * stride + (stride == 4 ? 4 : u->comps));
}

static void write_uniform(void *buf, uint32_t bufsize, prog_uniform *u, int stage, GLuint prog)
{
  int stride = u->st[stage].stride;
  unsigned res = u->st[stage].res;
  int arr = u->st[stage].arr ? (int)u->st[stage].arr : 1;
  if (stride == 4 && u->slots > arr)
    L_ONCE("shader: %s (prog %u) has %d slots in GLSL but %d in the gxp, clamping", u->name, prog, u->slots, arr);
  unsigned nfl = uni_words(u, stage);
  if ((res + nfl) * 4 > bufsize) {
    L_ONCE("shader: uniform %s (prog %u) wants %u floats at %u, buffer is %u bytes, clamping",
           u->name, prog, nfl, res, bufsize);
    if (res * 4 >= bufsize) return;
    nfl = bufsize / 4 - res;
  }
  SceGxmParameterType t = u->st[stage].type;
  if (t == SCE_GXM_PARAMETER_TYPE_F32) {
    float *dst = (float *)buf + res;
    if (stride == 4) memcpy(dst, u->data, nfl * 4);
    else memcpy(dst, u->data, u->comps * 4);
  } else if (t == SCE_GXM_PARAMETER_TYPE_S32 || t == SCE_GXM_PARAMETER_TYPE_U32) {
    int32_t *dst = (int32_t *)buf + res;
    for (unsigned i = 0; i < nfl; i++) {
      unsigned slot = stride == 4 ? i / 4 : 0, c = stride == 4 ? i % 4 : i;
      dst[i] = (int32_t)u->data[slot * 4 + c];
    }
  } else {
    // halves etc: let gxm pack it
    sceGxmSetUniformDataF(buf, u->st[stage].p, 0, stride == 4 ? nfl : (unsigned)u->comps, u->data);
  }
}

void prog_upload_uniforms(gl_program *p)
{
  for (int stage = 0; stage < 2; stage++) {
    uint32_t size = stage ? p->fbuf_size : p->vbuf_size;
    if (!size) continue;
    void *buf = NULL;
    int r = stage ? sceGxmReserveFragmentDefaultUniformBuffer(gxm_ctx, &buf)
                  : sceGxmReserveVertexDefaultUniformBuffer(gxm_ctx, &buf);
    if (r < 0 || !buf) {
      L_ONCE("shader: reserve %s uniform buffer failed 0x%08X (ring too small?)", stage ? "fs" : "vs", r);
      continue;
    }
    for (int i = 0; i < p->n_uniforms; i++) {
      prog_uniform *u = &p->uniforms[i];
      if (u->is_sampler || !u->st[stage].p) continue;
      write_uniform(buf, size, u, stage, p->name);
    }
  }
}

void prog_bind_textures(gl_program *p, uint32_t scene)
{
  for (int i = 0; i < p->n_samplers; i++) {
    prog_sampler *s = &p->samplers[i];
    int unit = s->uniform >= 0 ? (int)p->uniforms[s->uniform].data[0] : 0;
    if (unit < 0 || unit >= MAX_TEX_UNITS) unit = 0;
    const SceGxmTexture *t = tex_for_unit(unit, s->cube, scene);
    int r = s->stage ? sceGxmSetFragmentTexture(gxm_ctx, s->res, t)
                     : sceGxmSetVertexTexture(gxm_ctx, s->res, t);
    if (r < 0) L_ONCE("shader: set texture slot %u failed 0x%08X", s->res, r);
  }
}
