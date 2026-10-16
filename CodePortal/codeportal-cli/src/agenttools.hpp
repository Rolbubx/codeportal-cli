// agenttools.hpp : outils supplémentaires de l'assistant (appels d'outils natifs uniquement).
//
//   Fichiers   : read_file (par plage de lignes), tree, find_files, search_files, file_info, replace_lines,
//                insert_lines, append_file, copy_path, move_path, delete_dir, undo_last_change
//   Programmes : run_powershell, start_process, stop_process, list_processes, wait
//   Web        : fetch_url, download_file, web_search
//   Windows    : open_path, clipboard_read, clipboard_write, system_info, get_env
//   Agent      : calculate, update_plan, save_memory
//
// Les modifications de fichiers passent par pushBackup : /undo et undo_last_change les annulent.
#pragma once

#include "util.hpp"  // windows.h d'abord (winsock2.h, WIN32_LEAN_AND_MEAN)

#include <shellapi.h>
#include <winhttp.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "live.hpp"
#include "state.hpp"
#include "tools.hpp"
#include "util.hpp"

namespace xt {

// ---------------------------------------------------------------------------
// Petits outils communs
// ---------------------------------------------------------------------------
struct Args {
    const JVal& v;
    const JVal* get(const char* k) const {
        const JVal* x = v.type == JVal::OBJ ? v.find(k) : nullptr;
        return x && x->type != JVal::NUL ? x : nullptr;
    }
    std::string str(const char* k, const std::string& def = "") const {
        const JVal* x = get(k);
        return x ? x->str : def;
    }
    bool has(const char* k) const {
        const JVal* x = get(k);
        return x && !(x->type == JVal::STR && x->str.empty());
    }
    long long num(const char* k, long long def) const {
        const JVal* x = get(k);
        if (!x || x->str.empty()) return def;
        try {
            return (long long)std::stod(x->str);
        } catch (...) {
            return def;
        }
    }
    bool flag(const char* k, bool def = false) const {
        const JVal* x = get(k);
        if (!x) return def;
        return x->str == "true" || x->str == "1" || x->str == "yes";
    }
};

long long clampN(long long v, long long lo, long long hi) { return v < lo ? lo : v > hi ? hi : v; }

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

fs::path toPath(const std::string& p) { return fs::path(toWide(expandPath(p.empty() ? "." : p))); }

bool isBinary(const std::string& head) { return head.find('\0') != std::string::npos; }

std::string humanSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        ++u;
    }
    char buf[32];
    if (u == 0) snprintf(buf, sizeof buf, "%llu B", (unsigned long long)bytes);
    else snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    return buf;
}

// Chemin affiché relativement à la racine de la recherche.
std::string rel(const fs::path& p, const fs::path& base) {
    std::wstring s = p.wstring();
    const std::wstring b = base.wstring();
    if (!b.empty() && s.rfind(b, 0) == 0) {
        s = s.substr(b.size());
        while (!s.empty() && (s[0] == L'\\' || s[0] == L'/')) s.erase(0, 1);
    }
    if (s.size() > 2 && s[0] == L'.' && (s[1] == L'\\' || s[1] == L'/')) s.erase(0, 2);
    return toUtf8(s.empty() ? p.filename().wstring() : s);
}

// Dossiers qu'on ne parcourt pas (trop gros, ou sans intérêt pour le code).
bool skipDir(const fs::path& p) {
    static const std::set<std::string> skip = {".git", "node_modules", ".vs", "__pycache__", ".idea", ".venv",
                                               "venv", ".next", ".gradle", ".cache", ".svn", ".hg"};
    return skip.count(lower(p.filename().u8string())) > 0;
}

// Parcourt les fichiers sous `root` ; `fn` renvoie false pour arrêter. Échap arrête aussi.
void walkFiles(const fs::path& root, const std::function<bool(const fs::directory_entry&)>& fn) {
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (it->is_directory(e2)) {
            if (skipDir(it->path())) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(e2)) continue;
        if (!fn(*it)) break;
        if (LiveView::get().interrupted()) break;
    }
}

// Joker * et ?, sans casse ; / et \ sont équivalents.
bool wildMatch(const std::string& pat, const std::string& s) {
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    auto eq = [](char a, char b) {
        if ((a == '/' || a == '\\') && (b == '/' || b == '\\')) return true;
        return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    };
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || (pat[p] != '*' && eq(pat[p], s[i])))) {
            ++p;
            ++i;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

// « *.cpp;*.hpp » ou « *.cpp,*.hpp »
std::vector<std::string> splitGlobs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ';' || c == ',') {
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    if (out.empty()) out.push_back("*");
    return out;
}
bool matchAny(const std::vector<std::string>& globs, const std::string& name, const std::string& relPath) {
    for (const auto& g : globs) {
        const bool pathPattern = g.find('/') != std::string::npos || g.find('\\') != std::string::npos;
        if (wildMatch(g, pathPattern ? relPath : name)) return true;
    }
    return false;
}

std::string clipLine(const std::string& s, size_t max) {
    std::string t = trim(s);
    return t.size() > max ? utf8Prefix(t, max) + "…" : t;
}

std::string winTime(const FILETIME& ft) {
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return "?";
    char buf[32];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", local.wYear, local.wMonth, local.wDay, local.wHour,
             local.wMinute, local.wSecond);
    return buf;
}

std::string nowText() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[32];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return buf;
}

std::string tempFile(const std::wstring& prefix, const std::wstring& ext) {
    static std::atomic<int> counter{0};
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) wcscpy(dir, L".\\");
    const fs::path p = fs::path(dir) / (prefix + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                        std::to_wstring(++counter) + ext);
    return p.u8string();
}

ToolResult ok(const std::string& summary, const std::string& output = "") {
    ToolResult r;
    r.summary = summary;
    r.output = output;
    return r;
}

// ---------------------------------------------------------------------------
// Fichiers
// ---------------------------------------------------------------------------
ToolResult readFile(const Args& a) {
    const std::string path = a.str("path");
    if (path.empty()) return failTool("read_file needs a path.");
    const fs::path p = toPath(path);
    std::error_code ec;
    if (fs::is_directory(p, ec)) return failTool(path + " is a folder: use list_dir or tree.");
    const auto size = fs::file_size(p, ec);
    if (ec) return failTool("File not found: " + path);
    if (size > 30u * 1024 * 1024) return failTool(path + " is " + humanSize(size) + ": too big to read. Use search_files.");
    std::string text;
    if (!readText(p, text)) return failTool("Cannot read " + path);
    if (isBinary(text.substr(0, 8000))) return failTool(path + " is a binary file (" + humanSize(size) + ").");

    const auto lines = splitLines(text);
    const long long total = (long long)lines.size();
    long long start = clampN(a.num("start_line", 1), 1, std::max<long long>(1, total));
    long long end = a.has("end_line") ? a.num("end_line", total) : start + 499;
    end = clampN(end, start, std::max<long long>(start, total));
    if (total == 0) return ok("Read " + path + " (empty file)", "(empty file)\n");

    std::string out;
    for (long long n = start; n <= end && n <= total; ++n) {
        const std::string& l = lines[(size_t)n - 1];
        out += std::to_string(n) + "\t" + (l.size() > 3000 ? utf8Prefix(l, 3000) + "… [line cut]" : l) + "\n";
    }
    if (end < total)
        out += "[lines " + std::to_string(start) + "-" + std::to_string(end) + " of " + std::to_string(total) +
               ". Call read_file with start_line=" + std::to_string(end + 1) + " to continue.]\n";
    const bool whole = start == 1 && end >= total;
    return ok(whole ? "Read " + plural((size_t)total, "line")
                    : "Read lines " + std::to_string(start) + "-" + std::to_string(std::min(end, total)) + " of " + std::to_string(total),
              out);
}

