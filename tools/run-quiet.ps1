<#
.SYNOPSIS
Start LoopyMSE-Enhanced without disturbing the desktop, wait for it, and
return its exit code.

.DESCRIPTION
The window (if the run has one at all) starts minimised and is never
activated, so the person at the computer keeps the focus; stdout and stderr
go to a file. Pair it with an unattended run (--script, --hidden, --fast,
--frames, --out ...): screenshots come from the emulator itself, so nothing
depends on the screen.

.EXAMPLE
.\run-quiet.ps1 -Exe .\LoopyMSE.exe -Arguments 'game.bin --script route.txt --hidden --fast --out run1' -Log run1.log
#>
param(
    [Parameter(Mandatory)] [string] $Exe,
    [string] $Arguments = '',
    [string] $Log = 'loopymse-run.log',
    [string] $WorkingDirectory = '.',
    [int] $TimeoutSeconds = 0
)
$ErrorActionPreference = 'Stop'

if (-not ('LoopyQuietStart' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class LoopyQuietStart {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit,
        uint flags, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool SetHandleInformation(IntPtr h, uint mask, uint flags);
    public static int Start(string app, string cmd, string dir, SafeFileHandle output) {
        IntPtr h = output.DangerousGetHandle();
        SetHandleInformation(h, 1, 1);
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(si);
        si.dwFlags = 1 | 0x100;     // STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES
        si.wShowWindow = 7;         // SW_SHOWMINNOACTIVE
        si.hStdOutput = h; si.hStdError = h;
        PROCESS_INFORMATION pi;
        if (!CreateProcess(app, cmd, IntPtr.Zero, IntPtr.Zero, true, 0, IntPtr.Zero, dir, ref si, out pi))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }
}
"@
}

$env:SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN = '1'
$exePath = (Resolve-Path $Exe).Path
$logFile = [System.IO.File]::Open((Join-Path (Resolve-Path $WorkingDirectory).Path $Log), 'Create', 'Write', 'ReadWrite')
try {
    $id = [LoopyQuietStart]::Start($exePath, "`"$exePath`" $Arguments", (Resolve-Path $WorkingDirectory).Path, $logFile.SafeFileHandle)
    $process = Get-Process -Id $id
    $limit = if ($TimeoutSeconds -gt 0) { $TimeoutSeconds * 1000 } else { -1 }
    if (-not $process.WaitForExit($limit)) {
        Write-Warning "Timed out after $TimeoutSeconds s; stopping process $id"
        Stop-Process -Id $id -Force
        exit 124
    }
    exit $process.ExitCode
} finally {
    $logFile.Close()
}
