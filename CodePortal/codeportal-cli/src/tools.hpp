// tools.hpp : outils utilisables par l'utilisateur ET par l'assistant, sous la forme !nom(args).
//
//   !context()                      contexte de la discussion
//   !read_file('path')              lit un fichier (numéroté)
//   !list_dir('path')               liste un dossier
//   !create_file('path')            crée un fichier vide (ou avec le bloc <content>)
//   !write_file('path')             écrit/remplace un fichier avec le bloc <content>
//   !edit_file('path')              sans bloc : affiche le fichier. Avec <old>/<new> : remplacement exact + diff
//   !delete_file('path')            supprime un fichier (confirmation pour l'utilisateur)
#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "providers.hpp"
#include "state.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

// Une commande, avec les blocs de contenu qui la suivent dans la réponse de l'assistant.
struct Call {
    std::string command;  // ex. "!edit_file('a.txt')", "!dir", "/status"
    std::string content;  // bloc <content>
    std::string oldText;  // bloc <old>
    std::string newText;  // bloc <new>
    bool hasContent = false, hasOld = false, hasNew = false;
};

struct DiffLine {
    char kind;      // ' ' contexte, '-' supprimée, '+' ajoutée
    size_t lineNo;  // numéro de ligne dans le fichier
    std::string text;
};

struct ToolResult {
    bool ok = true;
    std::string summary;          // ligne courte affichée sous ⎿
    std::string output;           // contenu complet (renvoyé au modèle)
    std::vector<DiffLine> diff;   // modification avec contexte
    bool alwaysShow = false;      // afficher la sortie même côté assistant (shell, commandes)
};

inline bool isToolName(const std::string& n) {
    static const std::set<std::string> names = {"context",   "get_context", "read_file",   "list_dir",  "create_file",
                                                "write_file", "edit_file",  "delete_file", "create_dir"};
    return names.count(n) > 0;
}

// Développe ~ et les variables d'environnement (%USERPROFILE%\Desktop...).
std::string expandPath(const std::string& s) {
    std::string r = s;
    if (startsWith(r, "~")) {
        const char* home = std::getenv("USERPROFILE");
        r = std::string(home ? home : "") + r.substr(1);
    }
    std::wstring w = toWide(r);
    DWORD n = ExpandEnvironmentStringsW(w.c_str(), nullptr, 0);
    if (n == 0) return r;
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(w.c_str(), &out[0], n);
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return toUtf8(out);
}

// Sortie d'un programme console : certaines lignes arrivent dans la page de code de la console
// (850 en français, ex. les messages d'erreur de cmd.exe) au lieu d'UTF-8. Ces lignes sont converties.
std::string decodeConsoleOutput(const std::string& raw) {
    if (isValidUtf8(raw)) return raw;
    std::string out;
    size_t start = 0;
    while (start < raw.size()) {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos) end = raw.size();
        else ++end;
        const std::string line = raw.substr(start, end - start);
        if (isValidUtf8(line)) {
            out += line;
        } else {
            int n = MultiByteToWideChar(GetOEMCP(), 0, line.data(), (int)line.size(), nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(GetOEMCP(), 0, line.data(), (int)line.size(), &w[0], n);
            out += sanitizeUtf8(toUtf8(w));
        }
        start = end;
    }
    return out;
}

