// modules/services/evidence.cpp — the Signer column and the event-7040 lookup (evidence.h). Read-only throughout:
// nothing here writes a file, a registry value or an event log.
#include "modules/services/evidence.h"
#include "modules/services/backend.h"
#include "modules/services/store.h"
#include "core/sys/sys.h"
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <mscat.h>
#include <sddl.h>
#include <winevt.h>
#include <algorithm>
#include <cwctype>

namespace hx::services {

// ---- labels and the fixture format ---------------------------------------------------------------------------
std::wstring SignerShort(const SignerInfo& s) {
    switch (s.state) {
        case SignState::Pending: return L"";
        case SignState::Signed: return L"Signed: " + (s.subject.empty() ? std::wstring(L"(no name)") : s.subject);
        case SignState::Catalog: return L"Catalog-signed";
        case SignState::Unsigned: return L"Unsigned";
        case SignState::Untrusted: return L"Not trusted: " + s.detail;
        case SignState::CantCheck: return L"Can't check: " + s.detail;
    }
    return L"";
}

std::wstring SignerLong(const SignerInfo& s) {
    const std::wstring how = L"Authenticode, checked offline (no revocation check, no download)";
    switch (s.state) {
        case SignState::Pending: return L"";
        case SignState::Signed: return L"Signed: " + (s.subject.empty() ? std::wstring(L"(no name)") : s.subject) + L". Embedded signature; " + how;
        case SignState::Catalog:
            return L"Catalog-signed" + (s.subject.empty() ? std::wstring() : L" by " + s.subject) +
                   (s.detail.empty() ? std::wstring() : L" (" + s.detail + L")") + L": the file's hash is in a Windows catalog that verifies; " + how;
        case SignState::Unsigned: return L"Unsigned: no embedded signature, and its hash is in no Windows catalog; " + how;
        case SignState::Untrusted: return L"Not trusted: " + s.detail + L". A signature is there but does not verify; " + how;
        case SignState::CantCheck: return L"Can't check: " + s.detail;
    }
    return L"";
}

const wchar_t* SignStateName(SignState s) {
    switch (s) {
        case SignState::Pending: return L"pending";
        case SignState::Signed: return L"signed";
        case SignState::Catalog: return L"catalog";
        case SignState::Unsigned: return L"unsigned";
        case SignState::Untrusted: return L"untrusted";
        case SignState::CantCheck: return L"cantcheck";
    }
    return L"pending";
}

bool SignStateFromName(const std::wstring& n, SignState* out) {
    for (SignState s : {SignState::Pending, SignState::Signed, SignState::Catalog, SignState::Unsigned, SignState::Untrusted,
                        SignState::CantCheck})
        if (n == SignStateName(s)) { *out = s; return true; }
    return false;
}

std::wstring SignerAttrsProblem(DWORD attrs) {
    if (attrs == INVALID_FILE_ATTRIBUTES) return L"the file was not found";
    // Hard rule 1: reading the data of a placeholder would download it.
    if (attrs & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE))
        return L"a cloud or offline file: not read (reading it would download it)";
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return L"a reparse point: not followed";
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return L"a folder, not a file";
    return L"";
}

bool ParseSigners(const std::string& utf8, std::map<std::wstring, SignerInfo>* out, std::wstring* err) {
    out->clear();
    if (utf8.size() > kMaxFile) { *err = L"signers file is larger than 16 MB"; return false; }
    std::vector<std::wstring> lines = Split(FromUtf8(utf8), L'\n');
    if (lines.empty() || lines[0].rfind(L"HXSVC\t1\tsigners", 0) != 0) { *err = L"missing HXSVC 1 signers header"; return false; }
    for (size_t i = 1; i < lines.size(); i++) {
        std::wstring l = lines[i];
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        auto f = Split(l, L'\t');
        if (f.size() < 3 || f[0] != L"G") continue;   // unknown lines are skipped, as in every Hadron format
        SignerInfo s;
        if (!SignStateFromName(f[2], &s.state)) continue;
        if (f.size() >= 4) s.subject = Unesc(f[3]);
        if (f.size() >= 5) s.detail = Unesc(f[4]);
        (*out)[Lower(Unesc(f[1]))] = s;
    }
    return true;
}

