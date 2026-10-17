// web.hpp : /web — lance le chat web depuis le terminal.
//
//   /web          démarre le serveur (codeportal --headless, en arrière-plan) et ouvre le navigateur, déjà connecté
//   /web phone    le rend joignable depuis le Wi-Fi et affiche un QR code à scanner avec le téléphone
//   /web stop     arrête le serveur          /web status   où il tourne
//
// Le serveur est un second processus CodePortal, caché, rattaché à un « job » Windows : il s'arrête tout seul
// quand ce terminal se ferme. Il a ses propres conversations, dans le dossier de travail actuel.
// Sa sortie va dans %APPDATA%\CodePortal\web.log (utile si le port est déjà pris).
#pragma once

#include "util.hpp"  // winsock2.h puis windows.h

#include <shellapi.h>
#include <ws2tcpip.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "agenttools.hpp"
#include "qr.hpp"
#include "state.hpp"

namespace web {

namespace fs = std::filesystem;

constexpr unsigned short kDefaultPort = 8765;

struct Server {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    unsigned short port = 0;
    bool lan = false;
};

inline Server& server() {
    static Server s;
    return s;
}

inline bool running() {
    Server& s = server();
    return s.process && WaitForSingleObject(s.process, 0) == WAIT_TIMEOUT;
}

inline std::string logPath() { return g.configDir + "\\web.log"; }

inline std::string readToken() {
    std::ifstream in(fs::u8path(g.configDir + "\\api.token"));
    std::string t;
    std::getline(in, t);
    return trim(t);
}

// Adresse IPv4 de ce PC sur le réseau local (192.168.x.x de préférence), "" si aucune.
inline std::string lanAddress() {
    static bool started = false;
    if (!started) {
        WSADATA wsa;
        started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }
    // D'abord la carte réseau que Windows utiliserait pour sortir (UDP « connecté » : aucun paquet envoyé).
    if (SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); u != INVALID_SOCKET) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(80);
        to.sin_addr.s_addr = inet_addr("8.8.8.8");
        sockaddr_in me{};
        int len = sizeof me;
        std::string ip;
        if (connect(u, (sockaddr*)&to, sizeof to) == 0 && getsockname(u, (sockaddr*)&me, &len) == 0) {
            const unsigned char* b = (const unsigned char*)&me.sin_addr;
            if (b[0] != 0 && b[0] != 127)
                ip = std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." + std::to_string(b[3]);
        }
        closesocket(u);
        if (!ip.empty()) return ip;
    }
    // Sinon (pas d'accès à Internet) : les adresses du nom de la machine.
    char host[256] = {};
    if (gethostname(host, sizeof host) != 0) return "";
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0) return "";
    std::string best;
    int bestRank = 99;
    for (addrinfo* a = res; a; a = a->ai_next) {
        const auto* sin = (const sockaddr_in*)a->ai_addr;
        const unsigned char* b = (const unsigned char*)&sin->sin_addr;
        int rank = 9;
        if (b[0] == 127 || (b[0] == 169 && b[1] == 254)) continue;  // boucle locale, adresse automatique
        if (b[0] == 192 && b[1] == 168) rank = 0;
        else if (b[0] == 10) rank = 1;
        else if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) rank = 2;
        if (rank < bestRank) {
            bestRank = rank;
            best = std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
                   std::to_string(b[3]);
        }
    }
    freeaddrinfo(res);
    return best;
}

// Le serveur répond-il ? (n'importe quelle réponse HTTP suffit : sans jeton, c'est un 401)
inline bool answers(unsigned short port) {
    const xt::HttpGet r = xt::httpGet("http://127.0.0.1:" + std::to_string(port) + "/api/v1/status", 64 * 1024);
    return r.error.empty() && r.status != 0;
}

inline void stop() {
    Server& s = server();
    if (s.process) {
        TerminateProcess(s.process, 0);
        WaitForSingleObject(s.process, 3000);
        CloseHandle(s.process);
    }
    if (s.job) CloseHandle(s.job);
    s = Server{};
}

inline std::string lastLogLine() {
    std::ifstream in(fs::u8path(logPath()));
    std::string line, last;
    while (std::getline(in, line))
        if (!trim(line).empty()) last = trim(line);
    return last;
}

// Lance `codeportal --headless` caché, dans le dossier de travail. Retourne "" ou un message d'erreur.
inline std::string start(unsigned short port, bool lan) {
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof exe / sizeof exe[0]));
    std::wstring cmd = L"\"" + std::wstring(exe, n) + L"\" --headless --no-update-check --port " + std::to_wstring(port);
    if (lan) cmd += L" --bind 0.0.0.0";

    std::error_code ec;
    fs::create_directories(fs::u8path(g.configDir), ec);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(toWide(logPath()).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = si.hStdError = log != INVALID_HANDLE_VALUE ? log : nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    const std::wstring cwd = toWide(g.cwd);
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                   nullptr, cwd.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) return "Cannot start the web server (error " + std::to_string(GetLastError()) + ").";

    // Rattaché à un job qui tue ses processus à la fermeture de CodePortal (même en cas de plantage).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    Server& s = server();
    s.process = pi.hProcess;
    s.job = job;
    s.port = port;
    s.lan = lan;

    // Attente du démarrage (quelques secondes au plus).
    for (int i = 0; i < 50; ++i) {
        if (WaitForSingleObject(s.process, 100) == WAIT_OBJECT_0) {
            const std::string why = lastLogLine();
            stop();
            return why.empty() ? "The web server stopped right away." : why;
        }
        if (answers(port)) return "";
    }
    stop();
    return "The web server did not answer. See " + logPath() + ".";
}

inline std::string localUrl() { return "http://127.0.0.1:" + std::to_string(server().port); }

inline bool openBrowser(const std::string& url) {
    return (INT_PTR)ShellExecuteW(nullptr, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}

}  // namespace web
