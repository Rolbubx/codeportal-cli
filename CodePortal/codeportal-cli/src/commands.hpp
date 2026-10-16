// commands.hpp : commandes /xxx, shell !xxx et exécution commune (utilisateur et assistant).
#pragma once

#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "agenttools.hpp"
#include "live.hpp"
#include "providers.hpp"
#include "state.hpp"
#include "tools.hpp"
#include "ui.hpp"
#include "util.hpp"

// ---------------------------------------------------------------------------
// Clés et fournisseurs
// ---------------------------------------------------------------------------
// Récupère les modèles du fournisseur et propose de choisir dans une liste.
// Échap garde le modèle actuel. Si la liste est indisponible, le modèle actuel est conservé.
void chooseModel(const Provider& p) {
    std::vector<std::string> ids;
    std::string err;
    {
        Spinner spin("Fetching " + p.label + " models");
        spin.start();
        try {
            ids = listModels(p, keyFor(p));
        } catch (const std::exception& e) {
            err = e.what();
        }
    }
    const std::string current = modelFor(p);
    if (ids.empty()) {
        std::cout << "  " << P.muted << "Could not list the models" << (err.empty() ? "" : " (" + err + ")")
                  << "; keeping " << current << ". Change it with /model <name>." << ansi::reset << "\n";
        return;
    }
    std::sort(ids.begin(), ids.end());
    const std::string chosen = pickFromList("Choose a " + p.label + " model", ids, current);
    if (chosen.empty() || chosen == current) {
        const bool known = std::find(ids.begin(), ids.end(), current) != ids.end();
        if (known) std::cout << "  " << P.muted << "Model: " << current << ansi::reset << "\n";
        else
            std::cout << "  " << P.num << "⚠ " << ansi::reset << P.muted << current << " is not in " << p.label
                      << "'s list and may not work. Run /model to choose another one." << ansi::reset << "\n";
        return;
    }
    g.models[p.id] = chosen;
    g.activeId = p.id;
    saveConfig();
    printOk(p.label + " model set to " + chosen + ".");
}

void storeKey(const Provider& p, const std::string& key) {
    g.storedKeys[p.id] = key;
    g.activeId = p.id;
    saveConfig();
    printOk(p.label + " key saved (" + maskKey(key) + "). Active provider: " + p.label + ".");
    chooseModel(p);
}

// Vérifie la clé auprès du fournisseur donné, puis l'enregistre si elle est valide.
bool verifyAndStore(const Provider& p, const std::string& key) {
    std::string err;
    {
        Spinner spin("Verifying key with " + p.label);
        spin.start();
        err = validateKey(p, key);
    }
    if (!err.empty()) {
        printError(p.label + " rejected the key: " + err);
        return false;
    }
    storeKey(p, key);
    return true;
}

// Détection automatique : le préfixe de la clé désigne le fournisseur.
// La clé n'est envoyée qu'au fournisseur identifié, jamais aux autres.
bool addKeyAuto(const std::string& key) {
    auto matches = matchProviders(key);
    if (matches.empty()) {
        printError("Unrecognized key format. Name the provider: /key <provider> <key>");
        return false;
    }
    if (matches.size() > 1) {
        std::string names;
        for (const auto* p : matches) names += (names.empty() ? "" : ", ") + p->label;
        printError("This key format matches several providers (" + names + "). Name one: /key <provider> <key>");
        return false;
    }
    return verifyAndStore(*matches[0], key);
}

void cmdKey(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::string key = readMasked("API key (provider detected automatically): ");
        if (key.empty()) std::cout << "Cancelled.\n";
        else addKeyAuto(key);
        return;
    }
    const Provider* p = findProvider(args[1]);
    if (!p) {  // /key <clé> : fournisseur deviné
        addKeyAuto(args[1]);
        return;
    }
    std::string key = args.size() >= 3 ? args[2] : readMasked(p->label + " API key: ");
    if (key.empty()) {
        std::cout << "Cancelled.\n";
        return;
    }
    verifyAndStore(*p, key);
}

void cmdRemoveKey(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        printError("Usage: /removekey <provider>");
        return;
    }
    const Provider* p = findProvider(args[1]);
    if (!p) {
        printError("Unknown provider: " + args[1]);
        return;
    }
    if (!g.storedKeys.erase(p->id)) {
        printError("No saved key for " + p->label + " (environment variables are not modified).");
        return;
    }
    if (g.activeId == p->id) g.activeId.clear();
    saveConfig();
    printOk("Saved key for " + p->label + " removed.");
}

