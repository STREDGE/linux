#!/bin/bash
# Builds dist/SkinForgeSA.apk with the Android tools from Ubuntu packages:
#   apt-get install aapt apksigner zipalign dalvik-exchange android-sdk-platform-23
set -e
cd "$(dirname "$0")"
ANDROID_JAR=${ANDROID_JAR:-/usr/lib/android-sdk/platforms/android-23/android.jar}
OUT=build
rm -rf "$OUT" && mkdir -p "$OUT/gen" "$OUT/classes" "$OUT/dex" dist

aapt package -f -m -J "$OUT/gen" -M app/src/main/AndroidManifest.xml -S app/src/main/res -I "$ANDROID_JAR"
javac -nowarn -source 8 -target 8 -bootclasspath "$ANDROID_JAR" -d "$OUT/classes" \
    $(find app/src/main/java "$OUT/gen" -name '*.java')
dalvik-exchange --dex --output="$OUT/dex/classes.dex" "$OUT/classes"
aapt package -f -M app/src/main/AndroidManifest.xml -S app/src/main/res -I "$ANDROID_JAR" -F "$OUT/unsigned.apk"
(cd "$OUT/dex" && aapt add ../unsigned.apk classes.dex >/dev/null)
zipalign -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

# Debug signing key, kept in the repository so that updates install over older builds.
KEY=signing/debug.keystore
if [ ! -f "$KEY" ]; then
    mkdir -p signing
    keytool -genkeypair -keystore "$KEY" -storepass android -keypass android -alias debug \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=SkinForge Debug"
fi
apksigner sign --ks "$KEY" --ks-pass pass:android --key-pass pass:android --ks-key-alias debug \
    --out dist/SkinForgeSA.apk "$OUT/aligned.apk"
apksigner verify dist/SkinForgeSA.apk
ls -l dist/SkinForgeSA.apk
