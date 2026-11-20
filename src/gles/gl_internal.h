#pragma once
// shared state between the gles shim files. one GL context, one thread.
#include <stdint.h>
#include "gles/gl_defs.h"
#include "gles/gxm.h"
#include "gles/glsl2cg.h"
#include "util/abi.h"
#include "util/log.h"

#define MAX_TEX_UNITS 16
#define MAX_ATTRIBS   16

// short names in our code, prefixed symbols in the link
#define G   gles_state
#define T   gles_target
#define CLR gles_clear_prog

#ifdef NBA_TRACE_GL
#define TRACE(...) L(__VA_ARGS__)
#else
#define TRACE(...) do {} while (0)
#endif

// ---------------------------------------------------------------- names
// tiny name -> object tables, names count up and never get reused
typedef struct {
  void **objs;
  GLuint cap;
  GLuint next;
} name_table;

GLuint nt_gen(name_table *t);
void *nt_get(name_table *t, GLuint name);
void nt_set(name_table *t, GLuint name, void *obj);

// ---------------------------------------------------------------- buffers
typedef struct gl_buffer {
  GLuint name;
  gpu_block *mem;
  void *shadow;        // cpu copy when the gpu pool had no room, streamed per draw
  uint32_t size;
  GLenum usage;
  uint32_t last_use;   // scene id
} gl_buffer;

gl_buffer *buf_get(GLuint name);

// ---------------------------------------------------------------- textures
typedef struct gl_texture {
  GLuint name;
  GLenum target;            // 0 until first bind
  unsigned w, h;
  unsigned stride;          // bytes per row
  unsigned bpp;             // bytes per pixel of the stored format
  SceGxmTextureFormat fmt;
  GLenum ifmt;              // what the game asked for
  gpu_block *mem;
  SceGxmTexture gxm;
  int valid;
  int levels_dropped;
  unsigned skip;            // game mip levels dropped to fit (stored level = game level - skip)
  GLenum min_filter, mag_filter, wrap_s, wrap_t;
  uint32_t last_use;
  // render-to-texture
  int renderable;
  SceGxmColorFormat color_fmt;
  SceGxmOutputRegisterFormat out_fmt;
  SceGxmOutputRegisterSize out_size;
  int is_depth;             // depth texture (no real support yet)
  // mips / cube maps / srgb (gl_tex.c). w/h/stride above are level 0.
  // 2d: linear, level 0 at mem->ptr, mips packed after it. cube: gxm CUBE
  // (swizzled), not renderable.
  unsigned levels;          // mip levels the allocation has room for (>= 1)
  uint16_t defined[6];      // per face, bit n = level n was specified
  int is_cube;
  uint32_t face_size;       // cube: bytes per face incl. its whole chain
  int base_level, max_level;
  int srgb;                 // sampled with gxm gamma decode
  unsigned mips_used;       // levels in the current gxm descriptor
  int chain_guess;          // chain allocated up front on a hint, no level > 0 seen yet
} gl_texture;

gl_texture *tex_get(GLuint name);
// texture to sample for a unit. never NULL, falls back to a white dummy
const SceGxmTexture *tex_for_unit(int unit, int cube, uint32_t scene);

// ---------------------------------------------------------------- fbos
typedef struct gl_renderbuffer {
  GLuint name;
  GLenum ifmt;
  unsigned w, h;
  int is_depth;
  gpu_block *mem;
  SceGxmDepthStencilSurface ds;
  SceGxmColorFormat color_fmt;
  SceGxmOutputRegisterFormat out_fmt;
  unsigned stride_px;
  uint32_t last_use;
  // msaa twin of a depth rb: same contents role, sample-sized (lazily made)
  gpu_block *ms_mem;
  SceGxmDepthStencilSurface ms_ds;
  SceGxmMultisampleMode ms_mode;
} gl_renderbuffer;

