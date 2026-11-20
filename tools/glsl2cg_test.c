// host test harness for src/gles/glsl2cg.c
// build: gcc -Wall -Isrc tools/glsl2cg_test.c src/gles/glsl2cg.c -o glsl2cg_test
// usage:
//   glsl2cg_test [opts] FILE|DIR...        translate each shader on its own
//   glsl2cg_test [opts] --pairs DIR...     pair NAME.vert with NAME.frag
//   glsl2cg_test [opts] --pair VS FS       one explicit pair
//   glsl2cg_test --selftest                built-in synthetic tests
// opts: -v (print Cg), -o DIR (write Cg files), --lenient (no strict typing)
// stage comes from the extension (.vert/.vs/.vsh vs .frag/.fs/.fsh), else
// from whether the source mentions gl_Position.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gles/glsl2cg.h"

static int verbose;
static const char *outdir;
static unsigned flags = G2C_STRICT;

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *s = malloc((size_t)n + 1);
  if (!s) { fclose(f); return NULL; }
  size_t got = fread(s, 1, (size_t)n, f);
  s[got] = 0;
  fclose(f);
  return s;
}

static const char *base_name(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static int ends_with(const char *s, const char *e) {
  size_t a = strlen(s), b = strlen(e);
  return a >= b && !strcmp(s + a - b, e);
}

// 1 = vertex, 0 = fragment, -1 = not a shader file
static int stage_of(const char *path, const char *src) {
  if (ends_with(path, ".vert") || ends_with(path, ".vs") || ends_with(path, ".vsh")) return 1;
  if (ends_with(path, ".frag") || ends_with(path, ".fs") || ends_with(path, ".fsh")) return 0;
  if (!src) return -1;
  return strstr(src, "gl_Position") != NULL;
}

static void write_out(const char *name, const char *ext, const char *text) {
  if (!outdir || !text) return;
  char path[1024];
  snprintf(path, sizeof path, "%s/%s%s", outdir, name, ext);
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "can't write %s\n", path); return; }
  fputs(text, f);
  fclose(f);
}

static void dump(const char *title, const g2c_result *r) {
  if (!verbose) return;
  printf("==== %s\n", title);
  if (r->vs_cg) printf("---- vs\n%s", r->vs_cg);
  if (r->fs_cg) printf("---- fs\n%s", r->fs_cg);
  for (int i = 0; i < r->n_attribs; i++)
    printf("attrib %s type %d loc %d\n", r->attribs[i].name, r->attribs[i].type, r->attribs[i].location);
  for (int i = 0; i < r->n_uniforms; i++) {
    const g2c_uniform *u = &r->uniforms[i];
    printf("uniform %s type %d arr %d slots %d vs %d fs %d\n", u->name, u->type, u->array_size, u->vec4_slots,
           u->in_vs, u->in_fs);
  }
}

static int run_one(const char *path, int *ok, int *bad) {
  char *src = read_file(path);
  if (!src) { fprintf(stderr, "can't read %s\n", path); (*bad)++; return -1; }
  int vs = stage_of(path, src);
  g2c_result *r = calloc(1, sizeof *r);
  int rc = g2c_translate_ex(vs ? src : NULL, vs ? NULL : src, flags, r);
  if (rc) {
    printf("FAIL %s: %s\n", path, r->error);
    (*bad)++;
  } else {
    (*ok)++;
    dump(path, r);
    write_out(base_name(path), ".cg", vs ? r->vs_cg : r->fs_cg);
  }
  g2c_free(r);
  free(r);
  free(src);
  return rc;
}

