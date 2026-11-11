// modules/services/selftest_evidence.cpp — selftest for svcdrv follow-ups 6-9 (selftest_evidence.h).
//
// Signer states and labels from the fixture's signers.txt and the attribute rule; event 7040 parsing from fixture XML;
// device-interface and arrival evidence that may only keep more; and the registry fallback end to end on the fake
// backend: off by default, its plan line, the undo entry on disk before the write, a drift abort, a read-back mismatch,
// refusals, the pre-apply snapshot and the Undo round trip. The only live call is an unarmed WriteRegStart, which is
// refused before the registry is opened.
#include "modules/services/selftest_evidence.h"
#include "core/sys/sys.h"
#include "modules/services/cli.h"
#include "modules/services/evidence.h"
#include "modules/services/hw.h"
#include "modules/services/plan.h"
#include "modules/services/regfallback.h"
#include "modules/services/store.h"

#include <random>

namespace hx::services {
namespace {

struct EvWorld {
    FixtureData fx;
    std::vector<SvcItem> items;
    HwInfo hw;
    Ctx ctx;
};

bool Load(const std::wstring& dir, const Kb& kb, EvWorld* w, std::wstring* err) {
    if (!LoadFixtureDir(dir, &w->fx, err)) return false;
    w->items.clear();
    if (!w->fx.backend.Enumerate(&w->items, err)) return false;
    FoldInstances(&w->items);
    w->fx.backend.ReadHw(&w->hw);
    w->ctx = Ctx{};
    w->ctx.kb = &kb;
    w->ctx.hw = w->hw.ok ? &w->hw : nullptr;
    w->ctx.first = w->fx.hasFirst ? &w->fx.first : nullptr;
    w->ctx.build = w->fx.backend.Build();
    w->ctx.machine = w->fx.backend.MachineGuid();
    w->ctx.Prepare();
    return true;
}

const PlanRow* Row(const Plan& p, const wchar_t* name) {
    for (auto& r : p.rows) if (IEq(r.name, name)) return &r;
    return nullptr;
}
const SvcItem* Item(const std::vector<SvcItem>& v, const wchar_t* name) {
    for (auto& i : v) if (IEq(i.name, name)) return &i;
    return nullptr;
}

// The Stripped services plan with both markings, as the page builds it (MarkRegFallback, then MarkTi).
Plan StrippedPlan(EvWorld& w, bool regOn, bool tiOn) {
    Plan p = BuildPreset(LV_STRIPPED, TAB_SERVICES, w.items, w.ctx);
    MarkRegFallback(&p, w.fx.backend, regOn, tiOn, w.items, w.ctx);
    MarkTi(&p, w.fx.backend, tiOn, w.items);
    return p;
}

// Only the named row, ticked: the user's choice on the plan screen.
Plan Only(const Plan& p, const wchar_t* name) {
    Plan q = p;
    q.rows.clear();
    for (auto& r : p.rows)
        if (IEq(r.name, name)) { q.rows.push_back(r); q.rows.back().include = true; }
    return q;
}

ApplyEnv Env(Backend* b, Store* s, const Ctx* c, bool regOn) {
    ApplyEnv e;
    e.backend = b;
    e.store = s;
    e.lockName = L"Local\\Hadron.services.selftest.ev." + std::to_wstring(GetCurrentProcessId());
    e.build = L"19045";
    e.machine = L"6f1c2a4e-fixture-0000-0000-000000000001";
    e.requireFirst = false;
    e.regFallbackOn = regOn;
    e.ctx = c;
    return e;
}

std::wstring Lines(const ApplyResult& r) { return Join(r.lines, L"; "); }

const UndoLog* Newest(const std::vector<UndoLog>& logs, const std::wstring& stamp) {
    for (auto& l : logs) if (l.stamp == stamp) return &l;
    return nullptr;
}

}  // namespace

void RunEvidenceSelftest(const std::wstring& fixture, const Kb& kb, const std::function<std::wstring(const std::wstring&)>& newTempDir,
                         const SelftestCheck& check, const SelftestInfo& info) {
    std::wstring err;

    // ---- follow-up 6: the Signer column ------------------------------------------------------------------------
    {
        EvWorld w;
        bool ok = Load(fixture, kb, &w, &err);
        check(ok, L"evidence: the fixture loads with signers.txt and events7040.txt" + (ok ? L"" : L": " + err));
        if (!ok) return;
        const auto& sg = w.fx.backend.Signers();
        auto lbl = [&](const wchar_t* n) { auto it = sg.find(Lower(n)); return it == sg.end() ? std::wstring(L"<none>") : SignerShort(it->second); };
        check(lbl(L"nvlddmkm") == L"Signed: NVIDIA Corporation" && lbl(L"DiagTrack") == L"Catalog-signed" &&
                  lbl(L"ExampleUpdater") == L"Unsigned" && lbl(L"ExampleAudioFlt").rfind(L"Not trusted: ", 0) == 0 &&
                  lbl(L"Ntfs").rfind(L"Can't check: ", 0) == 0,
              L"signer: the five states read from the fixture and their list labels (Signed: <CN>, Catalog-signed, Unsigned, "
              L"Not trusted: <why>, Can't check: <why>)");
        SignerInfo pend;
        check(SignerShort(pend).empty() && SignerLong(pend).empty(), L"signer: an item not read yet shows nothing (never a guess)");
        bool rt = true;
        for (SignState s : {SignState::Pending, SignState::Signed, SignState::Catalog, SignState::Unsigned, SignState::Untrusted,
                            SignState::CantCheck}) {
            SignState back;
            if (!SignStateFromName(SignStateName(s), &back) || back != s) rt = false;
        }
        SignState dummy;
        check(rt && !SignStateFromName(L"trusted", &dummy), L"signer: state names round-trip; an unknown name is refused");
        auto it = sg.find(L"diagtrack");
        check(it != sg.end() && SignerLong(it->second).find(L"offline") != std::wstring::npos &&
                  SignerLong(it->second).find(L"Microsoft Windows") != std::wstring::npos,
              L"signer: the detail line names the catalog signer and says the check is offline");
        check(!SignerAttrsProblem(FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_ARCHIVE).empty() &&
                  !SignerAttrsProblem(FILE_ATTRIBUTE_RECALL_ON_OPEN).empty() && !SignerAttrsProblem(FILE_ATTRIBUTE_OFFLINE).empty() &&
                  !SignerAttrsProblem(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_ARCHIVE).empty() &&
                  !SignerAttrsProblem(INVALID_FILE_ATTRIBUTES).empty() && SignerAttrsProblem(FILE_ATTRIBUTE_ARCHIVE).empty(),
              L"signer: never reads a cloud or offline file (RECALL_ON_DATA_ACCESS, RECALL_ON_OPEN, OFFLINE); a reparse point is a leaf");
        SignerInfo unc = ReadSigner(L"\\\\srv\\share\\svc.exe");
        SignerInfo uncx = ReadSigner(L"\\\\?\\UNC\\srv\\share\\svc.exe");
        check(unc.state == SignState::CantCheck && uncx.state == SignState::CantCheck,
              L"signer: a UNC path is never opened (Can't check: " + unc.detail + L")");
        std::string sbad = "HXSVC\t1\tsigners\nG\tx\tbogus\t\t\nG\ty\tunsigned\t\t\n";
        std::map<std::wstring, SignerInfo> parsed;
        check(ParseSigners(sbad, &parsed, &err) && parsed.size() == 1 && parsed.count(L"y") &&
                  !ParseSigners("HXSVC\t1\tsnapshot\n", &parsed, &err),
              L"signer fixture: an unknown state line is skipped; a wrong header is refused");
        // A file with no signature, made in a temp folder of this test: never Signed or Catalog.
        std::wstring dir = newTempDir(L"signer");
        if (!dir.empty()) {
            Store st(dir);
            std::wstring e2;
            bool made = st.WriteNew(L"unsigned.bin", std::string(4096, 'x'), &e2);
            SignerInfo u = made ? ReadSigner(Store::Plain(st.Path(L"unsigned.bin"))) : SignerInfo{};
            check(made && (u.state == SignState::Unsigned || u.state == SignState::CantCheck),
                  L"signer: a file with no signature is Unsigned (or Can't check where WinVerifyTrust is missing): " + SignerShort(u));
            SignerInfo u2 = made ? ReadSigner(Store::Plain(st.Path(L"unsigned.bin"))) : SignerInfo{};
            check(u2.state == u.state && u2.detail == u.detail, L"signer: a second read of the same file comes from the cache unchanged");
            SignerInfo missing = ReadSigner(Store::Plain(st.Path(L"no-such-file.sys")));
            check(missing.state == SignState::CantCheck, L"signer: a missing file is Can't check: " + missing.detail);
        } else {
            info(L"signer: no temp folder for the unsigned-file check");
        }
    }

    // ---- follow-up 7: event 7040 -------------------------------------------------------------------------------
    {
        const std::wstring q1 =
            L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='Service Control Manager' "
            L"Guid='{555908d1-a6d7-4695-8e1e-26931d2012f4}' EventSourceName='Service Control Manager'/><EventID Qualifiers='16384'>7040</EventID>"
            L"<TimeCreated SystemTime='2026-11-08T21:14:09.5308412Z'/><EventRecordID>48211</EventRecordID><Channel>System</Channel>"
            L"<Security UserID='S-1-5-18'/></System><EventData><Data Name='param1'>Fax &amp; Scan &lt;x&gt;</Data>"
            L"<Data Name='param2'>demand start</Data><Data Name='param3'>disabled</Data><Data Name='param4'>Fax</Data></EventData></Event>";
        Ev7040 e;
        bool ok = ParseEvent7040(q1, &e, &err);
        check(ok && e.service == L"Fax" && e.display == L"Fax & Scan <x>" && e.from == L"demand start" && e.to == L"disabled" &&
                  e.sid == L"S-1-5-18" && e.record == 48211 && e.time == L"2026-11-08T21:14:09.5308412Z",
              L"7040: single-quoted event XML parses (service, display with entities, from, to, user SID, record, time)");
        std::wstring q2 = q1;
        for (auto& ch : q2) if (ch == L'\'') ch = L'"';
        Ev7040 e2;
        check(ParseEvent7040(q2, &e2, &err) && e2.service == L"Fax" && e2.sid == L"S-1-5-18", L"7040: the same event with double quotes parses");
        std::wstring other = q1;
        other.replace(other.find(L">7040<"), 6, L">7045<");
        std::wstring prov = q1;
        prov.replace(prov.find(L"Name='Service Control Manager'"), 30, L"Name='Some Other Provider'");
        Ev7040 x;
        std::wstring e3, e4;
        const bool refused = !ParseEvent7040(other, &x, &e3) && !ParseEvent7040(prov, &x, &e4) && !ParseEvent7040(L"<Event/>", &x, &err);
        check(refused,
              L"7040: another event id, another provider or a bare element is refused (" + e3 + L"; " + e4 + L")");
        check(XmlUnescape(L"a&amp;b&lt;c&gt;d&quot;e&apos;f&#65;&#x42;") == L"a&b<c>d\"e'fAB" && XmlUnescape(L"&bogus;") == L"&bogus;",
              L"7040: XML entities and character references unescape; an unknown one is left as written");
        check(EventTimeText(L"2026-11-08T21:14:09.5308412Z") == L"2026-11-08 21:14 UTC" && EventTimeText(L"junk") == L"junk",
              L"7040: TimeCreated shown as a UTC date and time");

        EvWorld w;
        Load(fixture, kb, &w, &err);
        std::map<std::wstring, std::wstring> names{{L"diagtrack", L"Connected User Experiences and Telemetry"},
                                                   {L"xblgamesave", L"Xbox Live Game Save"}, {L"wsearch", L"Windows Search"}};
        auto m = NewestPerService(w.fx.backend.Events(), names);
        auto d = m.find(L"diagtrack");
        auto xb = m.find(L"xblgamesave");
        check(w.fx.backend.Events().size() == 4 && m.size() == 2 && d != m.end() && d->second.record == 48211 &&
                  d->second.account == L"DESKTOP-FIXTURE\\user" && xb != m.end() && xb->second.service.empty() &&
                  xb->second.account == L"NT AUTHORITY\\SYSTEM" && !m.count(L"wsearch") && !m.count(L"spooler"),
              L"7040: the newest event per item wins; an event without param4 matches by display name; an item not asked "
              L"for is ignored; the fixture's SIDs resolve to accounts");
        check(Ev7040Text(nullptr) == L"no 7040 event found (log may have rolled over)",
              L"7040: an item with no event says \"no 7040 event found (log may have rolled over)\"");
        std::wstring t = d != m.end() ? Ev7040Text(&d->second) : L"";
        check(t.find(L"DESKTOP-FIXTURE\\user (S-1-5-21-") != std::wstring::npos && t.find(L"2026-11-08 21:14 UTC") != std::wstring::npos &&
                  t.find(L"from demand start to disabled") != std::wstring::npos,
              L"7040: the detail line says who (account and SID), when and from/to: " + t);
        Ev7040 nos;
        nos.time = L"2026-01-01T00:00:00Z";
        nos.sid = L"S-1-5-21-1-2-3-500";
        check(Ev7040Text(&nos).find(L"account not resolved") != std::wstring::npos, L"7040: an unresolved SID is shown as the SID, and says so");
        std::vector<Ev7040> evs;
        check(!ParseEventsFixture("HXSVC\t1\tevents\nE\t<Event/>\n", &evs, &err) && !ParseEventsFixture("nope\n", &evs, &err),
              L"7040 fixture: a broken event line or a wrong header is refused");
        std::vector<Ev7040> none;
        check(ReadLive7040({}, nullptr, &none, &err) && none.empty(), L"7040 live: nothing asked, nothing read (the log is not opened)");
    }

    // ---- follow-up 8: interface triggers and arrival dates only ever keep more ---------------------------------
    {
        EvWorld w;
        Load(fixture, kb, &w, &err);
        check(FileTimeDate(0).empty() && FileTimeDate(0x01DCAA7AF5BFEC00ull) == L"2026-03-02",
              L"hw: a last-arrival FILETIME shows as a date; 0 shows nothing");
        std::string raw;
        Store fs(fixture);
        HwInfo again;
        bool rt = fs.Read(L"hw.txt", &raw, &err) && ParseHw(raw, &again, &err) && DumpHw(again) == raw;
        check(rt && !w.hw.ifaceDevs.empty(), L"hw dump: F lines and the arrival field round-trip byte for byte");
        const SvcItem* frame = Item(w.items, L"FrameServer");
        const SvcItem* scard = Item(w.items, L"SCardSvr");
        std::wstring ev0, ev1, ev2;
        int base = frame ? HwEvidence(w.hw, L"class:Camera", frame->name, &ev0) : -9;
        int ex = frame ? HwEvidenceEx(w.hw, L"class:Camera", *frame, &ev1) : -9;
        check(base == 2 && ex == 0 && ev1.find(L"device-interface trigger") != std::wstring::npos &&
                  ev1.find(L"last arrived 2026-03-02") != std::wstring::npos && ev1.find(L"by the hint alone") != std::wstring::npos,
              L"hw: a webcam under the Image class, seen before, keeps FrameServer through its camera interface trigger "
              L"(the Camera class hint alone would offer it): " + ev1);
        check(scard && IfaceEvidence(w.hw, *scard, &ev2) == -1, L"hw: an item without interface triggers gets no interface evidence");
        Plan st = BuildPreset(LV_STRIPPED, TAB_SERVICES, w.items, w.ctx);
        const PlanRow* fr = Row(st, L"FrameServer");
        check(fr && !fr->blocked.empty() && !fr->include && fr->blocked.find(L"device-interface trigger") != std::wstring::npos,
              L"Stripped: FrameServer is kept, with the interface evidence on the row");
        // Randomized: whatever the devices, triggers and hints, the extended result is the hint's own or "kept".
        std::mt19937 rng(20261111u);
        const wchar_t* hints[] = {L"class:Camera", L"class:Image", L"class:Bluetooth", L"service:usbscan", L"service:nosuch",
                                  L"class:SmartCardReader", L"hypervisor", L"vm", L"bogus:x"};
        const wchar_t* guids[] = {L"{e5323777-f976-4f5b-9b55-b94699c46e44}", L"{0850302a-b344-4fda-9be9-90576b8d46f0}",
                                  L"{50dd5230-ba8a-11d1-bf5d-0000f805f530}"};
        bool only = true;
        std::wstring bad;
        for (int k = 0; k < 3000 && only; k++) {
            HwInfo h = w.hw;
            for (auto& n : h.nodes) {
                if (rng() % 3 == 0) n.present = !n.present;
                n.arrival = rng() % 2 ? 0 : 0x01DC000000000000ull + rng();
            }
            h.ifaceDevs.clear();
            h.presentIfaces.clear();
            for (auto* g : guids) {
                if (rng() % 2) h.ifaceDevs[g].push_back(h.nodes[rng() % h.nodes.size()].id);
                if (rng() % 4 == 0) h.ifaceDevs[g].push_back(L"USB\\VID_0000&PID_0000\\GONE");
                if (rng() % 5 == 0) h.presentIfaces.insert(g);
            }
            h.hypervisor = rng() % 2;
            h.isVm = rng() % 2;
            SvcItem it;
            it.name = L"RandomSvc";
            for (auto* g : guids) if (rng() % 2) it.ifaces.push_back(g);
            const wchar_t* hint = hints[rng() % (sizeof(hints) / sizeof(hints[0]))];
            std::wstring e1, e2;
            int b0 = HwEvidence(h, hint, it.name, &e1);
            int b1 = HwEvidenceEx(h, hint, it, &e2);
            if (b1 != b0 && b1 != 0) { only = false; bad = std::wstring(hint) + L": " + std::to_wstring(b0) + L" -> " + std::to_wstring(b1); }
            if (b0 == 0 && b1 != 0) { only = false; bad = std::wstring(hint) + L": a keep was loosened"; }
        }
        check(only, L"hw: 3000 random device pictures: interface and arrival evidence never turn a keep into a removal, and "
                    L"never change a removal into another removal kind" + (bad.empty() ? L"" : L" (" + bad + L")"));
        // Whole plans: with the interface data removed, every row the real plan ticks is ticked too (tighten only).
        EvWorld bare;
        Load(fixture, kb, &bare, &err);
        bare.hw.ifaceDevs.clear();
        for (auto& i : bare.items) i.ifaces.clear();
        bare.ctx.Prepare();
        bool tighter = true;
        std::wstring which;
        for (int tab : {TAB_SERVICES, TAB_DRIVERS})
            for (int lv : {LV_BLOAT, LV_STRIPPED, LV_MINIMUM}) {
                Plan full = BuildPreset(lv, tab, w.items, w.ctx);
                Plan noIf = BuildPreset(lv, tab, bare.items, bare.ctx);
                for (auto& r : full.rows) {
                    if (!r.include) continue;
                    const PlanRow* b = Row(noIf, r.name.c_str());
                    if (!b || !b->include) { tighter = false; which = r.name; }
                }
            }
        check(tighter, L"hw: every preset, both tabs: the interface evidence only removes ticks, never adds one" +
                           (which.empty() ? L"" : L" (" + which + L")"));
    }

    // ---- follow-up 9: the registry fallback, on the fake backend only ------------------------------------------
    {
        EvWorld w;
        Load(fixture, kb, &w, &err);
        check(!ApplyEnv{}.regFallbackOn, L"registry fallback: off by default in every apply environment");
        Plan off = StrippedPlan(w, false, false);
        const PlanRow* d0 = Row(off, L"dmwappushservice");
        bool anyReg = false;
        for (auto& r : off.rows) if (r.viaReg) anyReg = true;
        check(!anyReg && d0 && d0->needsTi && DumpPlan(off).find("registry:") == std::string::npos,
              L"registry fallback: with the toggle off no row uses it; the SCM-refused row keeps its TrustedInstaller warning");
        Plan on = StrippedPlan(w, true, false);
        const PlanRow* d1 = Row(on, L"dmwappushservice");
        std::string dump = DumpPlan(on);
        check(d1 && d1->viaReg && !d1->needsTi && d1->regNote == kRegLine &&
                  dump.find("    registry: SCM refused; will write registry Start directly, takes effect after restart\n") != std::string::npos,
              L"registry fallback: toggle on: the SCM-refused row gets its own plan line, \"SCM refused; will write registry Start "
              L"directly, takes effect after restart\"");
        int marked = 0;
        for (auto& r : on.rows) if (r.viaReg) marked++;
        check(marked == 1 && !on.notes.empty() && on.notes[0].find(L"only after you press Apply") != std::wstring::npos,
              L"registry fallback: only rows the SCM refuses are marked, and the plan says nothing is written before Apply");
        Plan ti = StrippedPlan(w, true, true);
        const PlanRow* d2 = Row(ti, L"dmwappushservice");
        check(d2 && !d2->viaReg && d2->needsTi, L"registry fallback: with the TrustedInstaller toggle on, the SCM path is used instead");
        check(w.fx.backend.faults.regWrites == 0, L"registry fallback: building plans writes nothing");

        // Refusals: launch-protected, never-touch, boot path, Boot/System-start drivers, the named never-preset set.
        auto refusal = [&](const wchar_t* name, int start = -2) {
            const SvcItem* it = Item(w.items, name);
            if (!it) return std::wstring(L"<missing item>");
            SvcItem s = *it;
            if (start != -2) s.cfg.start = start;
            RegStartVals rv;
            rv.start = s.cfg.start;
            rv.delayed = s.cfg.delayed;
            return RegRowRefusal(s, rv, s.cfg, StartCfg{4, -1}, &w.ctx);
        };
        std::wstring rWin = refusal(L"WinDefend"), rRpc = refusal(L"RpcSs"), rNvme = refusal(L"stornvme", 3), rSmb = refusal(L"mssmbios"),
                     rVss = refusal(L"VSS"), rDmw = refusal(L"dmwappushservice");
        check(!rWin.empty() && !rRpc.empty() && !rNvme.empty() && !rSmb.empty() && !rVss.empty() && rDmw.empty(),
              L"registry fallback refuses launch-protected (" + rWin + L"), never-touch (" + rRpc + L"), the boot path (" + rNvme +
                  L"), a System-start driver (" + rSmb + L") and the named never-preset set (" + rVss + L"); an ordinary service passes");
        {
            const SvcItem* dm = Item(w.items, L"dmwappushservice");
            SvcItem s = *dm;
            RegStartVals pend{4, -1};
            RegStartVals none{-1, -1};
            check(!RegRowRefusal(s, pend, s.cfg, StartCfg{4, -1}, &w.ctx).empty() && !RegRowRefusal(s, none, s.cfg, StartCfg{4, -1}, &w.ctx).empty() &&
                      !RegRowRefusal(s, RegStartVals{3, -1}, s.cfg, StartCfg{1, -1}, &w.ctx).empty(),
                  L"registry fallback refuses a change already waiting for a restart, a key with no Start value and a Boot/System target");
        }
        std::wstring fe;
        WriteOp w4{4, -1};
        bool wp = w.fx.backend.WriteRegStart(L"WinDefend", w4, &fe);
        bool wb = w.fx.backend.WriteRegStart(L"stornvme", w4, &fe);
        bool wt = w.fx.backend.WriteRegStart(L"dmwappushservice", WriteOp{1, -1}, &fe);
        check(!wp && !wb && !wt && w.fx.backend.faults.regWrites == 0,
              L"registry fallback (backend): a protected service, a Boot-start driver and a System target are refused before any write");
        {
            std::unique_ptr<Backend> live = MakeLiveBackend();
            std::wstring le;
            bool lw = live->WriteRegStart(L"HadronSelftestNoSuchService", w4, &le);
            check(!lw && le.find(L"not armed") != std::wstring::npos,
                  L"registry fallback (live backend): refused while unarmed, before the registry is opened: " + le);
        }

        // End to end: apply, with the undo entry on disk before the write, then Undo through the same path.
        std::wstring dir = newTempDir(L"regfb");
        if (dir.empty()) {
            check(false, L"registry fallback: a temp data folder for the apply");
            return;
        }
        Store st(dir);
        Plan one = Only(on, L"dmwappushservice");
        auto& bk = w.fx.backend;
        // The undo log is opened without sharing (it is half of the apply lock), so it can't be read while the apply runs.
        // Instead: the flush of the I record is made to fail, and then nothing may be written; with it succeeding (and
        // the D record's flush failing) exactly one write happens. So the write comes only after the I record is on disk.
        for (int failAt : {3, 4}) {
            EvWorld fw;
            Load(fixture, kb, &fw, &err);
            std::wstring fd = newTempDir(L"regflush");
            Store fs2(fd);
            Plan fp = Only(StrippedPlan(fw, true, false), L"dmwappushservice");
            ApplyEnv fe2 = Env(&fw.fx.backend, &fs2, &fw.ctx, true);
            fe2.failFlushAfter = failAt;   // header, B, P (3 records), then the I record (the 4th), then D
            ApplyResult fr;
            bool fok = RunApply(fp, fe2, &fr);
            RegStartVals now;
            fw.fx.backend.ReadRegStart(L"dmwappushservice", &now, &err);
            if (failAt == 3)
                check(!fd.empty() && !fok && fr.aborted && fw.fx.backend.faults.regWrites == 0 && now.start == 3,
                      L"registry fallback: when the undo entry with the old value can't be flushed, nothing is written to the registry");
            else
                check(!fd.empty() && !fok && fr.aborted && fw.fx.backend.faults.regWrites == 1 && now.start == 4,
                      L"registry fallback: once that entry is flushed the one write happens (the D record's flush failing stops the run after it)");
        }
        std::vector<std::wstring> order;
        bk.faults.onRegWrite = [&](const std::wstring& name) { order.push_back(name); };
        int setBefore = bk.faults.setStartCalls;
        ApplyResult res;
        ApplyEnv env = Env(&bk, &st, &w.ctx, true);
        RunApply(one, env, &res);
        RegStartVals after;
        bk.ReadRegStart(L"dmwappushservice", &after, &err);
        SvcItem q;
        bk.Query(L"dmwappushservice", &q, &err);
        check(res.ok == 1 && res.failed == 0 && after.start == 4 && q.cfg.start == 3 && bk.faults.setStartCalls == setBefore,
              L"registry fallback: applied through the registry (Start now 4; the service manager still says Manual until a restart; "
              L"no SCM write): " + Lines(res));
        check(order.size() == 1 && order[0] == L"dmwappushservice", L"registry fallback: exactly one registry write, for the confirmed row");
        check(!res.snapPath.empty() && !st.List(L"snap-*.txt").empty(), L"registry fallback: a pre-apply snapshot was written first");
        std::vector<UndoLog> logs = LoadLogs(st, nullptr);
        const UndoLog* lg = Newest(logs, res.stamp);
        const UndoRow* ur = lg && !lg->rows.empty() ? &lg->rows[0] : nullptr;
        check(lg && lg->hasE && ur && ur->identity == kRegIdentity && ur->hasD && ur->result == L"ok" && CfgText(ur->before) == L"3/-" &&
                  LastAppliedViaReg(logs).count(L"dmwappushservice"),
              L"registry fallback: the log's P/I records name the registry identity, with the old value 3/-, and the D record says ok");

        // Undo: left out while the toggle is off; with it on, written back through the registry.
        std::vector<SvcItem> items2;
        bk.Enumerate(&items2, &err);
        FoldInstances(&items2);
        Plan u = lg ? BuildUndo(*lg, bk, items2, w.ctx) : Plan{};
        Plan uOff = u, uOn = u;
        MarkRegFallback(&uOff, bk, false, false, items2, w.ctx);
        MarkTi(&uOff, bk, false, items2);
        MarkRegFallback(&uOn, bk, true, false, items2, w.ctx);
        MarkTi(&uOn, bk, false, items2);
        const PlanRow* uo = Row(uOff, L"dmwappushservice");
        const PlanRow* un = Row(uOn, L"dmwappushservice");
        check(uo && uo->viaReg && !uo->include && !uo->canToggle && uo->warning == kRegUndoOff && un && un->viaReg && un->include &&
                  un->before.start == 4 && un->after.start == 3,
              L"registry fallback undo: the row is read from the registry (4 -> 3) and left out until the toggle is on");
        ApplyResult ures;
        int writes = bk.faults.regWrites;
        RunApply(uOff, Env(&bk, &st, &w.ctx, false), &ures);
        check(bk.faults.regWrites == writes, L"registry fallback undo: nothing is written while the toggle is off");
        ApplyResult ures2;
        RunApply(uOn, Env(&bk, &st, &w.ctx, true), &ures2);
        RegStartVals back;
        bk.ReadRegStart(L"dmwappushservice", &back, &err);
        check(ures2.ok == 1 && back.start == 3 && bk.faults.setStartCalls == setBefore,
              L"registry fallback undo round trip: Start is 3 again, through the registry: " + Lines(ures2));
        // The same row planned with the toggle on but applied with it off: skipped, not written.
        ApplyResult offRes;
        writes = bk.faults.regWrites;
        Plan again = Only(StrippedPlan(w, true, false), L"dmwappushservice");
        RunApply(again, Env(&bk, &st, &w.ctx, false), &offRes);
        check(offRes.ok == 0 && offRes.skipped == 1 && bk.faults.regWrites == writes,
              L"registry fallback: a registry row is never written when the apply's toggle is off: " + Lines(offRes));

        // Drift: someone else wrote Start between the plan and the write.
        Plan drift = Only(StrippedPlan(w, true, false), L"dmwappushservice");
        bk.faults.regDrift.insert(L"dmwappushservice");
        writes = bk.faults.regWrites;
        ApplyResult dres;
        RunApply(drift, Env(&bk, &st, &w.ctx, true), &dres);
        check(dres.ok == 0 && dres.skipped == 1 && bk.faults.regWrites == writes && Lines(dres).find(L"changed since the plan") != std::wstring::npos,
              L"registry fallback: a value that drifted since the plan aborts that item, unwritten: " + Lines(dres));
        // Put the drifted value back the way the fake drifted it (4 -> 3), for the next check.
        bk.faults.regDrift.insert(L"dmwappushservice");
        RegStartVals tmp;
        bk.ReadRegStart(L"dmwappushservice", &tmp, &err);

        // Read-back mismatch: the write "succeeds" but the key holds something else.
        Plan rb = Only(StrippedPlan(w, true, false), L"dmwappushservice");
        bk.faults.regReadBack.insert(L"dmwappushservice");
        ApplyResult rres;
        RunApply(rb, Env(&bk, &st, &w.ctx, true), &rres);
        std::vector<UndoLog> logs2 = LoadLogs(st, nullptr);
        const UndoLog* l2 = Newest(logs2, rres.stamp);
        check(tmp.start == 3 && rres.failed == 1 && rres.ok == 0 && Lines(rres).find(L"Undo puts back") != std::wstring::npos && l2 &&
                  !l2->rows.empty() && l2->rows[0].hasD && l2->rows[0].result != L"ok",
              L"registry fallback: a read-back that is not what Hadron wrote is recorded as failed, with Undo as the way back: " + Lines(rres));
        bk.faults.regReadBack.clear();
        bk.faults.onRegWrite = nullptr;

        // Write-time re-check: a row whose item became launch-protected since the plan is not written.
        Plan pr = Only(StrippedPlan(w, true, false), L"dmwappushservice");
        std::vector<SvcItem> raw = bk.Items();
        for (auto& i : raw) if (IEq(i.name, L"dmwappushservice")) i.protect = 2;
        bk.SetItems(raw);
        writes = bk.faults.regWrites;
        ApplyResult pres;
        RunApply(pr, Env(&bk, &st, &w.ctx, true), &pres);
        check(pres.ok == 0 && bk.faults.regWrites == writes && Lines(pres).find(L"launch-protected") != std::wstring::npos,
              L"registry fallback: refusals are checked again on the live item at write time (now launch-protected): " + Lines(pres));
    }
}

}  // namespace hx::services
