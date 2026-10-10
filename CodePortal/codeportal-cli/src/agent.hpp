// agent.hpp : prompt système et boucle de l'agent avec outils natifs (function calling).
//
// Un tour : le modèle raisonne et répond en streaming (affiché en direct), puis appelle des outils.
// Le programme exécute chaque outil, renvoie les résultats au modèle, et recommence jusqu'à ce que
// le modèle réponde sans appeler d'outil.
#pragma once

#include <shlobj.h>

#include <memory>
#include <string>
#include <vector>

#include "commands.hpp"
#include "live.hpp"
#include "providers.hpp"
#include "state.hpp"
#include "tools.hpp"
#include "ui.hpp"
#include "util.hpp"

std::string knownFolder(int csidl) {
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf))) return toUtf8(buf);
    return "";
}

// PORTAL.md du dossier de travail (créé par /init) : instructions du projet envoyées à chaque requête.
std::string projectInstructions() {
    std::string text;
    if (!readText(std::filesystem::path(toWide(g.cwd + "\\PORTAL.md")), text) || trim(text).empty()) return "";
    return "\n\n# Project instructions (PORTAL.md)\n" + truncate(text, 8000);
}

// Statistiques d'un tour, affichées à la fin comme dans Claude Code : « ✻ Worked for 12s · 3 tool uses ».
struct TurnStats {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    size_t tools = 0, tokens = 0;

    ~TurnStats() {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        g.usage.agentSeconds += secs;
        if (secs < 3 && tools == 0) return;
        static const char* verbs[] = {"Worked", "Cooked", "Brewed", "Crafted", "Churned", "Baked", "Pondered"};
        std::string line = std::string(verbs[(size_t)secs % 7]) + " for " + formatDuration(secs);
        if (tools) line += " · " + plural(tools, "tool use");
        if (tokens) line += " · ↓ " + formatTokens(tokens) + " tokens";
        std::cout << "\n" << P.accent << "✻ " << ansi::reset << P.muted << line << ansi::reset << "\n" << std::flush;
    }
};

std::string systemPrompt() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char date[32];
    snprintf(date, sizeof date, "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    const char* user = std::getenv("USERNAME");

    return "You are CodePortal, an autonomous coding agent running in the user's Windows terminal. "
           "Reply in the same language as the user. Be concise.\n\n"
           "# Environment\n"
           "- Operating system: Windows (commands run in cmd.exe)\n"
           "- Working directory: " + g.cwd + "\n"
           "- User: " + std::string(user ? user : "unknown") + "\n"
           "- Home folder: " + knownFolder(CSIDL_PROFILE) + "\n"
           "- Desktop folder: " + knownFolder(CSIDL_DESKTOPDIRECTORY) + "\n"
           "- Documents folder: " + knownFolder(CSIDL_PERSONAL) + "\n"
           "- Date: " + date + "\n\n"
           "# How to work\n"
           "- Use your tools to act directly. Never tell the user to run something you can run yourself.\n"
           "- Before calling tools, write one short sentence saying what you are about to do; the user sees it.\n"
           "- For files and folders use the dedicated tools (create_file, write_file, edit_file, delete_file, "
           "create_dir, read_file, list_dir). Use run_command only for what they cannot do (build, git, tests...).\n"
           "- The folder paths above are exact: use them directly, do not look them up.\n"
           "- run_command has no keyboard input and a time limit: never start interactive programs (cmd, "
           "powershell without -Command, python without a script, editors...).\n"
           "- run_command uses cmd.exe: Unix tools (head, tail, grep, ls, cat, wc, sed) do NOT exist. Use "
           "findstr, dir, type, more, or powershell -NoProfile -Command \"...\" (e.g. Select-Object -First 20).\n"
           "- Read a file before editing it; edit_file's old_text must match the file exactly, once.\n"
           "- Check every tool result yourself and fix errors. Never ask the user for permission to use a tool.\n"
           "- Ask the user only when the request is genuinely ambiguous.\n"
           "- When finished, give a short summary of what you did. Use Markdown (lists, tables, code blocks) "
           "when it helps readability." + projectInstructions();
}