// ---- live signer ------------------------------------------------------------------------------------------------
namespace {

std::wstring RegSz(const std::wstring& path, const wchar_t* value) {
    DWORD cb = 0;
    const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), value, flags, nullptr, nullptr, &cb) != ERROR_SUCCESS || cb == 0) return {};
    std::vector<wchar_t> b(cb / sizeof(wchar_t) + 2, 0);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), value, flags, nullptr, b.data(), &cb) != ERROR_SUCCESS) return {};
    return b.data();
}

// Attributes as directory enumeration reports them (WIN32_FIND_DATA, the source rule 1 names); never opens the file.
bool FindData(const std::wstring& p, WIN32_FIND_DATAW* fd) {
    if (p.find_first_of(L"*?") != std::wstring::npos) return false;
    HANDLE h = FindFirstFileExW(sys::ToExtended(p).c_str(), FindExInfoBasic, fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

// "" when the path may be read for its signature (the Company lookup's rules: a local drive, not a network drive, no
// reparse point on the way), else why not.
std::wstring PathProblem(const std::wstring& path, WIN32_FIND_DATAW* fd) {
    if (!CompanyPathAllowed(path)) return L"not a local drive path (network and device paths are never read)";
    std::wstring p = path.rfind(L"\\\\?\\", 0) == 0 ? path.substr(4) : path;
    if (GetDriveTypeW(p.substr(0, 3).c_str()) == DRIVE_REMOTE) return L"on a network drive (never read)";
    for (size_t i = p.find(L'\\', 3); i != std::wstring::npos; i = p.find(L'\\', i + 1)) {
        WIN32_FIND_DATAW d{};
        if (!FindData(p.substr(0, i), &d)) return L"a folder on its path could not be read";
        if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return L"a folder on its path is a reparse point (not followed)";
    }
    if (!FindData(p, fd)) return L"the file was not found";
    return SignerAttrsProblem(fd->dwFileAttributes);
}

std::wstring CertCn(PCCERT_CONTEXT c) {
    if (!c) return {};
    wchar_t b[512] = {};
    DWORD n = CertGetNameStringW(c, CERT_NAME_ATTR_TYPE, 0, const_cast<char*>(szOID_COMMON_NAME), b, 512);
    if (n <= 1) n = CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, b, 512);
    return n > 1 ? std::wstring(b) : std::wstring();
}

// One WinVerifyTrust call (file or catalog member), always closed. *cn gets the leaf signer's CN when there is one.
LONG Verify(WINTRUST_DATA* wd, std::wstring* cn) {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    wd->cbStruct = sizeof(*wd);
    wd->dwUIChoice = WTD_UI_NONE;
    wd->fdwRevocationChecks = WTD_REVOKE_NONE;
    wd->dwStateAction = WTD_STATEACTION_VERIFY;
    wd->dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    LONG st = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, wd);
    if (wd->hWVTStateData) {
        if (CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd->hWVTStateData))
            if (CRYPT_PROVIDER_SGNR* sg = WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0))
                if (CRYPT_PROVIDER_CERT* pc = WTHelperGetProvCertFromChain(sg, 0)) *cn = CertCn(pc->pCert);
    }
    wd->dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, wd);
    return st;
}

bool NoSignature(LONG st) {
    return st == (LONG)TRUST_E_NOSIGNATURE || st == (LONG)TRUST_E_SUBJECT_FORM_UNKNOWN || st == (LONG)TRUST_E_PROVIDER_UNKNOWN;
}

