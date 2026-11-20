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

Budget, measured in Vita3K with `ATTRIBUTE2=12` (it matches mbaavita's notes):
- **~365 MB main**: newlib heap (288 MB now) + .so images (~41 MB) + thread stacks + whatever's left
- **112 MB CDRAM**: framebuffers, textures, static GPU buffers
- **26 MB phycont**: a separate pool. GXM rings and other non-CDRAM GPU memory go here first (`gxm.c` blk_alloc).

What the game wants (at the startup screens, 60s in):
- **game heap** (VCHEAP2 from get_global_heap, patched to 176 MB): peak **133.5 MB** used
- **the game's plain mallocs** on top: ~65 MB and climbing slowly
- our side (GL objects, shader blobs, translator, Cg compiler) is small. The compiler never goes past 4 MB.

`game_heap_mb=` in config.txt overrides the split (default = heap - 112 MB). The port.log monitor prints game heap total/free/peak + malloc stats every 5 s. Gameplay will want more than the menus. If the game heap fills up, the next levers are native ETC1 sampling (instead of ETC→RGBA8 at 8x the size) and moving more stuff into CDRAM.

## Rules for this repo

- No game files, game shader text, or other dumped content goes in git.
- No code from vitaGL / vitashark / Rinnegatamante's loaders. vitaGL is OK as a behavior reference for shader quirks, but nothing gets lifted.

## Game patches (src/game/patches.c)

Every patch checks the original bytes first.

- `get_global_heap()` @ +0x786A6C: grabs one **400 MB** block for the whole game heap (`mov r1/r2, #0x19000000` at +0x786AA8 / +0x786B3C). We shrink it to `heap - 64MB` (176 MB right now). Override it with `game_heap_mb=` in `ux0:data/nba2k16/config.txt`. Whether the game actually fits in that is unknown until the data loads.

## Other loader-level fixes

- **kuser helpers:** the Google Play Games SDK bits in the game keep function pointers to Linux's kuser helpers (`0xffff0fa0` dmb, `0xffff0fc0` cmpxchg), 19 of each in .data. The loader swaps any such word in a module's data segment for our own versions (src/loader/kuser.S).
- **OBB name:** comes from `getVersionCode()`, which is 29 for v0.0.29, so the file is `main.29.com.t2ksports.nba2k16android.obb`. Probe order: `/sdcard/` first, then the obb dir.

## Vita3K notes

- kubridge HLE supports reserve/commit/protect. `kuKernelFlushCaches` is a missing NID (harmless there). Only data aborts reach our exception handler.
- `port.log` loses its tail when Vita3K is killed. `tools/vita3k/run.ps1` reads the `sceClibPrintf` copy out of Vita3K's stdout instead. If Vita3K dies hard, even that loses the tail, so put `sync_log=1` in config.txt (slow, reopens the log per line).
- **libshacccg crashes Vita3K on some shaders.** This is the same class of problem seen in PSPMania/mbaavita. `shader.c` has a crash guard: it writes `<key>.compiling` + `<key>.cg` before compiling and deletes the marker after. A marker left over from a dead run turns into `<key>.bad`, and that shader gets skipped from then on. Delete the `.bad` files to retry them. Caveat: if the compiler died because the heap was empty, the shader gets blamed anyway.
- Run `tools/vita3k/run.ps1` from the repo. It defaults to `gamefiles/` for the libs + obb.

## Log

- **2026-11-20**: Analyzed the binaries and set up the repo. Wrote the loader (kubridge reserve/commit/protect), import registry with trap stubs, bionic shims, fake NativeActivity/looper/input, fake JNI, and curl/ssl stubs. The GLES→GXM layer, GLSL→Cg translator and OpenSL ES are written in parallel.
- **2026-11-20**: First boots in Vita3K with GL stubbed out. Fixed the 400 MB heap (patch) and the kuser helper crash. JNI needed ClassLoader.loadClass, the permission checks and getVersionCode. Now all 686 ctors run, then `JNI_OnLoad` → `onCreate` → `android_main` thread → full lifecycle → the game looks for the OBB → no OBB → the game asks for the Java expansion downloader → `exit(0)`. **Blocked on game data.**
- **2026-11-20, with the obb + the real GXM renderer:** fixed 64-bit file io (sceIo), fake /proc/cpuinfo, and the Cg keyword clash (`Texture`). In Vita3K the game now:
  - inits EGL
  - compiles + links ~260 programs from its own shaders (GLSL → glsl2cg → SceShaccCg, cached as .gxp)
  - renders its first frames (FBOs + clears + draws)
  - loads data from the obb
  - sets up audio, reaches IAP init and its Config.cfg lookup at ~14 s (shaders cached)
  
  Then Vita3K itself dies hard with no log, right after `IAPBridge.getInstance` (returns NULL from our fake JNI) while the game polls `files/Config.cfg`. **Next:** figure out whether that's a guest bug (probably IAP / config handling) or an emulator issue, and check on real hardware.
- OpenSL: PCM buffer-queue players work. The game also wants an AAC-ADTS decoder player (probably music/commentary streams), which gets refused for now. sceAudiodec can do AAC, that's for later. Took ALC-off and core pinning from PSPMania's audio driver.
