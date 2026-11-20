# Handoff: NBA 2K16 Vita port (state as of 2026-11-20)

Read this first, then `docs/NOTES.md` (deeper history, ABI notes, debug switches).

## Ground rules from the owner

- **No code from vitaGL or any other Rinnegatamante project.** vitaGL may be read to understand GLSL handling; lift nothing. kubridge is fine. Graphics are native sceGxm.
- **Repo is public, but users must supply their own APK/OBB.** Never commit game files or anything derived from them: compiled shader caches, shader source, textures.
- **Comments:** casual, short.
- **Commit trailer:**
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01BdJ3Tv4meJizzjqTaZYFve
  ```
- **One owner edits the repo.** Other agents are read-only, with output only under `%TEMP%\agent_*`.
  - Before every build, run `git status` and confirm every modified file is yours.
  - Unreviewed edits from earlier agents are on branch `agent-wip`. Don't merge it blindly.
- **On the Vita:** only add things under `ux0:data`, and replace `ux0:app/NBAK00016/eboot.bin`. Don't touch tai config, kubridge, ur0 or libshacccg.
- **Ask before launching or killing Vita3K.**

## Where things are

| What | Where |
|---|---|
| Repo | `C:\Users\Shiro\nba2k16` (github.com/shirotech000/nba2k16-vita, branch `main`) |
| Build | `wsl -e bash -lc 'cd /mnt/c/Users/Shiro/nba2k16 && bash build.sh'`, output `build/nba2k16_vita.vpk` + `build/eboot.bin` (ELF `build/nba2k16`). vitasdk is in WSL at `/usr/local/vitasdk`. |
| Game files (user-supplied, gitignored) | `gamefiles/libnba2k16_clean_opt.so`, `gamefiles/libgnustl_shared.so`, `gamefiles/obb/main.29.com.t2ksports.nba2k16android.obb` |
| Vita3K | `C:\Users\Shiro\mbaavita\tools\vita3k\app\Vita3K.exe`, profile `...\vita3k\profile\Vita3K\ux0\data\nba2k16\`. Run with `tools/vita3k/run.ps1 -Play` after setting `NBA_VITA3K` / `NBA_SHACCCG=C:\Users\Shiro\mbaavita\tools\libshacccg.suprx`. |
| Hardware logs | `%TEMP%\claude\408a5b75635925ce\dd26b651-...\scratchpad\hw1..hw13\port.log` (session scratchpad, may get cleaned) |
| Saved eboots/ELFs per build | same scratchpad: `eboot_<hash>.bin`, `nba2k16_<hash>.elf` |
| Analysis reports (read-only agents) | `%TEMP%\agent_nba2k16_analysis\report.md` (+ `TASK1-6.md`), `%TEMP%\agent_shadersrc\report.md`, `%TEMP%\agent_zerocopy\report.md` |

## Real hardware (VitaDrop)

- **Address:** `http://10.0.0.1` (`vitadrop.local`), port 80. It only answers while the VitaDrop app is open on the Vita.
- **Upload:**
  ```
  curl -X POST -T <file> -H "X-File-Name: <name>" -H "X-Upload-Dir: <url-encoded dir>" -H "Expect:" http://10.0.0.1/api/upload
  ```