// Garde l'historique raisonnable sans séparer un appel d'outil de son résultat :
// on coupe toujours juste avant un message de l'utilisateur.
void trimHistory() {
    if (g.history.size() <= 80) return;
    size_t cut = g.history.size() - 80;
    while (cut < g.history.size() && g.history[cut].role != "user") ++cut;
    g.history.erase(g.history.begin(), g.history.begin() + cut);
}

// Rendu Markdown pendant le streaming.
// Chaque ligne terminée (reçue jusqu'à son saut de ligne) est mise en forme et affichée pour de bon ;
// la ligne en cours d'écriture est affichée mise en forme elle aussi, puis redessinée à chaque morceau.
// Les blocs de code sont colorés ligne par ligne ; un tableau s'affiche dès qu'il est complet.
class MarkdownStream {
public:
    MarkdownStream(LiveView& lv, size_t width) : lv_(lv), width_(width) {}

    void feed(const std::string& chunk) {
        pending_ += chunk;
        size_t nl;
        while ((nl = pending_.find('\n')) != std::string::npos) {
            std::string line = pending_.substr(0, nl);
            pending_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            completeLine(line);
        }
        showPartial();
    }

    void finish() {
        if (!trim(pending_).empty()) completeLine(pending_);
        pending_.clear();
        flushTable();
        if (inCode_) {
            commit({md::codeFooter(width_)});
            inCode_ = false;
        }
        lv_.endBlock();
    }

private:
    LiveView& lv_;
    size_t width_;
    std::string pending_;              // ligne en cours (sans saut de ligne)
    bool inCode_ = false;
    std::string fence_, lang_;
    std::vector<std::string> table_;   // lignes de tableau en attente
    bool lastBlank_ = true;            // évite les lignes vides en double

    void commit(const std::vector<std::string>& lines) {
        if (lines.empty()) return;
        lv_.commitLines(lines);
        lastBlank_ = lines.back().empty();
    }

    void commitText(const std::string& line) {
        if (trim(line).empty()) {
            if (!lastBlank_) commit({""});
            return;
        }
        commit(md::render(line, width_));
    }

    void flushTable() {
        if (table_.empty()) return;
        std::string text;
        for (const auto& row : table_) text += row + "\n";
        table_.clear();
        commit(md::render(text, width_));
    }

    void completeLine(const std::string& line) {
        const std::string t = trim(line);
        if (inCode_) {
            if (startsWith(t, fence_)) {
                commit({md::codeFooter(width_)});
                inCode_ = false;
            } else {
                commit(md::codeLines(line, lang_, width_));
            }
            return;
        }
        if (md::isFence(t)) {
            flushTable();
            inCode_ = true;
            fence_ = t.substr(0, 3);
            lang_ = trim(t.substr(3));
            commit({md::codeHeader(lang_, width_)});
            return;
        }
        // Tableau : une ligne avec « | » suivie d'une ligne de séparation.
        if (!table_.empty()) {
            if (table_.size() == 1 && !md::isTableSeparator(line)) {  // finalement pas un tableau
                const std::string first = table_[0];
                table_.clear();
                commitText(first);
            } else if (t.find('|') != std::string::npos) {
                table_.push_back(line);
                return;
            } else {
                flushTable();
            }
        }
        if (t.find('|') != std::string::npos && table_.empty()) {
            table_.push_back(line);
            return;
        }
        commitText(line);
    }

    void showPartial() {
        std::vector<std::string> lines;
        for (const auto& row : table_) lines.push_back(std::string(P.muted) + row + ansi::reset);
        if (!pending_.empty()) {
            if (inCode_) lines = md::codeLines(pending_, lang_, width_);
            else if (!trim(pending_).empty())
                for (const auto& l : md::render(pending_, width_)) lines.push_back(l);
        }
        lv_.setPartial(lines);
    }
};

// Un tour est inachevé quand l'historique se termine par la question ou par des résultats d'outils :
// une erreur (réseau, quota, surcharge...) a arrêté l'agent avant sa réponse finale.
bool turnUnfinished() {
    return !g.history.empty() && (g.history.back().role == "user" || g.history.back().role == "tool");
}