ToolResult tree(const Args& a) {
    const fs::path root = toPath(a.str("path", "."));
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return failTool("Not a folder: " + a.str("path", "."));
    const int depth = (int)clampN(a.num("depth", 3), 1, 12);
    const size_t maxEntries = (size_t)clampN(a.num("max_entries", 400), 10, 5000);
    size_t count = 0;
    bool cut = false;
    std::string out = root.u8string() + "\n";
    std::function<void(const fs::path&, const std::string&, int)> walk = [&](const fs::path& dir, const std::string& indent, int level) {
        std::vector<std::pair<std::string, bool>> items;
        std::error_code e;
        for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, e)) {
            std::error_code e2;
            items.push_back({entry.path().filename().u8string(), entry.is_directory(e2)});
        }
        std::sort(items.begin(), items.end(), [](const auto& x, const auto& y) {
            if (x.second != y.second) return x.second;
            return lower(x.first) < lower(y.first);
        });
        for (const auto& [name, isDir] : items) {
            if (count >= maxEntries) {
                cut = true;
                return;
            }
            ++count;
            const fs::path child = dir / fs::path(toWide(name));
            const bool skipped = isDir && skipDir(child);
            out += indent + name + (isDir ? "/" : "") + (skipped ? "  (not listed)" : "") + "\n";
            if (isDir && !skipped && level < depth) walk(child, indent + "  ", level + 1);
            if (LiveView::get().interrupted()) return;
        }
    };
    walk(root, "  ", 1);
    if (cut) out += "[stopped after " + std::to_string(maxEntries) + " entries: use a deeper path or max_entries]\n";
    return ok(plural(count, "entry") + (cut ? " (cut)" : ""), out);
}

ToolResult findFiles(const Args& a) {
    const std::string pattern = a.str("pattern");
    if (pattern.empty()) return failTool("find_files needs a pattern, e.g. *.cpp");
    const fs::path root = toPath(a.str("path", "."));
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return failTool("Not a folder: " + a.str("path", "."));
    const size_t maxResults = (size_t)clampN(a.num("max_results", 200), 1, 5000);
    const auto globs = splitGlobs(pattern);
    std::string out;
    size_t n = 0;
    bool more = false;
    walkFiles(root, [&](const fs::directory_entry& e) {
        const std::string name = e.path().filename().u8string();
        const std::string r = rel(e.path(), root);
        if (!matchAny(globs, name, r)) return true;
        if (n >= maxResults) {
            more = true;
            return false;
        }
        ++n;
        out += r + "\n";
        return true;
    });
    if (!n) return ok("No files match " + pattern, "No files match " + pattern + " under " + root.u8string() + "\n");
    if (more) out += "[more files not shown: narrow the pattern or raise max_results]\n";
    return ok("Found " + plural(n, "file") + (more ? "+" : ""), out);
}

ToolResult searchFiles(const Args& a) {
    const std::string query = a.str("query");
    if (query.empty()) return failTool("search_files needs a query.");
    const fs::path root = toPath(a.str("path", "."));
    const auto globs = splitGlobs(a.str("glob", "*"));
    const bool useRegex = a.flag("regex"), caseSensitive = a.flag("case_sensitive");
    const size_t maxResults = (size_t)clampN(a.num("max_results", 100), 1, 2000);
    std::regex re;
    if (useRegex) {
        try {
            re = std::regex(query, caseSensitive ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase);
        } catch (const std::regex_error& e) {
            return failTool("Invalid regular expression: " + std::string(e.what()));
        }
    }
    const std::string needle = caseSensitive ? query : lower(query);
    std::string out;
    size_t matches = 0, files = 0, scanned = 0;
    bool more = false;

    auto searchOne = [&](const fs::path& file, const std::string& shown) {
        std::error_code e;
        if (fs::file_size(file, e) > 8u * 1024 * 1024) return true;
        std::string text;
        if (!readText(file, text) || isBinary(text.substr(0, 8000))) return true;
        ++scanned;
        bool hitFile = false;
        size_t lineNo = 0, pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            ++lineNo;
            bool hit;
            if (useRegex) hit = line.size() <= 5000 && std::regex_search(line, re);
            else hit = (caseSensitive ? line : lower(line)).find(needle) != std::string::npos;
            if (hit) {
                if (matches >= maxResults) {
                    more = true;
                    return false;
                }
                ++matches;
                if (!hitFile) {
                    ++files;
                    hitFile = true;
                }
                out += shown + ":" + std::to_string(lineNo) + ": " + clipLine(line, 300) + "\n";
            }
            if (nl >= text.size()) break;
            pos = nl + 1;
        }
        return true;
    };

    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        searchOne(root, root.filename().u8string());
    } else if (fs::is_directory(root, ec)) {
        walkFiles(root, [&](const fs::directory_entry& e) {
            const std::string r = rel(e.path(), root);
            if (!matchAny(globs, e.path().filename().u8string(), r)) return true;
            return searchOne(e.path(), r);
        });
    } else {
        return failTool("Not found: " + a.str("path", "."));
    }
    if (!matches)
        return ok("No matches for \"" + clipLine(query, 60) + "\"",
                  "No matches. Searched " + plural(scanned, "file") + " under " + root.u8string() + ".\n");
    if (more) out += "[more matches not shown: narrow the search or raise max_results]\n";
    return ok("Found " + plural(matches, "match") + " in " + plural(files, "file") + (more ? " (cut)" : ""), out);
}

ToolResult fileInfo(const Args& a) {
    const std::string path = a.str("path");
    if (path.empty()) return failTool("file_info needs a path.");
    const fs::path p = toPath(path);
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &d))
        return ok(path + " does not exist", "exists: no\n");
    const bool dir = d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    std::string out = "path: " + absolutePath(p) + "\nexists: yes\ntype: " + (dir ? "folder" : "file") + "\n";
    if (dir) {
        size_t n = 0;
        std::error_code ec;
        for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) ++n;
        out += "entries: " + std::to_string(n) + "\n";
    } else {
        const uint64_t size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        out += "size: " + humanSize(size) + " (" + std::to_string(size) + " bytes)\n";
        if (size < 5u * 1024 * 1024) {
            std::string text;
            if (readText(p, text)) {
                if (isBinary(text.substr(0, 8000))) out += "content: binary\n";
                else out += "lines: " + std::to_string(splitLines(text).size()) + "\nline endings: " +
                            (text.find("\r\n") != std::string::npos ? "CRLF" : "LF") + "\n";
            }
        }
    }
    out += "created: " + winTime(d.ftCreationTime) + "\nmodified: " + winTime(d.ftLastWriteTime) + "\n";
    if (d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) out += "read-only: yes\n";
    if (d.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) out += "hidden: yes\n";
    return ok((dir ? "Folder " : "File ") + path, out);
}

// Fichier texte découpé en lignes, avec ses fins de ligne d'origine.
struct TextFile {
    std::vector<std::string> lines;
    std::string eol = "\n";
    bool finalNewline = true;
};
bool loadLines(const fs::path& p, TextFile& tf, std::string& err) {
    std::string text;
    if (!readText(p, text)) {
        err = "Cannot read " + p.u8string();
        return false;
    }
    if (isBinary(text.substr(0, 8000))) {
        err = p.u8string() + " is a binary file.";
        return false;
    }
    tf.eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    tf.finalNewline = text.empty() || text.back() == '\n';
    tf.lines = splitLines(text);
    return true;
}
std::string joinLines(const TextFile& tf) {
    std::string o;
    for (size_t i = 0; i < tf.lines.size(); ++i) {
        o += tf.lines[i];
        if (i + 1 < tf.lines.size() || tf.finalNewline) o += tf.eol;
    }
    return o;
}