// A signature that is there and fails (as opposed to one Hadron could not evaluate offline).
bool BadSignature(LONG st) {
    return st == (LONG)TRUST_E_BAD_DIGEST || st == (LONG)TRUST_E_CERT_SIGNATURE || st == (LONG)TRUST_E_EXPLICIT_DISTRUST ||
           st == (LONG)CERT_E_REVOKED || st == (LONG)CERT_E_UNTRUSTEDROOT || st == (LONG)CERT_E_UNTRUSTEDTESTROOT ||
           st == (LONG)CERT_E_EXPIRED || st == (LONG)CERT_E_WRONG_USAGE || st == (LONG)TRUST_E_SYSTEM_ERROR;
}

std::wstring HrText(LONG st) {
    std::wstring t = sys::ErrorText((DWORD)st);
    while (!t.empty() && (t.back() == L'.' || t.back() == L' ' || t.back() == L'\n' || t.back() == L'\r')) t.pop_back();
    wchar_t b[16];
    swprintf(b, 16, L"0x%08lX", (unsigned long)st);
    return t.empty() ? std::wstring(b) : t + L" (" + b + L")";
}

// The catalog check: SHA-256 first (current Windows catalogs), then SHA-1 (older ones). Returns true and fills *out
// when the hash is in a catalog whose signature verifies; *why gets the reason otherwise.
// The SHA-256 ("2") entry points are Windows 8+ exports of wintrust.dll that MinGW's import library lacks: they are
// looked up in the already-loaded system DLL (hard rule 3 allows system DLLs only; nothing is loaded from elsewhere).
using AcquireCtx2Fn = BOOL(WINAPI*)(HCATADMIN*, const GUID*, PCWSTR, PCCERT_STRONG_SIGN_PARA, DWORD);
using CalcHash2Fn = BOOL(WINAPI*)(HCATADMIN, HANDLE, DWORD*, BYTE*, DWORD);
struct CatV2 { AcquireCtx2Fn acquire = nullptr; CalcHash2Fn hash = nullptr; };
const CatV2& CatalogV2() {
    static const CatV2 v = []() {
        CatV2 r;
        if (HMODULE m = GetModuleHandleW(L"wintrust.dll")) {
            r.acquire = reinterpret_cast<AcquireCtx2Fn>(reinterpret_cast<void*>(GetProcAddress(m, "CryptCATAdminAcquireContext2")));
            r.hash = reinterpret_cast<CalcHash2Fn>(reinterpret_cast<void*>(GetProcAddress(m, "CryptCATAdminCalcHashFromFileHandle2")));
        }
        if (!r.acquire || !r.hash) r = CatV2{};
        return r;
    }();
    return v;
}