// Fait travailler l'agent sur l'historique actuel jusqu'à sa réponse finale.
// En cas d'erreur, le travail déjà fait (réponses, appels d'outils et résultats) est conservé :
// /retry reprend là où l'agent s'est arrêté.
void runAgent() {
    const Provider* p = activeProvider();
    if (!p) {
        printError("No API key configured. Run /key first.");
        return;
    }
    const std::string key = keyFor(*p);
    const std::string model = modelFor(*p);

    TurnStats stats;

    for (int step = 0; step < kMaxAgentSteps; ++step) {
        LiveView& lv = LiveView::get();
        lv.reset();
        bool thinkingOpen = false, answerOpen = false, gotUsage = false;
        size_t reportedTokens = 0;
        size_t estimatedChars = 0;
        std::unique_ptr<MarkdownStream> answer;  // réponse en cours, rendue en Markdown au fil du flux
        auto closeAnswer = [&] {
            if (answer) answer->finish();
            answer.reset();
            answerOpen = false;
        };

        StreamHandlers h;
        h.onThinking = [&](const std::string& t) {  // raisonnement natif, affiché tel quel
            if (!thinkingOpen) {
                closeAnswer();
                if (lv.inBlock()) lv.endBlock();
                lv.beginBlock(Block::Thinking);
                thinkingOpen = true;
            }
            lv.write(t);
            if (!gotUsage) lv.setTokens((estimatedChars += t.size()) / 4);
        };
        h.onText = [&](const std::string& t) {
            if (!answerOpen) {
                if (trim(t).empty()) return;  // pas de bloc vide
                if (lv.inBlock()) lv.endBlock();
                thinkingOpen = false;
                lv.beginMarkdown();
                answer = std::make_unique<MarkdownStream>(lv, frameWidth() - 3);
                answerOpen = true;
            }
            answer->feed(t);
            if (!gotUsage) lv.setTokens((estimatedChars += t.size()) / 4);
        };
        h.onTokens = [&](size_t n) {
            gotUsage = true;
            reportedTokens = n;
            lv.setTokens(n);
        };
        h.cancelled = [&] { return lv.interrupted(); };

        StreamOutcome out;
        lv.startStatus(randomVerb(), true);
        try {
            out = streamProvider(*p, key, model, systemPrompt(), g.history, h);
        } catch (const std::exception& e) {
            closeAnswer();
            if (lv.inBlock()) lv.endBlock();
            lv.stopStatus();
            printError(e.what());  // l'historique est gardé tel quel : /retry reprendra à cette étape
            const std::string msg = e.what();
            if (msg.find("HTTP 404") != std::string::npos || msg.find("no longer available") != std::string::npos ||
                msg.find("not found") != std::string::npos || msg.find("does not exist") != std::string::npos)
                std::cout << "  " << P.muted << "The model " << model << " seems unavailable. Run " << ansi::bold
                          << "/model" << ansi::reset << P.muted << " to choose another one." << ansi::reset << "\n";
            else if (msg.find("HTTP 503") != std::string::npos || msg.find("HTTP 529") != std::string::npos ||
                     msg.find("overloaded") != std::string::npos || msg.find("UNAVAILABLE") != std::string::npos)
                std::cout << "  " << P.muted << model << " is overloaded right now. Try " << ansi::bold << "/retry"
                          << ansi::reset << P.muted << " in a moment, or " << ansi::bold << "/model" << ansi::reset
                          << P.muted << " to pick a lighter one." << ansi::reset << "\n";
            else if (msg.find("HTTP 429") != std::string::npos || msg.find("RESOURCE_EXHAUSTED") != std::string::npos ||
                     msg.find("rate limit") != std::string::npos || msg.find("quota") != std::string::npos)
                std::cout << "  " << P.muted << "Rate limit reached. Wait a moment, then type " << ansi::bold
                          << "/retry" << ansi::reset << P.muted << "." << ansi::reset << "\n";
            if (stats.tools)  // des outils ont déjà tourné : on précise que rien n'est perdu
                std::cout << "  " << P.muted << "Progress is kept (" << plural(stats.tools, "tool use")
                          << "): " << ansi::bold << "/retry" << ansi::reset << P.muted
                          << " continues from where the agent stopped." << ansi::reset << "\n";
            return;
        }
        closeAnswer();
        if (lv.inBlock()) lv.endBlock();
        lv.stopStatus();

        if (out.cancelled) {
            std::cout << P.err << "  ⎿  Interrupted by user" << ansi::reset << "\n";
            g.history.push_back({"assistant", out.text + "\n[interrupted by the user]"});
            return;
        }

        Message reply{"assistant", out.text};
        reply.toolCalls = out.toolCalls;
        if (p->protocol == Protocol::Anthropic) {  // la réflexion signée doit être renvoyée avec les outils
            reply.thinking = out.reasoning;
            reply.thinkingSignature = out.reasoningSignature;
        }
        g.history.push_back(reply);
        const size_t used = gotUsage ? reportedTokens : estimatedChars / 4;
        stats.tokens += used;
        stats.tools += out.toolCalls.size();
        g.usage.requests++;
        g.usage.outputTokens += used;
        g.usage.toolCalls += out.toolCalls.size();

        if (out.toolCalls.empty()) break;  // réponse finale (déjà affichée en Markdown pendant le flux)

        // Exécution des outils. Chaque appel reçoit un résultat, même après une interruption.
        bool interrupted = false;
        for (const auto& tc : out.toolCalls) {
            std::string result = interrupted ? "Not executed: interrupted by the user." : executeAgentTool(tc);
            g.history.push_back(Message::toolResult(truncate(result, kMaxResultChars), tc.id, tc.name));
            if (!interrupted && LiveView::get().interrupted()) interrupted = true;
        }
        if (interrupted) {
            std::cout << P.err << "  ⎿  Interrupted by user" << ansi::reset << "\n";
            break;
        }

        if (step == kMaxAgentSteps - 1)
            printError("Stopped after " + std::to_string(kMaxAgentSteps) + " steps.");
    }

    trimHistory();
}

