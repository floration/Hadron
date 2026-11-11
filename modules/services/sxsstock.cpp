// modules/services/sxsstock.cpp — Windows-stock start types from this PC's WinSxS component manifests.
// See sxsstock.h. Read only: the one write is the cache in the data folder, and that goes through Store.
#include "modules/services/sxsstock.h"
#include "modules/services/backend.h"
#include "modules/services/store.h"
#include "core/sys/sys.h"
#include <algorithm>
#include <cstring>

namespace hx::services {

// ---- values ---------------------------------------------------------------------------------------
SxsValue::Src SxsValue::StartSrc() const {
    if (edStart >= 0) return EDITION;
    if (svcStart >= 0) return SERVICE;
    if (regStart >= 0) return REGISTRY;
    if (setStart >= 0) return SETTING;
    return NONE;
}

SxsValue::Src SxsValue::DelayedSrc() const {
    if (edDelayed >= 0) return EDITION;
    if (delayed >= 0) return svcStart >= 0 && delayed == 1 ? SERVICE : REGISTRY;
    if (setDelayed >= 0) return SETTING;
    return NONE;
}

bool SxsValue::Usable() const {
    if (conflict || StartSrc() == NONE) return false;
    // serviceData and the registry value are the same layer: two different values there is a conflict.
    return !(svcStart >= 0 && regStart >= 0 && svcStart != regStart);
}

bool SxsValue::LowConfidence() const {
    if (StartSrc() == SETTING) return true;
    return Cfg().start == 2 && DelayedSrc() == SETTING;
}

StartCfg SxsValue::Cfg() const {
    StartCfg c;
    c.start = edStart >= 0 ? edStart : svcStart >= 0 ? svcStart : regStart >= 0 ? regStart : setStart;
    int d = edDelayed >= 0 ? edDelayed : delayed >= 0 ? delayed : setDelayed;
    c.delayed = c.start == 2 ? (d == 1 ? 1 : 0) : -1;
    return c;
}

const std::wstring& SxsValue::StartFile() const {
    static const std::wstring none;
    switch (StartSrc()) {
        case EDITION: return fileEdition;
        case SERVICE: return fileService;
        case REGISTRY: return fileRegistry;
        case SETTING: return fileSetting;
        case NONE: break;
    }
    return none;
}

std::wstring SxsSrcText(SxsValue::Src s) {
    switch (s) {
        case SxsValue::EDITION: return L"the edition file's override";
        case SxsValue::SERVICE: return L"the component's serviceData start";
        case SxsValue::REGISTRY: return L"the component's registry value";
        case SxsValue::SETTING: return L"a configuration-setting default";
        case SxsValue::NONE: break;
    }
    return L"none";
}

bool SxsRuntimeStart(const std::wstring& name) {
    static const wchar_t* const kList[] = {L"wlansvc", L"stisvc", L"camsvc", L"ikeext", L"policyagent", L"iphlpsvc",
                                           L"ssh-agent", L"winhttpautoproxysvc"};
    std::wstring n = Lower(name);
    for (auto* k : kList) if (n == k) return true;
    return false;
}

bool SxsRuntimeDelayed(const std::wstring& name) {
    static const wchar_t* const kList[] = {L"bits", L"winrm", L"dcsvc", L"dmwappushservice"};
    std::wstring n = Lower(name);
    for (auto* k : kList) if (n == k) return true;
    return false;
}

SxsWords SxsWordsFor(const SxsStock* s) {
    SxsWords w;
    const std::wstring b = s ? s->build : std::wstring();
    const std::wstring ed = s && !s->edition.empty() ? s->edition : std::wstring(L"edition unknown");
    if (s && s->fixture) {
        w.who = L"the test fixture's manifests";
        w.shortSrc = L"the test fixture's hand-written manifests, not a real PC, build " + b;
        w.label = L"read from the test fixture's hand-written manifests (not a real PC), build " + b;
        w.note = L"The start type the test fixture's hand-written manifests (not a real PC) give the service (build " + b;
        w.uses = L"Test data only.";
        w.reason = L"Start type the test fixture's manifests declare (build " + b + L")";
        w.planSrc = L"the test fixture's hand-written manifests (not a real PC; build " + b + L", " + ed + L")";
    } else {
        w.who = L"this PC's manifests";
        w.shortSrc = L"read on this PC by Hadron, component manifests, build " + b;
        w.label = L"read on this PC by Hadron (component manifests, build " + b + L", " + ed + L")";
        w.note = L"The start type this PC's WinSxS component manifests install the service with (read by Hadron on this PC, "
                 L"Windows build " + b + L" " + ed;
        w.uses = L"Hadron uses it as the Windows stock value.";
        w.reason = L"Start type this PC's component manifests declare (build " + b + L")";
        w.planSrc = L"this PC's own WinSxS component manifests (build " + b + L", " + ed + L")";
    }
    return w;
}

const SxsValue* SxsStock::Find(const std::wstring& name, bool instance) const {
    if (!ok) return nullptr;
    std::wstring n = Lower(name);
    auto it = byName.find(n);
    if (it == byName.end() && instance) {
        // A per-user instance ("cdpusersvc_4f2a1") maps to its template: the suffix is hex after the last '_'.
        size_t u = n.rfind(L'_');
        if (u != std::wstring::npos && u + 1 < n.size() &&
            std::all_of(n.begin() + u + 1, n.end(), [](wchar_t ch) { return iswxdigit(ch) != 0; }))
            it = byName.find(n.substr(0, u));
    }
    return it != byName.end() && it->second.Usable() ? &it->second : nullptr;
}

int SxsStock::Usable() const {
    int n = 0;
    for (auto& kv : byName) if (kv.second.Usable()) n++;
    return n;
}

// ---- the XML scanner ------------------------------------------------------------------------------
namespace {

std::string LowerA(std::string s) {
    for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
    return s;
}

std::string Entities(const std::string& v) {
    if (v.find('&') == std::string::npos) return v;
    std::string o;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == '&') {
            static const struct { const char* e; char c; } k[] = {{"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''},
                                                                  {"&lt;", '<'}, {"&gt;", '>'}};
            bool hit = false;
            for (auto& x : k)
                if (v.compare(i, strlen(x.e), x.e) == 0) { o += x.c; i += strlen(x.e) - 1; hit = true; break; }
            if (hit) continue;
        }
        o += v[i];
    }
    return o;
}

std::string Trim(const std::string& v) {
    size_t a = 0, b = v.size();
    while (a < b && isspace((unsigned char)v[a])) a++;
    while (b > a && isspace((unsigned char)v[b - 1])) b--;
    return v.substr(a, b - a);
}

using Attrs = std::map<std::string, std::string>;   // lower-case local name -> value

// A small quote-aware tokenizer: open(tag, attrs, selfClose), close(tag), text(content). Tags and attribute names
// are lower-case local names (any "prefix:" dropped). Comments, CDATA, <? ?> and <! > are skipped. A self-closing
// tag calls open and then close.
template <class Open, class Close, class Text>
void Tokenize(const std::string& x, Open open, Close close, Text text) {
    const size_t n = x.size();
    size_t i = 0;
    while (i < n) {
        size_t lt = x.find('<', i);
        if (lt == std::string::npos) lt = n;
        if (lt > i) text(x.substr(i, lt - i));
        if (lt >= n) break;
        if (x.compare(lt, 4, "<!--") == 0) {
            size_t e = x.find("-->", lt + 4);
            i = e == std::string::npos ? n : e + 3;
            continue;
        }
        if (x.compare(lt, 9, "<![CDATA[") == 0) {
            size_t e = x.find("]]>", lt + 9);
            i = e == std::string::npos ? n : e + 3;
            continue;
        }
        if (lt + 1 < n && (x[lt + 1] == '?' || x[lt + 1] == '!')) {
            size_t e = x.find('>', lt);
            i = e == std::string::npos ? n : e + 1;
            continue;
        }
        bool closing = lt + 1 < n && x[lt + 1] == '/';
        size_t p = lt + (closing ? 2 : 1);
        size_t ns = p;
        while (p < n && !isspace((unsigned char)x[p]) && x[p] != '>' && x[p] != '/') p++;
        std::string tag = x.substr(ns, p - ns);
        size_t colon = tag.rfind(':');
        if (colon != std::string::npos) tag = tag.substr(colon + 1);
        tag = LowerA(tag);
        Attrs attrs;
        bool selfClose = false;
        while (p < n) {
            while (p < n && isspace((unsigned char)x[p])) p++;
            if (p >= n) break;
            if (x[p] == '>') { p++; break; }
            if (x[p] == '/') { selfClose = true; p++; continue; }
            size_t as = p;
            while (p < n && x[p] != '=' && !isspace((unsigned char)x[p]) && x[p] != '>' && x[p] != '/') p++;
            std::string an = x.substr(as, p - as);
            while (p < n && isspace((unsigned char)x[p])) p++;
            if (p < n && x[p] == '=') {
                p++;
                while (p < n && isspace((unsigned char)x[p])) p++;
                if (p < n && (x[p] == '"' || x[p] == '\'')) {
                    char q = x[p++];
                    size_t vs = p;
                    while (p < n && x[p] != q) p++;
                    std::string v = x.substr(vs, p - vs);
                    if (p < n) p++;
                    size_t ac = an.rfind(':');
                    attrs[LowerA(ac == std::string::npos ? an : an.substr(ac + 1))] = Entities(v);
                }
            } else if (p == as) {
                p++;   // a stray character: move on
            }
        }
        i = p;
        if (closing) {
            close(tag);
        } else {
            open(tag, attrs);
            if (selfClose) close(tag);
        }
    }
}

int StartFromWord(const std::string& w, int* delayed) {
    std::string l = LowerA(Trim(w));
    if (l == "boot") return 0;
    if (l == "system") return 1;
    if (l == "auto") return 2;
    if (l == "delayedauto") { *delayed = 1; return 2; }
    if (l == "demand") return 3;
    if (l == "disabled") return 4;
    return -1;
}

// A REG_DWORD as manifests write it: hex digits, with or without 0x. -1 when it is not a number.
long long Dword(const std::string& v) {
    std::string s = Trim(v);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty() || s.size() > 8) return -1;
    long long n = 0;
    for (char ch : s) {
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (d < 0) return -1;
        n = n * 16 + d;
    }
    return n;
}

