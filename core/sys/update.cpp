// core/sys/update.cpp — self-update from GitHub releases. See update.h and docs/ARCHITECTURE.md "Updates".
// Own JSON reader (hard rule 3); WinHTTP, BCrypt and the version API are Windows SDK.
#include "core/sys/update.h"
#include "core/sys/sys.h"
#include "core/sys/log.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

#ifndef HADRON_VERSION_MAJOR
#define HADRON_VERSION_MAJOR 0
#define HADRON_VERSION_MINOR 0
#define HADRON_VERSION_PATCH 0
#endif
#ifndef HADRON_DEV_BUILD
#define HADRON_DEV_BUILD 1
#endif
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace hx::sys::update {

namespace {

const wchar_t* kLatestUrl = L"https://api.github.com/repos/floration/Hadron/releases/latest";
const uint64_t kMaxJson = 4ull << 20;     // a release document is a few KB
const uint64_t kMaxAsset = 64ull << 20;   // our exes are a few MB

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

// --- a small JSON reader: enough for the GitHub release document, strict about structure ---------
struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* Get(const char* k) const {
        if (t != Obj) return nullptr;
        for (auto& [key, v] : o) if (key == k) return &v;
        return nullptr;
    }
};

struct JParser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    void Ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    bool Lit(const char* w) {
        size_t n = strlen(w);
        if (s.compare(i, n, w) != 0) return false;
        i += n; return true;
    }
    static void PutUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    bool Hex4(unsigned* v) {
        if (i + 4 > s.size()) return false;
        *v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++]; *v <<= 4;
            if (c >= '0' && c <= '9') *v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') *v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') *v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool Str(std::string* out) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { *out += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': *out += '"'; break;   case '\\': *out += '\\'; break;
                case '/': *out += '/'; break;   case 'b': *out += '\b'; break;
                case 'f': *out += '\f'; break;  case 'n': *out += '\n'; break;
                case 'r': *out += '\r'; break;  case 't': *out += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!Hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        size_t save = i; i += 2; unsigned lo;
                        if (Hex4(&lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else i = save;
                    }
                    PutUtf8(*out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool Val(JVal* v) {
        if (++depth > 64) return false;
        Ws();
        if (i >= s.size()) return false;
        bool ok = false;
        char c = s[i];
        if (c == '{') {
            v->t = JVal::Obj; i++; Ws();
            if (i < s.size() && s[i] == '}') { i++; ok = true; }
            else for (;;) {
                Ws(); std::string k;
                if (!Str(&k)) break;
                Ws(); if (i >= s.size() || s[i] != ':') break; i++;
                JVal x; if (!Val(&x)) break;
                v->o.emplace_back(std::move(k), std::move(x));
                Ws(); if (i >= s.size()) break;
                if (s[i] == ',') { i++; continue; }
                if (s[i] == '}') { i++; ok = true; }
                break;
            }
        } else if (c == '[') {
            v->t = JVal::Arr; i++; Ws();
            if (i < s.size() && s[i] == ']') { i++; ok = true; }
            else for (;;) {
                JVal x; if (!Val(&x)) break;
                v->a.push_back(std::move(x));
                Ws(); if (i >= s.size()) break;
                if (s[i] == ',') { i++; continue; }
                if (s[i] == ']') { i++; ok = true; }
                break;
            }
        } else if (c == '"') {
            v->t = JVal::Str; ok = Str(&v->s);
        } else if (Lit("true")) { v->t = JVal::Bool; v->b = true; ok = true; }
        else if (Lit("false")) { v->t = JVal::Bool; ok = true; }
        else if (Lit("null")) { v->t = JVal::Null; ok = true; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            size_t b = i;
            while (i < s.size() && (strchr("+-.eE", s[i]) || (s[i] >= '0' && s[i] <= '9'))) i++;
            v->t = JVal::Num; v->n = strtod(s.substr(b, i - b).c_str(), nullptr); ok = true;
        }
        depth--;
        return ok;
    }
};

bool ParseJson(const std::string& text, JVal* out) {
    JParser p{text};
    if (!p.Val(out)) return false;
    p.Ws();
    return p.i == text.size();
}

// --- hashing ---------------------------------------------------------------------------------------
bool Sha256Hex(const std::vector<uint8_t>& data, std::wstring* hex) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; UCHAR dig[32];
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (BCRYPT_SUCCESS(st)) st = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    if (BCRYPT_SUCCESS(st)) st = BCryptHashData(h, const_cast<PUCHAR>(data.data()), (ULONG)data.size(), 0);
    if (BCRYPT_SUCCESS(st)) st = BCryptFinishHash(h, dig, sizeof(dig), 0);
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(st)) return false;
    hex->clear();
    static const wchar_t* d = L"0123456789abcdef";
    for (UCHAR b : dig) { *hex += d[b >> 4]; *hex += d[b & 15]; }
    return true;
}

