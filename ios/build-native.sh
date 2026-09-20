#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
repo="$(cd .. && pwd)"
host_tjsc="$repo/build/dpfjs/deps/lv_binding_js/deps/txiki/tjsc"
host_pb12="$repo/build/sameboy-bootroms/sameboy_pb12"
output="$PWD/.build/RetroPlugNative.xcframework"

if [ ! -x "$host_tjsc" ] || [ ! -x "$host_pb12" ]; then
  echo "error: host tjsc/pb12 is missing; run ./build.sh once before ios/generate.sh" >&2
  exit 1
fi

build_slice() {
  local name="$1" sdk="$2" arch="$3"
  local dir="$PWD/.build/native-$name"
  cmake -S "$repo" -B "$dir" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$sdk" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
    -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO \
    -DTJSC_EXECUTABLE="$host_tjsc" \
    -DSAMEBOY_PB12_EXECUTABLE="$host_pb12" \
    -DBUILD_WITH_FFI=OFF \
    -DBUILD_WITH_SQLITE=ON \
    -DRETROPLUG_EMBED_MGB=OFF >&2
  cmake --build "$dir" --target RetroPlugNative --config Release -- -quiet >&2 || return 1
  find "$dir" -path '*Release*' -name RetroPlugNative.framework -print -quit
}

if ! device_framework="$(build_slice device iphoneos arm64)"; then exit 1; fi
if ! sim_framework="$(build_slice simulator iphonesimulator arm64)"; then exit 1; fi
if [ -z "$device_framework" ] || [ -z "$sim_framework" ]; then
  echo "error: native framework slice was not produced" >&2
  exit 1
fi

rm -rf "$output"
xcodebuild -create-xcframework \
  -framework "$device_framework" \
  -framework "$sim_framework" \
  -output "$output"
echo "native: $output"
