// api.hpp : mode headless et API HTTP locale, pour piloter CodePortal depuis un autre programme.
//
//   codeportal.exe --headless [--port 8765] [--bind 127.0.0.1]
//
// Pas d'interface : le programme écoute sur 127.0.0.1 par défaut et répond en JSON.
//   GET  /api/v1/status                       fournisseur, modèle, dossier, occupé ou non
//   POST /api/v1/ask      {"message": "..."}  pose une question ; l'agent peut lire et modifier les fichiers
//   POST /api/v1/cancel                       arrête le tour en cours
//   POST /api/v1/reset                        efface la conversation
// Chaque requête porte « Authorization: Bearer <jeton> ». Le jeton est créé au premier lancement dans
// api.token, à côté de config.ini. Un seul tour à la fois : un ask pendant un autre tour reçoit 409.
//
// La page web web/codeportal-chat.html s'y connecte ; --bind 0.0.0.0 la rend joignable depuis le réseau.
//
// Attention : le jeton donne à l'agent le droit de modifier des fichiers et d'exécuter des commandes
// dans le dossier de travail. Par défaut le serveur n'écoute que sur la boucle locale.
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "agent.hpp"
#include "json.hpp"

namespace api {

constexpr size_t kMaxRequestBytes = 1 << 20;  // 1 Mo
inline std::mutex turnMu;                     // un seul tour à la fois
inline std::atomic<bool> busy{false};
inline std::string token;

std::string tokenPath() { return g.configDir + "\\api.token"; }

// Jeton partagé : créé une fois (64 caractères hexadécimaux), réutilisé aux lancements suivants.
std::string loadOrCreateToken() {
    std::string saved;
    std::ifstream in(tokenPath());
    std::getline(in, saved);
    saved = trim(saved);
    if (saved.size() >= 32) return saved;

    std::random_device rd;
    std::mt19937_64 rng((uint64_t(rd()) << 32) ^ uint64_t(rd()));
    std::string fresh;
    for (int i = 0; i < 4; ++i) {
        char part[17];
        snprintf(part, sizeof part, "%016llx", (unsigned long long)rng());
        fresh += part;
    }
    try {
        std::filesystem::create_directories(g.configDir);
    } catch (const std::exception&) {
    }
    std::ofstream out(tokenPath(), std::ios::trunc);
    out << fresh << "\n";
    return fresh;
}

// Comparaison sans sortie anticipée (pas de fuite de temps sur le jeton).
bool sameToken(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Retire les séquences de couleur et les retours chariot (pour les journaux renvoyés au client).
std::string stripControl(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
            continue;
        }
        if (s[i] != '\r') out += s[i];
    }
    return out;
}

struct Request {
    std::string method, path, authorization, body;
};