static int run_pair(const char *vp, const char *fp, int *ok, int *bad) {
  char *vs = read_file(vp), *fs = read_file(fp);
  if (!vs || !fs) {
    fprintf(stderr, "can't read %s / %s\n", vp, fp);
    free(vs);
    free(fs);
    (*bad)++;
    return -1;
  }
  g2c_result *r = calloc(1, sizeof *r);
  int rc = g2c_translate_ex(vs, fs, flags, r);
  if (rc) {
    printf("FAIL %s + %s: %s\n", vp, fp, r->error);
    (*bad)++;
  } else {
    (*ok)++;
    dump(vp, r);
    write_out(base_name(vp), ".cg", r->vs_cg);
    write_out(base_name(fp), ".cg", r->fs_cg);
  }
  g2c_free(r);
  free(r);
  free(vs);
  free(fs);
  return rc;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// sorted list of files in a dir (malloc'd, NULL terminated)
static char **list_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return NULL;
  int n = 0, cap = 64;
  char **v = malloc(sizeof(char *) * (size_t)cap);
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    if (n + 1 >= cap) { cap *= 2; v = realloc(v, sizeof(char *) * (size_t)cap); }
    size_t len = strlen(dir) + strlen(e->d_name) + 2;
    v[n] = malloc(len);
    snprintf(v[n], len, "%s/%s", dir, e->d_name);
    n++;
  }
  closedir(d);
  qsort(v, (size_t)n, sizeof(char *), cmp_str);
  v[n] = NULL;
  return v;
}

static void free_list(char **v) {
  if (!v) return;
  for (int i = 0; v[i]; i++) free(v[i]);
  free(v);
}

static int is_dir(const char *p) {
  DIR *d = opendir(p);
  if (!d) return 0;
  closedir(d);
  return 1;
}

// ---- self tests (all shaders here are made up for testing) ----

typedef struct {
  const char *name, *vs, *fs;
  const char *err;          // NULL = must succeed, else substring of the error
  const char *vs_has[32];
  const char *fs_has[32];
  int (*check)(const g2c_result *r, char *why);  // why has room for 600 chars
} selftest;

static const g2c_uniform *find_uniform(const g2c_result *r, const char *n) {
  for (int i = 0; i < r->n_uniforms; i++)
    if (!strcmp(r->uniforms[i].name, n)) return &r->uniforms[i];
  return NULL;
}

static int check_mats(const g2c_result *r, char *why) {
  const g2c_uniform *u = find_uniform(r, "u_bones");
  if (!u || u->type != G2C_MAT4 || u->array_size != 8 || u->vec4_slots != 32) { strcpy(why, "u_bones metadata"); return 1; }
  u = find_uniform(r, "u_nrm");
  if (!u || u->type != G2C_MAT3 || u->vec4_slots != 3) { strcpy(why, "u_nrm metadata"); return 1; }
  u = find_uniform(r, "u_lights");
  if (!u || u->type != G2C_VEC3 || u->array_size != 4 || u->vec4_slots != 4) { strcpy(why, "u_lights metadata"); return 1; }
  u = find_uniform(r, "u_iv");
  if (!u || u->type != G2C_IVEC3 || u->vec4_slots != 2) { strcpy(why, "u_iv metadata"); return 1; }
  if (r->n_attribs != 3 || strcmp(r->attribs[0].name, "a_pos") || r->attribs[0].location != 3 ||
      r->attribs[1].location != -1 || r->attribs[1].type != G2C_VEC3) {
    strcpy(why, "attrib metadata");
    return 1;
  }
  return 0;
}

static int check_builtins(const g2c_result *r, char *why) {
  if (!r->uses_instance_id || !r->uses_vertex_id || !r->uses_point_size || !r->uses_frag_coord ||
      !r->uses_front_facing || !r->uses_point_coord || !r->uses_frag_depth) {
    strcpy(why, "uses_* flags");
    return 1;
  }
  if (r->attribs[1].type != G2C_IVEC4) { strcpy(why, "ivec4 attrib type"); return 1; }
  return 0;
}

static int check_shared(const g2c_result *r, char *why) {
  const g2c_uniform *u = find_uniform(r, "u_tint");
  if (!u || !u->in_vs || !u->in_fs) { strcpy(why, "u_tint should be in both stages"); return 1; }
  u = find_uniform(r, "u_tex");
  if (!u || u->in_vs || !u->in_fs || u->type != G2C_SAMPLER2D) { strcpy(why, "u_tex metadata"); return 1; }
  u = find_uniform(r, "u_off");
  if (!u || !u->in_vs || u->in_fs) { strcpy(why, "u_off metadata"); return 1; }
  if (r->n_uniforms != 3) { strcpy(why, "uniform count"); return 1; }
  return 0;
}

