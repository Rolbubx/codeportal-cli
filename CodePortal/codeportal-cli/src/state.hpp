// state.hpp : état de l'application et fichier de configuration.
#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "util.hpp"

// Appel d'outil natif émis par le modèle.
struct ToolCallRec {
    std::string id;
    std::string name;
    std::string args;       // objet JSON (texte)
    std::string signature;  // Gemini : thoughtSignature à renvoyer telle quelle
};

struct Message {
    Message() = default;
    Message(std::string r, std::string t) : role(std::move(r)), text(std::move(t)) {}

    // Résultat d'un outil, rattaché à l'appel correspondant.
    static Message toolResult(std::string text, std::string callId, std::string name) {
        Message m("tool", std::move(text));
        m.toolCallId = std::move(callId);
        m.toolName = std::move(name);
        return m;
    }

    std::string role;  // "user", "assistant" ou "tool"
    std::string text;
    std::vector<ToolCallRec> toolCalls;  // assistant : outils appelés
    std::string toolCallId;              // tool : id de l'appel auquel on répond
    std::string toolName;                // tool : nom de l'outil
    std::string thinking;                // assistant (Anthropic) : réflexion à renvoyer avec sa signature
    std::string thinkingSignature;
};

struct AppState {
    std::string configDir;
    std::string configPath;
    std::string cwd;
    std::string activeId;
    std::string theme = "dark";                     // "dark" ou "light"
    bool autoRun = false;                           // /auto on : pas de confirmation pour les commandes utilisateur
    std::map<std::string, std::string> storedKeys;  // clés enregistrées dans config.ini
    std::map<std::string, std::string> models;      // modèle choisi par fournisseur
    std::vector<Message> history;

    // Statistiques de la session (/usage).
    struct Usage {
        size_t requests = 0;
        size_t outputTokens = 0;
        size_t toolCalls = 0;
        double agentSeconds = 0;
    } usage;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

inline AppState g;

void loadConfig() {
    std::ifstream in(g.configPath);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (k == "active") g.activeId = v;
        else if (k == "theme") g.theme = (v == "light") ? "light" : "dark";
        else if (k.rfind("key.", 0) == 0) g.storedKeys[k.substr(4)] = v;
        else if (k.rfind("model.", 0) == 0) g.models[k.substr(6)] = v;
    }
}

void saveConfig() {
    try {
        std::filesystem::create_directories(g.configDir);
    } catch (const std::exception&) {
    }
    std::ofstream out(g.configPath, std::ios::trunc);
    out << "# CodePortal config (API keys are stored in plain text, do not share this file)\n";
    out << "active=" << g.activeId << "\n";
    out << "theme=" << g.theme << "\n";
    for (const auto& [id, key] : g.storedKeys)
        if (!key.empty()) out << "key." << id << "=" << key << "\n";
    for (const auto& [id, model] : g.models)
        if (!model.empty()) out << "model." << id << "=" << model << "\n";
}
