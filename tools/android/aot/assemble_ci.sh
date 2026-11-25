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
# Normalize ZIP metadata using the SDK itself before offline signing. The temporary
# signature is stripped immediately and never becomes an update or a deliverable.
keytool -genkeypair -alias normalization -keyalg RSA -keysize 2048 -validity 1 \
  -keystore "$RUNNER_TEMP/normalize.jks" -storepass normalization -keypass normalization -dname CN=Normalization -noprompt
"$bt/apksigner" sign --ks "$RUNNER_TEMP/normalize.jks" --ks-pass pass:normalization \
    --v1-signing-enabled false --alignment-preserved true --out "$work/normalized-container.apk" "$work/aligned.apk"
python3 tools/android/aot/detached_signing.py normalize "$work/normalized-container.apk" "$work/request.apk"
rm -f "$RUNNER_TEMP/normalize.jks" "$work/normalized-container.apk"
openssl cms -encrypt -binary -aes256 -outform DER -in "$work/request.apk" -out "$work/request.cms" tools/android/aot/delivery-cert.pem
sha256sum "$work/request.apk"