void cmdUse(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        printError("Usage: /use <provider>  (see /providers)");
        return;
    }
    const Provider* p = findProvider(args[1]);
    if (!p) {
        printError("Unknown provider: " + args[1]);
        return;
    }
    if (keyFor(*p).empty()) {
        printError("No key for " + p->label + ". Run /key " + p->id + " first.");
        return;
    }
    g.activeId = p->id;
    saveConfig();
    printOk("Active provider: " + p->label + " (" + modelFor(*p) + ").");
}

void cmdModel(const std::vector<std::string>& args) {
    const Provider* p = activeProvider();
    if (!p) {
        printError("No key configured. Run /key first.");
        return;
    }
    if (args.size() < 2) {  // sans argument : liste de choix
        chooseModel(*p);
        return;
    }
    g.models[p->id] = args[1];
    g.activeId = p->id;
    saveConfig();
    printOk(p->label + " model set to " + args[1] + ".");
}

void cmdModels() {
    const Provider* p = activeProvider();
    if (!p) {
        printError("No key configured. Run /key first.");
        return;
    }
    std::vector<std::string> ids;
    std::string err;
    {
        Spinner spin("Fetching " + p->label + " models");
        spin.start();
        try {
            ids = listModels(*p, keyFor(*p));
        } catch (const std::exception& e) {
            err = e.what();
        }
    }
    if (!err.empty()) {
        printError(err);
        return;
    }
    std::string current = modelFor(*p);
    std::cout << "\n" << p->label << " models (" << ids.size() << "):\n";
    for (const auto& id : ids)
        std::cout << (id == current ? P.accent : "") << (id == current ? "● " : "  ") << ansi::reset << id << "\n";
}

void cmdAuto(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "Auto-run: " << (g.autoRun ? "on" : "off") << "\n";
        return;
    }
    if (args[1] == "on") g.autoRun = true;
    else if (args[1] == "off") g.autoRun = false;
    else {
        printError("Usage: /auto [on|off]");
        return;
    }
    printOk(std::string("Auto-run ") + (g.autoRun ? "enabled" : "disabled") + ".");
}

void cmdTheme(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "Theme: " << g.theme << "  (use /theme dark or /theme light)\n";
        return;
    }
    if (args[1] != "dark" && args[1] != "light") {
        printError("Usage: /theme [dark|light]");
        return;
    }
    g.theme = args[1];
    applyTheme(g.theme);
    saveConfig();
    std::cout << "\x1b[2J\x1b[H";
    printBanner();
    printOk("Theme set to " + g.theme + ".");
}

void cmdCd(const std::string& rest) {
    if (rest.empty()) {
        std::cout << g.cwd << "\n";
        return;
    }
    std::error_code ec;
    std::filesystem::path target = std::filesystem::absolute(std::filesystem::path(toWide(rest)), ec);
    if (ec || !std::filesystem::is_directory(target, ec)) {
        printError("Not a directory: " + rest);
        return;
    }
    std::filesystem::current_path(target, ec);
    if (ec) {
        printError("Cannot enter " + rest + ": " + ec.message());
        return;
    }
    g.cwd = currentDir();
    printOk("Working directory: " + g.cwd);
}

void cmdExport(const std::string& rest) {
    std::string file = rest.empty() ? "codeportal-export.md" : rest;
    std::ofstream out(std::filesystem::path(toWide(file)), std::ios::binary | std::ios::trunc);
    if (!out) {
        printError("Cannot write: " + file);
        return;
    }
    out << "# CodePortal conversation\n\n";
    for (const auto& m : g.history)
        out << (m.role == "user" ? "## You\n\n" : "## CodePortal\n\n") << m.text << "\n\n";
    printOk("Conversation exported to " + file + ".");
}

void cmdHistory() {
    size_t user = 0, assistant = 0;
    for (const auto& m : g.history) (m.role == "user" ? user : assistant)++;
    std::cout << plural(g.history.size(), "message") << " (" << user << " from you, " << assistant
              << " from the assistant)\n";
}

void cmdTools() {
    struct T {
        const char* sig;
        const char* desc;
    };
    const std::vector<T> tools = {
        {"!context()", "conversation context (messages, provider, directory)"},
        {"!read_file('path')", "read a text file with line numbers"},
        {"!list_dir('path')", "list a directory"},
        {"!create_file('path')", "create a file (add a <content> block to fill it)"},
        {"!write_file('path')", "overwrite a file with a <content> block"},
        {"!edit_file('path')", "open a file; with <old>/<new> blocks, replace the exact text"},
        {"!delete_file('path')", "delete a file (asks for confirmation)"},
        {"!create_dir('path')", "create a directory (and missing parents)"},
        {"!<command>", "run a Windows cmd command (asks for confirmation unless /auto on)"},
    };
    std::cout << "\n";
    for (const auto& t : tools)
        std::cout << "  " << P.accent << t.sig << ansi::reset << std::string(std::max<int>(2, 24 - (int)displayLength(t.sig)), ' ')
                  << P.muted << t.desc << ansi::reset << "\n";
}

