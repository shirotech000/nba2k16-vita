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

**Update, past the Config.cfg step (~15 s), the 288 MB heap isn't enough.** Right after Config.cfg the async loader (`VCASYNCTHREAD` → `VCSTATICCONTEXT::Create` → scene/texture/model `MakeAbsoluteWithVramOffsetTranslation`) mallocs ~43 MB in about 1 s, mostly 1 MB and 4 MB "VRAM" blocks through `VCHEAPWRAPPER::Allocate`. newlib runs dry (`malloc(13411) FAILED`, live 289.5 MB, arena free 0) at ~15.4 s. The game then crashes on the NULL and Vita3K exits ~5 s later. That's the same death in every run, dry run included.
- 288 total with `game_heap_mb=160` (+16 MB for malloc): same OOM, same spot. The total is what's short, not the split.
- **Experiment:** built with `-DNBA_HEAP_MB=316` (`build_heaptest/`, cmake `-DCMAKE_C_FLAGS=-DNBA_HEAP_MB=316`) plus `game_heap_mb=160`, so malloc gets 156 MB. It ran 120 s in Vita3K and was still rendering menus at the end (frame 6191). Peak newlib live was **314 MB**.
  - From ~45 s the game heap fills (peak used 171 MB) and `VCHEAP2::GrowMemory` grows it in 4 MB steps out of malloc (via `global_new_handler`, from `PlayerUniform_CreatePrebakeJerseyTextures` / `VCTexture_GenerateMipMaps`). Grows that fail (4 MB at 49–58 s, 12 MB at 64 s) get absorbed by the game, which goes on. By 60 s it drops back to 288.8 MB live.
  - free user memory was only **8 MB** with the 316 heap (36 MB with 288). Not tried on hardware, and if the heap block doesn't fit there, it won't even reach main. So the default stays 288 for now.
- The monitor now also prints the gxm line (CDRAM/LPDDR use + free user/cdram/phycont). CDRAM had **~67 MB free** at 120 s (47 MB of blocks, 34 MB peak used). That's the obvious spill target for big "VRAM" mallocs: CPU access is slower, but those blocks only get read once at upload.

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
- **libshacccg crash guard:** the first runs died right after a shader cache miss, so `shader.c` writes `<key>.compiling` + `<key>.cg` before compiling and deletes the marker after. A marker left over from a dead run turns into `<key>.bad`, and that shader gets skipped from then on. Delete the `.bad` files to retry them. That crash hasn't come back since. The `.bad` files we did get were false positives: the test script had killed Vita3K mid-compile. run.ps1 now wipes `*.compiling` after a timeout kill. Also, if the compiler dies because the heap ran dry, the shader gets blamed anyway.
- Run `tools/vita3k/run.ps1` from the repo. It defaults to `gamefiles/` for the libs + obb. Set `NBA_VITA3K` (path to Vita3K.exe) and `NBA_SHACCCG` (your dumped libshacccg.suprx).
- **vita3k.flag:** run.ps1 drops `ux0:data/nba2k16/vita3k.flag` into the emulator's profile, and nowhere else. `emu_is_vita3k()` (src/util/paths.c) checks for it once at boot and logs `vita3k.flag found`. It's the hook for emulator-only workarounds. Nothing uses it yet, because the fixes below are correct on hardware too.
- **Vita3K exits hard on a mkdir with a ':' inside the path** (e.g. `ux0:data/nba2k16/internal/files/ux0:data`). The whole process just exits: no guest fault, no "PC is", and the log tail is gone. The last thing vita3k.log managed to flush was `_sceIoMkdir: Creating new dir ux0:data/nba2k16/internal/files/ux0:data`. Refusing those calls on our side made the death go away. The exact host mechanism isn't confirmed; most likely the host fs call throws on a Windows path with ':' in it. A real Vita just returns an error there. Every path shim now goes through `path_fix_checked()`, which refuses such paths (ENOENT, logs the first 16 + one backtrace).
- After a guest malloc failure, Vita3K logs `exception_handler: Write violation at address 0x0` (host side). The emulator then stalls for ~5 s and exits, every time. Not chased further, the OOM is the real problem (see Memory).
- Vita3K refuses `sceKernelChangeThreadCpuAffinityMask` with the SYSTEM core bit, so `cpu_init()` logs stock caps (3 cores) there.