// "HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\X" -> "x"; "" for anything else (a subkey of X too).
std::string ServiceKey(const std::string& keyName) {
    std::string l = LowerA(Trim(keyName));
    static const char* const kPrefixes[] = {"hkey_local_machine\\system\\currentcontrolset\\services\\",
                                            "hklm\\system\\currentcontrolset\\services\\"};
    for (auto* p : kPrefixes) {
        size_t n = strlen(p);
        if (l.compare(0, n, p) == 0 && l.size() > n && l.find('\\', n) == std::string::npos) return l.substr(n);
    }
    return "";
}

// wcm:handler="regkey('HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\AudioSrv')" -> "audiosrv"
std::string HandlerService(const std::string& h) {
    std::string l = Trim(h);
    if (LowerA(l.substr(0, 7)) != "regkey(") return "";
    size_t a = l.find_first_of("'\"", 7);
    if (a == std::string::npos) return "";
    size_t b = l.find(l[a], a + 1);
    if (b == std::string::npos) return "";
    return ServiceKey(l.substr(a + 1, b - a - 1));
}

void SetField(int* field, int v, bool* conflict) {
    if (*field >= 0 && *field != v) *conflict = true;
    else *field = v;
}

}  // namespace

void ScanManifestXml(const std::string& x, SxsScan* acc, const std::wstring& file) {
    auto mark = [&](std::wstring* f) { if (f->empty()) *f = file; };
    // Cheap filter: most manifests have none of these.
    if (x.find("serviceData") == std::string::npos && LowerA(x).find("\\services\\") == std::string::npos) return;
    std::string curKey;      // the open registryKey's service name, "" when none
    std::string component;   // the manifest's own assemblyIdentity name (the first one outside <dependency>)
    bool haveComponent = false;
    int depDepth = 0;
    Tokenize(x,
        [&](const std::string& tag, const Attrs& a) {
            auto get = [&](const char* k) { auto it = a.find(k); return it == a.end() ? std::string() : it->second; };
            if (tag == "dependency") {
                depDepth++;
            } else if (tag == "assemblyidentity" && !haveComponent && depDepth == 0) {
                component = LowerA(get("name"));
                haveComponent = true;
            } else if (tag == "servicedata") {
                std::string nm = Trim(get("name")), st = get("start");
                if (nm.empty() || st.empty()) return;
                int delayed = -1;
                int start = StartFromWord(st, &delayed);
                if (start < 0) return;
                SxsValue& v = acc->byName[Lower(FromUtf8(nm))];
                SetField(&v.svcStart, start, &v.conflict);
                mark(&v.fileService);
                if (delayed == 1) SetField(&v.delayed, 1, &v.conflict);
            } else if (tag == "registrykey") {
                curKey = ServiceKey(get("keyname"));
            } else if (tag == "registryvalue" && !curKey.empty()) {
                std::string vn = LowerA(Trim(get("name")));
                long long d = Dword(get("value"));
                if (vn == "start" && d >= 0 && d <= 4) {
                    SxsValue& v = acc->byName[Lower(FromUtf8(curKey))];
                    SetField(&v.regStart, (int)d, &v.conflict);
                    mark(&v.fileRegistry);
                } else if (vn == "delayedautostart" && (d == 0 || d == 1)) {
                    SxsValue& v = acc->byName[Lower(FromUtf8(curKey))];
                    SetField(&v.delayed, (int)d, &v.conflict);
                }
            } else if (tag == "element") {   // a configuration setting (xsd:element)
                std::string svc = HandlerService(get("handler"));
                std::string legacy = LowerA(Trim(get("legacyname"))), setting = LowerA(Trim(get("name")));
                // Owner's live run (MSDTC): an element named for its value ("DelayedAutostart") may have no legacyName.
                if (legacy.empty()) legacy = setting;
                if (svc.empty() || setting.empty() || (legacy != "start" && legacy != "delayedautostart")) return;
                const bool isDelayed = legacy == "delayedautostart";
                std::wstring w = Lower(FromUtf8(svc));
                acc->settings[FromUtf8(component + "|" + setting)] = {w, isDelayed};
                auto def = a.find("default");
                if (def == a.end()) return;
                long long d = Dword(def->second);
                SxsValue& v = acc->byName[w];
                if (!isDelayed && d >= 0 && d <= 4) { SetField(&v.setStart, (int)d, &v.conflict); mark(&v.fileSetting); }
                else if (isDelayed && (d == 0 || d == 1)) SetField(&v.setDelayed, (int)d, &v.conflict);
            }
        },
        [&](const std::string& tag) {
            if (tag == "registrykey") curKey.clear();
            else if (tag == "dependency" && depDepth > 0) depDepth--;
        },
        [](const std::string&) {});
}