// Lance une commande shell et renvoie sa sortie (stdout + stderr).
// Demande confirmation pour l'utilisateur, sauf si /auto on est actif. L'assistant ne demande rien.
std::string runShell(const std::string& cmd, bool fromAgent, int* exitCode = nullptr, int timeoutSec = 120) {
    if (exitCode) *exitCode = -1;
    if (cmd.empty()) return "Usage: !<command>\n";
    if (!fromAgent && !g.autoRun && !askYesNo("Run " + cmd + "?")) return "Command refused by the user.\n";
    int code = -1;
    bool stopped = false;
    std::function<bool()> cancel;
    if (fromAgent) cancel = [] { return LiveView::get().interrupted(); };  // Échap pendant l'exécution
    std::string out = runProcess(cmd, timeoutSec, cancel, &code, &stopped);
    if (exitCode) *exitCode = stopped ? -1 : code;
    if (!stopped) out += "(exit code " + std::to_string(code) + ")\n";
    return out;
}

// ---------------------------------------------------------------------------
// Session, projet et diagnostic
// ---------------------------------------------------------------------------
constexpr const char* kVersion = "1.1.0";

std::string portalPath() { return g.cwd + "\\PORTAL.md"; }

std::string formatDuration(double secs) {
    if (secs < 60) return std::to_string((int)secs) + "s";
    const int m = (int)secs / 60, s = (int)secs % 60;
    if (m < 60) return std::to_string(m) + "m " + std::to_string(s) + "s";
    return std::to_string(m / 60) + "h " + std::to_string(m % 60) + "m";
}

void cmdUndo() {
    if (g_undo.empty()) {
        std::cout << "Nothing to undo.\n";
        return;
    }
    bool ok = false;
    const std::string msg = undoLast(&ok);
    if (ok) printOk(msg);
    else printError(msg);
    if (!g_undo.empty()) std::cout << "  " << P.muted << plural(g_undo.size(), "change") << " left to undo" << ansi::reset << "\n";
}

bool copyToClipboard(const std::string& text) {
    const std::wstring w = toWide(text);
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (h) {
        memcpy(GlobalLock(h), w.c_str(), bytes);
        GlobalUnlock(h);
        ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

std::string lastAnswer() {
    for (auto it = g.history.rbegin(); it != g.history.rend(); ++it)
        if (it->role == "assistant" && !trim(it->text).empty()) return trim(it->text);
    return "";
}

void cmdCopy() {
    const std::string a = lastAnswer();
    if (a.empty()) {
        printError("No answer to copy yet.");
        return;
    }
    if (copyToClipboard(a)) printOk("Copied the last answer (" + plural(splitLines(a).size(), "line") + ") to the clipboard.");
    else printError("Could not access the clipboard.");
}

void cmdUsage() {
    const double session = std::chrono::duration<double>(std::chrono::steady_clock::now() - g.started).count();
    auto line = [](const char* k, const std::string& v) {
        std::cout << "  " << P.muted << k << ansi::reset << std::string(std::max<int>(2, 18 - (int)displayLength(k)), ' ')
                  << v << "\n";
    };
    std::cout << "\n";
    line("Session", formatDuration(session));
    line("Agent working", formatDuration(g.usage.agentSeconds));
    line("Requests", std::to_string(g.usage.requests));
    line("Output tokens", "~" + formatTokens(g.usage.outputTokens));
    line("Tool calls", std::to_string(g.usage.toolCalls));
    line("Messages", std::to_string(g.history.size()));
    line("Undo stack", plural(g_undo.size(), "change"));
}

// ---- sessions sauvegardées (/save, /load, /sessions) ----
std::string sessionsDir() { return g.configDir + "\\sessions"; }

std::string sanitizeName(std::string n) {
    for (auto& c : n)
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '_';
    return n;
}

std::string defaultSessionName() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[32];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d_%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return buf;
}

std::string fileDate(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(toWide(path).c_str(), GetFileExInfoStandard, &data)) return "";
    FILETIME local;
    SYSTEMTIME t;
    FileTimeToLocalFileTime(&data.ftLastWriteTime, &local);
    FileTimeToSystemTime(&local, &t);
    char buf[32];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    return buf;
}