## Debug switches (ux0:data/nba2k16/config.txt, key=value)

- `sync_log=1`: reopen port.log after every line, so it survives a hard emulator death
- `no_crash_handler=1`: don't hook exceptions, let Vita3K / the Vita report faults itself
- `no_draw=1`: skip every draw. Clears and swaps still happen.
- `gpu_dry_run=1`: nothing reaches the GPU at all. No scenes, so no draws or clears either, plus no flips and no sceGxmFinish. eglSwapBuffers just paces at ~60 Hz. Texture uploads and shader compiles still run (CPU side). Use it to bisect emulator-only renderer trouble.
- `no_audio=1`: never open an audio port. The OpenSL mixer runs on a silent clock.
- `frame_log=1`: log draw/clear counts for every frame
- `game_heap_mb=N`, `version_code=N`
- `disp_buf=2/3` (default 3): display buffer count. 2 saves 2 MB of cdram; with vsync the display queue already paces at 60 Hz so the cost should be CPU stalls when the extra in-flight frame would have soaked latency, check the `display queue` line in the frame monitor.
- `msaa=0/2/4` (default 0 for now, main 3D target), `msaa_display=1`, `cube_dummy=1`, `async_shaders=0`
- `fbo_cull_flip=0`, `stencil_swap=1`: winding/stencil checks
- `capunlocker=0`: don't use core 3 even if CapUnlocker is loaded

## CPU cores

At boot `cpu_init()` (src/util/cpu.c) asks for the system core (`SCE_KERNEL_CPU_MASK_SYSTEM`). A stock Vita refuses; with the CapUnlocker plugin it works. This is the same probe as the user's Flycast/MBAA ports. When it works:
- every game thread (anything going through our pthread_create) gets cores 0-3
- sysconf and the fake /proc/cpuinfo say 4 cores, so the game's job system can size itself to that

Without it, the game sees 3. The main thread keeps its default mask, and the audio mixer stays pinned to core 2.
- **Render thread core (added):** every pthread gets cores 0-3, so core 3 was shared by every helper plus the render thread. Now the first thread that makes a GL context current (`eglMakeCurrent`) is treated as the render thread and pinned to core 3 alone (`cpu_pin_render()`, src/util/cpu.c; called from egl.c). Threads spawned after that are placed on 0-2 only. Threads spawned before it (loaders, the shader worker, audio on core 2) keep their old placement. Log line: `cpu: render thread pinned to core 3 alone`.

## Directory layout on the Vita

The game wipes files in what it thinks are android's data dirs on boot. The first OBB run took our .so files and config.txt with it. So it gets its own subfolders:
- `internal/`: /data/data/<pkg>, with files/ and cache/ inside
- `external/`: /sdcard/Android/data/<pkg>
- `root/`: any other absolute path, e.g. /proc/cpuinfo
- `obb/`: /sdcard/Android/obb/<pkg>
- `sdcard/`: plain /sdcard (getExternalStorageDirectory). The game probes for the obb here first.
- our own files sit at the top level: the libs, config.txt, port.log, shadercache/

**The game only ever sees android paths.** `internalDataPath`, `externalDataPath`, `obbPath` and the JNI File getters (getFilesDir etc.) hand out `/data/data/<pkg>/files`, `/sdcard/Android/data/<pkg>`, `/sdcard/Android/obb/<pkg>` and so on, and `path_fix` maps them back to the folders above. Don't hand it `ux0:` strings. `VCANDROIDFILEDEVICE::CreateChar8FullPathName` treats anything that doesn't start with '/' as relative and glues its current folder in front, so `ux0:data/...` turned into `.../files/ux0:data/...`. That's what killed Vita3K at ~15 s. Side effect: its recursive mkdir now also creates `sdcard/Android/data/` and `root/data/data/` (it walks every component of the android path). Those are harmless empty dirs.

## Log