typedef struct gl_framebuffer {
  GLuint name;
  GLuint color_tex, color_rb;
  GLuint depth_tex, depth_rb, stencil_rb;
  GLuint stencil_tex;       // set by STENCIL / DEPTH_STENCIL texture attachments
} gl_framebuffer;

// what the current scene is drawing into
typedef struct {
  unsigned w, h;
  int flip;                 // 1 = display, rows top-down; 0 = fbo, rows bottom-up like GL
  int has_color, has_depth;
  SceGxmOutputRegisterFormat out_fmt;
  GLuint fbo;
  // GL has a stencil buffer here. gxm surfaces are always d24s8, but a D16/D24
  // fbo has no stencil in GL, so the stencil test must act as always-pass there
  int has_stencil;
  SceGxmMultisampleMode msaa;   // sample mode of this target's scene
} gl_cur_target;

extern gl_cur_target T;
// make sure a scene is open on the bound draw fbo. full_ds_clear = the op
// about to run clears all of depth+stencil, so no need to load them.
// returns 0 when it's ok to draw.
int gl_ensure_scene(int full_ds_clear);
void gl_fbo_changed(void);   // end the scene if the draw target moved
void fbo_target_size(unsigned *w, unsigned *h);

// ---------------------------------------------------------------- shaders
typedef struct gl_shader {
  GLuint name;
  GLenum type;
  char *src;
  int compiled;
  int attached;
  int delete_pending;
} gl_shader;

typedef struct prog_uniform {
  char name[GLSL2CG_NAME_LEN];     // GLSL name, for glGetUniformLocation
  char cg_name[GLSL2CG_NAME_LEN];  // gxp param name (differs if it clashed with a Cg keyword)
  g2c_type type;
  int elems;               // GL array size
  int cols;                // vec4 slots per element (matrix columns) or 1
  int comps;               // live components per slot
  int slots;               // elems * cols
  int loc_base;
  int is_sampler, is_cube, is_int;
  int in_vs, in_fs;        // which stages declare it (gxp lookups happen at finish)
  float *data;             // slots * 4 floats, zero padded
  struct {
    const SceGxmProgramParameter *p;
    unsigned res, comps, arr;
    SceGxmParameterType type;
    int stride;            // floats per slot in the uniform buffer
  } st[2];                 // 0 = vertex, 1 = fragment
} prog_uniform;

typedef struct prog_attrib {
  char name[GLSL2CG_NAME_LEN];     // GLSL name, for glBindAttribLocation
  char cg_name[GLSL2CG_NAME_LEN];
  int loc;                 // GL generic attribute index
  int reg;                 // gxm register (resource index), -1 = optimised out
  int comps;
} prog_attrib;

typedef struct prog_sampler {
  int stage;               // 0 vs, 1 fs
  unsigned res;            // gxm texture slot
  int uniform;             // index in uniforms[] or -1
  int cube;
} prog_sampler;

typedef struct vp_variant {
  uint32_t hash;
  uint8_t key[MAX_ATTRIBS * 8 + 4];
  int keylen;
  SceGxmVertexProgram *vp;
  struct vp_variant *next;
} vp_variant;

typedef struct fp_variant {
  uint64_t key;
  SceGxmFragmentProgram *fp;
  struct fp_variant *next;
} fp_variant;

struct compile_job;

typedef struct gl_program {
  GLuint name;
  GLuint vs, fs;
  int linked;              // link succeeded (locations valid). may still be pending
  int link_tried;
  int pending;             // cg compile still running in the background, can't draw yet
  struct compile_job *job;
  char *log;
  struct { char name[GLSL2CG_NAME_LEN]; int loc; } binds[MAX_ATTRIBS * 2];
  int n_binds;

  SceGxmProgram *vgxp, *fgxp;
  SceGxmShaderPatcherId vid, fid;
  uint32_t vbuf_size, fbuf_size;

  prog_attrib attribs[MAX_ATTRIBS];
  int n_attribs;
  prog_uniform *uniforms;
  int n_uniforms;
  int16_t *loc_map;        // location -> uniform index
  int n_locs;
  prog_sampler samplers[32];
  int n_samplers;

  int uses_iid;            // reads gl_InstanceID (hardware INSTANCE param)

  vp_variant *vps;
  fp_variant *fps;
  int delete_pending;
  uint32_t last_use;
} gl_program;

