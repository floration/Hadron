// modules/services/store.cpp — data dir, snapshot and undo-log formats, CREATE_NEW + flush writers, lock.
#include "modules/services/store.h"
#include "core/sys/sys.h"
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <algorithm>
#include <set>

namespace hx::services {

namespace {
std::vector<std::wstring> Lines(const std::string& utf8) {
    std::vector<std::wstring> v = Split(FromUtf8(utf8), L'\n');
    for (auto& l : v) if (!l.empty() && l.back() == L'\r') l.pop_back();
    return v;
}
DWORD HexOf(const std::wstring& s) { return (DWORD)wcstoul(s.c_str(), nullptr, 16); }
int IntOf(const std::wstring& s, int def) { return s.empty() ? def : _wtoi(s.c_str()); }
std::wstring HexW(DWORD v) { wchar_t b[16]; swprintf(b, 16, L"%X", (unsigned)v); return b; }
}  // namespace

const SvcItem* Snapshot::Find(const std::wstring& name) const {
    for (auto& s : items) if (IEq(s.name, name)) return &s;
    return nullptr;
}

// S  name display type cfg trigger state errctl account image group deps desc regStart protect controls pid
//    failure triggers ifaces company readError
std::string WriteSnapshot(const Snapshot& s, bool extras) {
    std::wstring o = L"HXSVC\t1\tsnapshot\n";
    for (auto& m : s.meta) o += L"M\t" + Esc(m.first) + L"\t" + Esc(m.second) + L"\n";
    for (auto& i : s.items) {
        std::vector<std::wstring> f = {
            L"S", Esc(i.name), Esc(i.display), HexW(i.type), CfgText(i.cfg), i.trigger ? L"1" : L"0",
            std::to_wstring(i.state), std::to_wstring(i.errctl), Esc(i.account), Esc(i.image), Esc(i.group),
            Esc(Join(i.deps, L";")), Esc(i.desc), i.regStart < 0 ? L"" : std::to_wstring(i.regStart),
            std::to_wstring(i.protect), HexW(i.controls), std::to_wstring(i.pid), Esc(i.failure), Esc(i.triggers),
            Esc(Join(i.ifaces, L";")), Esc(i.company), Esc(i.readError)};
        o += Join(f, L"\t") + L"\n";
        if (extras && i.aclTi) o += L"X\t" + Esc(i.name) + L"\tacl=ti\n";
    }
    return ToUtf8(o);
}

bool ParseSnapshot(const std::string& utf8, Snapshot* out, std::wstring* err) {
    *out = Snapshot{};
    if (utf8.size() > kMaxFile) { *err = L"snapshot is larger than 16 MB"; return false; }
    auto lines = Lines(utf8);
    if (lines.empty() || lines[0] != L"HXSVC\t1\tsnapshot") { *err = L"not a Hadron services snapshot (header)"; return false; }
    std::map<std::wstring, size_t> seen;
    for (size_t n = 1; n < lines.size(); n++) {
        const std::wstring& l = lines[n];
        if (l.empty()) continue;
        auto f = Split(l, L'\t');
        if (f[0] == L"M" && f.size() >= 3) {
            out->meta[Unesc(f[1])] = Unesc(f[2]);
        } else if (f[0] == L"S") {
            if (f.size() < 13) { *err = L"snapshot line " + std::to_wstring(n + 1) + L": S line too short"; return false; }
            SvcItem i;
            i.name = Unesc(f[1]);
            i.display = Unesc(f[2]);
            i.type = HexOf(f[3]);
            if (!ParseCfg(f[4], &i.cfg)) { *err = L"snapshot line " + std::to_wstring(n + 1) + L": bad start '" + f[4] + L"'"; return false; }
            i.trigger = f[5] == L"1";
            i.state = (DWORD)IntOf(f[6], 1);
            i.errctl = IntOf(f[7], 1);
            i.account = Unesc(f[8]);
            i.image = Unesc(f[9]);
            i.group = Unesc(f[10]);
            for (auto& d : Split(Unesc(f[11]), L';')) if (!d.empty()) i.deps.push_back(d);
            i.desc = Unesc(f[12]);
            auto at = [&](size_t k) { return k < f.size() ? f[k] : std::wstring(); };
            i.regStart = IntOf(at(13), -1);
            i.protect = IntOf(at(14), 0);
            i.controls = HexOf(at(15));
            i.pid = (DWORD)IntOf(at(16), 0);
            i.failure = Unesc(at(17));
            i.triggers = Unesc(at(18));
            for (auto& g : Split(Unesc(at(19)), L';')) if (!g.empty()) i.ifaces.push_back(Lower(g));
            i.company = Unesc(at(20));
            i.readError = Unesc(at(21));
            i.cfgUnknown = i.readError.rfind(kCfgUnreadable, 0) == 0;
            i.kind = KindOf(i.type);
            if (i.name.empty()) { *err = L"snapshot line " + std::to_wstring(n + 1) + L": empty name"; return false; }
            if (seen.count(Lower(i.name))) { *err = L"snapshot line " + std::to_wstring(n + 1) + L": duplicate " + i.name; return false; }
            seen[Lower(i.name)] = out->items.size();
            out->items.push_back(std::move(i));
        } else if (f[0] == L"X" && f.size() >= 3) {
            auto it = seen.find(Lower(Unesc(f[1])));
            if (it == seen.end()) continue;
            for (size_t k = 2; k < f.size(); k++) if (f[k] == L"acl=ti") out->items[it->second].aclTi = true;
        } else {
            out->skippedLines++;   // unknown line kinds: forward compatibility
        }
    }
    return true;
}

// ---- undo log -------------------------------------------------------------------------------------
std::string Record(const std::vector<std::wstring>& fields) {
    std::string body = ToUtf8(Join(fields, L"\t"));
    uint32_t crc = Crc32(body.data(), body.size());
    return body + "\t" + ToUtf8(Hex8(crc)) + "\n";
}

UndoRow* UndoLog::Row(int seq) {
    for (auto& r : rows) if (r.seq == seq) return &r;
    return nullptr;
}

bool ParseUndoLog(const std::string& utf8, UndoLog* out, std::wstring* err) {
    *out = UndoLog{};
    if (utf8.size() > kMaxFile) { *err = L"undo log is larger than 16 MB"; return false; }
    // Split on LF but keep raw bytes for the CRC check.
    std::vector<std::string> raw;
    size_t a = 0;
    while (a < utf8.size()) {
        size_t b = utf8.find('\n', a);
        if (b == std::string::npos) { raw.push_back(utf8.substr(a)); break; }   // no LF: torn last record
        raw.push_back(utf8.substr(a, b - a));
        a = b + 1;
    }
    if (raw.empty() || raw[0] != "HXSVC\t1\tundo") { *err = L"not a Hadron services undo log (header)"; return false; }
    bool lastTorn = !utf8.empty() && utf8.back() != '\n';
    for (size_t n = 1; n < raw.size(); n++) {
        std::string line = raw[n];
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t t = line.rfind('\t');
        bool ok = t != std::string::npos && t + 9 == line.size() && !(lastTorn && n + 1 == raw.size());
        if (ok) {
            std::string body = line.substr(0, t);
            uint32_t crc = Crc32(body.data(), body.size());
            ok = ToUtf8(Hex8(crc)) == line.substr(t + 1);
            line = body;
        }
        if (!ok) { out->badRecords++; continue; }
        auto f = Split(FromUtf8(line), L'\t');
        const std::wstring& k = f[0];
        if (k == L"B" && f.size() >= 4) {
            out->hasB = true; out->stamp = f[1]; out->source = Unesc(f[2]); out->count = _wtoi(f[3].c_str());
            if (f.size() >= 5) out->machine = f[4];
            if (f.size() >= 6) out->build = f[5];
            if (f.size() >= 7) out->order = _wtoi(f[6].c_str());
        } else if (k == L"P" && f.size() >= 7) {
            UndoRow r;
            r.seq = _wtoi(f[1].c_str());
            r.name = Unesc(f[2]);
            if (!ParseCfg(f[4], &r.before) || !ParseCfg(f[5], &r.after)) { out->badRecords++; continue; }
            r.identity = f[6];
            out->rows.push_back(r);
        } else if (k == L"I" && f.size() >= 3) {
            UndoRow* r = out->Row(_wtoi(f[1].c_str()));
            if (r && ParseCfg(f[2], &r->observed)) { r->hasI = true; if (f.size() >= 4) r->iIdentity = f[3]; }
        } else if (k == L"D" && f.size() >= 4) {
            UndoRow* r = out->Row(_wtoi(f[1].c_str()));
            if (r) {
                r->hasD = true;
                r->result = f[2];
                if (!ParseCfg(f[3], &r->afterObs)) r->afterObs = r->observed;
                if (f.size() >= 5) r->text = Unesc(f[4]);
            }
        } else if (k == L"T" && f.size() >= 2) {
            if (f[1] == L"started") out->tiStarted = true;
            if (f[1] == L"stopped") out->tiStopped = true;
        } else if (k == L"E") {
            out->hasE = true;
        }
    }
    if (!out->hasB) { *err = L"undo log has no begin record"; return false; }
    return true;
}

bool& ImpersonationFlag::Active() {
    static thread_local bool active = false;
    return active;
}

// ---- writers ---------------------------------------------------------------------------------------
bool LogWriter::Open(const std::wstring& path, std::wstring* err) {
    if (ImpersonationFlag::Active()) { *err = L"refused: a Hadron file write while impersonating"; return false; }
    h_ = CreateFileW(sys::ToExtended(path).c_str(), GENERIC_WRITE, 0 /* no sharing: the lock's second half */,
                     nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) { *err = L"Could not create " + path + L": " + sys::ErrorText(GetLastError()); return false; }
    writes_ = 0;
    return true;
}

bool LogWriter::Append(const std::string& utf8, std::wstring* err) {
    if (h_ == INVALID_HANDLE_VALUE) { *err = L"undo log is not open"; return false; }
    if (ImpersonationFlag::Active()) { *err = L"refused: an undo-log write while impersonating"; return false; }
    DWORD put = 0;
    if (!WriteFile(h_, utf8.data(), (DWORD)utf8.size(), &put, nullptr) || put != utf8.size()) {
        *err = L"Undo log write failed: " + sys::ErrorText(GetLastError());
        return false;
    }
    if (failAfter >= 0 && writes_ >= failAfter) { *err = L"Undo log flush failed (injected by the selftest)"; return false; }
    if (!FlushFileBuffers(h_)) { *err = L"Undo log flush failed: " + sys::ErrorText(GetLastError()); return false; }
    writes_++;
    return true;
}

void LogWriter::Close() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
}