- **2026-11-20**: Analyzed the binaries and set up the repo. Wrote the loader (kubridge reserve/commit/protect), import registry with trap stubs, bionic shims, fake NativeActivity/looper/input, fake JNI, and curl/ssl stubs. The GLES→GXM layer, GLSL→Cg translator and OpenSL ES are written in parallel.
- **2026-11-20**: First boots in Vita3K with GL stubbed out. Fixed the 400 MB heap (patch) and the kuser helper crash. JNI needed ClassLoader.loadClass, the permission checks and getVersionCode. Now all 686 ctors run, then `JNI_OnLoad` → `onCreate` → `android_main` thread → full lifecycle → the game looks for the OBB → no OBB → the game asks for the Java expansion downloader → `exit(0)`. **Blocked on game data.**
- **2026-11-20, with the obb + the real GXM renderer:** fixed 64-bit file io (sceIo), fake /proc/cpuinfo, and the Cg keyword clash (`Texture`). In Vita3K the game now:
  - inits EGL
  - compiles + links ~260 programs from its own shaders (GLSL → glsl2cg → SceShaccCg, cached as .gxp)
  - renders its first frames (FBOs + clears + draws)
  - loads data from the obb
  - sets up audio, reaches IAP init and its Config.cfg lookup at ~14 s (shaders cached)
  
  Then, ~15 s in with a warm shader cache (~58 frames rendered), Vita3K itself exits hard with no log. That's right after the game's file layer runs its 33x retry on `files/Config.cfg` (open + `VCANDROIDFILEDEVICE::DeleteFile_LowLevel`). That retry is normal: `vcfeaturecodes.dbg` gets the same 32 tries earlier and the game moves on. So the death is in whatever comes next.
  - not draws: `no_draw=1` still dies
  - not audio output: `no_audio=1` still dies
  - not our crash handler: `no_crash_handler=1` still dies
  - **Next:** trace imports right before the death, and check on real hardware.
  - **Solved:** it was our paths. We handed the game `ux0:` strings as `internalDataPath` etc. The game's file layer treats anything without a leading '/' as relative, so after the Config.cfg miss its recursive CreateFolder asked for `mkdir("ux0:data/nba2k16/internal/files/ux0:data")` (33 tries per component), and Vita3K exits on that mkdir. vita3k.log's last flushed line was exactly that `_sceIoMkdir`. Fixed by handing out android paths everywhere (see Directory layout), and every path shim now refuses ':' junk (`path_fix_checked`). The game now writes Config.cfg and moves on. Then it runs out of malloc at ~15.4 s, see Memory.
- **2026-11-20, after the path fix:**
  - new switch `gpu_dry_run=1`: no GPU submission at all. The OOM death happens with it too, so the renderer isn't involved.
  - `vita3k.flag` plumbing (run.ps1 + `emu_is_vita3k()`)
  - JNI `Settings.Secure.getString` (android_id) now returns a fixed fake id instead of NULL. With NULL, Vita3K logged 6 guest null reads (host `0x400000000`) at ~30 s. Those are gone now. Real hardware would have taken a data abort there.
  - With a 316 MB heap test build, the game runs 120 s+ in Vita3K, rendering menus (up to ~117 draws/frame). **Next blocker: memory** (see Memory), and the AAC decoder player.
