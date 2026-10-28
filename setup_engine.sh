#!/usr/bin/env bash
# Prepara o motor: baixa o gta2_re (decompilacao do GTA 2) no commit fixado, aplica o patch do N64
# e gera os headers de compatibilidade de maiusculas/minusculas. Rode UMA vez depois do clone.
#   bash setup_engine.sh            (use --force para refazer do zero)
set -euo pipefail
cd "$(dirname "$0")"

UPSTREAM="https://github.com/CriminalRETeam/gta2_re.git"
PIN="f945bd13d2f3d03ebfe973e8f83c0c856d58b374"
DEST="port/gta2_re"

if [ -e "$DEST/Source" ]; then
  if [ "${1:-}" = "--force" ]; then
    echo "Refazendo $DEST ..."; rm -rf "$DEST"
  else
    echo "$DEST ja existe. Para refazer: bash setup_engine.sh --force"; exit 0
  fi
fi

for t in git patch; do command -v "$t" >/dev/null || { echo "ERRO: instale '$t' (ex.: sudo apt install git patch)"; exit 1; }; done

mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" remote add origin "$UPSTREAM"
echo "Baixando gta2_re ($PIN) ..."
git -C "$DEST" fetch -q --depth 1 origin "$PIN"
git -C "$DEST" checkout -q FETCH_HEAD

echo "Aplicando patches/gta2_re_n64.patch ..."
patch -p1 -d "$DEST" --dry-run -s < patches/gta2_re_n64.patch >/dev/null || { echo "ERRO: o patch nao aplica neste commit."; exit 1; }
patch -p1 -d "$DEST" -s < patches/gta2_re_n64.patch

# Copias dos 2 headers com nomes em outra caixa (para sistemas de arquivos que diferenciam maiusculas)
mkdir -p port/compat_ci
cp "$DEST/Source/fix16.hpp"       port/compat_ci/Fix16.hpp
cp "$DEST/Source/miss2_0x11C.hpp" port/compat_ci/miss2_0x11c.hpp

echo "Motor pronto. Proximo passo: ver README.md (secao Build)."