namespace {
// A mutex is recursive for the thread that owns it, so the names this process holds are tracked too: a second
// Acquire from the same process (any thread) is refused like one from another process.
SRWLOCK g_heldLock = SRWLOCK_INIT;
std::set<std::wstring>& Held() { static std::set<std::wstring> s; return s; }
}  // namespace

bool ApplyLock::Acquire(const std::wstring& name, std::wstring* err) {
    Release();
    const std::wstring busy = L"Another Hadron is changing services right now. Try again when it has finished.";
    AcquireSRWLockExclusive(&g_heldLock);
    bool mine = Held().count(name) > 0;
    if (!mine) Held().insert(name);
    ReleaseSRWLockExclusive(&g_heldLock);
    if (mine) { *err = busy; return false; }
    m_ = CreateMutexW(nullptr, FALSE, name.c_str());
    DWORD w = m_ ? WaitForSingleObject(m_, 0) : WAIT_FAILED;
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { name_ = name; return true; }   // abandoned: a crashed apply; the log tells
    DWORD e = GetLastError();
    if (m_) CloseHandle(m_);
    m_ = nullptr;
    AcquireSRWLockExclusive(&g_heldLock);
    Held().erase(name);
    ReleaseSRWLockExclusive(&g_heldLock);
    *err = w == WAIT_FAILED ? L"Could not create the apply lock: " + sys::ErrorText(e) : busy;
    return false;
}

