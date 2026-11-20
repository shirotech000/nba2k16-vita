// GL fixed state, buffers, getters, and pushing state to gxm per draw.
#include <stdlib.h>
#include <string.h>
#include <kubridge.h>
#include <psp2/kernel/threadmgr.h>
#include "gles/gl_internal.h"
#include "gles/gl_api.h"

gl_state G;
static name_table g_bufs;
extern int config_int(const char *key, int def);

// ============================================================ name tables

GLuint nt_gen(name_table *t)
{
  GLuint n = ++t->next;
  if (n >= t->cap) {
    GLuint nc = t->cap ? t->cap * 2 : 256;
    while (nc <= n) nc *= 2;
    void **no = realloc(t->objs, nc * sizeof(void *));
    if (!no) { t->next--; L("gl: out of memory growing a name table"); return 0; }
    t->objs = no;
    memset(t->objs + t->cap, 0, (nc - t->cap) * sizeof(void *));
    t->cap = nc;
  }
  return n;
}

void *nt_get(name_table *t, GLuint name)
{
  return (name && name < t->cap) ? t->objs[name] : NULL;
}

void nt_set(name_table *t, GLuint name, void *obj)
{
  if (name >= t->cap) {
    // game used a name it never generated, make room
    while (t->next < name) t->next++;
    t->next--;
    nt_gen(t);
  }
  t->objs[name] = obj;
}

// ============================================================ init

void gl_state_init(void)
{
  memset(&G, 0, sizeof(G));
  G.beq_rgb = G.beq_a = GL_FUNC_ADD;
  G.bsrc_rgb = G.bsrc_a = GL_ONE;
  G.bdst_rgb = G.bdst_a = GL_ZERO;
  G.cmask[0] = G.cmask[1] = G.cmask[2] = G.cmask[3] = GL_TRUE;
  G.depth_mask = GL_TRUE;
  G.depth_func = GL_LESS;
  G.cull_face = GL_BACK;
  G.front_face = GL_CCW;
  for (int i = 0; i < 2; i++) {
    G.stencil[i].func = GL_ALWAYS;
    G.stencil[i].ref = 0;
    G.stencil[i].vmask = 0xFFFFFFFF;
    G.stencil[i].wmask = 0xFFFFFFFF;
    G.stencil[i].sfail = G.stencil[i].dpfail = G.stencil[i].dppass = GL_KEEP;
  }
  G.vp[2] = G.sc[2] = SCREEN_W;
  G.vp[3] = G.sc[3] = SCREEN_H;
  G.clear_depth = 1.0f;
  G.unpack_align = G.pack_align = 4;
  for (int i = 0; i < MAX_ATTRIBS; i++) {
    G.va[i].size = 4;
    G.va[i].type = GL_FLOAT;
  }
  G.dirty = 1;
  L("gl: state initialized");
}

// ============================================================ caps

static int *cap_ptr(GLenum cap)
{
  switch (cap) {
  case GL_BLEND: return &G.blend;
  case GL_DEPTH_TEST: return &G.depth_test;
  case GL_STENCIL_TEST: return &G.stencil_test;
  case GL_CULL_FACE: return &G.cull;
  case GL_SCISSOR_TEST: return &G.scissor_test;
  case GL_POLYGON_OFFSET_FILL: return &G.poly_offset;
  case GL_RASTERIZER_DISCARD: return &G.rast_discard;
  default: return NULL;
  }
}

static void set_cap(GLenum cap, int on)
{
  int *p = cap_ptr(cap);
  if (p) { *p = on; G.dirty = 1; return; }
  switch (cap) {
  case GL_DITHER:
  case GL_PRIMITIVE_RESTART_FIXED_INDEX:
    break;
  case GL_SAMPLE_ALPHA_TO_COVERAGE:
  case GL_SAMPLE_COVERAGE:
    if (on) L_ONCE("gl: alpha to coverage / sample coverage not supported (0x%X)", cap);
    break;
  default:
    L_ONCE("gl: glEnable/glDisable unknown cap 0x%X", cap);
    GL_ERR(GL_INVALID_ENUM);
    break;
  }
}

void glEnable(GLenum cap) { set_cap(cap, 1); }
void glDisable(GLenum cap) { set_cap(cap, 0); }

GLboolean glIsEnabled(GLenum cap)
{
  int *p = cap_ptr(cap);
  return p ? (GLboolean)*p : GL_FALSE;
}

// ============================================================ blend / masks / depth / stencil

void glBlendEquation(GLenum m) { G.beq_rgb = G.beq_a = m; }
void glBlendEquationSeparate(GLenum rgb, GLenum a) { G.beq_rgb = rgb; G.beq_a = a; }
void glBlendFunc(GLenum s, GLenum d) { G.bsrc_rgb = G.bsrc_a = s; G.bdst_rgb = G.bdst_a = d; }
void glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da)
{
  G.bsrc_rgb = sr; G.bdst_rgb = dr; G.bsrc_a = sa; G.bdst_a = da;
}