// --- HTTP ------------------------------------------------------------------------------------------
struct HInet {
    HINTERNET h = nullptr;
    ~HInet() { if (h) WinHttpCloseHandle(h); }
};

std::mutex g_netMx;
HINTERNET g_req = nullptr;   // the request in flight, if any (one updater worker at a time)

std::wstring HttpErr(const wchar_t* step, DWORD e) {
    std::wstring t;
    if (e >= 12000 && e <= 12200) {   // WinHTTP codes live in winhttp.dll's message table
        wchar_t* buf = nullptr;
        DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 GetModuleHandleW(L"winhttp.dll"), e, 0, (LPWSTR)&buf, 0, nullptr);
        if (n && buf) { t.assign(buf, n); while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ')) t.pop_back(); }
        if (buf) LocalFree(buf);
    }
    if (t.empty()) t = ErrorText(e);
    return std::wstring(step) + L": " + t + L" (" + std::to_wstring(e) + L")";
}

// One GET hop over HTTPS. Redirects are not followed here: the caller checks each Location itself.
// The host check runs on WinHttpCrackUrl's own parse, the same one WinHttpConnect uses.
bool HttpHop(const std::wstring& url, uint64_t maxBytes, uint64_t deadline, std::vector<uint8_t>* body, DWORD* status,
             std::wstring* location, std::wstring* err, const std::atomic<bool>* cancel) {
    *status = 0;
    location->clear();
    if (GetTickCount64() > deadline) { *err = L"download took too long; stopped"; return false; }
    // Null pointers with length -1: the parts come back as pointers into `url`, so no length limit (the
    // CDN redirect URLs carry long signed queries).
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUserNameLength = uc.dwPasswordLength = (DWORD)-1;
    uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) { *err = HttpErr(L"WinHttpCrackUrl", GetLastError()); return false; }
    std::wstring h = uc.lpszHostName ? std::wstring(uc.lpszHostName, uc.dwHostNameLength) : std::wstring();
    if (uc.nScheme != INTERNET_SCHEME_HTTPS || uc.nPort != INTERNET_DEFAULT_HTTPS_PORT || uc.dwUserNameLength ||
        uc.dwPasswordLength || !HostAllowed(L"https://" + h + L"/")) {
        *err = L"refused URL outside GitHub: " + url; return false;
    }
    std::wstring ua = L"Hadron-Updater/" + ToString(Current());
    HInet ses{WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!ses.h) ses.h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses.h) { *err = HttpErr(L"WinHttpOpen", GetLastError()); return false; }
    if (!WinHttpSetTimeouts(ses.h, 15000, 15000, 30000, 30000)) { *err = HttpErr(L"WinHttpSetTimeouts", GetLastError()); return false; }
    HInet con{WinHttpConnect(ses.h, h.c_str(), uc.nPort, 0)};
    if (!con.h) { *err = HttpErr(L"WinHttpConnect", GetLastError()); return false; }
    std::wstring obj = (uc.lpszUrlPath ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : std::wstring(L"/")) +
                       (uc.lpszExtraInfo ? std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength) : std::wstring());
    HInet req{WinHttpOpenRequest(con.h, L"GET", obj.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE)};
    if (!req.h) { *err = HttpErr(L"WinHttpOpenRequest", GetLastError()); return false; }
    // Let AbortNetwork() (the UI thread, on close or opt-out) close this request: WinHTTP then fails the
    // blocking call at once instead of waiting out its timeout. Known window: an abort landing between two
    // calls makes the next call use a closed handle, which WinHTTP in practice rejects with ERROR_INVALID_HANDLE; no new
    // handle is created on that path (the hop returns and the next one sees `cancel`), so it can't be reused.
    struct Reg {
        HInet& r;
        explicit Reg(HInet& x) : r(x) { std::lock_guard<std::mutex> g(g_netMx); g_req = r.h; }
        ~Reg() { std::lock_guard<std::mutex> g(g_netMx); if (g_req == r.h) g_req = nullptr; else r.h = nullptr; }
    } reg(req);
    if (cancel && cancel->load()) { *err = L"cancelled"; return false; }
    DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof(noRedirect))) {
        *err = HttpErr(L"WinHttpSetOption", GetLastError()); return false;
    }
    const wchar_t* hdr = L"Accept: application/vnd.github+json, application/octet-stream\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(req.h, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        *err = HttpErr(L"download", GetLastError()); return false;
    }
    DWORD sc = 0, scl = sizeof(sc);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &sc, &scl, WINHTTP_NO_HEADER_INDEX)) {
        *err = HttpErr(L"WinHttpQueryHeaders", GetLastError()); return false;
    }
    *status = sc;
    if (sc == 301 || sc == 302 || sc == 303 || sc == 307 || sc == 308) {
        DWORD len = 0;
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !len) { *err = L"redirect without a location"; return false; }
        std::wstring loc(len / sizeof(wchar_t) + 1, L'\0');
        if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc.data(), &len, WINHTTP_NO_HEADER_INDEX)) {
            *err = HttpErr(L"WinHttpQueryHeaders", GetLastError()); return false;
        }
        loc.resize(wcslen(loc.c_str()));
        *location = loc;
        return true;
    }
    body->clear();
    for (;;) {
        if (cancel && cancel->load()) { *err = L"cancelled"; return false; }
        if (GetTickCount64() > deadline) { *err = L"download took too long; stopped"; return false; }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) { *err = HttpErr(L"download", GetLastError()); return false; }
        if (avail == 0) break;
        if (body->size() + avail > maxBytes) { *err = L"download larger than expected; stopped"; return false; }
        size_t at = body->size();
        body->resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, body->data() + at, avail, &got)) { *err = HttpErr(L"download", GetLastError()); return false; }
        body->resize(at + got);
    }
    return true;
}