// Remplace les lignes start..end (incluses) par newText. insert = true : insère avant la ligne start.
ToolResult spliceLines(const std::string& path, long long start, long long end, const std::string& newText, bool insert) {
    if (path.empty()) return failTool("A path is needed.");
    const fs::path p = toPath(path);
    TextFile tf;
    std::string err;
    if (!loadLines(p, tf, err)) return failTool(err);
    const long long n = (long long)tf.lines.size();
    if (insert) {
        if (start < 1 || start > n + 1) return failTool("line must be between 1 and " + std::to_string(n + 1) + " (the file has " + plural((size_t)n, "line") + ").");
        end = start - 1;
    } else {
        if (start < 1 || start > n) return failTool("start_line must be between 1 and " + std::to_string(n) + ".");
        if (end < start) end = start;
        if (end > n) end = n;
    }
    std::vector<std::string> repl = splitLines(newText);
    if (!newText.empty() && repl.empty()) repl.push_back("");

    ToolResult r;
    for (long long ln = std::max<long long>(1, start - 2); ln < start; ++ln) r.diff.push_back({' ', (size_t)ln, tf.lines[(size_t)ln - 1]});
    for (long long ln = start; ln <= end; ++ln) r.diff.push_back({'-', (size_t)ln, tf.lines[(size_t)ln - 1]});
    for (size_t k = 0; k < repl.size(); ++k) r.diff.push_back({'+', (size_t)start + k, repl[k]});
    const long long shift = (long long)repl.size() - (end - start + 1);
    for (long long ln = end + 1; ln <= end + 2 && ln <= n; ++ln) r.diff.push_back({' ', (size_t)(ln + shift), tf.lines[(size_t)ln - 1]});

    tf.lines.erase(tf.lines.begin() + (start - 1), tf.lines.begin() + end);
    tf.lines.insert(tf.lines.begin() + (start - 1), repl.begin(), repl.end());
    pushBackup(p);
    if (!writeText(p, joinLines(tf))) return failTool("Cannot write " + path);
    if (insert) r.summary = "Inserted " + plural(repl.size(), "line") + " at line " + std::to_string(start) + " of " + path;
    else r.summary = "Replaced lines " + std::to_string(start) + "-" + std::to_string(end) + " of " + path + " (" +
                     plural((size_t)(end - start + 1), "removal") + ", " + plural(repl.size(), "addition") + ")";
    return r;
}

ToolResult appendFile(const Args& a) {
    const std::string path = a.str("path");
    if (path.empty()) return failTool("append_file needs a path.");
    const fs::path p = toPath(path);
    std::string text;
    const bool existed = readText(p, text);
    std::string add = a.str("content");
    if (existed && !text.empty() && text.back() != '\n' && !add.empty() && add.front() != '\n')
        add = (text.find("\r\n") != std::string::npos ? "\r\n" : "\n") + add;
    pushBackup(p);
    if (!writeText(p, text + add)) return failTool("Cannot write " + path);
    return ok("Appended " + plural(splitLines(a.str("content")).size(), "line") + " to " + path + (existed ? "" : " (new file)"));
}