int ApplyEditionXml(const std::string& x, SxsScan* acc, const std::wstring& file) {
    int applied = 0;
    std::string component, open, text;   // the open <component>'s name; its open child element and that one's text
    Tokenize(x,
        [&](const std::string& tag, const Attrs& a) {
            if (tag == "component") {
                auto it = a.find("name");
                component = it == a.end() ? std::string() : LowerA(Trim(it->second));
                open.clear();
            } else if (!component.empty()) {
                open = tag;
                text.clear();
            }
        },
        [&](const std::string& tag) {
            if (tag == "component") { component.clear(); open.clear(); return; }
            if (component.empty() || open.empty() || tag != open) return;
            auto it = acc->settings.find(FromUtf8(component + "|" + open));
            open.clear();
            if (it == acc->settings.end()) return;
            long long d = Dword(text);
            SxsValue& v = acc->byName[it->second.first];
            if (!it->second.second && d >= 0 && d <= 4) {
                SetField(&v.edStart, (int)d, &v.conflict);
                if (v.fileEdition.empty()) v.fileEdition = file;
                applied++;
            }
            else if (it->second.second && (d == 0 || d == 1)) { SetField(&v.edDelayed, (int)d, &v.conflict); applied++; }
        },
        [&](const std::string& t) { if (!open.empty()) text += Entities(t); });
    return applied;
}