std::string messageJson(const Message& m) {
    std::string calls;
    for (const auto& tc : m.toolCalls)
        calls += std::string(calls.empty() ? "" : ",") + "{\"id\":" + jsonStr(tc.id) + ",\"name\":" + jsonStr(tc.name) +
                 ",\"args\":" + jsonStr(tc.args) + ",\"signature\":" + jsonStr(tc.signature) + "}";
    return "{\"role\":" + jsonStr(m.role) + ",\"text\":" + jsonStr(m.text) + ",\"toolCallId\":" + jsonStr(m.toolCallId) +
           ",\"toolName\":" + jsonStr(m.toolName) + ",\"thinking\":" + jsonStr(m.thinking) +
           ",\"thinkingSignature\":" + jsonStr(m.thinkingSignature) + ",\"toolCalls\":[" + calls + "]}";
}

std::vector<Message> messagesFromJson(const JVal& arr) {
    std::vector<Message> out;
    auto str = [](const JVal& o, const char* k) {
        const JVal* v = o.find(k);
        return v ? v->str : std::string();
    };
    for (const JVal& o : arr.arr) {
        Message m(str(o, "role"), str(o, "text"));
        m.toolCallId = str(o, "toolCallId");
        m.toolName = str(o, "toolName");
        m.thinking = str(o, "thinking");
        m.thinkingSignature = str(o, "thinkingSignature");
        if (const JVal* calls = o.find("toolCalls"))
            for (const JVal& c : calls->arr)
                m.toolCalls.push_back({str(c, "id"), str(c, "name"), str(c, "args"), str(c, "signature")});
        out.push_back(m);
    }
    return out;
}

void cmdSave(const std::string& rest) {
    if (g.history.empty()) {
        std::cout << "Nothing to save yet.\n";
        return;
    }
    const std::string name = rest.empty() ? defaultSessionName() : sanitizeName(rest);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(toWide(sessionsDir())), ec);
    const Provider* p = activeProvider();
    std::string j = "{\"version\":1,\"cwd\":" + jsonStr(g.cwd) + ",\"provider\":" + jsonStr(p ? p->id : "") +
                    ",\"messages\":[";
    for (size_t i = 0; i < g.history.size(); ++i) j += (i ? "," : "") + messageJson(g.history[i]);
    j += "]}";
    if (!writeText(std::filesystem::path(toWide(sessionsDir() + "\\" + name + ".json")), j)) {
        printError("Cannot write the session file.");
        return;
    }
    printOk("Session saved as " + std::string(ansi::bold) + name + ansi::reset + " (" +
            plural(g.history.size(), "message") + "). Restore it with /load " + name);
}

void cmdSessions() {
    std::error_code ec;
    struct Item {
        std::string name, date;
    };
    std::vector<Item> items;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(toWide(sessionsDir())), ec)) {
        if (e.path().extension() != ".json") continue;
        items.push_back({e.path().stem().u8string(), fileDate(e.path().u8string())});
    }
    if (items.empty()) {
        std::cout << "No saved sessions. Use /save [name].\n";
        return;
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.date > b.date; });
    std::cout << "\n";
    for (const auto& it : items)
        std::cout << "  " << P.accent << it.name << ansi::reset
                  << std::string(std::max<int>(2, 28 - (int)displayLength(it.name)), ' ') << P.muted << it.date
                  << ansi::reset << "\n";
    std::cout << "\n  " << P.muted << "Restore one with /load <name>" << ansi::reset << "\n";
}

void cmdLoad(const std::string& rest) {
    if (rest.empty()) {
        cmdSessions();
        return;
    }
    const std::string name = sanitizeName(rest);
    std::string text;
    if (!readText(std::filesystem::path(toWide(sessionsDir() + "\\" + name + ".json")), text)) {
        printError("No session named " + name + " (see /sessions).");
        return;
    }
    JVal root;
    JsonParser parser(text);
    const JVal* msgs = parser.parse(root) ? root.find("messages") : nullptr;
    if (!msgs || msgs->type != JVal::ARR) {
        printError("The session file is damaged.");
        return;
    }
    g.history = messagesFromJson(*msgs);
    printOk("Loaded session " + name + " (" + plural(g.history.size(), "message") + ").");
    for (auto it = g.history.rbegin(); it != g.history.rend(); ++it)
        if (it->role == "user") {
            std::cout << "  " << P.muted << "last request: " << truncate(it->text, 100) << ansi::reset << "\n";
            break;
        }
}

