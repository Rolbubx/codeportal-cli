// safety.hpp : garde-fous des outils de l'agent.
//
// L'agent travaille librement dans le dossier de travail. Ailleurs :
//   - Interdit (Deny) : modifier Windows et les dossiers système (C:\Windows, Program Files, ProgramData,
//     racine du disque système, Recovery, Boot...), et lire ou modifier les secrets (config.ini de
//     CodePortal qui contient les clés d'API, .ssh, identifiants cloud...).
//   - Sur confirmation (Ask) : écrire ou supprimer en dehors du dossier de travail, commandes risquées
//     (suppressions récursives, git push --force, git reset --hard, téléchargement puis exécution...).
//   - Commandes destructrices pour le système (format, diskpart, bcdedit, vssadmin delete, reg HKLM,
//     shutdown...) : interdites.
//
// L'analyse des commandes cmd.exe / PowerShell reste une heuristique : elle arrête les erreurs et les
// dérives courantes d'un modèle, pas un attaquant déterminé. Les clés d'API ne sont jamais transmises
// aux commandes de l'agent (voir environmentWithoutSecrets dans tools.hpp).
#pragma once

#include <shlobj.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "state.hpp"
#include "util.hpp"

namespace safety {

enum class Verdict { Allow, Ask, Deny };

struct Decision {
    Verdict verdict = Verdict::Allow;
    std::string reason;  // affiché à l'utilisateur et renvoyé au modèle
};

enum class Access { Read, Write };

// Dossiers que l'utilisateur a autorisés pour la session (« Allow for this session »).
inline std::vector<std::string>& sessionAllowed() {
    static std::vector<std::string> dirs;
    return dirs;
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Chemin absolu, normalisé (liens et jonctions résolus pour la partie existante), en minuscules,
// séparateurs « \ », sans préfixe \\?\ ni « \ » final.
inline std::string normalize(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(toWide(path));
    if (p.is_relative()) p = fs::path(toWide(g.cwd)) / p;
    fs::path c = fs::weakly_canonical(p, ec);
    if (ec) c = p.lexically_normal();
    std::string s = lower(c.u8string());
    std::replace(s.begin(), s.end(), '/', '\\');
    if (startsWith(s, "\\\\?\\unc\\")) s = "\\\\" + s.substr(8);
    else if (startsWith(s, "\\\\?\\")) s = s.substr(4);
    while (s.size() > 3 && s.back() == '\\') s.pop_back();
    return s;
}

// `path` est `root` ou se trouve dedans.
inline bool within(const std::string& path, const std::string& root) {
    if (root.empty()) return false;
    if (path == root) return true;
    const std::string r = root.back() == '\\' ? root : root + "\\";
    return startsWith(path, r);
}

inline std::string envDir(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? normalize(v) : "";
}

inline std::string knownDir(int csidl) {
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf))) return normalize(toUtf8(buf));
    return "";
}

inline std::string systemDrive() {
    const char* d = std::getenv("SystemDrive");
    return lower(d && *d ? d : "C:") + "\\";
}

// Dossiers du système : jamais modifiés par l'agent.
inline const std::vector<std::string>& systemRoots() {
    static const std::vector<std::string> roots = [] {
        std::vector<std::string> r = {envDir("SystemRoot"),        envDir("windir"),
                                      envDir("ProgramFiles"),      envDir("ProgramFiles(x86)"),
                                      envDir("ProgramW6432"),      envDir("ProgramData"),
                                      envDir("CommonProgramFiles"), envDir("CommonProgramFiles(x86)")};
        const std::string sd = systemDrive();
        for (const char* d : {"windows", "program files", "program files (x86)", "programdata", "recovery", "boot",
                              "$recycle.bin", "system volume information", "$windows.~bt", "$windows.~ws",
                              "$winreagent", "efi"})
            r.push_back(sd + d);
        std::vector<std::string> unique;  // sans doublons (C:\Windows = %SystemRoot% = %windir%...)
        for (const auto& d : r)
            if (!d.empty() && std::find(unique.begin(), unique.end(), d) == unique.end()) unique.push_back(d);
        return unique;
    }();
    return roots;
}