void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
  G.cmask[0] = r; G.cmask[1] = g; G.cmask[2] = b; G.cmask[3] = a;
}

void glDepthFunc(GLenum f) { G.depth_func = f; G.dirty = 1; }
void glDepthMask(GLboolean m) { G.depth_mask = m; G.dirty = 1; }
void glCullFace(GLenum m) { G.cull_face = m; G.dirty = 1; }
void glFrontFace(GLenum m) { G.front_face = m; G.dirty = 1; }

SOFTFP void glPolygonOffset(GLfloat factor, GLfloat units)
{
  G.poly_factor = factor;
  G.poly_units = units;
  G.dirty = 1;
}

static void stencil_faces(GLenum face, int *f0, int *f1)
{
  *f0 = (face == GL_FRONT || face == GL_FRONT_AND_BACK) ? 1 : 0;
  *f1 = (face == GL_BACK || face == GL_FRONT_AND_BACK) ? 1 : 0;
}

void glStencilFuncSeparate(GLenum face, GLenum f, GLint ref, GLuint mask)
{
  int a, b;
  stencil_faces(face, &a, &b);
  for (int i = 0; i < 2; i++) {
    if (!(i ? b : a)) continue;
    G.stencil[i].func = f;
    G.stencil[i].ref = ref;
    G.stencil[i].vmask = mask;
  }
  G.dirty = 1;
}
void glStencilFunc(GLenum f, GLint ref, GLuint mask) { glStencilFuncSeparate(GL_FRONT_AND_BACK, f, ref, mask); }

void glStencilMaskSeparate(GLenum face, GLuint m)
{
  int a, b;
  stencil_faces(face, &a, &b);
  if (a) G.stencil[0].wmask = m;
  if (b) G.stencil[1].wmask = m;
  G.dirty = 1;
}
void glStencilMask(GLuint m) { glStencilMaskSeparate(GL_FRONT_AND_BACK, m); }

void glStencilOpSeparate(GLenum face, GLenum sf, GLenum dpf, GLenum dpp)
{
  int a, b;
  stencil_faces(face, &a, &b);
  for (int i = 0; i < 2; i++) {
    if (!(i ? b : a)) continue;
    G.stencil[i].sfail = sf;
    G.stencil[i].dpfail = dpf;
    G.stencil[i].dppass = dpp;
  }
  G.dirty = 1;
}
void glStencilOp(GLenum sf, GLenum dpf, GLenum dpp) { glStencilOpSeparate(GL_FRONT_AND_BACK, sf, dpf, dpp); }

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
  G.vp[0] = x; G.vp[1] = y; G.vp[2] = w; G.vp[3] = h;
  G.dirty = 1;
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
  G.sc[0] = x; G.sc[1] = y; G.sc[2] = w; G.sc[3] = h;
  G.dirty = 1;
}

SOFTFP void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
  G.clear_color[0] = r; G.clear_color[1] = g; G.clear_color[2] = b; G.clear_color[3] = a;
}
SOFTFP void glClearDepthf(GLfloat d) { G.clear_depth = d < 0 ? 0 : d > 1 ? 1 : d; }
void glClearStencil(GLint s) { G.clear_stencil = s; }

void glPixelStorei(GLenum pname, GLint v)
{
  switch (pname) {
  case GL_UNPACK_ALIGNMENT: G.unpack_align = v; break;
  case GL_PACK_ALIGNMENT: G.pack_align = v; break;
  case GL_UNPACK_ROW_LENGTH: G.unpack_row_len = v; break;
  default:
    if (v) L_ONCE("gl: glPixelStorei 0x%X = %d ignored", pname, v);
    break;
  }
}

void glHint(GLenum target, GLenum mode) { (void)target; (void)mode; }
SOFTFP void glLineWidth(GLfloat w) { if (w != 1.0f) L_ONCE("gl: glLineWidth(%f) ignored", (double)w); }
SOFTFP void glDepthRangef(GLfloat n, GLfloat f)
{
  if (n != 0.0f || f != 1.0f) L_ONCE("gl: glDepthRangef(%f, %f) ignored, fixed at 0..1", (double)n, (double)f);
}

void glFlush(void) { }
void glFinish(void) { gxm_finish(); }

// ============================================================ buffers

gl_buffer *buf_get(GLuint name) { return nt_get(&g_bufs, name); }

static gl_buffer *buf_make(GLuint name)
{
  gl_buffer *b = calloc(1, sizeof(*b));
  b->name = name;
  nt_set(&g_bufs, name, b);
  return b;
}

void glGenBuffers(GLsizei n, GLuint *out)
{
  for (int i = 0; i < n; i++) {
    out[i] = nt_gen(&g_bufs);
    buf_make(out[i]);
  }
}

void glBindBuffer(GLenum target, GLuint name)
{
  if (name && !buf_get(name)) buf_make(name);
  switch (target) {
  case GL_ARRAY_BUFFER: G.array_buffer = name; break;
  case GL_ELEMENT_ARRAY_BUFFER: G.element_buffer = name; break;
  default: L_ONCE("gl: glBindBuffer target 0x%X not supported", target); break;
  }
}

