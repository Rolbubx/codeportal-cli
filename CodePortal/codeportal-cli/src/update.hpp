// update.hpp : mise à jour depuis les releases GitHub.
//
//   codeportal --update            cherche la dernière version, demande, télécharge, vérifie, remplace
//   codeportal --update --check    dit seulement si une version plus récente existe
//   /update  /update check  /update auto on|off   (dans CodePortal)
//
// Étapes : API GitHub (releases/latest) -> téléchargement de codeportal.exe (et de la page web) ->
// contrôle SHA-256 avec SHA256SUMS.txt publié dans la même release -> remplacement.
// Windows interdit d'écraser un exe en cours d'exécution mais permet de le renommer : l'exe actuel devient
// codeportal.old.exe (supprimé au lancement suivant) et le nouveau prend sa place. En cas d'échec, l'ancien
// est remis en place.
//
// Vérification automatique : au plus une fois par jour, en arrière-plan, sans rien afficher pendant le travail.
// Le résultat est gardé dans %APPDATA%\CodePortal\update.ini ; la bannière l'annonce au lancement suivant.
// CODEPORTAL_UPDATE_URL remplace l'adresse de l'API (tests, fork).
#pragma once

#include "util.hpp"  // windows.h d'abord

#include <bcrypt.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "agenttools.hpp"
#include "json.hpp"
#include "state.hpp"
#include "tools.hpp"
#include "version.hpp"

