#!/usr/bin/env bash
# 构建四个架构的 App_screen_off
# 用法：NDK=/path/to/android-ndk-r27c ./build.sh
set -e

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$(cd "${SRC_DIR}/.." && pwd)/bin"

NDK="${NDK:-${ANDROID_NDK_HOME}}"
if [ -z "${NDK}" ]; then
	echo "请先设置 NDK 环境变量，例如：NDK=~/android-ndk-r27c ./build.sh" >&2
	exit 1
fi

CC=""
for host in windows-x86_64 linux-x86_64 darwin-x86_64; do
	if [ -x "${NDK}/toolchains/llvm/prebuilt/${host}/bin/clang" ]; then
		CC="${NDK}/toolchains/llvm/prebuilt/${host}/bin/clang"
		break
	fi
	if [ -x "${NDK}/toolchains/llvm/prebuilt/${host}/bin/clang.exe" ]; then
		CC="${NDK}/toolchains/llvm/prebuilt/${host}/bin/clang.exe"
		break
	fi
done
if [ -z "${CC}" ]; then
	echo "在 ${NDK} 下找不到 clang" >&2
	exit 1
fi

TARGETS="aarch64-linux-android21:arm64-v8a armv7a-linux-androideabi21:armeabi-v7a i686-linux-android21:x86 x86_64-linux-android21:x86_64"

for item in ${TARGETS}; do
	triple="${item%%:*}"
	abi="${item##*:}"
	mkdir -p "${OUT_DIR}/${abi}"
	echo "编译 ${abi} (${triple})"
	"${CC}" --target="${triple}" -O2 -Wall -Wextra -fPIE -pie -s \
		-o "${OUT_DIR}/${abi}/App_screen_off" "${SRC_DIR}/App_restrict.c"
done

echo "完成，产物在 ${OUT_DIR}"
