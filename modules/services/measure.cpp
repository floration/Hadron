// modules/services/measure.cpp — live measurement, its file format, and before/after pairing (measure.h).
#include "modules/services/measure.h"
#include "core/sys/sys.h"
#include <tlhelp32.h>
#include <algorithm>
#include <cwctype>

namespace hx::services {

namespace {
std::wstring TimeText(const FILETIME& ft) {
    SYSTEMTIME t{};
    if (!FileTimeToSystemTime(&ft, &t)) return L"";
    wchar_t b[32];
    swprintf(b, 32, L"%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
uint64_t U64(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }
FILETIME Ft(uint64_t v) { FILETIME f; f.dwLowDateTime = (DWORD)v; f.dwHighDateTime = (DWORD)(v >> 32); return f; }
bool Digits(const std::wstring& s) {
    return !s.empty() && s.size() <= 20 && std::all_of(s.begin(), s.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}
const wchar_t* const kValues[] = {L"ram_used", L"ram_total", L"commit", L"commit_limit", L"processes", L"svchost"};
}  // namespace

std::wstring BootTimeUtc() {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    uint64_t up = GetTickCount64() * 10000ull;   // ms -> 100 ns
    uint64_t boot = U64(now) - up;
    boot -= boot % 10000000ull;                   // to the second
    return TimeText(Ft(boot));
}

bool TakeMeasurement(const std::wstring& phase, const std::wstring& build, const std::wstring& machine, const std::wstring& plan,
                     Measurement* out, std::wstring* err) {
    Measurement m;
    m.phase = phase;
    m.build = build;
    m.machine = machine;
    m.plan = plan;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    m.created = TimeText(now);
    uint64_t ticks = GetTickCount64();
    m.uptimeMin = (int)(ticks / 60000ull);
    m.boot = BootTimeUtc();
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) { *err = L"GlobalMemoryStatusEx failed: " + sys::ErrorText(GetLastError()); return false; }
    m.ramTotal = ms.ullTotalPhys;
    m.ramUsed = ms.ullTotalPhys - ms.ullAvailPhys;
    m.commitLimit = ms.ullTotalPageFile;                  // the commit limit
    m.commit = ms.ullTotalPageFile - ms.ullAvailPageFile; // the commit charge
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { *err = L"Could not list processes: " + sys::ErrorText(GetLastError()); return false; }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    BOOL more = Process32FirstW(snap, &pe);
    if (!more) {
        DWORD e = GetLastError();
        CloseHandle(snap);
        *err = L"Could not list processes: " + sys::ErrorText(e);
        return false;
    }
    while (more) {
        m.processes++;
        if (IEq(pe.szExeFile, L"svchost.exe")) m.svchost++;
        more = Process32NextW(snap, &pe);
    }
    DWORD e = GetLastError();
    CloseHandle(snap);
    if (e != ERROR_NO_MORE_FILES) { *err = L"Process list ended early: " + sys::ErrorText(e); return false; }
    *out = m;
    return true;
}

std::string WriteMeasurement(const Measurement& m) {
    std::wstring o = L"HXSVC\t1\tmeasure\n";
    auto meta = [&](const wchar_t* k, const std::wstring& v) { o += std::wstring(L"M\t") + k + L"\t" + Esc(v) + L"\n"; };
    meta(L"boot", m.boot);
    meta(L"build", m.build);
    meta(L"created", m.created);
    meta(L"machine", m.machine);
    meta(L"phase", m.phase);
    meta(L"plan", m.plan);
    meta(L"uptime", std::to_wstring(m.uptimeMin));
    const uint64_t vals[] = {m.ramUsed, m.ramTotal, m.commit, m.commitLimit, (uint64_t)m.processes, (uint64_t)m.svchost};
    for (size_t i = 0; i < 6; i++) o += std::wstring(L"V\t") + kValues[i] + L"\t" + std::to_wstring(vals[i]) + L"\n";
    return ToUtf8(o);
}

bool ParseMeasurement(const std::string& utf8, Measurement* out, std::wstring* err) {
    *out = Measurement{};
    if (utf8.size() > kMaxFile) { *err = L"measurement is larger than 16 MB"; return false; }
    auto lines = Split(FromUtf8(utf8), L'\n');
    for (auto& l : lines) if (!l.empty() && l.back() == L'\r') l.pop_back();
    if (lines.empty() || lines[0] != L"HXSVC\t1\tmeasure") { *err = L"not a Hadron measurement (header)"; return false; }
    bool have[6] = {};
    bool up = false;
    for (size_t n = 1; n < lines.size(); n++) {
        auto f = Split(lines[n], L'\t');
        if (f.size() < 3) continue;
        if (f[0] == L"M") {
            std::wstring v = Unesc(f[2]);
            if (f[1] == L"boot") out->boot = v;
            else if (f[1] == L"build") out->build = v;
            else if (f[1] == L"created") out->created = v;
            else if (f[1] == L"machine") out->machine = v;
            else if (f[1] == L"phase") out->phase = v;
            else if (f[1] == L"plan") out->plan = v;
            else if (f[1] == L"uptime" && Digits(v)) { out->uptimeMin = _wtoi(v.c_str()); up = true; }
        } else if (f[0] == L"V") {
            for (size_t i = 0; i < 6; i++) {
                if (f[1] != kValues[i]) continue;
                if (!Digits(f[2])) { *err = L"line " + std::to_wstring(n + 1) + L": " + f[1] + L" is not a number"; return false; }
                uint64_t v = wcstoull(f[2].c_str(), nullptr, 10);
                switch (i) {
                    case 0: out->ramUsed = v; break;
                    case 1: out->ramTotal = v; break;
                    case 2: out->commit = v; break;
                    case 3: out->commitLimit = v; break;
                    case 4: out->processes = (int)v; break;
                    case 5: out->svchost = (int)v; break;
                }
                have[i] = true;
            }
        }
    }
    for (size_t i = 0; i < 6; i++) if (!have[i]) { *err = std::wstring(L"the value ") + kValues[i] + L" is missing"; return false; }
    if (!up || out->created.size() != 19 || out->boot.size() != 19) { *err = L"the time, boot time or uptime is missing"; return false; }
    if (out->phase != L"before" && out->phase != L"after") { *err = L"phase '" + out->phase + L"' is neither before nor after"; return false; }
    return true;
}

bool SaveMeasurement(Store& st, Measurement* m, std::wstring* err) {
    if (!st.MakeDir(L"measure", err)) return false;
    // The file name from the record's own UTC time: "2026-11-12 08:31:05" -> measure-20261112-083105[-N].txt
    std::wstring stem;
    for (wchar_t c : m->created) if (iswdigit(c)) stem += c;
    if (stem.size() != 14) { *err = L"the measurement has no time"; return false; }
    stem = stem.substr(0, 8) + L"-" + stem.substr(8);
    for (int n = 0; n < 1000; n++) {
        std::wstring f = L"measure-" + (n == 0 ? stem : stem + L"-" + std::to_wstring(n)) + L".txt";
        if (st.Exists(L"measure\\" + f)) continue;
        if (!st.WriteNew(L"measure\\" + f, WriteMeasurement(*m), err)) return false;
        m->file = f;
        return true;
    }
    *err = L"no free file name in measure";
    return false;
}

std::vector<Measurement> LoadMeasurements(const Store& st, std::vector<std::wstring>* problems) {
    std::vector<Measurement> v;
    if (!st.Valid() || !st.Exists(L"measure")) return v;
    for (auto& f : st.List(L"measure\\measure-*.txt")) {
        std::string text;
        std::wstring err;
        Measurement m;
        if (!st.Read(L"measure\\" + f, &text, &err) || !ParseMeasurement(text, &m, &err)) {
            if (problems) problems->push_back(L"measurement measure\\" + f + L" skipped: " + err);
            continue;
        }
        m.file = f;
        v.push_back(std::move(m));
    }
    std::stable_sort(v.begin(), v.end(), [](const Measurement& a, const Measurement& b) { return a.created < b.created; });
    return v;
}

MeasurePair LatestPair(const std::vector<Measurement>& ms) {
    MeasurePair p;
    for (int a = (int)ms.size() - 1; a >= 0; a--) {
        if (ms[a].phase != L"after") continue;
        for (int b = (int)ms.size() - 1; b >= 0; b--) {
            // A before recorded before the boot the after was taken in, on the same PC.
            if (ms[b].phase == L"before" && ms[b].created < ms[a].boot && ms[b].machine == ms[a].machine) { p.before = b; p.after = a; return p; }
        }
    }
    return p;
}

int PendingBefore(const std::vector<Measurement>& ms, const std::wstring& bootNow) {
    int b = -1;
    for (int i = (int)ms.size() - 1; i >= 0; i--) if (ms[i].phase == L"before") { b = i; break; }
    if (b < 0 || !(ms[b].created < bootNow)) return -1;   // none, or no restart since it
    for (auto& m : ms) if (m.phase == L"after" && ms[b].created < m.boot) return -1;   // its after is taken
    return b;
}

std::wstring MiB(uint64_t bytes) {
    std::wstring d = std::to_wstring(bytes / (1024ull * 1024ull));
    for (int i = (int)d.size() - 3; i > 0; i -= 3) d.insert((size_t)i, L",");
    return d + L" MiB";
}

}  // namespace hx::services
