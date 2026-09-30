#!/bin/sh
# Builds forge.dll for Nexus (Windows x64) with MinGW-w64; run from the repository root.
set -e
CXX=${CXX:-x86_64-w64-mingw32-g++}
mkdir -p build
$CXX -std=c++17 -O2 -s -shared -static -static-libgcc -static-libstdc++ -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DUNICODE -D_UNICODE \
  -Isrc -Isrc/imgui \
  -o build/forge.dll \
  src/entry.cpp src/forge.cpp src/imgui/imgui.cpp src/imgui/imgui_draw.cpp src/imgui/imgui_tables.cpp src/imgui/imgui_widgets.cpp \
  -lwinhttp -Wl,--subsystem,windows
ls -la build/forge.dll