void cmdCompact() {
    if (g.history.size() < 2) {
        std::cout << "Nothing to compact yet.\n";
        return;
    }
    std::string err, summary;
    {
        Spinner spin("Compacting");
        spin.start();
        summary = askAgent(
            "Summarize this conversation between a user and a coding agent so the agent can continue the work. "
            "Keep the user's goals, decisions, files created or changed (with paths), important results and what "
            "remains to do. Be concise and factual, in Markdown bullet points.\n\n" + contextText(),
            "", &err);
    }
    if (summary.empty()) {
        printError("Compaction failed: " + err);
        return;
    }
    const size_t before = g.history.size();
    g.history.clear();
    g.history.push_back({"user", "Summary of our conversation so far:\n" + summary});
    g.history.push_back({"assistant", "Understood. I will continue from this summary."});
    printOk("Compacted " + plural(before, "message") + " into a summary:");
    for (const auto& l : md::render(summary, frameWidth() - 4)) std::cout << "  " << l << "\n";
}

void cmdDoctor() {
    auto check = [](int level, const std::string& name, const std::string& detail) {
        const char* icon = level == 0 ? "✔" : level == 1 ? "⚠" : "✖";
        const char* color = level == 0 ? P.ok : level == 1 ? P.num : P.err;
        std::cout << "  " << color << icon << ansi::reset << " " << name
                  << std::string(std::max<int>(2, 26 - (int)displayLength(name)), ' ') << P.muted << detail
                  << ansi::reset << "\n";
    };
    std::cout << "\n";
    DWORD mode = 0;
    const bool vt = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) && (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    check(vt ? 0 : 2, "Terminal colors", vt ? "VT mode enabled" : "VT mode disabled: colors and animations will not work");
    const size_t w = consoleWidth(), h = consoleHeight();
    check(w >= 80 ? 0 : 1, "Terminal size",
          std::to_string(w) + "x" + std::to_string(h) + (w < 80 ? " (80+ columns recommended)" : ""));
    {
        std::ofstream test(g.configPath, std::ios::app);
        check(test ? 0 : 2, "Config file", g.configPath);
    }
    const Provider* p = activeProvider();
    if (!p) {
        check(2, "API key", "none configured: run /key");
    } else {
        std::string err;
        {
            Spinner spin("Checking " + p->label);
            spin.start();
            err = validateKey(*p, keyFor(*p));
        }
        check(err.empty() ? 0 : 2, p->label + " API key", err.empty() ? "valid " + maskKey(keyFor(*p)) : err);
        check(0, "Model", modelFor(*p));
    }
    int code = -1;
    bool stopped = false;
    runProcess("ver", 10, nullptr, &code, &stopped);
    check(code == 0 ? 0 : 2, "Shell (cmd.exe)", code == 0 ? "works, no keyboard input, 120s limit" : "failed to run");
    std::error_code ec;
    const bool portal = std::filesystem::exists(std::filesystem::path(toWide(portalPath())), ec);
    check(portal ? 0 : 1, "PORTAL.md", portal ? "found: loaded into the agent's instructions" : "missing: run /init");
}

void cmdVersion() {
    std::cout << P.accent << "✻ " << ansi::reset << ansi::bold << "CodePortal " << kVersion << ansi::reset << P.muted
              << "  (built " << __DATE__ << " · " << providers().size() << " providers · " << toolSpecs().size()
              << " agent tools)" << ansi::reset << "\n";
}

void cmdOpen(const std::string& rest) {
    if (rest.empty()) {
        printError("Usage: /open <file or folder>");
        return;
    }
    const std::string path = expandPath(rest);
    HINSTANCE h = ShellExecuteW(nullptr, L"open", toWide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)h > 32) printOk("Opened " + path);
    else printError("Cannot open " + path);
}

void printMarkdownFile(const std::string& path, const std::string& title) {
    std::string text;
    if (!readText(std::filesystem::path(toWide(path)), text)) {
        printError("Cannot read " + path);
        return;
    }
    std::cout << "\n  " << P.muted << title << " · " << plural(splitLines(text).size(), "line") << ansi::reset << "\n\n";
    for (const auto& l : md::render(text, frameWidth() - 4)) std::cout << "  " << l << "\n";
}

void cmdMd(const std::string& rest) {
    if (rest.empty()) {
        printError("Usage: /md <file.md>");
        return;
    }
    printMarkdownFile(expandPath(rest), expandPath(rest));
}

void cmdMemory() {
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(toWide(portalPath())), ec)) {
        std::cout << P.muted << "No PORTAL.md in this folder. Run /init to create one: the agent reads it at every request."
                  << ansi::reset << "\n";
        return;
    }
    printMarkdownFile(portalPath(), "PORTAL.md (project instructions, sent to the agent)");
}