// GET from an allowed host, following up to 5 redirects; every hop is checked against the allowlist.
// The whole fetch has a 3-minute budget so a trickling server can't keep a closing window alive.
bool HttpGet(const std::wstring& url, uint64_t maxBytes, std::vector<uint8_t>* body, DWORD* status,
             std::wstring* err, const std::atomic<bool>* cancel) {
    uint64_t deadline = GetTickCount64() + 180000;
    std::wstring at = url, loc;
    for (int hop = 0; hop < 6; hop++) {
        if (cancel && cancel->load()) { *err = L"cancelled"; return false; }
        if (!HttpHop(at, maxBytes, deadline, body, status, &loc, err, cancel)) return false;
        if (loc.empty()) return true;
        if (loc.compare(0, 8, L"https://") != 0 && loc.compare(0, 8, L"HTTPS://") != 0) {
            *err = L"redirected to a non-https URL: " + loc; return false;
        }
        at = loc;
    }
    *err = L"too many redirects";
    return false;
}

// --- files -----------------------------------------------------------------------------------------
Version FileVersion(const std::wstring& path) {
    Version v;
    DWORD dummy = 0, n = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!n) return v;
    std::vector<uint8_t> buf(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data())) return v;
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi || len < sizeof(*fi)) return v;
    v.major = HIWORD(fi->dwFileVersionMS); v.minor = LOWORD(fi->dwFileVersionMS); v.patch = HIWORD(fi->dwFileVersionLS);
    v.ok = true;
    return v;
}

// a\\b without doubling the separator when `a` is a root such as \\?\\E:\\.
std::wstring Join(const std::wstring& a, const std::wstring& b) {
    return (!a.empty() && (a.back() == L'\\' || a.back() == L'/')) ? a + b : a + L"\\" + b;
}

std::wstring FinalPath(HANDLE h) {
    DWORD n = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!n) return {};
    std::wstring p(n, L'\0');
    n = GetFinalPathNameByHandleW(h, p.data(), n, FILE_NAME_NORMALIZED);
    p.resize(n);
    return p;
}

// Opens `path` (a \\?\ path) as a plain directory. Refuses a reparse point (rule 1: links are leaves), a
// non-directory, or one whose final path isn't `<parentFinal>\<leaf>` (an ancestor swapped for a link after
// the parent was checked). The handle is opened without FILE_SHARE_DELETE, so while the caller holds it the
// folder can't be renamed or replaced by a junction. parentFinal empty = don't check the location.
HANDLE OpenRealDir(const std::wstring& path, const std::wstring& parentFinal, const std::wstring& leaf, std::wstring* why) {
    // FILE_TRAVERSE makes the open share-checked (an attributes-only open isn't), so leaving out
    // FILE_SHARE_DELETE really does stop anyone renaming the folder while we hold it.
    HANDLE h = CreateFileW(path.c_str(), FILE_TRAVERSE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *why = ToShell(path) + L": " + ErrorText(GetLastError()); return nullptr; }
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi)) {
        *why = ToShell(path) + L": " + ErrorText(GetLastError()); CloseHandle(h); return nullptr;
    }
    if ((fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        *why = ToShell(path) + L" is a link or not a folder"; CloseHandle(h); return nullptr;
    }
    if (!parentFinal.empty()) {
        std::wstring fin = FinalPath(h), want = Join(parentFinal, leaf);
        if (fin.empty() || CompareStringOrdinal(fin.c_str(), (int)fin.size(), want.c_str(), (int)want.size(), TRUE) != CSTR_EQUAL) {
            *why = ToShell(path) + L" resolves to " + (fin.empty() ? std::wstring(L"(unknown)") : ToShell(fin));
            CloseHandle(h); return nullptr;
        }
    }
    return h;
}

