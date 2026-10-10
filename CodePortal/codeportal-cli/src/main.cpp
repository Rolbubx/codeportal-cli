// CodePortal CLI : interface plein écran inspirée de Claude Code.
// Windows uniquement (console + WinHTTP), C++17. Interface : FTXUI (third_party/ftxui, compilé par ftxui_impl.cpp).
//
// Structure :
//   util.hpp       texte, console, thèmes, sortie out()
//   tui.hpp        interface plein écran (journal, zone en direct, panneaux, saisie)
//   json.hpp       parseur JSON minimal
//   http.hpp       requêtes HTTPS (WinHTTP)
//   state.hpp      état + config.ini
//   providers.hpp  table des fournisseurs, clés, requêtes
//   live.hpp       statut animé et texte en direct pendant le streaming
//   ui.hpp         rendu (bannière, réponses, appels d'outils), saisie
//   safety.hpp     garde-fous des outils de l'agent
//   tools.hpp      outils !nom(args) : context, read_file, edit_file, create_file, delete_file...
//   commands.hpp   commandes /xxx, shell, exécution commune
//   agent.hpp      prompt système, protocole, boucle d'analyse autonome

#include "agent.hpp"

namespace {

void setupConsole() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleTitleW(L"CodePortal");
}

// Premier lancement sans aucune clé : on propose d'en coller une, dans la console, avant l'interface.
std::string onboarding() {
    std::cout << "No API key found. Paste a key from Gemini, OpenAI, Anthropic, Poolside, Mistral, Groq, "
                 "xAI, DeepSeek, OpenRouter or Cerebras.\n"
              << "The provider is detected automatically. Press Enter to skip.\n";
    return readMasked("API key: ");
}

// Le programme lui-même, sur le fil de travail : il écrit avec out() et lit avec readPrompt().
void session(const std::string& firstKey) {
    out() << "\x1b[2J\x1b[H";
    printBanner();
    if (!firstKey.empty()) addKeyAuto(firstKey);
    printTips();

    for (;;) {
        // Messages tapés pendant le dernier tour et arrivés trop tard pour l'agent : ils deviennent le tour suivant.
        std::string line;
        const auto notes = tui::takeNotes();
        for (const auto& n : notes) line += (line.empty() ? "" : "\n") + n;
        if (!line.empty()) echoUserMessage(line);
        else line = readPrompt();
        if (line.empty()) continue;
        if (line == "?") {
            printShortcuts();
            continue;
        }
        if (line[0] == '/') {
            if (handleAgentCommand(line)) continue;  // /retry, /init : relancent l'agent
            if (!handleCommand(line)) break;
            publishInfo();
            continue;
        }
        if (line[0] == '!') {
            Call call;
            call.command = line;
            executeCall(call, false);
            continue;
        }
        chat(line);
    }
}

}  // namespace

int main() {
    setupConsole();

    const char* appdata = std::getenv("APPDATA");
    g.configDir = std::string(appdata ? appdata : ".") + "\\CodePortal";
    g.configPath = g.configDir + "\\config.ini";
    g.cwd = currentDir();
    loadConfig();
    applyTheme(g.theme);

    std::string firstKey;
    if (!activeProvider()) firstKey = onboarding();

    tui::setHistory(loadInputHistory());
    publishInfo();
    tui::run(interfaceHooks(), [&] { session(firstKey); });

    std::cout << "Goodbye.\n" << std::flush;
    // Le fil de travail peut encore attendre une saisie : on termine sans l'attendre.
    ExitProcess(0);
}