void ApplyLock::Release() {
    if (!m_) return;
    ReleaseMutex(m_);
    CloseHandle(m_);
    m_ = nullptr;
    AcquireSRWLockExclusive(&g_heldLock);
    Held().erase(name_);
    ReleaseSRWLockExclusive(&g_heldLock);
    name_.clear();
}

// ---- store ---------------------------------------------------------------------------------------
std::wstring Store::DefaultRoot(std::wstring* err) {
    PWSTR p = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p);
    if (FAILED(hr) || !p) {
        if (p) CoTaskMemFree(p);
        *err = L"Could not find %ProgramData%: " + sys::ErrorText((DWORD)hr);
        return {};
    }
    std::wstring r = std::wstring(p) + L"\\Hadron\\services";
    CoTaskMemFree(p);
    return r;
}

std::wstring Store::Path(const std::wstring& rel) const { return rel.empty() ? root_ : root_ + L"\\" + rel; }

bool Store::Exists(const std::wstring& rel) const {
    return GetFileAttributesW(sys::ToExtended(Path(rel)).c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring Store::Plain(const std::wstring& path) { return sys::ToShell(path); }

#ifdef HX_SERVICES_TEST_HOOKS
bool& Store::AcceptTokenUserAsOwner() {
    static bool accept = false;
    return accept;
}

std::wstring& Store::InjectedReparsePoint() {
    static std::wstring path;
    return path;
}
#endif

namespace {
// Length of the volume or share root of a full path without a \\?\ prefix: "C:\" -> 3, "\\srv\share\" -> its
// length. 0 when the path has no root Hadron recognises.
size_t VolumeRootLen(const std::wstring& p) {
    if (p.size() >= 3 && p[1] == L':' && p[2] == L'\\') return 3;
    if (p.size() > 2 && p[0] == L'\\' && p[1] == L'\\') {
        size_t a = p.find(L'\\', 2);
        if (a == std::wstring::npos || a == 2) return 0;
        size_t b = p.find(L'\\', a + 1);
        return b == std::wstring::npos ? p.size() : b + 1;
    }
    return 0;
}

// Owner Administrators, protected DACL: Administrators and SYSTEM full control, inherited by files and subfolders.
const wchar_t kRootSddl[] = L"O:BAD:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)";

#ifdef HX_SERVICES_TEST_HOOKS
bool IsTokenUser(PSID sid) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    std::vector<BYTE> buf(256);
    DWORD need = 0;
    bool same = false;
    if (GetTokenInformation(tok, TokenUser, buf.data(), (DWORD)buf.size(), &need) ||
        (GetLastError() == ERROR_INSUFFICIENT_BUFFER && (buf.resize(need), GetTokenInformation(tok, TokenUser, buf.data(), need, &need))))
        same = EqualSid(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, sid) != FALSE;
    CloseHandle(tok);
    return same;
}
bool HookOwner(PSID sid) { return Store::AcceptTokenUserAsOwner() && IsTokenUser(sid); }
bool HookReparse(const std::wstring& path) { return !Store::InjectedReparsePoint().empty() && IEq(path, Store::InjectedReparsePoint()); }
#else
// Release builds: no hooks, nothing relaxes a check.
constexpr bool HookOwner(PSID) { return false; }
bool HookReparse(const std::wstring&) { return false; }
#endif

bool IsTiSid(PSID sid) {   // NT SERVICE\TrustedInstaller
    PSID ti = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &ti)) return false;
    bool same = EqualSid(ti, sid) != FALSE;
    LocalFree(ti);
    return same;
}