static gl_buffer *bound_buf(GLenum target)
{
  GLuint n = target == GL_ARRAY_BUFFER ? G.array_buffer
           : target == GL_ELEMENT_ARRAY_BUFFER ? G.element_buffer : 0;
  return buf_get(n);
}

// ---- zero-copy vbos ----
// the game uploads its vertex/index data straight from blocks it keeps for
// the whole scene (VCDisplayList, glBufferData @+0x1304f98 / +0x130573c,
// see the zero-copy analysis). from those two sites the buffer just points
// at the game's block (gpu-mapped) instead of copying it into the pool,
// which leaves the pool for textures. the game never fences, so freeing a
// block a live zero-copy buffer uses waits for the gpu first (libc free).
#include "loader/so_loader.h"
#define ZC_MAX 4096
static struct { uintptr_t lo, hi; } zc_ranges[ZC_MAX];
static int zc_n;
static volatile uintptr_t zc_lo = ~(uintptr_t)0, zc_hi;
static SceKernelLwMutexWork zc_lock;
static int zc_on = -1;

static int zc_site(uintptr_t lr)
{
  so_module *m = so_module_for_addr(lr);
  if (!m) return 0;
  uintptr_t off = lr - (uintptr_t)m->base;
  return (off >= 0x1304f98 && off <= 0x1304fa0) || (off >= 0x130573c && off <= 0x1305744);
}

static void zc_add(uintptr_t lo, uintptr_t hi)
{
  sceKernelLockLwMutex(&zc_lock, 1, NULL);
  if (zc_n < ZC_MAX) {
    zc_ranges[zc_n].lo = lo;
    zc_ranges[zc_n].hi = hi;
    zc_n++;
    if (lo < zc_lo) zc_lo = lo;
    if (hi > zc_hi) zc_hi = hi;
  }
  sceKernelUnlockLwMutex(&zc_lock, 1);
}

static void zc_remove(uintptr_t lo)
{
  sceKernelLockLwMutex(&zc_lock, 1, NULL);
  for (int i = 0; i < zc_n; i++)
    if (zc_ranges[i].lo == lo) { zc_ranges[i] = zc_ranges[--zc_n]; break; }
  sceKernelUnlockLwMutex(&zc_lock, 1);
}

int gl_zc_any(void) { return zc_on > 0 && zc_n > 0; }

// libc's free calls this: is the gpu maybe reading [p, p+n)?
int gl_zc_overlaps(const void *p, size_t n)
{
  uintptr_t a = (uintptr_t)p, e = a + n;
  if (zc_on <= 0 || !zc_n || e <= zc_lo || a >= zc_hi) return 0;
  int r = 0;
  sceKernelLockLwMutex(&zc_lock, 1, NULL);
  for (int i = 0; i < zc_n && !r; i++) r = a < zc_ranges[i].hi && e > zc_ranges[i].lo;
  sceKernelUnlockLwMutex(&zc_lock, 1);
  return r;
}

static void buf_release(gl_buffer *b)
{
  if (!b->mem) return;
  if (b->mem->zc) zc_remove((uintptr_t)b->mem->ptr);
  gpu_free_deferred(b->mem);
  b->mem = NULL;
}

void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
  uintptr_t lr = (uintptr_t)__builtin_return_address(0);
  gl_buffer *b = bound_buf(target);
  if (!b || size < 0) { GL_ERR(GL_INVALID_OPERATION); return; }
  if (zc_on < 0) {
    extern int config_int(const char *key, int def);
    zc_on = config_int("zero_copy", 1);
    sceKernelCreateLwMutex(&zc_lock, "zc", 0, 0, NULL);
    L("gl: zero-copy vbos %s (zero_copy=0 turns it off)", zc_on ? "on" : "off");
  }
  // zero-copy buffers never get reused for other data
  if (b->mem && b->mem->zc) buf_release(b);
  // orphan if the gpu may still read it, or the size is way off
  if (b->mem && ((uint32_t)size > b->mem->size || (uint32_t)size * 2 < b->mem->size ||
                 gxm_busy(b->last_use))) {
    gpu_free_deferred(b->mem);
    b->mem = NULL;
  }
  extern int bigmem_gpu_readable(const void *p, size_t n);
  if (zc_on > 0 && data && size > 0 && !((uintptr_t)data & 3) && zc_site(lr) &&
      bigmem_gpu_readable(data, (size_t)size)) {
    gpu_block *zb = calloc(1, sizeof(*zb));
    if (zb) {
      if (b->mem) { gpu_free_deferred(b->mem); b->mem = NULL; }
      zb->ptr = (void *)data;
      zb->size = (uint32_t)size;
      zb->zc = 1;
      kuKernelFlushCaches((void *)data, (uint32_t)size);
      zc_add((uintptr_t)data, (uintptr_t)data + (uint32_t)size);
      b->mem = zb;
      b->size = (uint32_t)size;
      b->usage = usage;
      b->last_use = 0;
      static unsigned zc_count, zc_bytes;
      zc_count++;
      zc_bytes += (uint32_t)size;
      if ((zc_count & 63) == 1)
        L("gl: zero-copy vbo #%u (%u KiB not copied so far)", zc_count, zc_bytes >> 10);
      return;
    }
  }
  free(b->shadow);
  b->shadow = NULL;
  if (!b->mem && size) {
    int pref = usage == GL_STATIC_DRAW ? GPU_PREF_CDRAM : GPU_PREF_LPDDR;
    b->mem = gpu_alloc((uint32_t)size, 16, pref);
    if (!b->mem) {
      // gpu pool's full (hw match load: heads + crowd vbos died here and their
      // draws got dropped). cpu memory mapped for the gpu instead
      b->mem = gpu_alloc_mapped((uint32_t)size);
      if (!b->mem) { GL_ERR(GL_OUT_OF_MEMORY); b->size = 0; return; }
      static int n;
      if (n++ < 8) L("gl: buffer %u (%u bytes) has no pool room, using mapped cpu memory", b->name, (unsigned)size);
    }
    b->last_use = 0;
  }
  b->size = (uint32_t)size;
  b->usage = usage;
  if (data && size) {
    memcpy(b->mem->ptr, data, size);
    gpu_mapped_flush(b->mem, 0, (uint32_t)size);
  }
}

void glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data)
{
  gl_buffer *b = bound_buf(target);
  if (!b || (!b->mem && !b->shadow) || off < 0 || size < 0 || (uint32_t)(off + size) > b->size) {
    GL_ERR(GL_INVALID_VALUE);
    return;
  }
  if (b->mem && b->mem->zc) {
    // morph re-uploads come from the same block, usually in place
    uint8_t *dst = (uint8_t *)b->mem->ptr + off;
    if ((const void *)dst != data) memmove(dst, data, size);
    kuKernelFlushCaches(dst, (uint32_t)size);
    return;
  }
  if (!b->mem) { memcpy((uint8_t *)b->shadow + off, data, size); return; }
  if (gxm_busy(b->last_use)) {
    // gpu still has the old contents queued, copy-on-write
    static uint32_t cow_count;
    if ((cow_count++ & 1023) == 0)
      L("gl: glBufferSubData copy-on-write #%u (buffer %u, %u bytes)", cow_count, b->name, b->size);
    gpu_block *nb = gpu_alloc(b->mem->size, 16, gpu_is_cdram(b->mem) ? GPU_PREF_CDRAM : GPU_PREF_LPDDR);
    if (!nb) { GL_ERR(GL_OUT_OF_MEMORY); return; }
    if (off > 0) memcpy(nb->ptr, b->mem->ptr, off);
    uint32_t end = (uint32_t)(off + size);
    if (end < b->size) memcpy((uint8_t *)nb->ptr + end, (uint8_t *)b->mem->ptr + end, b->size - end);
    gpu_free_deferred(b->mem);
    b->mem = nb;
    b->last_use = 0;
  }
  memcpy((uint8_t *)b->mem->ptr + off, data, size);
  gpu_mapped_flush(b->mem, (uint32_t)off, (uint32_t)size);
}

void glDeleteBuffers(GLsizei n, const GLuint *names)
{
  for (int i = 0; i < n; i++) {
    gl_buffer *b = buf_get(names[i]);
    if (!b) continue;
    if (G.array_buffer == names[i]) G.array_buffer = 0;
    if (G.element_buffer == names[i]) G.element_buffer = 0;
    for (int a = 0; a < MAX_ATTRIBS; a++)
      if (G.va[a].buffer == names[i]) G.va[a].buffer = (GLuint)-1;  // dangling, draw skips it
    if (b->mem && b->mem->zc) zc_remove((uintptr_t)b->mem->ptr);
    gpu_free_deferred(b->mem);
    nt_set(&g_bufs, names[i], NULL);
    free(b->shadow);
    free(b);
  }
}

// ============================================================ vertex attribs

void glVertexAttribPointer(GLuint idx, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr)
{
  if (idx >= MAX_ATTRIBS) { GL_ERR(GL_INVALID_VALUE); return; }
  gl_vattrib *a = &G.va[idx];
  a->size = size;
  a->type = type;
  a->norm = norm;
  a->stride = stride;
  a->ptr = ptr;
  a->buffer = G.array_buffer;
}

void glEnableVertexAttribArray(GLuint idx) { if (idx < MAX_ATTRIBS) G.va[idx].enabled = 1; }
void glDisableVertexAttribArray(GLuint idx) { if (idx < MAX_ATTRIBS) G.va[idx].enabled = 0; }

// ============================================================ getters

GLenum glGetError(void)
{
  GLenum e = G.err;
  G.err = GL_NO_ERROR;
  return e;
}

static const char *g_ext =
  "GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_texture_npot "
  "GL_OES_element_index_uint GL_OES_compressed_ETC1_RGB8_texture";

