# Port notes

Running notes on what the game needs and where the port is at. Newest stuff goes at the bottom of the log.

## The game binary (v0.0.29)

- `libnba2k16_clean_opt.so`: 27 MB, ARM EABI5, **softfp** (no hard-float flag in `e_flags`)
  - text 0x0–0x15551ac (RX), data 0x1556460–0x274deA5 (RW, ~13 MB of that is bss), so the whole image is ~40 MB
  - relocs: 123,991 RELATIVE, 650 ABS32, 20 GLOB_DAT, 370 JUMP_SLOT. No TLS relocs.
  - 686 constructors in `.init_array`, which all run before `JNI_OnLoad`
  - entry points: `JNI_OnLoad`, `ANativeActivity_onCreate` (it's a NativeActivity game using android_native_app_glue), plus a few `Java_..._vcbootbridge_*` / `IAPBridge_*` natives the Java side would normally call
  - NEEDED: liblog, libandroid, libOpenSLES, libEGL, **libGLESv3**, libgnustl_shared, libz, libcrypto, libcurl, libssl, libstdc++, libm, libc, libdl
- `libgnustl_shared.so`: 1.2 MB, gets loaded as a second module. It provides all 89 of the game's C++ imports plus the unwinder/`__cxa_*`, and it needs `__gnu_Unwind_Find_exidx` from us.
- 392 undefined symbols in the game, 113 in gnustl. All of them are covered by shim tables or gnustl now (anything missing gets a logging trap).
- **Shaders:** 82 VS + 82 FS embedded in the .so as `#version 300 es` source. No uniform blocks. Instancing is used.
- **Textures:** no compression extension strings are probed, so it's probably core ETC2 in GLES3. The SGX can't sample that, so it means CPU decode. Gets confirmed once the GL layer logs formats.
- **Data:** uses Google Play APK expansion files (OBB). There's also a `nativeValidateBinFile` native that probably checks the data.

## ABI gotchas (bionic vs newlib)

| thing | bionic | vitasdk newlib | handled in |
|---|---|---|---|
| float args/returns | softfp | hardfp | `SOFTFP` wrappers (math.c, gl, jni, input) |
| `pthread_mutex_t`/`cond_t` | 4 bytes, static init 0 / 0x4000 / 0x8000 | 4 bytes (pointer), static init -1 | lazy alloc in pthread.c |
| `pthread_once_t` | int | 16 bytes | own impl |
| `pthread_attr_t` | 24 bytes | 4 bytes | own impl |
| `time_t` / timespec / timeval | 32-bit | 32-bit too | pass through |
| `struct stat` | 104 bytes (stat64 layout) | 40 bytes | converted |
| `struct tm` | 44 bytes | 36 bytes | own statics |
| `mbstate_t` | 4 bytes | 8 bytes | trivial C-locale impls, never pass the game's state to newlib |
| `FILE`/`__sF` | 84-byte FILE, `__sF[3]` | 104-byte FILE | fake `__sF`, translated in stdio.c |
| `_ctype_` | `const char*` to a 1+256 table | array | own table |
| `O_*` flags | linux | newlib | translated |
| errno | linux values | newlib values | the common ones match, `ETIMEDOUT` from cond waits gets translated |

## Memory

Extended memory mode (`ATTRIBUTE2=12`) with a 224 MB newlib heap. On top of that:
- ~41 MB of .so images via kubridge
- GPU memory: CDRAM first, then LPDDR
- thread stacks (1 MB default per game thread)

Whether 2K16 fits at all is the big open question. `malloc` is instrumented (live/peak/biggest/fails), so check `port.log`.

## Rules for this repo

- No game files, game shader text, or other dumped content goes in git.
- No code from vitaGL / vitashark / Rinnegatamante's loaders. vitaGL is OK as a behavior reference for shader quirks, but nothing gets lifted.

## Log

- **2026-11-20**: Analyzed the binaries and set up the repo. Wrote the loader (kubridge reserve/commit/protect), import registry with trap stubs, bionic shims, fake NativeActivity/looper/input, fake JNI, and curl/ssl stubs. The GLES→GXM layer, GLSL→Cg translator and OpenSL ES are written in parallel.