// ---- scan ---------------------------------------------------------------------------------------
SxsStock ScanSxs(SxsReader& r, const std::function<bool()>& cancelled) {
    SxsStock s;
    s.build = r.BuildUbr();
    s.edition = r.Edition();
    s.fixture = r.IsFixture();
    std::vector<std::wstring> names;
    if (!r.List(&names, &s.truncated, &s.err)) return s;
    SxsScan acc;
    for (auto& nm : names) {
        if (cancelled && cancelled()) { s.err = L"Reading the component manifests was cancelled."; return s; }
        std::string xml;
        bool skipped = false;
        std::wstring e;
        if (!r.ReadXml(nm, &xml, &skipped, &e)) { (skipped ? s.skipped : s.failed)++; continue; }
        s.manifests++;
        ScanManifestXml(xml, &acc, nm);
    }
    if (s.manifests == 0) {
        s.err = L"No component manifest could be read (" + std::to_wstring(s.failed) + L" failed, " +
                std::to_wstring(s.skipped) + L" skipped).";
        return s;
    }
    std::string ed;
    std::wstring e;
    if (r.ReadEdition(&ed, &e)) {
        s.editionRead = true;
        s.overrides = ApplyEditionXml(ed, &acc, s.edition + L"Edition.xml");
    } else {
        // Review F3: every service a mapped setting targets could be overridden there: shown, not applied.
        s.editionErr = s.edition.empty() ? L"this PC's EditionID could not be read" : e.empty() ? L"unknown error" : e;
        for (auto& kv : acc.settings) acc.byName[kv.second.first].editionUnread = true;
    }
    s.byName = std::move(acc.byName);
    s.ok = true;
    return s;
}

