// draws + clear. one gxm stream per attribute (offset 0, pointer set per draw)
// so attribute offsets never end up in the vertex program cache key.
#include <stdlib.h>
#include <string.h>
#include "gles/gl_internal.h"
#include "gles/gl_api.h"

#define SEQ16_COUNT 64000   // gxm wants 16-bit stream indices below 64000

static gpu_block *g_seq16;   // 0..63999 for glDrawArrays
static gpu_block *g_static;  // clear quad + its indices + the constant attrib
static float *g_quad;
static uint16_t *g_quad_idx;
static float *g_const_attr;

// -1 = untested, 1 = gxm takes stride 0 streams, 0 = use the instance trick
static int g_stride0_ok = -1;

static void draw_init(void)
{
  if (g_seq16) return;
  g_seq16 = gpu_alloc(SEQ16_COUNT * 2, 16, GPU_PREF_CDRAM);
  g_static = gpu_alloc(256, 16, GPU_PREF_CDRAM);
  if (!g_seq16 || !g_static) fatal("gl: can't allocate static draw buffers");
  uint16_t *tmp = malloc(SEQ16_COUNT * 2);
  for (int i = 0; i < SEQ16_COUNT; i++) tmp[i] = (uint16_t)i;
  memcpy(g_seq16->ptr, tmp, SEQ16_COUNT * 2);
  free(tmp);

  uint8_t *s = g_static->ptr;
  g_quad = (float *)s;
  g_quad_idx = (uint16_t *)(s + 64);
  g_const_attr = (float *)(s + 128);
  static const float q[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
  static const uint16_t qi[4] = { 0, 1, 2, 3 };
  static const float ca[4] = { 0, 0, 0, 1 };
  memcpy(g_quad, q, sizeof(q));
  memcpy(g_quad_idx, qi, sizeof(qi));
  memcpy(g_const_attr, ca, sizeof(ca));
}

// ============================================================ stats

static struct { uint32_t frame, draws, clears, skipped; } g_st;

// debug switches from config.txt: no_draw=1 skips every draw (clears and
// swaps still happen), frame_log=1 logs every frame's counts
extern int config_int(const char *key, int def);
static int dbg_no_draw = -1, dbg_frame_log = -1;

static void stat_tick(void)
{
  uint32_t f = gxm_frame_count();
  if (f == g_st.frame) return;
  if (dbg_frame_log < 0) dbg_frame_log = config_int("frame_log", 0);
  if (g_st.frame < 5 || (g_st.frame % 1800) == 0 || dbg_frame_log)
    L("gl: frame %u: %u draws, %u clears, %u skipped", g_st.frame, g_st.draws, g_st.clears, g_st.skipped);
  g_st.frame = f;
  g_st.draws = g_st.clears = g_st.skipped = 0;
}

// ============================================================ mapping

static int map_prim(GLenum mode, SceGxmPrimitiveType *pt, int *kind)
{
  switch (mode) {
  case GL_TRIANGLES: *pt = SCE_GXM_PRIMITIVE_TRIANGLES; *kind = 0; return 1;
  case GL_TRIANGLE_STRIP: *pt = SCE_GXM_PRIMITIVE_TRIANGLE_STRIP; *kind = 0; return 1;
  case GL_TRIANGLE_FAN: *pt = SCE_GXM_PRIMITIVE_TRIANGLE_FAN; *kind = 0; return 1;
  case GL_LINES: *pt = SCE_GXM_PRIMITIVE_LINES; *kind = 1; return 1;
  case GL_POINTS: *pt = SCE_GXM_PRIMITIVE_POINTS; *kind = 2; return 1;
  default:
    L_ONCE("gl: draw mode 0x%X not supported (line strip/loop TODO)", mode);
    return 0;
  }
}

static int map_attr(GLenum type, GLboolean norm, uint8_t *fmt, unsigned *tsize)
{
  switch (type) {
  case GL_FLOAT: *fmt = SCE_GXM_ATTRIBUTE_FORMAT_F32; *tsize = 4; return 1;
  case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES: *fmt = SCE_GXM_ATTRIBUTE_FORMAT_F16; *tsize = 2; return 1;
  case GL_UNSIGNED_BYTE: *fmt = norm ? SCE_GXM_ATTRIBUTE_FORMAT_U8N : SCE_GXM_ATTRIBUTE_FORMAT_U8; *tsize = 1; return 1;
  case GL_BYTE: *fmt = norm ? SCE_GXM_ATTRIBUTE_FORMAT_S8N : SCE_GXM_ATTRIBUTE_FORMAT_S8; *tsize = 1; return 1;
  case GL_UNSIGNED_SHORT: *fmt = norm ? SCE_GXM_ATTRIBUTE_FORMAT_U16N : SCE_GXM_ATTRIBUTE_FORMAT_U16; *tsize = 2; return 1;
  case GL_SHORT: *fmt = norm ? SCE_GXM_ATTRIBUTE_FORMAT_S16N : SCE_GXM_ATTRIBUTE_FORMAT_S16; *tsize = 2; return 1;
  default: return 0;
  }
}

// ============================================================ program variants

typedef struct {
  const void *ptr;
  uint16_t stride;
  uint8_t fmt, comps;
  uint16_t reg;
  uint8_t isrc;
  uint8_t is_const;
  gl_buffer *buf;
} stream_t;

static int try_vp(gl_program *p, stream_t *st, int ns, int const_via_instance, SceGxmVertexProgram **out)
{
  SceGxmVertexAttribute attrs[MAX_ATTRIBS];
  SceGxmVertexStream streams[MAX_ATTRIBS];
  for (int i = 0; i < ns; i++) {
    attrs[i].streamIndex = i;
    attrs[i].offset = 0;
    attrs[i].format = st[i].fmt;
    attrs[i].componentCount = st[i].comps;
    attrs[i].regIndex = st[i].reg;
    streams[i].stride = st[i].stride;
    streams[i].indexSource = st[i].isrc;
    if (st[i].is_const && const_via_instance) {
      // non-instanced draws keep instance index 0, so this always reads element 0
      streams[i].stride = 16;
      streams[i].indexSource = SCE_GXM_INDEX_SOURCE_INSTANCE_16BIT;
    }
  }
  return sceGxmShaderPatcherCreateVertexProgram(gxm_patcher, p->vid, attrs, ns, streams, ns, out);
}

static SceGxmVertexProgram *get_vp(gl_program *p, stream_t *st, int ns)
{
  uint8_t key[sizeof(((vp_variant *)0)->key)];
  int kl = 0, has_const = 0;
  for (int i = 0; i < ns; i++) {
    key[kl++] = st[i].fmt;
    key[kl++] = st[i].comps;
    key[kl++] = st[i].stride & 0xFF;
    key[kl++] = st[i].stride >> 8;
    key[kl++] = st[i].reg & 0xFF;
    key[kl++] = st[i].isrc | (st[i].is_const << 4);
    has_const |= st[i].is_const;
  }
  uint32_t h = fnv1a(key, kl, 2166136261u);
  for (vp_variant *v = p->vps; v; v = v->next)
    if (v->hash == h && v->keylen == kl && !memcmp(v->key, key, kl)) return v->vp;

  SceGxmVertexProgram *vp = NULL;
  int r;
  if (has_const && g_stride0_ok == 0) {
    r = try_vp(p, st, ns, 1, &vp);
  } else {
    r = try_vp(p, st, ns, 0, &vp);
    if (has_const && g_stride0_ok < 0) {
      if (r >= 0) {
        g_stride0_ok = 1;
        L("gl: gxm accepted stride 0 streams for constant attributes");
      } else {
        int r2 = try_vp(p, st, ns, 1, &vp);
        L("gl: stride 0 stream rejected (0x%08X), instance-stream trick gave 0x%08X", r, r2);
        if (r2 >= 0) { g_stride0_ok = 0; r = r2; }
      }
    } else if (has_const && r < 0 && g_stride0_ok == 1) {
      r = try_vp(p, st, ns, 1, &vp);
    }
  }
  if (r < 0) {
    L("gl: sceGxmShaderPatcherCreateVertexProgram failed 0x%08X (prog %u, %d streams)", r, p->name, ns);
    for (int i = 0; i < ns; i++)
      L("gl:   stream %d fmt %d comps %d stride %d reg %d", i, st[i].fmt, st[i].comps, st[i].stride, st[i].reg);
    vp = NULL;
  }
  vp_variant *v = calloc(1, sizeof(*v));
  v->hash = h;
  v->keylen = kl;
  memcpy(v->key, key, kl);
  v->vp = vp;   // NULL is cached too, no point retrying every draw
  v->next = p->vps;
  p->vps = v;
  return vp;
}

static SceGxmFragmentProgram *get_fp(gl_program *p)
{
  SceGxmBlendInfo bi;
  gl_blend_info(&bi);
  if (!T.has_color) bi.colorMask = SCE_GXM_COLOR_MASK_NONE;
  uint32_t b;
  memcpy(&b, &bi, 4);
  // a fragment program's sample mode has to match the target's
  uint64_t key = b | ((uint64_t)T.out_fmt << 32) | ((uint64_t)T.msaa << 48);
  for (fp_variant *v = p->fps; v; v = v->next)
    if (v->key == key) return v->fp;
  SceGxmFragmentProgram *fp = NULL;
  int r = sceGxmShaderPatcherCreateFragmentProgram(gxm_patcher, p->fid, T.out_fmt, T.msaa,
                                                   &bi, p->vgxp, &fp);
  if (r < 0) {
    L("gl: sceGxmShaderPatcherCreateFragmentProgram failed 0x%08X (prog %u, outfmt %d)", r, p->name, T.out_fmt);
    fp = NULL;
  }
  fp_variant *v = calloc(1, sizeof(*v));
  v->key = key;
  v->fp = fp;
  v->next = p->fps;
  p->fps = v;
  return fp;
}

// ============================================================ draw

static void do_draw(GLenum mode, GLsizei count, GLenum itype, const void *indices, GLint first,
                    GLsizei inst, int arrays)
{
  stat_tick();
  if (count <= 0 || inst <= 0 || !gxm_ready()) return;
  if (dbg_no_draw < 0) dbg_no_draw = config_int("no_draw", 0);
  if (dbg_no_draw) { g_st.skipped++; return; }
  draw_init();
  gl_program *p = prog_current();
  // still compiling in the background? skip just this draw, the frame goes on
  if (p && p->pending && !prog_try_finish(p)) {
    g_st.skipped++;
    return;
  }
  if (!p || !p->linked) {
    g_st.skipped++;
    if (p && p->link_tried && !p->delete_pending) {
      static GLuint last;
      if (last != p->name) { last = p->name; L("gl: skipping draws with unlinked program %u", p->name); }
    }
    return;
  }
  if (G.rast_discard) return;
  SceGxmPrimitiveType pt;
  int kind;
  if (!map_prim(mode, &pt, &kind)) return;

  int has_client = 0;
  for (int i = 0; i < p->n_attribs; i++) {
    const prog_attrib *a = &p->attribs[i];
    if (a->reg >= 0 && G.va[a->loc].enabled && !G.va[a->loc].buffer) has_client = 1;
  }

  // ---- indices
  SceGxmIndexFormat ifmt = SCE_GXM_INDEX_FORMAT_U16;
  const void *iptr = NULL;
  const uint8_t *isrc = NULL;
  gl_buffer *ibuf = NULL;
  unsigned isz = 0, vmin = 0, vmax = 0;
  uint32_t need = 0;
  int gen32 = 0, conv8 = 0, copyidx = 0;

  if (arrays) {
    // indices always run 0..count-1, the stream pointers start at 'first'
    vmin = first;
    vmax = first + count - 1;
    if (count <= SEQ16_COUNT) {
      iptr = g_seq16->ptr;
    } else {
      ifmt = SCE_GXM_INDEX_FORMAT_U32;
      gen32 = 1;
      need += ALIGN_UP(count * 4, 16);
    }
  } else {
    isz = itype == GL_UNSIGNED_BYTE ? 1 : itype == GL_UNSIGNED_SHORT ? 2 : itype == GL_UNSIGNED_INT ? 4 : 0;
    if (!isz) { GL_ERR(GL_INVALID_ENUM); return; }
    if (G.element_buffer) {
      ibuf = buf_get(G.element_buffer);
      if (!ibuf || !ibuf->mem || (uintptr_t)indices + count * isz > ibuf->size) {
        L_ONCE("gl: glDrawElements index buffer missing or too small");
        return;
      }
      isrc = (const uint8_t *)ibuf->mem->ptr + (uintptr_t)indices;
    } else {
      if (!indices) return;
      isrc = indices;
    }
    ifmt = isz == 4 ? SCE_GXM_INDEX_FORMAT_U32 : SCE_GXM_INDEX_FORMAT_U16;
    if (isz == 1) { conv8 = 1; need += ALIGN_UP(count * 2, 16); }
    else if (!ibuf) { copyidx = 1; need += ALIGN_UP(count * isz, 16); }
    else iptr = isrc;
    if (has_client) {
      // client vertex arrays: copy vertices 0..max index (no negative base
      // pointers, gxm only ever sees addresses inside the copy)
      if (ibuf) L_ONCE("gl: client vertex arrays + index buffer, scanning gpu memory (slow)");
      vmin = 0; vmax = 0;
      for (int i = 0; i < count; i++) {
        unsigned v = isz == 1 ? isrc[i] : isz == 2 ? ((const uint16_t *)isrc)[i] : ((const uint32_t *)isrc)[i];
        if (v > vmax) vmax = v;
      }
    }
  }
  uint8_t isource = ifmt == SCE_GXM_INDEX_FORMAT_U32 ? SCE_GXM_INDEX_SOURCE_INDEX_32BIT
                                                    : SCE_GXM_INDEX_SOURCE_INDEX_16BIT;

  // ---- attributes, one stream each
  stream_t st[MAX_ATTRIBS];
  int ns = 0;
  for (int i = 0; i < p->n_attribs; i++) {
    const prog_attrib *a = &p->attribs[i];
    if (a->reg < 0) continue;
    gl_vattrib *va = &G.va[a->loc];
    stream_t *s = &st[ns++];
    memset(s, 0, sizeof(*s));
    s->reg = a->reg;
    s->isrc = isource;
    if (!va->enabled) {
      // GL's current attrib value, which is (0,0,0,1) since glVertexAttrib* isn't imported
      s->ptr = g_const_attr;
      s->fmt = SCE_GXM_ATTRIBUTE_FORMAT_F32;
      s->comps = (a->comps > 0 && a->comps <= 4) ? a->comps : 4;
      s->stride = 0;
      s->is_const = 1;
      continue;
    }
    unsigned tsz;
    if (!map_attr(va->type, va->norm, &s->fmt, &tsz)) {
      L_ONCE("gl: vertex attrib type 0x%X not supported, draw skipped", va->type);
      return;
    }
    s->comps = va->size;
    s->stride = va->stride ? va->stride : tsz * va->size;
    if (va->buffer) {
      gl_buffer *b = va->buffer == (GLuint)-1 ? NULL : buf_get(va->buffer);
      if (!b || !b->mem) { L_ONCE("gl: attrib %d points at a dead buffer, draw skipped", a->loc); return; }
      s->ptr = (const uint8_t *)b->mem->ptr + (uintptr_t)va->ptr + (arrays ? (uint32_t)first * s->stride : 0);
      s->buf = b;
    } else {
      s->ptr = NULL;   // filled from the ring below
      need += ALIGN_UP((vmax - vmin) * s->stride + tsz * va->size, 16);
    }
  }

  // ---- everything transient in one ring grab (may split the scene, so before begin)
  uint8_t *rp = NULL;
  if (need) {
    rp = ring_alloc(need, 16);
    if (!rp) { g_st.skipped++; return; }
  }
  if (gen32) {
    uint32_t *o = (uint32_t *)rp;
    for (int i = 0; i < count; i++) o[i] = i;
    iptr = o;
    rp += ALIGN_UP(count * 4, 16);
  } else if (conv8) {
    uint16_t tmp[512];
    uint16_t *o = (uint16_t *)rp;
    for (int i = 0; i < count; i += 512) {
      int n = count - i < 512 ? count - i : 512;
      for (int k = 0; k < n; k++) tmp[k] = isrc[i + k];
      memcpy(o + i, tmp, n * 2);
    }
    iptr = o;
    rp += ALIGN_UP(count * 2, 16);
  } else if (copyidx) {
    memcpy(rp, isrc, count * isz);
    iptr = rp;
    rp += ALIGN_UP(count * isz, 16);
  }
  for (int i = 0, k = 0; i < p->n_attribs; i++) {
    const prog_attrib *a = &p->attribs[i];
    if (a->reg < 0) continue;
    stream_t *s = &st[k++];
    if (s->ptr) continue;
    gl_vattrib *va = &G.va[a->loc];
    unsigned tsz;
    map_attr(va->type, va->norm, &s->fmt, &tsz);
    uint32_t bytes = (vmax - vmin) * s->stride + tsz * va->size;
    memcpy(rp, (const uint8_t *)va->ptr + vmin * s->stride, bytes);
    s->ptr = rp;
    rp += ALIGN_UP(bytes, 16);
  }

  // ---- scene + state
  if (gl_ensure_scene(0) < 0) { g_st.skipped++; return; }
  uint32_t sc = gxm_scene_counter();
  if (ibuf) ibuf->last_use = sc;
  for (int i = 0; i < ns; i++) if (st[i].buf) st[i].buf->last_use = sc;
  p->last_use = sc;

  SceGxmVertexProgram *vp = get_vp(p, st, ns);
  SceGxmFragmentProgram *fp = get_fp(p);
  if (!vp || !fp) { g_st.skipped++; return; }
  if (!gl_apply_state(kind)) { g_st.skipped++; return; }

  sceGxmSetVertexProgram(gxm_ctx, vp);
  sceGxmSetFragmentProgram(gxm_ctx, fp);
  prog_bind_textures(p, sc);
  for (int i = 0; i < ns; i++) sceGxmSetVertexStream(gxm_ctx, i, st[i].ptr);

  prog_upload_uniforms(p);
  int r;
  int list = pt == SCE_GXM_PRIMITIVE_TRIANGLES || pt == SCE_GXM_PRIMITIVE_LINES || pt == SCE_GXM_PRIMITIVE_POINTS;
  if (inst > 1 && p->uses_iid && list) {
    // hardware instancing: gxm walks the index list 'inst' times (wrapping at
    // count) and feeds the INSTANCE param. assumption, see report.
    r = sceGxmDrawInstanced(gxm_ctx, pt, ifmt, iptr, count * inst, count);
  } else {
    if (inst > 1 && p->uses_iid) L_ONCE("gl: instanced strip/fan with gl_InstanceID, copies all get id 0");
    r = 0;
    for (int k = 0; k < inst && r >= 0; k++) r = sceGxmDraw(gxm_ctx, pt, ifmt, iptr, count);
  }
  if (r < 0) {
    static int fails;
    if (fails++ < 16) L("gl: sceGxmDraw failed 0x%08X (mode 0x%X count %d inst %d)", r, mode, count, inst);
  } else {
    g_st.draws++;
  }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
  do_draw(mode, count, 0, NULL, first, 1, 1);
}

void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei inst)
{
  do_draw(mode, count, 0, NULL, first, inst, 1);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx)
{
  do_draw(mode, count, type, idx, 0, 1, 0);
}

