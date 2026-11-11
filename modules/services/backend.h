// modules/services/backend.h — where the service data comes from and where writes go (r2 §6).
// Not named IServiceProvider (that is a COM interface).
//
// LiveBackend (backend_live.cpp) is the ONLY code in Hadron that calls SCM or registry write APIs, and
// its write methods refuse until Arm() was called; only the UI's confirmed-plan path and --live-test arm
// it. FakeBackend (backend_fake.cpp) serves --screenshot, --selftest and --dump-plan from a fixture and
// mutates memory only, with fault injection for the selftest.
#pragma once

#include "modules/services/hw.h"
#include "modules/services/model.h"
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace hx::services {

enum class Identity { Admin, TrustedInstaller };
enum class Op { Start, Stop };

// One start-type write. delayed: -1 leaves DelayedAutostart alone, 0/1 sets it (only meaningful with Auto).
struct WriteOp {
    int start = 3;
    int delayed = -1;
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
    virtual bool Control(const std::wstring& name, Op op, const std::atomic<bool>* cancel, std::wstring* err,
                         bool* ownStart = nullptr) = 0;
    virtual bool ReadHw(HwInfo* out) = 0;
    virtual std::wstring Build() = 0;         // "19045"
    virtual std::wstring MachineGuid() = 0;
    virtual void Arm() {}
    virtual void Disarm() {}
};

std::unique_ptr<Backend> MakeLiveBackend();
// VERSIONINFO CompanyName of the item's image (ServiceDll for svchost services): the file's own claim.
std::wstring FileCompany(const SvcItem& item);

// Fault injection for the selftest (r5 §5.1).
struct FakeFaults {
    std::set<std::wstring> deny;          // SetStart fails with access denied, even as TrustedInstaller
    std::set<std::wstring> vanish;        // Query fails (item gone) from the next call on
    std::set<std::wstring> changeOnQuery; // the next Query first flips the item's start type (changed since plan)
    std::set<std::wstring> verifyMismatch;// SetStart "succeeds" but Windows keeps the old value
    std::set<std::wstring> startRace;     // a start request finds it already running (someone else started it first)
    std::map<std::wstring, int> stopPendingQueries;   // a STOP_PENDING item reads STOPPED after this many more Queries
    int setStartCalls = 0;
};

class FakeBackend : public Backend {
public:
    // dir holds snapshot.txt (the machine) and hw.txt (devnodes). Missing hw.txt = no hardware info.
    bool Load(const std::wstring& dir, std::wstring* err);
    void SetItems(std::vector<SvcItem> items) { items_ = std::move(items); }
    void SetHw(const HwInfo& hw) { hw_ = hw; }
    void SetBuild(const std::wstring& b) { build_ = b; }
    const std::vector<SvcItem>& Items() const { return items_; }
    void SetState(const std::wstring& name, DWORD state) { if (SvcItem* i = Find(name)) i->state = state; }   // "someone else" did it
    FakeFaults faults;
    std::vector<std::wstring> controlLog;   // "stop X", "start X"

    bool IsLive() const override { return false; }
    bool Enumerate(std::vector<SvcItem>* out, std::wstring* err) override;
    bool Query(const std::wstring& name, SvcItem* out, std::wstring* err) override;
    bool Dependents(const std::wstring& name, std::vector<std::wstring>* active, std::wstring* err) override;
    bool CanWrite(const std::wstring& name, bool* denied, std::wstring* err) override;
    bool SetStart(const std::wstring& name, const WriteOp& w, Identity id, std::wstring* err) override;
    bool Control(const std::wstring& name, Op op, const std::atomic<bool>* cancel, std::wstring* err,
                 bool* ownStart = nullptr) override;
    bool ReadHw(HwInfo* out) override { *out = hw_; return hw_.ok; }
    std::wstring Build() override { return build_; }
    std::wstring MachineGuid() override { return machine_; }

private:
    SvcItem* Find(const std::wstring& name);
    std::vector<SvcItem> items_;
    HwInfo hw_;
    std::wstring build_ = L"19045", machine_ = L"fixture-machine";
};

}  // namespace hx::services