ToolResult copyPath(const Args& a) {
    const std::string src = a.str("source"), dst = a.str("destination");
    if (src.empty() || dst.empty()) return failTool("copy_path needs a source and a destination.");
    fs::path s = toPath(src), d = toPath(dst);
    const bool overwrite = a.flag("overwrite");
    std::error_code ec;
    if (!fs::exists(s, ec)) return failTool("Not found: " + src);
    if (fs::is_directory(s, ec)) {
        if (fs::exists(d, ec) && !fs::is_directory(d, ec)) return failTool(dst + " is a file.");
        if (fs::exists(d, ec) && !overwrite) return failTool(dst + " already exists. Set overwrite=true to merge into it.");
        fs::copy(s, d, fs::copy_options::recursive | (overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::skip_existing), ec);
        if (ec) return failTool("Cannot copy " + src + ": " + ec.message());
        return ok("Copied folder " + src + " to " + dst);
    }
    if (fs::is_directory(d, ec)) d /= s.filename();
    if (fs::exists(d, ec) && !overwrite) return failTool(d.u8string() + " already exists. Set overwrite=true to replace it.");
    if (d.has_parent_path()) fs::create_directories(d.parent_path(), ec);
    pushBackup(d);
    fs::copy_file(s, d, overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
    if (ec) return failTool("Cannot copy " + src + ": " + ec.message());
    return ok("Copied " + src + " to " + d.u8string());
}

ToolResult movePath(const Args& a) {
    const std::string src = a.str("source"), dst = a.str("destination");
    if (src.empty() || dst.empty()) return failTool("move_path needs a source and a destination.");
    fs::path s = toPath(src), d = toPath(dst);
    const bool overwrite = a.flag("overwrite");
    std::error_code ec;
    if (!fs::exists(s, ec)) return failTool("Not found: " + src);
    const bool dir = fs::is_directory(s, ec);
    if (!dir && fs::is_directory(d, ec)) d /= s.filename();
    if (fs::exists(d, ec) && !overwrite) return failTool(d.u8string() + " already exists. Set overwrite=true to replace it.");
    if (d.has_parent_path()) fs::create_directories(d.parent_path(), ec);
    if (!dir) {  // deux sauvegardes : l'annulation remet la source, puis efface (ou restaure) la destination
        pushBackup(d);
        pushBackup(s);
    }
    ec.clear();
    fs::rename(s, d, ec);
    if (ec) {  // autre disque : copie puis suppression
        std::error_code e2;
        if (dir) fs::copy(s, d, fs::copy_options::recursive | fs::copy_options::overwrite_existing, e2);
        else fs::copy_file(s, d, fs::copy_options::overwrite_existing, e2);
        if (e2) return failTool("Cannot move " + src + ": " + e2.message());
        if (dir) fs::remove_all(s, e2);
        else fs::remove(s, e2);
    }
    return ok("Moved " + src + " to " + d.u8string());
}

// Dossiers qu'on refuse de supprimer : racines, dossiers de l'utilisateur et du système, dossier de travail.
bool protectedFolder(const fs::path& p, std::string& why) {
    std::error_code ec;
    std::wstring abs = fs::absolute(p, ec).lexically_normal().wstring();
    for (auto& c : abs) c = (wchar_t)towlower(c);
    while (abs.size() > 3 && (abs.back() == L'\\' || abs.back() == L'/')) abs.pop_back();
    if (abs.size() <= 3) {
        why = "a drive root";
        return true;
    }
    auto norm = [](std::wstring s) {
        for (auto& c : s) c = (wchar_t)towlower(c);
        while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
        return s;
    };
    const wchar_t* vars[] = {L"USERPROFILE", L"SystemRoot", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramData",
                             L"APPDATA", L"LOCALAPPDATA", L"PUBLIC"};
    for (const wchar_t* v : vars) {
        wchar_t buf[MAX_PATH];
        if (GetEnvironmentVariableW(v, buf, MAX_PATH) && norm(buf) == abs) {
            why = "a system or user folder";
            return true;
        }
    }
    wchar_t profile[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
        for (const wchar_t* sub : {L"\\Desktop", L"\\Documents", L"\\Downloads", L"\\Pictures", L"\\Music", L"\\Videos", L"\\OneDrive"})
            if (norm(std::wstring(profile) + sub) == abs) {
                why = "one of your main user folders";
                return true;
            }
    }
    const std::wstring cwd = norm(toWide(g.cwd));
    if (cwd == abs) {
        why = "the working folder";
        return true;
    }
    if (cwd.rfind(abs + L"\\", 0) == 0) {
        why = "a parent of the working folder";
        return true;
    }
    return false;
}

ToolResult deleteDir(const Args& a) {
    const std::string path = a.str("path");
    if (path.empty()) return failTool("delete_dir needs a path.");
    const fs::path p = toPath(path);
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return failTool("Not a folder: " + path);
    std::string why;
    if (protectedFolder(p, why)) return failTool("Refused: " + path + " is " + why + ".");
    if (!a.flag("recursive")) {
        if (!fs::remove(p, ec)) return failTool(path + " is not empty. Set recursive=true to delete it with everything inside.");
        return ok("Deleted empty folder " + path);
    }
    const auto n = fs::remove_all(p, ec);
    if (ec) return failTool("Could not delete everything in " + path + ": " + ec.message());
    return ok("Deleted folder " + path + " (" + plural((size_t)n, "item") + ")");
}

ToolResult undoChange() {
    bool done = false;
    const std::string msg = undoLast(&done);
    if (msg.empty()) return failTool("Nothing to undo.");
    if (!done) return failTool(msg);
    return ok(msg);
}

// ---------------------------------------------------------------------------
// Programmes
// ---------------------------------------------------------------------------
std::function<bool()> cancelled() {
    return [] { return LiveView::get().interrupted(); };
}

ToolResult runPowerShell(const Args& a) {
    const std::string script = a.str("script");
    if (trim(script).empty()) return failTool("run_powershell needs a script.");
    const int timeout = (int)clampN(a.num("timeout_seconds", 120), 5, 600);
    const std::string file = tempFile(L"codeportal-", L".ps1");
    // BOM UTF-8 : PowerShell 5 lit alors correctement les accents du script.
    const std::string body = "\xEF\xBB\xBF$ProgressPreference = 'SilentlyContinue'\r\n"
                             "[Console]::OutputEncoding = [System.Text.Encoding]::UTF8\r\n"
                             "$OutputEncoding = [System.Text.Encoding]::UTF8\r\n" +
                             script + "\r\n";
    if (!writeText(fs::path(toWide(file)), body)) return failTool("Cannot write the temporary script.");
    int code = -1;
    bool stopped = false;
    std::string out = runProcess("powershell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + file + "\"",
                                 timeout, cancelled(), &code, &stopped);
    std::error_code ec;
    fs::remove(fs::path(toWide(file)), ec);
    ToolResult r;
    r.output = out + (stopped ? "" : "(exit code " + std::to_string(code) + ")\n");
    r.ok = !stopped && code == 0;
    r.alwaysShow = true;
    if (!r.ok) r.summary = stopped ? "Stopped" : "Exit code " + std::to_string(code);
    return r;
}

ToolResult startProcess(const Args& a) {
    const std::string command = a.str("command");
    if (trim(command).empty()) return failTool("start_process needs a command.");
    const bool visible = a.flag("visible");
    std::wstring dir = a.has("working_dir") ? toPath(a.str("working_dir")).wstring() : L"";
    const std::string log = visible ? "" : tempFile(L"codeportal-process-", L".log");

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = INVALID_HANDLE_VALUE, in = INVALID_HANDLE_VALUE;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (!visible) {
        out = CreateFileW(toWide(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        if (out == INVALID_HANDLE_VALUE) return failTool("Cannot create the log file.");
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = out;
        si.hStdInput = in;
    }
    std::wstring cmdline = L"cmd.exe /d /s /c \"chcp 65001>nul & " + toWide(command) + L"\"";
    PROCESS_INFORMATION pi{};
    const DWORD flags = visible ? CREATE_NEW_CONSOLE : (CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP);
    const BOOL started = CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, visible ? FALSE : TRUE, flags, nullptr,
                                        dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (!started) return failTool("Could not start: " + command);
    const DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::string info = "pid: " + std::to_string(pid) + "\n";
    if (!visible) info += "output log: " + log + "\nRead the log with read_file. Stop it with stop_process.\n";
    else info += "It runs in its own window. Stop it with stop_process.\n";
    return ok("Started (PID " + std::to_string(pid) + ")", info);
}

ToolResult stopProcess(const Args& a) {
    const long long pid = a.num("pid", 0);
    if (pid <= 0) return failTool("stop_process needs a pid (see list_processes).");
    if ((DWORD)pid == GetCurrentProcessId()) return failTool("Refused: that is CodePortal itself.");
    int code = -1;
    std::string out = runProcess("taskkill /PID " + std::to_string(pid) + " /T /F", 30, nullptr, &code, nullptr);
    ToolResult r = ok(code == 0 ? "Stopped process " + std::to_string(pid) : "Could not stop process " + std::to_string(pid), out);
    r.ok = code == 0;
    return r;
}

ToolResult listProcesses(const Args& a) {
    const std::string filter = lower(a.str("filter"));
    int code = -1;
    const std::string csv = runProcess("tasklist /FO CSV /NH", 30, nullptr, &code, nullptr);
    struct Proc {
        std::string name;
        long long pid;
        long long memKb;
    };
    std::vector<Proc> procs;
    for (const auto& line : splitLines(csv)) {
        std::vector<std::string> f;
        std::string cur;
        bool q = false;
        for (char c : line) {
            if (c == '"') q = !q;
            else if (c == ',' && !q) {
                f.push_back(cur);
                cur.clear();
            } else cur += c;
        }
        f.push_back(cur);
        if (f.size() < 5) continue;
        long long pid = 0, mem = 0;
        try {
            pid = std::stoll(f[1]);
        } catch (...) {
            continue;
        }
        std::string digits;
        for (char c : f[4])
            if (std::isdigit((unsigned char)c)) digits += c;
        if (!digits.empty()) mem = std::stoll(digits);
        if (!filter.empty() && lower(f[0]).find(filter) == std::string::npos) continue;
        procs.push_back({f[0], pid, mem});
    }
    std::sort(procs.begin(), procs.end(), [](const Proc& x, const Proc& y) { return x.memKb > y.memKb; });
    const size_t limit = (size_t)clampN(a.num("max_results", 60), 1, 500);
    std::string out = "PID\tMEMORY\tNAME\n";
    for (size_t i = 0; i < procs.size() && i < limit; ++i)
        out += std::to_string(procs[i].pid) + "\t" + humanSize((uint64_t)procs[i].memKb * 1024) + "\t" + procs[i].name + "\n";
    if (procs.size() > limit) out += "[" + std::to_string(procs.size() - limit) + " more not shown]\n";
    return ok(plural(procs.size(), "process") + (filter.empty() ? "" : " matching \"" + filter + "\""), out);
}

ToolResult waitFor(const Args& a) {
    const long long secs = clampN(a.num("seconds", 5), 1, 300);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
    while (std::chrono::steady_clock::now() < until) {
        if (LiveView::get().interrupted()) return failTool("Wait stopped by the user.");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return ok("Waited " + std::to_string(secs) + "s");
}

// ---------------------------------------------------------------------------
// Web
// ---------------------------------------------------------------------------
struct HttpGet {
    DWORD status = 0;
    std::string body, contentType, error;
    bool truncated = false;
};

std::string winHttpError(DWORD e) {
    switch (e) {
        case 12002: return "timed out";
        case 12007: return "could not resolve the host name";
        case 12029: return "could not connect";
        case 12030: return "connection lost";
        case 12175: return "TLS/certificate error";
        default: return "error " + std::to_string(e);
    }
}

HttpGet httpGet(const std::string& url, size_t maxBytes) {
    HttpGet res;
    const std::wstring w = toWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(w.c_str(), (DWORD)w.size(), 0, &uc)) {
        res.error = "Invalid URL: " + url;
        return res;
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && uc.nScheme != INTERNET_SCHEME_HTTP) {
        res.error = "Only http:// and https:// URLs are supported.";
        return res;
    }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path = std::wstring(uc.lpszUrlPath ? uc.lpszUrlPath : L"", uc.dwUrlPathLength) +
                        std::wstring(uc.lpszExtraInfo ? uc.lpszExtraInfo : L"", uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    HINTERNET s = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                              L"Chrome/124.0 Safari/537.36 CodePortal/1.1",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {
        res.error = "WinHttpOpen failed";
        return res;
    }
    WinHttpSetTimeouts(s, 10000, 10000, 20000, 60000);
#ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(s, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof dec);
#endif
    HINTERNET c = WinHttpConnect(s, host.c_str(), uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    auto closeAll = [&] {
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        WinHttpCloseHandle(s);
    };
    if (!r) {
        res.error = "Cannot connect to " + toUtf8(host);
        closeAll();
        return res;
    }
    const wchar_t* headers = L"Accept: text/html,application/xhtml+xml,application/json,text/plain;q=0.9,*/*;q=0.8\r\n"
                             L"Accept-Language: en-US,en;q=0.9,fr;q=0.8\r\n";
    BOOL sent = WinHttpSendRequest(r, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (sent) sent = WinHttpReceiveResponse(r, nullptr);
    if (!sent) {
        res.error = "Request to " + toUtf8(host) + " failed: " + winHttpError(GetLastError());
        closeAll();
        return res;
    }
    DWORD len = sizeof res.status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &res.status,
                        &len, WINHTTP_NO_HEADER_INDEX);
    wchar_t ct[512];
    DWORD ctLen = sizeof ct;
    if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ct, &ctLen, WINHTTP_NO_HEADER_INDEX))
        res.contentType = toUtf8(std::wstring(ct, ctLen / sizeof(wchar_t)));
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(r, &avail) && avail > 0) {
        if (res.body.size() + avail > maxBytes) {
            res.truncated = true;
            avail = (DWORD)(maxBytes - res.body.size());
            if (!avail) break;
        }
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(r, &chunk[0], avail, &read) || read == 0) break;
        res.body.append(chunk, 0, read);
        if (res.truncated) break;
        if (LiveView::get().interrupted()) {
            res.error = "Stopped by the user.";
            break;
        }
    }
    closeAll();
    return res;
}

std::string utf8Encode(unsigned long cp) {
    std::string o;
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) {
        o += (char)(0xC0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        o += (char)(0xF0 | (cp >> 18));
        o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

std::string decodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += '&';
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        std::string rep;
        if (ent == "amp") rep = "&";
        else if (ent == "lt") rep = "<";
        else if (ent == "gt") rep = ">";
        else if (ent == "quot") rep = "\"";
        else if (ent == "apos") rep = "'";
        else if (ent == "nbsp") rep = " ";
        else if (ent == "mdash") rep = "—";
        else if (ent == "ndash") rep = "–";
        else if (ent == "hellip") rep = "…";
        else if (ent == "rsquo" || ent == "lsquo") rep = "'";
        else if (ent == "ldquo" || ent == "rdquo") rep = "\"";
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = 0;
            try {
                cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::stoul(ent.substr(2), nullptr, 16)
                                                                         : std::stoul(ent.substr(1));
            } catch (...) {
                cp = 0;
            }
            if (cp) rep = utf8Encode(cp);
        }
        if (rep.empty()) {  // lettres accentuées et symboles courants
            static const std::vector<std::pair<const char*, unsigned long>> named = {
                {"eacute", 0xE9}, {"egrave", 0xE8}, {"ecirc", 0xEA}, {"euml", 0xEB}, {"agrave", 0xE0}, {"aacute", 0xE1},
                {"acirc", 0xE2}, {"auml", 0xE4}, {"atilde", 0xE3}, {"aring", 0xE5}, {"ccedil", 0xE7}, {"igrave", 0xEC},
                {"iacute", 0xED}, {"icirc", 0xEE}, {"iuml", 0xEF}, {"ntilde", 0xF1}, {"ograve", 0xF2}, {"oacute", 0xF3},
                {"ocirc", 0xF4}, {"otilde", 0xF5}, {"ouml", 0xF6}, {"oslash", 0xF8}, {"ugrave", 0xF9}, {"uacute", 0xFA},
                {"ucirc", 0xFB}, {"uuml", 0xFC}, {"yacute", 0xFD}, {"yuml", 0xFF}, {"szlig", 0xDF}, {"oelig", 0x153},
                {"Eacute", 0xC9}, {"Egrave", 0xC8}, {"Ecirc", 0xCA}, {"Agrave", 0xC0}, {"Acirc", 0xC2}, {"Ccedil", 0xC7},
                {"Ocirc", 0xD4}, {"Ouml", 0xD6}, {"Uuml", 0xDC}, {"Auml", 0xC4}, {"OElig", 0x152}, {"copy", 0xA9},
                {"reg", 0xAE}, {"trade", 0x2122}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"euro", 0x20AC}, {"pound", 0xA3},
                {"times", 0xD7}, {"divide", 0xF7}, {"deg", 0xB0}, {"middot", 0xB7}, {"bull", 0x2022}, {"sect", 0xA7},
                {"para", 0xB6}, {"plusmn", 0xB1}, {"rarr", 0x2192}, {"larr", 0x2190}, {"iexcl", 0xA1}, {"iquest", 0xBF}};
            for (const auto& [n, cp] : named)
                if (ent == n) rep = utf8Encode(cp);
        }
        if (rep.empty()) {
            out += '&';
            continue;
        }
        out += rep;
        i = semi;
    }
    return out;
}

