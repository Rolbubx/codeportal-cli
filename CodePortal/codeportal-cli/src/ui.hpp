// ui.hpp : rendu de l'interface, dans le style de Claude Code.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "live.hpp"
#include "tui.hpp"
#include "markdown.hpp"
#include "providers.hpp"
#include "util.hpp"

// Largeur utilisée pour les cadres (bornée pour rester lisible).
size_t frameWidth() {
    return std::clamp<size_t>(consoleWidth(), 40, 110);
}

bool isConsole() {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
}

void pause(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string modelStatus() {
    const Provider* act = activeProvider();
    return act ? act->label + " · " + modelFor(*act) : "no provider (type /key)";
}

std::string configuredKeysList() {
    std::string out;
    for (const auto& p : providers()) {
        if (keyFor(p).empty()) continue;
        if (!out.empty()) out += ", ";
        out += p.id;
    }
    return out.empty() ? "none" : out;
}

// Chemin raccourci par la gauche si trop long : "…\dossier\projet"
std::string shortenPath(const std::string& path, size_t max) {
    if (displayLength(path) <= max) return path;
    return "…" + path.substr(path.size() - (max - 1));
}

// ---------------------------------------------------------------------------
// Écran d'accueil animé
// ---------------------------------------------------------------------------
void printBanner() {
    const size_t inner = std::min<size_t>(std::max<size_t>(56, 0), frameWidth() - 2);
    const std::string cwd = shortenPath(g.cwd, inner - 10);
    const std::string model = modelStatus();
    const bool animate = isConsole() && !tui::active();

    auto row = [&](const std::string& plain, const std::string& styled) {
        size_t pad = inner - 2 - std::min(inner - 2, displayLength(plain));
        return std::string(P.accent) + "│" + ansi::reset + " " + styled + std::string(pad, ' ') + " " + P.accent + "│" +
               ansi::reset;
    };

    out() << "\n" << P.accent << "╭" << repeatStr("─", inner) << "╮" << ansi::reset << "\n";

    // Titre : l'étoile tourne et un reflet traverse le texte, puis il se fige.
    const std::string title = "Welcome to CodePortal!";
    const std::string titlePlain = "✻ " + title;
    if (animate) {
        static const char* glyphs[] = {"·", "✢", "✳", "✶", "✻", "✽"};
        const long frames = (long)displayLength(title) + 6;
        for (long f = 0; f < frames; ++f) {
            std::string styled = std::string(P.accent) + glyphs[f % 6] + ansi::reset + " " +
                                 shimmerText(title, f - 3, P.accent);
            out() << "\r\x1b[2K" << row(titlePlain, styled) << std::flush;
            pause(28);
        }
    }
    const std::string titleStyled = std::string(P.accent) + "✻" + ansi::reset + " Welcome to " + ansi::bold + P.text +
                                    "CodePortal" + ansi::reset + "!";
    out() << "\r\x1b[2K" << row(titlePlain, titleStyled) << "\n";

    const std::vector<std::pair<std::string, std::string>> rows = {
        {"", ""},
        {"  /help for help, /status for your current setup",
         std::string("  ") + P.muted + "/help for help, /status for your current setup" + ansi::reset},
        {"", ""},
        {"  cwd: " + cwd, std::string("  ") + P.muted + "cwd: " + cwd + ansi::reset},
        {"  model: " + model, std::string("  ") + P.muted + "model: " + model + ansi::reset},
    };
    for (const auto& [plain, styled] : rows) {
        out() << row(plain, styled) << "\n" << std::flush;
        if (animate) pause(25);
    }
    out() << P.accent << "╰" << repeatStr("─", inner) << "╯" << ansi::reset << "\n";
}

// ---------------------------------------------------------------------------
// Registre des commandes : sert à /help, au menu qui s'ouvre en tapant « / » et à la complétion (Tab).
// ---------------------------------------------------------------------------
struct CommandInfo {
    const char* name;
    const char* args;  // "<x>" obligatoire, "[x]" facultatif
    const char* desc;
    const char* section;
};

const std::vector<CommandInfo>& commandList() {
    static const std::vector<CommandInfo> list = {
        {"/help", "", "show all commands", "General"},
        {"/clear", "", "clear the conversation", "Conversation"},
        {"/compact", "", "summarize the conversation to free up context", "Conversation"},
        {"/retry", "", "resume a failed turn, or resend your last message", "Conversation"},
        {"/copy", "", "copy the last answer to the clipboard", "Conversation"},
        {"/context", "", "show what the agent currently knows", "Conversation"},
        {"/history", "", "count the messages in the conversation", "Conversation"},
        {"/export", "[file]", "save the conversation as Markdown", "Conversation"},
        {"/save", "[name]", "save this session", "Sessions"},
        {"/load", "<name>", "restore a saved session", "Sessions"},
        {"/sessions", "", "list saved sessions", "Sessions"},
        {"/init", "", "analyze the project and write PORTAL.md", "Project"},
        {"/memory", "", "show PORTAL.md, the project instructions", "Project"},
        {"/undo", "", "revert the last file change made by the agent", "Project"},
        {"/cwd", "", "show the working directory", "Project"},
        {"/cd", "<path>", "change the working directory", "Project"},
        {"/ls", "[path]", "list a directory", "Project"},
        {"/read", "<file>", "show a text file with line numbers", "Project"},
        {"/md", "<file>", "render a Markdown file", "Project"},
        {"/open", "<file>", "open a file or folder with its default app", "Project"},
        {"/tools", "", "tools the agent can use", "Project"},
        {"/plan", "", "show the agent's current plan", "Project"},
        {"/safety", "", "what the agent is allowed to do", "Project"},
        {"/key", "[provider] [key]", "add an API key (provider detected from the key)", "Providers"},
        {"/removekey", "<provider>", "delete a saved key", "Providers"},
        {"/providers", "", "list providers and their key status", "Providers"},
        {"/use", "<provider>", "switch the active provider", "Providers"},
        {"/models", "", "list the models of the active provider", "Providers"},
        {"/model", "[name]", "show or change the model", "Providers"},
        {"/theme", "[dark|light]", "switch the color theme", "Settings"},
        {"/auto", "[on|off]", "run your !commands without confirmation", "Settings"},
        {"/status", "", "provider, model and settings", "Settings"},
        {"/usage", "", "time, requests, tokens and tools used", "Settings"},
        {"/doctor", "", "check that everything works", "Settings"},
        {"/version", "", "CodePortal version", "Settings"},
        {"/exit", "", "quit CodePortal", "General"},
    };
    return list;
}

void printTips() {
    out() << "\n " << P.muted << "Tips for getting started:" << ansi::reset << "\n\n";
    auto b = [](const char* s) { return std::string(ansi::bold) + P.text + s + ansi::reset + P.muted; };
    const std::vector<std::string> tips = {
        "Ask CodePortal to explain, edit or test your code",
        "Run " + b("/init") + " to create a PORTAL.md file with instructions for CodePortal",
        "Type " + b("/") + " to browse commands · " + b("↑") + " for your previous messages · " + b("Esc") +
            " to interrupt the agent",
        "While the agent works, type a message: it reads it at its next step · " + b("PgUp") + " to scroll back",
        b("/key") + " to add an API key · " + b("/safety") + " for the agent's limits · " + b("?") + " for shortcuts",
    };
    for (size_t i = 0; i < tips.size(); ++i)
        out() << " " << P.muted << (i + 1) << ". " << tips[i] << ansi::reset << "\n";
}

void printShortcuts() {
    auto item = [](const char* key, const char* desc) {
        out() << "  " << ansi::bold << P.text << key << ansi::reset
              << std::string(std::max<int>(2, 22 - (int)displayLength(key)), ' ') << P.muted << desc
              << ansi::reset << "\n";
    };
    out() << "\n";
    item("/", "command menu · Tab or Enter to pick, Esc to close");
    item("↑ ↓", "previous / next message (or move in a menu or list)");
    item("← → Home End", "move the cursor · Ctrl+← → by word");
    item("Ctrl+W  Ctrl+U", "delete the previous word · clear the line");
    item("PgUp PgDn  wheel", "scroll the conversation · End goes back to the bottom");
    item("Esc", "interrupt the agent · close a menu · skip a question");
    item("Enter while working", "send a message the agent reads at its next step");
    item("1-9", "pick an option in a question or confirmation");
    item("Ctrl+C", "clear the line, or quit when it is empty");
    item("!<command>", "run a shell command yourself");
    item("/safety  /plan", "what the agent may do · its current plan");
}

void printHelp() {
    static const char* sections[] = {"Conversation", "Sessions", "Project", "Providers", "Settings", "General"};
    out() << "\n";
    for (const char* section : sections) {
        out() << " " << ansi::bold << P.text << section << ansi::reset << "\n";
        for (const auto& c : commandList()) {
            if (std::string(c.section) != section) continue;
            const std::string usage = std::string(c.name) + (*c.args ? std::string(" ") + c.args : "");
            out() << "   " << P.accent << c.name << ansi::reset << P.muted << (*c.args ? std::string(" ") + c.args : "")
                      << ansi::reset << std::string(std::max<int>(2, 26 - (int)displayLength(usage)), ' ') << P.muted
                      << c.desc << ansi::reset << "\n";
        }
        out() << "\n";
    }
    out() << " " << P.muted << "Any other message goes to the agent: it reasons live, uses its tools, checks the results\n"
              << " and tells you when it is done. Shell: !<command>. File tools: /tools. Keys: ?" << ansi::reset << "\n";
}

void printStatus() {
    const Provider* act = activeProvider();
    auto line = [](const char* k, const std::string& v) {
        out() << "  " << P.muted << k << ansi::reset << std::string(std::max<int>(2, 18 - (int)displayLength(k)), ' ')
                  << v << "\n";
    };
    out() << "\n";
    line("Provider", act ? act->label : std::string("none"));
    line("Model", act ? modelFor(*act) : std::string("-"));
    line("Theme", g.theme);
    line("Auto-run", g.autoRun ? "on" : "off");
    line("Working dir", g.cwd);
    line("Config file", g.configPath);
}

void printProviders() {
    const Provider* act = activeProvider();
    out() << "\n";
    for (const auto& p : providers()) {
        std::string source;
        std::string key = keyFor(p, &source);
        std::string status = key.empty() ? std::string(P.muted) + "no key" + ansi::reset
                                         : std::string(P.ok) + "✔ " + maskKey(key) + ansi::reset + " " + P.muted +
                                               "(" + source + ")" + ansi::reset;
        bool active = act == &p;
        out() << "  " << (active ? P.accent : "") << (active ? "● " : "  ") << ansi::reset << ansi::bold << p.id
                  << ansi::reset << std::string(std::max<int>(2, 13 - (int)p.id.size()), ' ') << status << "\n";
    }
}

void printOk(const std::string& msg) {
    out() << P.ok << "✔ " << ansi::reset << msg << "\n";
}

void printError(const std::string& msg) {
    out() << "\r\x1b[2K";
    auto lines = wrapWords(msg, frameWidth() - 4);
    for (size_t i = 0; i < lines.size(); ++i)
        out() << (i == 0 ? std::string(P.err) + "✖ " + ansi::reset : std::string("  ")) << P.err << lines[i]
                  << ansi::reset << "\n";
}

// ---------------------------------------------------------------------------
// Appels d'outils : ⏺ Bash(dir)  — gris pendant l'exécution, vert si OK, rouge si erreur.
// ---------------------------------------------------------------------------
enum class ToolState { Pending, Ok, Error };

std::string toolLine(const std::string& title, ToolState state) {
    std::string label = title;
    const size_t max = frameWidth() - 3;
    if (displayLength(label) > max) label = utf8Prefix(label, max - 1) + "…";
    const size_t paren = label.find('(');
    const std::string name = paren == std::string::npos ? label : label.substr(0, paren);
    const std::string rest = paren == std::string::npos ? "" : label.substr(paren);
    const char* color = state == ToolState::Pending ? P.warn : state == ToolState::Ok ? P.ok : P.err;
    return std::string(color) + "⏺" + ansi::reset + " " + ansi::bold + P.text + name + ansi::reset + P.text + rest +
           ansi::reset;
}

void printToolCall(const std::string& title, ToolState state = ToolState::Ok) {
    out() << "\n" << toolLine(title, state) << "\n" << std::flush;
}

// Remplace la ligne d'appel affichée juste au-dessus (changement de couleur de la puce).
void redrawToolCall(const std::string& title, ToolState state) {
    out() << "\x1b[1A\r\x1b[2K" << toolLine(title, state) << "\n" << std::flush;
}

// Markdown : voir markdown.hpp.
std::vector<std::string> renderMarkdown(const std::string& text, size_t width) {
    return md::render(text, width);
}

// Réponse de l'assistant (sans ligne vide avant) : ⏺ en tête, Markdown rendu.
void printAnswerBody(const std::string& text) {
    auto lines = renderMarkdown(text, frameWidth() - 3);
    for (size_t i = 0; i < lines.size(); ++i)
        out() << (i == 0 ? std::string(P.text) + "⏺" + ansi::reset + " " : std::string("  ")) << lines[i] << "\n";
    out() << std::flush;
}

void printAnswer(const std::string& text) {
    out() << "\n";
    printAnswerBody(text);
}

// ---------------------------------------------------------------------------
// Suggestion du cadre de saisie (« Try "..." ») générée par l'IA en arrière-plan.
// L'appel réseau part dans un fil séparé : le cadre s'affiche tout de suite avec la suggestion
// précédente, puis se met à jour dès que la nouvelle arrive. Une nouvelle suggestion n'est
// demandée qu'après un échange avec l'agent (pas à chaque commande).
// ---------------------------------------------------------------------------
struct SuggestionState {
    std::mutex mu;
    std::string text = "lance les tests et corrige ce qui casse";
    size_t version = 0;              // incrémenté à chaque nouvelle suggestion
    bool busy = false;               // une demande est en cours
    size_t historyAtLast = (size_t)-1;  // taille de l'historique lors de la dernière demande
};

std::shared_ptr<SuggestionState> suggestionState() {
    static auto state = std::make_shared<SuggestionState>();
    return state;
}

// Nettoie la réponse du modèle : première ligne, sans « Try », sans guillemets.
std::string cleanSuggestion(const std::string& raw) {
    auto lines = splitLines(trim(raw));
    std::string s = lines.empty() ? "" : trim(lines[0]);
    for (const char* prefix : {"Try ", "Try:", "Essaie ", "Essaye "})
        if (startsWith(s, prefix)) s = trim(s.substr(std::string(prefix).size()));
    for (const char* q : {"\"", "'", "«", "»", "“", "”"}) {
        if (startsWith(s, q)) s = s.substr(std::string(q).size());
        if (s.size() >= std::string(q).size() && s.compare(s.size() - std::string(q).size(), std::string::npos, q) == 0)
            s = s.substr(0, s.size() - std::string(q).size());
    }
    s = trim(s);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

void refreshSuggestion() {
    auto st = suggestionState();
    const Provider* p = activeProvider();
    {
        std::lock_guard<std::mutex> lk(st->mu);
        if (st->busy || !p || st->historyAtLast == g.history.size()) return;
        st->busy = true;
        st->historyAtLast = g.history.size();
    }
    // Tout ce dont le fil a besoin est copié ici : il ne touche pas à l'état global.
    std::string lastRequest;
    for (auto it = g.history.rbegin(); it != g.history.rend(); ++it)
        if (it->role == "user") {
            lastRequest = truncate(it->text, 200);
            break;
        }
    // Quelques noms de fichiers du dossier pour que la suggestion soit pertinente.
    std::string files;
    {
        std::error_code ec;
        int n = 0;
        for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(toWide(g.cwd)), ec)) {
            if (++n > 25) break;
            files += (files.empty() ? "" : ", ") + e.path().filename().u8string();
        }
    }
    const std::string prompt =
        "Suggest ONE short, concrete task (under 70 characters) that the user could ask a coding agent working in "
        "the folder " + g.cwd + (files.empty() ? "" : " (it contains: " + files + ")") +
        (lastRequest.empty() ? "" : ". The user's last request was: " + lastRequest) +
        ". Base it on the folder content or the last request. Write it in the language that he is talking as a direct request (imperative). "
        "Reply with the task only: no quotes, no 'Try', no final period, no explanation.";
    const std::string key = keyFor(*p), model = modelFor(*p);

    std::thread([st, p, key, model, prompt] {
        std::string s = cleanSuggestion(askModel(*p, key, model, prompt));
        std::lock_guard<std::mutex> lk(st->mu);
        if (!s.empty() && displayLength(s) <= 120) {
            st->text = s;
            ++st->version;
        }
        st->busy = false;
        tui::redraw();
    }).detach();
}