// Lance une commande cmd.exe sans clavier (stdin = NUL : un programme interactif reçoit
// immédiatement « fin d'entrée » au lieu de bloquer). Délai maximal et annulation possibles :
// toute l'arborescence de processus est alors arrêtée via un Job Windows.
std::string runProcess(const std::string& command, int timeoutSec, const std::function<bool()>& cancelled,
                       int* exitCode, bool* stopped) {
    if (exitCode) *exitCode = -1;
    if (stopped) *stopped = false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return "Could not create the output pipe.\n";
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"cmd.exe /d /s /c \"chcp 65001>nul & " + toWide(command) + L"\"";

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    BOOL ok = CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(readPipe);
        if (job) CloseHandle(job);
        return "Could not start the command.\n";
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::string out;
    auto drain = [&] {
        DWORD avail = 0;
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096];
            DWORD n = 0;
            if (!ReadFile(readPipe, buf, std::min<DWORD>(avail, sizeof buf), &n, nullptr) || n == 0) break;
            if (out.size() < 400000) out.append(buf, n);
        }
    };
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        drain();
        if (WaitForSingleObject(pi.hProcess, 40) == WAIT_OBJECT_0) {
            drain();
            break;
        }
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        const bool cancel = cancelled && cancelled();
        if (cancel || secs >= timeoutSec) {
            if (job) TerminateJobObject(job, 1);
            else TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            drain();
            if (stopped) *stopped = true;
            out += cancel ? "\n[stopped by the user]\n" : "\n[stopped after " + std::to_string(timeoutSec) + "s timeout]\n";
            break;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exitCode) *exitCode = (int)code;
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    if (job) CloseHandle(job);
    return decodeConsoleOutput(out);
}

// "!edit_file('a.txt')" -> name "edit_file", args {"a.txt"}. Retourne false si ce n'est pas un outil connu.
bool parseToolName(const std::string& command, std::string& name, std::vector<std::string>& args) {
    if (command.empty() || command[0] != '!') return false;
    std::string t = trim(command.substr(1));
    if (t.size() < 2 || t.back() != ')') return false;
    size_t open = t.find('(');
    if (open == std::string::npos) return false;
    name = trim(t.substr(0, open));
    if (!isToolName(name)) return false;
    std::string inside = t.substr(open + 1, t.size() - open - 2);
    args.clear();
    std::string cur;
    char quote = 0;
    bool quoted = false;
    for (char c : inside) {
        if (quote) {
            if (c == quote) quote = 0;
            else cur += c;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            quoted = true;
            continue;
        }
        if (c == ',') {
            args.push_back(trim(cur));
            cur.clear();
            quoted = false;
            continue;
        }
        cur += c;
    }
    if (!trim(cur).empty() || quoted) args.push_back(trim(cur));
    return true;
}