std::string SxsDumpTsv(const SxsStock& s, unsigned long long elapsedMs) {
    std::string o = "# Hadron --dump-stock-live (read only: no SCM, registry or data-folder writes)\n";
    o += "# build.UBR\t" + ToUtf8(s.build) + "\n# EditionID\t" + ToUtf8(s.edition) + "\n# edition file read\t" +
         (s.editionRead ? "yes" : "no") + "\n# edition overrides\t" + std::to_string(s.overrides) + "\n# manifests read\t" +
         std::to_string(s.manifests) + "\n# decompress/read failures\t" + std::to_string(s.failed) +
         "\n# skipped (reparse, recall or offline)\t" + std::to_string(s.skipped) + "\n# elapsed ms\t" +
         std::to_string(elapsedMs) + "\n";
    if (!s.ok) o += "# error\t" + ToUtf8(s.err) + "\n";
    o += "name\tstart\tdelayed\tlayer\tconflict\tsource\n";
    for (auto& kv : s.byName) {
        const SxsValue& v = kv.second;
        const char* layer = "";
        switch (v.StartSrc()) {
            case SxsValue::EDITION: layer = "edition"; break;
            case SxsValue::SERVICE: layer = "serviceData"; break;
            case SxsValue::REGISTRY: layer = "registry"; break;
            case SxsValue::SETTING: layer = "settingDefault"; break;
            case SxsValue::NONE: layer = "none"; break;
        }
        StartCfg c = v.Cfg();
        // Owner's live run: conflict only when files disagree (a DelayedAutostart-only name has no start, not a conflict);
        // delayed as the files give it, "-" when none does.
        const bool conflict = v.conflict || (v.svcStart >= 0 && v.regStart >= 0 && v.svcStart != v.regStart);
        const int d = v.RawDelayed();
        o += ToUtf8(kv.first) + "\t" + (v.StartSrc() == SxsValue::NONE ? std::string("-") : std::to_string(c.start)) + "\t" +
             (d >= 0 ? std::to_string(d) : std::string("-")) + "\t" + layer + "\t" + (conflict ? "1" : "0") + "\t" +
             ToUtf8(v.StartFile()) + "\n";
    }
    return o;
}

// ---- cache --------------------------------------------------------------------------------------
std::wstring SxsCacheName(const std::wstring& b, const std::wstring& ed) {
    if (b.empty() || b.size() > 32 || b.find(L'.') == std::wstring::npos || ed.empty() || ed.size() > 64) return L"";
    for (wchar_t ch : b) if (!(iswdigit(ch) || ch == L'.')) return L"";
    for (wchar_t ch : ed)
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))) return L"";
    return L"sxs-v" + std::to_wstring(kSxsScannerVersion) + L"-" + b + L"-" + ed + L".txt";
}

std::string WriteSxsCache(const SxsStock& s) {
    std::string o = "HXSVC\t1\tsxsstock\t" + std::to_string(kSxsScannerVersion) + "\n";
    o += "M\tbuild\t" + ToUtf8(s.build) + "\nM\tedition\t" + ToUtf8(s.edition) + "\n";
    o += "M\tmanifests\t" + std::to_string(s.manifests) + "\nM\tfailed\t" + std::to_string(s.failed) +
         "\nM\tskipped\t" + std::to_string(s.skipped) + "\nM\toverrides\t" + std::to_string(s.overrides) +
         "\nM\teditionread\t" + (s.editionRead ? "1" : "0") + "\n";
    for (auto& kv : s.byName) {
        const SxsValue& v = kv.second;
        o += "S\t" + ToUtf8(kv.first);
        for (int f : {v.svcStart, v.regStart, v.delayed, v.setStart, v.setDelayed, v.edStart, v.edDelayed})
            o += "\t" + std::to_string(f);
        o += std::string("\t") + (v.conflict ? "1" : "0") + "\n";
    }
    o += "E\t" + std::to_string(s.byName.size()) + "\n";
    return o;
}

bool ParseSxsCache(const std::string& utf8, SxsStock* out, std::wstring* err) {
    *out = SxsStock{};
    if (utf8.size() > kMaxFile) { *err = L"larger than 16 MB"; return false; }
    std::vector<std::wstring> lines = Split(FromUtf8(utf8), L'\n');
    if (lines.empty() || lines[0].rfind(L"HXSVC\t1\tsxsstock", 0) != 0) { *err = L"not a manifest cache"; return false; }
    if (lines[0] != L"HXSVC\t1\tsxsstock\t" + std::to_wstring(kSxsScannerVersion)) {
        *err = L"written by another scanner version";
        return false;
    }
    bool end = false;
    auto num = [](const std::wstring& t, int lo, int hi, int* v) {
        if (t.empty() || t.size() > 10) return false;
        size_t k = 0;
        bool neg = t[0] == L'-';
        if (neg) k = 1;
        if (k >= t.size()) return false;
        long long n = 0;
        for (; k < t.size(); k++) {
            if (t[k] < L'0' || t[k] > L'9') return false;
            n = n * 10 + (t[k] - L'0');
        }
        if (neg) n = -n;
        if (n < lo || n > hi) return false;
        *v = (int)n;
        return true;
    };
    for (size_t i = 1; i < lines.size(); i++) {
        if (lines[i].empty()) continue;
        if (end) { *err = L"text after the end line"; return false; }
        std::vector<std::wstring> f = Split(lines[i], L'\t');
        if (f[0] == L"M" && f.size() == 3) {
            int v = 0;
            if (f[1] == L"build") out->build = f[2];
            else if (f[1] == L"edition") out->edition = f[2];
            else if (f[1] == L"manifests" && num(f[2], 0, 1 << 30, &v)) out->manifests = v;
            else if (f[1] == L"failed" && num(f[2], 0, 1 << 30, &v)) out->failed = v;
            else if (f[1] == L"skipped" && num(f[2], 0, 1 << 30, &v)) out->skipped = v;
            else if (f[1] == L"overrides" && num(f[2], 0, 1 << 30, &v)) out->overrides = v;
            else if (f[1] == L"editionread" && num(f[2], 0, 1, &v)) out->editionRead = v == 1;
        } else if (f[0] == L"S" && f.size() == 10 && !f[1].empty()) {
            SxsValue v;
            int c = 0;
            if (!num(f[2], -1, 4, &v.svcStart) || !num(f[3], -1, 4, &v.regStart) || !num(f[4], -1, 1, &v.delayed) ||
                !num(f[5], -1, 4, &v.setStart) || !num(f[6], -1, 1, &v.setDelayed) || !num(f[7], -1, 4, &v.edStart) ||
                !num(f[8], -1, 1, &v.edDelayed) || !num(f[9], 0, 1, &c)) {
                *err = L"bad row on line " + std::to_wstring(i + 1);
                return false;
            }
            v.conflict = c == 1;
            out->byName[Lower(f[1])] = v;
        } else if (f[0] == L"E" && f.size() == 2) {
            int v = 0;
            if (!num(f[1], 0, 1 << 30, &v) || (size_t)v != out->byName.size()) { *err = L"row count does not match"; return false; }
            end = true;
        } else {
            *err = L"unknown line " + std::to_wstring(i + 1);
            return false;
        }
    }
    if (!end) { *err = L"no end line (an incomplete write)"; return false; }
    if (out->manifests == 0) { *err = L"no manifests recorded"; return false; }
    out->ok = true;
    out->fromCache = true;
    return true;
}

