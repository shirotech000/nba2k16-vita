# NBA 2K16 Vita

A PS Vita port of the Android version of NBA 2K16 (v0.0.29). It loads the game's ARM `.so` straight onto the Vita and fakes enough Android around it for the game to think it's on a phone.

**Status: early WIP.** In Vita3K it boots, loads the OBB, compiles the game's shaders through its own GLSL→Cg path and runs its render loop (frames of draws + clears), then the emulator dies about 15 s in. Nothing has been confirmed on screen yet, and it's nowhere near playable. Not tested on real hardware. See [docs/NOTES.md](docs/NOTES.md) for details.

## You need your own game files

Nothing from the game is in this repo. You need:

- **The NBA 2K16 Android APK, v0.0.29.** The port loads the game's own code from it.
- **The game's OBB data** (`main.*.com.t2ksports.nba2k16android.obb`) from an Android device where the game finished its download.

## Vita requirements

- A hacked Vita (HENkaku / Ensō) with **unsafe homebrew enabled**
- **kubridge**, a recent build with `kuKernelMemReserve` / `kuKernelMemCommit` (in your taiHEN `*KERNEL` section)
- **libshacccg.suprx** in `ur0:data/`. Shaders get compiled on device, and you have to dump this file yourself (same as other ports that compile shaders at runtime).
- **Optional: the CapUnlocker plugin.** If it's loaded, the port detects it and lets the game's worker threads use the 4th CPU core too. Without it you get the usual 3 cores. Set `capunlocker=0` in `ux0:data/nba2k16/config.txt` to opt out.

## Setup

1. Pull the libs out of your APK:
   ```
   python tools/extract_apk.py path/to/nba2k16.apk out
   ```
2. Copy `out/libnba2k16_clean_opt.so` and `out/libgnustl_shared.so` to `ux0:data/nba2k16/`.
3. Copy the OBB to `ux0:data/nba2k16/obb/`.
4. Install the VPK.

Logs end up in `ux0:data/nba2k16/port.log`. Please attach that when reporting stuff.

## Building

Needs [VitaSDK](https://vitasdk.org). On Windows this builds through WSL:

```
./build.sh          # in WSL / linux
.\build.ps1         # from powershell, runs build.sh in WSL
```

The output goes to `build/nba2k16_vita.vpk`.

## How it works (short version)

- `src/loader`: our own ELF loader. It maps the `.so` with kubridge, does the relocations, and resolves imports against our shim tables. Anything unresolved gets a trap stub that logs who called it.
- `src/bionic`: Android libc → newlib. This covers pthread layouts, `struct stat`, `__sF`, `_ctype_`, open flags, softfp math bridges and so on.
- `src/android`: a fake NativeActivity, ALooper, input queue (Vita buttons/sticks/touch → Android gamepad + touch), fake JNI, and OpenSL ES → sceAudioOut.
- `src/gles`: GLES3/EGL implemented directly on **sceGxm**, plus a GLSL ES → Cg translator so the game's shaders can go through SceShaccCg.
- `src/net`: curl/openssl stubs. The port is offline only, on purpose.

The game is built **softfp** and VitaSDK is hardfp, so every float-passing call between the two sides goes through `SOFTFP` (`pcs("aapcs")`) wrappers.

## Credits / notes

- The loader, shims and GXM renderer are written from scratch for this port.
- [vitasoguide](https://github.com/Rocroverss/vitasoguide) was a handy overview of the general so-loader approach.
- vitaGL was only used as a reference for how GLSL quirks are handled by SceShaccCg. No code from it (or other Rinnegatamante projects) is used here.
- kubridge by TheFloW (and contributors) does the kernel-side memory work.

This is a fan project with no affiliation to 2K or Visual Concepts. Buy the game.
