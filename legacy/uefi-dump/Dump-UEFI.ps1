# Dump-UEFI.ps1 - READ-ONLY dump of all UEFI (BIOS NVRAM) variables using Windows' own API.
# Writes nothing to the BIOS. Saves each variable to a .bin file plus an index.csv, then zips the result.
# Must run as Administrator (needs SeSystemEnvironmentPrivilege). Windows 8 or newer, UEFI boot.

$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Please run this as Administrator (use Run-Dump.bat)." -ForegroundColor Red
    exit 1
}

$src = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public class UefiVar {
    public string Name;
    public string Guid;
    public uint Attributes;
    public byte[] Data;
    public int Error;
}

public static class UefiReader {
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct TOKEN_PRIVILEGES { public uint Count; public long Luid; public uint Attr; }

    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] static extern bool LookupPrivilegeValue(string system, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TOKEN_PRIVILEGES state, uint len, IntPtr prev, IntPtr retLen);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("ntdll.dll")] static extern int NtEnumerateSystemEnvironmentValuesEx(uint infoClass, IntPtr buffer, ref uint length);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern uint GetFirmwareEnvironmentVariableExW(string name, string guid, IntPtr buffer, uint size, out uint attributes);

    public static string EnablePrivilege() {
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(), 0x0020 | 0x0008, out token)) return "OpenProcessToken failed: " + Marshal.GetLastWin32Error();
        try {
            long luid;
            if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out luid)) return "LookupPrivilegeValue failed: " + Marshal.GetLastWin32Error();
            TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
            tp.Count = 1; tp.Luid = luid; tp.Attr = 2; // SE_PRIVILEGE_ENABLED
            if (!AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero)) return "AdjustTokenPrivileges failed: " + Marshal.GetLastWin32Error();
            int err = Marshal.GetLastWin32Error();
            if (err == 1300) return "Privilege not held (ERROR_NOT_ALL_ASSIGNED) - run as Administrator";
            return null;
        } finally { CloseHandle(token); }
    }

    public static List<UefiVar> ReadAll() {
        List<UefiVar> result = new List<UefiVar>();
        uint len = 0;
        int status = NtEnumerateSystemEnvironmentValuesEx(1, IntPtr.Zero, ref len); // 1 = names
        if (len == 0) throw new Exception(string.Format("Could not enumerate UEFI variables (status 0x{0:X8}). Is Windows booted in UEFI mode?", status));
        len += 4096;
        IntPtr buf = Marshal.AllocHGlobal((int)len);
        try {
            status = NtEnumerateSystemEnvironmentValuesEx(1, buf, ref len);
            if (status != 0) throw new Exception(string.Format("Enumeration failed (status 0x{0:X8})", status));
            int off = 0;
            while (true) {
                IntPtr entry = new IntPtr(buf.ToInt64() + off);
                int next = Marshal.ReadInt32(entry);
                byte[] g = new byte[16];
                Marshal.Copy(new IntPtr(entry.ToInt64() + 4), g, 0, 16);
                string name = Marshal.PtrToStringUni(new IntPtr(entry.ToInt64() + 20));
                UefiVar v = new UefiVar();
                v.Name = name;
                v.Guid = new Guid(g).ToString("B");
                Read(v);
                result.Add(v);
                if (next == 0) break;
                off += next;
                if (off >= len) break;
            }
        } finally { Marshal.FreeHGlobal(buf); }
        return result;
    }

    static void Read(UefiVar v) {
        uint size = 4096;
        while (size <= 4 * 1024 * 1024) {
            IntPtr b = Marshal.AllocHGlobal((int)size);
            try {
                uint attr;
                uint got = GetFirmwareEnvironmentVariableExW(v.Name, v.Guid, b, size, out attr);
                if (got > 0) {
                    v.Data = new byte[got];
                    Marshal.Copy(b, v.Data, 0, (int)got);
                    v.Attributes = attr;
                    v.Error = 0;
                    return;
                }
                int err = Marshal.GetLastWin32Error();
                if (err == 122) { size *= 4; continue; } // ERROR_INSUFFICIENT_BUFFER
                v.Error = err;
                return;
            } finally { Marshal.FreeHGlobal(b); }
        }
        v.Error = 122;
    }
}
"@

Add-Type -TypeDefinition $src -Language CSharp

$msg = [UefiReader]::EnablePrivilege()
if ($msg) { Write-Host $msg -ForegroundColor Red; exit 1 }

Write-Host "Reading UEFI variables (read-only)..." -ForegroundColor Cyan
$vars = [UefiReader]::ReadAll()

$stamp = Get-Date -Format 'yyyyMMdd_HHmm'
$outDir = Join-Path $PSScriptRoot "UEFI_Dump_$stamp"
$varDir = Join-Path $outDir 'vars'
New-Item -ItemType Directory -Path $varDir -Force | Out-Null

$rows = foreach ($v in $vars) {
    $safe = ($v.Name -replace '[^A-Za-z0-9_\-\.]', '_')
    if ($safe.Length -gt 80) { $safe = $safe.Substring(0, 80) }
    $file = '{0}__{1}.bin' -f $safe, $v.Guid.Trim('{}')
    if ($null -ne $v.Data) { [IO.File]::WriteAllBytes((Join-Path $varDir $file), $v.Data) }
    [pscustomobject]@{
        Name       = $v.Name
        Guid       = $v.Guid
        Size       = if ($null -ne $v.Data) { $v.Data.Length } else { 0 }
        Attributes = '0x{0:X}' -f $v.Attributes
        Error      = $v.Error
        File       = if ($null -ne $v.Data) { $file } else { '' }
    }
}
$rows | Export-Csv -Path (Join-Path $outDir 'index.csv') -NoTypeInformation -Encoding UTF8

$bios = Get-CimInstance Win32_BIOS
$board = Get-CimInstance Win32_BaseBoard
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
@(
    "Dumped:     $(Get-Date -Format 's')"
    "Board:      $($board.Manufacturer) $($board.Product)"
    "BIOS:       $($bios.Manufacturer) $($bios.SMBIOSBIOSVersion) ($($bios.ReleaseDate))"
    "CPU:        $($cpu.Name)"
    "Variables:  $($rows.Count) found, $(($rows | Where-Object Size -gt 0).Count) read"
) | Set-Content -Path (Join-Path $outDir 'system.txt') -Encoding UTF8

$zip = "$outDir.zip"
Compress-Archive -Path (Join-Path $outDir '*') -DestinationPath $zip -Force

Write-Host ""
Write-Host "Done. $($rows.Count) variables found." -ForegroundColor Green
Write-Host "Largest variables (the BIOS settings usually live in Setup / CpuSetup / SaSetup / PchSetup / MeSetup):"
$rows | Sort-Object Size -Descending | Select-Object -First 15 Name, Size, Guid | Format-Table -AutoSize
$failed = $rows | Where-Object { $_.Error -ne 0 }
if ($failed) { Write-Host "$($failed.Count) variables could not be read (normal for protected ones)." -ForegroundColor Yellow }
Write-Host "Saved: $zip" -ForegroundColor Green
