#!/usr/bin/env bash
# Copia dados do GTA 2 (freeware) para o DragonFS com nomes em minusculas.
# Arquivos .gxt/.sty/.gmp sao convertidos de little-endian (PC) para big-endian (N64) por tools/le2be.py,
# SEMPRE a partir do original (nunca converte in-place -> sem risco de troca dupla).
# uso: stage_data.sh <pasta_data_do_jogo> <pasta_fs_saida> <entradas...>   (entrada = arquivo ou pasta, sem diferenciar maiusculas)
src="$1"; out="$2"; shift 2
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$out/data"
# p.gxt (portugues do Brasil) nao existe no jogo original: se faltar na pasta de dados, gera a partir do e.gxt + tools/pt_br.tsv
if [ -z "$(find "$src" -maxdepth 1 -iname p.gxt | head -1)" ]; then
  e_gxt=$(find "$src" -maxdepth 1 -iname e.gxt | head -1)
  if [ -n "$e_gxt" ] && python3 "$here/make_pt_gxt.py" "$e_gxt" "$src/p.gxt" "$here/pt_br.tsv"; then
    echo "    [PT-BR] p.gxt gerado em $src"
  else
    echo "AVISO: nao foi possivel gerar o p.gxt (idioma portugues desativado)"
  fi
fi
# radios: taxa e qualidade. Se mudar (arquivo de marca diferente), TODAS as radios sao reconvertidas.
RADIO_RATE="${RADIO_RATE:-12000}"        # 12 kHz; 8000 = ainda menor (ex.: RADIO_RATE=8000 make ...)
RADIO_QUALITY="${RADIO_QUALITY:-35}"
radio_cfg="ulc$RADIO_QUALITY-mono-$RADIO_RATE"
radio_stamp="${out%/}.radio_cfg"
radio_force=0
[ "$(cat "$radio_stamp" 2>/dev/null)" = "$radio_cfg" ] || radio_force=1
# menu screens: target size (the framebuffer: 640x240). If it changes, all of them are converted again.
img_cfg="n64i-${N64_IMG_W:-640}x${N64_IMG_H:-240}"
img_stamp="${out%/}.img_cfg"
img_force=0
[ "$(cat "$img_stamp" 2>/dev/null)" = "$img_cfg" ] || img_force=1
# Bancos de efeitos sonoros: UM sfx.raw compartilhado entre os mapas (tools/sfx_share.py), em vez de 6 MB por mapa.
SFX_MAPS="${SFX_MAPS:-bil ste wil}"
keep_list="${out%/}.staged_list"; : > "$keep_list"
if [ -n "$SFX_MAPS" ]; then
  python3 "$here/sfx_share.py" "$src" "$out/data" $SFX_MAPS || { echo "ERRO: sfx_share.py falhou"; exit 1; }
  echo "$out/data/audio/sfx.raw" >> "$keep_list"
  for b in $SFX_MAPS; do
    [ -f "$out/data/audio/$b.sdt" ] && { echo "$out/data/audio/$b.sdt" >> "$keep_list"; echo "$out/data/audio/$b.raw" >> "$keep_list"; }
  done