bool CatalogSigned(HANDLE file, const std::wstring& path, SignerInfo* out, std::wstring* why) {
    const CatV2& v2 = CatalogV2();
    for (int pass = v2.acquire ? 0 : 1; pass < 2; pass++) {
        HCATADMIN ca = nullptr;
        BOOL ok = pass == 0 ? v2.acquire(&ca, nullptr, BCRYPT_SHA256_ALGORITHM, nullptr, 0)
                            : CryptCATAdminAcquireContext(&ca, nullptr, 0);
        if (!ok) { *why = L"CryptCATAdminAcquireContext: " + sys::ErrorText(GetLastError()); continue; }
        DWORD cb = 0;
        std::vector<BYTE> hash;
        BOOL hashed = FALSE;
        if (SetFilePointer(file, 0, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
            if (pass == 0) {
                v2.hash(ca, file, &cb, nullptr, 0);
                if (cb) { hash.assign(cb, 0); SetFilePointer(file, 0, nullptr, FILE_BEGIN); hashed = v2.hash(ca, file, &cb, hash.data(), 0); }
            } else {
                CryptCATAdminCalcHashFromFileHandle(file, &cb, nullptr, 0);
                if (cb) { hash.assign(cb, 0); SetFilePointer(file, 0, nullptr, FILE_BEGIN); hashed = CryptCATAdminCalcHashFromFileHandle(file, &cb, hash.data(), 0); }
            }
        }
        if (!hashed) {
            *why = L"the file's hash could not be computed: " + sys::ErrorText(GetLastError());
            CryptCATAdminReleaseContext(ca, 0);
            continue;
        }
        HCATINFO ci = CryptCATAdminEnumCatalogFromHash(ca, hash.data(), cb, 0, nullptr);
        if (!ci) {
            CryptCATAdminReleaseContext(ca, 0);
            continue;   // not in a catalog with this hash
        }
        CATALOG_INFO info{};
        info.cbStruct = sizeof(info);
        bool found = false;
        if (CryptCATCatalogInfoFromContext(ci, &info, 0)) {
            std::wstring tag;
            for (DWORD k = 0; k < cb; k++) { wchar_t h[4]; swprintf(h, 4, L"%02X", hash[k]); tag += h; }
            WINTRUST_CATALOG_INFO wc{};
            wc.cbStruct = sizeof(wc);
            wc.pcwszCatalogFilePath = info.wszCatalogFile;
            wc.pcwszMemberTag = tag.c_str();
            wc.pcwszMemberFilePath = path.c_str();
            wc.hMemberFile = file;
            wc.pbCalculatedFileHash = hash.data();
            wc.cbCalculatedFileHash = cb;
            wc.hCatAdmin = ca;
            WINTRUST_DATA wd{};
            wd.dwUnionChoice = WTD_CHOICE_CATALOG;
            wd.pCatalog = &wc;
            std::wstring cn;
            SetFilePointer(file, 0, nullptr, FILE_BEGIN);
            LONG st = Verify(&wd, &cn);
            if (st == 0) {
                std::wstring cat = info.wszCatalogFile;
                size_t sl = cat.find_last_of(L"\\/");
                out->state = SignState::Catalog;
                out->subject = cn;
                out->detail = L"catalog " + (sl == std::wstring::npos ? cat : cat.substr(sl + 1));
                found = true;
            } else {
                *why = L"its catalog does not verify: " + HrText(st);
            }
        } else {
            *why = L"CryptCATCatalogInfoFromContext: " + sys::ErrorText(GetLastError());
        }
        CryptCATAdminReleaseCatalogContext(ca, ci, 0);
        CryptCATAdminReleaseContext(ca, 0);
        if (found) return true;
    }
    return false;
}

// The per-path cache (path, size and last-write time), shared by every signer pass in this process.
SRWLOCK g_cacheLock = SRWLOCK_INIT;
std::map<std::wstring, SignerInfo>* g_cache = nullptr;

std::wstring CacheKey(const std::wstring& path, const WIN32_FIND_DATAW& fd) {
    wchar_t b[80];
    swprintf(b, 80, L"|%lu|%lu|%lu|%lu", fd.nFileSizeHigh, fd.nFileSizeLow, fd.ftLastWriteTime.dwHighDateTime,
             fd.ftLastWriteTime.dwLowDateTime);
    return Lower(path) + b;
}

}  // namespace

std::wstring ItemSignedPath(const SvcItem& item, std::wstring* why) {
    std::wstring win;
    {
        std::vector<wchar_t> w(MAX_PATH);
        UINT n = GetSystemWindowsDirectoryW(w.data(), (UINT)w.size());
        if (n >= w.size()) { w.assign(n + 1, 0); n = GetSystemWindowsDirectoryW(w.data(), (UINT)w.size()); }
        if (n == 0 || n >= w.size()) { *why = L"the Windows folder could not be read: " + sys::ErrorText(GetLastError()); return {}; }
        win.assign(w.data(), n);
    }
    ImageInfo im = NormalizeImage(item.image, win, item.name, IsDriverType(item.type));
    std::wstring path = im.path;
    if (!im.svchostGroup.empty()) {   // a shared svchost: the DLL is what the service runs
        std::wstring dll = RegSz(L"SYSTEM\\CurrentControlSet\\Services\\" + item.name + L"\\Parameters", L"ServiceDll");
        if (dll.empty()) dll = RegSz(L"SYSTEM\\CurrentControlSet\\Services\\" + item.name, L"ServiceDll");
        if (!dll.empty()) path = dll;
    }
    if (path.empty()) { *why = L"no file path in its configuration"; return {}; }
    if (path.find(L'%') != std::wstring::npos) {
        DWORD need = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
        if (need == 0) { *why = L"its path could not be expanded: " + sys::ErrorText(GetLastError()); return {}; }
        std::vector<wchar_t> exp(need + 1, 0);
        DWORD got = ExpandEnvironmentStringsW(path.c_str(), exp.data(), (DWORD)exp.size());
        if (got == 0 || got > exp.size()) { *why = L"its path could not be expanded"; return {}; }
        path = exp.data();
    }
    return path;
}