std::string plural(size_t n, const std::string& word) {
    // entry -> entries (consonne + y), mais key -> keys
    if (n != 1 && word.size() > 1 && word.back() == 'y' && std::string("aeiou").find(word[word.size() - 2]) == std::string::npos)
        return std::to_string(n) + " " + word.substr(0, word.size() - 1) + "ies";
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

ToolResult failTool(const std::string& msg) {
    ToolResult r;
    r.ok = false;
    r.summary = msg;
    return r;
}

bool askYesNo(const std::string& question) {
    if (g_headless) return false;  // pas de clavier : on refuse sans attendre
    std::cout << question << " [y/N] " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    answer = trim(answer);
    return answer == "y" || answer == "Y" || answer == "o" || answer == "O";
}

bool readText(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeText(const fs::path& p, const std::string& content) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return (bool)out;
}

std::string numbered(const std::string& text, size_t maxLines) {
    auto lines = splitLines(text);
    std::string out;
    for (size_t i = 0; i < lines.size() && i < maxLines; ++i)
        out += std::to_string(i + 1) + "\t" + lines[i] + "\n";
    if (lines.size() > maxLines) out += "... " + plural(lines.size() - maxLines, "more line") + "\n";
    return out;
}

std::string contextText() {
    const Provider* p = activeProvider();
    std::string out = "Working directory: " + g.cwd + "\n";
    out += "Provider: " + (p ? p->label + " (" + modelFor(*p) + ")" : std::string("none")) + "\n";
    out += "Messages: " + std::to_string(g.history.size()) + "\n\n";
    for (const auto& m : g.history) {
        if (m.role == "tool") {
            out += "[Tool result: " + m.toolName + "]\n" + truncate(m.text, 600) + "\n\n";
            continue;
        }
        out += std::string(m.role == "user" ? "[User]" : "[Assistant]") + "\n" + truncate(m.text, 1200) + "\n";
        for (const auto& tc : m.toolCalls) out += "  -> called " + tc.name + " " + truncate(tc.args, 300) + "\n";
        out += "\n";
    }
    return out;
}

// Diff d'un remplacement : 2 lignes de contexte avant, lignes retirées, lignes ajoutées, 2 lignes après.
void buildDiff(ToolResult& r, const std::string& before, const std::string& after, size_t offset,
               const std::string& oldText, const std::string& newText) {
    const size_t start = 1 + (size_t)std::count(before.begin(), before.begin() + offset, '\n');
    const auto beforeLines = splitLines(before);
    const auto afterLines = splitLines(after);
    const auto oldLines = splitLines(oldText);
    const auto newLines = splitLines(newText);

    for (size_t n = start > 2 ? start - 2 : 1; n < start && n <= beforeLines.size(); ++n)
        r.diff.push_back({' ', n, beforeLines[n - 1]});
    for (size_t i = 0; i < oldLines.size(); ++i) r.diff.push_back({'-', start + i, oldLines[i]});
    for (size_t i = 0; i < newLines.size(); ++i) r.diff.push_back({'+', start + i, newLines[i]});
    const size_t next = start + newLines.size();
    for (size_t n = next; n < next + 2 && n <= afterLines.size(); ++n) r.diff.push_back({' ', n, afterLines[n - 1]});
}

// ---------------------------------------------------------------------------
// Annulation (/undo) : état d'un fichier sauvegardé juste avant chaque modification.
// ---------------------------------------------------------------------------
struct Backup {
    std::string path;     // chemin absolu
    bool existed = false; // le fichier existait avant la modification
    bool isDir = false;   // dossier créé par create_dir
    std::string content;  // contenu d'origine
};

inline std::vector<Backup> g_undo;

std::string absolutePath(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    return (ec ? p : a).u8string();
}

void pushBackup(const fs::path& p, bool isDir = false) {
    Backup b;
    b.path = absolutePath(p);
    b.isDir = isDir;
    std::error_code ec;
    if (!isDir && fs::is_regular_file(p, ec)) {
        b.existed = true;
        readText(p, b.content);
    }
    g_undo.push_back(b);
    if (g_undo.size() > 100) g_undo.erase(g_undo.begin());
}

// Annule la dernière modification. Retourne une description, ou "" s'il n'y a rien à annuler.
std::string undoLast(bool* ok = nullptr) {
    if (ok) *ok = false;
    if (g_undo.empty()) return "";
    Backup b = g_undo.back();
    g_undo.pop_back();
    const fs::path p(toWide(b.path));
    std::error_code ec;
    if (b.isDir) {
        if (!fs::remove(p, ec)) return "Could not remove " + b.path + " (not empty?)";
        if (ok) *ok = true;
        return "Removed directory " + b.path;
    }
    if (b.existed) {
        if (!writeText(p, b.content)) return "Could not restore " + b.path;
        if (ok) *ok = true;
        return "Restored " + b.path;
    }
    if (!fs::remove(p, ec)) return "Could not delete " + b.path;
    if (ok) *ok = true;
    return "Deleted " + b.path + " (it did not exist before)";
}

// Exécute un outil. `fromAgent` : l'assistant n'a pas de confirmation à demander.
ToolResult runTool(const Call& c, const std::string& name, const std::vector<std::string>& args, bool fromAgent) {
    ToolResult r;
    if (name == "context" || name == "get_context") {
        r.output = contextText();
        r.summary = plural(g.history.size(), "message") + " in context";
        return r;
    }
    if (name == "list_dir") {
        const std::string dirName = args.empty() || args[0].empty() ? "." : expandPath(args[0]);
        fs::path dir(toWide(dirName));
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return failTool("Not a directory: " + dirName);
        std::vector<std::string> dirs, files;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            std::string n = entry.path().filename().u8string();
            if (entry.is_directory(ec)) dirs.push_back(n + "/");
            else files.push_back(n);
        }
        std::sort(dirs.begin(), dirs.end());
        std::sort(files.begin(), files.end());
        for (const auto& d : dirs) r.output += d + "\n";
        for (const auto& f : files) r.output += f + "\n";
        r.summary = plural(dirs.size() + files.size(), "entry");
        if (dirs.empty() && files.empty()) r.output = "(empty)\n";
        return r;
    }

    if (args.empty() || args[0].empty()) return failTool("Missing path: use !" + name + "('path')");
    const std::string path = expandPath(args[0]);
    const fs::path p(toWide(path));

    if (name == "create_dir") {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            r.summary = path + " already exists";
            return r;
        }
        if (!fs::create_directories(p, ec)) return failTool("Cannot create " + path + (ec ? ": " + ec.message() : ""));
        pushBackup(p, true);
        r.summary = "Created directory " + path;
        return r;
    }

    if (name == "read_file") {
        std::string text;
        if (!readText(p, text)) return failTool("Cannot read " + path);
        r.summary = "Read " + plural(splitLines(text).size(), "line");
        r.output = numbered(text, 400);
        return r;
    }

    if (name == "create_file") {
        std::error_code ec;
        if (fs::exists(p, ec)) return failTool(path + " already exists. Use edit_file or write_file instead.");
        std::string text = c.hasContent ? c.content : "";
        pushBackup(p);
        if (!writeText(p, text)) return failTool("Cannot create " + path);
        r.summary = "Created " + path + " (" + plural(splitLines(text).size(), "line") + ")";
        return r;
    }

    if (name == "write_file") {
        if (!c.hasContent) return failTool("write_file needs a <content> block after the command.");
        pushBackup(p);
        if (!writeText(p, c.content)) return failTool("Cannot write " + path);
        r.summary = "Wrote " + path + " (" + plural(splitLines(c.content).size(), "line") + ")";
        return r;
    }

    if (name == "edit_file") {
        std::string text;
        if (!readText(p, text)) return failTool("File not found: " + path);
        if (!c.hasOld) {  // ouverture : l'assistant voit le fichier, puis envoie <old>/<new>
            r.summary = "Opened " + path + " (" + plural(splitLines(text).size(), "line") +
                        "). Send <old> and <new> blocks to change it.";
            r.output = numbered(text, 400);
            return r;
        }
        if (c.oldText.empty()) return failTool("<old> is empty: give the exact text to replace.");
        size_t count = 0, found = std::string::npos, pos = 0;
        while ((pos = text.find(c.oldText, pos)) != std::string::npos) {
            ++count;
            found = pos;
            pos += c.oldText.size();
        }
        if (count == 0)
            return failTool("The <old> text was not found in " + path +
                            ". Open the file again with !edit_file('" + path + "') and copy the exact text.");
        if (count > 1)
            return failTool("The <old> text appears " + std::to_string(count) + " times in " + path +
                            ". Make it longer so it matches once.");
        const std::string before = text;
        text.replace(found, c.oldText.size(), c.newText);
        pushBackup(p);
        if (!writeText(p, text)) return failTool("Cannot write " + path);
        buildDiff(r, before, text, found, c.oldText, c.newText);
        size_t added = splitLines(c.newText).size(), removed = splitLines(c.oldText).size();
        r.summary = "Updated " + path + " with " + plural(added, "addition") + " and " + plural(removed, "removal");
        return r;
    }

    if (name == "delete_file") {
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return failTool("Not a file: " + path);
        if (!fromAgent && !g.autoRun && !askYesNo("Delete " + path + "?"))
            return failTool("Deletion cancelled by the user.");
        pushBackup(p);
        if (!fs::remove(p, ec)) return failTool("Cannot delete " + path + ": " + ec.message());
        r.summary = "Deleted " + path;
        return r;
    }

    return failTool("Unknown tool: " + name);
}
