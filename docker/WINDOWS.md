# CED for Windows (experimental)

Run `ced.exe`, or launch it from PowerShell with a workspace:

```powershell
.\ced.exe 'C:\Projects\my-project'
```

These are native Windows executables, cross-compiled with LLVM/MinGW and the
Win32 CUI backend. They do not require WSL, Docker, MSYS2 or Cygwin at runtime.
The compiler runtime and winpthreads are linked statically. Windows system DLLs
and the Universal C Runtime must be available. Keep the accompanying notices
when redistributing the package.

Use the executable matching your Windows CPU: x86 (32-bit), x64 (64-bit Intel/AMD),
or ARM64. Windows 10 version 1809 or newer is required for the integrated ConPTY
terminal; ARM64 builds require Windows on ARM. This is an initial Windows port;
successful cross-compilation does not establish runtime parity with macOS/Linux.

- Install Git for Windows and put `git.exe` on `PATH` for Git integration.
- Install Windows clangd and put `clangd.exe` on `PATH`, or set `CED_CLANGD` to
  its absolute path, for C/C++ language services.
- The terminal starts `%COMSPEC%` (normally `cmd.exe`). Set `SHELL` to the full
  path to another Windows shell if desired.
- Preferences are stored in `%USERPROFILE%\.ced\settings.json` when `HOME` is
  unset. An existing `HOME` takes precedence.
- Custom font paths accept `C:/Windows/Fonts/consola.ttf`, for example. Installed
  font lookup uses the Windows font registry; use `editor.fontPath` if a family
  cannot be resolved. The embedded default font is always available.
- The experimental extension host is enabled. Use **Install Extension from
  Folder** to review and install a package, then use **Manage Extensions** to
  trust the current workspace. The included example is at
  `examples/extensions/uppercase-selection`. Executable packages require a
  native `.exe` entrypoint for `windows-i686`, `windows-x86_64` or
  `windows-arm64`. Use `--disable-extensions` to start without extensions.
- The file tree skips symbolic links and directory junctions. File mutations
  reject reparse-point ancestors and protect parent directories against renames
  during the operation.

The exported folder contains the application and license notices; it is not an
installer. No code signing or Windows installer registration is performed.

## Native VM validation

On 2026-11-19, the application was built and launched in GitHub-hosted Windows
Server 2025 x64 and Windows 11 Enterprise ARM64 virtual machines. Both passed
the native filesystem/process/preferences tests and a real ConPTY shell test.
The editor window rendered in both VMs; the integrated terminal was also
visually confirmed in the x64 window. The ARM64 desktop's Windows setup overlay
blocked the global keyboard shortcut, so its window was inspected through
`PrintWindow` instead of the desktop capture.

The test found and fixed ConPTY inheriting redirected parent standard handles.
See the [successful VM run](https://github.com/viniciusfonseca/ced/actions/runs/44552793809),
the `Windows VM Validation` workflow, and `packaging/windows-vm-smoke.ps1`.
The x86 executable has been cross-compiled but has not been run in a native VM.