- **2026-11-20, gameplay reached:**
  - **Memory:**
    - The overflow allocator (`src/bionic/bigmem.c`) puts the game's big blocks in free main memory first, then cached CDRAM.
    - Its reserves are 4 MB main / 6 MB CDRAM.
    - A big allocation that fits nowhere waits up to 2 s before failing.
    - The GL pool uses 2 MB chunks and hands empty ones back.
    - Result: quick match loads and plays, ~30-48 fps in Vita3K. Live memory sits flat at ~329 MB in a match (no leak). One run still died after ~8 min on a sudden 4 MB request. **Next memory lever: ETC2 -> BC1/BC3 transcoding** (we expand ETC to RGBA8 today, 4-8x the size). That's in progress.
  - **Shaders:**
    - compile on a background worker (draws with a still-compiling program get skipped);
    - the Cg compiler gets an 8 MB fallback arena when malloc fails;
    - "out of memory" / "fatal internal error" compiles retry;
    - glsl2cg output is cached (`shadercache/t*.g2c`, keyed by the GLSL pair + the translator build stamp), so cached links run inline. That fixed the laggy intro logo fade (the game links ~200 programs there).
  - **Rendering:**
    - real cube maps, mip chains, sRGB, stencil only where the target has one;
    - **4x MSAA on the main 3D target** (mbaavita-style: sample-mode fragment programs, downscale-on-store color, sample-sized depth twin, +8 MB CDRAM). `msaa=0/2/4`, `msaa_display=1`. **Off by default for now**: with it on, the match load hung at 96% (`malloc(4194315) FAILED`, main + cdram both at their reserves). Turn it back on once textures shrink (ETC2→BC transcoding).
    - bigmem now also takes 1 MB+ blocks from phycont (keeps 2 MB for gxm). The 96% hang had ~8 MB sitting idle in there.
  - **Player skin rendered green in Vita3K**: a Vita3K shader recompiler bug, not ours. With a one-component dest mask it only checks swizzle channel 0 for an immediate-bank constant, so `pa0.y = 1 * base.x` reads 0 and red dies. `link_back` patches the gxp (constant copied into channel 0) under Vita3K only; `v3k_gxp_fix=0` turns it off. Real hw never had it. (`cube_dummy=1` ruled out cube maps.)
  - **Game-side perf leak:** `DeleteFile_LowLevel` retries unlink 33x on any failure and mid match the game sweeps cache files that mostly don't exist (~1100 failed removes/s, each logged). Patched to one try; missing-file unlinks aren't logged past 8.
  - **Frame timing** in the 5 s monitor: avg frame ms, cpu stalls on the gpu, display queue wait, draws and scenes per frame. `prof=1` also times the draws.
- **ETC -> BC1/BC3 at upload (tex_bc, on by default):** swizzled BC in morton block order, NPOT via SWIZZLED_ARBITRARY. RGB ends up at 1/8 of RGBA8, RGBA at 1/4. Cube maps and sRGB stay RGBA8. Confirmed on hw: menu textures look right, and the white-texture/OOM screens went away. Switches: `tex_bc=0`, `tex_bc_srgb=1`, `tex_bc_npot=0`.
- **Hw runs 2-3:** a cold shader cache stalled the compile queue at 230+ (draws skipped, only portraits visible). Fixes: worker prio 0x9A, plus Vita3K's gxp cache copied over to the Vita (same libshacccg, keyed by the Cg hash, so it's valid). Without BC the match load ran ~71 MB of textures out of GPU memory, then a NULL from malloc crashed the game's load thread at 87%. Head meshes went missing when their VBOs couldn't get GPU memory ("dead buffer").
- **First real hw run (2026-11-20):** loader, relocs, patches and CapUnlocker detection all fine, then a data abort 3.5 s in. Our `so_backtrace` (logging the 176 MB heap alloc) scanned 512 words up from its frame and ran off the top of the thread stack into an unmapped page. Vita3K maps memory past stacks, so it never showed. Now bounded by the stack's memblock.
  - The eboot loads **0x7f000 above its link address** (extended memory mode). Boot logs `eboot: main @ ...`; subtract `nm`'s address of `main` to get the slide for addr2line. Crash dumps (`ux0:data/psp2core-*.psp2dmp`, gzip'd ELF core) have the real base in their MODULE_INFO note.
  - hw boot memory: free user 70656 KiB, cdram 114688 KiB, phycont 26624 KiB (288 MB newlib heap already taken). Matches mbaavita's extended-mode numbers.
  - From mbaavita's hw runs: card io is slow (sceIoOpen/Getstat several ms, per-line open/close ~12 ms), CPU reads from uncached CDRAM are very slow (copy rows to cached ram), `--pic-veneer` keeps thumb->arm stubs pc-relative (ours already were).
  - Deploying: see VitaDrop notes below.
  - **Screenshots:** `shot.flag` dumps the back buffer, but Vita3K doesn't write rendered frames back to guest memory, so it comes out black there (works on hw). In Vita3K, PrintWindow on the Vita3K window works instead.