std::wstring Stamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t b[64];
    swprintf(b, 64, L"%04u%02u%02u-%02u%02u%02u-%lu", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             GetCurrentProcessId());
    return b;
}

// Writes the verified bytes next to the exe, checks their VERSIONINFO, then swaps: running exe ->
// .hadron-old\<stamp>\, new file -> exe path. Every move is same-folder (same volume), never a copy.
bool Install(const std::vector<uint8_t>& bytes, const Version& want, const std::wstring& exe, std::wstring* err,
             const std::atomic<bool>* cancel) {
    size_t slash = exe.find_last_of(L"\\/");
    std::wstring dir = exe.substr(0, slash), name = exe.substr(slash + 1), stamp = Stamp();

    // Two instances of the same exe could both find the update: one installs, the other sees it done.
    HANDLE mx = CreateMutexW(nullptr, FALSE, (L"Local\\Hadron.update." + Lower(name)).c_str());
    if (!mx) { *err = L"can't create the update lock: " + ErrorText(GetLastError()); return false; }
    DWORD wr = WAIT_TIMEOUT;
    for (int i = 0; i < 120 && wr == WAIT_TIMEOUT && !(cancel && cancel->load()); i++) wr = WaitForSingleObject(mx, 500);
    if (wr != WAIT_OBJECT_0 && wr != WAIT_ABANDONED) {
        DWORD e = GetLastError();
        CloseHandle(mx);
        *err = wr == WAIT_FAILED ? L"update lock: " + ErrorText(e)
             : cancel && cancel->load() ? std::wstring(L"cancelled")
             : L"another Hadron window is installing an update; try again later";
        return false;
    }
    struct Unlock { HANDLE h; ~Unlock() { ReleaseMutex(h); CloseHandle(h); } } unlock{mx};
    Version onDisk = FileVersion(exe);
    if (onDisk.ok && Compare(onDisk, want) >= 0) return true;   // already installed by another instance

    std::wstring tmp = ToExtended(dir + L"\\" + name + L".update-" + stamp + L".tmp");
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { *err = L"can't create " + ToShell(tmp) + L": " + ErrorText(GetLastError()); return false; }
    DWORD wrote = 0;
    bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size() && FlushFileBuffers(f);
    DWORD we = GetLastError();
    CloseHandle(f);
    auto dropTmp = [&] {   // our own temp file, created above with CREATE_NEW
        if (!DeleteFileW(tmp.c_str()))
            Write(LogLevel::Warn, L"update: couldn't remove " + ToShell(tmp) + L": " + ErrorText(GetLastError()));
    };
    if (!ok) { *err = L"can't write the update: " + ErrorText(we); dropTmp(); return false; }

    Version got = FileVersion(tmp);
    if (!got.ok || Compare(got, want) != 0) {
        *err = L"the downloaded exe says it is version " + (got.ok ? ToString(got) : std::wstring(L"(none)")) +
               L", not " + ToString(want) + L"; not installed";
        dropTmp(); return false;
    }

    // The exe's folder, then .hadron-old and the stamp folder, each checked (and held open, so it can't be
    // swapped for a link) before anything is created inside it.
    // The folder comes from the full exe path (ToExtended("E:") would be E:'s current directory, not E:\\).
    std::wstring exeFull = ToExtended(exe);
    std::wstring dirX = exeFull.substr(0, exeFull.find_last_of(L"\\/") + 1);   // keeps the trailing separator
    if (dirX.size() > 4 && dirX.back() == L'\\' && dirX[dirX.size() - 2] != L':') dirX.pop_back();   // not a root
    std::wstring oldRoot = Join(dirX, L".hadron-old"), oldDir = Join(oldRoot, stamp);
    std::wstring why;
    HANDLE hDir = OpenRealDir(dirX, L"", L"", &why);
    if (!hDir) { *err = L"refused to keep the old exe: " + why; dropTmp(); return false; }
    std::wstring dirFinal = FinalPath(hDir);
    struct Holder { std::vector<HANDLE> h; ~Holder() { for (HANDLE x : h) CloseHandle(x); } } held{{hDir}};
    if (dirFinal.empty()) { *err = L"can't resolve " + dir + L": " + ErrorText(GetLastError()); dropTmp(); return false; }
    if (!CreateDirectoryW(oldRoot.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        *err = L"can't create " + ToShell(oldRoot) + L": " + ErrorText(GetLastError()); dropTmp(); return false;
    }
    HANDLE hRoot = OpenRealDir(oldRoot, dirFinal, L".hadron-old", &why);
    if (!hRoot) { *err = L"refused to keep the old exe: " + why; dropTmp(); return false; }
    held.h.push_back(hRoot);
    std::wstring rootFinal = FinalPath(hRoot);
    if (!CreateDirectoryW(oldDir.c_str(), nullptr)) {
        *err = L"can't create " + ToShell(oldDir) + L": " + ErrorText(GetLastError()); dropTmp(); return false;
    }
    HANDLE hStamp = OpenRealDir(oldDir, rootFinal, stamp, &why);
    if (!hStamp) { *err = L"refused to keep the old exe: " + why; dropTmp(); return false; }
    held.h.push_back(hStamp);
    std::wstring exeX = ToExtended(exe), kept = oldDir + L"\\" + name;
    // A running exe can be renamed but not overwritten. No MOVEFILE_COPY_ALLOWED: same folder, same volume.
    if (!MoveFileExW(exeX.c_str(), kept.c_str(), MOVEFILE_WRITE_THROUGH)) {
        *err = L"can't move the current exe aside: " + ErrorText(GetLastError()); dropTmp(); return false;
    }
    if (!MoveFileExW(tmp.c_str(), exeX.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DWORD e = GetLastError();
        if (!MoveFileExW(kept.c_str(), exeX.c_str(), MOVEFILE_WRITE_THROUGH)) {
            *err = L"can't put the new exe in place (" + ErrorText(e) + L") and can't move the old one back (" +
                   ErrorText(GetLastError()) + L"). Your previous exe is at " + ToShell(kept);
        } else {
            *err = L"can't put the new exe in place: " + ErrorText(e) + L" (old exe restored)";
        }
        dropTmp();
        return false;
    }
    Write(LogLevel::Info, L"update: installed " + ToString(want) + L"; previous exe kept at " + ToShell(kept));
    return true;
}

}  // namespace

// --- public ---------------------------------------------------------------------------------------
bool PinnedUrl(const std::wstring& url, const std::wstring& tag, const std::wstring& file) {
    std::wstring want = Lower(L"https://github.com/floration/hadron/releases/download/" + tag + L"/" + file);
    return Lower(url) == want;   // exactly that file: no dot segments, escapes, queries or fragments
}

void AbortNetwork() {
    std::lock_guard<std::mutex> g(g_netMx);
    if (g_req) { WinHttpCloseHandle(g_req); g_req = nullptr; }
}
Version ParseVersion(const std::wstring& in) {
    Version v;
    std::wstring s = in;
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s = s.substr(1);
    int parts[3] = {0, 0, 0}, k = 0;
    size_t i = 0;
    while (k < 3) {
        size_t b = i;
        long n = 0;
        while (i < s.size() && iswdigit(s[i]) && i - b < 6) n = n * 10 + (s[i++] - L'0');
        if (i == b || n > 65535) return v;   // VERSIONINFO fields are 16-bit
        parts[k++] = (int)n;
        if (k < 3) { if (i >= s.size() || s[i] != L'.') return v; i++; }
    }
    if (i != s.size()) return v;   // no pre-release or build suffixes
    v.major = parts[0]; v.minor = parts[1]; v.patch = parts[2]; v.ok = true;
    return v;
}

int Compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::wstring ToString(const Version& v) {
    return std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." + std::to_wstring(v.patch);
}

Version Current() {
    Version v; v.major = HADRON_VERSION_MAJOR; v.minor = HADRON_VERSION_MINOR; v.patch = HADRON_VERSION_PATCH; v.ok = true;
    return v;
}

bool IsDevBuild() { return HADRON_DEV_BUILD != 0; }

bool HostAllowed(const std::wstring& url) {
    std::wstring u = Lower(url);
    const std::wstring https = L"https://";
    if (u.compare(0, https.size(), https) != 0) return false;
    size_t end = u.find_first_of(L"/?#", https.size());
    std::wstring host = u.substr(https.size(), end == std::wstring::npos ? std::wstring::npos : end - https.size());
    if (host.find_first_of(L"@:") != std::wstring::npos) return false;   // no userinfo, no explicit port
    if (host == L"github.com" || host == L"api.github.com") return true;
    const std::wstring suf = L".githubusercontent.com";
    return host.size() > suf.size() && host.compare(host.size() - suf.size(), suf.size(), suf) == 0;
}

bool ParseRelease(const std::string& json, const std::wstring& exeName, Release* out, std::wstring* err) {
    JVal root;
    if (!ParseJson(json, &root) || root.t != JVal::Obj) { *err = L"the release document isn't valid JSON"; return false; }
    const JVal* tag = root.Get("tag_name");
    if (!tag || tag->t != JVal::Str) { *err = L"the release has no tag"; return false; }
    const JVal* draft = root.Get("draft");
    const JVal* pre = root.Get("prerelease");
    if ((draft && draft->t == JVal::Bool && draft->b) || (pre && pre->t == JVal::Bool && pre->b)) {
        *err = L"the latest release is a draft or pre-release"; return false;
    }
    *out = Release{};
    out->tag = Widen(tag->s);
    out->version = ParseVersion(out->tag);
    if (!out->version.ok) { *err = L"release tag \"" + out->tag + L"\" isn't vMAJOR.MINOR.PATCH"; return false; }
    const JVal* assets = root.Get("assets");
    if (assets && assets->t == JVal::Arr) {
        std::wstring want = Lower(exeName);
        for (auto& a : assets->a) {
            const JVal* nm = a.Get("name");
            const JVal* url = a.Get("browser_download_url");
            if (!nm || nm->t != JVal::Str || !url || url->t != JVal::Str) continue;
            std::wstring n = Lower(Widen(nm->s));
            if (n == want) {
                out->assetUrl = Widen(url->s);
                const JVal* sz = a.Get("size");
                if (sz && sz->t == JVal::Num && sz->n > 0) out->assetSize = sz->n < 1e15 ? (uint64_t)sz->n : UINT64_MAX;
            } else if (n == L"sha256sums") {
                out->sumsUrl = Widen(url->s);
            }
        }
    }
    return true;
}

bool FindSum(const std::string& sums, const std::wstring& name, std::wstring* hex) {
    std::wstring text = Widen(sums), want = Lower(name);
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = text.substr(i, e - i);
        i = e + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (line.size() < 66) continue;
        std::wstring h = Lower(line.substr(0, 64));
        if (h.find_first_not_of(L"0123456789abcdef") != std::wstring::npos || line[64] != L' ') continue;
        std::wstring n = line.substr(65);
        if (!n.empty() && (n[0] == L'*' || n[0] == L' ')) n = n.substr(1);
        if (Lower(n) == want) { *hex = h; return true; }
    }
    return false;
}