SignerInfo ReadSigner(const std::wstring& path) {
    SignerInfo r;
    WIN32_FIND_DATAW fd{};
    std::wstring why = PathProblem(path, &fd);
    if (!why.empty()) { r.state = SignState::CantCheck; r.detail = why; return r; }
    const std::wstring key = CacheKey(path, fd);
    AcquireSRWLockShared(&g_cacheLock);
    if (g_cache) {
        auto it = g_cache->find(key);
        if (it != g_cache->end()) { r = it->second; ReleaseSRWLockShared(&g_cacheLock); return r; }
    }
    ReleaseSRWLockShared(&g_cacheLock);

    // Rule 1 again at open time: the handle never follows a reparse point, and its own attributes are checked before
    // anything reads the data.
    HANDLE h = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        r.state = SignState::CantCheck;
        r.detail = L"the file could not be opened: " + sys::ErrorText(GetLastError());
        return r;
    }
    BY_HANDLE_FILE_INFORMATION bi{};
    if (!GetFileInformationByHandle(h, &bi)) {
        r.state = SignState::CantCheck;
        r.detail = L"the file's attributes could not be read: " + sys::ErrorText(GetLastError());
        CloseHandle(h);
        return r;
    }
    if (std::wstring a = SignerAttrsProblem(bi.dwFileAttributes); !a.empty()) {
        r.state = SignState::CantCheck;
        r.detail = a;
        CloseHandle(h);
        return r;
    }
    const std::wstring plain = path.rfind(L"\\\\?\\", 0) == 0 ? path.substr(4) : path;
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = plain.c_str();
    fi.hFile = h;
    WINTRUST_DATA wd{};
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    std::wstring cn;
    LONG st = Verify(&wd, &cn);
    if (st == 0) {
        r.state = SignState::Signed;
        r.subject = cn;
    } else {
        // No valid embedded signature: Windows' own files are mostly catalog-signed, so the catalog decides next.
        std::wstring cwhy;
        SignerInfo cat;
        if (CatalogSigned(h, plain, &cat, &cwhy)) r = cat;
        else if (NoSignature(st)) {
            if (cwhy.empty()) r.state = SignState::Unsigned;
            else { r.state = SignState::CantCheck; r.detail = cwhy; }
        } else if (BadSignature(st)) {
            r.state = SignState::Untrusted;
            r.detail = HrText(st);
        } else {
            r.state = SignState::CantCheck;
            r.detail = L"WinVerifyTrust: " + HrText(st) + L" (offline check: a certificate chain that needs a download can't be built)";
        }
    }
    CloseHandle(h);
    AcquireSRWLockExclusive(&g_cacheLock);
    if (!g_cache) g_cache = new std::map<std::wstring, SignerInfo>();   // process lifetime, like the KB
    if (g_cache->size() < 8192) (*g_cache)[key] = r;
    ReleaseSRWLockExclusive(&g_cacheLock);
    return r;
}

SignerInfo ItemSigner(const SvcItem& item) {
    std::wstring why;
    std::wstring path = ItemSignedPath(item, &why);
    if (path.empty()) {
        SignerInfo r;
        r.state = SignState::CantCheck;
        r.detail = why;
        return r;
    }
    return ReadSigner(path);
}