- OpenSL: PCM buffer-queue players work. The game also wants an AAC-ADTS decoder player (probably music/commentary streams), which gets refused for now. sceAudiodec can do AAC, that's for later. Took ALC-off and core pinning from PSPMania's audio driver.

- **2026-11-20, OOM null-check audit (src/gles, src/android):** audited every malloc/calloc/strdup/realloc in src/gles/*.c and src/android/*.c. Most were already checked; fixed 5 unchecked ones (all NULL-deref under oom):
  - shader.c:583 compile output copy (`p = malloc(out->programSize)` memcpy'd straight away) — likely the hw8 crash PC 0x810BDEE4 (eboot, far 0x0, right after a shader-cache read + inline compile); now logs "out of memory copying a compiled program" and fails the compile.
  - shader.c glCreateShader: `calloc` for gl_shader unchecked; now GL_ERR(GL_OUT_OF_MEMORY), returns 0 like GL.
  - shader.c prog_reset: `calloc` for the prog_garbage node unchecked; now leaks the old programs instead (safe: job_detach just ran, no queue thread can use them).
  - jni.c get_method/get_field: `calloc` for jmeth/jfield unchecked; now fatal() like the other jni oom paths.
  Everything else (tex_realloc, gpu_alloc, tcache, amalloc longjmp, opensles, egl, input, looper, aac) already checks or can't fail; opensles dec_job and the patcher callbacks were checked by inspection.

## Real hardware testing (VitaDrop)

The Vita runs VitaDrop, an HTTP server on port 80 (`vitadrop.local`). It only answers while the app is open.
- Upload: `curl -X POST -T <file> -H "X-File-Name: <name>" -H "X-Upload-Dir: <url-encoded dir>" http://<vita>/api/upload` (raw body; `-T` streams, so the 2 GB OBB works).
- List: `GET /api/ls?path=<dir>/`. A missing dir also returns `[]`, so look for it in its parent's listing.
- Download: `GET /api/dl?path=<file>`. Mkdir: `POST /api/mkdir?path=<url-encoded dir>`.
- First install: VPK to `ux0:data/`, installed by hand in VitaShell. After that, only `ux0:app/NBAK00016/eboot.bin` gets replaced (app closed, no reboot needed). Game data: `ux0:data/nba2k16/` (libs) and `obb/`.
- Always md5 the readback, push a saved copy of a finished build, keep old eboots for rollback, and pull `port.log` before the next launch (each boot truncates it).
- **2026-11-20: shadercache pack** (`shadercache/pack.bin`): all the .gxp/.g2c cache entries sit in ONE file with a name-sorted index, opened once at boot (`pack_init()`, logs `shader: shadercache pack up (N entries)`), read with `sceIoPread`. Built by `tools/shaderpack.py <shadercache-dir>` from the loose files (they stay as fallback: pack first, loose second, and misses still get written loose until the next pack run). Cuts ~750 sceIoOpen+getstats per boot down to one open. Verified in Vita3K: pack boot takes a full 0-g misses run to 0, libshacccg never even loads.
- **Translation-cache key** is now `G2C_BUILD` (CMake-time SHA1 of glsl2cg.c + glsl2cg.h, one combined hash) instead of `__DATE__ " " __TIME__`. Plain rebuilds no longer wipe shadercache/t*.g2c; only translator changes do. One-time cost: the first boot after this change retranslates everything.
- **2026-11-20, core 3 crowding fix:** see CPU cores above. Verify on hw via the "cpu: render thread pinned to core 3 alone" line in port.log, and check the next run's monitor frame ms / display queue line.
- **2026-11-20, GrowMemory shrink:** VCHEAP2::Init stores the minimum growth chunk into [heap+0x6c] with one `mov r2, #0x400000` at +0x132AA08; GrowMemory (+0x1329718, only called from VCHEAP2::Allocate) sizes every new chunk as max(request+overhead, [heap+0x6c]), and the VCHEAPINTERFACE header adds the 11 bytes (that's the `malloc(4194315)` in the logs = 4 MB + 11). Only Init writes that field, so it's a one-instruction patch. New patch in src/game/patches.c shrinks the floor to **1 MB** (`heap_grow_mb=` overrides, 1..64, must be an ARM modify-immediate, so power-of-two). Requests > 1 MB still grow by their exact size; this only trims the dead tail of partly-used chunks. hw long session grew ~9-12 × 4 MB chunks, so expect ~15-35 MB of live slack saved in a match. Don't go below 1 MB — per-chunk bookkeeping then costs more than it saves. Untested on hw.
- **2026-11-20, GPU pool texture compaction (gxm.c):** match loads leave half-empty 2 MB texture chunks behind and nothing reclaimed them while live textures were scattered through them. Design + implementation:
  - `gpu_block` carries `align`, a per-chunk list pointer (`cnext`, maintained by `gpu_alloc`/`gpu_free`) and a `movable` + `owner` tag. Only texture blocks get tagged (gl_tex.c at both alloc sites); buffers, renderbuffers, depth, patcher blocks and dummies stay put because their gxm surfaces keep raw pointers. Chunks with outstanding `gxm_borrow` handles (raw pointers held across threads) are skipped outright (`c->borrows`).
  - `gpu_compact(ti)` (runs with the pool lock held) walks the shared chunks of one type: chunks that are empty, dedicated, borrowed-from, or hold immovable blocks are skipped (a chunk only helps if it can end up empty). Every movable block memmoves into whichever other chunk of the same kind has the most free space, then the block's `off`/`ptr` are updated and `gl_tex_moved(owner)` re-runs `tex_desc()` so the `SceGxmTexture` descriptor points at the new base-level address (sceGxmTextureSetData-equivalent: the descriptor is rebuilt from `mem->ptr`).
  - Safety window: it only runs from `gxm_swap` every 900 frames (twice a minute) when no scene is open, every submitted scene is done on the gpu and the deferred- free queue is empty — nothing can be sampling an old address, and every freed-but-queued block has already left its chunk list. CPU texture access all goes through `t->mem->ptr`, which is updated. Render-to-texture is safe, fbo color surfaces are rebuilt from `mem->ptr` on every bind.
  - Empty chunks release via the existing `chunk_maybe_release` (keeps one spare), logged as "gxm: compacted cdram textures: moved N blocks..., freed ... KiB in ... chunks". Watch that line plus the periodic gxm mem line on hw: after a match load and 30 s idle in the menus, cdram block bytes should drop by (chunks × 2 MB).
- **2026-11-20, big-alloc attribution (hw9/port.log):** every `big alloc` backtrace in the match load resolves to one call chain: `VCCONTEXTMANAGER::ASYNCTHREADREQUEST::Function` → `VCSTATICCONTEXT::Create` → `VCScene_InitWithVramOffsetTranslation` → `VCTexture_MakeAbsoluteWithVramOffsetTranslation` / `VCModel_MakeAbsolute` → `VCHEAPWRAPPER::Allocate`. I.e. the ~160 MB that sits outside the game heap is **the game's own CPU-side copies of scene resources** (texture payloads + model vertex/index data), materialized when a static context is built, and only released when a scene's context is destroyed. tools/symbolize.py symbolizes those `libnba2k16...so+0xOFFSET` lines (parses the .so's dynsym, no binutils needed).
  - bigmem policy updated: since those blobs are written once at load and re-read rarely, 1 MB+ non-urgent allocations now prefer CDRAM when user memory is under 64 MB (main is what newlib's heap grows into and where the OOM'd 4 MB steps died). big-alloc backtrace cap raised 16 → 64.
- **Proposed next levers for the double storage** (game blob + GPU copy), in confidence order:
  - textures: blobs are ETC/BC compressed and our GXM copy is BC — no linear RGBA8 copy is retained anywhere. tex_bc=0 would be the wrong direction.
  - models: VCModel_MakeAbsoluteWithVramOffsetTranslation re-writes the vertex/index payload in place when a scene goes absolute; GL copies it into gxp vertex/index buffers. A safe GL-side trick: stream it — mark buffers created with `data ==` a recently MakeAbsolute'd bigmem block as static and copy lazily on first use instead of storing a bigmem copy + GL copy (our bigmem copy is already the game's, don't copy a second time). Needs effort + hw testing (determine offsetof copy sites).