Result CheckAndInstall(bool install, const std::atomic<bool>* cancel) {
    Result r;
    std::wstring exe = ModulePath();
    std::wstring name = exe.substr(exe.find_last_of(L"\\/") + 1);
    std::vector<uint8_t> body;
    DWORD sc = 0;
    std::wstring err;
    if (!HttpGet(kLatestUrl, kMaxJson, &body, &sc, &err, cancel)) {
        r.outcome = cancel && cancel->load() ? Outcome::Cancelled : Outcome::Failed;
        r.message = L"Update check failed: " + err;
        return r;
    }
    if (sc == 404) { r.outcome = Outcome::NoRelease; r.message = L"Updates: no published release found (HTTP 404)"; return r; }
    if (sc != 200) {
        // GitHub explains refusals (rate limit, blocked) in a JSON "message"; show it rather than guess.
        std::wstring why;
        JVal doc;
        if (ParseJson(std::string(body.begin(), body.end()), &doc))
            if (const JVal* m = doc.Get("message"); m && m->t == JVal::Str) why = Widen(m->s.substr(0, 160));
        if (why.empty() && !body.empty()) why = Widen(std::string(body.begin(), body.begin() + std::min<size_t>(body.size(), 120)));
        r.message = L"Update check failed: HTTP " + std::to_wstring(sc) + (why.empty() ? L"" : L": " + why);
        return r;
    }
    Release rel;
    if (!ParseRelease(std::string(body.begin(), body.end()), name, &rel, &err)) { r.message = L"Update check failed: " + err; return r; }
    r.latest = rel.version;
    if (Compare(rel.version, Current()) <= 0 && !IsDevBuild()) {
        r.outcome = Outcome::UpToDate;
        r.message = L"Hadron " + ToString(Current()) + L" is up to date";
        return r;
    }
    if (!install || IsDevBuild()) {
        r.outcome = Outcome::Available;
        r.message = IsDevBuild() ? L"Dev build: latest release is " + ToString(rel.version) + L" (dev builds don't update themselves)"
                                 : L"Hadron " + ToString(rel.version) + L" is available";
        return r;
    }
    if (rel.assetUrl.empty()) { r.message = L"Update " + ToString(rel.version) + L" has no " + name + L"; not installed"; return r; }
    if (rel.sumsUrl.empty()) { r.message = L"Update " + ToString(rel.version) + L" has no SHA256SUMS; not installed"; return r; }
    {   // Both files must come from this repo's release for this tag.
        if (!PinnedUrl(rel.assetUrl, rel.tag, name) || !PinnedUrl(rel.sumsUrl, rel.tag, L"SHA256SUMS")) {
            r.message = L"Update " + ToString(rel.version) + L": download links aren't this repo's release files; not installed";
            return r;
        }
    }
    if (rel.assetSize > kMaxAsset) { r.message = L"Update " + ToString(rel.version) + L" is unexpectedly large; not installed"; return r; }

    std::vector<uint8_t> sums;
    if (!HttpGet(rel.sumsUrl, 1 << 20, &sums, &sc, &err, cancel) || sc != 200) {
        r.outcome = cancel && cancel->load() ? Outcome::Cancelled : Outcome::Failed;
        r.message = L"Update download failed: " + (err.empty() ? L"HTTP " + std::to_wstring(sc) : err);
        return r;
    }
    std::wstring wantHex;
    if (!FindSum(std::string(sums.begin(), sums.end()), name, &wantHex)) {
        r.message = L"Update " + ToString(rel.version) + L": SHA256SUMS has no line for " + name + L"; not installed";
        return r;
    }
    if (!HttpGet(rel.assetUrl, kMaxAsset, &body, &sc, &err, cancel) || sc != 200) {
        r.outcome = cancel && cancel->load() ? Outcome::Cancelled : Outcome::Failed;
        r.message = L"Update download failed: " + (err.empty() ? L"HTTP " + std::to_wstring(sc) : err);
        return r;
    }
    std::wstring gotHex;
    if (!Sha256Hex(body, &gotHex)) { r.message = L"Update failed: SHA-256 unavailable"; return r; }
    if (gotHex != wantHex) { r.message = L"Update " + ToString(rel.version) + L" failed its SHA-256 check; not installed"; return r; }
    if (body.size() < 2 || body[0] != 'M' || body[1] != 'Z') { r.message = L"Update isn't a Windows exe; not installed"; return r; }
    if (cancel && cancel->load()) { r.outcome = Outcome::Cancelled; r.message = L"Update cancelled"; return r; }

    if (!Install(body, rel.version, exe, &err, cancel)) {
        // Only Install's own lock-wait cancel counts as cancelled; any other failure keeps its text (it may
        // be the one message that says where the previous exe is).
        r.outcome = err == L"cancelled" ? Outcome::Cancelled : Outcome::Failed;
        r.message = r.outcome == Outcome::Cancelled ? std::wstring(L"Update cancelled") : L"Update failed: " + err;
        return r;
    }
    r.outcome = Outcome::Installed;
    r.message = L"Hadron " + ToString(rel.version) + L" installed. Restart to use it";
    return r;
}