// ---------------------------------------------------------------------------
// Liste de choix interactive (panneau de l'interface) : ↑/↓ pour se déplacer, taper pour filtrer,
// Entrée pour choisir, Échap pour annuler (renvoie "").
// ---------------------------------------------------------------------------
std::string pickFromList(const std::string& title, const std::vector<std::string>& items, const std::string& current) {
    if (items.empty()) return "";
    return tui::pick(title, items, current);
}

// Masque la clé dans « /key [fournisseur] <clé> » (écho à l'écran et historique des saisies).
std::string maskKeyCommand(const std::string& text) {
    auto words = splitWords(text);
    if (words.empty() || words[0] != "/key") return text;
    if (words.size() >= 3) words[2] = maskKey(words[2]);
    else if (words.size() == 2 && !findProvider(words[1])) words[1] = maskKey(words[1]);
    else return text;
    std::string out;
    for (const auto& w : words) out += (out.empty() ? "" : " ") + w;
    return out;
}

// Historique des messages saisis (↑/↓), conservé entre deux lancements dans history.txt.
std::string inputHistoryPath() { return g.configDir + "\\history.txt"; }

std::vector<std::wstring> loadInputHistory() {
    std::vector<std::wstring> hist;
    std::ifstream in(std::filesystem::path(toWide(inputHistoryPath())));
    std::string line;
    while (std::getline(in, line))
        if (!trim(line).empty()) hist.push_back(toWide(trim(line)));
    if (hist.size() > 500) hist.erase(hist.begin(), hist.end() - 500);
    return hist;
}

