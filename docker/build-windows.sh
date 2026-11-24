#!/bin/sh
set -eu
arch=${1:?Expected i686, x86_64 or aarch64}
case "$arch" in i686|x86_64|aarch64) ;; *) echo "Unsupported Windows CPU: $arch" >&2; exit 1 ;; esac
cmake -S /src -B /build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/src/docker/windows-toolchain.cmake \
    -DCED_WINDOWS_ARCH="$arch" -DCMAKE_BUILD_TYPE=Release \
    -DCED_ENABLE_EXTENSIONS=ON -DBUILD_TESTING=OFF
cmake --build /build --target ced ced_uppercase_extension --parallel "${BUILD_JOBS:-4}"
mkdir -p /out/licenses
cp /build/ced.exe /out/ced.exe
cp -R /build/examples /out/examples
cp /src/THIRD_PARTY.md /out/THIRD_PARTY.md
cp /src/docker/WINDOWS.md /out/README.md
cp /src/third_party/CUI/LICENSE /out/licenses/CUI.txt
cp /src/third_party/cjson/LICENSE /out/licenses/cJSON.txt
cp /src/third_party/libvterm/LICENSE /out/licenses/libvterm.txt
cp /src/third_party/CUI/assets/fonts/LICENSE-* /out/licenses/
cp /src/third_party/CUI/third_party/LICENSE-Unicode.txt /out/licenses/
# Static winpthreads and compiler runtime are covered by these toolchain notices.
cp /opt/llvm-mingw/LICENSE* /out/licenses/
cp /opt/llvm-mingw/"$arch"-w64-mingw32/share/mingw32/COPYING* /out/licenses/
file /out/ced.exe