// ---- event 7040 ---------------------------------------------------------------------------------------------------
std::wstring XmlUnescape(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != L'&') { o += s[i]; continue; }
        size_t semi = s.find(L';', i);
        if (semi == std::wstring::npos || semi - i > 10) { o += s[i]; continue; }
        std::wstring ent = s.substr(i + 1, semi - i - 1);
        wchar_t c = 0;
        if (ent == L"lt") c = L'<';
        else if (ent == L"gt") c = L'>';
        else if (ent == L"amp") c = L'&';
        else if (ent == L"quot") c = L'"';
        else if (ent == L"apos") c = L'\'';
        else if (ent.size() > 1 && ent[0] == L'#') {
            unsigned long v = ent[1] == L'x' || ent[1] == L'X' ? wcstoul(ent.c_str() + 2, nullptr, 16) : wcstoul(ent.c_str() + 1, nullptr, 10);
            if (v > 0 && v < 0xD800) c = (wchar_t)v;
        }
        if (!c) { o += s[i]; continue; }
        o += c;
        i = semi;
    }
    return o;
}

namespace {
// The text of attribute `attr` on the first <tag ...> element after `from`; either quote style.
bool AttrOf(const std::wstring& x, const std::wstring& tag, const std::wstring& attr, std::wstring* out, size_t from = 0) {
    size_t t = from;
    for (;;) {
        t = x.find(L"<" + tag, t);
        if (t == std::wstring::npos) return false;
        wchar_t after = t + 1 + tag.size() < x.size() ? x[t + 1 + tag.size()] : L'\0';
        if (after == L' ' || after == L'/' || after == L'>' || after == L'\t' || after == L'\n' || after == L'\r') break;
        t += 1;
    }
    size_t end = x.find(L'>', t);
    if (end == std::wstring::npos) return false;
    for (size_t a = x.find(attr + L"=", t); a != std::wstring::npos && a < end; a = x.find(attr + L"=", a + 1)) {
        if (a == 0 || !(x[a - 1] == L' ' || x[a - 1] == L'\t' || x[a - 1] == L'\n' || x[a - 1] == L'\r')) continue;
        size_t q = a + attr.size() + 1;
        if (q >= end || (x[q] != L'"' && x[q] != L'\'')) return false;
        size_t q2 = x.find(x[q], q + 1);
        if (q2 == std::wstring::npos || q2 > end) return false;
        *out = XmlUnescape(x.substr(q + 1, q2 - q - 1));
        return true;
    }
    return false;
}

// The text content of the first <tag ...>text</tag>.
bool TextOf(const std::wstring& x, const std::wstring& tag, std::wstring* out) {
    size_t t = 0;
    for (;;) {
        t = x.find(L"<" + tag, t);
        if (t == std::wstring::npos) return false;
        wchar_t after = t + 1 + tag.size() < x.size() ? x[t + 1 + tag.size()] : L'\0';
        if (after == L' ' || after == L'>' || after == L'\t') break;
        t += 1;
    }
    size_t gt = x.find(L'>', t);
    if (gt == std::wstring::npos || x[gt - 1] == L'/') return false;
    size_t lt = x.find(L"</" + tag, gt);
    if (lt == std::wstring::npos) return false;
    *out = XmlUnescape(x.substr(gt + 1, lt - gt - 1));
    return true;
}

// EventData's <Data Name="...">text</Data> pairs ("" for <Data Name="x"/>).
std::map<std::wstring, std::wstring> DataOf(const std::wstring& x) {
    std::map<std::wstring, std::wstring> m;
    size_t ed = x.find(L"<EventData");
    if (ed == std::wstring::npos) return m;
    for (size_t d = x.find(L"<Data", ed); d != std::wstring::npos; d = x.find(L"<Data", d + 5)) {
        std::wstring name;
        if (!AttrOf(x, L"Data", L"Name", &name, d)) continue;
        size_t gt = x.find(L'>', d);
        if (gt == std::wstring::npos) break;
        if (x[gt - 1] == L'/') { m[name] = L""; continue; }
        size_t lt = x.find(L"</Data>", gt);
        if (lt == std::wstring::npos) break;
        m[name] = XmlUnescape(x.substr(gt + 1, lt - gt - 1));
    }
    return m;
}
}  // namespace