// Secrets : ni lus ni modifiés par l'agent.
inline const std::vector<std::string>& secretPaths() {
    static const std::vector<std::string> paths = [] {
        std::vector<std::string> r = {normalize(g.configDir)};  // config.ini (clés d'API), sessions, historique
        const std::string home = envDir("USERPROFILE");
        if (!home.empty())
            for (const char* d : {".ssh", ".aws", ".azure", ".gnupg", ".kube", ".docker\\config.json",
                                  ".git-credentials", ".netrc", "_netrc", ".npmrc", ".pypirc",
                                  "appdata\\roaming\\microsoft\\credentials", "appdata\\local\\microsoft\\credentials",
                                  "appdata\\roaming\\microsoft\\protect", "appdata\\roaming\\gnupg"})
                r.push_back(home + "\\" + d);
        return r;
    }();
    return paths;
}

// Noms qui désignent des secrets quel que soit l'utilisateur ou le dossier (chemins en minuscules, « \ »).
inline bool looksSecret(const std::string& lowerPath) {
    static const char* parts[] = {"\\.ssh\\",        "\\.aws\\",         "\\.azure\\", "\\.gnupg\\",
                                  "\\.git-credentials", "\\id_rsa",          "\\id_ed25519", "\\id_ecdsa",
                                  "\\.netrc",           "\\codeportal\\config.ini", "\\microsoft\\credentials\\",
                                  "\\microsoft\\protect\\"};
    const std::string p = lowerPath + "\\";
    for (const char* part : parts)
        if (p.find(part) != std::string::npos) return true;
    return false;
}

inline Decision allow() { return {}; }
inline Decision deny(const std::string& why) { return {Verdict::Deny, why}; }
inline Decision ask(const std::string& why) { return {Verdict::Ask, why}; }

// Accès d'un outil de fichiers de l'agent à `path`.
inline Decision checkPath(const std::string& rawPath, Access access) {
    const std::string raw = trim(rawPath);
    if (raw.empty()) return allow();
    const std::string lr = lower(raw);
    if (startsWith(lr, "\\\\.\\") || startsWith(lr, "\\\\?\\globalroot") || lr.find("physicaldrive") != std::string::npos)
        return deny("device paths are not allowed");
    // Flux de données alternatifs (fichier.txt:flux) : seul le « : » du lecteur est permis.
    if (raw.find(':', raw.size() > 2 && raw[1] == ':' ? 2 : 0) != std::string::npos)
        return deny("alternate data streams (\"file:stream\") are not allowed");

    const std::string p = normalize(raw);
    for (const auto& s : secretPaths())
        if (within(p, s) || looksSecret(p)) return deny(raw + " holds credentials or API keys; the agent may not " +
                                      (access == Access::Read ? "read" : "change") + " it");
    if (access == Access::Read) return allow();

    for (const auto& root : systemRoots())
        if (within(p, root)) return deny(raw + " is a Windows system location; the agent may not change it");
    const std::string sd = systemDrive();
    if (p.size() > sd.size() && startsWith(p, sd) && p.find('\\', sd.size()) == std::string::npos &&
        !within(p, normalize(g.cwd)))
        return deny("files at the root of the system drive (" + sd + ") are off limits");
    if (p == lower(sd) || p + "\\" == lower(sd)) return deny("the system drive itself is off limits");

    const std::string cwd = normalize(g.cwd);
    if (within(p, cwd)) return allow();
    for (const auto& d : sessionAllowed())
        if (within(p, d)) return allow();
    return ask(raw + " is outside the working directory");
}

// ---------------------------------------------------------------------------
// Commandes (run_command)
// ---------------------------------------------------------------------------
inline bool hasWord(const std::string& s, const std::string& w) {
    for (size_t pos = s.find(w); pos != std::string::npos; pos = s.find(w, pos + 1)) {
        const bool okL = pos == 0 || !(std::isalnum((unsigned char)s[pos - 1]) || s[pos - 1] == '_' || s[pos - 1] == '-');
        const size_t e = pos + w.size();
        const bool okR = e >= s.size() || !(std::isalnum((unsigned char)s[e]) || s[e] == '_');
        if (okL && okR) return true;
    }
    return false;
}

