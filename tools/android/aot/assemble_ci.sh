#!/usr/bin/env bash
set -euo pipefail
work="$PWD/out/aot-apk"
bt="$ANDROID_HOME/build-tools/35.0.0"
jar="$ANDROID_HOME/platforms/android-35/android.jar"
mkdir -p "$work/classes"
javac -source 8 -target 8 -encoding UTF-8 -cp "$jar" -d "$work/classes" android/app/src/ro/cobrabm/fexdroid/*.java
mapfile -d '' classes < <(find "$work/classes" -name '*.class' -print0)
"$bt/d8" --min-api 30 --lib "$jar" --output "$work/package" "${classes[@]}"
"$bt/aapt2" compile --dir android/app/res -o "$work/res.zip"
cp android/app/AotLocalManifest.xml "$work/AndroidManifest.xml"
"$bt/aapt2" link -I "$jar" "$work/res.zip" --manifest "$work/AndroidManifest.xml" --min-sdk-version 30 --target-sdk-version 28 -o "$work/base.apk"
(cd "$work/package"; zip -q -0 -r "$work/base.apk" assets lib classes.dex)
"$bt/zipalign" -P 16 -f 4 "$work/base.apk" "$work/aligned.apk"
apk="$work/aligned.apk"
if [[ -n "${ISS_AOT_KEYSTORE_B64:-}" ]]; then
    umask 077
    key=$(mktemp)
    trap 'rm -f "$key"' EXIT
    printf '%s' "$ISS_AOT_KEYSTORE_B64" | base64 -d >"$key"
    apk="$work/update.apk"
    "$bt/apksigner" sign --ks "$key" --ks-key-alias aot-local --ks-pass env:ISS_AOT_KEY_PASSWORD --out "$apk" "$work/aligned.apk"
    "$bt/apksigner" verify --print-certs "$apk" > "$work/signers.txt"
    grep -q 'SHA-256 digest: a787209296559bd6ffe0d0790368b53cb64050f73e51d211d5bcb80b6b36f0b3' "$work/signers.txt"
fi
"$bt/aapt" dump badging "$apk" > "$work/badging.txt"
grep -F "package: name='com.daedalus.iss.aot.local'" "$work/badging.txt"
openssl cms -encrypt -binary -aes256 -outform DER -in "$apk" -out "$work/delivery.cms" tools/android/aot/delivery-cert.pem
sha256sum "$apk"
