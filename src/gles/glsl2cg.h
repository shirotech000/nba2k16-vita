#pragma once
// GLSL ES (1.00 / 3.00) -> Cg translator, feeds SceShaccCg.
//
// matrix convention (important for the GL side):
//   every matrix uniform becomes an array of float4 "columns" in Cg and gets
//   rebuilt inside the shader, so uploads are dead simple: copy each GLSL
//   column into its own float4 slot (pad mat2/mat3 columns with zeros).
//   same deal for *arrays* of float/vec2/vec3: one float4 slot per element.
//   plain (non-array) float/vec2/vec3/vec4/int/bool uniforms stay native.
//   inside the shader GLSL `A * B` with matrices turns into mul(B, A), since
//   Cg rows == GLSL columns with this layout.
//   arrays of int/ivec/bool/bvec also use float4 slots (one per element), so
//   upload those as floats; the shader casts back on read.
//   vec4 arrays are plain float4[N] (same thing as the slot layout).
//
// varyings get TEXCOORDn semantics, assigned by name so VS + FS always match.
// (translate both stages in one call for that; a lone stage just numbers its
// own varyings in declaration order.)
// attributes keep their GLSL names as the VS entry param names. int attribs
// arrive as float params and get cast in the shader.
// samplers keep their names (samplerCube is spelled samplerCUBE in Cg).
// builtins: gl_FragCoord=WPOS (no y flip), gl_FrontFacing=FACE sign-corrected
// by the GL layer via the generated "_g2c_ffs" uniform (see gl_state.c),
// gl_PointCoord=SPRITECOORD, gl_PointSize=PSIZE, gl_InstanceID=INSTANCE,
// gl_VertexID=INDEX, gl_FragDepth=DEPTH, frag output = COLOR.

#define GLSL2CG_MAX_ATTRIBS  16
#define GLSL2CG_MAX_UNIFORMS 128
#define GLSL2CG_NAME_LEN     64

// flags for g2c_translate_ex
#define G2C_STRICT 1u  // fail on any GLSL type mismatch (testing aid)

typedef enum {
  G2C_FLOAT, G2C_VEC2, G2C_VEC3, G2C_VEC4,
  G2C_INT, G2C_IVEC2, G2C_IVEC3, G2C_IVEC4,
  G2C_BOOL, G2C_BVEC2, G2C_BVEC3, G2C_BVEC4,
  G2C_MAT2, G2C_MAT3, G2C_MAT4,
  G2C_SAMPLER2D, G2C_SAMPLERCUBE,
  G2C_OTHER            // non-square matrices (vec4_slots = column count)
} g2c_type;

typedef struct {
  char name[GLSL2CG_NAME_LEN];
  char cg_name[GLSL2CG_NAME_LEN]; // name in the gxp, differs if the GLSL one clashes with Cg
  g2c_type type;
  int location;        // layout(location=N) if given, else -1
} g2c_attrib;

typedef struct {
  char name[GLSL2CG_NAME_LEN];
  char cg_name[GLSL2CG_NAME_LEN]; // look the gxp param up by THIS one (e.g. "Texture" -> "g2cu_Texture")
  g2c_type type;
  int array_size;      // 1 if not an array
  int vec4_slots;      // 1 = native layout, >1 = float4 slot array (matrices / arrays)
  int in_vs, in_fs;    // which stage actually declares it
} g2c_uniform;

typedef struct {
  char *vs_cg;         // malloc'd, NUL terminated
  char *fs_cg;
  int n_attribs;
  g2c_attrib attribs[GLSL2CG_MAX_ATTRIBS];
  int n_uniforms;
  g2c_uniform uniforms[GLSL2CG_MAX_UNIFORMS];
  int uses_frag_coord, uses_point_size, uses_instance_id;
  int uses_vertex_id, uses_front_facing, uses_point_coord, uses_frag_depth;
  char error[256];
} g2c_result;   // ~12KB, don't put it on a small thread stack

// 0 on success. on failure out->error says why. always call g2c_free after.
// either source may be NULL (translate just the other stage).
int g2c_translate(const char *vs_glsl, const char *fs_glsl, g2c_result *out);
int g2c_translate_ex(const char *vs_glsl, const char *fs_glsl, unsigned flags, g2c_result *out);
void g2c_free(g2c_result *r);

// build stamp of the translator, part of the translation cache key.
// it's G2C_BUILD in shader.c (CMake-time SHA1 of glsl2cg.c + glsl2cg.h,
// see glsl2cg.c for the note), not __DATE__/__TIME__ anymore
