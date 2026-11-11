// modules/services/evidence_view.h — the page's read-only evidence passes (svcdrv follow-ups 6 and 7), kept out of
// module.cpp so the page only holds one member and a few hooks.
//
// Two Worker passes after each load, like the Company pass: the Authenticode signer of every item's file
// (ItemSigner: offline, cached per path) and the System log's newest event 7040 for each item whose start type
// differs from first.txt or from what Hadron last set (ReadLive7040). Both work on copies of the items; a done
// message from a pass that a newer one replaced finds its job's `done` false (or a stale job) and is ignored. Neither
// ever opens a dialog. A fixture's data comes from the fake backend (signers.txt, events7040.txt); nothing is read.
#pragma once

#include "core/sys/worker.h"
#include "modules/services/backend.h"
#include "modules/services/evidence.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hx::services {

class EvidencePasses {
public:
    // Live: starts both passes (the previous ones are cancelled and joined first). events: lower-case name ->
    // display name of the items whose 7040 is wanted. False with *err when a worker can't start.
    bool StartSigners(HWND notify, UINT msg, const std::vector<SvcItem>& items, std::wstring* err);
    bool StartEvents(HWND notify, UINT msg, const std::map<std::wstring, std::wstring>& events, std::wstring* err);
    // The workers' done messages: true when this pass's results were taken (the caller refilters and repaints).
    bool OnSigners();
    bool OnEvents();
    // Fixture: the fake backend's data, at once.
    void FromFixture(const FakeBackend& fake, const std::map<std::wstring, std::wstring>& events);
    void Shutdown();

    bool SignersPending() const { return signerJob_ != nullptr; }
    bool EventsPending() const { return eventJob_ != nullptr; }
    // Null while unknown (pending, or never read).
    const SignerInfo* Signer(const std::wstring& name) const;
    // Was this item's 7040 looked for? Then Event() is its newest one, or null for "none found".
    bool EventsLookedFor(const std::wstring& name) const { return eventsAsked_.count(Lower(name)) != 0; }
    const Ev7040* Event(const std::wstring& name) const;
    const std::wstring& EventsError() const { return eventsErr_; }
    // The detail pane's "Changed by (event 7040)" value for a changed item.
    std::wstring EventLine(const std::wstring& name) const;

private:
    struct SignerJob {
        std::vector<SvcItem> in;
        std::vector<std::pair<std::wstring, SignerInfo>> out;
        std::atomic<bool> done{false};
    };
    struct EventJob {
        std::map<std::wstring, std::wstring> names;
        std::vector<Ev7040> out;
        std::wstring err;
        bool ok = false;
        std::atomic<bool> done{false};
    };
    sys::Worker signerW_, eventW_;
    std::shared_ptr<SignerJob> signerJob_;
    std::shared_ptr<EventJob> eventJob_;
    std::map<std::wstring, SignerInfo> signers_;          // lower-case name
    std::map<std::wstring, Ev7040> events_;               // lower-case name -> newest 7040
    std::map<std::wstring, std::wstring> eventsAsked_;    // what the last finished pass looked for
    std::wstring eventsErr_;
};

}  // namespace hx::services
