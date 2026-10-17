// ui.hpp : rendu de l'interface, dans le style de Claude Code.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "live.hpp"
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
// Logo en lettres-blocs (3 lignes) : « CODE » en couleur du texte, « PORTAL » dans le dégradé.
std::vector<std::string> logoRow(const std::string& word, int row) {
    static const std::map<char, std::vector<std::string>> glyphs = {
        {'C', {"█▀▀", "█  ", "▀▀▀"}}, {'O', {"█▀█", "█ █", "▀▀▀"}}, {'D', {"█▀▄", "█ █", "▀▀ "}},
        {'E', {"█▀▀", "█▀▀", "▀▀▀"}}, {'P', {"█▀█", "█▀▀", "▀  "}}, {'R', {"█▀█", "█▀▄", "▀ ▀"}},
        {'T', {"▀█▀", " █ ", " ▀ "}}, {'A', {"▄▀█", "█▀█", "▀ ▀"}}, {'L', {"█  ", "█  ", "▀▀▀"}}};
    std::vector<std::string> out;
    for (char c : word) out.push_back(glyphs.at(c)[(size_t)row]);
    return out;
}

void printBanner() {
    const bool animate = isConsole();
    const size_t w = frameWidth();
    std::cout << "\n";
    if (w >= 48) {
        for (int row = 0; row < 3; ++row) {
            std::string code, portal;
            for (const auto& g : logoRow("CODE", row)) code += g + " ";
            const auto letters = logoRow("PORTAL", row);
            for (size_t i = 0; i < letters.size(); ++i) portal += letters[i] + (i + 1 < letters.size() ? " " : "");
            std::cout << "  " << P.text << code << ansi::reset << " " << gradientText(portal) << "\n" << std::flush;
            if (animate) pause(60);
        }
    } else {
        std::cout << "  " << ansi::bold << P.text << "Code" << ansi::reset << gradientText("Portal", 0, 1, true) << "\n";
    }
    std::cout << "\n  " << P.muted << "Your coding agent, right in the terminal." << ansi::reset << "\n\n";
    const std::string cwd = shortenPath(g.cwd, w > 24 ? w - 14 : 20);
    std::cout << "  " << gradientColor(0) << "◉" << ansi::reset << " " << P.muted << "model   " << ansi::reset << P.text
              << modelStatus() << ansi::reset << "\n";
    std::cout << "  " << gradientColor(1) << "◉" << ansi::reset << " " << P.muted << "folder  " << ansi::reset << P.text << cwd
              << ansi::reset << "\n";
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
        {"/compact", "[focus]", "summarize the conversation to free up context", "Conversation"},
        {"/retry", "", "resend your last message", "Conversation"},
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
        {"/update", "[check | auto on|off]", "install the latest version from GitHub", "Settings"},
        {"/exit", "", "quit CodePortal", "General"},
    };
    return list;
}

void printTips() {
    auto k = [](const char* s) { return std::string(P.accent) + s + ansi::reset + P.muted; };
    std::cout << "\n  " << P.muted << k("/init") << " maps this project · " << k("/") << " commands · " << k("?")
              << " shortcuts · " << k("Esc") << " interrupts" << ansi::reset << "\n";
    std::cout << "  " << P.muted << k("/key") << " adds an API key · " << k("/theme") << " dark or light · " << k("/compact")
              << " shrinks a chat" << ansi::reset << "\n";
}

void printShortcuts() {
    auto item = [](const char* key, const char* desc) {
        std::cout << "  " << ansi::bold << P.text << key << ansi::reset
                  << std::string(std::max<int>(2, 20 - (int)displayLength(key)), ' ') << P.muted << desc
                  << ansi::reset << "\n";
    };
    std::cout << "\n";
    item("/", "command menu · Tab or Enter to pick, Esc to close");
    item("↑ ↓", "previous / next message (or move in the menu)");
    item("← → Home End", "move the cursor · Ctrl+← → by word");
    item("Ctrl+W", "delete the previous word");
    item("Ctrl+U", "clear the line");
    item("Esc", "clear the line · interrupt the agent while it works");
    item("Ctrl+O", "show the full output of the last shortened result, without stopping the agent");
    item("Ctrl+C", "clear the line, or quit when it is empty");
    item("!<command>", "run a shell command yourself");
    item("!edit_file('f')", "file tools: create_file, write_file, delete_file, create_dir");
    item("/help", "all commands");
}