// ---- readers ------------------------------------------------------------------------------------
namespace {

bool ReadSmallFile(const std::wstring& path, std::string* out, bool* skipped, std::wstring* err) {
    *skipped = false;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) { *err = sys::ErrorText(GetLastError()); return false; }
    if (!CompanyAttrsAllowed(a)) { *skipped = true; *err = L"reparse point, recall or offline attribute"; return false; }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = sys::ErrorText(GetLastError()); return false; }
    BY_HANDLE_FILE_INFORMATION fi{};
    bool ok = false;
    if (!GetFileInformationByHandle(h, &fi)) *err = sys::ErrorText(GetLastError());
    else if (!CompanyAttrsAllowed(fi.dwFileAttributes)) { *skipped = true; *err = L"reparse point, recall or offline attribute"; }
    else if (fi.nFileSizeHigh || fi.nFileSizeLow > kMaxFile) *err = L"larger than 16 MB";
    else {
        out->resize(fi.nFileSizeLow);
        DWORD got = 0;
        if (fi.nFileSizeLow && !ReadFile(h, &(*out)[0], fi.nFileSizeLow, &got, nullptr)) *err = sys::ErrorText(GetLastError());
        else if (got != fi.nFileSizeLow) *err = L"short read";
        else ok = true;
    }
    CloseHandle(h);
    return ok;
}

// Review F4/F10: the FindFirstFileExW error is the caller's text; a listing that ends on anything but
// ERROR_NO_MORE_FILES is marked truncated.
bool ListFiles(const std::wstring& dir, const wchar_t* pattern, std::vector<std::wstring>* v, bool* truncated, std::wstring* err) {
    v->clear();
    *truncated = false;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\" + pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        *err = L"Could not list " + dir + L"\\" + pattern + L": " + sys::ErrorText(e);
        return false;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        v->push_back(fd.cFileName);
    } while (FindNextFileW(f, &fd));
    DWORD last = GetLastError();
    FindClose(f);
    if (last != ERROR_NO_MORE_FILES) *truncated = true;
    std::sort(v->begin(), v->end());
    if (v->empty()) { *err = L"No files match " + dir + L"\\" + pattern; return false; }
    return true;
}

class FakeSxsReader : public SxsReader {
public:
    explicit FakeSxsReader(std::wstring dir) : dir_(std::move(dir)) {}
    bool List(std::vector<std::wstring>* names, bool* truncated, std::wstring* err) override {
        return ListFiles(dir_, L"*.manifest", names, truncated, err);
    }
    bool IsFixture() const override { return true; }
    bool ReadXml(const std::wstring& name, std::string* xml, bool* skipped, std::wstring* err) override {
        return ReadSmallFile(dir_ + L"\\" + name, xml, skipped, err);
    }
    std::wstring BuildUbr() override { return Line(L"build.txt"); }
    std::wstring Edition() override { return Line(L"edition.txt"); }
    bool ReadEdition(std::string* xml, std::wstring* err) override {
        std::wstring ed = Edition();
        bool sk = false;
        if (ed.empty()) { *err = L"no edition.txt"; return false; }
        return ReadSmallFile(dir_ + L"\\" + ed + L"Edition.xml", xml, &sk, err);
    }
private:
    std::wstring Line(const wchar_t* file) {
        std::string t;
        bool sk = false;
        std::wstring e;
        if (!ReadSmallFile(dir_ + L"\\" + file, &t, &sk, &e)) return L"";
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
        return FromUtf8(t);
    }
    std::wstring dir_;
};

