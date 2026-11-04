#!/bin/bash
# usage: package-windows.sh <bin dir> <qt dir> <mingw sysroot> <out dir>
# Collects the Windows programs with the Qt plugins they use and every DLL they import that
# comes from the build, llvm-mingw or Qt (system DLLs are left out; Windows ships them).
set -euo pipefail
bin=$1; qt=$2; sysroot=$3; out=$4
objdump=${OBJDUMP:-llvm-objdump}
rm -rf "$out"; mkdir -p "$out"
cp "$bin"/eglantine.exe "$bin"/eden-cli.exe "$out"/
for p in platforms/qwindows.dll styles/qmodernwindowsstyle.dll imageformats/qjpeg.dll imageformats/qico.dll imageformats/qgif.dll imageformats/qsvg.dll iconengines/qsvgicon.dll tls/qschannelbackend.dll; do
  [ -f "$qt/plugins/$p" ] || continue
  mkdir -p "$out/$(dirname "$p")"; cp "$qt/plugins/$p" "$out/$p"
done
search=("$bin" "$sysroot/bin" "$qt/bin")
# Copy every DLL the programs and plugins import that ships with the toolchain, Qt or the build.
changed=1
while [ $changed = 1 ]; do
  changed=0
  while read -r dll; do
    [ -f "$out/$dll" ] && continue
    for dir in "${search[@]}"; do
      if [ -f "$dir/$dll" ]; then cp "$dir/$dll" "$out/"; changed=1; break; fi
    done
  done < <(find "$out" -name '*.exe' -o -name '*.dll' | xargs -n1 "$objdump" -p | awk '/DLL Name/{print $3}' | sort -u)
done
printf '[Paths]\nPlugins = .\n' > "$out/qt.conf"