fi
for e in "$@"; do
  case "$e" in
    */*) m=$(find "$src" -maxdepth 2 -ipath "$src/$e" | head -1) ;;   # ex.: audio/bil.raw
    *)   m=$(find "$src" -maxdepth 1 -iname "$e" | head -1) ;;
  esac
  if [ -z "$m" ]; then echo "AVISO: '$e' nao existe em $src"; continue; fi
  while read -r f; do
    rel="${f#$src/}"; low=$(echo "$rel" | tr 'A-Z' 'a-z')
    mkdir -p "$out/data/$(dirname "$low")"
    case "$low" in
      old\ *|*/old\ *|.ds_store|*/.ds_store|audio/*.raw|audio/*.sdt) ;;
      movie/intro.bik) ;;   # converted below; its outputs are listed there
      gtaudio/*.wav|audio/vocals/*.wav) echo "$out/data/${low%.wav}.wav64" >> "$keep_list" ;;
      *) echo "$out/data/$low" >> "$keep_list" ;;
    esac
    case "$low" in
      old\ *|*/old\ *|.ds_store|*/.ds_store) continue ;;
      movie/intro.bik)
        # intro movie: Bink -> MPEG-1 320x240 + audio, with libdragon's videoconv64 (needs ffmpeg). Played by intro_n64.c.
        # MPEG-1 (not H.264, half the size): its decoder is much smaller, and that code stays in the console's RAM.
        # 29.97 fps = twice the movie's 14.985: MPEG-1 has no 15 fps (the repeated frames cost almost no bits).
        vdst="$out/data/intro.m1v"; adst="$out/data/intro.wav64"; vcfg="mpeg1-q${INTRO_QUALITY:-75}-320-30fps-a22050"
        vstamp="${out%/}.intro_cfg"
        if [ ! -f "$vdst" ] || [ ! -f "$adst" ] || [ "$f" -nt "$vdst" ] || [ "$(cat "$vstamp" 2>/dev/null)" != "$vcfg" ]; then
          ffdir=""; ffm=""
          if command -v ffmpeg >/dev/null 2>&1 && command -v ffprobe >/dev/null 2>&1; then
            ffm="$(command -v ffmpeg)"; ffp="$(command -v ffprobe)"
          elif command -v ffmpeg.exe >/dev/null 2>&1 && command -v ffprobe.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
            # WSL with only the Windows ffmpeg: small wrappers that hand it Windows paths (tools/ffwin.sh)
            ffdir="$(mktemp -d)"
            for t in ffmpeg ffprobe; do
              { echo '#!/usr/bin/env bash'; echo "FFWIN_EXE=\"$(command -v $t.exe)\" exec bash \"$here/ffwin.sh\" \"\$@\""; } > "$ffdir/$t"
              chmod +x "$ffdir/$t"
            done
            ffm="$ffdir/ffmpeg"; ffp="$ffdir/ffprobe"
          fi
          if [ -z "$ffm" ]; then
            echo "AVISO: ffmpeg nao encontrado: a ROM fica sem o filme de abertura (instale o ffmpeg ou use INCLUDE_INTRO=0)"
          else
            echo "    [INTRO] $low (MPEG-1 320x240, quality ${INTRO_QUALITY:-75})"
            vtmp="$(mktemp -d)"; cp "$f" "$vtmp/intro.bik"
            if N64_INST="${N64_INST:-/opt/libdragon}" "${N64_INST:-/opt/libdragon}/bin/videoconv64" -o "$vtmp" --codec mpeg1 --fps 29.97 --width 320 \
                 --quality "${INTRO_QUALITY:-75}" --no-progress --audio-parms 22050,1 --audio-compress 1 \
                 --ffmpeg-path "$ffm" --ffprobe-path "$ffp" "$vtmp/intro.bik" >/dev/null 2>"$vtmp/log" \
               && [ -s "$vtmp/intro.m1v" ] && [ -s "$vtmp/intro.wav64" ]; then
              cp "$vtmp/intro.m1v" "$vdst" && cp "$vtmp/intro.wav64" "$adst" && echo "$vcfg" > "$vstamp"
            else
              echo "AVISO: a conversao do filme de abertura falhou (a ROM fica sem ele):"; tail -3 "$vtmp/log"
              rm -f "$vdst" "$adst"
            fi
            rm -rf "$vtmp"
          fi
          [ -n "$ffdir" ] && rm -rf "$ffdir"
        fi
        [ -f "$vdst" ] && [ -f "$adst" ] && { echo "$vdst" >> "$keep_list"; echo "$adst" >> "$keep_list"; } ;;
      *.tga)
        # menu screens: resampled here to the size they cover on the N64 framebuffer (tools/img_n64.py), not at run time.
        # Redone only when the source, the converter or the target size changed.
        dst="$out/data/$low"
        if [ "$img_force" = 1 ] || [ ! -f "$dst" ] || [ "$f" -nt "$dst" ] || [ "$here/img_n64.py" -nt "$dst" ] || [ "$here/logo_svg.py" -nt "$dst" ] || [ "$here/gta2_logo.svg" -nt "$dst" ]; then
          echo "    [IMG] $low (${N64_IMG_W:-640}x${N64_IMG_H:-240})"
          python3 "$here/img_n64.py" "$f" "$dst" "${N64_IMG_W:-640}" "${N64_IMG_H:-240}" || { echo "ERRO convertendo $f"; exit 1; }
        fi ;;
      *.sty)
        # city styles (not fstyle.sty, which has no in-game font): the PS1 text font replaces the PC "normal" font (tools/ps1_font.py).
        # PS1_FONT=0 keeps the PC font.
        case "$low" in
          fstyle.sty)
            # front-end: the thin menu fonts (white and red) become the PlayStation ones (tools/ps1_menu_font.py)
            # PS1_MENU_FONT=0 keeps the PC menu font (best when the menus run in 640x480i, where it is not squashed)
            if [ "${PS1_MENU_FONT:-${PS1_FONT:-1}}" = 1 ]; then
              ptmp="$(mktemp --suffix=.sty)"
              python3 "$here/ps1_menu_font.py" "$f" "$ptmp" || { echo "ERRO na fonte PS1 do menu de $f"; rm -f "$ptmp"; exit 1; }
              python3 "$here/le2be.py" "$ptmp" "$out/data/$low" || { echo "ERRO convertendo $f"; rm -f "$ptmp"; exit 1; }
              rm -f "$ptmp"
            else
              python3 "$here/le2be.py" "$f" "$out/data/$low" || { echo "ERRO convertendo $f"; exit 1; }
            fi ;;
          *)
            if [ "${PS1_FONT:-1}" = 1 ]; then
              ptmp="$(mktemp --suffix=.sty)"
              python3 "$here/ps1_font.py" "$f" "$ptmp" || { echo "ERRO na fonte PS1 de $f"; rm -f "$ptmp"; exit 1; }
              python3 "$here/le2be.py" "$ptmp" "$out/data/$low" || { echo "ERRO convertendo $f"; rm -f "$ptmp"; exit 1; }
              rm -f "$ptmp"
            else
              python3 "$here/le2be.py" "$f" "$out/data/$low" || { echo "ERRO convertendo $f"; exit 1; }
            fi ;;
        esac ;;
      *.gxt|*.gmp)
        python3 "$here/le2be.py" "$f" "$out/data/$low" || { echo "ERRO convertendo $f"; exit 1; } ;;
      gtaudio/*.wav)
        dst="$out/data/${low%.wav}.wav64"
        if [ "$radio_force" = 1 ] || [ ! -f "$dst" ] || [ "$f" -nt "$dst" ]; then
          echo "    [RADIO] $low (ULC mono $RADIO_RATE Hz)"
          "${N64_INST:-/opt/libdragon}/bin/audioconv64" --wav-compress ulc,quality=$RADIO_QUALITY --wav-mono --wav-resample "$RADIO_RATE" \
            -o "$out/data/gtaudio" "$f" >/dev/null || { echo "ERRO convertendo $f"; exit 1; }
          produced="$out/data/gtaudio/$(basename "${f%.*}").wav64"   # audioconv64 usa o nome original (pode ter maiusculas)
          # (through a temporary name: on a case-insensitive drive A.wav64 -> a.wav64 is otherwise a no-op,
          #  and the clean-up below, which compares names case-sensitively, then deletes it)
          [ "$produced" != "$dst" ] && [ -f "$produced" ] && mv -f "$produced" "$dst.tmp" 2>/dev/null && mv -f "$dst.tmp" "$dst" 2>/dev/null
        fi ;;
      audio/vocals/*.wav)
        # announcer vocals ("Frenzy!", "Wasted"...): streamed like the radios, ULC mono at their own 22 kHz
        dst="$out/data/${low%.wav}.wav64"
        if [ "$radio_force" = 1 ] || [ ! -f "$dst" ] || [ "$f" -nt "$dst" ]; then
          echo "    [VOCAL] $low"
          "${N64_INST:-/opt/libdragon}/bin/audioconv64" --wav-compress ulc,quality=$RADIO_QUALITY --wav-mono \
            -o "$out/data/audio/vocals" "$f" >/dev/null || { echo "ERRO convertendo $f"; exit 1; }
          produced="$out/data/audio/vocals/$(basename "${f%.*}").wav64"
          [ "$produced" != "$dst" ] && [ -f "$produced" ] && mv -f "$produced" "$dst.tmp" 2>/dev/null && mv -f "$dst.tmp" "$dst" 2>/dev/null
        fi ;;
      *.scr)
        # scripts de missao: conversao campo a campo (tools/scr_le2be.py + scr_tabela.json)
        python3 "$here/scr_le2be.py" "$f" "$out/data/$low" || { echo "ERRO convertendo $f"; exit 1; } ;;
      audio/*.raw|audio/*.sdt)
        bn=$(basename "${low%.*}")
        case " $SFX_MAPS " in *" $bn "*) continue ;; esac     # ja gerado por sfx_share.py
        echo "$out/data/$low" >> "$keep_list"
        python3 "$here/audio_le2be.py" "$f" "$out/data/$low" || { echo "ERRO convertendo $f"; exit 1; } ;;
      *) cp -u "$f" "$out/data/$low" ;;
    esac
  done < <(find "$m" -type f)
done
# poda: remove do DragonFS (pasta de build, regeneravel) o que NAO foi preparado nesta rodada (ex.: frontend/*.tga de builds antigos)
sort -u "$keep_list" -o "$keep_list"
find "$out/data" -type f | sort | comm -23 - "$keep_list" | while IFS= read -r f; do rm -f "$f" && echo "    [PODA] ${f#$out/data/}"; done
find "$out/data" -mindepth 1 -type d -empty -delete 2>/dev/null
echo "$radio_cfg" > "$radio_stamp"
echo "$img_cfg" > "$img_stamp"