namespace upd {

namespace fs = std::filesystem;

constexpr const char* kRepo = "Rolbubx/codeportal-cli";
constexpr const char* kExeAsset = "codeportal.exe";
constexpr const char* kPageAsset = "codeportal-chat.html";
constexpr const char* kSumsAsset = "SHA256SUMS.txt";
constexpr long kCheckEvery = 24 * 3600;  // secondes entre deux vérifications automatiques

inline std::string apiBase() {
    const char* env = std::getenv("CODEPORTAL_UPDATE_URL");
    if (env && *env) {
        std::string b = env;
        while (!b.empty() && b.back() == '/') b.pop_back();
        return b;
    }
    return std::string("https://api.github.com/repos/") + kRepo;
}

// ---------------------------------------------------------------------------
// Versions : "v1.2.10" -> {1, 2, 10}. Tout ce qui suit un tiret (1.3.0-beta) est ignoré.
// ---------------------------------------------------------------------------
inline std::vector<int> parseVersion(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    v = v.substr(0, v.find_first_of("-+ "));
    std::vector<int> parts;
    size_t i = 0;
    while (i < v.size()) {
        if (!std::isdigit((unsigned char)v[i])) return parts;
        int n = 0;
        while (i < v.size() && std::isdigit((unsigned char)v[i])) n = n * 10 + (v[i++] - '0');
        parts.push_back(n);
        if (i < v.size() && v[i] == '.') ++i;
    }
    return parts;
}

// < 0 : a plus ancienne que b, 0 : identiques, > 0 : a plus récente.
inline int compareVersions(const std::string& a, const std::string& b) {
    auto x = parseVersion(a), y = parseVersion(b);
    const size_t n = std::max(x.size(), y.size());
    x.resize(n, 0);
    y.resize(n, 0);
    for (size_t i = 0; i < n; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

inline std::string bare(const std::string& tag) {
    return !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
}

// ---------------------------------------------------------------------------
// SHA-256 (BCrypt, inclus dans Windows)
// ---------------------------------------------------------------------------
inline std::string sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[32];
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        if (BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 &&
            BCryptFinishHash(h, digest, sizeof digest, 0) == 0) {
            static const char* d = "0123456789abcdef";
            for (unsigned char c : digest) {
                hex += d[c >> 4];
                hex += d[c & 15];
            }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

// SHA256SUMS.txt (format de sha256sum) : "<hash>  <nom>" ou "<hash> *<nom>", une ligne par fichier.
inline std::map<std::string, std::string> parseSums(const std::string& text) {
    std::map<std::string, std::string> sums;
    for (const auto& raw : splitLines(text)) {
        const std::string line = trim(raw);
        const size_t sp = line.find_first_of(" \t");
        if (sp != 64) continue;
        std::string name = trim(line.substr(sp));
        if (!name.empty() && name[0] == '*') name.erase(0, 1);
        std::string hash = line.substr(0, 64);
        for (auto& c : hash) c = (char)std::tolower((unsigned char)c);
        sums[fs::path(name).filename().string()] = hash;
    }
    return sums;
}

// ---------------------------------------------------------------------------
// Release GitHub
// ---------------------------------------------------------------------------
struct Asset {
    std::string name, url;
};

struct Release {
    bool ok = false;
    std::string error, tag, notes, page;
    std::vector<Asset> assets;

    const Asset* asset(const std::string& name) const {
        for (const auto& a : assets)
            if (a.name == name) return &a;
        return nullptr;
    }
};

inline Release fetchLatest() {
    Release rel;
    const xt::HttpGet res = xt::httpGet(apiBase() + "/releases/latest", 2u * 1024 * 1024);
    if (!res.error.empty()) {
        rel.error = res.error;
        return rel;
    }
    if (res.status == 404) {
        rel.error = "No release found on GitHub (" + std::string(kRepo) + ").";
        return rel;
    }
    if (res.status == 403 || res.status == 429) {
        rel.error = "GitHub refused the request (rate limit). Try again in a few minutes.";
        return rel;
    }
    if (res.status != 200) {
        rel.error = "GitHub answered HTTP " + std::to_string(res.status) + ".";
        return rel;
    }
    JVal v;
    if (!JsonParser(res.body).parse(v) || v.type != JVal::OBJ) {
        rel.error = "Unexpected answer from GitHub.";
        return rel;
    }
    auto str = [&](const JVal& o, const char* k) {
        const JVal* f = o.find(k);
        return f && f->type == JVal::STR ? f->str : std::string();
    };
    rel.tag = str(v, "tag_name");
    rel.notes = str(v, "body");
    rel.page = str(v, "html_url");
    if (const JVal* a = v.find("assets"); a && a->type == JVal::ARR)
        for (const auto& x : a->arr) rel.assets.push_back({str(x, "name"), str(x, "browser_download_url")});
    if (parseVersion(rel.tag).empty()) {
        rel.error = "The latest release has no version tag.";
        return rel;
    }
    rel.ok = true;
    return rel;
}

// ---------------------------------------------------------------------------
// Fichiers locaux
// ---------------------------------------------------------------------------
inline fs::path exePath() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    return fs::path(std::wstring(buf, n));
}

inline fs::path oldExePath() { return exePath().parent_path() / L"codeportal.old.exe"; }

// Au lancement : l'exe remplacé lors de la dernière mise à jour n'est plus utilisé, on le supprime.
inline void cleanupOldExe() {
    std::error_code ec;
    fs::remove(oldExePath(), ec);
    fs::remove(exePath().parent_path() / L"codeportal.new.exe", ec);
}

// Page web à côté de l'exe : installation (web\ à côté) ou dossier du projet (build\..\web).
inline fs::path pageTarget() {
    const fs::path dir = exePath().parent_path();
    std::error_code ec;
    if (fs::is_directory(dir / ".." / "web", ec) && !fs::is_directory(dir / "web", ec))
        return dir / ".." / "web" / kPageAsset;
    return dir / "web" / kPageAsset;
}

inline bool writeFileBin(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), (std::streamsize)data.size());
    return (bool)out;
}

// Version affichée dans « Applications installées », si CodePortal a été installé avec l'installeur.
inline void updateInstalledVersion(const std::string& version) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CodePortal", 0,
                      KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    const std::wstring w = toWide(version);
    RegSetValueExW(key, L"DisplayVersion", 0, REG_SZ, (const BYTE*)w.c_str(), (DWORD)((w.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// PATH de l'utilisateur (HKCU\Environment) : l'installeur appelle --add-to-path et --remove-from-path.
// Fait ici plutôt que dans l'installeur : NSIS coupe les chaînes à 1024 caractères, ce qui abîmerait un long PATH.
// ---------------------------------------------------------------------------
inline std::wstring normDir(std::wstring d) {
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    for (auto& c : d) c = (wchar_t)towlower(c);
    return d;
}

// Ajoute (add) ou retire le dossier de l'exe du PATH de l'utilisateur. Retourne false en cas d'erreur.
inline bool editUserPath(bool add) {
    const std::wstring dir = exePath().parent_path().wstring();
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return false;
    DWORD type = REG_EXPAND_SZ, size = 0;
    std::wstring path;
    if (RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size > 0) {
        path.resize(size / sizeof(wchar_t) + 1);
        if (RegQueryValueExW(key, L"Path", nullptr, &type, (BYTE*)&path[0], &size) != ERROR_SUCCESS) {
            RegCloseKey(key);
            return false;
        }
        path.resize(wcsnlen(path.c_str(), path.size()));
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) type = REG_EXPAND_SZ;

    std::vector<std::wstring> parts;
    bool present = false;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = std::min(path.find(L';', start), path.size());
        const std::wstring part = path.substr(start, end - start);
        if (!part.empty()) {
            if (normDir(part) == normDir(dir)) present = true;
            else parts.push_back(part);
        }
        start = end + 1;
    }
    if (add) parts.push_back(dir);
    if (add == present) {  // déjà dans l'état voulu
        RegCloseKey(key);
        return true;
    }
    std::wstring joined;
    for (const auto& p : parts) joined += (joined.empty() ? L"" : L";") + p;
    const bool ok = RegSetValueExW(key, L"Path", 0, type, (const BYTE*)joined.c_str(),
                                   (DWORD)((joined.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    // Les terminaux ouverts ensuite voient le nouveau PATH.
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 3000, nullptr);
    return ok;
}

// ---------------------------------------------------------------------------
// update.ini : dernière vérification, dernière version vue, vérification automatique on/off
// ---------------------------------------------------------------------------
struct Settings {
    long long lastCheck = 0;
    std::string latest;
    bool autoCheck = true;
};

inline std::string settingsPath() { return g.configDir + "\\update.ini"; }

inline Settings loadSettings() {
    Settings s;
    std::ifstream in(fs::u8path(settingsPath()));
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "last_check") s.lastCheck = std::atoll(v.c_str());
        else if (k == "latest") s.latest = v;
        else if (k == "auto_check") s.autoCheck = v != "off";
    }
    return s;
}

inline void saveSettings(const Settings& s) {
    std::error_code ec;
    fs::create_directories(fs::u8path(g.configDir), ec);
    std::ofstream out(fs::u8path(settingsPath()), std::ios::trunc);
    out << "# CodePortal update checks\n"
        << "last_check=" << s.lastCheck << "\n"
        << "latest=" << s.latest << "\n"
        << "auto_check=" << (s.autoCheck ? "on" : "off") << "\n";
}

inline void rememberLatest(const std::string& tag) {
    Settings s = loadSettings();
    s.lastCheck = (long long)std::time(nullptr);
    s.latest = tag;
    saveSettings(s);
}

// Version plus récente vue lors d'une vérification précédente ("" si aucune).
inline std::string availableUpdate() {
    const Settings s = loadSettings();
    if (!s.autoCheck || s.latest.empty()) return "";
    return compareVersions(s.latest, kVersion) > 0 ? s.latest : "";
}

// Au lancement : si la dernière vérification date de plus d'un jour, on revérifie en arrière-plan.
// Rien n'est affiché : le résultat est enregistré pour le lancement suivant (et pour /update).
inline void startBackgroundCheck() {
    const Settings s = loadSettings();
    const long long now = (long long)std::time(nullptr);
    if (!s.autoCheck || (now - s.lastCheck >= 0 && now - s.lastCheck < kCheckEvery)) return;
    std::thread([] {
        const Release rel = fetchLatest();
        if (rel.ok) rememberLatest(rel.tag);
    }).detach();
}

// ---------------------------------------------------------------------------
// Mise à jour
// ---------------------------------------------------------------------------
inline void say(const std::string& s) { std::cout << s << std::flush; }
inline void ok(const std::string& s) { say(std::string(P.ok) + "✓ " + ansi::reset + s + "\n"); }
inline void fail(const std::string& s) { say(std::string(P.err) + "✗ " + s + ansi::reset + "\n"); }
inline void info(const std::string& s) { say(std::string(P.muted) + "  " + s + ansi::reset + "\n"); }

// Télécharge un fichier de la release et vérifie son SHA-256. Vide en cas d'erreur (message dans err).
inline std::string downloadVerified(const Asset& a, const std::map<std::string, std::string>& sums, std::string& err) {
    const auto it = sums.find(a.name);
    if (it == sums.end()) {
        err = a.name + " is not listed in " + kSumsAsset + ".";
        return "";
    }
    const xt::HttpGet res = xt::httpGet(a.url, 200u * 1024 * 1024);
    if (!res.error.empty()) err = "Download of " + a.name + " failed: " + res.error;
    else if (res.status != 200) err = "Download of " + a.name + " failed: HTTP " + std::to_string(res.status);
    else if (res.truncated) err = a.name + " is too large.";
    else if (sha256Hex(res.body) != it->second)
        err = a.name + " does not match its SHA-256 checksum: the download is corrupted or was tampered with.";
    if (!err.empty()) return "";
    return res.body;
}

// Remplace l'exe en cours d'exécution. Retourne "" si tout va bien, sinon le message d'erreur.
inline std::string replaceExe(const std::string& data) {
    const fs::path exe = exePath(), old = oldExePath(), fresh = exe.parent_path() / L"codeportal.new.exe";
    if (!writeFileBin(fresh, data))
        return "Cannot write to " + exe.parent_path().u8string() +
               ". Run CodePortal as administrator, or reinstall it in a folder you own.";
    std::error_code ec;
    fs::remove(old, ec);
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fs::remove(fresh, ec);
        return "Cannot rename the current codeportal.exe (error " + std::to_string(GetLastError()) + ").";
    }
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD e = GetLastError();
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);  // on remet l'ancien
        fs::remove(fresh, ec);
        return "Cannot put the new codeportal.exe in place (error " + std::to_string(e) + "). Nothing was changed.";
    }
    return "";
}

