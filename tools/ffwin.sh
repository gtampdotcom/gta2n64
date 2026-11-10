#!/usr/bin/env bash
# Lets a Linux tool (videoconv64) drive a Windows ffmpeg/ffprobe from WSL: stage_data.sh points --ffmpeg-path at this
# script when the only ffmpeg on PATH is ffmpeg.exe. Arguments that are Linux paths are rewritten as Windows paths.
#   FFWIN_EXE=ffmpeg.exe ffwin.sh <ffmpeg arguments...>
exe="${FFWIN_EXE:?set FFWIN_EXE to ffmpeg.exe or ffprobe.exe}"
args=()
for a in "$@"; do
  case "$a" in
    /*) if [ -e "$a" ] || [ -d "$(dirname "$a")" ]; then a="$(wslpath -w "$a")"; fi ;;
  esac
  args+=("$a")
done
exec "$exe" "${args[@]}"
