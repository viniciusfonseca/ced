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
- The experimental extension host is disabled: its process and trust machinery
  currently requires POSIX APIs. Configuring it ON for Windows fails explicitly.
- The file tree skips symbolic links and directory junctions. File mutations
  reject reparse-point ancestors and protect parent directories against renames
  during the operation.

The exported folder contains the application and license notices; it is not an
installer. No code signing or Windows installer registration is performed.