- **List / download:** `GET /api/ls?path=ux0:/data/` lists a folder. A missing folder also returns `[]`, so look for it in the parent's listing. `GET /api/dl?path=...` downloads a file.
- **Updating:** replace `ux0:app/NBAK00016/eboot.bin`, then md5-check the readback. Push a saved copy of the eboot, never `build/` directly. Pull `ux0:data/nba2k16/port.log` before the next launch, because each boot truncates it.
- **Crash dumps:** `ux0:data/psp2core-*.psp2dmp` (gzip'd ELF core) and `*GPUCRASH*`.
- **Symbolizing a crash pc in our eboot:** the log prints `eboot: main @ X`. The slide is X minus `nm`'s address of `main` minus 1 (Thumb bit). The eboot loads about 0x7b000–0x7f000 above its link address. Feed the slid address to `addr2line` with the matching ELF.
- **What's already on the Vita:**
  - the libs and OBB in `ux0:data/nba2k16/`;
  - a warm shader cache copied from Vita3K (`shadercache/*.gxp`);
  - no `config.txt`.

## Current state

The Vita and main run the same code: build **8355718**, which main matches as c64b14d.

**What works on hardware:**
- Boot, intro, menus and a quick match, including loading into the match.
- Textures: ETC transcoded to BC1/BC3 renders correctly.
- In-match frame rate about 40 fps (25 ms frames).
- Player heads render.
- Audio: PCM and AAC music both play.

**Still broken:**
1. **Hair renders wrong, there's no crowd, and the hoops are untextured and look like they float.** Cause: about 99 textures (mostly 512×512 and 256×256 with mips, 16-bit and RGBA8) fail `gpu_alloc` at match load (`tex: OOM ... gxm fmt`), and they draw as a white 1×1 fallback. Total memory is simply exhausted.
2. **First boot on a fresh install has no shader cache.** The ~480 compiles then make the intro lag and draws get skipped until they finish. Your Vita only works because Vita3K's compiled cache was copied over, and that can't be shipped.
3. **In-match frame rate is about 40 fps.** The CPU-bound per-draw path is the cause; see the fps plan below.

## Memory architecture (the main battle)

Extended memory mode (ATTRIBUTE2=12). At boot there's about 70 MB of free user memory after the 288 MB newlib heap, plus 112 MB of CDRAM and 26 MB of phycont. The game's `.so` takes 41 MB.

**Game heap.** `patches.c` patches the game's `VCHEAP2` global heap from 400 MB down to 176 MB. It grows itself by roughly 23 MB plus several 4 MB `GrowMemory` chunks during a match.

**Allocator chain (`bionic/libc.c`).** Every malloc in the eboot goes through it via `-Wl,--wrap=malloc,calloc,realloc,free,memalign,strdup,strndup`. Order:
1. newlib.
2. **bigmem** (`bionic/bigmem.c`), which gives each block its own memblock. It tries:
   1. main memory, keeping an 8 MB reserve;
   2. phycont, keeping a 4 MB reserve;
   3. CDRAM, L1-cached type if possible, keeping a 6 MB reserve.
3. **Urgent mode**, used before handing back NULL. The floors drop to 2 MB main / 256 KB CDRAM / 0 phycont, and as a last stop it borrows idle space inside GPU pool chunks (`gxm_borrow`).
4. **spill** (`bionic/spill.c`): allocations under 64 KB carved from 1 MB bigmem chunks once newlib's heap is full.
5. A 512 KB rainy-day block inside newlib, freed on the first spill.

**GPU side (`gles/gxm.c`).**
- **Pool:** 2 MB chunks, with a lock (owner + depth). Range-heap nodes come from a static pool, so nothing in there mallocs.
- **Fixed allocations:**
  - param buffer 4 MB (`pb_mb=`);
  - transient ring 1 MB (`ring_mb=`; the game never uses it);
  - vdm/vertex/fragment rings at SDK defaults;
  - 3 display buffers.
- **Overflow buffers:** vertex/index buffers that don't fit the pool get a bigmem block mapped read-only with `sceGxmMapMemory` (`gpu_alloc_mapped`). Writes flush via `kuKernelFlushCaches`. This is what brought the heads back.

**Texture compression.** ETC2 RGB is transcoded to BC1 and ETC2 RGBA to BC3 at upload (`tex_bc`, on by default). That saves up to ~44 MB of GPU memory. Cube maps and sRGB stay RGBA8.

**Shader compiler.** libshacccg (3.4 MB) now loads only on a cache miss.

**Analysis findings to keep in mind:**
- **The game stores everything twice.** It keeps CPU copies of all texture and model data for the whole scene's lifetime (`VCTexture/VCModel_MakeAbsoluteWithVramOffsetTranslation`), on top of our GPU copy.
- **Zero-copy was tried and failed.** Build 2c31af5, reverted in c64b14d: it saved only 266 KB and caused glitches plus a GPU crash. Don't retry it for vertex buffers. Zero-copy textures can't help either: ETC needs transcoding anyway.

## Next steps, in priority order

1. **Reduced-resolution texture fallback** (planned, not started). Fixes hair, crowd and hoops.
   - In `gl_tex.c` `tex_realloc`, when `gpu_alloc` fails for a texture with a mip chain, allocate from mip 1 instead (half width and height, a quarter of the memory).
   - Have the upload path map game level N to stored level N−1 and drop level 0.
   - Keep a per-texture `lod_bias` / `level_offset`, so `glTexImage2D`/`glCompressedTexImage2D` for level 0 is skipped (or downsampled if there's only one level).
   - Also try `gpu_alloc_mapped` (mapped CPU memory) as a second fallback. Every CPU write to texture memory then needs `gpu_mapped_flush`, and there are several write sites: upload, BC transcode, mip copy, unbc.
   - Log `tex: downscaled ...`.
2. **"Preparing for first launch" screen** (the owner wants it to look decent).
   - **Shader sources can't be extracted offline.** Only 8 of the 384 linked pairs are plain text in the `.so`; the rest come from compressed OBB archives (`agent_shadersrc` report). Every shader arrives through one `glShaderSource` call site as a complete pre-built source.
   - **Plan:**
     1. Record a manifest of every (vs, fs) GLSL pair the game links, stored as source text in `ux0:data/nba2k16/shadercache/manifest.bin`. That's the user's own data; never ship it.
     2. At boot, before the game starts, find pairs that are missing from the cache.
     3. If any are missing, show the screen and compile them on the main thread, with plenty of memory free.
     4. Then build the pack, unload libshacccg, and start the game.
   - **Pack:** use the `TASK5.md` design in `agent_nba2k16_analysis`: one `pack.bin`, an index sorted by filename, `sceIoPread`, loose-file fallback. Key `g2c_build_id` on SHA1 of `glsl2cg.c`/`.h` through CMake instead of `__DATE__ __TIME__`.
   - **Look:** dark gradient, a "Preparing for first launch" title, a rounded progress bar, and "Compiling shaders N / M" with a "only happens once" note. Draw with raw sceGxm through the existing clear-shader path, using a small original bitmap font with only the needed glyphs. No game art.
3. **Frame rate** (`TASK4.md`). The render thread is CPU-bound with zero GPU stalls.
   - Track dirty uniforms per program in `prog_upload_uniforms` (`shader.c`). It re-uploads every uniform every draw, which is the biggest cost (estimated 3–6 ms).
   - Skip `gl_apply_state` when `G.dirty` is clear (1.5–3 ms).
   - Cache the last VP/FP variant and the last bound texture per unit.
   - The 4 scenes per frame can't be merged; they're separate render targets.
4. **Smaller items:**
   - Patch the game's 4 MB heap growth step down to 1 MB: `patch_word(game, 0x132AA08, 0xE3A02501, 0xE3A02940)`. Verify the encoding first.
   - Add the remaining NULL checks listed in `TASK6.md`.
   - Fix the glsl2cg `_g2c_v_vsout_` truncation (prog 1185, `LINK FAILED`).
   - Fix `gl_FrontFacing` vs `glFrontFace(GL_CW)` / FBO mirroring.
   - Re-enable 4x MSAA (`msaa=4`) only if memory ever allows.

## Debug switches (`ux0:data/nba2k16/config.txt`)

| Switch | Effect |
|---|---|
| `tex_bc=0` | turn off BC transcoding |
| `tex_bc_srgb=1` | also transcode sRGB textures |
| `tex_bc_npot=0` | keep non-power-of-two textures uncompressed |
| `pb_mb=` | parameter buffer size, 4–16 |
| `ring_mb=` | transient ring size |
| `msaa=0/2/4` | MSAA on the main 3D target (default 0) |
| `async_shaders=0` | compile shaders inline |
| `v3k_gxp_fix=0` | turn off the Vita3K-only shader patch |
| `prof=1` | time individual draws |
| `frame_log=1` | log every frame |
| `no_draw=1` | skip all draws |
| `shots=1` | enable `shot.flag` screenshots |
| `sync_log=1` | write the log synchronously |
| `game_heap_mb=` | size of the game's main heap |
| `capunlocker=0` | ignore CapUnlocker |
| `cube_dummy=1` | replace cube maps with a dummy |

## Gotchas learned the hard way

- **Vita3K hides memory-map bugs.** It maps memory past stacks, which once hid an `so_backtrace` overrun. It also ignores strict thread priority: on hardware the compile worker starved below the game's threads (it's at 0x9A now).
- **`sceIoRead` into CDRAM or GPU-pool memory fails on hardware.** Read through a stack buffer (`io_read_bounce`).
- **Newlib's `strdup` uses `_malloc_r`, which `--wrap=malloc` doesn't catch.** That's why `strdup` is wrapped separately.
- **The game's `DeleteFile_LowLevel` retries `unlink` 33 times.** It's patched to 1.
- **Vita3K's shader recompiler mishandles immediate swizzles.** It made skin render green. Workaround is in `link_back`, active under Vita3K only.
- **Card I/O on hardware costs several ms per open or stat.** The logger already writes from a background thread.