void printHelp() {
    static const char* sections[] = {"Conversation", "Sessions", "Project", "Providers", "Settings", "General"};
    std::cout << "\n";
    for (const char* section : sections) {
        std::cout << " " << ansi::bold << P.text << section << ansi::reset << "\n";
        for (const auto& c : commandList()) {
            if (std::string(c.section) != section) continue;
            const std::string usage = std::string(c.name) + (*c.args ? std::string(" ") + c.args : "");
            std::cout << "   " << P.accent << c.name << ansi::reset << P.muted << (*c.args ? std::string(" ") + c.args : "")
                      << ansi::reset << std::string(std::max<int>(2, 26 - (int)displayLength(usage)), ' ') << P.muted
                      << c.desc << ansi::reset << "\n";
        }
        std::cout << "\n";
    }
    std::cout << " " << P.muted << "Any other message goes to the agent: it reasons live, uses its tools, checks the results\n"
              << " and tells you when it is done. Shell: !<command>. File tools: /tools. Keys: ?" << ansi::reset << "\n";
}

void printStatus() {
    const Provider* act = activeProvider();
    auto line = [](const char* k, const std::string& v) {
        std::cout << "  " << P.muted << k << ansi::reset << std::string(std::max<int>(2, 18 - (int)displayLength(k)), ' ')
                  << v << "\n";
    };
    std::cout << "\n";
    line("Provider", act ? act->label : std::string("none"));
    line("Model", act ? modelFor(*act) : std::string("-"));
    line("Theme", g.theme);
    line("Auto-run", g.autoRun ? "on" : "off");
    line("Working dir", g.cwd);
    line("Config file", g.configPath);
}

void printProviders() {
    const Provider* act = activeProvider();
    std::cout << "\n";
    for (const auto& p : providers()) {
        std::string source;
        std::string key = keyFor(p, &source);
        std::string status = key.empty() ? std::string(P.muted) + "no key" + ansi::reset
                                         : std::string(P.ok) + "✔ " + maskKey(key) + ansi::reset + " " + P.muted +
                                               "(" + source + ")" + ansi::reset;
        bool active = act == &p;
        std::cout << "  " << (active ? P.accent : "") << (active ? "● " : "  ") << ansi::reset << ansi::bold << p.id
                  << ansi::reset << std::string(std::max<int>(2, 13 - (int)p.id.size()), ' ') << status << "\n";
    }
}

void printOk(const std::string& msg) {
    std::cout << P.ok << "✔ " << ansi::reset << msg << "\n";
}

void printError(const std::string& msg) {
    std::cout << "\r\x1b[2K";
    auto lines = wrapWords(msg, frameWidth() - 4);
    for (size_t i = 0; i < lines.size(); ++i)
        std::cout << (i == 0 ? std::string(P.err) + "✖ " + ansi::reset : std::string("  ")) << P.err << lines[i]
                  << ansi::reset << "\n";
}

// ---------------------------------------------------------------------------
// Appels d'outils : « ▸ Read main.cpp » — jaune pendant l'exécution, vert si OK, rouge si erreur.
// ---------------------------------------------------------------------------
enum class ToolState { Pending, Ok, Error };

std::string toolLine(const std::string& title, ToolState state) {
    std::string label = title;
    const size_t max = frameWidth() - 3;
    if (displayLength(label) > max) label = utf8Prefix(label, max - 1) + "…";
    const size_t paren = label.find('(');
    const std::string name = paren == std::string::npos ? label : label.substr(0, paren);
    std::string arg = paren == std::string::npos ? "" : label.substr(paren + 1);
    if (!arg.empty() && arg.back() == ')') arg.pop_back();
    const char* color = state == ToolState::Pending ? P.warn : state == ToolState::Ok ? P.ok : P.err;
    return std::string(color) + (state == ToolState::Pending ? "▹" : "▸") + ansi::reset + " " + ansi::bold + P.text + name +
           ansi::reset + (arg.empty() ? "" : " ") + P.muted + arg + ansi::reset;
}