static const GLenum g_comp_fmts[] = {
  GL_ETC1_RGB8_OES, GL_COMPRESSED_RGB8_ETC2, GL_COMPRESSED_SRGB8_ETC2,
  GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2,
  GL_COMPRESSED_RGBA8_ETC2_EAC, GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC,
  GL_COMPRESSED_R11_EAC, GL_COMPRESSED_SIGNED_R11_EAC, GL_COMPRESSED_RG11_EAC, GL_COMPRESSED_SIGNED_RG11_EAC,
};

const GLubyte *glGetString(GLenum name)
{
  const char *s;
  switch (name) {
  case GL_VENDOR: s = "Sony Computer Entertainment"; break;
  case GL_RENDERER: s = "PowerVR SGX543MP4+ (sceGxm)"; break;
  case GL_VERSION: s = "OpenGL ES 3.0"; break;
  case GL_SHADING_LANGUAGE_VERSION: s = "OpenGL ES GLSL ES 3.00"; break;
  case GL_EXTENSIONS: s = g_ext; break;
  default: GL_ERR(GL_INVALID_ENUM); return NULL;
  }
  L("gl: glGetString(0x%X) -> \"%s\"", name, s);
  return (const GLubyte *)s;
}

static int count_ext(void)
{
  int n = 1;
  for (const char *p = g_ext; *p; p++) n += *p == ' ';
  return n;
}

void glGetIntegerv(GLenum pname, GLint *out)
{
  switch (pname) {
  case GL_VIEWPORT: memcpy(out, G.vp, sizeof(G.vp)); return;
  case GL_SCISSOR_BOX: memcpy(out, G.sc, sizeof(G.sc)); return;
  case GL_MAX_VIEWPORT_DIMS: out[0] = out[1] = 4096; return;
  case GL_ALIASED_POINT_SIZE_RANGE: out[0] = 1; out[1] = 64; return;
  case GL_ALIASED_LINE_WIDTH_RANGE: out[0] = 1; out[1] = 1; return;
  case GL_COLOR_WRITEMASK: for (int i = 0; i < 4; i++) out[i] = G.cmask[i]; return;
  case GL_COMPRESSED_TEXTURE_FORMATS:
    for (unsigned i = 0; i < ARRAY_LEN(g_comp_fmts); i++) out[i] = g_comp_fmts[i];
    return;
  default: break;
  }
  GLint v;
  switch (pname) {
  case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: case GL_MAX_CUBE_MAP_TEXTURE_SIZE: v = 4096; break;
  case GL_MAX_3D_TEXTURE_SIZE: case GL_MAX_ARRAY_TEXTURE_LAYERS: v = 256; break;
  case GL_MAX_VERTEX_ATTRIBS: v = MAX_ATTRIBS; break;
  case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: v = MAX_TEX_UNITS; break;
  case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: v = 4; break;
  case GL_MAX_VERTEX_UNIFORM_VECTORS: v = 256; break;
  case GL_MAX_FRAGMENT_UNIFORM_VECTORS: v = 224; break;
  case GL_MAX_VERTEX_UNIFORM_COMPONENTS: v = 1024; break;
  case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS: v = 896; break;
  case GL_MAX_VARYING_VECTORS: v = 10; break;
  case GL_MAX_VARYING_COMPONENTS: case GL_MAX_VERTEX_OUTPUT_COMPONENTS:
  case GL_MAX_FRAGMENT_INPUT_COMPONENTS: v = 40; break;
  case GL_MAX_SAMPLES: v = 4; break;   // accepted, rendered without msaa
  case GL_SAMPLES: case GL_SAMPLE_BUFFERS: v = 0; break;
  case GL_MAX_COLOR_ATTACHMENTS: case GL_MAX_DRAW_BUFFERS: v = 1; break;
  case GL_MAX_ELEMENTS_VERTICES: case GL_MAX_ELEMENTS_INDICES: v = 65536; break;
  case GL_MAX_ELEMENT_INDEX: v = 0x7FFFFFFF; break;
  case GL_MAX_TEXTURE_LOD_BIAS: v = 15; break;
  case GL_MAX_UNIFORM_BUFFER_BINDINGS: case GL_MAX_VERTEX_UNIFORM_BLOCKS:
  case GL_MAX_FRAGMENT_UNIFORM_BLOCKS: case GL_MAX_COMBINED_UNIFORM_BLOCKS: v = 0; break;
  case GL_MAX_UNIFORM_BLOCK_SIZE: v = 16384; break;
  case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: v = 16; break;
  case GL_MAX_PROGRAM_TEXEL_OFFSET: v = 7; break;
  case GL_MIN_PROGRAM_TEXEL_OFFSET: v = -8; break;
  case GL_SUBPIXEL_BITS: v = 4; break;
  case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: v = 8; break;
  case GL_DEPTH_BITS: v = 24; break;
  case GL_STENCIL_BITS: v = 8; break;
  case GL_NUM_COMPRESSED_TEXTURE_FORMATS: v = ARRAY_LEN(g_comp_fmts); break;
  case GL_NUM_SHADER_BINARY_FORMATS: case GL_NUM_PROGRAM_BINARY_FORMATS: v = 0; break;
  case GL_NUM_EXTENSIONS: v = count_ext(); break;
  case GL_MAJOR_VERSION: v = 3; break;
  case GL_MINOR_VERSION: v = 0; break;
  case GL_FRAMEBUFFER_BINDING: v = G.draw_fbo; break;
  case GL_RENDERBUFFER_BINDING: v = G.renderbuffer; break;
  case GL_TEXTURE_BINDING_2D: v = G.tex2d[G.active_tex]; break;
  case GL_TEXTURE_BINDING_CUBE_MAP: v = G.texcube[G.active_tex]; break;
  case GL_ARRAY_BUFFER_BINDING: v = G.array_buffer; break;
  case GL_ELEMENT_ARRAY_BUFFER_BINDING: v = G.element_buffer; break;
  case GL_CURRENT_PROGRAM: v = G.program; break;
  case GL_ACTIVE_TEXTURE: v = GL_TEXTURE0 + G.active_tex; break;
  case GL_UNPACK_ALIGNMENT: v = G.unpack_align; break;
  case GL_PACK_ALIGNMENT: v = G.pack_align; break;
  case GL_UNPACK_ROW_LENGTH: v = G.unpack_row_len; break;
  case GL_IMPLEMENTATION_COLOR_READ_TYPE: v = GL_UNSIGNED_BYTE; break;
  case GL_IMPLEMENTATION_COLOR_READ_FORMAT: v = GL_RGBA; break;
  case GL_DEPTH_WRITEMASK: v = G.depth_mask; break;
  case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS: v = 0; break;
  case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: v = 1; break;
  default:
    L("gl: glGetIntegerv(0x%X) unknown, returning 0", pname);
    v = 0;
    break;
  }
  *out = v;
}

