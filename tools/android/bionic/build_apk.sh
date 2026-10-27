#!/usr/bin/env bash
# Build and verify the inFAMOUS Second Son — Daedalus release APK.
set -Eeuo pipefail
trap 'echo "ISS Bionic APK failed at $LINENO: $BASH_COMMAND" >&2' ERR
cd -- "$(dirname "$0")/../../.."
sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:?Android SDK required}}
bt="$sdk/build-tools/35.0.0"
jar="$sdk/platforms/android-35/android.jar"
for t in aapt2 d8 zipalign apksigner; do test -x "$bt/$t"; done
test -s "$jar"
test -s out/bionic/lib/libisspresent.so
python3 tools/android/bionic/package_runtime.py
# Also validate the exact launcher bytes embedded in the runtime archive that
# will be shipped inside the APK, not only the checked-in source.
tar -xOzf out/release/iss-bionic-runtime.tar.gz issport/run-bionic.sh | sh -n
tar -xOzf out/release/iss-bionic-runtime.tar.gz issport/run-bionic.sh | bash -n
work="$PWD/out/bionic/apk-work"
rm -rf "$work"
mkdir -p "$work/classes" "$work/package/assets" "$work/package/lib/arm64-v8a" out/release
javac -encoding UTF-8 -source 8 -target 8 -cp "$jar" -d "$work/classes" \
  android/app/src/ro/cobrabm/fexdroid/*.java
mapfile -d '' classes < <(find "$work/classes" -name '*.class' -print0)
test "${#classes[@]}" -gt 0
"$bt/d8" --min-api 30 --lib "$jar" --output "$work/package" "${classes[@]}"
cp out/release/iss-bionic-runtime.tar.gz "$work/package/assets/runtime.tar.gz"
cp out/release/iss-bionic-runtime.sha256 "$work/package/assets/runtime.sha256"
cp out/bionic/lib/libisspresent.so "$work/package/lib/arm64-v8a/libisspresent.so"
"$bt/aapt2" compile --dir android/app/res -o "$work/resources.zip"
"$bt/aapt2" link -I "$jar" "$work/resources.zip" --manifest android/app/BionicAndroidManifest.xml \
  --min-sdk-version 30 --target-sdk-version 28 -o "$work/base.apk"
(cd "$work/package" && zip -q -0 -r "$work/base.apk" assets lib classes.dex)
"$bt/zipalign" -f 4 "$work/base.apk" "$work/aligned.apk"
key="$PWD/.local-deps/android/bionic-development.keystore"
if [[ ! -f "$key" && ${ISS_RELEASE_BUILD:-1} = 1 ]]; then
  echo "Release signing key missing: restore the existing signing cache; do not replace the key." >&2
  exit 1
fi
if [[ ! -f "$key" ]]; then
  keytool -genkeypair -keystore "$key" -storepass android -keypass android \
    -alias iss-bionic-dev -dname "CN=ISS Bionic Android Experimental" -keyalg RSA \
    -keysize 2048 -validity 10000 -noprompt >/dev/null 2>&1
fi
version=$(python3 -c 'import xml.etree.ElementTree as E; print(E.parse("android/app/BionicAndroidManifest.xml").getroot().get("{http://schemas.android.com/apk/res/android}versionName"))')
[[ $version =~ ^[0-9]+([.][0-9]+)*$ ]]
apk="$PWD/out/release/inFAMOUS-Second-Son-Daedalus-v${version}.apk"
"$bt/apksigner" sign --ks "$key" --ks-key-alias iss-bionic-dev \
  --ks-pass pass:android --key-pass pass:android --out "$apk" "$work/aligned.apk"
"$bt/apksigner" verify --verbose "$apk"
"$bt/aapt" dump badging "$apk" | grep -F "package: name='com.daedalus.iss.depthmatch'"
for member in assets/runtime.tar.gz assets/runtime.sha256 lib/arm64-v8a/libisspresent.so classes.dex; do
  unzip -Z1 "$apk" | grep -qx "$member"
done
if [[ ${ISS_RELEASE_BUILD:-1} = 1 ]]; then
  python3 tools/android/bionic/prepare_release.py --apk "$apk" \
    --aapt "$bt/aapt" --apksigner "$bt/apksigner"
else
  (cd -- "$(dirname "$apk")" && sha256sum "$(basename "$apk")") > "$apk.sha256"
fi
echo "Installable inFAMOUS Daedalus v${version} APK built and signature verified: $apk"