// Espaces regroupés, au plus une ligne vide de suite.
std::string collapseSpace(const std::string& in) {
    std::string out;
    int newlines = 0;
    bool space = false;
    for (char c : in) {
        if (c == '\r') continue;
        if (c == '\n') {
            ++newlines;
            space = false;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
            space = true;
            continue;
        }
        if (newlines && !out.empty()) out += newlines > 1 ? "\n\n" : "\n";
        else if (space && !out.empty()) out += ' ';
        newlines = 0;
        space = false;
        out += c;
    }
    return out;
}

// Texte lisible d'une page HTML : scripts et styles retirés, blocs séparés par des retours à la ligne.
std::string htmlToText(const std::string& html, std::string* title) {
    const std::string low = lower(html);
    if (title) {
        size_t t = low.find("<title");
        if (t != std::string::npos && (t = low.find('>', t)) != std::string::npos) {
            const size_t e = low.find("</title>", t);
            if (e != std::string::npos) *title = trim(collapseSpace(decodeEntities(html.substr(t + 1, e - t - 1))));
        }
    }
    static const std::set<std::string> blocks = {"p", "div", "br", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6",
                                                 "section", "article", "header", "footer", "ul", "ol", "table", "pre",
                                                 "blockquote", "hr", "main", "nav", "dd", "dt", "figcaption", "form"};
    static const std::set<std::string> dropped = {"script", "style", "noscript", "svg", "template", "head", "iframe"};
    std::string out;
    out.reserve(html.size() / 2);
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') {
            out += html[i++];
            continue;
        }
        if (html.compare(i, 4, "<!--") == 0) {
            const size_t e = html.find("-->", i + 4);
            i = e == std::string::npos ? html.size() : e + 3;
            continue;
        }
        const size_t e = html.find('>', i);
        if (e == std::string::npos) break;
        const std::string tag = low.substr(i + 1, e - i - 1);
        i = e + 1;
        if (tag.empty()) continue;
        const bool closing = tag[0] == '/';
        std::string name;
        for (size_t k = closing ? 1 : 0; k < tag.size() && std::isalnum((unsigned char)tag[k]); ++k) name += tag[k];
        if (!closing && dropped.count(name)) {
            const size_t end = low.find("</" + name, i);
            const size_t gt = end == std::string::npos ? std::string::npos : low.find('>', end);
            i = gt == std::string::npos ? html.size() : gt + 1;
            continue;
        }
        if (blocks.count(name) && !(closing && (name == "li" || name == "tr" || name == "dt" || name == "dd"))) out += "\n";
        if (!closing && name == "li") out += "- ";
        if (!closing && (name == "td" || name == "th")) out += "  ";
    }
    return trim(collapseSpace(decodeEntities(out)));
}

std::string urlEncode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else if (c == ' ') o += '+';
        else {
            char buf[4];
            snprintf(buf, sizeof buf, "%%%02X", c);
            o += buf;
        }
    }
    return o;
}
std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::string withScheme(const std::string& url) {
    const std::string t = trim(url);
    if (lower(t).rfind("http://", 0) == 0 || lower(t).rfind("https://", 0) == 0) return t;
    return "https://" + t;
}

ToolResult fetchUrl(const Args& a) {
    const std::string url = withScheme(a.str("url"));
    if (url == "https://") return failTool("fetch_url needs a url.");
    const size_t maxChars = (size_t)clampN(a.num("max_chars", 20000), 1000, 200000);
    const HttpGet res = httpGet(url, 6u * 1024 * 1024);
    if (!res.error.empty()) return failTool(res.error);
    const std::string ct = lower(res.contentType);
    const bool html = ct.find("html") != std::string::npos ||
                      (ct.empty() && lower(res.body.substr(0, 200)).find("<html") != std::string::npos);
    if (res.status >= 400) {
        ToolResult r = failTool("HTTP " + std::to_string(res.status) + " for " + url);
        r.output = truncate(html ? htmlToText(res.body, nullptr) : res.body, 2000);
        return r;
    }
    if (isBinary(res.body.substr(0, 4000)) && ct.find("text") == std::string::npos && ct.find("json") == std::string::npos)
        return ok("Binary content (" + (ct.empty() ? std::string("unknown type") : res.contentType) + ", " + humanSize(res.body.size()) + ")",
                  "This is not text. Use download_file to save it.\n");
    std::string title;
    std::string text = html && !a.flag("raw") ? htmlToText(res.body, &title) : res.body;
    const size_t total = text.size();
    if (total > maxChars) text = utf8Prefix(text, maxChars) + "\n[... " + std::to_string(total - maxChars) + " more characters. Raise max_chars to read more.]";
    if (res.truncated) text += "\n[download stopped at 6 MB]";
    return ok("Fetched " + url + " (HTTP " + std::to_string(res.status) + ", " + humanSize(res.body.size()) +
                  (title.empty() ? "" : ": " + clipLine(title, 80)) + ")",
              (title.empty() ? "" : "Title: " + title + "\n\n") + text + "\n");
}