bool ParseEvent7040(const std::wstring& xml, Ev7040* out, std::wstring* err) {
    *out = Ev7040{};
    std::wstring provider, id;
    if (!AttrOf(xml, L"Provider", L"Name", &provider)) { *err = L"no System/Provider/@Name"; return false; }
    if (provider != L"Service Control Manager") { *err = L"not a Service Control Manager event (" + provider + L")"; return false; }
    if (!TextOf(xml, L"EventID", &id) || _wtoi(id.c_str()) != 7040) { *err = L"not event 7040"; return false; }
    if (!AttrOf(xml, L"TimeCreated", L"SystemTime", &out->time) || out->time.empty()) { *err = L"no TimeCreated/@SystemTime"; return false; }
    std::wstring rec;
    if (TextOf(xml, L"EventRecordID", &rec)) out->record = wcstoull(rec.c_str(), nullptr, 10);
    AttrOf(xml, L"Security", L"UserID", &out->sid);   // optional: an event without one names no account
    auto d = DataOf(xml);
    out->display = d[L"param1"];
    out->from = d[L"param2"];
    out->to = d[L"param3"];
    out->service = d[L"param4"];
    if (out->display.empty() && out->service.empty()) { *err = L"no EventData param1/param4 (the service)"; return false; }
    return true;
}

std::wstring EventTimeText(const std::wstring& t) {
    // yyyy-mm-ddThh:mm...Z
    if (t.size() >= 16 && t[4] == L'-' && t[7] == L'-' && t[10] == L'T' && t[13] == L':' && t.back() == L'Z')
        return t.substr(0, 10) + L" " + t.substr(11, 5) + L" UTC";
    return t;
}

std::map<std::wstring, Ev7040> NewestPerService(const std::vector<Ev7040>& events, const std::map<std::wstring, std::wstring>& names) {
    std::map<std::wstring, std::wstring> byDisplay;   // lower display -> lower name
    for (auto& n : names) if (!n.second.empty()) byDisplay[Lower(n.second)] = n.first;
    std::map<std::wstring, Ev7040> m;
    for (auto& e : events) {
        std::wstring key;
        if (!e.service.empty()) key = Lower(e.service);
        else if (auto it = byDisplay.find(Lower(e.display)); it != byDisplay.end()) key = it->second;
        if (key.empty() || !names.count(key)) continue;
        auto it = m.find(key);
        // SystemTime is fixed-width ISO 8601 UTC, so text order is time order.
        if (it == m.end() || e.time > it->second.time || (e.time == it->second.time && e.record > it->second.record)) m[key] = e;
    }
    return m;
}

std::wstring Ev7040Text(const Ev7040* e) {
    if (!e) return L"no 7040 event found (log may have rolled over)";
    std::wstring who = !e->account.empty() ? e->account + (e->sid.empty() ? L"" : L" (" + e->sid + L")")
                       : !e->sid.empty() ? e->sid + L" (account not resolved)" : std::wstring(L"an account the event does not name");
    std::wstring t = L"changed by " + who + L" on " + EventTimeText(e->time);
    if (!e->from.empty() || !e->to.empty()) t += L", from " + e->from + L" to " + e->to;
    return t + L" (System log, Service Control Manager event 7040; the newest one for it)";
}

bool ParseEventsFixture(const std::string& utf8, std::vector<Ev7040>* out, std::wstring* err) {
    out->clear();
    if (utf8.size() > kMaxFile) { *err = L"events file is larger than 16 MB"; return false; }
    std::vector<std::wstring> lines = Split(FromUtf8(utf8), L'\n');
    if (lines.empty() || lines[0].rfind(L"HXSVC\t1\tevents", 0) != 0) { *err = L"missing HXSVC 1 events header"; return false; }
    std::map<std::wstring, std::wstring> accounts;
    for (size_t i = 1; i < lines.size(); i++) {
        std::wstring l = lines[i];
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        auto f = Split(l, L'\t');
        if (f.size() >= 3 && f[0] == L"A") accounts[Unesc(f[1])] = Unesc(f[2]);
        else if (f.size() >= 2 && f[0] == L"E") {
            Ev7040 e;
            std::wstring pe;
            if (!ParseEvent7040(Unesc(f[1]), &e, &pe)) { *err = L"line " + std::to_wstring(i + 1) + L": " + pe; return false; }
            out->push_back(std::move(e));
        }
    }
    for (auto& e : *out) if (auto it = accounts.find(e.sid); it != accounts.end()) e.account = it->second;
    return true;
}

