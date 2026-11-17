#!/usr/bin/env bash
# Atalho de build: bash build.sh            -> gera gta2_recomp.z64
#                  bash build.sh clean      -> limpa
# Requer libdragon (toolchain MIPS + n64.mk). Caminho padrao: /opt/libdragon (mude com N64_INST=...).
set -e
cd "$(dirname "$0")"
export N64_INST="${N64_INST:-/opt/libdragon}"
export PATH="$N64_INST/bin:$PATH"
[ -d port/gta2_re/Source ] || bash ./setup_engine.sh
if [ "${1:-}" = "clean" ]; then exec make -f Makefile.recomp clean; fi
# Default build: menus in 640x480i with the PC menu font, the game in 640x240 (menu pictures are built at 480 lines).
# Override for the older 240p-only build: N64_IMG_H=240 PS1_MENU_FONT=1 EXTRA_CFLAGS="" bash build.sh BUILD_DIR=build_recomp FS_DIR=filesystem_recomp
export N64_IMG_H="${N64_IMG_H:-480}"
export PS1_MENU_FONT="${PS1_MENU_FONT:-0}"
export EXTRA_CFLAGS="${EXTRA_CFLAGS--DN64_RES_MENU_480I}"
exec make -f Makefile.recomp -j"$(nproc)" all BUILD_DIR=build_480i FS_DIR=filesystem_480i "$@"