// Dispatch des commandes slash (utilisateur). Retourne false pour quitter.
bool handleCommand(const std::string& line) {
    std::vector<std::string> args = splitWords(line);
    const std::string& cmd = args[0];
    const std::string rest = trim(line.substr(cmd.size()));
    if (cmd == "/exit" || cmd == "/quit") return false;
    if (cmd == "/help") printHelp();
    else if (cmd == "/status") printStatus();
    else if (cmd == "/providers") printProviders();
    else if (cmd == "/key") cmdKey(args);
    else if (cmd == "/removekey") cmdRemoveKey(args);
    else if (cmd == "/use" || cmd == "/provider") cmdUse(args);
    else if (cmd == "/model") cmdModel(args);
    else if (cmd == "/models") cmdModels();
    else if (cmd == "/auto") cmdAuto(args);
    else if (cmd == "/theme") cmdTheme(args);
    else if (cmd == "/tools") cmdTools();
    else if (cmd == "/clear") {
        g.history.clear();
        std::cout << "Conversation cleared.\n";
    } else if (cmd == "/history") cmdHistory();
    else if (cmd == "/export") cmdExport(rest);
    else if (cmd == "/cwd") std::cout << g.cwd << "\n";
    else if (cmd == "/cd") cmdCd(rest);
    else if (cmd == "/ls") {
        ToolResult r = runTool(Call{}, "list_dir", {rest}, false);
        if (!r.ok) printError(r.summary);
        else std::cout << r.output;
    } else if (cmd == "/read") {
        ToolResult r = runTool(Call{}, "read_file", {rest}, false);
        if (!r.ok) printError(r.summary);
        else std::cout << r.summary << "\n" << r.output;
    } else if (cmd == "/context") {
        std::cout << contextText();
    } else if (cmd == "/undo") cmdUndo();
    else if (cmd == "/copy") cmdCopy();
    else if (cmd == "/usage" || cmd == "/cost") cmdUsage();
    else if (cmd == "/save") cmdSave(rest);
    else if (cmd == "/load" || cmd == "/resume") cmdLoad(rest);
    else if (cmd == "/sessions") cmdSessions();
    else if (cmd == "/compact") cmdCompact();
    else if (cmd == "/doctor") cmdDoctor();
    else if (cmd == "/version") cmdVersion();
    else if (cmd == "/open") cmdOpen(rest);
    else if (cmd == "/md") cmdMd(rest);
    else if (cmd == "/memory") cmdMemory();
    else printError("Unknown command: " + cmd + " (type /help)");
    return true;
}

// Commandes slash que l'assistant a le droit d'utiliser. Le reste est réservé à l'utilisateur.
bool agentAllowed(const std::string& name) {
    static const std::set<std::string> allowed = {"/help",  "/status", "/providers", "/models", "/use",
                                                  "/provider", "/model", "/history", "/cwd", "/cd",
                                                  "/ls",    "/read",   "/context", "/tools",
                                                  "/md",    "/memory", "/usage",   "/version"};
    return allowed.count(name) > 0;
}

std::string toolTitle(const std::string& name, const std::vector<std::string>& args) {
    static const std::vector<std::pair<std::string, std::string>> titles = {
        {"context", "Context"},    {"get_context", "Context"}, {"read_file", "Read"},
        {"list_dir", "List"},      {"create_file", "Write"},   {"write_file", "Write"},
        {"edit_file", "Update"},   {"delete_file", "Delete"},  {"create_dir", "Mkdir"},
        {"run_command", "Bash"},   {"tree", "Tree"},           {"find_files", "Find"},
        {"search_files", "Search"}, {"file_info", "Info"},     {"replace_lines", "Update"},
        {"insert_lines", "Update"}, {"append_file", "Append"}, {"copy_path", "Copy"},
        {"move_path", "Move"},     {"delete_dir", "Delete"},   {"undo_last_change", "Undo"},
        {"run_powershell", "PowerShell"}, {"start_process", "Start"}, {"stop_process", "Stop"},
        {"list_processes", "Processes"}, {"wait", "Wait"},     {"fetch_url", "Fetch"},
        {"download_file", "Download"}, {"web_search", "WebSearch"}, {"open_path", "Open"},
        {"clipboard_read", "Clipboard"}, {"clipboard_write", "Clipboard"}, {"system_info", "System"},
        {"get_env", "Env"},        {"calculate", "Calc"},      {"update_plan", "Plan"},
        {"save_memory", "Remember"}};
    std::string label = name;
    for (const auto& [n, t] : titles)
        if (n == name) label = t;
    return label + "(" + (args.empty() ? "" : args[0]) + ")";
}