static const selftest tests[] = {
  { "matrices",
    "#version 300 es\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat3 u_nrm;\n"
    "uniform mat4 u_bones[8];\n"
    "uniform vec3 u_lights[4];\n"
    "uniform float u_w[4];\n"
    "uniform ivec3 u_iv[2];\n"
    "layout(location = 3) in vec4 a_pos;\n"
    "in vec3 a_nrm;\n"
    "in vec4 a_idx;\n"
    "out vec3 v_n;\n"
    "out vec4 v_c;\n"
    "void main() {\n"
    "  mat4 b = u_bones[int(a_idx.x)];\n"
    "  vec4 col = u_bones[1][2];\n"
    "  float e = u_bones[2][3][1];\n"
    "  vec3 l = u_lights[1] * u_w[2];\n"
    "  int k = u_iv[1].y + u_iv[0][2];\n"
    "  mat4 m = u_mvp;\n"
    "  m *= b;\n"
    "  vec4 p = a_pos;\n"
    "  p *= m;\n"
    "  vec4 q = a_pos * u_mvp;\n"
    "  v_n = u_nrm * a_nrm;\n"
    "  v_c = col * e + vec4(l, float(k)) + u_mvp[3] + vec4(u_nrm[1], 0.0) + q;\n"
    "  gl_Position = u_mvp * b * a_pos + p * 0.0;\n"
    "}\n",
    NULL, NULL,
    { "uniform float4 u_mvp[4];", "uniform float4 u_nrm[3];", "uniform float4 u_bones[32];",
      "float4x4 _g2c_mu_u_bones(int i)", "_g2c_mu_u_bones(((int)(_g2c_in_a_idx.x)))",
      "u_bones[(1) * 4 + (2)]", "u_bones[(2) * 4 + (3)][1]", "(u_lights[1].xyz * u_w[2].x)",
      "((int)(u_iv[1].y))", "((int)(u_iv[0][2]))", "m = mul(b, m)", "p = mul(m, p)",
      "mul(float4x4(u_mvp[0], u_mvp[1], u_mvp[2], u_mvp[3]), _g2c_in_a_pos)",
      "mul(_g2c_in_a_nrm, float3x3(u_nrm[0].xyz, u_nrm[1].xyz, u_nrm[2].xyz))",
      "mul(_g2c_in_a_pos, mul(b, float4x4(u_mvp[0], u_mvp[1], u_mvp[2], u_mvp[3])))",
      "u_nrm[1].xyz", "float4 a_pos,", "out float3 _g2c_tc0 : TEXCOORD0", "out float4 _g2c_tc1 : TEXCOORD1",
      "out float4 _g2c_opos : POSITION", NULL },
    { NULL }, check_mats },

  { "constructors",
    NULL,
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec4 u_a;\n"
    "uniform mat4 u_m4;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  vec4 a = vec4(0.5);\n"
    "  vec3 b = vec3(u_a);\n"
    "  vec2 c2 = vec2(u_a.x, b);\n"
    "  mat3 m3 = mat3(u_m4);\n"
    "  mat2 m2 = mat2(2.0);\n"
    "  mat4 m4 = mat4(m3);\n"
    "  mat2 mc = mat2(vec2(1.0, 0.0), vec2(0.0, 1.0));\n"
    "  mat2 ms = mat2(1.0, 2.0, 3.0, 4.0);\n"
    "  ivec2 iv = ivec2(v_uv);\n"
    "  bool same = b == vec3(1.0);\n"
    "  bool diff = a != u_a;\n"
    "  bvec2 lt = lessThan(v_uv, vec2(0.5));\n"
    "  float f = float(iv.x) + (same ? 1.0 : 0.0) + (diff ? 1.0 : 0.0);\n"
    "  if (any(lt) && !all(lt)) f += 1.0;\n"
    "  if (same ^^ diff) f -= 1.0;\n"
    "  o_col = vec4(c2, m2[0]) + a * f + vec4(m3[0], 1.0) + m4[3] + vec4(mc[1], ms[0]);\n"
    "}\n",
    NULL,
    { NULL },
    { "((float4)(0.5))", "u_a.xyz", "float2(u_a.x, b.x)",
      "_g2c_m3x3_from_4x4(float4x4(u_m4[0], u_m4[1], u_m4[2], u_m4[3]))",
      "float3x3 _g2c_m3x3_from_4x4(float4x4 m)", "_g2c_diag2x2(2.0)", "_g2c_m4x4_from_3x3(m3)",
      "float2x2(float2(1.0, 0.0), float2(0.0, 1.0))", "float2x2(1.0, 2.0, 3.0, 4.0)",
      "((int2)(_g2c_v_v_uv))", "all(b == ((float3)(1.0)))", "any(a != u_a)",
      "(_g2c_v_v_uv < ((float2)(0.5)))", "(same != diff)", "float4 main(", ") : COLOR",
      "float2 _g2c_tc0 : TEXCOORD0", "return _g2c_o_o_col;", NULL },
    NULL },

  { "builtins",
    NULL,
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform samplerCube u_env;\n"
    "in vec4 v_p;\n"
    "in vec3 v_d;\n"
    "out vec4 o_c;\n"
    "void main() {\n"
    "  vec4 t = texture(u_tex, v_p.xy);\n"
    "  t += textureLod(u_tex, v_p.xy, 2.0);\n"
    "  t += textureProj(u_tex, v_p);\n"
    "  t += textureProj(u_tex, v_p.xyz);\n"
    "  t += texture(u_env, v_d);\n"
    "  t += textureLod(u_env, v_d, 1.0);\n"
    "  t += texture(u_tex, v_p.zw, 0.5);\n"
    "  float a = atan(v_p.y, v_p.x) + atan(v_p.z);\n"
    "  vec3 m = mod(v_d, 2.0);\n"
    "  vec3 mx = mix(v_d, m, 0.25);\n"
    "  vec3 sel = mix(v_d, m, lessThan(v_d, m));\n"
    "  float fr = fract(a) + inversesqrt(a) + dFdx(a) + dFdy(a) + mod(a, 3.0);\n"
    "  vec3 cl = clamp(v_d, 0.0, 1.0) + step(0.5, v_d) + smoothstep(0.0, 1.0, v_d) + max(v_d, 0.1);\n"
    "  bvec3 eq = equal(v_d, m);\n"
    "  mat3 mm = matrixCompMult(mat3(1.0), mat3(2.0));\n"
    "  o_c = t + vec4(mx + sel + cl + mm[0], fr) + vec4(eq.x ? 1.0 : 0.0);\n"
    "}\n",
    NULL,
    { NULL },
    { "uniform sampler2D u_tex;", "uniform samplerCUBE u_env;", "tex2D(u_tex, _g2c_v_v_p.xy)",
      "tex2Dlod(u_tex, float4(_g2c_v_v_p.xy, 0.0, 2.0))", "tex2Dproj(u_tex, (_g2c_v_v_p).xyw)",
      "tex2Dproj(u_tex, _g2c_v_v_p.xyz)", "texCUBE(u_env, _g2c_v_v_d)",
      "texCUBElod(u_env, float4(_g2c_v_v_d, 1.0))", "tex2Dbias(u_tex, float4(_g2c_v_v_p.zw, 0.0, 0.5))",
      "atan2(_g2c_v_v_p.y, _g2c_v_v_p.x)", "atan(_g2c_v_v_p.z)", "_g2c_mod(_g2c_v_v_d, ((float3)(2.0)))",
      "float3 _g2c_mod(float3 x, float3 y) { return x - y * floor(x / y); }",
      "float _g2c_mod(float x, float y)", "lerp(_g2c_v_v_d, m, ((float3)(0.25)))",
      "((_g2c_v_v_d < m) ? m : _g2c_v_v_d)", "frac(a)", "rsqrt(a)", "ddx(a)", "ddy(a)",
      "clamp(_g2c_v_v_d, ((float3)(0.0)), ((float3)(1.0)))", "step(((float3)(0.5)), _g2c_v_v_d)",
      "(_g2c_v_v_d == m)", "(_g2c_diag3x3(1.0) * _g2c_diag3x3(2.0))", NULL },
    NULL },

  { "functions_structs_loops",
    "#version 300 es\n"
    "struct Light { vec3 dir; float power; };\n"
    "const float k_scale = 2.0;\n"
    "const int k_n = 3;\n"
    "const vec2 k_offs[2] = vec2[2](vec2(1.0, 0.0), vec2(0.0, 1.0));\n"
    "uniform vec4 u_dir;\n"
    "in vec4 a_pos;\n"
    "out float v_lit;\n"
    "float later(float x);\n"
    "float half_of(float x) { return x * 0.5; }\n"
    "vec2 half_of(vec2 x) { return x * 0.5; }\n"
    "void split(in vec4 v, out vec3 xyz, inout float w) { xyz = v.xyz; w += v.w; }\n"
    "Light make_light(vec3 d) { Light l = Light(d, k_scale); return l; }\n"
    "void main() {\n"
    "  float sampler = 0.0;\n"
    "  float acc[k_n];\n"
    "  for (int i = 0; i < k_n; i++) { acc[i] = float(i); }\n"
    "  int i = 0;\n"
    "  for (int i = 0; i < 2; ) { sampler += acc[i]; i++; }\n"
    "  vec3 d; float w = 1.0;\n"
    "  split(u_dir, d, w);\n"
    "  Light l = make_light(d);\n"
    "  float matrix = half_of(l.power) + later(l.dir.x) + half_of(k_offs[1]).y;\n"
    "  float u_dir = 1.0;\n"
    "  float[3] wt = float[3](0.25, 0.5, 0.25);\n"
    "  float ws[] = float[](1.0, 2.0);\n"
    "  while (w > 10.0) { w -= 1.0; if (w < 0.0) break; else continue; }\n"
    "  do { w += 1.0; } while (w < 0.0);\n"
    "  int bits = (k_n << 2) | 1;\n"
    "  v_lit = sampler + matrix + w + float(i + bits) + float(acc.length()) + u_dir + wt[1] + ws[1];\n"
    "  gl_Position = a_pos;\n"
    "}\n"
    "float later(float x) { return x + 1.0; }\n",
    NULL, NULL,
    { "struct Light {", "Light _g2c_mk_Light(float3 _p0, float _p1)", "static const float k_scale = 2.0;",
      "static const int k_n = 3;", "static const float2 k_offs[2] = {float2(1.0, 0.0), float2(0.0, 1.0)};",
      "float later(float _g2c_p0);", "float half_of(float x)", "float2 half_of(float2 x)",
      "void split(float4 v, out float3 xyz, inout float w)", "_g2c_mk_Light(d, k_scale)",
      "float acc[3];", "_sampler = 0.0;", "_matrix = ", "_u_dir = 1.0;", "float wt[3] = {0.25, 0.5, 0.25};",
      "float ws[2] = {1.0, 2.0};", "split(u_dir, d, w);", "((float)(3))", "half_of(k_offs[1]).y",
      "while ((w > 10.0))", "while ((w < 0.0));", "break;", "continue;", "for (; (i < k_n); (i++))", NULL },
    { NULL }, NULL },

  { "preprocessor",
    NULL,
    "#version 300 es\n"
    "#define SCALE 2.0\n"
    "#define MUL(a, b) ((a) * (b))\n"
    "#define CAT(a, b) a##b\n"
    "#define EMPTY()\n"
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "#if defined(SCALE) && __VERSION__ >= 300 && (1 + 2 * 3 == 7)\n"
    "#define HAVE_IT 1\n"
    "#elif 0\n"
    "#error should not happen\n"
    "#else\n"
    "#define HAVE_IT 0\n"
    "#endif\n"
    "#ifndef HAVE_IT\n"
    "#error broken\n"
    "#endif\n"
    "#if 0\n"
    "  this is not glsl at all ###\n"
    "#endif\n"
    "/* block\n comment */\n"
    "in vec2 v_uv; // line comment\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  float CAT(my, val) = MUL(v_uv.x, \\\n SCALE) EMPTY();\n"
    "#if HAVE_IT\n"
    "  o_col = vec4(myval);\n"
    "#else\n"
    "  o_col = vec4(0.0);\n"
    "#endif\n"
    "#undef SCALE\n"
    "#ifdef SCALE\n"
    "  o_col = vec4(1.0);\n"
    "#endif\n"
    "  if (myval < 0.0) discard;\n"
    "}\n",
    NULL,
    { NULL },
    { "float myval = ", "((_g2c_v_v_uv.x) * (2.0))", "_g2c_o_o_col = ((float4)(myval));", "discard;", NULL },
    NULL },

  { "builtin_vars",
    "#version 300 es\n"
    "in vec4 a_pos;\n"
    "in ivec4 a_bone;\n"
    "flat out int v_id;\n"
    "out vec2 v_uv;\n"
    "invariant gl_Position;\n"
    "void main() {\n"
    "  v_id = gl_InstanceID + gl_VertexID + a_bone.x;\n"
    "  v_uv = a_pos.xy;\n"
    "  gl_PointSize = 4.0;\n"
    "  gl_Position = a_pos;\n"
    "}\n",
    "#version 300 es\n"
    "precision highp float;\n"
    "flat in int v_id;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  vec4 c = gl_FragCoord + vec4(gl_PointCoord, 0.0, 0.0);\n"
    "  if (!gl_FrontFacing) c.x = float(v_id);\n"
    "  gl_FragDepth = 0.5;\n"
    "  o_col = c + vec4(v_uv, 0.0, 0.0);\n"
    "}\n",
    NULL,
    { "int _g2c_iid : INSTANCE", "int _g2c_vid : INDEX", "out float _g2c_opsize : PSIZE", "float4 a_bone",
      "_g2c_in_a_bone = (int4)(a_bone);", "out float _g2c_tc0 : TEXCOORD0", "_g2c_tc0 = (float)(_g2c_v_v_id);",
      "out float2 _g2c_tc1 : TEXCOORD1", "_g2c_InstanceID = _g2c_iid;", NULL },
    { "float4 _g2c_wpos : WPOS", "float _g2c_face : FACE", "float2 _g2c_spc : SPRITECOORD",
      "out float _g2c_odepth : DEPTH", "float2 _g2c_tc1 : TEXCOORD1", "_g2c_v_v_id = (int)round(_g2c_tc0);",
      "_g2c_FrontFacing = _g2c_face > 0.0;", "(!_g2c_FrontFacing)", "_g2c_odepth = _g2c_FragDepth;", NULL },
    check_builtins },

  { "shared_uniforms",
    "#version 300 es\n"
    "uniform vec4 u_tint;\n"
    "uniform vec2 u_off;\n"
    "in vec4 a_pos;\n"
    "out vec4 v_col;\n"
    "out vec2 v_unused;\n"
    "void main() { v_col = u_tint; v_unused = u_off; gl_Position = a_pos + vec4(u_off, 0.0, 0.0); }\n",
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec4 u_tint;\n"
    "uniform sampler2D u_tex;\n"
    "in vec4 v_col;\n"
    "out vec4 o_col;\n"
    "void main() { o_col = v_col * u_tint * texture(u_tex, v_col.xy); }\n",
    NULL,
    { "uniform float4 u_tint;", "uniform float2 u_off;", "out float4 _g2c_tc0 : TEXCOORD0", NULL },
    { "float4 _g2c_tc0 : TEXCOORD0", NULL },
    check_shared },

  { "es100",
    "attribute vec4 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "uniform mat4 u_mvp;\n"
    "void main() { v_uv = a_uv; gl_Position = u_mvp * a_pos; }\n",
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv) * 0.5; }\n",
    NULL,
    { "float2 a_uv", "static float2 _g2c_v_v_uv;", NULL },
    { "tex2D(u_tex, _g2c_v_v_uv)", "return _g2c_FragColor;", NULL },
    NULL },

  { "fragdata",
    NULL,
    "precision mediump float;\n"
    "void main() { gl_FragData[0] = vec4(1.0); }\n",
    NULL, { NULL }, { "_g2c_FragData[0] = ((float4)(1.0));", "return _g2c_FragData[0];", NULL }, NULL },

  { "fs_out_float",
    NULL,
    "#version 300 es\nprecision mediump float;\nout float o_v;\nvoid main() { o_v = 0.25; }\n",
    NULL, { NULL }, { "return float4(_g2c_o_o_v, 0.0, 0.0, 1.0);", NULL }, NULL },

  { "too_many_varyings",
    "#version 300 es\n"
    "in vec4 a;\n"
    "out vec4 v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10;\n"
    "void main() { v0 = a; v1 = a; v2 = a; v3 = a; v4 = a; v5 = a; v6 = a; v7 = a; v8 = a; v9 = a; v10 = a; gl_Position = a; }\n",
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10;\n"
    "out vec4 o;\n"
    "void main() { o = v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10; }\n",
    "too many varyings", { NULL }, { NULL }, NULL },

  { "missing_varying",
    "#version 300 es\nin vec4 a;\nout vec4 v_a;\nvoid main() { v_a = a; gl_Position = a; }\n",
    "#version 300 es\nprecision mediump float;\nin vec4 v_b;\nout vec4 o;\nvoid main() { o = v_b; }\n",
    "never written", { NULL }, { NULL }, NULL },

  { "varying_type_mismatch",
    "#version 300 es\nin vec4 a;\nout vec4 v_a;\nvoid main() { v_a = a; gl_Position = a; }\n",
    "#version 300 es\nprecision mediump float;\nin vec3 v_a;\nout vec4 o;\nvoid main() { o = vec4(v_a, 1.0); }\n",
    "different types", { NULL }, { NULL }, NULL },

  { "switch_unsupported",
    NULL,
    "#version 300 es\nprecision mediump float;\nuniform int u_m;\nout vec4 o;\n"
    "void main() { switch (u_m) { case 0: o = vec4(0.0); break; default: o = vec4(1.0); } }\n",
    "switch", { NULL }, { NULL }, NULL },

  { "reserved_uniform",
    NULL,
    "#version 300 es\nprecision mediump float;\nuniform vec4 matrix;\nout vec4 o;\nvoid main() { o = matrix; }\n",
    "reserved", { NULL }, { NULL }, NULL },

  { "unknown_function",
    NULL,
    "#version 300 es\nprecision mediump float;\nout vec4 o;\nvoid main() { o = frobnicate(1.0); }\n",
    "unknown function", { NULL }, { NULL }, NULL },

  { "mrt_unsupported",
    NULL,
    "#version 300 es\nprecision mediump float;\nlayout(location = 0) out vec4 o0;\n"
    "layout(location = 1) out vec4 o1;\nvoid main() { o0 = vec4(1.0); o1 = vec4(0.0); }\n",
    "location 1", { NULL }, { NULL }, NULL },

  { "strict_type_error",
    NULL,
    "#version 300 es\nprecision mediump float;\nout vec4 o;\nvoid main() { vec3 a = vec4(1.0); o = vec4(a, 1.0); }\n",
    "can't initialize", { NULL }, { NULL }, NULL },

  { "texel_fetch_unsupported",
    NULL,
    "#version 300 es\nprecision mediump float;\nuniform sampler2D s;\nout vec4 o;\n"
    "void main() { o = texelFetch(s, ivec2(0), 0); }\n",
    "texelFetch", { NULL }, { NULL }, NULL },

  { "int_uniforms_native",
    NULL,
    "#version 300 es\nprecision mediump float;\nuniform int u_n;\nuniform bool u_on;\nuniform int u_list[3];\n"
    "out vec4 o;\nvoid main() { float f = 0.0; for (int i = 0; i < u_n; ++i) f += 1.0;\n"
    "  if (u_on) f += float(u_list[2]); o = vec4(f); }\n",
    NULL, { NULL },
    { "uniform int u_n;", "uniform bool u_on;", "uniform float4 u_list[3];", "((int)(u_list[2].x))",
      "for (; (i < u_n); (++i))", NULL },
    NULL },

  { "local_struct_hoist",
    NULL,
    "#version 300 es\nprecision mediump float;\nout vec4 o;\n"
    "void main() { struct P { vec2 a; float b; }; P p = P(vec2(1.0), 2.0); o = vec4(p.a, p.b, 1.0); }\n",
    NULL, { NULL }, { "struct P {", "P _g2c_mk_P(float2 _p0, float _p1)", "P p = _g2c_mk_P(((float2)(1.0)), 2.0);", NULL },
    NULL },
};

