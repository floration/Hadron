// modules/services/recovery.cpp — restore.cmd, restore-online.cmd, README.txt and the stored-only zip.
#include "modules/services/recovery.h"
#include <cstring>

namespace hx::services {

namespace {
std::string A(const std::wstring& s) {   // ASCII-only text (callers pass only CmdSafeName names)
    std::string o;
    for (wchar_t c : s) o += (c < 0x80) ? (char)c : '?';
    return o;
}
void L(std::string& o, const std::string& line) { o += line; o += "\r\n"; }
std::string Rem(const std::wstring& s) {   // text for a rem/echo line: no cmd metacharacters
    std::string o = A(s);
    std::string r;
    for (char c : o) {
        if (strchr("&|<>^%!()\"", c)) c = ' ';
        if (c == ' ' && (r.empty() || r.back() == ' ')) continue;   // collapse runs, no leading space
        r += c;
    }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}
std::string StartNameA(int s) { return A(StartName(s)); }
}  // namespace

bool CmdSafeName(const std::wstring& name) {
    if (name.empty()) return false;
    for (wchar_t c : name) {
        if (c >= 0x80 || c < 0x20) return false;
        if (wcschr(L"%!^&|<>\"=;,", c)) return false;
    }
    return true;
}

std::string RestoreCmd(const std::wstring& stamp, const std::wstring& title, const std::vector<RestoreRow>& rows) {
    std::string o;
    L(o, "@echo off");
    L(o, "rem Hadron services offline recovery, " + Rem(stamp) + ": " + Rem(title));
    L(o, "rem Run from WinRE > Troubleshoot > Advanced options > Command Prompt. See README.txt.");
    L(o, "rem It writes only Start (and DelayedAutostart where noted) for service keys that already exist.");
    L(o, "rem It never creates or deletes a key and never searches other drives.");
    L(o, "setlocal EnableExtensions");
    L(o, "set \"T=%~1\"");
    L(o, "if \"%T%\"==\"\" set \"T=%~d0\"");
    L(o, "if not exist \"%T%\\Windows\\System32\\config\\SYSTEM\" goto :nowin");
    L(o, "echo Windows to repair: %T%\\Windows");
    L(o, "reg load HKLM\\HXOFF \"%T%\\Windows\\System32\\config\\SYSTEM\"");
    L(o, "if errorlevel 1 goto :noload");
    L(o, "set \"CS=\"");
    L(o, "for /f \"tokens=3\" %%V in ('reg query HKLM\\HXOFF\\Select /v Current 2^>nul ^| find \"Current\"') do set /a CS=%%V");
    L(o, "if not defined CS goto :nosel");
    L(o, "set \"CSN=00%CS%\"");
    L(o, "set \"K=HKLM\\HXOFF\\ControlSet%CSN:~-3%\\Services\"");
    L(o, "echo Control set: ControlSet%CSN:~-3%");
    L(o, "set /a OK=0");
    L(o, "set /a MISS=0");
    L(o, "set /a BAD=0");
    int skipped = 0;
    for (auto& r : rows) {
        if (!CmdSafeName(r.name)) { skipped++; continue; }
        std::string d = r.writeDelayed ? std::to_string(r.cfg.delayed < 0 ? 0 : r.cfg.delayed) : "-";
        L(o, "call :put \"" + A(r.name) + "\" " + std::to_string(r.cfg.start) + " " + d +
                 "   & rem " + StartNameA(r.cfg.start) + (r.cfg.Delayed() ? " (delayed)" : ""));
    }
    if (skipped)
        L(o, "echo " + std::to_string(skipped) + " name(s) can't be typed in this script: see README.txt, section C.");
    L(o, "reg unload HKLM\\HXOFF");
    L(o, "echo Done: %OK% restored, %MISS% not on this Windows, %BAD% failed.");
    L(o, "echo Close this window and choose Continue to start Windows.");
    L(o, "exit /b 0");
    L(o, "");
    L(o, ":put");
    L(o, "reg query \"%K%\\%~1\" >nul 2>&1");
    L(o, "if errorlevel 1 (echo skip %~1: no such key & set /a MISS+=1 & exit /b 0)");
    L(o, "reg add \"%K%\\%~1\" /v Start /t REG_DWORD /d %~2 /f >nul");
    L(o, "if errorlevel 1 (echo FAILED %~1 & set /a BAD+=1 & exit /b 0)");
    L(o, "if not \"%~3\"==\"-\" reg add \"%K%\\%~1\" /v DelayedAutostart /t REG_DWORD /d %~3 /f >nul");
    L(o, "echo restored %~1 Start=%~2");
    L(o, "set /a OK+=1");
    L(o, "exit /b 0");
    L(o, "");
    L(o, ":nowin");
    L(o, "echo No Windows installation at %T% (no %T%\\Windows\\System32\\config\\SYSTEM).");
    L(o, "echo Run it with the drive that holds Windows, for example: restore.cmd D:");
    L(o, "exit /b 1");
    L(o, ":noload");
    L(o, "echo reg load failed: is the drive BitLocker-locked? See README.txt, section A.");
    L(o, "exit /b 1");
    L(o, ":nosel");
    L(o, "echo Could not read Select\\Current from the offline hive. Nothing was changed.");
    L(o, "reg unload HKLM\\HXOFF");
    L(o, "exit /b 1");
    return o;
}

std::string RestoreOnlineCmd(const std::wstring& stamp, const std::vector<RestoreRow>& rows) {
    std::string o;
    L(o, "@echo off");
    L(o, "rem Hadron services recovery, " + A(stamp) + ", for Safe Mode with Command Prompt (Windows running).");
    L(o, "rem Admin check with fltmc (Hadron rule 4). Writes Start for existing keys; takes effect at the next restart.");
    L(o, "setlocal EnableExtensions");
    L(o, "fltmc >nul 2>&1");
    L(o, "if errorlevel 1 (echo Run this from an administrator Command Prompt. & exit /b 1)");
    L(o, "set \"K=HKLM\\SYSTEM\\CurrentControlSet\\Services\"");
    L(o, "set /a OK=0");
    L(o, "set /a MISS=0");
    L(o, "set /a BAD=0");
    for (auto& r : rows) {
        if (!CmdSafeName(r.name)) continue;
        std::string d = r.writeDelayed ? std::to_string(r.cfg.delayed < 0 ? 0 : r.cfg.delayed) : "-";
        L(o, "call :put \"" + A(r.name) + "\" " + std::to_string(r.cfg.start) + " " + d);
    }
    L(o, "echo Done: %OK% restored, %MISS% not found, %BAD% failed. Restart Windows normally.");
    L(o, "exit /b 0");
    L(o, "");
    L(o, ":put");
    L(o, "reg query \"%K%\\%~1\" >nul 2>&1");
    L(o, "if errorlevel 1 (echo skip %~1: no such key & set /a MISS+=1 & exit /b 0)");
    L(o, "reg add \"%K%\\%~1\" /v Start /t REG_DWORD /d %~2 /f >nul");
    L(o, "if errorlevel 1 (echo FAILED %~1 & set /a BAD+=1 & exit /b 0)");
    L(o, "if not \"%~3\"==\"-\" reg add \"%K%\\%~1\" /v DelayedAutostart /t REG_DWORD /d %~3 /f >nul");
    L(o, "echo restored %~1 Start=%~2");
    L(o, "set /a OK+=1");
    L(o, "exit /b 0");
    return o;
}

std::string ReadmeTxt(const std::wstring& stamp, const std::wstring& folder, const std::wstring& title,
                      const std::vector<RestoreRow>& rows) {
    std::string o;
    std::string f = A(folder);
    L(o, "Hadron services recovery " + A(stamp));
    L(o, "What this restores: " + A(title));
    L(o, "Only start types are restored. Service state can't be restored offline.");
    L(o, "");
    L(o, "A. If Windows doesn't start (WinRE)");
    L(o, "1. Windows opens the recovery environment by itself after failed starts. Choose");
    L(o, "   Troubleshoot > Advanced options > Command Prompt.");
    L(o, "2. If the Windows drive uses BitLocker, unlock it first (you need the recovery key):");
    L(o, "     manage-bde -unlock C: -RecoveryPassword <your-48-digit-key>");
    L(o, "   (use the drive letter WinRE shows for Windows; it is often not C:).");
    L(o, "3. Find the drive that holds Windows. Either try:  dir C:\\Windows  then  dir D:\\Windows");
    L(o, "   or run:  bcdedit | find \"osdevice\"");
    L(o, "4. Run, replacing D: with that drive:");
    L(o, "     D:" + f + "\\restore.cmd D:");
    L(o, "   The script refuses if D:\\Windows\\System32\\config\\SYSTEM is missing, loads that hive,");
    L(o, "   reads Select\\Current, writes the values below for keys that exist, and unloads it.");
    L(o, "5. Close the window and choose Continue.");
    L(o, "");
    L(o, "If you copied this folder to a USB stick, run it from there and give the Windows drive:");
    L(o, "     E:\\restore.cmd D:");
    L(o, "WinRE can't open .zip files: use the plain folder. The .zip beside it is the same files.");
    L(o, "");
    L(o, "B. If Windows starts in Safe Mode");
    L(o, "Open an administrator Command Prompt and run:");
    L(o, "     C:" + f + "\\restore-online.cmd     (C: is where Windows normally runs)");
    L(o, "It checks for admin rights with fltmc and writes the same values to the running registry.");
    L(o, "Or start Hadron in Safe Mode and use Undo.");
    L(o, "");
    std::vector<const RestoreRow*> manual;
    for (auto& r : rows) if (!CmdSafeName(r.name)) manual.push_back(&r);
    L(o, "C. Names the scripts can't carry");
    if (manual.empty()) {
        L(o, "None.");
    } else {
        L(o, "These names have characters a batch file can't hold safely. They are in");
        L(o, "restore-unicode-ControlSet001.reg (and ...002, ...003). In WinRE: run regedit, select");
        L(o, "HKEY_LOCAL_MACHINE, File > Load Hive, pick <drive>:\\Windows\\System32\\config\\SYSTEM,");
        L(o, "name it HXOFF, check HXOFF\\Select\\Current, check each key below exists under that");
        L(o, "control set, then File > Import the matching file. Importing creates a missing key, so");
        L(o, "check first. Then select HXOFF and File > Unload Hive.");
        L(o, "Count: " + std::to_string(manual.size()));
    }
    L(o, "");
    L(o, "D. Values (" + std::to_string(rows.size()) + " items)");
    for (auto& r : rows) {
        std::string n = CmdSafeName(r.name) ? A(r.name) : "(see C) " + ToUtf8(r.name);
        L(o, "  " + n + "  Start=" + std::to_string(r.cfg.start) + " (" + StartNameA(r.cfg.start) + ")" +
                 (r.writeDelayed ? "  DelayedAutostart=" + std::to_string(r.cfg.delayed < 0 ? 0 : r.cfg.delayed) : ""));
    }
    return o;
}

std::string UnicodeReg(const std::vector<RestoreRow>& rows, int controlSet) {
    std::wstring t = L"\xFEFFWindows Registry Editor Version 5.00\r\n\r\n";
    wchar_t cs[8];
    swprintf(cs, 8, L"%03d", controlSet);
    for (auto& r : rows) {
        if (CmdSafeName(r.name)) continue;
        t += L"[HKEY_LOCAL_MACHINE\\HXOFF\\ControlSet" + std::wstring(cs) + L"\\Services\\" + r.name + L"]\r\n";
        wchar_t v[64];
        swprintf(v, 64, L"\"Start\"=dword:%08x\r\n", (unsigned)r.cfg.start);
        t += v;
        if (r.writeDelayed) {
            swprintf(v, 64, L"\"DelayedAutostart\"=dword:%08x\r\n", (unsigned)(r.cfg.delayed < 0 ? 0 : r.cfg.delayed));
            t += v;
        }
        t += L"\r\n";
    }
    std::string o;
    for (wchar_t c : t) { o += (char)(c & 0xFF); o += (char)((c >> 8) & 0xFF); }
    return o;
}

// ---- stored-only zip (local headers, central directory, UTF-8 name flag) ---------------------------
namespace {
void P16(std::string& o, uint16_t v) { o += (char)(v & 0xFF); o += (char)(v >> 8); }
void P32(std::string& o, uint32_t v) { for (int i = 0; i < 4; i++) o += (char)((v >> (8 * i)) & 0xFF); }
uint16_t G16(const std::string& s, size_t i) { return (uint16_t)((uint8_t)s[i] | ((uint8_t)s[i + 1] << 8)); }
uint32_t G32(const std::string& s, size_t i) { return (uint32_t)G16(s, i) | ((uint32_t)G16(s, i + 2) << 16); }
}  // namespace

std::string MakeZip(const std::vector<ZipEntry>& entries, uint16_t dosTime, uint16_t dosDate) {
    std::string out, central;
    for (auto& e : entries) {
        uint32_t crc = Crc32(e.data.data(), e.data.size());
        uint32_t off = (uint32_t)out.size();
        P32(out, 0x04034b50); P16(out, 20); P16(out, 0x0800 /* UTF-8 names */); P16(out, 0 /* stored */);
        P16(out, dosTime); P16(out, dosDate); P32(out, crc); P32(out, (uint32_t)e.data.size());
        P32(out, (uint32_t)e.data.size()); P16(out, (uint16_t)e.name.size()); P16(out, 0);
        out += e.name;
        out += e.data;
        P32(central, 0x02014b50); P16(central, 20); P16(central, 20); P16(central, 0x0800); P16(central, 0);
        P16(central, dosTime); P16(central, dosDate); P32(central, crc); P32(central, (uint32_t)e.data.size());
        P32(central, (uint32_t)e.data.size()); P16(central, (uint16_t)e.name.size()); P16(central, 0); P16(central, 0);
        P16(central, 0); P16(central, 0); P32(central, 0); P32(central, off);
        central += e.name;
    }
    uint32_t cdOff = (uint32_t)out.size();
    out += central;
    P32(out, 0x06054b50); P16(out, 0); P16(out, 0); P16(out, (uint16_t)entries.size()); P16(out, (uint16_t)entries.size());
    P32(out, (uint32_t)central.size()); P32(out, cdOff); P16(out, 0);
    return out;
}

bool ParseZip(const std::string& z, std::vector<ZipEntry>* out, std::wstring* err) {
    out->clear();
    if (z.size() < 22) { *err = L"zip too short"; return false; }
    size_t eocd = z.size() - 22;
    if (G32(z, eocd) != 0x06054b50) { *err = L"no end-of-central-directory record"; return false; }
    uint16_t n = G16(z, eocd + 10);
    uint32_t cdSize = G32(z, eocd + 12), cdOff = G32(z, eocd + 16);
    if ((size_t)cdOff + cdSize > eocd) { *err = L"central directory out of range"; return false; }
    size_t p = cdOff;
    for (uint16_t k = 0; k < n; k++) {
        if (p + 46 > z.size() || G32(z, p) != 0x02014b50) { *err = L"bad central header"; return false; }
        uint16_t flags = G16(z, p + 8), method = G16(z, p + 10);
        uint32_t crc = G32(z, p + 16), csize = G32(z, p + 20), usize = G32(z, p + 24);
        uint16_t nlen = G16(z, p + 28), xlen = G16(z, p + 30), clen = G16(z, p + 32);
        uint32_t loff = G32(z, p + 42);
        std::string name = z.substr(p + 46, nlen);
        if (method != 0 || csize != usize) { *err = L"entry is not stored"; return false; }
        if (!(flags & 0x0800)) { *err = L"UTF-8 flag missing"; return false; }
        if (loff + 30 > z.size() || G32(z, loff) != 0x04034b50) { *err = L"bad local header"; return false; }
        if (G32(z, loff + 14) != crc || G16(z, loff + 26) != nlen || z.substr(loff + 30, nlen) != name) {
            *err = L"local and central headers disagree"; return false;
        }
        size_t data = loff + 30 + nlen + G16(z, loff + 28);
        if (data + usize > z.size()) { *err = L"entry data out of range"; return false; }
        ZipEntry e{name, z.substr(data, usize)};
        if (Crc32(e.data.data(), e.data.size()) != crc) { *err = L"CRC mismatch in " + FromUtf8(name); return false; }
        out->push_back(std::move(e));
        p += 46 + nlen + xlen + clen;
    }
    return true;
}

bool WriteRecovery(Store& store, const std::wstring& stamp, const std::wstring& title, const std::vector<RestoreRow>& rows,
                   std::wstring* dir, std::wstring* zip, std::wstring* err) {
    // The folder as seen from WinRE: the data dir without its drive letter.
    std::wstring rel = L"recovery\\" + stamp;
    std::wstring full = store.Path(rel);
    std::wstring folder = full.size() > 2 && full[1] == L':' ? full.substr(2) : full;
    if (!store.MakeDir(L"recovery", err) || !store.MakeDir(rel, err)) return false;
    std::vector<ZipEntry> files;
    files.push_back({"restore.cmd", RestoreCmd(stamp, title, rows)});
    files.push_back({"restore-online.cmd", RestoreOnlineCmd(stamp, rows)});
    files.push_back({"README.txt", ReadmeTxt(stamp, folder, title, rows)});
    bool unicode = false;
    for (auto& r : rows) if (!CmdSafeName(r.name)) unicode = true;
    if (unicode)
        for (int cs = 1; cs <= 3; cs++)
            files.push_back({"restore-unicode-ControlSet00" + std::to_string(cs) + ".reg", UnicodeReg(rows, cs)});
    for (auto& f : files)
        if (!store.WriteNew(rel + L"\\" + FromUtf8(f.name), f.data, err)) return false;
    // DOS time from the stamp (yyyyMMdd-HHmmss) so the zip is reproducible.
    auto num = [&](size_t a, size_t n) { return stamp.size() >= a + n ? _wtoi(stamp.substr(a, n).c_str()) : 0; };
    uint16_t dosDate = (uint16_t)(((num(0, 4) - 1980) & 0x7F) << 9 | (num(4, 2) & 0xF) << 5 | (num(6, 2) & 0x1F));
    uint16_t dosTime = (uint16_t)((num(9, 2) & 0x1F) << 11 | (num(11, 2) & 0x3F) << 5 | ((num(13, 2) / 2) & 0x1F));
    std::string z = MakeZip(files, dosTime, dosDate);
    std::wstring zrel = L"recovery-" + stamp + L".zip";
    if (!store.WriteNew(zrel, z, err)) return false;
    *dir = full;
    *zip = store.Path(zrel);
    return true;
}

}  // namespace hx::services
