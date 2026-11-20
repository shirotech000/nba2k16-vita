// softfp <-> hardfp bridges for libm and the aeabi double helpers.
// the game/gnustl pass doubles in r0-r3, our libm wants them in d0-d7.
#include <math.h>
#include <stdint.h>
#include "loader/exports.h"
#include "util/abi.h"

#define WRAP1(f) static SOFTFP double w_##f(double x) { return f(x); }
#define WRAP2(f) static SOFTFP double w_##f(double x, double y) { return f(x, y); }

WRAP1(acos) WRAP1(asin) WRAP1(atan) WRAP1(ceil) WRAP1(cos) WRAP1(cosh)
WRAP1(exp) WRAP1(floor) WRAP1(log) WRAP1(log10) WRAP1(sin) WRAP1(sinh)
WRAP1(sqrt) WRAP1(tan) WRAP1(tanh)
WRAP2(atan2) WRAP2(fmod) WRAP2(pow)

static SOFTFP double w_frexp(double x, int *e) { return frexp(x, e); }
static SOFTFP double w_modf(double x, double *i) { return modf(x, i); }
static SOFTFP double w_ldexp(double x, int e) { return ldexp(x, e); }

// float flavors, in case anything dlsym's them
static SOFTFP float w_sinf(float x) { return sinf(x); }
static SOFTFP float w_cosf(float x) { return cosf(x); }
static SOFTFP float w_sqrtf(float x) { return sqrtf(x); }
static SOFTFP float w_powf(float x, float y) { return powf(x, y); }
static SOFTFP float w_atan2f(float y, float x) { return atan2f(y, x); }

// RTABI helpers. these are always base-AAPCS by spec, gnustl doesn't
// carry these ones so we provide them.
static SOFTFP double a_dmul(double a, double b) { return a * b; }
static SOFTFP double a_dsub(double a, double b) { return a - b; }
static SOFTFP double a_f2d(float f) { return f; }
static SOFTFP double a_l2d(int64_t v) { return (double)v; }
static SOFTFP double a_ul2d(uint64_t v) { return (double)v; }
static SOFTFP double a_ui2d(uint32_t v) { return (double)v; }
static SOFTFP int64_t a_d2lz(double d) { return (int64_t)d; }
static SOFTFP uint64_t a_d2ulz(double d) { return d <= 0 ? 0 : (uint64_t)d; }
static SOFTFP uint32_t a_d2uiz(double d) { return d <= 0 ? 0 : (uint32_t)d; }

// pure integer, ABI neutral, comes straight from libgcc
extern void __aeabi_ldivmod(void);

EXPORT_TABLE(math) = {
  EXP_AS("acos", w_acos), EXP_AS("asin", w_asin), EXP_AS("atan", w_atan),
  EXP_AS("atan2", w_atan2), EXP_AS("ceil", w_ceil), EXP_AS("cos", w_cos),
  EXP_AS("cosh", w_cosh), EXP_AS("exp", w_exp), EXP_AS("floor", w_floor),
  EXP_AS("fmod", w_fmod), EXP_AS("frexp", w_frexp), EXP_AS("log", w_log),
  EXP_AS("log10", w_log10), EXP_AS("modf", w_modf), EXP_AS("pow", w_pow),
  EXP_AS("sin", w_sin), EXP_AS("sinh", w_sinh), EXP_AS("sqrt", w_sqrt),
  EXP_AS("tan", w_tan), EXP_AS("tanh", w_tanh), EXP_AS("ldexp", w_ldexp),
  EXP_AS("sinf", w_sinf), EXP_AS("cosf", w_cosf), EXP_AS("sqrtf", w_sqrtf),
  EXP_AS("powf", w_powf), EXP_AS("atan2f", w_atan2f),
  EXP_AS("__aeabi_dmul", a_dmul),
  EXP_AS("__aeabi_dsub", a_dsub),
  EXP_AS("__aeabi_f2d", a_f2d),
  EXP_AS("__aeabi_l2d", a_l2d),
  EXP_AS("__aeabi_ul2d", a_ul2d),
  EXP_AS("__aeabi_ui2d", a_ui2d),
  EXP_AS("__aeabi_d2lz", a_d2lz),
  EXP_AS("__aeabi_d2ulz", a_d2ulz),
  EXP_AS("__aeabi_d2uiz", a_d2uiz),
  EXP(__aeabi_ldivmod),
};
EXPORT_TABLE_END(math);
