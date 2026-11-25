#!/bin/bash
# Собирает java/net/sacoop/Picker.java (выбор файла с телефона) в classes.dex
# и вшивает в src/platform/picker_dex.h.
# Нужны: JDK (javac), android.jar (платформа Android) и d8 (build-tools).
#   ANDROID_JAR=/path/android.jar D8=/path/d8 bash tools/build_dex.sh
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d)
javac -source 8 -target 8 -g -nowarn -bootclasspath "$ANDROID_JAR" -cp "$ANDROID_JAR" -d "$W/classes" $(find "$R/java/net/sacoop" -name '*.java')
"$D8" --min-api 26 --release --output "$W" $(find "$W/classes" -name '*.class')
python3 - "$W/classes.dex" "$R/src/platform/picker_dex.h" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
out = ["// Создано tools/build_dex.sh из java/net/sacoop/*.java — не править руками.",
       "#pragma once", "#include <stddef.h>", "static const unsigned char kPickerDex[] = {"]
for i in range(0, len(data), 20):
    out.append("    " + ",".join("0x%02x" % b for b in data[i:i+20]) + ",")
out.append("};")
out.append("static const size_t kPickerDexSize = %d;" % len(data))
open(sys.argv[2], 'w').write("\n".join(out) + "\n")
print("dex:", len(data), "bytes")
PY
rm -rf "$W"
