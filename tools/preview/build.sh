#!/bin/bash
# Сборка и запуск превью меню на компьютере. Картинки — в каталоге запуска.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
clang++ -std=c++17 -O1 -DSACOOP_PREVIEW -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS \
  -I"$R/tools/preview/stubs" -I"$R/third_party" -I"$R/src" \
  "$R/tools/preview/preview.cpp" "$R/src/ui/overlay.cpp" \
  "$R/third_party/imgui/imgui.cpp" "$R/third_party/imgui/imgui_draw.cpp" \
  "$R/third_party/imgui/imgui_widgets.cpp" "$R/third_party/imgui/imgui_tables.cpp" \
  -o preview_menu -lpthread
./preview_menu "$@"