void glGetFloatv(GLenum pname, GLfloat *out)
{
  GLint tmp[16] = { 0 };
  int n = 1;
  if (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX || pname == GL_COLOR_WRITEMASK) n = 4;
  else if (pname == GL_ALIASED_POINT_SIZE_RANGE || pname == GL_ALIASED_LINE_WIDTH_RANGE ||
           pname == GL_MAX_VIEWPORT_DIMS) n = 2;
  else if (pname == GL_LINE_WIDTH) { *out = 1.0f; return; }
  glGetIntegerv(pname, tmp);
  for (int i = 0; i < n; i++) out[i] = (GLfloat)tmp[i];
}

void glGetBooleanv(GLenum pname, GLboolean *out)
{
  GLint tmp[16] = { 0 };
  int n = (pname == GL_COLOR_WRITEMASK) ? 4 : 1;
  int *cap = cap_ptr(pname);
  if (cap) { *out = (GLboolean)*cap; return; }
  glGetIntegerv(pname, tmp);
  for (int i = 0; i < n; i++) out[i] = tmp[i] ? GL_TRUE : GL_FALSE;
}

// ============================================================ state -> gxm

static SceGxmDepthFunc map_depth(GLenum f)
{
  switch (f) {
  case GL_NEVER: return SCE_GXM_DEPTH_FUNC_NEVER;
  case GL_LESS: return SCE_GXM_DEPTH_FUNC_LESS;
  case GL_EQUAL: return SCE_GXM_DEPTH_FUNC_EQUAL;
  case GL_LEQUAL: return SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
  case GL_GREATER: return SCE_GXM_DEPTH_FUNC_GREATER;
  case GL_NOTEQUAL: return SCE_GXM_DEPTH_FUNC_NOT_EQUAL;
  case GL_GEQUAL: return SCE_GXM_DEPTH_FUNC_GREATER_EQUAL;
  default: return SCE_GXM_DEPTH_FUNC_ALWAYS;
  }
}

static SceGxmStencilFunc map_sfunc(GLenum f)
{
  switch (f) {
  case GL_NEVER: return SCE_GXM_STENCIL_FUNC_NEVER;
  case GL_LESS: return SCE_GXM_STENCIL_FUNC_LESS;
  case GL_EQUAL: return SCE_GXM_STENCIL_FUNC_EQUAL;
  case GL_LEQUAL: return SCE_GXM_STENCIL_FUNC_LESS_EQUAL;
  case GL_GREATER: return SCE_GXM_STENCIL_FUNC_GREATER;
  case GL_NOTEQUAL: return SCE_GXM_STENCIL_FUNC_NOT_EQUAL;
  case GL_GEQUAL: return SCE_GXM_STENCIL_FUNC_GREATER_EQUAL;
  default: return SCE_GXM_STENCIL_FUNC_ALWAYS;
  }
}