int SelfTest() {
    int fails = 0, checks = 0;
    auto check = [&](bool c, const wchar_t* what) { checks++; if (!c) { fails++; wprintf(L"FAIL: %ls\n", what); } };

    check(ParseVersion(L"v1.2.3").ok && ParseVersion(L"1.2.3").patch == 3, L"parse v1.2.3");
    check(!ParseVersion(L"v1.2").ok && !ParseVersion(L"v1.2.3-rc1").ok && !ParseVersion(L"x1.2.3").ok &&
          !ParseVersion(L"").ok && !ParseVersion(L"1..3").ok, L"reject malformed versions");
    check(Compare(ParseVersion(L"0.10.0"), ParseVersion(L"0.9.9")) > 0, L"numeric, not lexical, compare");
    check(Compare(ParseVersion(L"1.0.0"), ParseVersion(L"v1.0.0")) == 0, L"equal versions");
    check(ParseVersion(L"1.2.65535").ok && !ParseVersion(L"1.2.65536").ok, L"VERSIONINFO 16-bit limit");
    const wchar_t* base = L"https://github.com/floration/Hadron/releases/download/v1.2.3/";
    check(PinnedUrl(std::wstring(base) + L"Hadron.exe", L"v1.2.3", L"hadron.exe"), L"pinned URL accepted");
    check(!PinnedUrl(std::wstring(base) + L"../../../evil/Hadron.exe", L"v1.2.3", L"Hadron.exe") &&
          !PinnedUrl(std::wstring(base) + L"Hadron.exe?x=1", L"v1.2.3", L"Hadron.exe") &&
          !PinnedUrl(std::wstring(base) + L"%2e%2e/Hadron.exe", L"v1.2.3", L"Hadron.exe") &&
          !PinnedUrl(L"https://github.com/floration/Hadron/releases/download/v1.2.4/Hadron.exe", L"v1.2.3", L"Hadron.exe"),
          L"pinned URL refuses other paths, queries, escapes and tags");

    check(HostAllowed(L"https://api.github.com/repos/x"), L"api.github.com allowed");
    check(HostAllowed(L"https://objects.githubusercontent.com/a?b"), L"githubusercontent allowed");
    check(!HostAllowed(L"http://github.com/x"), L"plain http refused");
    check(!HostAllowed(L"https://github.com.evil.example/x"), L"lookalike host refused");
    check(!HostAllowed(L"https://evilgithubusercontent.com/x"), L"suffix without dot refused");
    check(!HostAllowed(L"https://github.com@evil.example/x"), L"userinfo refused");
    check(!HostAllowed(L"https://github.com:8443/x"), L"explicit port refused");

    std::string doc = R"({"url":"x","tag_name":"v0.3.1","draft":false,"prerelease":false,"name":"Hadron \"0.3.1\" é",
      "assets":[{"name":"HadronBios.exe","size":1234,"uploader":{"name":"z","login":"q"},
                 "browser_download_url":"https://github.com/floration/Hadron/releases/download/v0.3.1/HadronBios.exe"},
                {"name":"SHA256SUMS","size":10,"browser_download_url":"https://github.com/floration/Hadron/releases/download/v0.3.1/SHA256SUMS"}],
      "body":"line\nline [x] {y}","n":[1,-2.5e3,true,null]})";
    Release rel; std::wstring err;
    check(ParseRelease(doc, L"hadronbios.EXE", &rel, &err), L"parse release document");
    check(rel.version.ok && rel.version.minor == 3 && rel.version.patch == 1, L"release version");
    check(rel.assetUrl.find(L"/HadronBios.exe") != std::wstring::npos && rel.assetSize == 1234, L"asset matched by exe name");
    check(rel.sumsUrl.find(L"SHA256SUMS") != std::wstring::npos, L"SHA256SUMS found");
    Release none;
    check(ParseRelease(doc, L"Hadron.exe", &none, &err) && none.assetUrl.empty(), L"no asset for another exe");
    check(!ParseRelease(R"({"tag_name":"v1.0.0","prerelease":true})", L"a", &rel, &err), L"pre-release refused");
    check(!ParseRelease(R"({"tag_name":"v1.0.0",})", L"a", &rel, &err), L"trailing comma refused");
    check(!ParseRelease(R"({"tag_name":"v1.0.0"} x)", L"a", &rel, &err), L"trailing junk refused");
    std::string deep(200, '['); deep += std::string(200, ']');
    JVal dv; check(!ParseJson(deep, &dv), L"nesting limit");

    std::string sums =
        "0000000000000000000000000000000000000000000000000000000000000000  Hadron.exe\r\n"
        "ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789 *HadronBios.exe\n";
    std::wstring hex;
    check(FindSum(sums, L"hadronbios.exe", &hex) && hex == L"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
          L"sha256sum line, binary marker, case");
    check(!FindSum(sums, L"HadronRunAs.exe", &hex), L"missing name");
    std::wstring h2;
    check(Sha256Hex(std::vector<uint8_t>{'a', 'b', 'c'}, &h2) &&
          h2 == L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", L"SHA-256 of abc");

    // Install() end to end in a temp folder: a stand-in "exe" is swapped for this exe's bytes, the old one is kept.
    {
        wchar_t tbuf[MAX_PATH]; GetTempPathW(MAX_PATH, tbuf);
        std::wstring dir = std::wstring(tbuf) + L"hadron-upd-test-" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring target = dir + L"\\Fake.exe";
        auto writeAll = [](const std::wstring& p, const std::string& d) {
            HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;
            DWORD w = 0; bool ok = WriteFile(h, d.data(), (DWORD)d.size(), &w, nullptr) && w == d.size();
            CloseHandle(h); return ok;
        };
        auto readAll = [](const std::wstring& p) {
            std::vector<uint8_t> v; FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return v;
            uint8_t b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
            fclose(f); return v;
        };
        std::vector<uint8_t> self = readAll(ModulePath());
        check(!self.empty() && writeAll(target, "old exe"), L"install fixture");
        Version next = Current(); next.patch++;
        std::wstring ierr;
        check(!Install(self, next, target, &ierr, nullptr) && ierr.find(L"says it is version") != std::wstring::npos,
              L"install refuses an exe whose VERSIONINFO doesn't match the release");
        check(readAll(target) == std::vector<uint8_t>{'o','l','d',' ','e','x','e'}, L"refused install leaves the exe alone");
        check(Install(self, Current(), target, &ierr, nullptr), L"install swaps in the new exe");
        check(readAll(target) == self, L"new exe in place");
        // The previous exe is under .hadron-old\<stamp>\Fake.exe; leftover temp files: none.
        int kept = 0, tmps = 0;
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW((dir + L"\\.hadron-old\\*").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == L'.') continue;
                std::wstring k = dir + L"\\.hadron-old\\" + fd.cFileName + L"\\Fake.exe";
                auto v = readAll(k);
                if (v == std::vector<uint8_t>{'o','l','d',' ','e','x','e'}) kept++;
                DeleteFileW(k.c_str());
                RemoveDirectoryW((dir + L"\\.hadron-old\\" + fd.cFileName).c_str());
            } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
        fh = FindFirstFileW((dir + L"\\*.tmp").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) { do tmps++; while (FindNextFileW(fh, &fd)); FindClose(fh); }
        check(kept == 1 && tmps == 0, L"previous exe kept once, no temp files left");
        // Test fixture cleanup (our own temp folder).
        DeleteFileW(target.c_str());
        RemoveDirectoryW((dir + L"\\.hadron-old").c_str());
        RemoveDirectoryW(dir.c_str());
    }

    wprintf(L"update selftest: %ls (%d checks, %d failed)\n", fails ? L"FAIL" : L"PASS", checks, fails);
    return fails ? 1 : 0;
}

}  // namespace hx::sys::update