static int run_selftests(void) {
  int ok = 0, bad = 0;
  for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
    const selftest *t = &tests[i];
    g2c_result *r = calloc(1, sizeof *r);
    int rc = g2c_translate_ex(t->vs, t->fs, G2C_STRICT, r);
    char why[600] = "";
    if (t->err) {
      if (!rc) snprintf(why, sizeof why, "expected an error containing '%s'", t->err);
      else if (!strstr(r->error, t->err)) snprintf(why, sizeof why, "error '%s' doesn't mention '%s'", r->error, t->err);
    } else if (rc) {
      snprintf(why, sizeof why, "translate failed: %s", r->error);
    } else {
      for (int k = 0; !why[0] && t->vs_has[k]; k++)
        if (!r->vs_cg || !strstr(r->vs_cg, t->vs_has[k])) snprintf(why, sizeof why, "vs output lacks '%s'", t->vs_has[k]);
      for (int k = 0; !why[0] && t->fs_has[k]; k++)
        if (!r->fs_cg || !strstr(r->fs_cg, t->fs_has[k])) snprintf(why, sizeof why, "fs output lacks '%s'", t->fs_has[k]);
      if (!why[0] && t->vs && !r->vs_cg) strcpy(why, "no vs output");
      if (!why[0] && t->fs && !r->fs_cg) strcpy(why, "no fs output");
      if (!why[0] && (t->vs ? 0 : r->vs_cg != NULL)) strcpy(why, "unexpected vs output");
      if (!why[0] && (t->fs ? 0 : r->fs_cg != NULL)) strcpy(why, "unexpected fs output");
      if (!why[0] && t->check) t->check(r, why);
    }
    if (why[0]) {
      printf("FAIL %s: %s\n", t->name, why);
      bad++;
      if (!verbose) {
        if (r->vs_cg) printf("---- vs\n%s", r->vs_cg);
        if (r->fs_cg) printf("---- fs\n%s", r->fs_cg);
      }
    } else {
      printf("ok   %s\n", t->name);
      ok++;
    }
    dump(t->name, r);
    g2c_free(r);
    free(r);
  }
  printf("selftest: %d ok, %d failed\n", ok, bad);
  return bad != 0;
}

