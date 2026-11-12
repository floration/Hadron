// modules/services/collect.cpp — the SCM enumeration plus the registry cross-check and the TrustedInstaller read pass.
// Reads only (collect.h). The TrustedInstaller token is reverted on every path out of the pass (RAII), and Hadron's
// own file writes refuse while it is on (ImpersonationFlag), as in RunApply.
#include "modules/services/collect.h"
#include "core/sys/sys.h"
#include <map>
#include <set>

namespace hx::services {

namespace {

struct Pass {
    std::vector<SvcItem> items;
    EnumStats scm;
    int regKeys = -1, links = 0, direct = 0, regOnly = 0;
    std::wstring regErr;
    bool regPartial = false;
};

SvcItem FromKey(const RegSvcKey& k, const std::wstring& why) {
    SvcItem i;
    i.name = k.name;
    i.src = SvcItem::Source::Registry;
    i.regLink = k.link;
    i.regWhy = why;
    i.regNoType = k.type < 0;
    i.type = k.type < 0 ? 0 : (DWORD)k.type;
    i.kind = KindOf(i.type);
    i.regStart = k.start;
    i.startOverride = k.startOverride;
    i.cfg.start = k.start >= 0 && k.start <= 4 ? k.start : 3;   // a placeholder: cfgUnknown keeps it out of every plan
    i.cfgUnknown = true;
    i.state = 0;   // StateName(0): "Registry only"
    i.image = k.image;
    i.display = k.display;
    i.group = k.group;
    if (!k.err.empty()) i.readError = L"the key could not be opened: " + k.err;
    return i;
}

const wchar_t kRegWhyTail[] =
    L" Hadron shows it read-only: it is never planned, written, recorded in a snapshot or restored. Why a key can sit "
    L"outside the SCM (reasoning, from how the SCM works, not checked on this PC): the SCM builds its service list from this "
    L"key at boot, so a key written since the last restart, a key whose values the SCM rejected, or a leftover of an "
    L"uninstall stays outside it until the next restart, if not for good.";

bool RunPass(Backend& b, const std::atomic<bool>* cancel, Pass* p, std::wstring* err) {
    if (!b.Enumerate(&p->items, err)) return false;
    p->scm = b.lastEnum;
    std::vector<RegSvcKey> keys;
    std::wstring rerr;
    bool partial = false;
    if (!b.ListServiceKeys(&keys, &partial, &rerr)) { p->regErr = rerr; return true; }
    p->regKeys = (int)keys.size();
    if (partial) { p->regPartial = true; p->regErr = rerr; }
    std::map<std::wstring, size_t> have;
    for (size_t n = 0; n < p->items.size(); n++) have[Lower(p->items[n].name)] = n;
    for (auto& k : keys) {
        if (cancel && cancel->load()) break;
        if (k.link) p->links++;
        if (auto it = have.find(Lower(k.name)); it != have.end()) {
            // The key's StartOverride, read with the key (read only), and with it the key's raw Start (the same value
            // the item's own read takes on a live PC).
            SvcItem& i = p->items[it->second];
            i.startOverride = k.startOverride;
            if (!k.startOverride.empty() && k.start >= 0) i.regStart = k.start;
            continue;
        }
        have[Lower(k.name)] = p->items.size();
        if (k.link) {
            p->items.push_back(FromKey(k, std::wstring(L"This Services key is a registry link: Hadron treats a link as a leaf "
                                                       L"and never follows it or reads its values, and the SCM did not list it.") +
                                              kRegWhyTail));
            p->regOnly++;
            continue;
        }
        // The enumeration did not return it: ask the SCM for it by name before calling it registry-only.
        SvcItem x;
        std::wstring qe;
        if (b.Query(k.name, &x, &qe)) {
            x.src = SvcItem::Source::ScmDirect;
            p->items.push_back(std::move(x));
            p->direct++;
            continue;
        }
        p->items.push_back(FromKey(k, L"The SCM's list did not include this Services key, and opening it by name failed (" + qe + L")." +
                                          kRegWhyTail));
        p->regOnly++;
    }
    return true;
}

// Reverts the TrustedInstaller token and clears the file-write refusal on every path out of the pass.
struct TiScope {
    TiProvider* ti;
    bool on = false;
    ~TiScope() {
        if (on) ti->Revert();
        ImpersonationFlag::Active() = false;
    }
};

// Review R5: one guard puts everything back on every path out of the TrustedInstaller pass: Release (stops
// TrustedInstaller only if this pass started it), the control permission, and the claim's stop and end records.
struct TiRelease {
    Backend& b;
    TiProvider* ti;
    TiClaimLog* claim = nullptr;   // set once Begin succeeded
    std::wstring* note;
    ~TiRelease() {
        const bool mine = ti->StartedService();
        std::wstring n;
        const bool stopped = ti->Release(&n);
        b.AllowTiControl(false);
        // The provider's sentence names an apply; this was the list read.
        if (size_t at = n.find(L"for this apply"); at != std::wstring::npos) n.replace(at, 14, L"for the TrustedInstaller read");
        *note = n;
        if (claim) {
            if (mine) claim->Stopped(stopped, n);
            claim->End();
        }
    }
};

}  // namespace

bool CollectItems(Backend& b, TiProvider* ti, TiClaimLog* claim, const std::atomic<bool>* cancel, std::vector<SvcItem>* out,
                  CollectStats* st, std::wstring* err) {
    ULONGLONG t0 = GetTickCount64();
    *st = CollectStats{};
    Pass admin;
    if (!RunPass(b, cancel, &admin, err)) return false;
    st->scm = admin.scm;
    st->regKeys = admin.regKeys;
    st->regErr = admin.regErr;
    st->regPartial = admin.regPartial;
    st->regLinks = admin.links;
    st->direct = admin.direct;
    st->regOnly = admin.regOnly;
    *out = std::move(admin.items);

    if (ti && !(cancel && cancel->load())) {
        st->tiAsked = true;
        std::wstring qe;
        SvcItem tis;
        const bool running = b.Query(L"TrustedInstaller", &tis, &qe) && tis.state == SERVICE_RUNNING;
        std::wstring ce;
        if (!running && !claim) {
            st->tiErr = L"TrustedInstaller is not running, and this read never starts a service (start it first with: sc start "
                        L"TrustedInstaller)";
        } else if (!running && !claim->Begin(&ce)) {
            st->tiErr = L"TrustedInstaller is not running, and its start could not be recorded first, so it was not started: " + ce;
        } else {
            TiRelease rel{b, ti, running ? nullptr : claim, &st->tiNote};
            // Only a start the claim covers: with TrustedInstaller running, control stays refused.
            if (!running) b.AllowTiControl(true);
            std::wstring e;
            bool acquired = ti->Acquire(cancel, &e);
            if (!running) {   // R4: the process Hadron started, by pid and creation time, flushed before the read
                std::wstring we;
                bool recorded = true;
                if (ti->StartedService()) {
                    SvcItem now;
                    std::wstring qe2;
                    DWORD pid = 0;
                    uint64_t ps = 0;
                    if (b.Query(L"TrustedInstaller", &now, &qe2)) { pid = now.pid; ps = now.pstart; }
                    recorded = claim->Started(pid, ps, &we);
                } else {
                    recorded = claim->NotStarted(&we);
                }
                if (!recorded && acquired) { acquired = false; e = L"the TrustedInstaller start could not be recorded: " + we; }
            }
            if (!acquired) {
                st->tiErr = e;
            } else {
                Pass tp;
                std::wstring te, ident;
                bool ran = false, ok = false;
                {
                    TiScope scope{ti};
                    ImpersonationFlag::Active() = true;   // every Hadron file write refuses until the scope ends
                    if (ti->Impersonate(&ident, &te)) {
                        scope.on = true;
                        ran = true;
                        ok = RunPass(b, cancel, &tp, &te);
                    }
                }
                if (!ran) st->tiErr = te;
                else if (!ok) st->tiErr = L"the TrustedInstaller read failed: " + te;
                else {
                    st->tiUsed = true;
                    std::map<std::wstring, size_t> at;
                    for (size_t k = 0; k < out->size(); k++) at[Lower((*out)[k].name)] = k;
                    for (auto& t : tp.items) {
                        auto it = at.find(Lower(t.name));
                        if (it == at.end()) {
                            if (t.RegOnly()) {   // the administrator's pass would have listed the key too: keep it as is
                                at[Lower(t.name)] = out->size();
                                out->push_back(t);
                                continue;
                            }
                            t.viaTi = true;
                            st->tiItems++;
                            st->tiNames.push_back(t.name);
                            at[Lower(t.name)] = out->size();
                            out->push_back(t);
                            continue;
                        }
                        SvcItem& a = (*out)[it->second];
                        if (t.RegOnly()) continue;
                        if (a.RegOnly()) {   // the SCM opened it only for TrustedInstaller
                            t.viaTi = true;
                            st->tiItems++;
                            st->tiNames.push_back(t.name);
                            a = t;
                        } else if ((a.cfgUnknown && !t.cfgUnknown) || (!a.readError.empty() && t.readError.empty())) {
                            t.viaTi = true;
                            if (a.src == SvcItem::Source::ScmDirect) t.src = a.src;
                            st->tiConfigs++;
                            st->tiNames.push_back(t.name);
                            a = t;
                        }
                    }
                }
            }
        }   // TiRelease: Release, control off, the claim's stop and end records
        // TrustedInstaller's own row as it is now (stopped again, or the process left running for the banner).
        SvcItem now;
        std::wstring qe3;
        if (b.Query(L"TrustedInstaller", &now, &qe3))
            for (auto& i : *out)
                if (IEq(i.name, L"TrustedInstaller") && !i.RegOnly()) { now.src = i.src; now.viaTi = i.viaTi; now.startOverride = i.startOverride; i = now; }
    }
    for (auto& i : *out) if (i.StartOverridden()) st->overridden++;
    st->ms = GetTickCount64() - t0;
    return true;
}

std::wstring TiCollectText(const CollectStats& st) {
    if (!st.tiAsked) return L"";
    if (!st.tiUsed)
        return L"TrustedInstaller read did not run (" + st.tiErr + L"): the list is what an administrator sees.";
    std::wstring t = L"TrustedInstaller showed " + std::to_wstring(st.tiItems) + L" more item" + (st.tiItems == 1 ? L"" : L"s") +
                     L" and " + std::to_wstring(st.tiConfigs) + L" more config" + (st.tiConfigs == 1 ? L"" : L"s") +
                     L" than administrator";
    if (st.tiItems + st.tiConfigs == 0) t += L" (TrustedInstaller saw nothing an administrator didn't)";
    t += L".";
    if (!st.tiNote.empty()) t += L" " + st.tiNote;
    return t;
}

std::wstring VisibilityText(const CollectStats& st) {
    std::wstring t;
    if (st.regOnly) t += std::to_wstring(st.regOnly) + L" from the registry only (not loaded by the SCM). ";
    if (st.direct) t += std::to_wstring(st.direct) + L" missing from the SCM's list, read from it by name. ";
    if (st.regKeys < 0) t += L"The Services key could not be listed (" + st.regErr + L"), so registry-only keys are not shown. ";
    else if (st.regPartial)
        t += L"The Services key list stopped at " + std::to_wstring(st.regKeys) + L": " + st.regErr + L"; keys after that are not cross-checked. ";
    if (std::wstring ti = TiCollectText(st); !ti.empty()) t += ti + L" ";
    return t;
}

namespace {
std::wstring StartText(const SvcItem& s) {
    if (s.RegOnly()) return s.regStart >= 0 ? StartName(s.regStart) : L"none";
    if (s.cfgUnknown) return L"unknown";
    return s.cfg.Delayed() ? L"Auto (delayed)" : StartName(s.cfg.start);
}
std::wstring KindText(const SvcItem& s) { return s.RegOnly() ? std::wstring(kRegOnlyKind) : TypeName(s.type); }
std::wstring Tsv(const std::wstring& s) { return Esc(s); }
std::wstring OverrideText(const SvcItem& s) {   // "0=3;1=3;2=3", "-" without a StartOverride subkey
    std::wstring t;
    for (auto& kv : s.startOverride) t += (t.empty() ? L"" : L";") + Esc(kv.first) + L"=" + std::to_wstring(kv.second);
    return t.empty() ? L"-" : t;
}
}  // namespace

std::string DumpListTsv(const std::vector<SvcItem>& items, const CollectStats& st, const std::wstring& build) {
    std::map<std::wstring, int> byTab, byKind, byStart, bySrc;
    for (auto& s : items) {
        byTab[OnTab(s, TAB_DRIVERS) ? L"drivers" : L"services"]++;
        byKind[KindText(s)]++;
        byStart[StartText(s)]++;
        bySrc[SourceName(s)]++;
    }
    auto counts = [](const std::map<std::wstring, int>& m) {
        std::wstring t;
        for (auto& kv : m) t += (t.empty() ? L"" : L"; ") + kv.first + L"=" + std::to_wstring(kv.second);
        return t;
    };
    wchar_t mask[16];
    swprintf(mask, 16, L"0x%X", (unsigned)st.scm.mask);
    std::wstring o = L"# Hadron --dump-list-live (read only: nothing armed, no SCM, registry or data-folder writes)\n";
    o += L"#build\t" + build + L"\n";
    o += L"#elapsed_ms\t" + std::to_wstring(st.ms) + L"\n";
    o += L"#enum_mask\t" + std::wstring(mask) + L"\n";
    o += L"#enum_masks_tried\t" + Tsv(st.scm.masksTried) + L"\n";
    o += L"#scm_raw\t" + std::to_wstring(st.scm.raw) + L" (drivers " + std::to_wstring(st.scm.rawDrivers) + L", pages " +
         std::to_wstring(st.scm.pages) + L", duplicates dropped " + std::to_wstring(st.scm.duplicates) + L", config unreadable " +
         std::to_wstring(st.scm.unreadable) + L")\n";
    o += L"#registry_keys\t" +
         (st.regKeys < 0 ? L"not listed: " + Tsv(st.regErr)
                         : std::to_wstring(st.regKeys) + (st.regPartial ? L" (the Services key list stopped at " + std::to_wstring(st.regKeys) +
                                                                              L": " + Tsv(st.regErr) + L")"
                                                                        : L"")) +
         L" (links " + std::to_wstring(st.regLinks) + L")\n";
    o += L"#enum_missed_read_by_name\t" + std::to_wstring(st.direct) + L"\n";
    o += L"#registry_only\t" + std::to_wstring(st.regOnly) + L"\n";
    o += L"#start_overridden\t" + std::to_wstring(st.overridden) +
         L" (registry Start differs from the SCM's start type and a StartOverride value is present)\n";
    o += L"#per_user_instances_folded\t" + std::to_wstring(st.folded) + L"\n";
    o += L"#ti\t" + (!st.tiAsked ? std::wstring(L"off") : !st.tiUsed ? L"failed: " + Tsv(st.tiErr)
                                                                     : L"items+" + std::to_wstring(st.tiItems) + L" configs+" +
                                                                           std::to_wstring(st.tiConfigs) +
                                                                           (st.tiNames.empty() ? L"" : L" (" + Tsv(Join(st.tiNames, L", ")) + L")")) +
         L"\n";
    o += L"#rows\t" + std::to_wstring(items.size()) + L"\n";
    o += L"#by_tab\t" + counts(byTab) + L"\n";
    o += L"#by_kind\t" + counts(byKind) + L"\n";
    o += L"#by_start\t" + counts(byStart) + L"\n";
    o += L"#by_source\t" + counts(bySrc) + L"\n";
    o += L"name\ttab\tkind\ttype\tstart\tstate\tsource\treadError\traw_start\tstart_override\n";
    for (auto& s : items) {
        wchar_t ty[16];
        swprintf(ty, 16, L"0x%X", (unsigned)s.type);
        std::wstring re = s.readError;
        if (s.RegOnly()) re = (re.empty() ? L"" : re + L"; ") + s.regWhy;
        o += Tsv(s.name) + L"\t" + (OnTab(s, TAB_DRIVERS) ? L"drivers" : L"services") + L"\t" + Tsv(KindText(s)) + L"\t" + ty +
             L"\t" + StartText(s) + L"\t" + Tsv(StateName(s.state)) + L"\t" + SourceName(s) + (s.viaTi ? L"+ti" : L"") + L"\t" +
             Tsv(re) + L"\t" + (s.regStart >= 0 ? std::to_wstring(s.regStart) : std::wstring(L"-")) + L"\t" + OverrideText(s) + L"\n";
    }
    return ToUtf8(o);
}

}  // namespace hx::services
