// modules/services/evidence_view.cpp — see evidence_view.h.
#include "modules/services/evidence_view.h"
#include "core/sys/sys.h"

namespace hx::services {

bool EvidencePasses::StartSigners(HWND notify, UINT msg, const std::vector<SvcItem>& items, std::wstring* err) {
    signerW_.Cancel();
    signerW_.Join();
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
    sys::Worker* w = &signerW_;
    signerJob_ = job;
    if (!signerW_.Start(notify, msg, [job, w]() {
            for (auto& i : job->in) {
                if (w->Cancelled()) break;
                job->out.push_back({Lower(i.name), ItemSigner(i)});
            }
            job->done.store(true);
        })) {
        signerJob_.reset();
        *err = sys::ErrorText(GetLastError());
        return false;
    }
    return true;
}

bool EvidencePasses::OnSigners() {
    std::shared_ptr<SignerJob> job = signerJob_;
    if (!job || !job->done.load()) return false;   // a replaced pass: the current one posts its own
    signerW_.Join();
    signerJob_.reset();
    for (auto& p : job->out) signers_[p.first] = p.second;
    return true;
}

bool EvidencePasses::StartEvents(HWND notify, UINT msg, const std::map<std::wstring, std::wstring>& events, std::wstring* err) {
    eventW_.Cancel();
    eventW_.Join();
    eventJob_.reset();
    if (events.empty()) {   // nothing changed: nothing to look for
        events_.clear();
        eventsAsked_.clear();
        eventsErr_.clear();
        return true;
    }
    auto job = std::make_shared<EventJob>();
    job->names = events;
    sys::Worker* w = &eventW_;
    eventJob_ = job;
    if (!eventW_.Start(notify, msg, [job, w]() {
            job->ok = ReadLive7040(job->names, [w]() { return w->Cancelled(); }, &job->out, &job->err);
            job->done.store(!w->Cancelled());
        })) {
        eventJob_.reset();
        *err = sys::ErrorText(GetLastError());
        return false;
    }
    return true;
}

bool EvidencePasses::OnEvents() {
    std::shared_ptr<EventJob> job = eventJob_;
    if (!job || !job->done.load()) return false;
    eventW_.Join();
    eventJob_.reset();
    events_ = NewestPerService(job->out, job->names);
    eventsAsked_ = job->names;
    eventsErr_ = job->ok ? std::wstring() : job->err;
    return true;
}

void EvidencePasses::FromFixture(const FakeBackend& fake, const std::map<std::wstring, std::wstring>& events) {
    Shutdown();
    signers_ = fake.Signers();
    events_ = NewestPerService(fake.Events(), events);
    eventsAsked_ = events;
    eventsErr_.clear();
}

void EvidencePasses::Shutdown() {
    signerW_.Cancel();
    eventW_.Cancel();
    signerW_.Join();
    eventW_.Join();
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
