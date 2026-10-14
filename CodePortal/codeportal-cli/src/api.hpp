// api.hpp : mode headless et API HTTP locale, pour piloter CodePortal depuis un autre programme.
//
//   codeportal.exe --headless [--port 8765] [--bind 127.0.0.1]
//
// Pas d'interface : le programme écoute sur 127.0.0.1 par défaut et répond en JSON.
//   GET  /                                    la page de chat (web/codeportal-chat.html), sans jeton
//   GET  /api/v1/status                       fournisseur, modèle, dossier, occupé ou non
//   POST /api/v1/ask      {"message": "..."}  pose une question ; l'agent peut lire et modifier les fichiers
//   GET  /api/v1/events?after=N               ce que fait l'agent, en direct : événements numérotés après N
//   POST /api/v1/cancel                       arrête le tour en cours
//   POST /api/v1/reset                        efface la conversation
// Les routes /api/v1/* exigent « Authorization: Bearer <jeton> ». Le jeton est créé au premier lancement
// dans api.token, à côté de config.ini. Un seul tour à la fois : un ask pendant un autre tour reçoit 409.
// Au démarrage, le lien à ouvrir (le jeton est mis dans le fragment #token=...) est affiché.
//
// Attention : le jeton donne à l'agent le droit de modifier des fichiers et d'exécuter des commandes
// dans le dossier de travail. Par défaut le serveur n'écoute que sur la boucle locale.
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "agent.hpp"
#include "json.hpp"

