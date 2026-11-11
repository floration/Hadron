// core/sys/update.h — self-update from GitHub releases. See docs/ARCHITECTURE.md "Updates".
//
// Each exe updates only itself: it reads the latest release of floration/Hadron, picks the asset whose
// name matches its own file name, checks it against the release's SHA256SUMS and its own VERSIONINFO,
// then swaps it in. The running exe is moved (never deleted) into <exe dir>\.hadron-old\<stamp>\, so the
// previous version stays on disk and the user can roll back by hand. Nothing here deletes a user file.
//
// Trust: HTTPS to github.com / *.githubusercontent.com only, plus the SHA-256 list from the same release.
// That catches corruption and a wrong asset, not a compromised GitHub account; the exes are not code-signed.
#pragma once

#include "core/sys/win_compat.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace hx::sys::update {

struct Version {
    int major = 0, minor = 0, patch = 0;
    bool ok = false;
};
Version ParseVersion(const std::wstring& s);            // "1.2.3" or "v1.2.3"; anything else => !ok
int Compare(const Version& a, const Version& b);        // <0, 0, >0
std::wstring ToString(const Version& v);

// This build's version. Dev builds (no HADRON_VERSION at configure time) never install an update.
Version Current();
bool IsDevBuild();

struct Release {
    std::wstring tag;           // "v0.2.0"
    Version version;
    std::wstring assetUrl;      // browser_download_url of the asset named like this exe ("" if none)
    uint64_t assetSize = 0;
    std::wstring sumsUrl;       // browser_download_url of SHA256SUMS ("" if none)
};

// Parses the JSON of GET /repos/{owner}/{repo}/releases/latest. exeName is matched case-insensitively.
bool ParseRelease(const std::string& json, const std::wstring& exeName, Release* out, std::wstring* err);

// Finds the hex SHA-256 for `name` in a sha256sum-format file ("<hex>  <name>" or "<hex> *<name>").
bool FindSum(const std::string& sums, const std::wstring& name, std::wstring* hex);

// Only https URLs on github.com, api.github.com or *.githubusercontent.com are fetched.
bool HostAllowed(const std::wstring& url);

enum class Outcome { UpToDate, NoRelease, Installed, Available, Failed, Cancelled };

struct Result {
    Outcome outcome = Outcome::Failed;
    Version latest;
    std::wstring message;       // one line for the status bar and the log
};

// Checks the latest release and, when `install` is set and it is newer, downloads, verifies and swaps it
// in. Blocking: run it on a Worker. `cancel` is polled between network reads; the swap itself is never
// interrupted once it starts. Dev builds report Available instead of installing.
Result CheckAndInstall(bool install, const std::atomic<bool>* cancel);

// True only for exactly https://github.com/floration/Hadron/releases/download/<tag>/<file> (case-insensitive).
bool PinnedUrl(const std::wstring& url, const std::wstring& tag, const std::wstring& file);

// Called from the UI thread: closes the request in flight so a blocking WinHTTP call returns at once.
void AbortNetwork();

// Headless checks of the parsing and comparison logic (no network). Prints and returns 0 on success.
int SelfTest();

}  // namespace hx::sys::update