// Remplace %VAR%, %VAR:~a,b% (sous-chaîne) et $env:VAR par leur valeur, pour repérer les chemins protégés.
inline std::string expandVars(const std::string& cmd) {
    std::string out;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (cmd[i] == '%') {
            const size_t j = cmd.find('%', i + 1);
            if (j != std::string::npos && j > i + 1) {
                std::string inner = cmd.substr(i + 1, j - i - 1);  // VAR, ou VAR:~off,len
                std::string name = inner;
                long off = -1, len = -1;
                const size_t sub = inner.find(":~");
                if (sub != std::string::npos) {
                    name = inner.substr(0, sub);
                    const std::string rest = inner.substr(sub + 2);
                    const size_t comma = rest.find(',');
                    try {
                        off = std::stol(rest.substr(0, comma));
                        if (comma != std::string::npos) len = std::stol(rest.substr(comma + 1));
                    } catch (...) {
                        off = -1;
                    }
                }
                const char* v = std::getenv(name.c_str());
                if (v) {
                    std::string val = v;
                    if (off >= 0) {  // %VAR:~off,len%
                        if (off > (long)val.size()) off = (long)val.size();
                        val = len >= 0 ? val.substr(off, len) : val.substr(off);
                    }
                    out += val;
                    i = j;
                    continue;
                }
            }
        }
        if (lower(cmd.substr(i, 5)) == "$env:") {
            size_t j = i + 5;
            while (j < cmd.size() && (std::isalnum((unsigned char)cmd[j]) || cmd[j] == '_' || cmd[j] == '(' || cmd[j] == ')')) ++j;
            const char* v = std::getenv(cmd.substr(i + 5, j - i - 5).c_str());
            if (v) {
                out += v;
                i = j - 1;
                continue;
            }
        }
        out += cmd[i];
    }
    return out;
}

// Ramène une commande à une forme comparable, pour empêcher les contournements courants :
//   - ^ (échappement cmd.exe) : C:\Win^dows -> C:\Windows
//   - concaténation de chaînes : "C:\Win"dows, 'C:\Win'+'dows' -> C:\Windows
//   - noms courts 8.3 : C:\PROGRA~1 -> C:\Program Files
//   - %VAR%, %VAR:~a,b%, $env:VAR développés
inline std::string canonicalize(const std::string& cmd) {
    std::string s;
    for (size_t i = 0; i < cmd.size(); ++i) {  // ^ : on garde le caractère suivant, on jette l'accent
        if (cmd[i] == '^') {
            if (i + 1 < cmd.size()) s += cmd[++i];
            continue;
        }
        s += cmd[i];
    }
    replaceAll(s, "'+'", "");  // 'C:\\Win'+'dows' -> C:\\Windows
    replaceAll(s, "\"+\"", "");
    replaceAll(s, "' + '", "");
    replaceAll(s, "\" + \"", "");
    std::string noq;  // guillemets et accents graves retirés : "C:\\Win"dows -> C:\\Windows
    for (char ch : s)
        if (ch != '"' && ch != '\'' && ch != '`') noq += ch;
    s = lower(expandVars(noq));
    // Noms courts 8.3 des dossiers protégés.
    replaceAll(s, "progra~1", "program files");
    replaceAll(s, "progra~2", "program files (x86)");
    replaceAll(s, "progra~3", "program files");
    replaceAll(s, "windo~1", "windows");
    return s;
}

// Jetons qui ressemblent à des chemins, normalisés (les « .. » et chemins relatifs sont résolus),
// pour que les vérifications de dossiers protégés ne soient pas trompées par la forme du chemin.
inline std::vector<std::string> pathTokens(const std::string& canonSlash) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        std::string t = cur;
        cur.clear();
        while (!t.empty() && (t[0] == '>' || t[0] == '<' || t[0] == '@' || t[0] == '&')) t.erase(0, 1);
        if (t.size() < 2) return;
        if (t.find('\\') != std::string::npos || t.find("..") != std::string::npos ||
            (t.size() >= 2 && t[1] == ':'))
            out.push_back(normalize(t));
    };
    for (char ch : canonSlash) {
        if (std::isspace((unsigned char)ch) || ch == '|' || ch == ';' || ch == ',' || ch == '(' || ch == ')' || ch == '=')
            flush();
        else if (ch == '&')
            cur += ' ', flush();
        else
            cur += ch;
    }
    flush();
    // Cible d'un changement de dossier (cd, pushd, Set-Location...) : résolue comme un chemin.
    static const std::vector<std::string> cdVerbs = {"cd ", "chdir ", "pushd ", "set-location ", "sl "};
    for (const auto& v : cdVerbs) {
        size_t pos = 0;
        while ((pos = canonSlash.find(v, pos)) != std::string::npos) {
            pos += v.size();
            while (pos < canonSlash.size() && canonSlash[pos] == ' ') ++pos;
            size_t e = pos;
            while (e < canonSlash.size() && canonSlash[e] != ' ' && canonSlash[e] != '&' && canonSlash[e] != '|') ++e;
            if (e > pos) out.push_back(normalize(canonSlash.substr(pos, e - pos)));
        }
    }
    return out;
}

