#!/bin/bash
# Собирает встроенный Tailscale (tsbridge/*.go) в статическую библиотеку для arm64 Android:
#   third_party/tsbridge/libsacoopts.so (вшивается в мод сжатой: tools/tsbridge/embed.py)
# Нужны: Go (сам скачает нужную версию — GOTOOLCHAIN=auto) и Android NDK r27c.
#   NDK=/opt/android-ndk-r27c bash tools/tsbridge/build.sh
set -e
R="$(cd "$(dirname "$0")/../.." && pwd)"
NDK="${NDK:-/opt/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang"
OUT="$R/third_party/tsbridge"
mkdir -p "$OUT"
cd "$R/tsbridge"
GOTOOLCHAIN=auto GOOS=android GOARCH=arm64 CGO_ENABLED=1 CC="$CC" \
  go build -trimpath -buildmode=c-shared -ldflags="-s -w" -tags ts_omit_aws,ts_omit_kube \
  -o "$OUT/libsacoopts.so" .
ls -la "$OUT"
# сжать для вшивания в мод (src/net/ts_blob.S)
gzip -9 -n -c "$OUT/libsacoopts.so" > "$OUT/sacoopts.gz"
ls -la "$OUT/sacoopts.gz"