static SceGxmStencilOp map_sop(GLenum op)
{
  switch (op) {
  case GL_ZERO: return SCE_GXM_STENCIL_OP_ZERO;
  case GL_REPLACE: return SCE_GXM_STENCIL_OP_REPLACE;
  case GL_INCR: return SCE_GXM_STENCIL_OP_INCR;
  case GL_DECR: return SCE_GXM_STENCIL_OP_DECR;
  case GL_INVERT: return SCE_GXM_STENCIL_OP_INVERT;
  case GL_INCR_WRAP: return SCE_GXM_STENCIL_OP_INCR_WRAP;
  case GL_DECR_WRAP: return SCE_GXM_STENCIL_OP_DECR_WRAP;
  default: return SCE_GXM_STENCIL_OP_KEEP;
  }
}

static SceGxmBlendFunc map_beq(GLenum e)
{
  switch (e) {
  case GL_FUNC_SUBTRACT: return SCE_GXM_BLEND_FUNC_SUBTRACT;
  case GL_FUNC_REVERSE_SUBTRACT: return SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
  case GL_MIN: return SCE_GXM_BLEND_FUNC_MIN;
  case GL_MAX: return SCE_GXM_BLEND_FUNC_MAX;
  default: return SCE_GXM_BLEND_FUNC_ADD;
  }
}

static SceGxmBlendFactor map_bf(GLenum f)
{
  switch (f) {
  case GL_ZERO: return SCE_GXM_BLEND_FACTOR_ZERO;
  case GL_ONE: return SCE_GXM_BLEND_FACTOR_ONE;
  case GL_SRC_COLOR: return SCE_GXM_BLEND_FACTOR_SRC_COLOR;
  case GL_ONE_MINUS_SRC_COLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  case GL_SRC_ALPHA: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
  case GL_ONE_MINUS_SRC_ALPHA: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case GL_DST_COLOR: return SCE_GXM_BLEND_FACTOR_DST_COLOR;
  case GL_ONE_MINUS_DST_COLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  case GL_DST_ALPHA: return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
  case GL_ONE_MINUS_DST_ALPHA: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  case GL_SRC_ALPHA_SATURATE: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
  default:
    L_ONCE("gl: blend factor 0x%X (constant color?) not supported by gxm, using ONE", f);
    return SCE_GXM_BLEND_FACTOR_ONE;
  }
}

uint8_t gl_gxm_color_mask(void)
{
  uint8_t m = 0;
  if (G.cmask[0]) m |= SCE_GXM_COLOR_MASK_R;
  if (G.cmask[1]) m |= SCE_GXM_COLOR_MASK_G;
  if (G.cmask[2]) m |= SCE_GXM_COLOR_MASK_B;
  if (G.cmask[3]) m |= SCE_GXM_COLOR_MASK_A;
  return m;
}

