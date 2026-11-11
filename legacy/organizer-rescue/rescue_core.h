// rescue_core.h - platform-independent analysis of NTFS change-journal records:
// finds the moves Organizer made into its category folders and works out where each item came from.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <algorithm>
#include <cstdint>
#include <cwctype>

namespace rc {
typedef uint64_t u64;
typedef uint32_t u32;

static const u32 R_RENAME_OLD = 0x00001000;   // USN_REASON_RENAME_OLD_NAME
static const u32 R_RENAME_NEW = 0x00002000;   // USN_REASON_RENAME_NEW_NAME
static const u32 A_DIRECTORY  = 0x00000010;   // FILE_ATTRIBUTE_DIRECTORY

struct Rec {
    u64 usn = 0, frn = 0, parent = 0, time = 0;
    u32 reason = 0, attr = 0;
    std::wstring name;
};

struct Move {
    u64 frn = 0;
    bool isDir = false;
    std::wstring origDir;    // folder it was in before Organizer moved it, relative to the drive root ("" = root)
    std::wstring origName;   // its name there
    std::wstring destDir;    // folder Organizer put it in (relative)
    std::wstring destName;
    u64 time = 0;
    int session = 0;
};

inline std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

inline const std::set<std::wstring>& CategoryFolders() {
    static const std::set<std::wstring> s = { L"images", L"videos", L"music", L"documents", L"discord exports", L"web pages",
        L"archives", L"disk images", L"programs", L"code", L"fonts", L"shortcuts", L"other" };
    return s;
}

// currentPath(frn, out): path (relative to root, "" for the root itself) of a folder that exists right now
struct Analyzer {
    u64 rootFrn = 0;
    std::function<bool(u64, std::wstring&)> currentPath;

    struct DirInfo { std::wstring name; u64 parent; };
    std::map<u64, DirInfo> firstSeen, live;
    std::map<u64, std::wstring> currentCache;
    std::map<u64, bool> currentFail;

    bool Resolve(u64 frn, std::wstring& out) {
        std::vector<std::wstring> parts;
        for (int guard = 0; guard < 128; guard++) {
            if (frn == rootFrn) break;
            auto l = live.find(frn);
            const DirInfo* d = nullptr;
            if (l != live.end()) d = &l->second;
            else { auto f = firstSeen.find(frn); if (f != firstSeen.end()) d = &f->second; }
            if (d) { parts.push_back(d->name); frn = d->parent; continue; }
            // never mentioned in the journal: the folder hasn't changed, so ask the file system where it is now
            auto cc = currentCache.find(frn);
            std::wstring cur;
            if (cc != currentCache.end()) cur = cc->second;
            else {
                if (currentFail.count(frn) || !currentPath || !currentPath(frn, cur)) { currentFail[frn] = true; return false; }
                currentCache[frn] = cur;
            }
            std::wstring s = cur;
            for (size_t i = parts.size(); i-- > 0;) s += (s.empty() ? L"" : L"\\") + parts[i];
            out = s;
            return true;
        }
        if (frn != rootFrn) return false;
        std::wstring s;
        for (size_t i = parts.size(); i-- > 0;) s += (s.empty() ? L"" : L"\\") + parts[i];
        out = s;
        return true;
    }

    static std::wstring LeafOf(const std::wstring& p) {
        size_t k = p.rfind(L'\\');
        return k == std::wstring::npos ? p : p.substr(k + 1);
    }

    std::vector<Move> Run(const std::vector<Rec>& recs, u64 sessionGap) {
        std::vector<Move> moves;
        for (auto& r : recs)
            if ((r.attr & A_DIRECTORY) && !firstSeen.count(r.frn)) firstSeen[r.frn] = { r.name, r.parent };
        std::map<u64, Rec> pending;
        std::set<u64> done;
        for (auto& r : recs) {
            if (r.reason & R_RENAME_OLD) pending[r.frn] = r;
            if ((r.reason & R_RENAME_NEW) && pending.count(r.frn)) {
                Rec old = pending[r.frn];
                pending.erase(r.frn);
                std::wstring newDir, oldDir;
                if (!done.count(r.frn) && Resolve(r.parent, newDir) && !newDir.empty() &&
                    CategoryFolders().count(Lower(LeafOf(newDir))) && Resolve(old.parent, oldDir) &&
                    Lower(oldDir) != Lower(newDir) && Lower(newDir).find(L"$recycle.bin") == std::wstring::npos) {
                    Move m;
                    m.frn = r.frn; m.isDir = (r.attr & A_DIRECTORY) != 0;
                    m.origDir = oldDir; m.origName = old.name;
                    m.destDir = newDir; m.destName = r.name;
                    m.time = r.time;
                    moves.push_back(m);
                    done.insert(r.frn);   // only the first move into a category folder counts
                }
            }
            if (r.attr & A_DIRECTORY) live[r.frn] = { r.name, r.parent };
        }
        std::sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) { return a.time < b.time; });
        int s = 0;
        for (size_t i = 0; i < moves.size(); i++) {
            if (i > 0 && moves[i].time - moves[i - 1].time > sessionGap) s++;
            moves[i].session = s;
        }
        return moves;
    }
};
}  // namespace rc
