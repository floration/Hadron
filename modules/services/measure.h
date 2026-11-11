// modules/services/measure.h — measured before/after (docs/design/services.md "Measure").
//
// Hadron shows no predicted savings anywhere (hard rule 7). What it can show is what this PC measured: RAM in use and
// committed memory (GlobalMemoryStatusEx), the process count and the svchost.exe count (a Toolhelp snapshot), each
// recorded with its conditions: when (UTC), the Windows build, the machine GUID, when the PC booted and how many
// minutes after boot it was taken, and what it was taken for ("before" a plan, "after" a restart). The page shows a
// "before" and the first "after" taken after a restart side by side, labelled owner-measured with those conditions.
//
// File: measure\measure-<stamp>.txt in the data folder, CREATE_NEW and flushed, never overwritten:
//   HXSVC 1 measure
//   M <key> <value>     created (yyyy-MM-dd HH:mm:ss UTC), phase (before|after), build, machine, boot (UTC, as
//                       created), uptime (whole minutes since boot), plan (what the before was for, in words)
//   V <key> <value>     ram_used, ram_total, commit, commit_limit (bytes), processes, svchost (counts)
// Unknown lines are skipped (forward compatibility); a missing value makes the record unreadable.
#pragma once

#include "modules/services/store.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hx::services {

struct Measurement {
    std::wstring file;            // measure\<file>; empty until saved
    std::wstring created, boot;   // "2026-11-12 08:31:05" UTC
    std::wstring phase;           // "before" or "after"
    std::wstring build, machine, plan;
    int uptimeMin = 0;
    uint64_t ramUsed = 0, ramTotal = 0, commit = 0, commitLimit = 0;   // bytes
    int processes = 0, svchost = 0;
};

// Reads the live values now (GlobalMemoryStatusEx, CreateToolhelp32Snapshot, GetTickCount64, the system clock).
// build and machine are the caller's (the Backend's); plan is what a "before" is for.
bool TakeMeasurement(const std::wstring& phase, const std::wstring& build, const std::wstring& machine, const std::wstring& plan,
                     Measurement* out, std::wstring* err);
// When this PC booted, as Measurement::boot ("yyyy-MM-dd HH:mm:ss" UTC), to the second.
std::wstring BootTimeUtc();

std::string WriteMeasurement(const Measurement& m);
bool ParseMeasurement(const std::string& utf8, Measurement* out, std::wstring* err);

// Callers run Store::EnsureRoot first (the A1 ancestor rule), as for every file in the data folder.
bool SaveMeasurement(Store& st, Measurement* m, std::wstring* err);
std::vector<Measurement> LoadMeasurements(const Store& st, std::vector<std::wstring>* problems);   // oldest first

// The pair the page compares: the newest "after" taken in a boot that started after its "before" was recorded,
// and that "before" (the newest one recorded before that boot). -1 when there is none.
struct MeasurePair { int before = -1, after = -1; };
MeasurePair LatestPair(const std::vector<Measurement>& ms);
// The newest "before" with no "after" from a later boot yet: the page offers "Measure after" once this PC has
// restarted since it (bootNow later than its created). -1 when there is none or no restart happened yet.
int PendingBefore(const std::vector<Measurement>& ms, const std::wstring& bootNow);

std::wstring MiB(uint64_t bytes);   // "8,123 MiB"

}  // namespace hx::services
