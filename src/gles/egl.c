// dummy EGL on top of gxm. one display, one config, one window surface.
#include <string.h>
#include "gles/gl_internal.h"
#include "loader/exports.h"

typedef struct { int magic; } egl_obj;

static egl_obj g_disp = { 0xD15B };
static egl_obj g_conf = { 0xC0F1 };
static egl_obj g_ctx = { 0xC7C7 };
static egl_obj g_surf = { 0x5F5F };

static int g_inited;
static EGLSurface g_cur_draw, g_cur_read;
static EGLContext g_cur_ctx;
static int g_gl_inited;

// main.c calls this on the main thread before the game starts. the gxm
// context isn't tied to a thread, the game's render thread uses it after.
void gfx_init(void)
{
  gxm_init();
  shader_init();
}

static void ensure_init(void)
{
  if (!gxm_ready()) gfx_init();
  if (!g_gl_inited) {
    g_gl_inited = 1;
    gl_state_init();
  }
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType id)
{
  (void)id;
  return &g_disp;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
  (void)dpy;
  L("egl: eglInitialize");
  ensure_init();
  g_inited = 1;
  if (major) *major = 1;
  if (minor) *minor = 4;
  return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib, EGLConfig *configs,
                           EGLint size, EGLint *num)
{
  (void)dpy;
  // log what they want, we only have rgba8 + d24s8 anyway
  if (attrib) {
    for (int i = 0; attrib[i] != EGL_NONE && i < 64; i += 2)
      L("egl: eglChooseConfig wants 0x%04X = %d", attrib[i], attrib[i + 1]);
  }
  if (configs && size > 0) configs[0] = &g_conf;
  if (num) *num = 1;
  return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig cfg, EGLint attr, EGLint *val)
{
  (void)dpy; (void)cfg;
  EGLint v;
  switch (attr) {
  case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: v = 8; break;
  case EGL_BUFFER_SIZE: v = 32; break;
  case EGL_DEPTH_SIZE: v = 24; break;
  case EGL_STENCIL_SIZE: v = 8; break;
  case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: v = 0; break;
  case EGL_CONFIG_ID: v = 1; break;
  case EGL_CONFIG_CAVEAT: v = EGL_NONE; break;
  case EGL_LEVEL: v = 0; break;
  case EGL_NATIVE_RENDERABLE: v = EGL_FALSE; break;
  case EGL_NATIVE_VISUAL_ID: v = 1; break;      // WINDOW_FORMAT_RGBA_8888
  case EGL_NATIVE_VISUAL_TYPE: v = 0; break;
  case EGL_SURFACE_TYPE: v = EGL_WINDOW_BIT; break;
  case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT: v = EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT; break;
  case EGL_COLOR_BUFFER_TYPE: v = EGL_RGB_BUFFER; break;
  case EGL_TRANSPARENT_TYPE: v = EGL_NONE; break;
  case EGL_MIN_SWAP_INTERVAL: v = 0; break;
  case EGL_MAX_SWAP_INTERVAL: v = 4; break;
  case EGL_LUMINANCE_SIZE: case EGL_ALPHA_MASK_SIZE: v = 0; break;
  case EGL_MAX_PBUFFER_WIDTH: v = SCREEN_W; break;
  case EGL_MAX_PBUFFER_HEIGHT: v = SCREEN_H; break;
  case EGL_MAX_PBUFFER_PIXELS: v = SCREEN_W * SCREEN_H; break;
  case EGL_BIND_TO_TEXTURE_RGB: case EGL_BIND_TO_TEXTURE_RGBA: v = EGL_FALSE; break;
  default:
    L("egl: eglGetConfigAttrib unknown 0x%04X", attr);
    v = 0;
    break;
  }
  if (val) *val = v;
  return EGL_TRUE;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share, const EGLint *attrib)
{
  (void)dpy; (void)cfg; (void)share;
  if (attrib) {
    for (int i = 0; attrib[i] != EGL_NONE && i < 32; i += 2)
      L("egl: eglCreateContext 0x%04X = %d", attrib[i], attrib[i + 1]);
  }
  ensure_init();
  return &g_ctx;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
  (void)dpy;
  L("egl: eglDestroyContext %p (ignored, state kept)", ctx);
  if (g_cur_ctx == ctx) g_cur_ctx = EGL_NO_CONTEXT;
  return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win,
                                  const EGLint *attrib)
{
  (void)dpy; (void)cfg; (void)attrib;
  L("egl: eglCreateWindowSurface win=%p -> %dx%d", win, SCREEN_W, SCREEN_H);
  ensure_init();
  return &g_surf;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface s)
{
  (void)dpy;
  L("egl: eglDestroySurface %p", s);
  if (g_cur_draw == s) g_cur_draw = EGL_NO_SURFACE;
  if (g_cur_read == s) g_cur_read = EGL_NO_SURFACE;
  return EGL_TRUE;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
  (void)dpy;
  L("egl: eglMakeCurrent draw=%p read=%p ctx=%p", draw, read, ctx);
  if (ctx) ensure_init();
  g_cur_draw = draw;
  g_cur_read = read;
  g_cur_ctx = ctx;
  return EGL_TRUE;
}

EGLSurface eglGetCurrentSurface(EGLint which)
{
  return which == EGL_READ ? g_cur_read : g_cur_draw;
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface s, EGLint attr, EGLint *val)
{
  (void)dpy; (void)s;
  EGLint v = 0;
  switch (attr) {
  case EGL_WIDTH: v = SCREEN_W; break;
  case EGL_HEIGHT: v = SCREEN_H; break;
  case EGL_CONFIG_ID: v = 1; break;
  case EGL_RENDER_BUFFER: v = EGL_BACK_BUFFER; break;
  case EGL_SWAP_BEHAVIOR: v = EGL_BUFFER_DESTROYED; break;
  case EGL_MULTISAMPLE_RESOLVE: v = EGL_MULTISAMPLE_RESOLVE_DEFAULT; break;
  case EGL_HORIZONTAL_RESOLUTION: case EGL_VERTICAL_RESOLUTION:
  case EGL_PIXEL_ASPECT_RATIO: v = EGL_UNKNOWN; break;
  default:
    L("egl: eglQuerySurface unknown 0x%04X", attr);
    return EGL_FALSE;
  }
  if (val) *val = v;
  return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface s)
{
  (void)dpy; (void)s;
  static int first = 1;
  if (first) { first = 0; L("egl: first eglSwapBuffers"); }
  gxm_swap();
  // next draw/clear opens a new scene on whatever fbo is bound
  G.dirty = 1;
  return EGL_TRUE;
}

EXPORT_TABLE(egl) = {
  EXP(eglChooseConfig),
  EXP(eglCreateContext),
  EXP(eglCreateWindowSurface),
  EXP(eglDestroyContext),
  EXP(eglDestroySurface),
  EXP(eglGetConfigAttrib),
  EXP(eglGetCurrentSurface),
  EXP(eglGetDisplay),
  EXP(eglInitialize),
  EXP(eglMakeCurrent),
  EXP(eglQuerySurface),
  EXP(eglSwapBuffers),
};
EXPORT_TABLE_END(egl);
