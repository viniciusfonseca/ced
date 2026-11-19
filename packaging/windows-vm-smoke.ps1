param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CedWindow {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
}
'@
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$output = (Resolve-Path $OutputDirectory).Path
$workspace = Join-Path $output 'workspace'
$preferencesRoot = Join-Path $output 'profile'
New-Item -ItemType Directory -Force $workspace, $preferencesRoot | Out-Null
Set-Content -LiteralPath (Join-Path $workspace 'sample.c') -Encoding utf8NoBOM -Value @'
#include <stdio.h>

int main(void) {
    printf("CED on a Windows VM\n");
    return 0;
}
'@
$env:HOME = $preferencesRoot
$env:SHELL = $env:COMSPEC
$system = Get-CimInstance Win32_OperatingSystem
$processor = Get-CimInstance Win32_Processor | Select-Object -First 1
$report = [ordered]@{ os=$system.Caption; version=$system.Version; cpu=$processor.Name; architecture=$env:PROCESSOR_ARCHITECTURE; session=(Get-Process -Id $PID).SessionId }
$arguments = @('--disable-extensions', ('"' + $workspace + '"'), 'sample.c')
$process = Start-Process -FilePath (Resolve-Path $Executable).Path -ArgumentList $arguments -PassThru `
    -RedirectStandardOutput (Join-Path $output 'stdout.log') -RedirectStandardError (Join-Path $output 'stderr.log')
try {
    $deadline = (Get-Date).AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited) { throw "CED exited early with code $($process.ExitCode)" }
    } while ($process.MainWindowHandle -eq 0 -and (Get-Date) -lt $deadline)
    if ($process.MainWindowHandle -eq 0) { throw 'CED did not create a native window' }
    $window = $process.MainWindowHandle
    [CedWindow]::ShowWindow($window, 9) | Out-Null
    [CedWindow]::SetForegroundWindow($window) | Out-Null
    Start-Sleep -Seconds 2
    function Capture-Ced([string]$name) {
        $rect = New-Object CedWindow+Rect
        if (![CedWindow]::GetWindowRect($window, [ref]$rect)) { throw 'GetWindowRect failed' }
        $width = $rect.Right - $rect.Left
        $height = $rect.Bottom - $rect.Top
        if ($width -lt 500 -or $height -lt 300) { throw "Unexpected window size: ${width}x${height}" }
        $bitmap = New-Object System.Drawing.Bitmap $width, $height
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
            $bitmap.Save((Join-Path $output "$name-screen.png"), [System.Drawing.Imaging.ImageFormat]::Png)
            $dc = $graphics.GetHdc()
            try { [CedWindow]::PrintWindow($window, $dc, 2) | Out-Null } finally { $graphics.ReleaseHdc($dc) }
            $bitmap.Save((Join-Path $output "$name-window.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
        $report['windowWidth'] = $width; $report['windowHeight'] = $height
    }
    Capture-Ced 'editor'
    [System.Windows.Forms.SendKeys]::SendWait('^j')
    Start-Sleep -Seconds 2
    Capture-Ced 'terminal'
    $process.Refresh()
    if ($process.HasExited) { throw "CED exited with code $($process.ExitCode)" }
    $report['windowTitle'] = $process.MainWindowTitle
    $report['running'] = $true
    $report | ConvertTo-Json | Set-Content (Join-Path $output 'result.json')
    $report | ConvertTo-Json
} catch {
    $report['error'] = $_.Exception.Message
    $report | ConvertTo-Json | Set-Content (Join-Path $output 'result.json')
    throw
} finally {
    if (!$process.HasExited) { $process.CloseMainWindow() | Out-Null; if (!$process.WaitForExit(5000)) { $process.Kill($true) } }
}
