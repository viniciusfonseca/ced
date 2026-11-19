#!/bin/sh
set -eu

# Pin both the release and its published asset checksums. The builder's CPU is
# independent from the Windows target CPU; no target emulation is required.
version=20261117
case "$(dpkg --print-architecture)" in
    amd64) host=x86_64; checksum=e7a15a49a2ea95e9f3d8a9d0cfabe5052a4b10fb4777cc54de5a8c27b64bb701 ;;
    arm64) host=aarch64; checksum=6d9442ead9ace3b3e4c3389bfef69f93f873305e0705e6cc60978ac895320872 ;;
    *) echo "Unsupported Linux builder architecture" >&2; exit 1 ;;
esac
archive="llvm-mingw-$version-ucrt-ubuntu-22.04-$host.tar.xz"
curl --fail --location --retry 3 "https://github.com/mstorsjo/llvm-mingw/releases/download/$version/$archive" -o /tmp/llvm-mingw.tar.xz
printf '%s  %s\n' "$checksum" /tmp/llvm-mingw.tar.xz | sha256sum --check --strict
mkdir -p /opt/llvm-mingw
tar -xJf /tmp/llvm-mingw.tar.xz --strip-components=1 -C /opt/llvm-mingw
rm /tmp/llvm-mingw.tar.xz
