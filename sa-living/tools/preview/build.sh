#!/bin/bash
# Превью интерфейса SA Living на компьютере. Картинки .ppm — в каталоге запуска.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
ROOT=$(cd "$R/.." && pwd)
clang++ -std=c++17 -O1 -DSALIVING_PREVIEW -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -Wno-everything \
  -I"$R/tools/preview/stubs" -I"$ROOT/tools/preview/stubs" -I"$ROOT/third_party" -I"$R/src" \
  "$R/tools/preview/preview.cpp" "$R/src/ui/hud.cpp" "$R/src/util/json.cpp" \
  "$ROOT/third_party/imgui/imgui.cpp" "$ROOT/third_party/imgui/imgui_draw.cpp" \
  "$ROOT/third_party/imgui/imgui_widgets.cpp" "$ROOT/third_party/imgui/imgui_tables.cpp" \
  -o saliving_preview -lpthread
./saliving_preview "$@"