// ---- live: wcp.dll's dictionary + msdelta's ApplyDeltaB --------------------------------------------
// msdelta.h's types, declared here (MinGW's headers lack them).
struct DeltaInput { const void* start; SIZE_T size; BOOL editable; };
struct DeltaOutput { void* start; SIZE_T size; };
typedef BOOL(WINAPI* ApplyDeltaBFn)(long long flags, DeltaInput source, DeltaInput delta, DeltaOutput* target);
typedef BOOL(WINAPI* DeltaFreeFn)(void* mem);
// Review F8: the apply flags. The probe on the owner's PC (19045.2965) expanded all 18,883 manifests with 4, so that is
// what Hadron passes first. This toolchain ships no msdelta.h. As far as I know the SDK's header defines
// DELTA_APPLY_FLAG_ALLOW_PA19 as 0x00000001, not 4, so 4 is not called that here. A delta that fails with 4 is tried
// once more with DELTA_APPLY_FLAG_ALLOW_PA19 (1). The owner's live run (--dump-stock-live) confirms which works.
constexpr long long kDeltaApplyFlagsProbe = 4;          // the value the probe used; its SDK name is unconfirmed
constexpr long long kDeltaApplyFlagAllowPa19 = 1;       // DELTA_APPLY_FLAG_ALLOW_PA19 (msdelta.h, from memory)
constexpr SIZE_T kMaxManifestXml = 64u << 20;           // review F7: a larger expanded manifest is a failure

// "amd64_microsoft-windows-servicingstack_31bf3856ad364e35_10.0.19041.2905_none_..." -> {10,0,19041,2905}
std::vector<unsigned> FolderVersion(const std::wstring& name) {
    std::vector<std::wstring> parts = Split(name, L'_');
    std::vector<unsigned> v;
    if (parts.size() < 4) return v;
    for (auto& t : Split(parts[3], L'.')) {
        if (t.empty() || t.size() > 9 || !std::all_of(t.begin(), t.end(), iswdigit)) return {};
        v.push_back((unsigned)std::stoul(t));
    }
    return v;
}