ToolResult downloadFile(const Args& a) {
    const std::string url = withScheme(a.str("url"));
    std::string path = a.str("path");
    if (url == "https://" || path.empty()) return failTool("download_file needs a url and a path.");
    fs::path p = toPath(path);
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        std::string name = url.substr(url.find("://") + 3);
        name = name.substr(0, name.find_first_of("?#"));
        name = name.substr(name.find_last_of('/') + 1);
        p /= fs::path(toWide(name.empty() ? "download" : urlDecode(name)));
    }
    if (fs::exists(p, ec) && !a.flag("overwrite")) return failTool(p.u8string() + " already exists. Set overwrite=true to replace it.");
    const HttpGet res = httpGet(url, 200u * 1024 * 1024);
    if (!res.error.empty()) return failTool(res.error);
    if (res.status >= 400) return failTool("HTTP " + std::to_string(res.status) + " for " + url);
    if (res.truncated) return failTool("The file is bigger than 200 MB: not downloaded.");
    pushBackup(p);
    if (!writeText(p, res.body)) return failTool("Cannot write " + p.u8string());
    return ok("Downloaded " + humanSize(res.body.size()) + " to " + p.u8string());
}

std::string attr(const std::string& tag, const std::string& name) {
    for (const char q : {'"', '\''}) {
        const std::string key = name + "=" + q;
        const size_t a = tag.find(key);
        if (a == std::string::npos) continue;
        const size_t b = tag.find(q, a + key.size());
        if (b != std::string::npos) return tag.substr(a + key.size(), b - a - key.size());
    }
    return "";
}
std::string inlineText(const std::string& html) { return trim(collapseSpace(htmlToText(html, nullptr))); }

