cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Defina a variavel VITASDK apontando para o VitaSDK.")
  endif()
endif()

project(MusicaVita C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Musica Vita")
set(VITA_TITLEID  "MUSV00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O2 -std=gnu99 -Wno-unused-function")

add_executable(${PROJECT_NAME}.elf
  src/main.c
  src/deps/stb_vorbis.c
)

# Lista explicita das bibliotecas "stub" do sistema que o app realmente usa
# (vita2d, audio, controle, arquivos, threads, energia).
# So entram as que existirem nesta versao do VitaSDK.
set(MV_STUB_NAMES
  SceDisplay SceGxm SceSysmodule SceCtrl ScePgf ScePvf SceCommonDialog
  SceAudio SceAppUtil SceAppMgr SceSharedFb
  SceLibKernel SceThreadmgr SceIofilemgr SceProcessmgr SceSysmem
  ScePower
)
set(MV_STUBS "")
foreach(n ${MV_STUB_NAMES})
  if(EXISTS "$ENV{VITASDK}/arm-vita-eabi/lib/lib${n}_stub.a")
    list(APPEND MV_STUBS "${n}_stub")
  endif()
endforeach()

# Bibliotecas de imagem/fonte que o vita2d usa (so as que existirem nesta versao do VitaSDK).
set(MV_EXTRA "")
foreach(lib freetype png bz2 brotlidec brotlicommon jpeg webp sharpyuv z)
  find_library(MVLIB_${lib} NAMES ${lib})
  if(MVLIB_${lib})
    list(APPEND MV_EXTRA ${lib})
  endif()
endforeach()
if(NOT MV_EXTRA)
  set(MV_EXTRA freetype png jpeg z)
endif()
message(STATUS "Stubs usados: ${MV_STUBS}")
message(STATUS "Extras: ${MV_EXTRA}")

target_link_libraries(${PROJECT_NAME}.elf vita2d ${MV_STUBS} ${MV_EXTRA} m c)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME}.elf)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
       sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
       sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
       sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