inline Decision checkCommand(const std::string& command) {
    const std::string c = canonicalize(command);  // déjà en minuscules
    std::string slash = c;
    std::replace(slash.begin(), slash.end(), '/', '\\');
    const std::vector<std::string> toks = pathTokens(slash);

    // 1. Toujours interdit : peut détruire le système, les données ou les sauvegardes.
    struct Rule {
        std::vector<std::string> all;  // tous ces mots doivent apparaître
        const char* why;
    };
    auto matches = [&](const Rule& r) {
        for (const auto& w : r.all) {
            const bool literal = w.find_first_of("\\|.:") != std::string::npos || w == "--";
            if (literal ? slash.find(w) == std::string::npos : !hasWord(c, w)) return false;
        }
        return true;
    };
    // format X: (et pas git log --format, Format-Table, dotnet format...)
    for (size_t pos = c.find("format"); pos != std::string::npos; pos = c.find("format", pos + 1)) {
        if (pos > 0 && (std::isalnum((unsigned char)c[pos - 1]) || c[pos - 1] == '-')) continue;
        size_t k = pos + 6;
        while (k < c.size() && c[k] == ' ') ++k;
        if (k > pos + 6 && k + 1 < c.size() && std::isalpha((unsigned char)c[k]) && c[k + 1] == ':')
            return deny("formatting a drive is not allowed");
    }
    static const std::vector<Rule> blocked = {
        {{"diskpart"}, "partitioning disks"},
        {{"bcdedit"}, "changing the boot configuration"},
        {{"bootrec"}, "changing the boot configuration"},
        {{"bcdboot"}, "changing the boot configuration"},
        {{"vssadmin", "delete"}, "deleting shadow copies"},
        {{"vssadmin", "resize"}, "deleting shadow copies"},
        {{"wbadmin", "delete"}, "deleting backups"},
        {{"shadowcopy", "delete"}, "deleting shadow copies"},
        {{"cipher", "\\w"}, "wiping free disk space"},
        {{"sdelete"}, "securely erasing data"},
        {{"takeown"}, "taking ownership of files"},
        {{"shutdown"}, "shutting down or restarting the computer"},
        {{"restart-computer"}, "restarting the computer"},
        {{"stop-computer"}, "shutting down the computer"},
        {{"net", "user"}, "managing user accounts"},
        {{"net", "localgroup"}, "managing user groups"},
        {{"set-executionpolicy"}, "changing the PowerShell execution policy"},
        {{"set-mppreference"}, "changing Windows Defender settings"},
        {{"add-mppreference"}, "changing Windows Defender settings"},
        {{"netsh", "firewall"}, "changing the firewall"},
        {{"netsh", "advfirewall"}, "changing the firewall"},
        {{"clear-disk"}, "erasing a disk"},
        {{"initialize-disk"}, "initializing a disk"},
        {{"remove-partition"}, "removing a partition"},
        {{"format-volume"}, "formatting a volume"},
        {{"wevtutil", "cl"}, "clearing event logs"},
        {{"clear-eventlog"}, "clearing event logs"},
    };
    for (const auto& r : blocked)
        if (matches(r)) return deny(std::string(r.why) + " is not allowed");
    // Registre de la machine (HKLM) : lecture permise, modification interdite.
    if (hasWord(c, "hklm") || c.find("hkey_local_machine") != std::string::npos || c.find("hklm:") != std::string::npos) {
        for (const char* v : {"add", "delete", "copy", "restore", "load", "unload", "import", "set-itemproperty",
                              "new-itemproperty", "remove-itemproperty", "new-item", "remove-item", "set-item"})
            if (hasWord(c, v)) return deny("changing machine-wide registry settings (HKLM) is not allowed");
    }

    // 2. Secrets : jamais lus par une commande (config.ini de CodePortal, .ssh...).
    auto anyTokenWithin = [&](const std::string& root) {
        for (const auto& t : toks)
            if (within(t, root)) return true;
        return false;
    };
    for (const auto& s : secretPaths()) {
        std::string tail = s;
        const std::string home = envDir("USERPROFILE");
        if (!home.empty() && startsWith(tail, home + "\\")) tail = tail.substr(home.size() + 1);
        if (slash.find(s) != std::string::npos || anyTokenWithin(s) ||
            (tail != s && tail.size() > 4 && slash.find(tail) != std::string::npos))
            return deny("the command touches " + s + ", which holds credentials or API keys");
    }
    if (looksSecret(slash)) return deny("the command touches files that hold credentials or API keys");
    for (const auto& t : toks)
        if (looksSecret(t)) return deny("the command touches files that hold credentials or API keys");

    // Recherche récursive dans AppData / le profil : peut faire remonter config.ini, les clés, les jetons.
    // On teste `c` (slashes d'origine) car /s et /r deviennent \s, \r après normalisation.
    const bool recursive = c.find(" /s") != std::string::npos || c.find("-recurse") != std::string::npos ||
                           c.find(" /r ") != std::string::npos || c.find("-r ") != std::string::npos ||
                           c.find("-recurse") != std::string::npos;
    if (recursive) {
        const std::string appdata = envDir("APPDATA"), local = envDir("LOCALAPPDATA"), home = envDir("USERPROFILE");
        for (const auto& t : toks)
            if ((!appdata.empty() && within(t, appdata)) || (!local.empty() && within(t, local)) ||
                (!home.empty() && t == home))
                return ask("this command searches recursively through AppData, which holds credentials and API keys");
    }

    // 3. Écriture dans un dossier système : interdit. Lire (dir, type...) reste permis.
    static const std::vector<std::string> writeVerbs = {
        "del",         "erase",       "rd",          "rmdir",        "move",        "ren",          "rename",
        "copy",        "xcopy",       "robocopy",    "mklink",       "attrib",      "icacls",       "cacls",
        "remove-item", "ri",          "rm",          "move-item",    "mv",          "mi",           "copy-item",
        "cp",          "cpi",         "rename-item", "rni",          "set-content", "sc",           "add-content",
        "ac",          "out-file",    "new-item",    "ni",           "clear-content", "set-acl",    "set-itemproperty",
        "compact",     "expand",      "tar",         "replace",      "setx",        "wsl"};
    bool writes = c.find('>') != std::string::npos;
    for (const auto& v : writeVerbs)
        if (hasWord(c, v)) writes = true;
    const std::string sd = systemDrive();
    if (writes) {
        for (const auto& root : systemRoots())
            if (slash.find(root) != std::string::npos || anyTokenWithin(root))
                return deny("the command would change " + root + ", a Windows system location");
        // Cible = un disque entier ou tout le dossier de l'utilisateur (del C:\*, rd /s C:\, rd /s %USERPROFILE%).
        const std::string padded = " " + slash + " ";
        std::vector<std::string> whole = {sd + "*", sd + " ", sd + "\"", sd + "'"};
        const std::string home = envDir("USERPROFILE");
        if (!home.empty())
            for (const char* end : {" ", "\"", "'", "\\*", "\\ "}) whole.push_back(home + end);
        for (const auto& t : whole)
            if (padded.find(t) != std::string::npos) return deny("the command targets a whole drive or the whole user folder");
        // Après normalisation : la cible est la racine d'un disque ou tout le dossier utilisateur.
        const std::string sdRoot = sd.substr(0, 2);  // « c: »
        for (const auto& t : toks)
            if (t == sdRoot || (!home.empty() && t == home))
                return deny("the command targets a whole drive or the whole user folder");
    }

    // 4. Sur confirmation : risqué mais parfois légitime.
    static const std::vector<Rule> risky = {
        {{"rd", "\\s"}, "deletes a folder tree"},
        {{"rmdir", "\\s"}, "deletes a folder tree"},
        {{"del", "\\s"}, "deletes files in all subfolders"},
        {{"del", "\\q"}, "deletes files without asking"},
        {{"erase", "\\s"}, "deletes files in all subfolders"},
        {{"remove-item", "-recurse"}, "deletes a folder tree"},
        {{"rm", "-rf"}, "deletes a folder tree"},
        {{"rm", "-r"}, "deletes a folder tree"},
        {{"git", "push"}, "publishes commits to a remote repository"},
        {{"git", "reset", "--hard"}, "discards uncommitted work"},
        {{"git", "clean"}, "deletes untracked files"},
        {{"git", "checkout", "--", "."}, "discards uncommitted changes"},
        {{"git", "restore"}, "discards uncommitted changes"},
        {{"git", "stash", "drop"}, "deletes stashed work"},
        {{"git", "branch", "-d"}, "deletes a branch"},
        {{"reg", "add"}, "changes the registry"},
        {{"reg", "delete"}, "changes the registry"},
        {{"reg", "import"}, "changes the registry"},
        {{"regedit"}, "imports or changes the registry"},
        {{"regini"}, "changes the registry"},
        {{"set-itemproperty"}, "may change the registry"},
        {{"schtasks", "\\create"}, "creates a scheduled task"},
        {{"sc", "create"}, "creates a Windows service"},
        {{"sc", "delete"}, "deletes a Windows service"},
        {{"sc", "config"}, "changes a Windows service"},
        {{"setx"}, "changes environment variables permanently"},
        {{"taskkill"}, "kills processes"},
        {{"stop-process"}, "kills processes"},
        {{"invoke-expression"}, "runs dynamically built code"},
        {{"iex"}, "runs dynamically built code"},
        {{"-encodedcommand"}, "runs an encoded (hidden) command"},
        {{"-enc"}, "runs an encoded (hidden) command"},
        {{"start-process", "runas"}, "asks for administrator rights"},
        {{"runas"}, "runs as another user"},
        {{"winget", "install"}, "installs software"},
        {{"winget", "uninstall"}, "uninstalls software"},
        {{"choco", "install"}, "installs software"},
        {{"msiexec"}, "installs or removes software"},
        {{"npm", "-g"}, "installs packages globally"},
        {{"npm", "publish"}, "publishes a package"},
        {{"pip", "install"}, "installs Python packages"},
        {{"curl", "|"}, "downloads and pipes content to a program"},
        {{"invoke-webrequest"}, "downloads from the internet"},
        {{"iwr"}, "downloads from the internet"},
        {{"bitsadmin"}, "downloads from the internet"},
        {{"certutil", "-urlcache"}, "downloads from the internet"},
        {{"sfc"}, "checks or repairs Windows system files"},
        {{"dism"}, "services the Windows image"},
    };
    for (const auto& r : risky)
        if (matches(r)) return ask(std::string("this command ") + r.why);
    return allow();
}

