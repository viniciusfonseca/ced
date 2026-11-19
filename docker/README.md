# Windows cross-builds with Docker

Run these commands from the repository root, after initializing the submodules:

```sh
git submodule update --init --recursive
docker buildx build -f docker/Dockerfile.windows-x86 --output type=local,dest=dist/windows-x86 .
docker buildx build -f docker/Dockerfile.windows-x64 --output type=local,dest=dist/windows-x64 .
docker buildx build -f docker/Dockerfile.windows-arm64 --output type=local,dest=dist/windows-arm64 .
```

The same commands work in PowerShell. On Windows, use Docker Desktop in **Linux
containers** mode. The builder can run on Linux, macOS, or Windows with Docker
Desktop. Linux AMD64 and ARM64 builders are supported. Each toolchain runs on
the builder's own CPU and cross-compiles to the selected Windows CPU, so changing
the Docker `--platform` is unnecessary.

| Dockerfile | Windows target | Exported executable |
| --- | --- | --- |
| `Dockerfile.windows-x86` | x86 / i686, 32-bit | `dist/windows-x86/ced.exe` |
| `Dockerfile.windows-x64` | x64 / x86_64, 64-bit | `dist/windows-x64/ced.exe` |
| `Dockerfile.windows-arm64` | ARM64 / aarch64, 64-bit | `dist/windows-arm64/ced.exe` |

Each output includes `ced.exe`, a Windows usage README, and third-party license
notices. The final Docker stage is an artifact export, not a runnable container.
The Windows application does not require Docker at runtime. See
[Windows functionality and requirements](WINDOWS.md), including the experimental
extension host limitation.

The build uses Ubuntu 24.04 and LLVM/MinGW **20261117**, downloaded from its
[upstream release](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261117).
Published SHA-256 checksums are checked before extraction. CUI, cJSON and libvterm
come from the initialized local submodules; Docker does not fetch different
dependency revisions or require Git credentials. Downloading the base image,
Ubuntu packages and compiler requires network access.

Use `--build-arg BUILD_JOBS=2` to reduce compilation concurrency. Output folders
under `dist/` are already ignored by Git and excluded from the Docker context.
Native build folders, Git metadata, and local caches are also excluded.

Cross-compilation checks the Windows source and link dependencies. It cannot run
the resulting executables on the Linux builder; validate runtime behavior on
Windows for each target CPU before distributing a release.

The native Windows CMake test target is `ced_windows_tests` (`BUILD_TESTING=ON`).
Run it through CTest on Windows to check UTF-8 files, atomic saves, protected file
operations, preferences, process arguments, binary stdin/stdout and large output.
This test can also run under 64-bit Wine with `LANG=C.UTF-8` and `LC_ALL=C.UTF-8`;
Wine checks do not validate the native window system or ConPTY behavior.
