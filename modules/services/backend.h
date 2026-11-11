// modules/services/backend.h — where the service data comes from and where writes go (r2 §6).
// Not named IServiceProvider (that is a COM interface).
//
// LiveBackend (backend_live.cpp) is the ONLY code in Hadron that calls SCM or registry write APIs, and
// its write methods refuse until Arm() was called; only the UI's confirmed-plan path and --live-test arm
// it. FakeBackend (backend_fake.cpp) serves --screenshot, --selftest and --dump-plan from a fixture and
// mutates memory only, with fault injection for the selftest.
#pragma once

#include "modules/services/evidence.h"
#include "modules/services/hw.h"
#include "modules/services/model.h"
#include "modules/services/sxsstock.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace hx::services {

enum class Identity { Admin, TrustedInstaller };
enum class Op { Start, Stop };

constexpr DWORD kControlWaitMs = 30000;   // the wait for RUNNING/STOPPED after a start or stop request (r1 §4)
// Cleanup requests (TrustedInstaller put back after an apply, R13's restarts after a failed stop) are sent even after
// Cancel and waited for this long without Cancel cutting it short (mini-analyze A5).
constexpr DWORD kCleanupWaitMs = 10000;
// Control's error when the request was sent but the wait for the new state ran out or was cancelled: the request may
// still take effect, so callers say "requested, not confirmed" instead of "failed".
inline bool RequestSentNotConfirmed(const std::wstring& err) {
    return err.rfind(L"timed out waiting", 0) == 0 || err.rfind(L"cancelled while waiting", 0) == 0;
}

// One start-type write. delayed: -1 leaves DelayedAutostart alone, 0/1 sets it (only meaningful with Auto).
struct WriteOp {
    int start = 3;
    int delayed = -1;
};

// The raw registry values the registry fallback reads and writes (HKLM\SYSTEM\CurrentControlSet\Services\<name>):
// Start and DelayedAutostart, -1 when absent. Hadron writes them only when the SCM refuses a start-type change, with
// the per-apply toggle on (regfallback.h).
struct RegStartVals {
    int start = -1;
    int delayed = -1;
    StartCfg Cfg() const { return StartCfg{start, delayed < 0 ? -1 : (delayed ? 1 : 0)}; }
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual bool IsLive() const = 0;
    virtual bool Enumerate(std::vector<SvcItem>* out, std::wstring* err) = 0;   // services + drivers, full config
    virtual bool Query(const std::wstring& name, SvcItem* out, std::wstring* err) = 0;   // false = vanished/unreadable
    virtual bool Dependents(const std::wstring& name, std::vector<std::wstring>* active, std::wstring* err) = 0;
    // Opens the service for SERVICE_CHANGE_CONFIG as the current identity and closes it: changes nothing.
    // *denied = true on ERROR_ACCESS_DENIED (the plan marks the row "needs TrustedInstaller").
    virtual bool CanWrite(const std::wstring& name, bool* denied, std::wstring* err) = 0;
    virtual bool SetStart(const std::wstring& name, const WriteOp& w, Identity id, std::wstring* err) = 0;
    // *ownStart (optional) is true only when this call's own start request took the service out of STOPPED;
    // false when it was already running (ERROR_SERVICE_ALREADY_RUNNING: someone else started it first).
    // waitMs caps the wait for the new state after the request was sent (cancel, when set, cuts it shorter).
    virtual bool Control(const std::wstring& name, Op op, const std::atomic<bool>* cancel, std::wstring* err,
                         bool* ownStart = nullptr, DWORD waitMs = kControlWaitMs) = 0;
    virtual bool ReadHw(HwInfo* out) = 0;
    virtual std::wstring Build() = 0;         // "19045"
    virtual std::wstring MachineGuid() = 0;
    // The reader for this PC's WinSxS component manifests (Windows-stock values); null when there is none.
    virtual std::unique_ptr<SxsReader> MakeSxs() { return nullptr; }
    virtual void Arm() {}
    virtual void Disarm() {}
    // Registry fallback (regfallback.h). ReadRegStart reads the service key's Start and DelayedAutostart (query rights
    // only). WriteRegStart refuses until Arm(), refuses a Boot or System target, a key whose Start is Boot or System
    // now, a launch-protected service and a key that is a registry link. Under its write handle it reads the values
    // again and refuses, writing nothing, when they are not `expect` (Start, and DelayedAutostart raw: absent is not 0)
    // : the value RunApply re-read and logged in the I record. Then it writes DelayedAutostart (when w.delayed >= 0)
    // and Start, as REG_DWORD, into an existing key only; if Start fails after DelayedAutostart was written, the old
    // DelayedAutostart is put back (deleted when it was absent) on the same handle.
    virtual bool ReadRegStart(const std::wstring& name, RegStartVals* out, std::wstring* err) {
        (void)name; (void)out;
        *err = L"this backend has no registry access";
        return false;
    }
    virtual bool WriteRegStart(const std::wstring& name, const WriteOp& w, const RegStartVals& expect, std::wstring* err) {
        (void)name; (void)w; (void)expect;
        *err = L"this backend has no registry access";
        return false;
    }
};

std::unique_ptr<Backend> MakeLiveBackend();
// The fake backend's process creation time for a pid (fixtures write the same value into T started records).
inline uint64_t FakeProcStart(DWORD pid) { return pid ? 0x01DC000000000000ull + pid : 0; }
// VERSIONINFO CompanyName of the item's image (ServiceDll for svchost services): the file's own claim. Only for a
// local drive path whose attributes have no RECALL_ON_DATA_ACCESS / RECALL_ON_OPEN / OFFLINE bit (hard rule 1); a UNC
// path, a network drive or a cloud placeholder gives "" without the file being opened.
std::wstring FileCompany(const SvcItem& item);
bool CompanyPathAllowed(const std::wstring& path);   // a local drive path (X:\ or \\?\X:\), never UNC or a device
bool CompanyAttrsAllowed(DWORD attrs);               // a file with no recall or offline attribute

