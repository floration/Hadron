// core/sys/worker.h — the legacy worker-thread pattern made safe. See docs/ARCHITECTURE.md "Worker".
//
// Long work runs here and posts completion to the module HWND; the UI reads `progress` on a timer.
// The destructor cancels then joins with no timeout, so a module can never outlive its worker.
// Uses CreateThread (not std::thread) so a scan can reserve a large stack, as legacy DiskMap does.
#pragma once

#include "core/sys/win_compat.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>

namespace hx::sys {

class Worker {
public:
    Worker() = default;
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    ~Worker() { Cancel(); Join(); }

    // fn runs on the new thread and should poll Cancelled() and update `progress`. When it returns,
    // doneMsg is posted to `notify` (wParam = cancelled ? 1 : 0). stackReserve bytes are reserved
    // for the thread stack when non-zero (STACK_SIZE_PARAM_IS_A_RESERVATION).
    bool Start(HWND notify, UINT doneMsg, std::function<void()> fn, SIZE_T stackReserve = 0) {
        Cancel(); Join();
        cancel_.store(false);
        progress.store(0);
        notify_ = notify;
        doneMsg_ = doneMsg;
        fn_ = std::move(fn);
        DWORD flags = stackReserve ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0;
        thread_ = CreateThread(nullptr, stackReserve, &Worker::Thunk, this, flags, nullptr);
        return thread_ != nullptr;   // callers must check: a failed start never leaves "running" true
    }

    void Cancel() { cancel_.store(true); }
    bool Cancelled() const { return cancel_.load(); }
    bool Running() const { return thread_ != nullptr; }

    void Join() {
        if (!thread_) return;
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
    }

    std::atomic<uint64_t> progress{0};

private:
    static DWORD WINAPI Thunk(LPVOID p) {
        auto* w = static_cast<Worker*>(p);
        w->fn_();
        if (w->notify_) PostMessageW(w->notify_, w->doneMsg_, w->cancel_.load() ? 1 : 0, 0);
        return 0;
    }

    HANDLE thread_ = nullptr;
    HWND notify_ = nullptr;
    UINT doneMsg_ = 0;
    std::function<void()> fn_;
    std::atomic<bool> cancel_{false};
};

}  // namespace hx::sys