std::string base64Decode(std::string s) {  // accepte aussi la variante « URL » (- et _)
    std::string out;
    unsigned int val = 0;
    int bits = -8;
    for (char ch : s) {
        const unsigned char c = (unsigned char)(ch == '-' ? '+' : ch == '_' ? '/' : ch);
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else break;
        val = ((val << 6) | (unsigned)d) & 0xFFFFFF;
        bits += 6;
        if (bits >= 0) {
            out += (char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

// Paramètre d'URL (« u » dans https://...?a=1&u=xyz).
std::string queryParam(const std::string& url, const std::string& name) {
    for (const std::string sep : {"?", "&"}) {
        const size_t a = url.find(sep + name + "=");
        if (a == std::string::npos) continue;
        const size_t start = a + sep.size() + name.size() + 1;
        return url.substr(start, url.find('&', start) - start);
    }
    return "";
}

struct SearchHit {
    std::string title, url, snippet;
};

// Résultats de la page HTML de DuckDuckGo.
std::vector<SearchHit> parseDuckDuckGo(const std::string& h, size_t maxResults) {
    std::vector<SearchHit> hits;
    size_t pos = 0;
    while (hits.size() < maxResults && (pos = h.find("class=\"result__a\"", pos)) != std::string::npos) {
        const size_t tagStart = h.rfind('<', pos), tagEnd = h.find('>', pos);
        if (tagStart == std::string::npos || tagEnd == std::string::npos) break;
        std::string href = decodeEntities(attr(h.substr(tagStart, tagEnd - tagStart), "href"));
        const size_t close = h.find("</a>", tagEnd);
        if (close == std::string::npos) break;
        SearchHit hit;
        hit.title = inlineText(h.substr(tagEnd + 1, close - tagEnd - 1));
        pos = close;
        const size_t next = h.find("class=\"result__a\"", close);
        const size_t sn = h.find("class=\"result__snippet\"", close);
        if (sn != std::string::npos && (next == std::string::npos || sn < next)) {
            const size_t se = h.find('>', sn);
            const size_t sc = se == std::string::npos ? std::string::npos : h.find("</a>", se);
            if (sc != std::string::npos) hit.snippet = inlineText(h.substr(se + 1, sc - se - 1));
        }
        const std::string target = queryParam(href, "uddg");
        if (!target.empty()) href = urlDecode(target);
        if (href.rfind("//", 0) == 0) href = "https:" + href;
        if (href.empty() || hit.title.empty() || href.find("duckduckgo.com/y.js") != std::string::npos) continue;  // publicités
        hit.url = href;
        hits.push_back(hit);
    }
    return hits;
}

// Résultats de la page de Bing (liens de suivi « bing.com/ck/a?...&u=a1<base64> » décodés).
std::vector<SearchHit> parseBing(const std::string& h, size_t maxResults) {
    std::vector<SearchHit> hits;
    const std::string marker = "<li class=\"b_algo\"";
    size_t pos = h.find(marker);
    while (hits.size() < maxResults && pos != std::string::npos) {
        const size_t next = h.find(marker, pos + marker.size());
        const std::string block = h.substr(pos, (next == std::string::npos ? h.size() : next) - pos);
        pos = next;
        const size_t h2 = block.find("<h2");
        if (h2 == std::string::npos) continue;
        const size_t aStart = block.find("<a ", h2), aEnd = aStart == std::string::npos ? std::string::npos : block.find('>', aStart);
        const size_t aClose = aEnd == std::string::npos ? std::string::npos : block.find("</a>", aEnd);
        if (aClose == std::string::npos) continue;
        SearchHit hit;
        std::string href = decodeEntities(attr(block.substr(aStart, aEnd - aStart), "href"));
        hit.title = inlineText(block.substr(aEnd + 1, aClose - aEnd - 1));
        const std::string u = queryParam(href, "u");
        if (href.find("bing.com/ck/a") != std::string::npos && u.rfind("a1", 0) == 0) href = base64Decode(u.substr(2));
        const size_t p = block.find("<p", aClose);
        if (p != std::string::npos) {
            const size_t pe = block.find('>', p), pc = pe == std::string::npos ? std::string::npos : block.find("</p>", pe);
            if (pc != std::string::npos) hit.snippet = inlineText(block.substr(pe + 1, pc - pe - 1));
        }
        if (href.rfind("http", 0) != 0 || hit.title.empty()) continue;
        hit.url = href;
        hits.push_back(hit);
    }
    return hits;
}

// Recherche sur le web sans clé d'API : DuckDuckGo, puis Bing si DuckDuckGo refuse ou ne trouve rien.
ToolResult webSearch(const Args& a) {
    const std::string query = trim(a.str("query"));
    if (query.empty()) return failTool("web_search needs a query.");
    const size_t maxResults = (size_t)clampN(a.num("max_results", 8), 1, 20);
    std::vector<SearchHit> hits;
    std::string source, problem;
    bool answered = false;  // au moins un moteur a répondu normalement
    const HttpGet ddg = httpGet("https://html.duckduckgo.com/html/?q=" + urlEncode(query), 3u * 1024 * 1024);
    if (ddg.error.empty() && ddg.status == 200) {
        answered = true;
        hits = parseDuckDuckGo(ddg.body, maxResults);
        source = "DuckDuckGo";
    } else {
        problem = "DuckDuckGo: " + (ddg.error.empty() ? "HTTP " + std::to_string(ddg.status) : ddg.error);
    }
    if (hits.empty()) {
        const HttpGet bing = httpGet("https://www.bing.com/search?q=" + urlEncode(query) + "&setlang=en", 3u * 1024 * 1024);
        if (bing.error.empty() && bing.status == 200) {
            answered = true;
            hits = parseBing(bing.body, maxResults);
            source = "Bing";
        } else {
            problem += std::string(problem.empty() ? "" : "; ") + "Bing: " + (bing.error.empty() ? "HTTP " + std::to_string(bing.status) : bing.error);
        }
    }
    if (hits.empty()) {
        if (answered) return ok("No results for \"" + clipLine(query, 60) + "\"", "No results.\n");
        return failTool("Web search failed (" + problem + "). Try again later, or use fetch_url on a known site.");
    }
    std::string out;
    for (size_t i = 0; i < hits.size(); ++i)
        out += std::to_string(i + 1) + ". " + hits[i].title + "\n   " + hits[i].url + "\n" +
               (hits[i].snippet.empty() ? "" : "   " + hits[i].snippet + "\n");
    return ok(plural(hits.size(), "result") + " for \"" + clipLine(query, 60) + "\" (" + source + ")",
              out + "Use fetch_url to read a page.\n");
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------
ToolResult openPath(const Args& a) {
    std::string target = trim(a.str("target"));
    if (target.empty()) return failTool("open_path needs a target (file, folder or URL).");
    const std::string lt = lower(target);
    const bool url = lt.rfind("http://", 0) == 0 || lt.rfind("https://", 0) == 0 || lt.rfind("mailto:", 0) == 0;
    if (!url) {
        const fs::path p = toPath(target);
        std::error_code ec;
        if (!fs::exists(p, ec)) return failTool("Not found: " + target);
        target = absolutePath(p);
    }
    const INT_PTR rc = (INT_PTR)ShellExecuteW(nullptr, L"open", toWide(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (rc <= 32) return failTool("Windows could not open " + target + " (code " + std::to_string((long long)rc) + ").");
    return ok("Opened " + target);
}

bool openClipboard() {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(nullptr)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

ToolResult clipboardRead() {
    if (!openClipboard()) return failTool("The clipboard is busy.");
    std::string text;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
            text = toUtf8(w);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (text.empty()) return ok("The clipboard has no text", "(no text in the clipboard)\n");
    return ok("Read " + plural(text.size(), "character") + " from the clipboard", truncate(text, 50000) + "\n");
}

ToolResult clipboardWrite(const Args& a) {
    const std::wstring w = toWide(a.str("text"));
    if (!openClipboard()) return failTool("The clipboard is busy.");
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    bool done = false;
    if (mem) {
        if (void* dst = GlobalLock(mem)) {
            memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            done = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!done) GlobalFree(mem);
    }
    CloseClipboard();
    if (!done) return failTool("Could not write to the clipboard.");
    return ok("Copied " + plural(w.size(), "character") + " to the clipboard");
}

std::string regString(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t buf[512];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return toUtf8(buf);
    return "";
}

ToolResult systemInfo() {
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string product = regString(HKEY_LOCAL_MACHINE, nt, L"ProductName");
    const std::string build = regString(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild");
    const std::string version = regString(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion");
    int buildNo = 0;
    try {
        buildNo = std::stoi(build);
    } catch (...) {
    }
    if (buildNo >= 22000 && product.find("Windows 10") != std::string::npos)  // le registre dit encore « 10 » sur Windows 11
        product.replace(product.find("Windows 10"), 10, "Windows 11");
    std::string out = "os: " + product + (version.empty() ? "" : " " + version) + (build.empty() ? "" : " (build " + build + ")") + "\n";
    wchar_t name[256];
    DWORD len = 256;
    if (GetComputerNameW(name, &len)) out += "computer: " + toUtf8(name) + "\n";
    const char* user = std::getenv("USERNAME");
    out += "user: " + std::string(user ? user : "?") + "\n";
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const std::string cpu = trim(regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString"));
    out += "cpu: " + (cpu.empty() ? std::string("?") : cpu) + " (" + std::to_string(si.dwNumberOfProcessors) + " logical cores)\n";
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem))
        out += "memory: " + humanSize(mem.ullAvailPhys) + " free of " + humanSize(mem.ullTotalPhys) + "\n";
    const std::wstring drive = toWide(g.cwd).substr(0, 3);
    ULARGE_INTEGER freeBytes, total;
    if (GetDiskFreeSpaceExW(drive.c_str(), &freeBytes, &total, nullptr))
        out += "disk " + toUtf8(drive) + ": " + humanSize(freeBytes.QuadPart) + " free of " + humanSize(total.QuadPart) + "\n";
    const unsigned long long up = GetTickCount64() / 1000;
    out += "uptime: " + std::to_string(up / 86400) + "d " + std::to_string((up % 86400) / 3600) + "h " + std::to_string((up % 3600) / 60) + "m\n";
    out += "local time: " + nowText() + "\nworking folder: " + g.cwd + "\n";
    return ok("System information", out);
}

// Valeur d'une variable d'environnement. Celles qui ressemblent à des secrets ne sont jamais montrées.
ToolResult getEnv(const Args& a) {
    const std::string name = trim(a.str("name"));
    auto secret = [](const std::string& n) {
        const std::string u = lower(n);
        for (const char* w : {"key", "token", "secret", "password", "passwd", "credential", "auth", "cookie", "session"})
            if (u.find(w) != std::string::npos) return true;
        return false;
    };
    if (name.empty()) {  // liste des noms seulement
        std::string out;
        size_t n = 0;
        if (wchar_t* env = GetEnvironmentStringsW()) {
            for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
                const std::string entry = toUtf8(p);
                const size_t eq = entry.find('=');
                if (eq == 0 || eq == std::string::npos) continue;
                out += entry.substr(0, eq) + "\n";
                ++n;
            }
            FreeEnvironmentStringsW(env);
        }
        return ok(plural(n, "variable") + " (names only)", out);
    }
    if (secret(name)) return failTool(name + " looks like a secret: its value is not shown.");
    wchar_t buf[32767];
    const DWORD len = GetEnvironmentVariableW(toWide(name).c_str(), buf, 32767);
    if (!len) return ok(name + " is not set", name + " is not set.\n");
    return ok(name + " is set", name + "=" + toUtf8(std::wstring(buf, len)) + "\n");
}

// ---------------------------------------------------------------------------
// Calcul
// ---------------------------------------------------------------------------
struct Calc {
    const std::string& s;
    size_t i = 0;
    std::string err;
    int depth = 0;

    explicit Calc(const std::string& text) : s(text) {}

    void ws() {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    }
    bool eat(char c) {
        ws();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    double expr() {
        if (++depth > 200) {
            err = "expression too deep";
            return 0;
        }
        double v = term();
        for (;;) {
            if (eat('+')) v += term();
            else if (eat('-')) v -= term();
            else break;
        }
        --depth;
        return v;
    }
    double term() {
        double v = unary();
        for (;;) {
            ws();
            if (i + 1 < s.size() && s[i] == '*' && s[i + 1] == '*') break;  // puissance, gérée plus bas
            if (eat('*')) v *= unary();
            else if (eat('/')) v /= unary();
            else if (eat('%')) v = std::fmod(v, unary());
            else break;
        }
        return v;
    }
    double unary() {
        if (eat('-')) return -unary();
        if (eat('+')) return unary();
        return power();
    }
    double power() {
        const double b = primary();
        ws();
        if (eat('^')) return std::pow(b, unary());
        if (i + 1 < s.size() && s[i] == '*' && s[i + 1] == '*') {
            i += 2;
            return std::pow(b, unary());
        }
        return b;
    }
    double primary() {
        ws();
        if (eat('(')) {
            const double v = expr();
            if (!eat(')')) err = "missing )";
            return v;
        }
        if (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) {
            const size_t start = i;
            while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == '_')) ++i;
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                const size_t save = i++;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                if (i < s.size() && std::isdigit((unsigned char)s[i])) while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
                else i = save;
            }
            std::string num = s.substr(start, i - start);
            num.erase(std::remove(num.begin(), num.end(), '_'), num.end());
            try {
                return std::stod(num);
            } catch (...) {
                err = "bad number " + num;
                return 0;
            }
        }
        if (i < s.size() && (std::isalpha((unsigned char)s[i]) || s[i] == '_')) {
            std::string name;
            while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) name += (char)std::tolower((unsigned char)s[i++]);
            if (name == "pi") return 3.14159265358979323846;
            if (name == "e") return 2.71828182845904523536;
            if (name == "tau") return 6.28318530717958647692;
            if (!eat('(')) {
                err = "unknown name: " + name;
                return 0;
            }
            std::vector<double> a;
            if (!eat(')')) {
                do a.push_back(expr());
                while (eat(','));
                if (!eat(')')) err = "missing ) after " + name;
            }
            auto need = [&](size_t n) {
                if (a.size() != n) err = name + " takes " + std::to_string(n) + " argument" + (n == 1 ? "" : "s");
                return a.size() == n;
            };
            if (name == "sqrt" && need(1)) return std::sqrt(a[0]);
            if (name == "cbrt" && need(1)) return std::cbrt(a[0]);
            if (name == "abs" && need(1)) return std::fabs(a[0]);
            if (name == "sin" && need(1)) return std::sin(a[0]);
            if (name == "cos" && need(1)) return std::cos(a[0]);
            if (name == "tan" && need(1)) return std::tan(a[0]);
            if (name == "asin" && need(1)) return std::asin(a[0]);
            if (name == "acos" && need(1)) return std::acos(a[0]);
            if (name == "atan" && need(1)) return std::atan(a[0]);
            if (name == "atan2" && need(2)) return std::atan2(a[0], a[1]);
            if (name == "sinh" && need(1)) return std::sinh(a[0]);
            if (name == "cosh" && need(1)) return std::cosh(a[0]);
            if (name == "tanh" && need(1)) return std::tanh(a[0]);
            if (name == "exp" && need(1)) return std::exp(a[0]);
            if ((name == "ln" || name == "log") && a.size() == 1) return std::log(a[0]);
            if (name == "log" && need(2)) return std::log(a[0]) / std::log(a[1]);
            if (name == "log10" && need(1)) return std::log10(a[0]);
            if (name == "log2" && need(1)) return std::log2(a[0]);
            if (name == "floor" && need(1)) return std::floor(a[0]);
            if (name == "ceil" && need(1)) return std::ceil(a[0]);
            if (name == "round" && need(1)) return std::round(a[0]);
            if (name == "trunc" && need(1)) return std::trunc(a[0]);
            if (name == "pow" && need(2)) return std::pow(a[0], a[1]);
            if (name == "hypot" && need(2)) return std::hypot(a[0], a[1]);
            if ((name == "fact" || name == "factorial") && need(1)) return std::tgamma(a[0] + 1);
            if (name == "deg" && need(1)) return a[0] * 180 / 3.14159265358979323846;
            if (name == "rad" && need(1)) return a[0] * 3.14159265358979323846 / 180;
            if ((name == "min" || name == "max") && !a.empty()) {
                double v = a[0];
                for (double x : a) v = name == "min" ? std::min(v, x) : std::max(v, x);
                return v;
            }
            if (err.empty()) err = "unknown function: " + name;
            return 0;
        }
        err = i < s.size() ? std::string("unexpected '") + s[i] + "'" : "unexpected end";
        return 0;
    }
};

ToolResult calculate(const Args& a) {
    const std::string expression = a.str("expression");
    if (trim(expression).empty()) return failTool("calculate needs an expression.");
    if (expression.size() > 2000) return failTool("The expression is too long.");
    Calc c(expression);
    const double v = c.expr();
    c.ws();
    if (c.err.empty() && c.i < expression.size()) c.err = std::string("unexpected '") + expression[c.i] + "'";
    if (!c.err.empty()) return failTool("Cannot calculate: " + c.err);
    char buf[64];
    if (std::isfinite(v) && std::fabs(v) < 1e15 && v == std::floor(v)) snprintf(buf, sizeof buf, "%.0f", v);
    else snprintf(buf, sizeof buf, "%.15g", v);
    return ok(trim(expression) + " = " + buf, std::string(buf) + "\n");
}

// ---------------------------------------------------------------------------
// Plan et mémoire
// ---------------------------------------------------------------------------
ToolResult updatePlan(const Args& a) {
    const auto lines = splitLines(a.str("items"));
    std::string out, current;
    size_t total = 0, done = 0;
    for (std::string l : lines) {
        l = trim(l);
        if (l.empty()) continue;
        if (l.rfind("- ", 0) == 0 || l.rfind("* ", 0) == 0) l = trim(l.substr(2));
        char state = ' ';
        if (l.size() >= 3 && l[0] == '[' && l[2] == ']') {
            const char m = (char)std::tolower((unsigned char)l[1]);
            state = m == 'x' ? 'x' : (m == '~' || m == '-' || m == '>') ? '~' : ' ';
            l = trim(l.substr(3));
        }
        if (l.empty()) continue;
        ++total;
        if (state == 'x') ++done;
        if (state == '~' && current.empty()) current = l;
        out += std::string("[") + state + "] " + l + "\n";
    }
    if (!total) return failTool("update_plan needs items, one per line, e.g. \"[x] Read the code\\n[~] Fix the bug\\n[ ] Test\".");
    return ok("Plan: " + std::to_string(done) + "/" + std::to_string(total) + " done" + (current.empty() ? "" : " · now: " + clipLine(current, 60)), out);
}

std::string memoryPath() { return g.configDir + "\\memory.md"; }

// Notes gardées d'une conversation à l'autre (envoyées dans les instructions de l'agent).
std::string memoryText() {
    std::string text;
    if (!readText(fs::path(toWide(memoryPath())), text)) return "";
    return truncate(trim(text), 6000);
}

ToolResult saveMemory(const Args& a) {
    std::string note = a.str("note");
    for (auto& c : note)
        if (c == '\n' || c == '\r') c = ' ';
    note = trim(note);
    if (note.empty()) return failTool("save_memory needs a note.");
    const fs::path p(toWide(memoryPath()));
    std::string text;
    readText(p, text);
    if (text.size() > 24000) return failTool("The memory file is full (" + memoryPath() + "). Ask the user to clean it up.");
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    text += "- " + note + " (" + nowText().substr(0, 10) + ")\n";
    if (!writeText(p, text)) return failTool("Cannot write " + memoryPath());
    return ok("Saved to memory", note + "\n");
}

}  // namespace xt

// ---------------------------------------------------------------------------
// Point d'entrée : utilisé par executeAgentTool (commands.hpp)
// ---------------------------------------------------------------------------
inline bool isAgentTool(const std::string& name) {
    static const std::set<std::string> names = {
        "read_file",      "tree",          "find_files",     "search_files",  "file_info",    "replace_lines",
        "insert_lines",   "append_file",   "copy_path",      "move_path",     "delete_dir",   "undo_last_change",
        "run_powershell", "start_process", "stop_process",   "list_processes", "wait",        "fetch_url",
        "download_file",  "web_search",    "open_path",      "clipboard_read", "clipboard_write", "system_info",
        "get_env",        "calculate",     "update_plan",    "save_memory"};
    return names.count(name) > 0;
}

// Argument principal, pour la ligne « ⏺ Outil(argument) » du terminal.
inline std::string agentToolArg(const JVal& args) {
    if (args.type != JVal::OBJ) return "";
    for (const char* k : {"path", "query", "pattern", "url", "target", "source", "command", "name", "expression", "note", "pid", "seconds"}) {
        const JVal* v = args.find(k);
        if (v && !v->str.empty()) return v->str;
    }
    for (const char* k : {"script", "items", "text"}) {
        const JVal* v = args.find(k);
        if (v && !v->str.empty()) return trim(v->str.substr(0, v->str.find('\n')));
    }
    return "";
}

inline ToolResult runAgentTool(const std::string& name, const JVal& args) {
    using namespace xt;
    const Args a{args};
    if (name == "read_file") return readFile(a);
    if (name == "tree") return tree(a);
    if (name == "find_files") return findFiles(a);
    if (name == "search_files") return searchFiles(a);
    if (name == "file_info") return fileInfo(a);
    if (name == "replace_lines")
        return spliceLines(a.str("path"), a.num("start_line", 0), a.num("end_line", a.num("start_line", 0)), a.str("new_text"), false);
    if (name == "insert_lines") return spliceLines(a.str("path"), a.num("line", 0), 0, a.str("text"), true);
    if (name == "append_file") return appendFile(a);
    if (name == "copy_path") return copyPath(a);
    if (name == "move_path") return movePath(a);
    if (name == "delete_dir") return deleteDir(a);
    if (name == "undo_last_change") return undoChange();
    if (name == "run_powershell") return runPowerShell(a);
    if (name == "start_process") return startProcess(a);
    if (name == "stop_process") return stopProcess(a);
    if (name == "list_processes") return listProcesses(a);
    if (name == "wait") return waitFor(a);
    if (name == "fetch_url") return fetchUrl(a);
    if (name == "download_file") return downloadFile(a);
    if (name == "web_search") return webSearch(a);
    if (name == "open_path") return openPath(a);
    if (name == "clipboard_read") return clipboardRead();
    if (name == "clipboard_write") return clipboardWrite(a);
    if (name == "system_info") return systemInfo();
    if (name == "get_env") return getEnv(a);
    if (name == "calculate") return calculate(a);
    if (name == "update_plan") return updatePlan(a);
    if (name == "save_memory") return saveMemory(a);
    return failTool("Unknown tool: " + name);
}
