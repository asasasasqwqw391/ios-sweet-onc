#!/bin/bash
# Build the freestanding arm64 logdump Mach-O (no libSystem).
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p out
SDK=$(xcrun --sdk macosx --show-sdk-path)
echo "SDK=$SDK"
xcrun --find clang
clang -target arm64-apple-macos11 -isysroot "$SDK" -O2 -Wall -Wextra \
	-fno-stack-protector -fno-builtin -nostdlib \
	-c tools/logdump/logdump.c -o out/logdump.o
ld -arch arm64 -static -e _start -o out/logdump out/logdump.o
codesign -s - out/logdump || true
file out/logdump
otool -hv out/logdump | head -5
otool -l out/logdump | grep -A3 LC_UNIXTHREAD | head -8
ls -l out/logdump
shasum -a 256 out/logdump | tee out/logdump.sha256
