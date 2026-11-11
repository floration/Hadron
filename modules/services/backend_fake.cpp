// modules/services/backend_fake.cpp — fixture-backed backend: reads tests/fixtures/services, writes memory.
// Never touches the SCM or the registry. Used by --screenshot, --selftest and --dump-plan.
#include "modules/services/backend.h"
#include "modules/services/store.h"

namespace hx::services {

bool FakeBackend::Load(const std::wstring& dir, std::wstring* err) {
    Store s(dir);
    std::string text;
    if (!s.Read(L"snapshot.txt", &text, err)) return false;
    Snapshot snap;
    if (!ParseSnapshot(text, &snap, err)) { *err = L"snapshot.txt: " + *err; return false; }
    items_ = std::move(snap.items);
    for (auto& i : items_) if (i.pid && !i.pstart) i.pstart = FakeProcStart(i.pid);
    if (snap.meta.count(L"build")) {
        std::wstring b = snap.meta[L"build"];
        // "10.0.19045.5737" -> "19045"
        auto parts = Split(b, L'.');
        build_ = parts.size() >= 3 ? parts[2] : b;
    }
    if (snap.meta.count(L"machine")) machine_ = snap.meta[L"machine"];
    std::string hw;
    std::wstring herr;
    if (s.Exists(L"hw.txt")) {
        if (!s.Read(L"hw.txt", &hw, &herr) || !ParseHw(hw, &hw_, &herr)) { *err = L"hw.txt: " + herr; return false; }
    } else {
        hw_ = HwInfo{};
        hw_.error = L"no hw.txt in the fixture";
    }
    return true;
}

SvcItem* FakeBackend::Find(const std::wstring& name) {
    for (auto& i : items_) if (IEq(i.name, name)) return &i;
    return nullptr;
}

bool FakeBackend::Enumerate(std::vector<SvcItem>* out, std::wstring* err) {
    (void)err;
    out->clear();
    for (auto& i : items_) if (!faults.vanish.count(Lower(i.name))) out->push_back(i);
    return true;
}

bool FakeBackend::Query(const std::wstring& name, SvcItem* out, std::wstring* err) {
    SvcItem* i = Find(name);
    if (!i || faults.vanish.count(Lower(name))) { *err = L"The specified service does not exist as an installed service."; return false; }
    if (auto sp = faults.stopPendingQueries.find(Lower(name)); sp != faults.stopPendingQueries.end() && i->state == SERVICE_STOP_PENDING) {
        if (sp->second-- <= 0) { i->state = SERVICE_STOPPED; faults.stopPendingQueries.erase(sp); }
    }
    if (faults.changeOnQuery.count(Lower(name))) {
        faults.changeOnQuery.erase(Lower(name));
        i->cfg.start = i->cfg.start == 3 ? 2 : 3;   // something else changed it
    }
    *out = *i;
    return true;
}

bool FakeBackend::Dependents(const std::wstring& name, std::vector<std::wstring>* active, std::wstring* err) {
    (void)err;
    active->clear();
    // Running items that list name directly (one level, then recursively, dependents first).
    std::vector<std::wstring> stack{name};
    std::set<std::wstring> seen;
    while (!stack.empty()) {
        std::wstring cur = stack.back();
        stack.pop_back();
        for (auto& i : items_) {
            if (i.state != SERVICE_RUNNING || seen.count(Lower(i.name))) continue;
            for (auto& d : i.deps)
                if (IEq(d, cur)) { seen.insert(Lower(i.name)); active->insert(active->begin(), i.name); stack.push_back(i.name); break; }
        }
    }
    return true;
}

bool FakeBackend::CanWrite(const std::wstring& name, bool* denied, std::wstring* err) {
    SvcItem* i = Find(name);
    *denied = false;
    if (!i) { *err = L"The specified service does not exist as an installed service."; return false; }
    if (i->aclTi || faults.deny.count(Lower(name))) { *denied = true; *err = L"Access is denied."; return false; }
    return true;
}

bool FakeBackend::SetStart(const std::wstring& name, const WriteOp& w, Identity id, std::wstring* err) {
    faults.setStartCalls++;
    SvcItem* i = Find(name);
    if (!i || faults.vanish.count(Lower(name))) { *err = L"The specified service does not exist as an installed service."; return false; }
    if (faults.deny.count(Lower(name)) || (i->aclTi && id != Identity::TrustedInstaller)) { *err = L"Access is denied."; return false; }
    if (w.start < 0 || w.start > 4 || (w.start < 2 && !IsDriverType(i->type))) { *err = L"The parameter is incorrect."; return false; }
    if (faults.verifyMismatch.count(Lower(name))) return true;   // Windows "kept" the old value
    i->cfg.start = w.start;
    if (w.delayed >= 0) i->cfg.delayed = w.delayed;
    i->regStart = w.start;
    return true;
}

bool FakeBackend::Control(const std::wstring& name, Op op, const std::atomic<bool>* cancel, std::wstring* err, bool* ownStart) {
    (void)cancel;
    if (ownStart) *ownStart = false;
    SvcItem* i = Find(name);
    if (!i) { *err = L"The specified service does not exist as an installed service."; return false; }
    if (IsDriverType(i->type)) { *err = L"Drivers are never loaded or unloaded live."; return false; }
    if (op == Op::Start) {
        if (i->cfg.start == 4) { *err = L"The service cannot be started, either because it is disabled or because it has no enabled devices associated with it."; return false; }
        bool race = faults.startRace.erase(Lower(name)) > 0;   // Windows started it between the caller's Query and this
        bool fresh = i->state == SERVICE_STOPPED;
        if (ownStart) *ownStart = fresh && !race;
        if (fresh || race) { i->pid = nextPid_++; i->pstart = FakeProcStart(i->pid); }   // a new process
        if (faults.startNoWait.erase(Lower(name))) {
            i->state = SERVICE_START_PENDING;
            controlLog.push_back(L"start " + name + L" (still starting)");
            *err = L"timed out waiting; last state Starting";
            return false;
        }
        i->state = SERVICE_RUNNING;
        controlLog.push_back(race ? L"start " + name + L" (already running)" : L"start " + name);
    } else {
        if (faults.stopFail.count(Lower(name))) {
            controlLog.push_back(L"stop " + name + L" (failed)");
            *err = L"timed out waiting; last state Running";
            return false;
        }
        i->state = SERVICE_STOPPED;
        i->pid = 0;
        i->pstart = 0;
        controlLog.push_back(L"stop " + name);
    }
    return true;
}

}  // namespace hx::services
