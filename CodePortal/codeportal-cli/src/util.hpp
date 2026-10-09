// util.hpp : styles, thèmes clair/sombre, helpers texte UTF-8, helpers console, spinner.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ansi {
constexpr const char* reset = "\x1b[0m";
constexpr const char* bold = "\x1b[1m";
constexpr const char* dim = "\x1b[2m";
constexpr const char* italic = "\x1b[3m";
}  // namespace ansi

// Couleurs selon le thème (couleurs 24 bits, orange « Claude » en accent).
struct Palette {
    const char* accent;      // bannière, spinner, puces d'outils
    const char* text;        // texte principal
    const char* muted;       // texte secondaire, résultats, réflexion
    const char* ok;          // succès
    const char* err;         // erreurs
    const char* warn;        // appels d'outils en attente
    const char* add;         // texte des lignes ajoutées (diff)
    const char* del;         // texte des lignes supprimées (diff)
    const char* code;        // code inline / blocs de code
    const char* addBg;       // fond des lignes ajoutées
    const char* delBg;       // fond des lignes supprimées
    const char* shimmerHi;   // reflet du scintillement
    const char* shimmerMid;  // halo du scintillement
    const char* border;      // cadre de saisie
    const char* kw;          // code : mots-clés
    const char* str;         // code : chaînes
    const char* num;         // code : nombres
    const char* cmt;         // code : commentaires
};

inline Palette P;

inline void applyTheme(const std::string& name) {
    if (name == "light") {
        P = {"\x1b[38;2;196;92;58m",   "\x1b[38;2;30;30;30m",    "\x1b[38;2;112;112;112m",
             "\x1b[38;2;40;128;60m",   "\x1b[38;2;196;40;60m",   "\x1b[38;2;150;150;150m",
             "\x1b[38;2;20;90;35m",    "\x1b[38;2;140;20;35m",   "\x1b[38;2;70;80;190m",
             "\x1b[48;2;208;242;214m", "\x1b[48;2;250;214;218m", "\x1b[38;2;245;160;120m",
             "\x1b[38;2;222;125;90m",  "\x1b[38;2;160;160;160m", "\x1b[38;2;160;40;150m",
             "\x1b[38;2;30;120;50m",   "\x1b[38;2;170;90;0m",    "\x1b[38;2;130;130;130m"};
    } else {
        P = {"\x1b[38;2;215;119;87m",  "\x1b[38;2;235;235;235m", "\x1b[38;2;145;145;145m",
             "\x1b[38;2;78;186;101m",  "\x1b[38;2;255;107;128m", "\x1b[38;2;120;120;120m",
             "\x1b[38;2;200;255;205m", "\x1b[38;2;255;205;210m", "\x1b[38;2;177;185;249m",
             "\x1b[48;2;34;84;44m",    "\x1b[48;2;110;38;50m",   "\x1b[38;2;255;214;190m",
             "\x1b[38;2;240;165;135m", "\x1b[38;2;110;110;110m", "\x1b[38;2;198;146;234m",
             "\x1b[38;2;152;195;121m", "\x1b[38;2;229;192;123m", "\x1b[38;2;110;118;129m"};
    }
}

size_t consoleHeight() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return (size_t)(info.srWindow.Bottom - info.srWindow.Top + 1);
    return 25;
}

constexpr int kMaxAgentSteps = 30;         // appels API max par question (un par série d'outils)
constexpr size_t kMaxResultChars = 4000;   // taille max d'un résultat renvoyé au modèle

// ---------------------------------------------------------------------------
// Texte / UTF-8
// ---------------------------------------------------------------------------
std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Retire seulement les retours à la ligne (préserve l'indentation du code).
std::string trimNewlines(const std::string& s) {
    size_t a = s.find_first_not_of("\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of("\r\n");
    return s.substr(a, b - a + 1);
}

// Nombre de colonnes approximatif (un caractère UTF-8 = une colonne).
size_t displayLength(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string repeatStr(const std::string& unit, size_t n) {
    std::string r;
    for (size_t i = 0; i < n; ++i) r += unit;
    return r;
}

std::vector<std::string> splitWords(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> out;
    std::string w;
    while (iss >> w) out.push_back(w);
    return out;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string indentLines(const std::string& text) {
    std::string out;
    for (char c : text) {
        out += c;
        if (c == '\n') out += "  ";
    }
    return out;
}

std::string truncate(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    return s.substr(0, max) + "\n[... truncated]\n";
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Premiers `n` caractères (pas d'octets) d'une chaîne UTF-8.
std::string utf8Prefix(const std::string& s, size_t n) {
    size_t count = 0, i = 0;
    for (; i < s.size(); ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (count == n) break;
            ++count;
        }
    }
    return s.substr(0, i);
}

// Coupe un texte en lignes de `width` colonnes max, en gardant les mots entiers si possible.
std::vector<std::string> wrapWords(const std::string& text, size_t width) {
    if (width < 10) width = 10;
    std::vector<std::string> out;
    std::string line;
    std::istringstream iss(text);
    std::string word;
    while (iss >> word) {
        while (displayLength(word) > width) {  // mot trop long : découpe brutale
            if (!line.empty()) {
                out.push_back(line);
                line.clear();
            }
            std::string head = utf8Prefix(word, width);
            out.push_back(head);
            word = word.substr(head.size());
        }
        if (!line.empty() && displayLength(line) + 1 + displayLength(word) > width) {
            out.push_back(line);
            line.clear();
        }
        if (!line.empty()) line += ' ';
        line += word;
    }
    if (!line.empty() || out.empty()) out.push_back(line);
    return out;
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------
size_t consoleWidth() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return (size_t)(info.srWindow.Right - info.srWindow.Left + 1);
    return 80;
}

std::string currentDir() {
    DWORD n = GetCurrentDirectoryW(0, nullptr);
    std::wstring buf(n, L'\0');
    GetCurrentDirectoryW(n, &buf[0]);
    if (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return toUtf8(buf);
}

// Exécute f en capturant tout ce qui serait écrit sur std::cout.
template <class F>
std::string captureOutput(F&& f) {
    std::ostringstream ss;
    std::streambuf* old = std::cout.rdbuf(ss.rdbuf());
    try {
        f();
    } catch (...) {
        std::cout.rdbuf(old);
        throw;
    }
    std::cout.rdbuf(old);
    return ss.str();
}

// Saisie masquée (clés API) : chaque caractère est remplacé par *.
std::string readMasked(const std::string& label) {
    std::wstring buf;
    std::cout << label << std::flush;
    for (;;) {
        wint_t ch = _getwch();
        if (ch == L'\r' || ch == L'\n') break;
        if (ch == 3) {  // Ctrl+C
            std::cout << "\n";
            std::exit(0);
        }
        if (ch == 8) {
            if (!buf.empty()) {
                buf.pop_back();
                std::cout << "\b \b" << std::flush;
            }
        } else if (ch == 0 || ch == 0xE0) {
            _getwch();
        } else if (ch >= 32) {
            buf.push_back((wchar_t)ch);
            std::cout << '*' << std::flush;
        }
    }
    std::cout << "\n";
    return trim(toUtf8(buf));
}