void printToolCall(const std::string& title, ToolState state = ToolState::Ok) {
    std::cout << "\n" << toolLine(title, state) << "\n" << std::flush;
}

// Remplace la ligne d'appel affichée juste au-dessus (changement de couleur de la puce).
void redrawToolCall(const std::string& title, ToolState state) {
    std::cout << "\x1b[1A\r\x1b[2K" << toolLine(title, state) << "\n" << std::flush;
}

// Markdown : voir markdown.hpp.
std::vector<std::string> renderMarkdown(const std::string& text, size_t width) {
    return md::render(text, width);
}

// Réponse de l'assistant (sans ligne vide avant) : ◆ en tête, Markdown rendu.
void printAnswerBody(const std::string& text) {
    auto lines = renderMarkdown(text, frameWidth() - 3);
    for (size_t i = 0; i < lines.size(); ++i)
        std::cout << (i == 0 ? gradientColor(0) + "◆" + ansi::reset + " " : std::string("  ")) << lines[i] << "\n";
    std::cout << std::flush;
}

void printAnswer(const std::string& text) {
    std::cout << "\n";
    printAnswerBody(text);
}

// ---------------------------------------------------------------------------
// Zone de saisie
// ---------------------------------------------------------------------------
std::string promptFooter(size_t w) {
    std::string left = "? for shortcuts · / for commands";
    std::string right = modelStatus() + (g.autoRun ? " · auto-run" : "");
    if (displayLength(left) + displayLength(right) + 4 > w) right.clear();
    size_t gap = w - displayLength(left) - displayLength(right) - 2;
    return std::string("  ") + P.muted + left + std::string(gap, ' ') + right + ansi::reset;
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
    }).detach();
}

// ---------------------------------------------------------------------------
// Liste de choix interactive : ↑/↓ (et Page préc./suiv.) pour se déplacer, taper pour filtrer,
// Entrée pour choisir, Échap pour annuler (renvoie "").
// ---------------------------------------------------------------------------
std::string pickFromList(const std::string& title, const std::vector<std::string>& items, const std::string& current) {
    if (items.empty()) return "";
    const size_t rows = std::min<size_t>(10, items.size());
    std::string filter;
    size_t sel = 0, top = 0, drawn = 0;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i] == current) sel = i;

    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    auto filtered = [&] {
        std::vector<size_t> idx;
        const std::string f = lower(filter);
        for (size_t i = 0; i < items.size(); ++i)
            if (f.empty() || lower(items[i]).find(f) != std::string::npos) idx.push_back(i);
        return idx;
    };
    auto draw = [&] {
        const auto idx = filtered();
        if (sel >= idx.size()) sel = idx.empty() ? 0 : idx.size() - 1;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
        std::string o = drawn ? "\x1b[" + std::to_string(drawn) + "A" : "";
        o += "\r\x1b[J  " + std::string(ansi::bold) + P.text + title + ansi::reset + "  " + P.muted +
             "↑↓ move · type to filter · Enter choose · Esc skip" + ansi::reset + "\n";
        o += "  " + std::string(P.accent) + "› " + ansi::reset + P.text + filter + ansi::reset +
             (filter.empty() ? std::string(P.muted) + "filter…" + ansi::reset : std::string()) + "\n";
        size_t lines = 2;
        for (size_t k = top; k < idx.size() && k < top + rows; ++k) {
            const std::string& name = items[idx[k]];
            const bool chosen = k == sel;
            o += "  " + (chosen ? std::string(P.accent) + "❯ " + ansi::bold : std::string("  ") + P.text) + name +
                 ansi::reset + (name == current ? std::string(P.muted) + "  (current)" + ansi::reset : std::string()) + "\n";
            ++lines;
        }
        if (idx.empty()) {
            o += "  " + std::string(P.muted) + "  no match" + ansi::reset + "\n";
            ++lines;
        } else if (idx.size() > rows) {
            o += "  " + std::string(P.muted) + "  " + std::to_string(sel + 1) + "/" + std::to_string(idx.size()) +
                 ansi::reset + "\n";
            ++lines;
        }
        drawn = lines;
        std::cout << o << std::flush;
    };

    std::string result;
    draw();
    for (;;) {
        const wint_t ch = _getwch();
        const auto idx = filtered();
        if (ch == L'\r' || ch == L'\n') {
            if (!idx.empty()) {
                result = items[idx[sel]];
                break;
            }
        } else if (ch == 27 || ch == 3) {
            break;
        } else if (ch == 8) {
            if (!filter.empty()) filter.pop_back();
            sel = top = 0;
        } else if (ch == 0 || ch == 0xE0) {
            switch (_getwch()) {
                case 72: if (sel > 0) --sel; break;                                     // ↑
                case 80: if (sel + 1 < idx.size()) ++sel; break;                        // ↓
                case 73: sel = sel > rows ? sel - rows : 0; break;                       // Page préc.
                case 81: sel = std::min(sel + rows, idx.empty() ? 0 : idx.size() - 1); break;  // Page suiv.
                default: break;
            }
        } else if (ch >= 32 && ch < 127) {
            filter += (char)ch;
            sel = top = 0;
        }
        draw();
    }
    std::cout << "\x1b[" << drawn << "A\r\x1b[J" << std::flush;  // efface la liste
    return result;
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