std::wstring SidText(PSID sid);

// "" when the folder open in h is owned by Administrators or SYSTEM (or TrustedInstaller, when allowTi), else why
// not. *owner gets the owner's name when one was read.
std::wstring OwnerProblem(HANDLE h, const std::wstring& path, bool allowTi, std::wstring* owner) {
    PSID sid = nullptr;
    PSECURITY_DESCRIPTOR osd = nullptr;
    std::wstring fail;
    DWORD e = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &sid, nullptr, nullptr, nullptr, &osd);
    if (e != ERROR_SUCCESS || !sid) fail = L"Could not read the owner of " + path + L": " + sys::ErrorText(e);
    else if (!IsWellKnownSid(sid, WinBuiltinAdministratorsSid) && !IsWellKnownSid(sid, WinLocalSystemSid) &&
             !(allowTi && IsTiSid(sid)) && !HookOwner(sid)) {
        *owner = SidText(sid);
        fail = L"owned";
    }
    if (osd) LocalFree(osd);
    return fail;
}

std::wstring SidText(PSID sid) {
    wchar_t name[256], dom[256];
    DWORD n = 256, d = 256;
    SID_NAME_USE use;
    if (LookupAccountSidW(nullptr, sid, name, &n, dom, &d, &use)) return (d > 0 && dom[0] ? std::wstring(dom) + L"\\" : L"") + name;
    LPWSTR s = nullptr;
    if (ConvertSidToStringSidW(sid, &s)) { std::wstring r = s; LocalFree(s); return r; }
    return L"(unknown)";
}

bool IsAdminSystemOrTi(PSID sid) {
    // HookOwner: test builds under Wine only, where every DACL reads back as "this user: full control".
    return IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || IsWellKnownSid(sid, WinLocalSystemSid) || IsTiSid(sid) || HookOwner(sid);
}