void rememberInput(const std::string& raw) {
    const std::string text = maskKeyCommand(raw);  // jamais de clé API dans history.txt
    tui::addHistory(toWide(text));
    std::ofstream out(std::filesystem::path(toWide(inputHistoryPath())), std::ios::app | std::ios::binary);
    out << text << "\n";
}

// Réaffiche le message envoyé dans le journal : « > ... » sur fond discret.
void echoUserMessage(const std::string& text) {
    const size_t w = frameWidth();
    auto lines = wrapWords(maskKeyCommand(text), w - 4);
    out() << "\n";
    for (size_t i = 0; i < lines.size(); ++i)
        out() << P.muted << (i == 0 ? "> " : "  ") << ansi::reset << P.text << lines[i] << ansi::reset << "\n";
    out() << std::flush;
}

// Saisie d'un message dans le cadre de l'interface (menu « / », historique, suggestion : voir tui.hpp).
void publishInfo() { tui::setInfo(modelStatus(), g.cwd, g.autoRun); }

std::string readPrompt() {
    publishInfo();
    refreshSuggestion();
    const std::string text = trim(tui::prompt());
    if (text.empty()) return text;
    if (text != "?") rememberInput(text);
    echoUserMessage(text);
    return text;
}

// Branche l'interface sur les commandes, la suggestion et l'état du programme.
tui::Hooks interfaceHooks() {
    tui::Hooks h;
    h.menu = [](const std::string& input) {
        std::vector<tui::MenuItem> m;
        if (input.empty() || input[0] != '/' || input.find(' ') != std::string::npos) return m;
        for (const auto& c : commandList())
            if (startsWith(c.name, input) && m.size() < 8) m.push_back({c.name, c.args, c.desc});
        return m;
    };
    h.argHint = [](const std::string& s) -> std::string {
        if (s.empty() || s[0] != '/') return "";
        const size_t space = s.find(' ');
        if (space == std::string::npos) return "";
        for (const auto& c : commandList())
            if (s.substr(0, space) == c.name && *c.args) return std::string(c.name) + " " + c.args + "  ·  " + c.desc;
        return "";
    };
    h.placeholder = [] {
        auto st = suggestionState();
        std::lock_guard<std::mutex> lk(st->mu);
        return "Try \"" + st->text + "\"";
    };
    return h;
}