std::vector<std::wstring>& inputHistory() {
    static std::vector<std::wstring> hist;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream in(std::filesystem::path(toWide(inputHistoryPath())));
        std::string line;
        while (std::getline(in, line))
            if (!trim(line).empty()) hist.push_back(toWide(trim(line)));
        if (hist.size() > 500) hist.erase(hist.begin(), hist.end() - 500);
    }
    return hist;
}

void rememberInput(const std::string& raw) {
    const std::string text = maskKeyCommand(raw);  // jamais de clé API dans history.txt
    auto& hist = inputHistory();
    const std::wstring w = toWide(text);
    if (!hist.empty() && hist.back() == w) return;
    hist.push_back(w);
    std::ofstream out(std::filesystem::path(toWide(inputHistoryPath())), std::ios::app | std::ios::binary);
    out << text << "\n";
}

// Cadre de saisie.
//   - curseur déplaçable (← → Début Fin, Ctrl+← →), Suppr, Ctrl+W, Ctrl+U
//   - ↑/↓ : historique des messages
//   - « / » : menu des commandes sous le cadre (↑/↓ pour choisir, Tab ou Entrée pour compléter, Échap pour fermer)
//   - sous le cadre : aide sur les arguments de la commande en cours
// Après Entrée, le cadre disparaît et le message est réaffiché « > ... ».
std::string readPrompt() {
    const size_t w = frameWidth();
    const size_t inner = w - 2;
    const size_t maxContent = w - 5;  // "│ > " (4 colonnes) + "│" (1)
    const size_t maxMenu = 8;

    refreshSuggestion();
    auto st = suggestionState();
    size_t shownVersion = 0;
    std::string placeholder;
    auto loadPlaceholder = [&] {
        std::lock_guard<std::mutex> lk(st->mu);
        placeholder = "Try \"" + st->text + "\"";
        shownVersion = st->version;
    };
    loadPlaceholder();

    auto& hist = inputHistory();
    size_t histPos = hist.size();
    std::wstring draft, buf;
    size_t cur = 0, view = 0, sel = 0;
    bool menuClosed = false;

    // Commandes qui correspondent à ce qui est tapé (menu « / »).
    auto matches = [&] {
        std::vector<const CommandInfo*> m;
        if (menuClosed || buf.empty() || buf[0] != L'/' || buf.find(L' ') != std::wstring::npos) return m;
        const std::string prefix = toUtf8(buf);
        for (const auto& c : commandList())
            if (startsWith(c.name, prefix) && m.size() < maxMenu) m.push_back(&c);
        return m;
    };
    // Aide sur les arguments une fois la commande tapée : « /cd <path> · change the working directory »
    auto argHint = [&]() -> std::string {
        if (buf.empty() || buf[0] != L'/') return "";
        const std::string s = toUtf8(buf);
        const size_t space = s.find(' ');
        if (space == std::string::npos) return "";
        for (const auto& c : commandList())
            if (s.substr(0, space) == c.name && *c.args) return std::string(c.name) + " " + c.args + "  ·  " + c.desc;
        return "";
    };

    std::cout << "\n" << P.border << "╭" << repeatStr("─", inner) << "╮" << ansi::reset << "\n"
              << "\n"
              << P.border << "╰" << repeatStr("─", inner) << "╯" << ansi::reset << "\n"
              << promptFooter(w) << "\x1b[2A";  // curseur remonté sur la ligne de saisie

    auto draw = [&] {
        const auto m = matches();
        if (sel >= m.size()) sel = 0;
        if (cur < view) view = cur;
        if (cur > view + maxContent) view = cur - maxContent;

        std::string content;
        size_t len = 0;
        if (buf.empty()) {
            const std::string ph = utf8Prefix(placeholder, maxContent);
            content = std::string(P.muted) + ph + ansi::reset;
            len = displayLength(ph);
        } else {
            const std::wstring visible = buf.substr(view, maxContent);
            content = std::string(P.text) + toUtf8(visible) + ansi::reset;
            len = visible.size();
        }
        const bool command = !buf.empty() && buf[0] == L'/';
        std::string o = "\r\x1b[2K" + std::string(P.border) + "│" + ansi::reset + " " + (command ? std::string(P.accent) : gradientColor(0)) +
                        "❯" + ansi::reset + " " + content + std::string(maxContent > len ? maxContent - len : 0, ' ') +
                        P.border + "│" + ansi::reset;
        // Bas du cadre puis pied de page (ou aide sur les arguments).
        o += "\x1b[1B\r\x1b[2K" + std::string(P.border) + "╰" + repeatStr("─", inner) + "╯" + ansi::reset;
        const std::string hint = argHint();
        o += "\x1b[1B\r\x1b[2K" +
             (hint.empty() ? promptFooter(w) : std::string("  ") + P.muted + utf8Prefix(hint, w - 4) + ansi::reset);
        // Menu des commandes.
        for (size_t i = 0; i < m.size(); ++i) {
            const bool chosen = i == sel;
            const std::string name = std::string(m[i]->name) + (*m[i]->args ? std::string(" ") + m[i]->args : "");
            o += "\n\x1b[2K  " + (chosen ? std::string(P.accent) + "❯ " + ansi::bold : std::string("  ") + P.text) +
                 name + ansi::reset + std::string(std::max<int>(2, 26 - (int)displayLength(name)), ' ') +
                 (chosen ? P.text : P.muted) + utf8Prefix(m[i]->desc, w > 34 ? w - 34 : 10) + ansi::reset;
        }
        o += "\x1b[J\x1b[" + std::to_string(2 + m.size()) + "A";  // efface le reste, remonte sur la saisie
        o += "\r\x1b[" + std::to_string(5 + (buf.empty() ? 0 : cur - view)) + "G";
        std::cout << o << std::flush;
    };

    auto setBuffer = [&](const std::wstring& s) {
        buf = s;
        cur = buf.size();
    };

    draw();
    for (;;) {
        // En attendant une touche : si une nouvelle suggestion arrive et que rien n'est tapé, on l'affiche.
        while (!_kbhit()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            bool changed;
            {
                std::lock_guard<std::mutex> lk(st->mu);
                changed = st->version != shownVersion;
            }
            if (changed) {
                loadPlaceholder();
                if (buf.empty()) draw();
            }
        }
        const wint_t ch = _getwch();
        const auto m = matches();

        if (ch == L'\r' || ch == L'\n') {
            if (!m.empty() && toUtf8(buf) != m[sel]->name) {  // Entrée dans le menu : on complète
                setBuffer(toWide(m[sel]->name));
                if (m[sel]->args[0] == '<') {  // argument obligatoire : on laisse la main pour le taper
                    setBuffer(buf + L" ");
                    draw();
                    continue;
                }
            }
            break;
        }
        if (ch == 3) {  // Ctrl+C : vide la ligne, ou quitte si elle est déjà vide
            if (!buf.empty()) {
                setBuffer(L"");
                menuClosed = false;
                draw();
                continue;
            }
            std::cout << "\x1b[2B\n\x1b[J" << std::flush;
            std::exit(0);
        }
        if (ch == 9) {  // Tab : complète la commande choisie
            if (!m.empty()) setBuffer(toWide(std::string(m[sel]->name) + (*m[sel]->args ? " " : "")));
        } else if (ch == 27) {  // Échap : ferme le menu, sinon vide la ligne
            if (!m.empty()) menuClosed = true;
            else setBuffer(L"");
        } else if (ch == 8) {  // Retour arrière
            if (cur > 0) buf.erase(--cur, 1);
            menuClosed = false;
        } else if (ch == 21) {  // Ctrl+U
            setBuffer(L"");
        } else if (ch == 23) {  // Ctrl+W : supprime le mot précédent
            size_t s = cur;
            while (s > 0 && buf[s - 1] == L' ') --s;
            while (s > 0 && buf[s - 1] != L' ') --s;
            buf.erase(s, cur - s);
            cur = s;
        } else if (ch == 1) {  // Ctrl+A
            cur = 0;
        } else if (ch == 5) {  // Ctrl+E
            cur = buf.size();
        } else if (ch == 0 || ch == 0xE0) {  // touches spéciales
            switch (_getwch()) {
                case 75: if (cur > 0) --cur; break;           // ←
                case 77: if (cur < buf.size()) ++cur; break;  // →
                case 71: cur = 0; break;                      // Début
                case 79: cur = buf.size(); break;             // Fin
                case 83: if (cur < buf.size()) buf.erase(cur, 1); break;  // Suppr
                case 115:                                     // Ctrl+←
                    while (cur > 0 && buf[cur - 1] == L' ') --cur;
                    while (cur > 0 && buf[cur - 1] != L' ') --cur;
                    break;
                case 116:                                     // Ctrl+→
                    while (cur < buf.size() && buf[cur] != L' ') ++cur;
                    while (cur < buf.size() && buf[cur] == L' ') ++cur;
                    break;
                case 72:  // ↑ : menu, sinon message précédent
                    if (!m.empty()) sel = (sel + m.size() - 1) % m.size();
                    else if (histPos > 0) {
                        if (histPos == hist.size()) draft = buf;
                        setBuffer(hist[--histPos]);
                        menuClosed = true;
                    }
                    break;
                case 80:  // ↓ : menu, sinon message suivant
                    if (!m.empty()) sel = (sel + 1) % m.size();
                    else if (histPos < hist.size()) {
                        ++histPos;
                        setBuffer(histPos == hist.size() ? draft : hist[histPos]);
                        menuClosed = true;
                    }
                    break;
                default: break;
            }
        } else if (ch >= 32) {
            buf.insert(buf.begin() + cur, (wchar_t)ch);
            ++cur;
            menuClosed = false;
            sel = 0;
        }
        draw();
    }

    const std::string text = trim(toUtf8(buf));
    if (text.empty()) {
        std::cout << "\x1b[2A\r\x1b[J" << std::flush;  // efface le cadre et la ligne vide au-dessus
        return text;
    }
    rememberInput(text);
    std::cout << "\x1b[1A\r\x1b[J";  // efface le cadre et le menu, puis réaffiche le message
    auto lines = wrapWords(maskKeyCommand(text), w - 3);
    for (size_t i = 0; i < lines.size(); ++i)
        std::cout << P.muted << (i == 0 ? "❯ " : "  ") << lines[i] << ansi::reset << "\n";
    std::cout << std::flush;
    return text;
}