// The DACL of the folder open in h, read by handle and checked by Store::WriteAccessProblem. Nothing is changed.
std::wstring DaclProblem(HANDLE h) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD e = GetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
    if (e != ERROR_SUCCESS) return L"its permissions could not be read (" + sys::ErrorText(e) + L")";
    std::wstring why = Store::WriteAccessProblem(sd);
    if (sd) LocalFree(sd);
    return why;
}
}  // namespace

// The same test as modules/bios's backup folder (SdTrusted): every allowed entry with a write, delete or re-permission
// right, inherit-only ones included (they reach the files Hadron writes there).
std::wstring Store::WriteAccessProblem(PSECURITY_DESCRIPTOR sd) {
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    if (!sd || !GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted))
        return L"its DACL could not be read (" + sys::ErrorText(GetLastError()) + L")";
    if (!present || !dacl) return L"it has no DACL, so everyone has full access";
    const ACCESS_MASK kWrite = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD |
                               DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation))
        return L"its DACL could not be read (" + sys::ErrorText(GetLastError()) + L")";
    for (DWORD i = 0; i < info.AceCount; i++) {
        void* p = nullptr;
        if (!GetAce(dacl, i, &p) || !p) return L"an entry of its DACL could not be read";
        const BYTE t = static_cast<const ACE_HEADER*>(p)->AceType;
        const bool allowed = t == ACCESS_ALLOWED_ACE_TYPE || t == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
                             t == ACCESS_ALLOWED_OBJECT_ACE_TYPE || t == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE ||
                             t == ACCESS_ALLOWED_COMPOUND_ACE_TYPE;
        if (!allowed) continue;   // deny and audit entries grant nothing
        // Every allowed entry layout has the mask right after the header.
        ACCESS_MASK mask = *reinterpret_cast<const ACCESS_MASK*>(static_cast<const BYTE*>(p) + sizeof(ACE_HEADER));
        if (!(mask & kWrite)) continue;
        if (t != ACCESS_ALLOWED_ACE_TYPE && t != ACCESS_ALLOWED_CALLBACK_ACE_TYPE)
            return L"an object or compound entry in its DACL grants write access (access mask 0x" + Hex8(mask) + L")";
        PSID sid = (PSID)&static_cast<ACCESS_ALLOWED_ACE*>(p)->SidStart;
        if (IsAdminSystemOrTi(sid)) continue;
        return L"its permissions let " + SidText(sid) + L" change it (access mask 0x" + Hex8(mask) + L")";
    }
    return L"";
}