namespace {
std::wstring LookupSid(const std::wstring& sid, std::map<std::wstring, std::wstring>* cache) {
    if (sid.empty()) return {};
    if (auto it = cache->find(sid); it != cache->end()) return it->second;
    std::wstring out;
    PSID p = nullptr;
    if (ConvertStringSidToSidW(sid.c_str(), &p)) {
        wchar_t name[256] = {}, dom[256] = {};
        DWORD cn = 256, cd = 256;
        SID_NAME_USE use{};
        if (LookupAccountSidW(nullptr, p, name, &cn, dom, &cd, &use)) out = dom[0] ? std::wstring(dom) + L"\\" + name : std::wstring(name);
        LocalFree(p);
    }
    (*cache)[sid] = out;
    return out;
}
}  // namespace

bool ReadLive7040(const std::map<std::wstring, std::wstring>& names, const std::function<bool()>& cancelled, std::vector<Ev7040>* out,
                  std::wstring* err) {
    out->clear();
    if (names.empty()) return true;
    EVT_HANDLE q = EvtQuery(nullptr, L"System", L"*[System[Provider[@Name='Service Control Manager'] and (EventID=7040)]]",
                            EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!q) { *err = L"EvtQuery(System): " + sys::ErrorText(GetLastError()); return false; }
    std::set<std::wstring> missing;
    for (auto& n : names) missing.insert(n.first);
    std::map<std::wstring, std::wstring> sids;
    std::vector<wchar_t> buf(8192);
    bool ok = true;
    // Newest first; a busy System log can hold thousands of 7040s (every update toggles a few), so the scan is bounded.
    const int kMaxEvents = 20000;
    int seen = 0;
    while (!missing.empty() && seen < kMaxEvents) {
        if (cancelled && cancelled()) break;
        EVT_HANDLE hs[64];
        DWORD got = 0;
        if (!EvtNext(q, 64, hs, 5000, 0, &got)) {
            DWORD e = GetLastError();
            if (e != ERROR_NO_MORE_ITEMS) { *err = L"EvtNext: " + sys::ErrorText(e); ok = false; }
            break;
        }
        for (DWORD k = 0; k < got; k++) {
            seen++;
            DWORD used = 0, props = 0;
            if (!EvtRender(nullptr, hs[k], EvtRenderEventXml, (DWORD)(buf.size() * sizeof(wchar_t)), buf.data(), &used, &props)) {
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && used < (1u << 22)) {
                    buf.assign(used / sizeof(wchar_t) + 16, 0);
                    if (!EvtRender(nullptr, hs[k], EvtRenderEventXml, (DWORD)(buf.size() * sizeof(wchar_t)), buf.data(), &used, &props)) {
                        EvtClose(hs[k]);
                        continue;
                    }
                } else {
                    EvtClose(hs[k]);
                    continue;
                }
            }
            EvtClose(hs[k]);
            Ev7040 e;
            std::wstring pe;
            if (!ParseEvent7040(std::wstring(buf.data()), &e, &pe)) continue;
            std::wstring key = Lower(e.service);
            if (key.empty() || !names.count(key))
                for (auto& n : names) if (!e.display.empty() && IEq(n.second, e.display)) key = n.first;
            if (!names.count(key)) continue;
            e.account = LookupSid(e.sid, &sids);
            missing.erase(key);
            out->push_back(std::move(e));
        }
    }
    EvtClose(q);
    return ok;
}

}  // namespace hx::services
