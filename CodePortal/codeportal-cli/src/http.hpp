// http.hpp : requêtes HTTPS via WinHTTP (inclus dans Windows, rien à installer).
// Deux modes : réponse complète (httpsRequest) ou lecture au fil de l'eau (httpsStream).
#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "util.hpp"  // doit venir avant windows.h (définit WIN32_LEAN_AND_MEAN / NOMINMAX)

#include <winhttp.h>

struct HttpResult {
    DWORD status = 0;
    std::string body;
    bool cancelled = false;  // lecture interrompue (Échap)
};

// Si onData est fourni et que le statut est 200, chaque morceau reçu lui est passé au lieu d'être
// accumulé dans body. onData renvoie false pour interrompre la lecture.
HttpResult httpsStream(const std::string& method, const std::string& host, const std::string& path,
                       const std::vector<std::string>& headers, const std::string& body,
                       const std::function<bool(const std::string&)>& onData) {
    HttpResult result;
    HINTERNET session = WinHttpOpen(L"CodePortal/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) throw std::runtime_error("WinHttpOpen failed");
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 300000);  // réponses longues : jusqu'à 5 min

    HINTERNET conn = WinHttpConnect(session, toWide(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    std::wstring wMethod = toWide(method);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, wMethod.c_str(), toWide(path).c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE)
                         : nullptr;
    auto cleanup = [&] {
        if (req) WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
    };
    if (!req) {
        cleanup();
        throw std::runtime_error("cannot connect to " + host + " (error " + std::to_string(GetLastError()) + ")");
    }

    for (const auto& h : headers) {
        std::wstring w = toWide(h);
        WinHttpAddRequestHeaders(req, w.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    BOOL ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 const_cast<char*>(body.data()), (DWORD)body.size(),
                                 (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        DWORD err = GetLastError();
        cleanup();
        throw std::runtime_error("request to " + host + " failed (error " + std::to_string(err) + ")");
    }

    DWORD code = 0, size = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
    result.status = code;
    const bool streaming = onData && code == 200;

    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req, &chunk[0], avail, &read)) break;
        chunk.resize(read);
        if (!streaming) {
            result.body += chunk;
        } else if (!onData(chunk)) {
            result.cancelled = true;
            break;
        }
    }
    cleanup();
    return result;
}

HttpResult httpsRequest(const std::string& method, const std::string& host, const std::string& path,
                        const std::vector<std::string>& headers, const std::string& body) {
    return httpsStream(method, host, path, headers, body, nullptr);
}