// Envoie une question et laisse l'agent travailler jusqu'à sa réponse finale.
void chat(const std::string& userText) {
    if (!activeProvider()) {
        printError("No API key configured. Run /key first.");
        return;
    }
    // Question précédente restée sans réponse (erreur avant toute étape) : l'utilisateur passe à autre
    // chose, on la retire pour ne pas envoyer deux messages utilisateur d'affilée.
    if (!g.history.empty() && g.history.back().role == "user") g.history.pop_back();
    g.history.push_back({"user", userText});
    runAgent();
}

// Commandes qui relancent l'agent : /retry et /init. Retourne true si la commande a été traitée.
bool handleAgentCommand(const std::string& line) {
    const std::string cmd = splitWords(line)[0];
    if (cmd == "/retry") {
        if (turnUnfinished()) {  // le dernier tour s'est arrêté sur une erreur : on le reprend
            if (g.history.back().role == "user")
                std::cout << P.muted << "> " << truncate(g.history.back().text, 300) << ansi::reset << "\n";
            else
                std::cout << P.muted << "Resuming where the agent stopped…" << ansi::reset << "\n";
            runAgent();
            return true;
        }
        for (size_t i = g.history.size(); i-- > 0;) {
            if (g.history[i].role != "user") continue;
            const std::string text = g.history[i].text;
            g.history.erase(g.history.begin() + (long)i, g.history.end());
            std::cout << P.muted << "> " << truncate(text, 300) << ansi::reset << "\n";
            chat(text);
            return true;
        }
        printError("Nothing to retry yet.");
        return true;
    }
    if (cmd == "/init") {
        std::error_code ec;
        const bool exists = std::filesystem::exists(std::filesystem::path(toWide(portalPath())), ec);
        chat(std::string(exists ? "Update the existing PORTAL.md file in the working directory: read it, check it against "
                                  "the project, and fix what is outdated or missing."
                                : "Create a PORTAL.md file in the working directory.") +
             " First explore the project with list_dir and read_file (README, build files, main sources). PORTAL.md "
             "must contain: a short overview of the project, its folder structure, the exact commands to build, run "
             "and test it, the code conventions you observe, and important notes for an AI agent working here. "
             "Keep it under 60 lines of Markdown.");
        return true;
    }
    return false;
}
