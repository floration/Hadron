// modules/services/model.h — the pure data model of the Services page (docs/design/services.md).
// No Win32 calls here beyond types: everything is testable from fixtures under Wine.
#pragma once

#include "core/sys/win_compat.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hx::services {

enum class Kind { Service, UserTemplate, UserInstance, KernelDriver, FsDriver, Other };
extern const wchar_t kCfgUnreadable[];   // "configuration unreadable": the readError prefix of an item with cfgUnknown
enum Tab { TAB_SERVICES = 0, TAB_DRIVERS = 1 };

// What Hadron can write and must restore: the start type and the DelayedAutostart value. delayed keeps
// three states so undo can tell "value absent" from "value is 0" (r1 §0.7).
struct StartCfg {
    int start = 3;        // 0 Boot, 1 System, 2 Auto, 3 Manual (demand), 4 Disabled
    int delayed = -1;     // -1 value absent, 0, 1
    bool Delayed() const { return start == 2 && delayed == 1; }
    // Same effect at boot: the delayed flag only means anything with Auto.
    bool SameEffect(const StartCfg& o) const { return start == o.start && Delayed() == o.Delayed(); }
    bool operator==(const StartCfg& o) const { return start == o.start && delayed == o.delayed; }
    bool operator!=(const StartCfg& o) const { return !(*this == o); }
};

// "2/1", "3/-": the text form used in plans, undo logs and goldens.
std::wstring CfgText(const StartCfg& c);
bool ParseCfg(const std::wstring& s, StartCfg* out);

struct SvcItem {
    std::wstring name, display, desc, account, image, group;
    DWORD type = 0x10;
    StartCfg cfg;
    bool trigger = false;
    int regStart = -1;                 // registry Start value; -1 when unknown (pending-restart check)
    DWORD state = 1, controls = 0, pid = 0;
    int errctl = 1;
    int protect = 0;                   // SERVICE_LAUNCH_PROTECTED_* (0 none)
    std::vector<std::wstring> deps;    // DependOnService; a group dependency keeps its '+' prefix
    std::wstring failure, triggers;    // human-readable summaries
    std::vector<std::wstring> ifaces;  // device-interface trigger class GUIDs, lower case with braces
    std::wstring company;              // VERSIONINFO CompanyName: the file's own claim
    std::wstring readError;            // a partial read failure, shown in the detail pane
    // The configuration itself could not be read (readError starts with kCfgUnreadable): cfg is a placeholder, so the
    // item is never planned, recorded in first.txt or restored (Review R2).
    bool cfgUnknown = false;
    uint64_t pstart = 0;               // creation time (FILETIME) of process pid, 0 when unknown (TrustedInstaller claim)
    bool aclTi = false;                // fixture only: an admin write is refused (needs TrustedInstaller)
    // Fixture only (X lines): the fake's enumeration leaves it out but a read by name finds it (enum=miss); the
    // administrator can't see it at all, TrustedInstaller can (scm=ti); its configuration is readable only as
    // TrustedInstaller (cfg=ti).
    bool fxEnumMiss = false, fxTiOnly = false, fxCfgTi = false;
    // Where the row came from (fix/svcdrv-visibility). Scm: the SCM's enumeration. ScmDirect: a Services key the
    // enumeration did not return, read from the SCM by name. Registry: a Services key the SCM does not have (or would
    // not open): read-only, never planned, written, recorded or restored (cfgUnknown is set too); regWhy says why.
    enum class Source { Scm, ScmDirect, Registry } src = Source::Scm;
    std::wstring regWhy;               // Registry: why it is registry-only (shown in the detail pane)
    bool regLink = false;              // Registry: the key is a registry link (a leaf: never followed or read)
    bool regNoType = false;            // Registry: the key has no Type value (shown on the Services tab, labelled)
    bool viaTi = false;                // the row, or its configuration, was read only as TrustedInstaller
    bool RegOnly() const { return src == Source::Registry; }
    // Derived after load (FoldInstances).
    Kind kind = Kind::Service;
    std::vector<std::wstring> instances;   // "Name_1a2b3 (Running)" for per-user templates
};

// ---- names -------------------------------------------------------------------------------------
std::wstring StartName(int start);                                   // "Auto", "Manual", ...
std::wstring StartLabel(const StartCfg& c, bool trigger);            // "Auto (delayed)", "Manual +trigger"
std::wstring StateName(DWORD state);                                 // "Running", "Stopped", ...
std::wstring TypeName(DWORD type);                                   // "Own process", "Kernel driver", ...
std::wstring ErrCtlName(int e);
std::wstring ProtectName(int p);
Kind KindOf(DWORD type);
// "scm", "scm-direct" or "registry": the --dump-list-live source column.
const wchar_t* SourceName(const SvcItem& s);
extern const wchar_t kRegOnlyKind[];   // "Registry only (not loaded by the SCM)"
bool IsDriverType(DWORD type);
bool OnTab(const SvcItem& s, Tab t);
// The template name for a per-user instance ("CDPUserSvc_4f2a1" -> "CDPUserSvc"). Only when the type has
// the instance bit: a third-party "Foo_1a2b" without it stays itself (r5 E1).
std::wstring TemplateName(const std::wstring& name, DWORD type);
// Removes per-user instances from the list and records them on their template (read-only, folded).
void FoldInstances(std::vector<SvcItem>* items);

// Image path normalisation (r1 §1.4): \SystemRoot\ -> windir, \??\ stripped, quotes and arguments split,
// system32\drivers\x.sys relative to windir, an empty driver path -> windir\System32\drivers\<name>.sys.
// svchost -k <group> is reported separately. %VAR% is left for the caller to expand.
struct ImageInfo { std::wstring path, args, svchostGroup; bool unquotedSpaces = false; };
ImageInfo NormalizeImage(const std::wstring& raw, const std::wstring& windir, const std::wstring& name, bool driver);

// ---- text helpers --------------------------------------------------------------------------------
std::string ToUtf8(const std::wstring& s);
std::wstring FromUtf8(const std::string& s);
std::wstring Lower(std::wstring s);
bool IEq(const std::wstring& a, const std::wstring& b);
bool IContains(const std::wstring& hay, const std::wstring& needle);
// bios escaping: backslash doubled, TAB -> space, CR dropped, LF -> \n.
std::wstring Esc(const std::wstring& s);
std::wstring Unesc(const std::wstring& s);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep);
std::wstring Join(const std::vector<std::wstring>& v, const std::wstring& sep);
uint32_t Crc32(const void* data, size_t n, uint32_t crc = 0);
std::wstring Hex8(uint32_t v);

}  // namespace hx::services