bool Store::EnsureRoot(bool secure, std::wstring* err) {
    if (root_.empty()) { *err = L"no data folder"; return false; }
    if (ImpersonationFlag::Active()) { *err = L"refused: a Hadron file write while impersonating"; return false; }
    const std::wstring plain = sys::ToShell(sys::ToExtended(root_));   // full path, no \\?\ prefix
    const size_t rl = VolumeRootLen(plain);
    if (rl == 0 || rl >= plain.size()) { *err = L"the data folder must be a full path below a drive or share: " + root_; return false; }
    // Every component below the volume or share root, top down: an existing one must be a real folder, never a
    // reparse point (junction, mount point, symlink), so nothing is followed into a folder someone planted. Missing
    // ones are created: parents with their inherited ACLs, the root with kRootSddl.
    std::vector<std::wstring> chain;
    for (size_t pos = rl; pos < plain.size();) {
        size_t n = plain.find(L'\\', pos);
        if (n == std::wstring::npos) n = plain.size();
        if (n > pos) chain.push_back(plain.substr(0, n));
        pos = n + 1;
    }
    bool createdRoot = false;   // this call created the root, with kRootSddl
    for (size_t i = 0; i < chain.size(); i++) {
        const bool last = i + 1 == chain.size();
        // The folder that holds the root (%ProgramData%\Hadron by default; never a volume or share root): its owner
        // could rename the root aside and put their own in its place, so it must be Administrators, SYSTEM or
        // TrustedInstaller too. Checked before the root is created or opened (Analyze r2).
        if (last && secure && i > 0) {
            HANDLE ph = CreateFileW(sys::ToExtended(chain[i - 1]).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (ph == INVALID_HANDLE_VALUE) {
                *err = L"Could not open " + chain[i - 1] + L" to check its owner: " + sys::ErrorText(GetLastError());
                return false;
            }
            std::wstring who, why = OwnerProblem(ph, chain[i - 1], true, &who);
            CloseHandle(ph);
            if (why == L"owned")
                why = chain[i - 1] + L", the folder that holds Hadron's data folder, is owned by " + who +
                      L", not by Administrators, SYSTEM or TrustedInstaller. Its owner could swap the data folder for "
                      L"their own, so changes are off. Rename it aside as an administrator and restart Hadron, or give it another --data-dir.";
            if (!why.empty()) { *err = why; return false; }
        }
        const std::wstring ext = sys::ToExtended(chain[i]);
        DWORD a = GetFileAttributesW(ext.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            DWORD e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) {
                *err = L"Could not read " + chain[i] + L": " + sys::ErrorText(e);
                return false;
            }
            PSECURITY_DESCRIPTOR sd = nullptr;
            SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
            // The root gets kRootSddl; the folder that holds it gets owner Administrators (with the inherited DACL), as
            // the holding-folder check requires: an elevated token's default owner may be the user ("object creator").
            const wchar_t* sddl = !secure ? nullptr : last ? kRootSddl : i + 2 == chain.size() ? L"O:BA" : nullptr;
            if (sddl) {
                if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) {
                    *err = L"Could not build the data folder's DACL: " + sys::ErrorText(GetLastError());
                    return false;
                }
                sa.lpSecurityDescriptor = sd;
            }
            BOOL ok = CreateDirectoryW(ext.c_str(), sd ? &sa : nullptr);
            e = GetLastError();
            if (sd) LocalFree(sd);
            if (!ok && e != ERROR_ALREADY_EXISTS) { *err = L"Could not create " + chain[i] + L": " + sys::ErrorText(e); return false; }
            if (ok && last) createdRoot = true;   // ERROR_ALREADY_EXISTS: someone made it meanwhile; checked as existing
            a = GetFileAttributesW(ext.c_str());
            if (a == INVALID_FILE_ATTRIBUTES) { *err = L"Could not read " + chain[i] + L": " + sys::ErrorText(GetLastError()); return false; }
        }
        if ((a & FILE_ATTRIBUTE_REPARSE_POINT) || HookReparse(chain[i])) {
            *err = chain[i] + L" is a link (a junction, mount point or symbolic link). Hadron never writes through one, so changes "
                              L"are off. Remove the link as an administrator, or give Hadron another --data-dir.";
            return false;
        }
        if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { *err = chain[i] + L" exists and is not a folder"; return false; }
    }
    if (!secure) return true;

    // The root itself, opened as itself (never what a reparse point names), read-only: check again, then its owner,
    // then its DACL. Hadron sets permissions only when it creates the root (kRootSddl in CreateDirectoryW above) and
    // never changes the security of a folder that already existed (Review R1: --data-dir C:\ProgramData would
    // otherwise have been re-secured, children included). An existing folder must already be admin-only.
    HANDLE h = CreateFileW(sys::ToExtended(plain).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = L"Could not open " + plain + L" to check its owner: " + sys::ErrorText(GetLastError()); return false; }
    std::wstring fail;
    BY_HANDLE_FILE_INFORMATION bi{};
    if (!GetFileInformationByHandle(h, &bi)) fail = L"Could not read " + plain + L": " + sys::ErrorText(GetLastError());
    else if (bi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) fail = plain + L" is a link (a reparse point). Hadron never writes through one.";
    else if (!(bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) fail = plain + L" is not a folder.";
    if (fail.empty()) {
        std::wstring who;
        fail = OwnerProblem(h, plain, false, &who);
        if (fail == L"owned")
            fail = plain + L" is owned by " + who +
                   L", not by Administrators or SYSTEM, so another program may have made it and could change what Hadron "
                   L"writes there. Changes are off. Rename it aside as an administrator and restart Hadron, or give it another --data-dir.";
    }
    if (fail.empty()) {
        std::wstring why = DaclProblem(h);
        if (!why.empty())
            fail = plain + (createdRoot ? L" was just created by Hadron, but " : L" already exists and ") + why +
                   L". Hadron sets permissions only on a data folder it creates itself and never changes the permissions of "
                   L"an existing folder, so changes are off. Give Hadron a --data-dir that doesn't exist yet (it is created "
                   L"admin-only), or, if this is Hadron's own folder, rename it aside as an administrator and restart Hadron.";
    }
    CloseHandle(h);
    if (!fail.empty()) { *err = fail; return false; }
    return true;
}