// Fault injection for the selftest (r5 §5.1).
struct FakeFaults {
    std::set<std::wstring> deny;          // SetStart fails with access denied, even as TrustedInstaller
    std::set<std::wstring> vanish;        // Query fails (item gone) from the next call on
    std::set<std::wstring> changeOnQuery; // the next Query first flips the item's start type (changed since plan)
    std::set<std::wstring> verifyMismatch;// SetStart "succeeds" but Windows keeps the old value
    std::set<std::wstring> startRace;     // a start request finds it already running (someone else started it first)
    std::set<std::wstring> stopFail;      // a stop request fails (the service keeps running)
    std::set<std::wstring> startNoWait;   // our start request is accepted, but the wait for RUNNING fails (START_PENDING)
    std::map<std::wstring, int> stopPendingQueries;   // a STOP_PENDING item reads STOPPED after this many more Queries
    std::set<std::wstring> cancelOnStop;  // a stop request for it sets *raise (the user pressed Cancel meanwhile)
    std::atomic<bool>* raise = nullptr;
    int setStartCalls = 0;
    // Registry fallback (the fake's in-memory registry; nothing real is ever written).
    std::set<std::wstring> regDeny;       // WriteRegStart fails with access denied
    std::set<std::wstring> regDrift;      // the next ReadRegStart first flips the registry Start (someone else wrote it)
    std::set<std::wstring> regReadBack;   // WriteRegStart "succeeds" but the value read back is not what was written
    std::set<std::wstring> regDriftAtWrite;   // someone else writes Start after RunApply's re-read and I record, just
                                              // before Hadron's write (WriteRegStart must see it under its handle)
    std::set<std::wstring> regStartFail;  // the Start write fails after DelayedAutostart was written
    std::function<void(const std::wstring&)> onRegWrite;   // called just before each registry write (the selftest
                                                           // reads the undo log from disk there)
    int regWrites = 0;
};

class FakeBackend : public Backend {
public:
    // dir holds snapshot.txt (the machine) and hw.txt (devnodes). Missing hw.txt = no hardware info.
    bool Load(const std::wstring& dir, std::wstring* err);
    void SetItems(std::vector<SvcItem> items) { items_ = std::move(items); }
    void SetHw(const HwInfo& hw) { hw_ = hw; }
    void SetBuild(const std::wstring& b) { build_ = b; }
    const std::vector<SvcItem>& Items() const { return items_; }
    // "Someone else" did it: a service that goes from stopped to running is a new process (new pid and creation time).
    void SetState(const std::wstring& name, DWORD state) {
        SvcItem* i = Find(name);
        if (!i) return;
        if (state == SERVICE_STOPPED) { i->pid = 0; i->pstart = 0; }
        else if (i->state == SERVICE_STOPPED && state != SERVICE_STOP_PENDING) { i->pid = nextPid_++; i->pstart = FakeProcStart(i->pid); }
        i->state = state;
    }
    FakeFaults faults;
    std::vector<std::wstring> controlLog;   // "stop X", "start X"

    bool IsLive() const override { return false; }
    bool Enumerate(std::vector<SvcItem>* out, std::wstring* err) override;
    bool Query(const std::wstring& name, SvcItem* out, std::wstring* err) override;
    bool Dependents(const std::wstring& name, std::vector<std::wstring>* active, std::wstring* err) override;
    bool CanWrite(const std::wstring& name, bool* denied, std::wstring* err) override;
    bool SetStart(const std::wstring& name, const WriteOp& w, Identity id, std::wstring* err) override;
    // Honours cancel like the live backend: the request takes effect, but the wait for it is cut short (the item is
    // left pending and the call fails with "cancelled while waiting").
    bool Control(const std::wstring& name, Op op, const std::atomic<bool>* cancel, std::wstring* err,
                 bool* ownStart = nullptr, DWORD waitMs = kControlWaitMs) override;
    bool ReadHw(HwInfo* out) override { *out = hw_; return hw_.ok; }
    std::wstring Build() override { return build_; }
    std::wstring MachineGuid() override { return machine_; }
    // The fake registry: Start (regStart, else the configured start) and DelayedAutostart of each item. A write
    // changes regStart and the delayed value only: the service manager's start type changes at the next "restart".
    bool ReadRegStart(const std::wstring& name, RegStartVals* out, std::wstring* err) override;
    bool WriteRegStart(const std::wstring& name, const WriteOp& w, const RegStartVals& expect, std::wstring* err) override;
    // Fixture evidence: signers.txt (Authenticode results by item name, lower case) and events7040.txt.
    const std::map<std::wstring, SignerInfo>& Signers() const { return signers_; }
    const std::vector<Ev7040>& Events() const { return events_; }
    // The fixture's sxs\ folder (hand-written XML manifests), when it has one.
    std::unique_ptr<SxsReader> MakeSxs() override { return sxsDir_.empty() ? nullptr : MakeFakeSxsReader(sxsDir_); }
    void SetSxsDir(std::wstring d) { sxsDir_ = std::move(d); }

private:
    std::wstring sxsDir_;
    SvcItem* Find(const std::wstring& name);
    std::vector<SvcItem> items_;
    HwInfo hw_;
    std::wstring build_ = L"19045", machine_ = L"fixture-machine";
    DWORD nextPid_ = 9000;   // a service this backend starts gets a new process
    std::map<std::wstring, SignerInfo> signers_;
    std::vector<Ev7040> events_;
};

}  // namespace hx::services