// Diff façon Claude Code : numéro de ligne, signe, fond rouge/vert sur toute la largeur. Au plus `limit` lignes.
void printDiffLines(const std::vector<DiffLine>& diff, size_t limit) {
    const size_t width = frameWidth() - 6;
    for (size_t i = 0; i < diff.size() && i < limit; ++i) {
        const DiffLine& d = diff[i];
        std::string num = std::to_string(d.lineNo);
        if (num.size() < 4) num = std::string(4 - num.size(), ' ') + num;
        std::string body = num + " " + (d.kind == ' ' ? std::string(" ") : std::string(1, d.kind)) + " " + d.text;
        if (displayLength(body) > width) body = utf8Prefix(body, width - 1) + "…";
        const size_t pad = width - std::min(width, displayLength(body));
        if (d.kind == ' ') std::cout << "      " << P.muted << body << ansi::reset << "\n";
        else
            std::cout << "      " << (d.kind == '+' ? P.addBg : P.delBg) << (d.kind == '+' ? P.add : P.del) << body
                      << std::string(pad, ' ') << ansi::reset << "\n";
    }
}

// Sortie texte, indentée sous le résumé. `first` : la première ligne part sous ⎿ si aucun résumé.
void printOutputLines(const std::vector<std::string>& lines, bool first, size_t limit) {
    for (size_t i = 0; i < lines.size() && i < limit; ++i)
        std::cout << P.muted << (i == 0 && first ? "  ⎿  " : "     ") << lines[i] << ansi::reset << "\n";
}

// Dernière sortie réduite (diff ou sortie coupée) : Ctrl+O l'affiche en entier pendant que l'agent continue.
struct Expandable {
    std::string title;
    ToolResult result;
    std::vector<std::string> outputLines;
};
std::mutex g_expandMu;
std::optional<Expandable> g_expand;

// Affiche le résultat d'une commande : résumé sous ⎿ (rouge si erreur), diff coloré, sortie si demandée.
void printOutcome(const ToolResult& r, bool full, const std::string& title = "") {
    if (!r.summary.empty()) std::cout << (r.ok ? P.muted : P.err) << "  ⎿  " << r.summary << ansi::reset << "\n";
    const size_t maxDiff = 40, maxLines = 25;
    printDiffLines(r.diff, maxDiff);
    bool truncated = r.diff.size() > maxDiff;
    if (truncated)
        std::cout << P.muted << "      … +" << r.diff.size() - maxDiff << " lines (ctrl+o to expand)" << ansi::reset << "\n";

    Expandable e{title, r, {}};
    if ((full || r.alwaysShow) && !r.output.empty()) {
        e.outputLines = splitLines(r.output);
        while (!e.outputLines.empty() && trim(e.outputLines.back()).empty()) e.outputLines.pop_back();
        printOutputLines(e.outputLines, r.summary.empty(), maxLines);
        if (e.outputLines.size() > maxLines) {
            truncated = true;
            std::cout << P.muted << "     … +" << e.outputLines.size() - maxLines << " more lines (ctrl+o to expand)"
                      << ansi::reset << "\n";
        }
    }
    if (truncated) {
        std::lock_guard<std::mutex> lk(g_expandMu);
        g_expand = std::move(e);
    }
}

// Ctrl+O : affiche en entier la dernière sortie réduite. Appelée par le fil du spinner, sans bloquer l'agent.
void expandLastOutcome() {
    std::optional<Expandable> e;
    {
        std::lock_guard<std::mutex> lk(g_expandMu);
        e = std::move(g_expand);
        g_expand.reset();
    }
    if (!e) return;
    LiveView::get().writeAside([&] {
        std::cout << P.muted << "  ⎿  " << (e->title.empty() ? "full output" : e->title + " · full output") << ansi::reset
                  << "\n";
        printDiffLines(e->result.diff, e->result.diff.size());
        printOutputLines(e->outputLines, false, e->outputLines.size());
    });
}

// Exécute une commande (slash, outil ou shell). Retourne le texte renvoyé à l'assistant.
// fromAgent = true : pas de confirmation, commandes restreintes, résultat résumé.
std::string executeCall(const Call& call, bool fromAgent) {
    const std::string& cmd = call.command;
    if (cmd.empty()) return "Empty command.";

    if (cmd[0] == '/') {
        std::string verb = splitWords(cmd)[0];
        printToolCall(cmd);
        if (fromAgent && !agentAllowed(verb)) {
            ToolResult r = failTool("Not allowed for the assistant: " + verb);
            printOutcome(r, false);
            return r.summary;
        }
        std::string out = captureOutput([&] { handleCommand(cmd); });
        ToolResult r;
        r.output = out;
        r.alwaysShow = true;
        printOutcome(r, false, cmd);
        return out;
    }

    if (cmd[0] == '!') {
        std::string name;
        std::vector<std::string> args;
        const bool isTool = parseToolName(cmd, name, args);
        const std::string shell = trim(cmd.substr(1));
        const std::string title = isTool ? toolTitle(name, args) : "Bash(" + shell + ")";

        // Pour l'assistant : puce grise + spinner « Running… » pendant l'exécution, puis puce verte ou rouge.
        // Pour l'utilisateur : une confirmation peut être demandée, donc pas d'animation.
        printToolCall(title, fromAgent ? ToolState::Pending : ToolState::Ok);
        ToolResult r;
        int exitCode = 0;
        {
            Spinner spin("Running");
            if (fromAgent) spin.start();
            if (isTool) {
                r = runTool(call, name, args, fromAgent);
            } else {
                r.output = runShell(shell, fromAgent, &exitCode);
                r.alwaysShow = true;
                r.ok = exitCode == 0;
            }
        }
        if (fromAgent) redrawToolCall(title, r.ok ? ToolState::Ok : ToolState::Error);
        printOutcome(r, !fromAgent, title);
        if (!isTool) return r.output;
        std::string modelText = (r.ok ? "" : "Error: ") + r.summary;
        if (!r.output.empty()) modelText += "\n" + r.output;
        return modelText;
    }

    printToolCall(cmd);
    ToolResult r = failTool("Invalid command: it must start with / or !");
    printOutcome(r, false);
    return r.summary;
}