bool Store::MakeDir(const std::wstring& rel, std::wstring* err) {
    if (ImpersonationFlag::Active()) { *err = L"refused: a Hadron file write while impersonating"; return false; }
    std::wstring p = sys::ToExtended(Path(rel));
    if (!CreateDirectoryW(p.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        *err = L"Could not create " + Path(rel) + L": " + sys::ErrorText(GetLastError());
        return false;
    }
    return true;
}

bool Store::WriteNew(const std::wstring& rel, const std::string& data, std::wstring* err) {
    if (ImpersonationFlag::Active()) { *err = L"refused: a Hadron file write while impersonating"; return false; }
    std::wstring p = Path(rel);
    HANDLE h = CreateFileW(sys::ToExtended(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = L"Could not create " + p + L": " + sys::ErrorText(GetLastError()); return false; }
    DWORD put = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    DWORD e = GetLastError();
    if (ok) { ok = FlushFileBuffers(h) != FALSE; e = GetLastError(); }
    CloseHandle(h);
    if (!ok) { *err = L"Could not write " + p + L": " + sys::ErrorText(e); return false; }
    return true;
}

bool Store::WriteAtomicNew(const std::wstring& rel, const std::string& data, std::wstring* err) {
    if (Exists(rel)) { *err = Path(rel) + L" already exists; it is never overwritten"; return false; }
    std::wstring tmp = rel + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount());
    if (!WriteNew(tmp, data, err)) return false;
    // No MOVEFILE_REPLACE_EXISTING (never overwrite) and no MOVEFILE_COPY_ALLOWED (hard rule 1).
    if (!MoveFileExW(sys::ToExtended(Path(tmp)).c_str(), sys::ToExtended(Path(rel)).c_str(), MOVEFILE_WRITE_THROUGH)) {
        *err = L"Could not move " + Path(tmp) + L" into place: " + sys::ErrorText(GetLastError()) +
               L" (the temp file is left as it is)";
        return false;
    }
    return true;
}

bool Store::Read(const std::wstring& rel, std::string* out, std::wstring* err) const {
    std::wstring p = Path(rel);
    HANDLE h = CreateFileW(sys::ToExtended(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = L"Could not open " + p + L": " + sys::ErrorText(GetLastError()); return false; }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (LONGLONG)kMaxFile) {
        CloseHandle(h);
        *err = p + L" is larger than 16 MB or unreadable";
        return false;
    }
    out->assign((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    bool ok = sz.QuadPart == 0 || (ReadFile(h, &(*out)[0], (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart);
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok) { *err = L"Could not read " + p + L": " + sys::ErrorText(e); return false; }
    return true;
}

bool Store::RenameAside(const std::wstring& rel, std::wstring* newRel, std::wstring* err) {
    if (ImpersonationFlag::Active()) { *err = L"refused: a Hadron file write while impersonating"; return false; }
    std::wstring dst = rel + L".corrupt-" + NewStamp();
    if (!MoveFileExW(sys::ToExtended(Path(rel)).c_str(), sys::ToExtended(Path(dst)).c_str(), MOVEFILE_WRITE_THROUGH)) {
        *err = L"Could not rename " + Path(rel) + L" aside: " + sys::ErrorText(GetLastError());
        return false;
    }
    *newRel = dst;
    return true;
}

std::vector<std::wstring> Store::List(const std::wstring& pattern) const {
    std::vector<std::wstring> v;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(sys::ToExtended(Path(pattern)).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return v;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) v.push_back(fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(v.begin(), v.end());
    return v;
}

std::wstring Store::NewStamp() const {
    std::wstring base = fixedStamp;
    if (base.empty()) {
        SYSTEMTIME t;
        GetSystemTime(&t);   // UTC: a DST change can't make a later stamp sort earlier
        wchar_t b[32];
        swprintf(b, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        base = b;
    }
    for (int n = 0; n < 1000; n++) {
        std::wstring s = n == 0 ? base : base + L"-" + std::to_wstring(n);
        if (!Exists(L"snap-" + s + L".txt") && !Exists(L"undo-" + s + L".log") && !Exists(L"recovery-" + s + L".zip") &&
            !Exists(L"recovery\\" + s))
            return s;
    }
    return base + L"-x" + std::to_wstring(GetTickCount());
}

}  // namespace hx::services
