# Third-party notices

## gta2-browser (M1sterMo) — MIT

Parts of `port/compat/` and `port/n64/` were derived from the platform layer of
<https://github.com/M1sterMo/gta2-browser> and adapted for the Nintendo 64.

MIT License

Copyright (c) 2026 M1sterMo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

Scope: this licence covers this project's own work (the browser platform layers
in port/platform, port/render and port/audio, the web shell in web/, scripts/ and
docs/). The files in port/patches modify the gta2_re decompilation
(https://github.com/CriminalRETeam/gta2_re) and remain subject to that project's
terms. GTA2 itself is (c) Rockstar Games / Take-Two Interactive; no game files
are included.

## gta2_re (CriminalRETeam)

Not included in this repository. `setup_engine.sh` downloads it from
<https://github.com/CriminalRETeam/gta2_re> at a pinned commit; `patches/gta2_re_n64.patch` contains only our modifications.

## libdragon (DragonMinded)

Used as an external SDK (Unlicense). Not included.

## Grand Theft Auto 2

Property of Rockstar Games / Take-Two Interactive. No game assets, binaries or ROMs are part of this repository.

## GTA2 title logo (vector)

`tools/gta2_logo.svg` is the "Grand Theft Auto 2 logo" from Wikimedia Commons
(<https://commons.wikimedia.org/wiki/File:Grand_Theft_Auto_2_logo.svg>, vectorised by Connum, optimised by VulcanSphere), tagged
public domain as a text logo (simple geometric shapes). The logo is a trademark of Rockstar Games. The build uses it only to redraw
the logo on the menu screens at the console's resolution (`tools/img_n64.py`, `tools/logo_svg.py`).
