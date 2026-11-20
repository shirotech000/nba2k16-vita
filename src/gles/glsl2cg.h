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
//
// varyings get TEXCOORDn semantics, assigned by name so VS + FS always match.
// attributes keep their GLSL names as the VS entry param names.

#define GLSL2CG_MAX_ATTRIBS  16
#define GLSL2CG_MAX_UNIFORMS 128
#define GLSL2CG_NAME_LEN     64

typedef enum {
  G2C_FLOAT, G2C_VEC2, G2C_VEC3, G2C_VEC4,
  G2C_INT, G2C_IVEC2, G2C_IVEC3, G2C_IVEC4,
  G2C_BOOL, G2C_BVEC2, G2C_BVEC3, G2C_BVEC4,
  G2C_MAT2, G2C_MAT3, G2C_MAT4,
  G2C_SAMPLER2D, G2C_SAMPLERCUBE,
  G2C_OTHER
} g2c_type;

typedef struct {
  char name[GLSL2CG_NAME_LEN];
  g2c_type type;
  int location;        // layout(location=N) if given, else -1
} g2c_attrib;

typedef struct {
  char name[GLSL2CG_NAME_LEN];
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
  char error[256];
} g2c_result;

// 0 on success. on failure out->error says why. always call g2c_free after.
int g2c_translate(const char *vs_glsl, const char *fs_glsl, g2c_result *out);
void g2c_free(g2c_result *r);
