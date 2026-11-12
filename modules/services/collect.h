// modules/services/collect.h — everything the Services page lists (fix/svcdrv-visibility).
//
// CollectItems runs the backend's SCM enumeration, then cross-checks it against the Services keys of the current
// control set: a key the enumeration did not return is read from the SCM by name (source ScmDirect), and a key the
// SCM does not have, or would not open, becomes a read-only "Registry only (not loaded by the SCM)" row. With a
// TrustedInstaller provider the same pass runs again under TrustedInstaller's token (reverted on every path) and the
// union is kept, with counts of what TrustedInstaller added. Reads only: nothing here writes a service or a key.
#pragma once

#include "modules/services/backend.h"
#include "modules/services/plan.h"
#include <atomic>
#include <string>
#include <vector>

namespace hx::services {

bool ParseRegKeys(const std::string& utf8, std::vector<RegSvcKey>* out, std::wstring* err);

struct CollectStats {
    EnumStats scm;                 // the administrator pass's enumeration, before any Hadron processing
    int regKeys = -1;              // Services subkeys listed (-1: the key could not be listed)
    std::wstring regErr;           // why the list failed, or (regPartial) why it stopped part way
    bool regPartial = false;       // the listing stopped after regKeys keys (review R10)
    int overridden = 0;            // drivers whose registry Start a StartOverride value replaces
    int regLinks = 0;              // of those, registry links (leaves)
    int direct = 0;                // keys the enumeration missed that the SCM read by name
    int regOnly = 0;               // keys added as Registry only
    int folded = 0;                // per-user instances folded onto their templates
    // TrustedInstaller pass.
    bool tiAsked = false, tiUsed = false;
    std::wstring tiErr;            // why the pass didn't run, or failed part way
    std::wstring tiNote;           // Release's note (Hadron started TrustedInstaller and stopped it again)
    int tiItems = 0;               // rows only TrustedInstaller could see (or the SCM opened only for it)
    int tiConfigs = 0;             // rows whose configuration only TrustedInstaller could read
    std::vector<std::wstring> tiNames;
    unsigned long long ms = 0;     // elapsed, the whole collection
};

// out: every row before FoldInstances (the caller folds; LoadJob and the dump do). ti may be null (administrator only).
// claim: when TrustedInstaller is stopped, the pass may start it (Backend::AllowTiControl, then stops it again) only
// after claim->Begin wrote the flushed "T starting" record (review R2); null (--dump-list-live, no data folder)
// leaves a stopped TrustedInstaller alone and skips the pass with the reason. When TrustedInstaller is running already
// nothing is recorded and control stays refused, so the pass can't start it.
// cancel may be null. False only when the administrator's enumeration failed (*err says why).
bool CollectItems(Backend& b, TiProvider* ti, TiClaimLog* claim, const std::atomic<bool>* cancel, std::vector<SvcItem>* out,
                  CollectStats* st, std::wstring* err);

// The status-line sentence for a TrustedInstaller pass ("" when it was not asked for).
std::wstring TiCollectText(const CollectStats& st);
// The load's status-line sentences: registry-only rows, rows read by name, the TrustedInstaller result. "" or ends in
// a space.
std::wstring VisibilityText(const CollectStats& st);
// The TSV --dump-list-live writes (header with counts, then one line per row; rows after FoldInstances).
std::string DumpListTsv(const std::vector<SvcItem>& items, const CollectStats& st, const std::wstring& build);

}  // namespace hx::services
