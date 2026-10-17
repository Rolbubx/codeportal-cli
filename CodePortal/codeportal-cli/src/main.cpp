// CodePortal CLI : interface console inspirée de Claude Code.
// Windows uniquement (console + WinHTTP), C++17, aucune dépendance externe.
//
// Structure :
//   util.hpp       texte, console, thèmes, spinner
//   json.hpp       parseur JSON minimal
//   http.hpp       requêtes HTTPS (WinHTTP)
//   state.hpp      état + config.ini
//   providers.hpp  table des fournisseurs, clés, requêtes
//   ui.hpp         rendu (bannière, saisie, réponses, réflexion, appels d'outils)
//   tools.hpp      outils !nom(args) : context, read_file, edit_file, create_file, delete_file...
//   commands.hpp   commandes /xxx, shell, exécution commune
//   agent.hpp      prompt système, protocole, boucle d'analyse autonome

#include "api.hpp"

namespace {

void setupConsole() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

// Premier lancement sans aucune clé : on propose d'en coller une.
void onboarding() {
    std::cout << "No API key found. Paste a key from Gemini, OpenAI, Anthropic, Poolside, Mistral, Groq, "
                 "xAI, DeepSeek, OpenRouter or Cerebras.\n"
              << "The provider is detected automatically. Press Enter to skip.\n";
    std::string key = readMasked("API key: ");
    if (!key.empty()) addKeyAuto(key);
}

void printUsage() {
    std::cout << "CodePortal " << kVersion << " - coding agent for the terminal\n\n"
              << "Usage:\n"
              << "  codeportal                       start the interactive interface\n"
              << "  codeportal --headless [--port N] [--bind ADDR]\n"
              << "                                   run the local HTTP API and web chat (default 127.0.0.1:8765)\n"
              << "  codeportal --update [--yes]      install the latest version from GitHub\n"
              << "  codeportal --check               only check whether a newer version exists\n"
              << "  codeportal --version             print the version\n\n"
              << "Options:\n"
              << "  --no-update-check                skip the daily background update check\n"
              << "  --add-to-path / --remove-from-path\n"
              << "                                   add or remove this folder in your PATH (used by the installer)\n";
}

}  // namespace

//   codeportal.exe --headless [--port 8765] [--bind 127.0.0.1]   : pas d'interface, API HTTP (voir api.hpp)
int main(int argc, char** argv) {
    setupConsole();

    bool headless = false, update = false, checkOnly = false, assumeYes = false, autoCheck = true;
    unsigned short port = 8765;
    std::string bind = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless") headless = true;
        else if (arg == "--port" && i + 1 < argc) port = (unsigned short)std::atoi(argv[++i]);
        else if (arg == "--bind" && i + 1 < argc) bind = argv[++i];
        else if (arg == "--update") update = true;
        else if (arg == "--check") checkOnly = true;
        else if (arg == "--yes" || arg == "-y") assumeYes = true;
        else if (arg == "--no-update-check") autoCheck = false;
        else if (arg == "--add-to-path" || arg == "--remove-from-path")  // utilisés par l'installeur
            return upd::editUserPath(arg == "--add-to-path") ? 0 : 1;
        else if (arg == "--version" || arg == "-v") {
            std::cout << "CodePortal " << kVersion << "\n";
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << " (see codeportal --help)\n";
            return 2;
        }
    }

    const char* appdata = std::getenv("APPDATA");
    g.configDir = std::string(appdata ? appdata : ".") + "\\CodePortal";
    g.configPath = g.configDir + "\\config.ini";
    g.cwd = currentDir();
    loadConfig();
    applyTheme(g.theme);
    upd::cleanupOldExe();
    if (update || checkOnly) return upd::runUpdate(checkOnly, assumeYes);
    if (autoCheck) upd::startBackgroundCheck();
    if (headless) {
        g_headless = true;
        printUpdateNotice();
        return api::run(bind, port);
    }
    LiveView::get().setExpandHandler(expandLastOutcome);

    std::cout << "\x1b[2J\x1b[H";
    if (!activeProvider()) onboarding();
    std::cout << "\x1b[2J\x1b[H";
    printBanner();
    printUpdateNotice();
    printTips();

    for (;;) {
        std::string line = readPrompt();
        if (line.empty()) continue;
        if (line == "?") {
            printShortcuts();
            continue;
        }
        if (line[0] == '/') {
            if (handleAgentCommand(line)) continue;  // /retry, /init : relancent l'agent
            if (!handleCommand(line)) break;
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
    std::cout << "Goodbye.\n";
    return 0;
}
