// modules/services/evidence_view.cpp — see evidence_view.h.
#include "modules/services/evidence_view.h"
#include "core/sys/sys.h"

namespace hx::services {

template <class J>
void EvidencePasses::Retire(Pass<J>* cur, std::vector<Pass<J>>* retired) {
    if (!cur->w) return;
    cur->w->Cancel();          // never Join here: this runs on the UI thread
    retired->push_back(std::move(*cur));
    *cur = Pass<J>{};
}

template <class J>
void EvidencePasses::Reap(std::vector<Pass<J>>* retired, bool all) {
    for (auto it = retired->begin(); it != retired->end();) {
        if (all || it->job->finished.load()) {
            it->w->Join();   // finished: the thread only has its done message left to post
            it = retired->erase(it);
        } else {
            ++it;
        }
    }
}

bool EvidencePasses::StartSigners(HWND notify, UINT msg, const std::vector<SvcItem>& items, std::wstring* err) {
    Retire(&signer_, &retiredSigners_);
    Reap(&retiredSigners_, false);
    signerJob_.reset();
    auto job = std::make_shared<SignerJob>();
    job->in.reserve(items.size());
    for (auto& s : items) {   // name, type and image only: ItemSignedPath reads ServiceDll itself
        SvcItem c;
        c.name = s.name;
        c.type = s.type;
        c.image = s.image;
        c.kind = s.kind;
        job->in.push_back(std::move(c));
    }
    auto w = std::make_unique<sys::Worker>();
    sys::Worker* wp = w.get();
    if (!w->Start(notify, msg, [job, wp]() {
            for (auto& i : job->in) {
                if (wp->Cancelled()) break;
                job->out.push_back({Lower(i.name), ItemSigner(i)});
            }
            job->done.store(!wp->Cancelled());
            job->finished.store(true);
        })) {
        *err = sys::ErrorText(GetLastError());
        return false;
    }
    signer_.w = std::move(w);
    signer_.job = job;
    signerJob_ = job;
    return true;
}

bool EvidencePasses::OnSigners() {
    Reap(&retiredSigners_, false);   // a replaced pass's message: join it now that it has finished, drop its results
    std::shared_ptr<SignerJob> job = signerJob_;
    if (!job || !job->finished.load()) return false;
    signer_.w->Join();
    signer_ = Pass<SignerJob>{};
    signerJob_.reset();
    if (!job->done.load()) return false;
    for (auto& p : job->out) signers_[p.first] = p.second;
    return true;
}

bool EvidencePasses::StartEvents(HWND notify, UINT msg, const std::map<std::wstring, std::wstring>& events, std::wstring* err) {
    Retire(&event_, &retiredEvents_);
    Reap(&retiredEvents_, false);
    eventJob_.reset();
    if (events.empty()) {   // nothing changed: nothing to look for
        events_.clear();
        eventsAsked_.clear();
        eventsErr_.clear();
        return true;
    }
    auto job = std::make_shared<EventJob>();
    job->names = events;
    auto w = std::make_unique<sys::Worker>();
    sys::Worker* wp = w.get();
    if (!w->Start(notify, msg, [job, wp]() {
            job->ok = ReadLive7040(job->names, [wp]() { return wp->Cancelled(); }, &job->out, &job->err);
            job->done.store(!wp->Cancelled());
            job->finished.store(true);
        })) {
        *err = sys::ErrorText(GetLastError());
        return false;
    }
    event_.w = std::move(w);
    event_.job = job;
    eventJob_ = job;
    return true;
}

bool EvidencePasses::OnEvents() {
    Reap(&retiredEvents_, false);
    std::shared_ptr<EventJob> job = eventJob_;
    if (!job || !job->finished.load()) return false;
    event_.w->Join();
    event_ = Pass<EventJob>{};
    eventJob_.reset();
    if (!job->done.load()) return false;
    events_ = NewestPerService(job->out, job->names);
    eventsAsked_ = job->names;
    eventsErr_ = job->ok ? std::wstring() : job->err;
    return true;
}

void EvidencePasses::FromFixture(const FakeBackend& fake, const std::map<std::wstring, std::wstring>& events) {
    Retire(&signer_, &retiredSigners_);
    Retire(&event_, &retiredEvents_);
    signerJob_.reset();
    eventJob_.reset();
    signers_ = fake.Signers();
    events_ = NewestPerService(fake.Events(), events);
    eventsAsked_ = events;
    eventsErr_.clear();
}

// The page is closing: the only place a pass is waited for.
void EvidencePasses::Shutdown() {
    Retire(&signer_, &retiredSigners_);
    Retire(&event_, &retiredEvents_);
    Reap(&retiredSigners_, true);
    Reap(&retiredEvents_, true);
    signerJob_.reset();
    eventJob_.reset();
}

const SignerInfo* EvidencePasses::Signer(const std::wstring& name) const {
    auto it = signers_.find(Lower(name));
    return it == signers_.end() ? nullptr : &it->second;
}

const Ev7040* EvidencePasses::Event(const std::wstring& name) const {
    auto it = events_.find(Lower(name));
    return it == events_.end() ? nullptr : &it->second;
}

std::wstring EvidencePasses::EventLine(const std::wstring& name) const {
    if (eventJob_) return L"reading the System log...";
    if (!EventsLookedFor(name)) return L"";
    const Ev7040* e = Event(name);
    if (!e && !eventsErr_.empty()) return L"the System log could not be read: " + eventsErr_;
    return Ev7040Text(e);
}

}  // namespace hx::services