void gl_blend_info(SceGxmBlendInfo *bi)
{
  memset(bi, 0, sizeof(*bi));
  bi->colorMask = gl_gxm_color_mask();
  if (!G.blend) {
    bi->colorFunc = bi->alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
    bi->colorSrc = bi->alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    bi->colorDst = bi->alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
    return;
  }
  bi->colorFunc = map_beq(G.beq_rgb);
  bi->alphaFunc = map_beq(G.beq_a);
  bi->colorSrc = map_bf(G.bsrc_rgb);
  bi->colorDst = map_bf(G.bdst_rgb);
  bi->alphaSrc = map_bf(G.bsrc_a);
  bi->alphaDst = map_bf(G.bdst_a);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// GL window coords are y-up. on the display we flip (negative y scale) so the
// image is upright; fbos are drawn unflipped so texture row 0 is GL's bottom
// row, like GL expects when sampling. culling winding flips with it.
// prim: 0 tris, 1 lines, 2 points. returns 0 if the draw can be skipped.
// scissor -> region clip (inclusive max). 0 = nothing visible
int gl_set_region_clip(void)
{
  int x0 = 0, y0 = 0, x1 = (int)T.w - 1, y1 = (int)T.h - 1;
  if (G.scissor_test) {
    int sx0 = G.sc[0], sx1 = G.sc[0] + G.sc[2] - 1;
    int sy0, sy1;
    if (T.flip) { sy0 = (int)T.h - (G.sc[1] + G.sc[3]); sy1 = (int)T.h - G.sc[1] - 1; }
    else { sy0 = G.sc[1]; sy1 = G.sc[1] + G.sc[3] - 1; }
    if (sx1 < sx0 || sy1 < sy0 || sx1 < 0 || sy1 < 0 || sx0 >= (int)T.w || sy0 >= (int)T.h) return 0;
    x0 = clampi(sx0, 0, T.w - 1); x1 = clampi(sx1, 0, T.w - 1);
    y0 = clampi(sy0, 0, T.h - 1); y1 = clampi(sy1, 0, T.h - 1);
  }
  sceGxmSetRegionClip(gxm_ctx, SCE_GXM_REGION_CLIP_OUTSIDE, x0, y0, x1, y1);
  return 1;
}

int gl_apply_state(int prim)
{
  SceGxmContext *c = gxm_ctx;
  float w = (float)G.vp[2], h = (float)G.vp[3];
  float xs = w * 0.5f, xo = (float)G.vp[0] + xs;
  float ys, yo;
  if (T.flip) { ys = -h * 0.5f; yo = (float)T.h - (float)G.vp[1] - h * 0.5f; }
  else { ys = h * 0.5f; yo = (float)G.vp[1] + h * 0.5f; }
  sceGxmSetViewport(c, xo, xs, yo, ys, 0.5f, 0.5f);

  if (!gl_set_region_clip()) return 0;

  // depth. GL: depth test off means no depth writes either
  int dt = G.depth_test && T.has_depth;
  SceGxmDepthFunc df = dt ? map_depth(G.depth_func) : SCE_GXM_DEPTH_FUNC_ALWAYS;
  SceGxmDepthWriteMode dw = (dt && G.depth_mask) ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
  sceGxmSetFrontDepthFunc(c, df);
  sceGxmSetBackDepthFunc(c, df);
  sceGxmSetFrontDepthWriteEnable(c, dw);
  sceGxmSetBackDepthWriteEnable(c, dw);

  // winding test knobs (config.txt), both unverified on hw:
  //   fbo_cull_flip=0  don't mirror winding on fbos (if gxm judges facing pre-viewport)
  //   stencil_swap=1   swap gxm front/back stencil (if gxm "front" = cw on screen)
  static int k_fbo_flip = -1, k_sswap;
  if (k_fbo_flip < 0) {
    k_fbo_flip = config_int("fbo_cull_flip", 1) ? 1 : 0;
    k_sswap = config_int("stencil_swap", 0) ? 1 : 0;
    if (!k_fbo_flip || k_sswap) L("gl: winding knobs fbo_cull_flip=%d stencil_swap=%d", k_fbo_flip, k_sswap);
  }
  int mirrored = !T.flip && k_fbo_flip;

  // stencil. assumption: gxm "front" = ccw on screen (y down). no stencil
  // attachment in GL (D16 fbo) = test always passes, gxm's plane is junk there
  int st = G.stencil_test && T.has_stencil;
  if (G.stencil_test && !T.has_stencil && T.has_depth)
    L_ONCE("gl: stencil test on a depth-only fbo (%ux%u), treated as always-pass", T.w, T.h);
  int swap = (G.front_face == GL_CW) ^ mirrored ^ k_sswap;
  for (int side = 0; side < 2; side++) {
    const gl_stencil_side *s = &G.stencil[side ^ swap];
    void (*fn)(SceGxmContext *, SceGxmStencilFunc, SceGxmStencilOp, SceGxmStencilOp, SceGxmStencilOp,
               unsigned char, unsigned char) = side ? sceGxmSetBackStencilFunc : sceGxmSetFrontStencilFunc;
    if (st) {
      fn(c, map_sfunc(s->func), map_sop(s->sfail), map_sop(s->dpfail), map_sop(s->dppass),
         s->vmask & 0xFF, s->wmask & 0xFF);
    } else {
      fn(c, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
         SCE_GXM_STENCIL_OP_KEEP, 0xFF, 0);
    }
    if (side) sceGxmSetBackStencilRef(c, s->ref & 0xFF);
    else sceGxmSetFrontStencilRef(c, s->ref & 0xFF);
  }
  int two_sided = st && memcmp(&G.stencil[0], &G.stencil[1], sizeof(gl_stencil_side)) != 0;
  if (two_sided) L_ONCE("gl: two-sided stencil in use (front/back mapping unverified)");
  sceGxmSetTwoSidedEnable(c, two_sided ? SCE_GXM_TWO_SIDED_ENABLED : SCE_GXM_TWO_SIDED_DISABLED);

  // cull. on the display gxm winding == what you see; fbos are mirrored
  SceGxmCullMode cm = SCE_GXM_CULL_NONE;
  if (G.cull && prim == 0) {
    if (G.cull_face == GL_FRONT_AND_BACK) return 0;
    int front_ccw = G.front_face != GL_CW;
    int cull_ccw = (G.cull_face == GL_FRONT) ? front_ccw : !front_ccw;
    if (mirrored) cull_ccw = !cull_ccw;
    cm = cull_ccw ? SCE_GXM_CULL_CCW : SCE_GXM_CULL_CW;
  }
  sceGxmSetCullMode(c, cm);

  int fac = 0, uni = 0;
  if (G.poly_offset && prim == 0) { fac = (int)G.poly_factor; uni = (int)G.poly_units; }
  sceGxmSetFrontDepthBias(c, fac, uni);
  sceGxmSetBackDepthBias(c, fac, uni);

  SceGxmPolygonMode pm = prim == 1 ? SCE_GXM_POLYGON_MODE_LINE
                       : prim == 2 ? SCE_GXM_POLYGON_MODE_POINT_10UV
                       : SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
  sceGxmSetFrontPolygonMode(c, pm);
  sceGxmSetBackPolygonMode(c, pm);
  sceGxmSetFrontFragmentProgramEnable(c, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
  sceGxmSetBackFragmentProgramEnable(c, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
  G.dirty = 0;
  return 1;
}

