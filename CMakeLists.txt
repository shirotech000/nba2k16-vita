cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "set VITASDK first (e.g. export VITASDK=/usr/local/vitasdk)")
  endif()
endif()

project(nba2k16_vita C CXX ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "NBA 2K16")
set(VITA_TITLEID  "NBAK00016")
set(VITA_VERSION  "00.10")
# ATTRIBUTE2=12 -> extended memory mode, we need every MB we can get
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

option(NBA_TRACE_IMPORTS "log every unresolved import call" ON)
option(NBA_TRACE_GL "log every GL call (slow)" OFF)

set(CMAKE_C_STANDARD 11)
set(COMMON_FLAGS "-Wall -Wno-unused-function -O2 -g -fno-optimize-sibling-calls")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMMON_FLAGS} -fno-exceptions -fno-rtti")

if(NBA_TRACE_IMPORTS)
  add_compile_definitions(NBA_TRACE_IMPORTS=1)
endif()
if(NBA_TRACE_GL)
  add_compile_definitions(NBA_TRACE_GL=1)
endif()

include_directories(src)

file(GLOB_RECURSE PORT_SOURCES CONFIGURE_DEPENDS
  src/*.c
  src/*.S
)

# translation-cache stamp: a hash of glsl2cg.c/glsl2cg.h instead of
# __DATE__/__TIME__, so cached translations survive ordinary rebuilds and
# only die when the translator itself changes
file(SHA1 src/gles/glsl2cg.c G2C_C_HASH)
file(SHA1 src/gles/glsl2cg.h G2C_H_HASH)
set(G2C_BUILD_ID "${G2C_C_HASH}${G2C_H_HASH}")

add_executable(nba2k16 ${PORT_SOURCES})
target_compile_definitions(nba2k16 PRIVATE G2C_BUILD="${G2C_BUILD_ID}")
# extended memory mode loads the eboot above its link address, so the
# thumb->arm stubs ld makes must be pc-relative (bit mbaavita on hw)
target_link_options(nba2k16 PRIVATE -Wl,--pic-veneer)
# every malloc in the eboot goes through bionic/libc.c's fallbacks
target_link_options(nba2k16 PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
                    -Wl,--wrap=free -Wl,--wrap=memalign -Wl,--wrap=strdup -Wl,--wrap=strndup)

target_link_libraries(nba2k16
  kubridge_stub
  SceShaccCg_stub_weak
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  SceAudiodec_stub
  SceSysmodule_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceCommonDialog_stub
  SceKernelDmacMgr_stub
  SceRtc_stub
  ScePower_stub
  SceLibKernel_stub
  SceIofilemgr_stub
  pthread
  z
  m
  c
)

vita_create_self(eboot.bin nba2k16 UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