// Notes de version : les premières lignes, sans le Markdown le plus visible.
inline void printNotes(const std::string& notes) {
    size_t shown = 0;
    for (const auto& raw : splitLines(notes)) {
        std::string l = raw;
        while (!l.empty() && l.back() == '\r') l.pop_back();
        if (trim(l).empty()) continue;
        if (shown == 12) {
            info("…");
            break;
        }
        if (startsWith(l, "#")) l = trim(l.substr(l.find_first_not_of('#')));
        info(l);
        ++shown;
    }
}

// Cherche et installe la dernière version. checkOnly : seulement dire si elle existe. assumeYes : sans question.
// Retourne 0 si tout va bien (à jour, ou mis à jour), 1 en cas d'erreur.
inline int runUpdate(bool checkOnly, bool assumeYes) {
    say(std::string(P.muted) + "Checking for updates…" + ansi::reset + "\n");
    const Release rel = fetchLatest();
    if (!rel.ok) {
        fail("Update check failed: " + rel.error);
        return 1;
    }
    rememberLatest(rel.tag);
    const std::string current = kVersion, latest = bare(rel.tag);
    if (compareVersions(latest, current) <= 0) {
        ok("CodePortal " + current + " is up to date.");
        return 0;
    }
    say(std::string("\n") + ansi::bold + "CodePortal " + latest + ansi::reset + " is available " + P.muted + "(you have " +
        current + ")" + ansi::reset + "\n");
    printNotes(rel.notes);
    if (!rel.page.empty()) info(rel.page);
    if (checkOnly) {
        say(std::string("\n") + "Run " + ansi::bold + "codeportal --update" + ansi::reset + " (or /update) to install it.\n");
        return 0;
    }

    const Asset* exe = rel.asset(kExeAsset);
    const Asset* sumsFile = rel.asset(kSumsAsset);
    if (!exe || !sumsFile) {
        fail("This release has no " + std::string(exe ? kSumsAsset : kExeAsset) + ", so it cannot be installed automatically. " +
             "Download it from " + (rel.page.empty() ? "GitHub" : rel.page) + ".");
        return 1;
    }
    say("\n");
    if (!assumeYes && !askYesNo("Install CodePortal " + latest + "?")) {
        info("Update cancelled.");
        return 0;
    }

    const xt::HttpGet sumsRes = xt::httpGet(sumsFile->url, 1024 * 1024);
    if (!sumsRes.error.empty() || sumsRes.status != 200) {
        fail("Cannot download " + std::string(kSumsAsset) + ": " +
             (sumsRes.error.empty() ? "HTTP " + std::to_string(sumsRes.status) : sumsRes.error));
        return 1;
    }
    const auto sums = parseSums(sumsRes.body);

    say(std::string(P.muted) + "Downloading " + kExeAsset + "…" + ansi::reset + "\n");
    std::string err;
    const std::string exeData = downloadVerified(*exe, sums, err);
    if (exeData.empty()) {
        fail(err);
        return 1;
    }
    std::string pageData;
    if (const Asset* page = rel.asset(kPageAsset)) {
        pageData = downloadVerified(*page, sums, err);
        if (pageData.empty()) {
            fail(err);
            return 1;
        }
    }
    ok("Downloaded and verified (SHA-256)");

    err = replaceExe(exeData);
    if (!err.empty()) {
        fail(err);
        return 1;
    }
    if (!pageData.empty()) {
        const fs::path page = pageTarget();
        std::error_code ec;
        fs::create_directories(page.parent_path(), ec);
        if (!writeFileBin(page, pageData)) info("Could not update " + page.u8string() + " (the program itself is updated).");
    }
    updateInstalledVersion(latest);
    ok("Updated to CodePortal " + latest + ". Restart CodePortal to use it.");
    return 0;
}

}  // namespace upd
