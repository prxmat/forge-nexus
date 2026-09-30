#!/bin/sh
# Builds forge.dll for Nexus (Windows x64) with MinGW-w64; run from the repository root.
set -e
CXX=${CXX:-x86_64-w64-mingw32-g++}
mkdir -p build
CC=${CC:-x86_64-w64-mingw32-gcc}
# miniz (zip reading for .zevtc) is C.
for unit in miniz miniz_tdef miniz_tinfl miniz_zip; do
  $CC -O2 -c -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_WRITING_APIS -Isrc/thirdparty/miniz -o build/$unit.o src/thirdparty/miniz/$unit.c
done
$CXX -std=c++17 -O2 -s -shared -static -static-libgcc -static-libstdc++ -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DUNICODE -D_UNICODE -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_WRITING_APIS \
  -Isrc -Isrc/imgui \
  -o build/forge.dll \
  src/entry.cpp src/forge.cpp src/arcdps.cpp src/evtc.cpp src/fights.cpp src/imgui/imgui.cpp src/imgui/imgui_draw.cpp src/imgui/imgui_tables.cpp src/imgui/imgui_widgets.cpp \
  build/miniz.o build/miniz_tdef.o build/miniz_tinfl.o build/miniz_zip.o \
  -lwinhttp -lshell32 -lole32 -luuid -lpsapi -ladvapi32 -Wl,--subsystem,windows
ls -la build/forge.dll