// Lit une requête HTTP/1.1 : en-têtes, puis Content-Length octets de corps.
bool readRequest(SOCKET s, Request& req) {
    std::string buf;
    char chunk[4096];
    size_t headEnd;
    while ((headEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        const int n = recv(s, chunk, sizeof chunk, 0);
        if (n <= 0 || buf.size() > kMaxRequestBytes) return false;
        buf.append(chunk, (size_t)n);
    }

    const std::string head = buf.substr(0, headEnd);
    size_t pos = 0, contentLength = 0;
    bool expectContinue = false, first = true;
    while (pos < head.size()) {
        size_t end = head.find("\r\n", pos);
        if (end == std::string::npos) end = head.size();
        const std::string line = head.substr(pos, end - pos);
        pos = end + 2;
        if (first) {  // « POST /api/v1/ask HTTP/1.1 »
            first = false;
            const size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
            req.method = line.substr(0, sp1);
            req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lowerAscii(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        try {
            if (name == "content-length") contentLength = std::stoul(value);
        } catch (const std::exception&) {
            return false;
        }
        if (name == "authorization") req.authorization = value;
        if (name == "expect") expectContinue = true;
    }
    if (contentLength > kMaxRequestBytes) return false;
    if (expectContinue) {  // curl envoie « Expect: 100-continue » pour les corps un peu gros
        const std::string cont = "HTTP/1.1 100 Continue\r\n\r\n";
        send(s, cont.data(), (int)cont.size(), 0);
    }

    req.body = buf.substr(headEnd + 4);
    while (req.body.size() < contentLength) {
        const int n = recv(s, chunk, sizeof chunk, 0);
        if (n <= 0) return false;
        req.body.append(chunk, (size_t)n);
    }
    req.body.resize(contentLength);
    return true;
}

// Réponse JSON. CORS : une page web (web/codeportal-chat.html) peut appeler l'API. Le jeton reste
// obligatoire, et une page web ne peut pas le lire.
void reply(SOCKET s, int code, const char* reason, const std::string& json) {
    const std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason +
                            "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
                            std::to_string(json.size()) +
                            "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Authorization, Content-Type"
                            "\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nConnection: close\r\n\r\n" + json;
    size_t sent = 0;
    while (sent < out.size()) {
        const int n = send(s, out.data() + sent, (int)(out.size() - sent), 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

std::string errorJson(const std::string& msg) { return "{\"ok\":false,\"error\":" + jsonStr(msg) + "}"; }

// Appels d'outils faits pendant le tour, dans l'ordre, avec leur résultat.
std::string stepsJson(size_t from) {
    struct Step {
        std::string id, name, args, result;
    };
    std::vector<Step> steps;
    for (size_t i = from; i < g.history.size(); ++i) {
        const Message& m = g.history[i];
        for (const auto& tc : m.toolCalls) steps.push_back({tc.id, tc.name, tc.args, ""});
        if (m.role != "tool") continue;
        for (auto it = steps.rbegin(); it != steps.rend(); ++it)
            if (it->id == m.toolCallId) {
                it->result = m.text;
                break;
            }
    }
    std::string out = "[";
    for (size_t i = 0; i < steps.size(); ++i) {
        const Step& s = steps[i];
        out += std::string(i ? "," : "") + "{\"tool\":" + jsonStr(s.name) + ",\"args\":" + jsonStr(s.args) +
               ",\"result\":" + jsonStr(s.result) + "}";
    }
    return out + "]";
}

// Index du message utilisateur qui a lancé le tour (le dernier qui porte ce texte).
size_t turnStart(const std::string& message) {
    for (size_t i = g.history.size(); i-- > 0;)
        if (g.history[i].role == "user" && g.history[i].text == message) return i;
    return 0;
}

// Dernière réponse de l'assistant contenant du texte, après le début du tour.
std::string lastAnswer(size_t from) {
    for (size_t i = g.history.size(); i-- > from;)
        if (g.history[i].role == "assistant" && !trim(g.history[i].text).empty()) return g.history[i].text;
    return "";
}

void handleStatus(SOCKET s) {
    const Provider* p = activeProvider();
    reply(s, 200, "OK",
          "{\"ok\":true,\"busy\":" + std::string(busy ? "true" : "false") + ",\"headless\":true,\"provider\":" +
              jsonStr(p ? p->id : "") + ",\"model\":" + jsonStr(p ? modelFor(*p) : "") + ",\"cwd\":" + jsonStr(g.cwd) +
              "}");
}

void handleAsk(SOCKET s, const std::string& body) {
    JVal v;
    JsonParser parser(body);
    const JVal* field = (parser.parse(v) && v.type == JVal::OBJ) ? v.find("message") : nullptr;
    if (!field || field->type != JVal::STR || trim(field->str).empty())
        return reply(s, 400, "Bad Request", errorJson("expected {\"message\": \"...\"}"));

    std::unique_lock<std::mutex> lock(turnMu, std::try_to_lock);
    if (!lock.owns_lock()) return reply(s, 409, "Conflict", errorJson("a turn is already running"));

    const std::string message = field->str;
    LiveView::get().clearCancel();
    busy = true;
    std::string console;
    try {
        console = captureOutput([&] { chat(message); });  // l'affichage part dans le vide : seul le résultat compte
    } catch (...) {
        busy = false;
        return reply(s, 500, "Internal Server Error", errorJson("the turn failed"));
    }
    busy = false;

    const bool cancelled = LiveView::get().cancelRequested();
    const size_t from = turnStart(message);
    const std::string answer = lastAnswer(from);
    std::string json = "{\"ok\":" + std::string(answer.empty() ? "false" : "true") + ",\"answer\":" + jsonStr(answer) +
                       ",\"cancelled\":" + (cancelled ? "true" : "false") + ",\"steps\":" + stepsJson(from);
    if (answer.empty()) {  // rien de la part du modèle : on renvoie ce qui a été affiché (ex. « No API key »)
        const std::string log = stripControl(console);
        json += ",\"error\":" + jsonStr("no answer from the model") + ",\"log\":" + jsonStr(truncate(log, 4000));
    }
    reply(s, answer.empty() ? 502 : 200, answer.empty() ? "Bad Gateway" : "OK", json + "}");
}

void handleCancel(SOCKET s) {
    if (!busy) return reply(s, 409, "Conflict", errorJson("nothing is running"));
    LiveView::get().requestCancel();
    reply(s, 200, "OK", "{\"ok\":true,\"cancelling\":true}");
}

void handleReset(SOCKET s) {
    std::unique_lock<std::mutex> lock(turnMu, std::try_to_lock);
    if (!lock.owns_lock()) return reply(s, 409, "Conflict", errorJson("a turn is already running"));
    g.history.clear();
    reply(s, 200, "OK", "{\"ok\":true}");
}

// Une connexion = une requête (Connection: close).
void serveConnection(SOCKET s) {
    DWORD timeoutMs = 30000;  // lecture de la requête seulement ; la réponse à ask peut durer longtemps
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof timeoutMs);

    Request req;
    if (!readRequest(s, req)) {
        reply(s, 400, "Bad Request", errorJson("malformed request"));
    } else if (req.method == "OPTIONS") {  // préflight CORS du navigateur : pas de jeton demandé
        reply(s, 204, "No Content", "");
    } else if (req.authorization.rfind("Bearer ", 0) != 0 || !sameToken(token, trim(req.authorization.substr(7)))) {
        reply(s, 401, "Unauthorized", errorJson("missing or wrong token: send Authorization: Bearer <token>"));
    } else if (req.path == "/api/v1/status" && req.method == "GET") {
        handleStatus(s);
    } else if (req.path == "/api/v1/ask" && req.method == "POST") {
        handleAsk(s, req.body);
    } else if (req.path == "/api/v1/cancel" && req.method == "POST") {
        handleCancel(s);
    } else if (req.path == "/api/v1/reset" && req.method == "POST") {
        handleReset(s);
    } else {
        reply(s, 404, "Not Found", errorJson("unknown route: " + req.method + " " + req.path));
    }
    closesocket(s);
}

// Écoute sur host:port (127.0.0.1 par défaut). Ne retourne qu'en cas d'erreur.
int run(const std::string& host, unsigned short port) {
    if (!activeProvider()) {
        std::cerr << "No API key configured. Start CodePortal once with a key, then run --headless again.\n";
        return 1;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed.\n";
        return 1;
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::cerr << "Cannot create the socket.\n";
        return 1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        std::cerr << "Invalid address: " << host << "\n";
        return 1;
    }
    if (bind(listener, (sockaddr*)&addr, sizeof addr) == SOCKET_ERROR || listen(listener, 16) == SOCKET_ERROR) {
        std::cerr << "Cannot listen on " << host << ":" << port << " (is the port already in use? try --port)\n";
        return 1;
    }
    token = loadOrCreateToken();
    std::cout << "CodePortal API listening on http://127.0.0.1:" << port << "\n"
              << "Token file: " << tokenPath() << "\n"
              << "Send it as: Authorization: Bearer <token>\n";
    if (host != "127.0.0.1" && host != "localhost")
        std::cout << "Warning: reachable from other machines (" << host
                  << "). Anyone with the token can make the agent edit files and run commands here.\n";
    std::cout << std::flush;
    for (;;) {
        const SOCKET c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::thread(serveConnection, c).detach();
    }
}

}  // namespace api