// Diff en JSON pour le journal d'activité : [{"k":"+","n":12,"t":"texte"}, ...]
std::string diffJson(const std::vector<DiffLine>& diff) {
    std::string o = "[";
    for (size_t i = 0; i < diff.size() && i < 300; ++i)
        o += std::string(i ? "," : "") + "{\"k\":" + jsonStr(std::string(1, diff[i].kind)) +
             ",\"n\":" + std::to_string(diff[i].lineNo) + ",\"t\":" + jsonStr(diff[i].text) + "}";
    return o + "]";
}

// Exécute un appel d'outil natif de l'assistant (arguments JSON). Retourne le résultat pour le modèle.
// Puce grise + spinner pendant l'exécution (Échap pour arrêter), puis verte ou rouge.
std::string executeAgentTool(const ToolCallRec& tc) {
    JVal args;
    JsonParser parser(tc.args.empty() ? "{}" : tc.args);
    const bool parsed = parser.parse(args) && args.type == JVal::OBJ;
    auto str = [&](const char* k) -> std::string {
        const JVal* v = parsed ? args.find(k) : nullptr;
        return v ? v->str : "";
    };
    auto has = [&](const char* k) { return parsed && args.find(k) != nullptr; };

    const std::string command = str("command");
    const std::string title = tc.name == "run_command" ? "Bash(" + command + ")" : toolTitle(tc.name, {agentToolArg(args)});
    printToolCall(title, ToolState::Pending);
    activity("tool_start", "{\"id\":" + jsonStr(tc.id) + ",\"tool\":" + jsonStr(tc.name) + ",\"args\":" + jsonStr(tc.args) + "}");

    ToolResult r;
    {
        Spinner spin("Running", true);
        spin.start();
        if (!parsed) {
            r = failTool("Invalid JSON arguments: " + truncate(tc.args, 200));
        } else if (tc.name == "run_command") {
            int timeout = 120;
            try {
                if (has("timeout_seconds")) timeout = std::clamp(std::stoi(str("timeout_seconds")), 5, 600);
            } catch (...) {
            }
            int code = -1;
            r.output = runShell(command, true, &code, timeout);
            r.ok = code == 0;
            r.alwaysShow = true;
        } else if (isAgentTool(tc.name)) {  // outils de agenttools.hpp (dont read_file par plage de lignes)
            r = runAgentTool(tc.name, args);
        } else if (isToolName(tc.name)) {
            Call c;
            c.content = str("content");
            c.hasContent = has("content");
            c.oldText = str("old_text");
            c.hasOld = has("old_text");
            c.newText = str("new_text");
            c.hasNew = has("new_text");
            r = runTool(c, tc.name, {str("path")}, true);
        } else {
            r = failTool("Unknown tool: " + tc.name);
        }
    }
    redrawToolCall(title, r.ok ? ToolState::Ok : ToolState::Error);
    printOutcome(r, false, title);

    std::string forModel = (r.ok ? "" : "Error: ") + r.summary;
    if (!r.output.empty()) forModel += (forModel.empty() ? "" : "\n") + r.output;
    if (forModel.empty()) forModel = "Done.";
    activity("tool_end", "{\"id\":" + jsonStr(tc.id) + ",\"tool\":" + jsonStr(tc.name) + ",\"ok\":" +
                             std::string(r.ok ? "true" : "false") + ",\"summary\":" + jsonStr(r.summary) +
                             ",\"diff\":" + diffJson(r.diff) + ",\"result\":" + jsonStr(truncate(forModel, 4000)) + "}");
    return forModel;
}