namespace api {

constexpr size_t kMaxRequestBytes = 1 << 20;  // 1 Mo
constexpr size_t kMaxEvents = 5000;           // événements gardés en mémoire
constexpr size_t kMaxEventsPerPoll = 500;

inline std::mutex turnMu;  // un seul tour à la fois
inline std::atomic<bool> busy{false};
inline std::string token;

// Journal des événements : numérotés, pour que le client demande « tout ce qui suit N ».
struct Event {
    unsigned long long seq;
    std::string type;
    std::string data;  // objet JSON
};
inline std::mutex evMu;
inline std::deque<Event> events;
inline unsigned long long nextSeq = 0;

void record(const std::string& type, const std::string& data) {
    std::lock_guard<std::mutex> lk(evMu);
    events.push_back({++nextSeq, type, data});
    if (events.size() > kMaxEvents) events.pop_front();
}

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

// Réponse. CORS : une page web peut appeler l'API ; le jeton reste obligatoire.
void reply(SOCKET s, int code, const char* reason, const std::string& body,
           const char* type = "application/json; charset=utf-8") {
    const std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + type +
                            "\r\nContent-Length: " + std::to_string(body.size()) +
                            "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Authorization, Content-Type"
                            "\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nCache-Control: no-store"
                            "\r\nConnection: close\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < out.size()) {
        const int n = send(s, out.data() + sent, (int)(out.size() - sent), 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

std::string errorJson(const std::string& msg) { return "{\"ok\":false,\"error\":" + jsonStr(msg) + "}"; }

// Page de chat : web/codeportal-chat.html, dans le dossier parent de build\ ou dans build\web.
// Lue à chaque requête : une modification de la page est visible sans recompiler.
bool readPage(std::string& html) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    const fs::path dir = fs::path(exe).parent_path();
    for (const fs::path& p : {dir / ".." / "web" / "codeportal-chat.html", dir / "web" / "codeportal-chat.html"})
        if (readText(p, html)) return true;
    return false;
}

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

unsigned long long afterFrom(const std::string& query);

// Événements après le numéro `after`. `next` est le dernier numéro renvoyé : le client repart de là.
// Sans « after= » (première synchronisation d'une page), on commence au dernier tour, pour qu'une page
// rechargée pendant un tour retrouve ce qui se passe.
void handleEvents(SOCKET s, const std::string& query) {
    std::lock_guard<std::mutex> lk(evMu);
    unsigned long long after = 0;
    if (query.find("after=") != std::string::npos) {
        after = afterFrom(query);
    } else {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->type == "turn_start") {
                after = it->seq - 1;
                break;
            }
    }
    std::string list;
    unsigned long long next = nextSeq;
    size_t sent = 0;
    for (const auto& e : events) {
        if (e.seq <= after) continue;
        if (sent == kMaxEventsPerPoll) break;
        list += std::string(sent ? "," : "") + "{\"seq\":" + std::to_string(e.seq) + ",\"type\":" + jsonStr(e.type) +
                ",\"data\":" + e.data + "}";
        next = e.seq;
        ++sent;
    }
    const bool gap = after > 0 && !events.empty() && events.front().seq > after + 1;
    reply(s, 200, "OK",
          "{\"ok\":true,\"busy\":" + std::string(busy ? "true" : "false") + ",\"next\":" + std::to_string(next) +
              ",\"gap\":" + (gap ? "true" : "false") + ",\"events\":[" + list + "]}");
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
    activity("turn_start", "{\"message\":" + jsonStr(message) + "}");
    std::string console;
    try {
        console = captureOutput([&] { chat(message); });  // l'affichage part dans le vide : seul le résultat compte
    } catch (...) {
        busy = false;
        activity("turn_end", "{\"answer\":\"\",\"cancelled\":false,\"error\":\"the turn failed\"}");
        return reply(s, 500, "Internal Server Error", errorJson("the turn failed"));
    }
    busy = false;

    const bool cancelled = LiveView::get().cancelRequested();
    const size_t from = turnStart(message);
    const std::string answer = lastAnswer(from);
    activity("turn_end", "{\"answer\":" + jsonStr(answer) + ",\"cancelled\":" + (cancelled ? "true" : "false") +
                             ",\"error\":" + jsonStr(answer.empty() ? "no answer from the model" : "") + "}");

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

// « after=N » dans la chaîne de requête ; 0 si absent ou invalide.
unsigned long long afterFrom(const std::string& query) {
    const size_t p = query.find("after=");
    if (p == std::string::npos) return 0;
    try {
        return std::stoull(query.substr(p + 6));
    } catch (const std::exception&) {
        return 0;
    }
}

// Une connexion = une requête (Connection: close).
void serveConnection(SOCKET s) {
    DWORD timeoutMs = 30000;  // lecture de la requête seulement ; la réponse à ask peut durer longtemps
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof timeoutMs);

    Request req;
    if (!readRequest(s, req)) {
        reply(s, 400, "Bad Request", errorJson("malformed request"));
        closesocket(s);
        return;
    }
    std::string target = req.path, query;
    if (const size_t q = target.find('?'); q != std::string::npos) {
        query = target.substr(q + 1);
        target.resize(q);
    }

    if (req.method == "OPTIONS") {  // préflight CORS du navigateur : pas de jeton demandé
        reply(s, 204, "No Content", "");
    } else if (req.method == "GET" && (target == "/" || target == "/index.html")) {
        std::string html;
        if (readPage(html)) reply(s, 200, "OK", html, "text/html; charset=utf-8");
        else reply(s, 404, "Not Found", errorJson("web/codeportal-chat.html not found next to codeportal.exe"));
    } else if (req.authorization.rfind("Bearer ", 0) != 0 || !sameToken(token, trim(req.authorization.substr(7)))) {
        reply(s, 401, "Unauthorized", errorJson("missing or wrong token: send Authorization: Bearer <token>"));
    } else if (target == "/api/v1/status" && req.method == "GET") {
        handleStatus(s);
    } else if (target == "/api/v1/events" && req.method == "GET") {
        handleEvents(s, query);
    } else if (target == "/api/v1/ask" && req.method == "POST") {
        handleAsk(s, req.body);
    } else if (target == "/api/v1/cancel" && req.method == "POST") {
        handleCancel(s);
    } else if (target == "/api/v1/reset" && req.method == "POST") {
        handleReset(s);
    } else {
        reply(s, 404, "Not Found", errorJson("unknown route: " + req.method + " " + target));
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
    g_activity = record;

    const std::string shown = host == "0.0.0.0" ? "localhost" : host;
    std::cout << "CodePortal is running headless on http://" << shown << ":" << port << "\n"
              << "Open this in your browser (the page stores the token, then removes it from the address):\n"
              << "  http://" << shown << ":" << port << "/#token=" << token << "\n"
              << "Token file: " << tokenPath() << "\n";
    if (host != "127.0.0.1" && host != "localhost")
        std::cout << "Warning: reachable from other machines. Anyone with the token can make the agent edit files and run commands here.\n";
    std::cout << std::flush;
    for (;;) {
        const SOCKET c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::thread(serveConnection, c).detach();
    }
}

}  // namespace api
