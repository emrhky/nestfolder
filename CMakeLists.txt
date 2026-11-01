cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "")
  else()
    message(FATAL_ERROR "VITASDK tanimli degil")
  endif()
endif()
project(nestfolder C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -Wextra -O2")

# vita_create_self bir makro: bayraklari sonraki cagriya sizdirir
# (ornegin eklentinin exports.yml'si kurulum uygulamasina gecer). Her
# cagridan once sifirla.
macro(nest_create_self)
  set(VITA_ELF_CREATE_FLAGS "")
  set(VITA_MAKE_FSELF_FLAGS "")
  vita_create_self(${ARGN})
endmacro()

# ------------------------------------------------------------------
#  Eklenti (taiHEN, startfiles yok)
# ------------------------------------------------------------------
set(PLUGIN_CFLAGS -fno-builtin)
set(PLUGIN_LDFLAGS -nostartfiles -Wl,-e,module_start)

add_executable(nestfolder
    src/main.c
    src/taihen_stubs.S)
target_compile_options(nestfolder PRIVATE ${PLUGIN_CFLAGS})
target_link_options(nestfolder PRIVATE ${PLUGIN_LDFLAGS})
target_link_libraries(nestfolder -Wl,--start-group c
    SceLibKernel_stub SceIofilemgr_stub SceKernelThreadMgr_stub
    SceKernelModulemgr_stub SceSysmem_stub gcc -Wl,--end-group)
nest_create_self(nestfolder.suprx nestfolder CONFIG exports.yml UNSAFE)

# ------------------------------------------------------------------
#  Kurulum uygulamasi (VPK); eklenti payload/ altina gomulur
# ------------------------------------------------------------------
set(INSTALLER_TITLEID "NESTF0001")
set(INSTALLER_VERSION "01.03")

add_executable(nestfolder_installer
    installer/src/main.c
    installer/src/db.c
    installer/src/cfg.c
    installer/src/vfs_sceio.c)
target_include_directories(nestfolder_installer PRIVATE installer/src)
target_link_libraries(nestfolder_installer
    SceSqlite_stub SceSysmodule_stub SceDisplay_stub SceCtrl_stub
    ScePower_stub SceShellSvc_stub SceIofilemgr_stub SceAppUtil_stub)
nest_create_self(eboot.bin nestfolder_installer UNSAFE)
vita_create_vpk(nestfolder.vpk ${INSTALLER_TITLEID} eboot.bin
    VERSION ${INSTALLER_VERSION}
    NAME "nestfolder"
    FILE ${CMAKE_CURRENT_BINARY_DIR}/nestfolder.suprx payload/nestfolder.suprx
    FILE installer/sce_sys/icon0.png sce_sys/icon0.png
    FILE installer/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE installer/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE installer/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