int main(int argc, char **argv) {
  int pairs = 0, ok = 0, bad = 0, any = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "-v")) { verbose = 1; continue; }
    if (!strcmp(a, "-o") && i + 1 < argc) { outdir = argv[++i]; continue; }
    if (!strcmp(a, "--lenient")) { flags = 0; continue; }
    if (!strcmp(a, "--selftest")) return run_selftests();
    if (!strcmp(a, "--pairs")) { pairs = 1; continue; }
    if (!strcmp(a, "--pair") && i + 2 < argc) {
      run_pair(argv[i + 1], argv[i + 2], &ok, &bad);
      i += 2;
      any = 1;
      continue;
    }
    any = 1;
    if (!is_dir(a)) {
      run_one(a, &ok, &bad);
      continue;
    }
    char **files = list_dir(a);
    for (int k = 0; files && files[k]; k++) {
      const char *f = files[k];
      if (stage_of(f, NULL) < 0) continue;
      if (!pairs) { run_one(f, &ok, &bad); continue; }
      if (!ends_with(f, ".vert")) continue;
      size_t n = strlen(f);
      char *fp = malloc(n + 1);
      memcpy(fp, f, n - 5);
      strcpy(fp + n - 5, ".frag");
      FILE *t = fopen(fp, "rb");
      if (t) {
        fclose(t);
        run_pair(f, fp, &ok, &bad);
      } else {
        run_one(f, &ok, &bad);
      }
      free(fp);
    }
    free_list(files);
  }
  if (!any) {
    fprintf(stderr, "usage: %s [-v] [-o dir] [--lenient] [--pairs] FILE|DIR... | --pair VS FS | --selftest\n", argv[0]);
    return 2;
  }
  printf("%d ok, %d failed\n", ok, bad);
  return bad != 0;
}
