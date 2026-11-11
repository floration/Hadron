// modules/runas/launch.h — token and launch logic for the Run As module.
//
// Everything here is testable without the UI: the pure helpers (argument quoting, path
// absolutisation, .msc command building, integrity RID -> name, the account filter and the
// last-run serialiser) are exercised by --selftest, and the Launch() entry points run on a
// core/sys Worker (never a dialog from the worker). See docs/design/runas.md.
#pragma once

#include "core/sys/win_compat.h"
#include <string>
#include <vector>

namespace hx::runas {

// Who to launch as. The Full/Limited choice is a separate axis (TokenMode).
enum class Who { ThisAdmin = 0, AnotherUser = 1, System = 2, TrustedInstaller = 3 };
enum class TokenMode { Full = 0, Limited = 1 };

const wchar_t* WhoName(Who w);   // "This admin" / "Another user" / "SYSTEM" / "TrustedInstaller"

// A request the UI hands the worker. The password is passed out of band (never stored here or
// logged), as a separate argument to Launch().
struct LaunchRequest {
    std::wstring path;         // target file / program / document / .lnk / .msc
    std::wstring args;         // extra arguments, appended after any the shell verb implies
    std::wstring workingDir;   // optional; empty => the target's folder
    Who       who  = Who::ThisAdmin;
    TokenMode mode = TokenMode::Full;
    std::wstring account;      // local account name, for Who::AnotherUser
};

// What actually happened. The UI shows the effective identity read back from the child's token,
// never what was requested; `downgraded` is set when they differ.
struct LaunchResult {
    bool         ok = false;
    DWORD        pid = 0;
    DWORD        sessionId = 0;
    std::wstring effectiveUser;    // domain\user from the child token's TokenUser
    DWORD        integrityRid = 0; // SECURITY_MANDATORY_*_RID read back from the child
    std::wstring integrityName;    // IntegrityName(integrityRid)
    std::wstring requestedLabel;   // e.g. "SYSTEM (Full)" — for the honest-downgrade line
    bool         downgraded = false;
    bool         startedService = false;   // TrustedInstaller: did we start the service
    std::wstring error;            // exact Win32 error text on failure (ok == false)
};

// ---- Pure helpers (no I/O; covered by --selftest) --------------------------------------------

// Quote one argument so CommandLineToArgvW round-trips it (MSDN "Parsing C++ Command-Line
// Arguments" rules: backslashes before a quote, and spaces/tabs/empty force quoting).
std::wstring QuoteArg(const std::wstring& arg);

// "<exe>" + (args.empty() ? "" : " " + args). exe is always quoted; args is passed through as the
// caller already composed it (the UI quotes individual args with QuoteArg before joining).
std::wstring BuildCommandLine(const std::wstring& exe, const std::wstring& args);

// GetFullPathNameW against `base` (or the process CWD when base is empty). Leaves a UNC or an
// already-absolute path alone. Never adds the \\?\ prefix (that is for the file APIs, not
// CreateProcess/ShellExecute).
std::wstring AbsolutePath(const std::wstring& path, const std::wstring& base = L"");

// "Untrusted" / "Low" / "Medium" / "High" / "System" / "Protected", else "0x…".
std::wstring IntegrityName(DWORD rid);

// mmc.exe command for a .msc snap-in: `"<system32>\mmc.exe" "<file>"`. system32 is a parameter so
// the builder is testable without SHGetFolderPath.
std::wstring MscCommand(const std::wstring& system32, const std::wstring& mscFile);

// The rule reviewers flagged in diskmap: AdjustTokenPrivileges can return TRUE yet leave the
// privilege unassigned. A privilege counts as enabled only when the call succeeded AND
// GetLastError() != ERROR_NOT_ALL_ASSIGNED.
bool PrivilegeEnabled(BOOL adjustReturn, DWORD lastError);

// The rule-4 gate: SYSTEM and TrustedInstaller launch only while the "Allow SYSTEM /
// TrustedInstaller" switch is on; This admin and Another user are always allowed.
bool LaunchAllowed(Who who, bool allowElevated);

// Split for CreateProcessWithLogonW: "domain\user" -> (user, domain); bare "user" -> (user, ".");
// a UPN "user@domain" stays whole with an empty domain, which the caller passes as NULL (per the API).
void SplitAccount(const std::wstring& account, std::wstring* user, std::wstring* domain);

// Expand a file-association command template (%1/%L = the file, %* = args, %% = %). The file is
// wrapped with QuoteArg unless the template already puts that placeholder inside quotes.
std::wstring ExpandAssoc(const std::wstring& tmpl, const std::wstring& file, const std::wstring& args);

// ---- Local accounts --------------------------------------------------------------------------

struct Account {
    std::wstring name;
    DWORD flags = 0;        // USER_INFO_1.usri1_flags (UF_ACCOUNTDISABLE, UF_NORMAL_ACCOUNT, …)
};

// Drops disabled accounts (UF_ACCOUNTDISABLE), non-normal accounts (temp duplicate / server trust
// / workstation trust) and the well-known built-in service accounts (Guest, DefaultAccount,
// WDAGUtilityAccount). Pure, so --selftest can check it on sample data.
std::vector<Account> FilterAccounts(const std::vector<Account>& all);

// Live enumeration via NetUserEnum(FILTER_NORMAL_ACCOUNT), then FilterAccounts. Empty on failure.
std::vector<Account> EnumerateLocalAccounts();

// ---- Last-run list (persisted, no passwords) -------------------------------------------------

struct LastRun {
    std::wstring path;
    std::wstring args;
    std::wstring account;     // only for Who::AnotherUser
    int  who = 0;             // Who
    int  mode = 0;            // TokenMode
    std::wstring integrity;   // the effective integrity that came back ("High", …)
    long long when = 0;       // Unix epoch seconds (0 = unknown)
};

// One escaped line per run; fields are unit-separated and the separator / backslash are escaped,
// so commas and Unicode survive. There is deliberately no password field.
std::wstring SerializeLastRun(const LastRun& r);
bool         ParseLastRun(const std::wstring& line, LastRun* out);

// Identity label for the list and the honest-downgrade line, e.g. "SYSTEM (Limited)" or
// "alice (Full)".
std::wstring IdentityLabel(int who, int mode, const std::wstring& account);

// ---- The launch itself (runs on a Worker; no UI) ---------------------------------------------

// Resolves the target to an absolute path, launches per `req.who`/`req.mode`, then reads the
// child's token back and fills `out`. `password` is used only for Who::AnotherUser and is the
// caller's responsibility to zero after Launch returns. Every failed Win32 call fills out->error.
void Launch(const LaunchRequest& req, const wchar_t* password, LaunchResult* out);

// Headless --launch-test: launch `cmd.exe /c exit 0` as This-admin-limited, read back the child's
// integrity, print it, return 0 iff it is Medium. Under Wine print SKIP and return 0.
int RunLaunchTest(const std::wstring& mode);

bool RunningUnderWine();

}  // namespace hx::runas