// Chemins normalisés des scripts qu'une commande exécute (.bat, .ps1, .vbs...), pour demander
// confirmation avant de lancer un script que l'agent vient d'écrire.
inline std::vector<std::string> referencedScripts(const std::string& command) {
    static const std::vector<std::string> ext = {".bat", ".cmd", ".ps1", ".psm1", ".vbs", ".vbe",
                                                 ".js",  ".jse", ".wsf", ".wsh", ".hta", ".scr", ".py"};
    std::vector<std::string> out;
    const std::string canon = canonicalize(command);
    std::string slash = canon;
    std::replace(slash.begin(), slash.end(), '/', '\\');
    for (const auto& t : pathTokens(slash)) {
        const std::string low = lower(t);
        for (const auto& e : ext)
            if (low.size() >= e.size() && low.compare(low.size() - e.size(), e.size(), e) == 0) {
                out.push_back(t);
                break;
            }
    }
    // Jetons sans « \ » (nom nu, ex. « evil.bat ») : résolus dans le dossier de travail.
    std::string cur;
    auto consider = [&](const std::string& tok) {
        const std::string low = lower(tok);
        for (const auto& e : ext)
            if (low.size() >= e.size() && low.compare(low.size() - e.size(), e.size(), e) == 0)
                out.push_back(normalize(tok));
    };
    for (char ch : slash) {
        if (std::isspace((unsigned char)ch) || ch == '&' || ch == '|' || ch == ';' || ch == ',' || ch == '(' || ch == ')') {
            consider(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    consider(cur);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Texte des règles pour /safety.
inline std::vector<std::pair<std::string, std::string>> rules() {
    std::string roots;
    for (const auto& r : systemRoots()) roots += (roots.empty() ? "" : ", ") + r;
    return {
        {"Working directory", "the agent reads and writes freely in " + g.cwd},
        {"Outside it", "reading is allowed; writing or deleting asks you first"},
        {"System folders", "never changed by the agent: " + roots},
        {"Secrets", "never read or changed: CodePortal's config (API keys), .ssh, .aws, .azure, credentials"},
        {"API keys", "removed from the environment of the agent's commands (*_API_KEY, *_TOKEN, *_SECRET)"},
        {"Blocked commands", "format X:, diskpart, bcdedit, vssadmin delete, shutdown, HKLM changes, net user, Defender..."},
        {"Confirmed commands", "recursive deletes, git push/reset --hard/clean, installs, registry, downloads..."},
        {"Running scripts", "running a script the agent just wrote (.bat, .ps1...) asks you first"},
        {"Command tricks", "checks see through ^, quotes, 8.3 names, %VAR% and .. so paths cannot be disguised"},
    };
}

}  // namespace safety