gl_program *prog_get(GLuint name);
gl_program *prog_current(void);
// finishes a background-compiled program once its gxps are in. 1 = drawable
int prog_try_finish(gl_program *p);
// writes all uniforms of p into freshly reserved default uniform buffers
void prog_upload_uniforms(gl_program *p);
void prog_bind_textures(gl_program *p, uint32_t scene);

// libshacccg wrapper. returns a malloc'd gxp or NULL (log in *log, malloc'd)
int shacc_ready(void);
SceGxmProgram *shacc_compile(const char *cg, int frag, char **log);
void shader_init(void);

// clear shader (ours, compiled at init)
typedef struct {
  SceGxmShaderPatcherId vid, fid;
  SceGxmProgram *vgxp, *fgxp;
  SceGxmVertexProgram *vp;
  const SceGxmProgramParameter *u_depth, *u_color;
  fp_variant *fps;
  int ok;
} clear_prog_t;
extern clear_prog_t CLR;
SceGxmFragmentProgram *clear_get_fp(uint8_t cmask, SceGxmOutputRegisterFormat fmt);

// ---------------------------------------------------------------- state
typedef struct {
  int enabled;
  GLint size;
  GLenum type;
  GLboolean norm;
  GLsizei stride;
  const void *ptr;
  GLuint buffer;
} gl_vattrib;

typedef struct {
  GLenum func;
  GLint ref;
  GLuint vmask, wmask;
  GLenum sfail, dpfail, dppass;
} gl_stencil_side;

typedef struct {
  GLenum err;
  GLuint array_buffer, element_buffer;
  GLuint program;
  GLuint draw_fbo, read_fbo, renderbuffer;
  int active_tex;
  GLuint tex2d[MAX_TEX_UNITS], texcube[MAX_TEX_UNITS];
  gl_vattrib va[MAX_ATTRIBS];

  int blend, depth_test, stencil_test, cull, scissor_test, poly_offset, rast_discard;
  GLenum beq_rgb, beq_a, bsrc_rgb, bdst_rgb, bsrc_a, bdst_a;
  GLboolean cmask[4];
  GLboolean depth_mask;
  GLenum depth_func;
  GLenum cull_face, front_face;
  gl_stencil_side stencil[2];   // 0 front, 1 back
  float poly_factor, poly_units;
  GLint vp[4], sc[4];
  float clear_color[4];
  float clear_depth;
  GLint clear_stencil;
  GLint unpack_align, pack_align, unpack_row_len;

  int dirty;          // reapply everything to gxm before the next draw
} gl_state;

extern gl_state G;

#define GL_ERR(e) do { if (!G.err) G.err = (e); } while (0)

void gl_state_init(void);
// push viewport/region clip/depth/stencil/cull to gxm. prim: 0 tri 1 line 2 point.
// returns 0 if the draw would be fully clipped/culled and can be skipped
int gl_apply_state(int prim);
int gl_set_region_clip(void);
uint8_t gl_gxm_color_mask(void);
void gl_blend_info(SceGxmBlendInfo *bi);

// ---------------------------------------------------------------- misc
void etc_decode(GLenum fmt, const uint8_t *src, unsigned w, unsigned h,
                uint8_t *dst, unsigned dst_stride);
int etc_block_bytes(GLenum fmt);   // 8 or 16, 0 if not etc
int etc_has_alpha(GLenum fmt);

static inline uint32_t fnv1a(const void *data, size_t n, uint32_t h)
{
  const uint8_t *p = data;
  while (n--) { h ^= *p++; h *= 16777619u; }
  return h;
}

// etc.c: decoded bytes per pixel (4 rgba8, 1 r11, 2 rg11), 0 if not etc
int etc_out_bpp(GLenum fmt);