class LiveSxsReader : public SxsReader {
public:
    ~LiveSxsReader() override {
        if (wcp_) FreeLibrary(wcp_);
        if (delta_) FreeLibrary(delta_);
    }
    bool List(std::vector<std::wstring>* names, bool* truncated, std::wstring* err) override {
        wchar_t w[MAX_PATH];
        UINT n = GetSystemWindowsDirectoryW(w, MAX_PATH);
        if (!n || n >= MAX_PATH) { *err = L"Could not find the Windows folder: " + sys::ErrorText(GetLastError()); return false; }
        sxs_ = std::wstring(w, n) + L"\\WinSxS";
        // msdelta.dll from System32 only.
        delta_ = LoadLibraryExW(L"msdelta.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!delta_) { *err = L"Could not load msdelta.dll: " + sys::ErrorText(GetLastError()); return false; }
        apply_ = reinterpret_cast<ApplyDeltaBFn>(reinterpret_cast<void*>(GetProcAddress(delta_, "ApplyDeltaB")));
        free_ = reinterpret_cast<DeltaFreeFn>(reinterpret_cast<void*>(GetProcAddress(delta_, "DeltaFree")));
        if (!apply_ || !free_) { *err = L"msdelta.dll has no ApplyDeltaB/DeltaFree: " + sys::ErrorText(GetLastError()); return false; }
        // The newest servicing-stack folder's wcp.dll, loaded as a data file only (no code runs).
#if defined(__aarch64__) || defined(_M_ARM64)
        const std::wstring arch = L"arm64";
#elif defined(_WIN64)
        const std::wstring arch = L"amd64";
#else
        const std::wstring arch = L"x86";
#endif
        std::wstring best;
        std::vector<unsigned> bestV;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((sxs_ + L"\\" + arch + L"_microsoft-windows-servicingstack_31bf3856ad364e35_*").c_str(),
                                    FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) { *err = L"No servicing-stack folder in WinSxS: " + sys::ErrorText(GetLastError()); return false; }
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            std::vector<unsigned> v = FolderVersion(fd.cFileName);
            if (!v.empty() && (best.empty() || v > bestV)) { best = fd.cFileName; bestV = v; }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        if (best.empty()) { *err = L"No servicing-stack folder in WinSxS has a version."; return false; }
        std::wstring wcp = sxs_ + L"\\" + best + L"\\wcp.dll";
        if (!CompanyAttrsAllowed(GetFileAttributesW(wcp.c_str()))) { *err = L"Can't read " + wcp + L" (missing, a link or offline)."; return false; }
        wcp_ = LoadLibraryExW(wcp.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!wcp_) { *err = L"Could not load " + wcp + L" as data: " + sys::ErrorText(GetLastError()); return false; }
        HRSRC r = FindResourceW(wcp_, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(614));
        HGLOBAL g = r ? LoadResource(wcp_, r) : nullptr;
        dict_ = g ? LockResource(g) : nullptr;
        dictSize_ = r ? SizeofResource(wcp_, r) : 0;
        if (!dict_ || !dictSize_) { *err = L"wcp.dll has no manifest dictionary (resource 614): " + sys::ErrorText(GetLastError()); return false; }
        return ListFiles(sxs_ + L"\\Manifests", L"*.manifest", names, truncated, err);
    }
    bool ReadXml(const std::wstring& name, std::string* xml, bool* skipped, std::wstring* err) override {
        std::string raw;
        if (!ReadSmallFile(sys::ToExtended(sxs_ + L"\\Manifests\\" + name), &raw, skipped, err)) return false;
        if (raw.size() < 4 || memcmp(raw.data(), "DCM\x01", 4) != 0) {
            if (raw.compare(0, 5, "<?xml") == 0 || raw.compare(0, 8, "\xEF\xBB\xBF<?xml") == 0) { *xml = std::move(raw); return true; }
            *err = L"not a DCM manifest";
            return false;
        }
        DeltaInput src{dict_, dictSize_, FALSE};
        DeltaInput del{raw.data() + 4, raw.size() - 4, FALSE};
        DeltaOutput o{nullptr, 0};
        if (!apply_(kDeltaApplyFlagsProbe, src, del, &o)) {
            DWORD e1 = GetLastError();
            o = DeltaOutput{nullptr, 0};
            if (!apply_(kDeltaApplyFlagAllowPa19, src, del, &o)) {
                *err = L"ApplyDeltaB: " + sys::ErrorText(e1) + L" (flags 4); " + sys::ErrorText(GetLastError()) + L" (flags 1)";
                return false;
            }
        }
        // Review F7: DeltaFree on every path out.
        struct Freer { DeltaFreeFn f; void* p; ~Freer() { if (p) f(p); } } guard{free_, o.start};
        if (o.size > kMaxManifestXml) { *err = L"expanded manifest is larger than 64 MB"; return false; }
        try {
            xml->assign(static_cast<const char*>(o.start), o.size);
        } catch (const std::exception&) {
            *err = L"out of memory for an expanded manifest";
            return false;
        }
        return true;
    }
    std::wstring BuildUbr() override {
        const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        wchar_t b[32] = {};
        DWORD cb = sizeof(b);
        DWORD ubr = 0, cu = sizeof(ubr);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, b, &cb) != ERROR_SUCCESS) return L"";
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &cu) != ERROR_SUCCESS) return L"";
        return std::wstring(b) + L"." + std::to_wstring(ubr);
    }
    std::wstring Edition() override {
        wchar_t b[64] = {};
        DWORD cb = sizeof(b);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"EditionID", RRF_RT_REG_SZ,
                         nullptr, b, &cb) != ERROR_SUCCESS)
            return L"";
        return b;
    }
    // %windir%\servicing\Editions\<EditionID>Edition.xml: plain XML, read with the same attribute checks.
    bool ReadEdition(std::string* xml, std::wstring* err) override {
        std::wstring ed = Edition();
        if (ed.empty()) { *err = L"no EditionID"; return false; }
        for (wchar_t ch : ed)
            if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9'))) {
                *err = L"EditionID has unexpected characters";
                return false;
            }
        wchar_t w[MAX_PATH];
        UINT n = GetSystemWindowsDirectoryW(w, MAX_PATH);
        if (!n || n >= MAX_PATH) { *err = sys::ErrorText(GetLastError()); return false; }
        bool sk = false;
        return ReadSmallFile(sys::ToExtended(std::wstring(w, n) + L"\\servicing\\Editions\\" + ed + L"Edition.xml"), xml, &sk, err);
    }
private:
    std::wstring sxs_;
    HMODULE delta_ = nullptr, wcp_ = nullptr;
    ApplyDeltaBFn apply_ = nullptr;
    DeltaFreeFn free_ = nullptr;
    const void* dict_ = nullptr;
    DWORD dictSize_ = 0;
};

}  // namespace

std::unique_ptr<SxsReader> MakeLiveSxsReader() { return std::make_unique<LiveSxsReader>(); }
std::unique_ptr<SxsReader> MakeFakeSxsReader(const std::wstring& dir) { return std::make_unique<FakeSxsReader>(dir); }

}  // namespace hx::services
