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

# Bibliotecas: usa as que existirem nesta versao do VitaSDK (a lista varia um pouco entre versoes).
set(MV_LIBS vita2d)
set(MV_CANDIDATES
  SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub ScePgf_stub ScePvf_stub SceCommonDialog_stub
  SceAudio_stub ScePower_stub SceLibKernel_stub SceThreadmgr_stub SceProcessmgr_stub SceIofilemgr_stub
  freetype png bz2 brotlidec brotlicommon jpeg webp sharpyuv z)
set(MV_FOUND 0)
foreach(lib ${MV_CANDIDATES})
  find_library(MVLIB_${lib} NAMES ${lib})
  if(MVLIB_${lib})
    list(APPEND MV_LIBS ${lib})
    math(EXPR MV_FOUND "${MV_FOUND}+1")
  endif()
endforeach()
if(MV_FOUND LESS 8)
  message(STATUS "find_library nao localizou as bibliotecas; usando a lista padrao")
  set(MV_LIBS vita2d SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub ScePgf_stub ScePvf_stub
      SceCommonDialog_stub SceAudio_stub ScePower_stub SceLibKernel_stub SceProcessmgr_stub freetype png jpeg z)
endif()
list(APPEND MV_LIBS m c)
message(STATUS "Bibliotecas: ${MV_LIBS}")

target_link_libraries(${PROJECT_NAME}.elf ${MV_LIBS})

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME}.elf)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
       sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
       sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
       sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