void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *idx, GLsizei inst)
{
  do_draw(mode, count, type, idx, 0, inst, 0);
}

// ============================================================ clear

void glClear(GLbitfield mask)
{
  stat_tick();
  if (!gxm_ready() || G.rast_discard) return;
  mask &= GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
  if (!mask) return;
  if (!CLR.ok) {
    L_ONCE("gl: glClear without the clear shader (libshacccg missing?), skipped");
    return;
  }
  draw_init();

  // whole depth+stencil cleared up front -> the scene doesn't need to load them
  int full = (mask & GL_DEPTH_BUFFER_BIT) && (mask & GL_STENCIL_BUFFER_BIT) && G.depth_mask &&
             (G.stencil[0].wmask & 0xFF) == 0xFF && !G.scissor_test;
  if (gl_ensure_scene(full) < 0) return;

  SceGxmContext *c = gxm_ctx;
  uint8_t cmask = (mask & GL_COLOR_BUFFER_BIT) && T.has_color ? gl_gxm_color_mask() : 0;
  int do_depth = (mask & GL_DEPTH_BUFFER_BIT) && G.depth_mask && T.has_depth;
  int do_sten = (mask & GL_STENCIL_BUFFER_BIT) && T.has_stencil && (G.stencil[0].wmask & 0xFF);
  if (!cmask && !do_depth && !do_sten) return;

  SceGxmFragmentProgram *fp = clear_get_fp(cmask, T.out_fmt);
  if (!fp) return;

  // clear ignores the viewport, only scissor + masks apply
  sceGxmSetViewport(c, T.w * 0.5f, T.w * 0.5f, T.h * 0.5f, -(T.h * 0.5f), 0.5f, 0.5f);
  if (!gl_set_region_clip()) return;
  sceGxmSetCullMode(c, SCE_GXM_CULL_NONE);
  sceGxmSetTwoSidedEnable(c, SCE_GXM_TWO_SIDED_DISABLED);
  sceGxmSetFrontDepthBias(c, 0, 0);
  sceGxmSetBackDepthBias(c, 0, 0);
  sceGxmSetFrontPolygonMode(c, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
  sceGxmSetBackPolygonMode(c, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
  sceGxmSetFrontFragmentProgramEnable(c, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
  sceGxmSetBackFragmentProgramEnable(c, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
  sceGxmSetFrontDepthFunc(c, SCE_GXM_DEPTH_FUNC_ALWAYS);
  sceGxmSetBackDepthFunc(c, SCE_GXM_DEPTH_FUNC_ALWAYS);
  SceGxmDepthWriteMode dw = do_depth ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
  sceGxmSetFrontDepthWriteEnable(c, dw);
  sceGxmSetBackDepthWriteEnable(c, dw);
  if (do_sten) {
    unsigned char wm = G.stencil[0].wmask & 0xFF;
    sceGxmSetFrontStencilFunc(c, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_REPLACE,
                              SCE_GXM_STENCIL_OP_REPLACE, SCE_GXM_STENCIL_OP_REPLACE, 0xFF, wm);
    sceGxmSetBackStencilFunc(c, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_REPLACE,
                             SCE_GXM_STENCIL_OP_REPLACE, SCE_GXM_STENCIL_OP_REPLACE, 0xFF, wm);
    sceGxmSetFrontStencilRef(c, G.clear_stencil & 0xFF);
    sceGxmSetBackStencilRef(c, G.clear_stencil & 0xFF);
  } else {
    sceGxmSetFrontStencilFunc(c, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP,
                              SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 0xFF, 0);
    sceGxmSetBackStencilFunc(c, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP,
                             SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 0xFF, 0);
  }

  sceGxmSetVertexProgram(c, CLR.vp);
  sceGxmSetFragmentProgram(c, fp);
  void *ub;
  if (sceGxmReserveVertexDefaultUniformBuffer(c, &ub) >= 0) {
    float z = G.clear_depth * 2.0f - 1.0f;   // ndc z that lands on clear_depth
    sceGxmSetUniformDataF(ub, CLR.u_depth, 0, 1, &z);
  }
  if (sceGxmReserveFragmentDefaultUniformBuffer(c, &ub) >= 0)
    sceGxmSetUniformDataF(ub, CLR.u_color, 0, 4, G.clear_color);
  sceGxmSetVertexStream(c, 0, g_quad);
  int r = sceGxmDraw(c, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, g_quad_idx, 4);
  if (r < 0) L_ONCE("gl: clear draw failed 0x%08X", r);
  g_st.clears++;
  G.dirty = 1;
}
