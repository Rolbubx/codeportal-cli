// markdown.hpp : rendu Markdown pour le terminal.
//
//   En ligne : **gras**, *italique*, ***les deux***, ~~barré~~, `code`, [liens](url), URLs, ![images](url)
//   Blocs    : titres (#), listes imbriquées (- * +), listes numérotées (1.), cases [ ] / [x],
//              citations (>), séparateurs (---), tableaux (| a | b |) avec alignement,
//              blocs de code (```lang) avec coloration syntaxique.
//
// Le texte est découpé en mots qui gardent leur style : le retour à la ligne ne casse jamais
// un gras ou un lien en deux.
#pragma once

#include <cctype>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "util.hpp"

namespace md {

enum : unsigned { BOLD = 1, ITALIC = 2, CODE = 4, STRIKE = 8, LINK = 16, URL = 32 };

struct Span {
    std::string text;
    unsigned style;
};

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
inline bool isBoundary(char c) { return c == 0 || isSpace(c) || std::ispunct((unsigned char)c); }

// ---------------------------------------------------------------------------
// Style en ligne
// ---------------------------------------------------------------------------
std::vector<Span> parseInline(const std::string& s, unsigned base = 0) {
    std::vector<Span> out;
    std::string cur;
    unsigned st = base;
    auto flush = [&] {
        if (!cur.empty()) {
            out.push_back({cur, st});
            cur.clear();
        }
    };
    // Un délimiteur ouvre s'il est suivi d'un caractère visible et refermé plus loin.
    auto opens = [&](size_t i, size_t len) {
        size_t j = i + len;
        return j < s.size() && !isSpace(s[j]) && s.find(s.substr(i, len), j) != std::string::npos;
    };

    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        const char prev = i ? s[i - 1] : 0;
        const char next = i + 1 < s.size() ? s[i + 1] : 0;

        if (c == '\\' && next && std::ispunct((unsigned char)next)) {  // caractère échappé
            cur += next;
            i += 2;
            continue;
        }
        if (c == '`') {  // `code` (ou ``code``)
            size_t ticks = 1;
            while (i + ticks < s.size() && s[i + ticks] == '`') ++ticks;
            size_t end = s.find(std::string(ticks, '`'), i + ticks);
            if (end != std::string::npos) {
                flush();
                out.push_back({trim(s.substr(i + ticks, end - i - ticks)), st | CODE});
                i = end + ticks;
                continue;
            }
        }
        if (s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0) {  // URL nue
            size_t j = i;
            while (j < s.size() && !isSpace(s[j]) && s[j] != ')' && s[j] != '>') ++j;
            while (j > i && std::strchr(".,;:!?", s[j - 1])) --j;
            flush();
            out.push_back({s.substr(i, j - i), st | LINK});
            i = j;
            continue;
        }
        if ((c == '[' || (c == '!' && next == '[')) ) {  // [texte](url) ou ![image](url)
            const bool image = c == '!';
            const size_t open = image ? i + 1 : i;
            const size_t close = s.find(']', open + 1);
            if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
                const size_t paren = s.find(')', close + 2);
                if (paren != std::string::npos) {
                    flush();
                    const std::string label = s.substr(open + 1, close - open - 1);
                    const std::string url = s.substr(close + 2, paren - close - 2);
                    if (image) out.push_back({"[image: " + label + "]", st | ITALIC});
                    else
                        for (const auto& sp : parseInline(label, st)) out.push_back({sp.text, sp.style | LINK});
                    if (!url.empty() && url != label) out.push_back({" (" + url + ")", st | URL});
                    i = paren + 1;
                    continue;
                }
            }
        }
        if (s.compare(i, 2, "~~") == 0 && ((st & STRIKE) || opens(i, 2))) {
            flush();
            st ^= STRIKE;
            i += 2;
            continue;
        }
        if (s.compare(i, 3, "***") == 0 && (((st & BOLD) && (st & ITALIC)) || opens(i, 3))) {
            flush();
            st ^= (BOLD | ITALIC);
            i += 3;
            continue;
        }
        if (s.compare(i, 2, "**") == 0 && ((st & BOLD) ? !isSpace(prev) : opens(i, 2))) {
            flush();
            st ^= BOLD;
            i += 2;
            continue;
        }
        if (s.compare(i, 2, "__") == 0 && ((st & BOLD) ? isBoundary(i + 2 < s.size() ? s[i + 2] : 0)
                                                       : isBoundary(prev) && opens(i, 2))) {
            flush();
            st ^= BOLD;
            i += 2;
            continue;
        }
        if (c == '*' && ((st & ITALIC) ? !isSpace(prev) : opens(i, 1))) {
            flush();
            st ^= ITALIC;
            ++i;
            continue;
        }
        // _italique_ seulement entre deux limites de mot (snake_case reste intact)
        if (c == '_' && ((st & ITALIC) ? isBoundary(next) : isBoundary(prev) && opens(i, 1))) {
            flush();
            st ^= ITALIC;
            ++i;
            continue;
        }
        cur += c;
        ++i;
    }
    flush();
    return out;
}

std::string plainText(const std::vector<Span>& spans) {
    std::string out;
    for (const auto& sp : spans) out += sp.text;
    return out;
}

// Séquence ANSI pour un style, par-dessus le style de base du bloc.
std::string sgr(unsigned st, const std::string& base) {
    std::string s = std::string(ansi::reset) + base;
    if (st & BOLD) s += ansi::bold;
    if (st & ITALIC) s += ansi::italic;
    if (st & STRIKE) s += "\x1b[9m";
    if (st & LINK) s += std::string("\x1b[4m") + P.accent;
    if (st & URL) s += P.muted;
    if (st & CODE) s += P.code;
    return s;
}

struct Word {
    std::string text;
    unsigned style;
    bool spaceBefore;
};

std::vector<Word> toWords(const std::vector<Span>& spans) {
    std::vector<Word> words;
    bool space = false;
    for (const auto& sp : spans) {
        std::string w;
        for (char c : sp.text) {
            if (isSpace(c)) {
                if (!w.empty()) words.push_back({w, sp.style, space});
                w.clear();
                space = true;
            } else {
                w += c;
            }
        }
        if (!w.empty()) {
            words.push_back({w, sp.style, space});
            space = false;
        }
    }
    return words;
}

// Coupe des segments stylés en lignes de `width` colonnes.
std::vector<std::string> wrapSpans(const std::vector<Span>& spans, size_t width, const std::string& base) {
    if (width < 8) width = 8;
    std::vector<std::string> lines;
    std::string line;
    size_t col = 0;
    auto newline = [&] {
        lines.push_back(line + ansi::reset);
        line.clear();
        col = 0;
    };
    for (Word w : toWords(spans)) {
        size_t wl = displayLength(w.text);
        const size_t space = (col > 0 && w.spaceBefore) ? 1 : 0;
        if (col > 0 && col + space + wl > width) newline();
        else if (space) {
            line += sgr(0, base) + " ";
            ++col;
        }
        if (wl > width && col > 0) newline();
        while (wl > width) {  // mot plus long que la ligne
            std::string head = utf8Prefix(w.text, width);
            line += sgr(w.style, base) + head;
            col += displayLength(head);
            w.text = w.text.substr(head.size());
            wl = displayLength(w.text);
            newline();
        }
        line += sgr(w.style, base) + w.text;
        col += wl;
    }
    if (!line.empty() || lines.empty()) lines.push_back(line + ansi::reset);
    return lines;
}

// Segments ajustés à exactement `width` colonnes (cellules de tableau). align : 0 gauche, 1 centre, 2 droite.
std::string fitSpans(const std::vector<Span>& spans, size_t width, const std::string& base, int align) {
    const size_t len = displayLength(plainText(spans));
    std::string out;
    if (len <= width) {
        const size_t pad = width - len;
        const size_t left = align == 1 ? pad / 2 : align == 2 ? pad : 0;
        out += sgr(0, base) + std::string(left, ' ');
        for (const auto& sp : spans) out += sgr(sp.style, base) + sp.text;
        return out + sgr(0, base) + std::string(pad - left, ' ') + ansi::reset;
    }
    size_t used = 0;
    for (const auto& sp : spans) {
        if (used + 1 >= width) break;
        std::string t = sp.text;
        if (used + displayLength(t) > width - 1) t = utf8Prefix(t, width - 1 - used);
        out += sgr(sp.style, base) + t;
        used += displayLength(t);
    }
    return out + sgr(0, base) + "…" + std::string(width - 1 - used, ' ') + ansi::reset;
}

// ---------------------------------------------------------------------------
// Coloration syntaxique (simple, ligne par ligne)
// ---------------------------------------------------------------------------
enum class Lang { Plain, CLike, Python, Shell, Data };

Lang langOf(std::string lang) {
    for (auto& ch : lang) ch = (char)std::tolower((unsigned char)ch);
    static const std::set<std::string> clike = {"c",    "cpp",  "c++",    "cc",    "h",     "hpp",  "cs",
                                                "csharp", "java", "js",   "javascript", "jsx", "ts", "tsx",
                                                "typescript", "go", "rust", "rs",  "kotlin", "kt", "swift",
                                                "php",  "dart", "scala",  "cuda",  "glsl",  "hlsl", "zig"};
    static const std::set<std::string> shell = {"bash", "sh",  "shell", "zsh", "console", "cmd",
                                                "bat",  "batch", "powershell", "ps1", "ps", "terminal"};
    static const std::set<std::string> data = {"json", "jsonc", "yaml", "yml", "toml", "ini", "xml", "html", "css"};
    if (clike.count(lang)) return Lang::CLike;
    if (lang == "python" || lang == "py") return Lang::Python;
    if (shell.count(lang)) return Lang::Shell;
    if (data.count(lang)) return Lang::Data;
    return lang.empty() ? Lang::Plain : Lang::CLike;  // langage inconnu : règles génériques
}

const std::set<std::string>& keywords(Lang l) {
    static const std::set<std::string> clike = {
        "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "goto",
        "class", "struct", "enum", "union", "namespace", "using", "public", "private", "protected", "static",
        "const", "constexpr", "auto", "void", "int", "long", "short", "char", "bool", "float", "double",
        "unsigned", "signed", "size_t", "new", "delete", "try", "catch", "throw", "true", "false", "null",
        "nullptr", "this", "template", "typename", "virtual", "override", "final", "inline", "extern",
        "import", "export", "from", "function", "let", "var", "async", "await", "yield", "interface",
        "implements", "extends", "package", "fn", "mut", "impl", "pub", "use", "mod", "match", "where",
        "trait", "self", "Self", "func", "go", "defer", "chan", "map", "type", "range", "string", "val",
        "fun", "when", "object", "def", "undefined", "typeof", "instanceof", "in", "of", "as", "is"};
    static const std::set<std::string> python = {
        "def", "class", "if", "elif", "else", "for", "while", "return", "import", "from", "as", "with",
        "try", "except", "finally", "raise", "lambda", "yield", "pass", "break", "continue", "and", "or",
        "not", "in", "is", "None", "True", "False", "self", "async", "await", "global", "nonlocal",
        "assert", "del", "print", "match", "case"};
    static const std::set<std::string> shell = {
        "if", "then", "else", "elif", "fi", "for", "do", "done", "while", "case", "esac", "function", "echo",
        "cd", "set", "export", "exit", "in", "return", "local", "read", "rem", "goto", "call", "setlocal",
        "endlocal", "mkdir", "del", "copy", "move", "dir", "type", "git", "npm", "cmake", "make", "sudo"};
    static const std::set<std::string> data = {"true", "false", "null", "yes", "no"};
    static const std::set<std::string> none;
    switch (l) {
        case Lang::CLike: return clike;
        case Lang::Python: return python;
        case Lang::Shell: return shell;
        case Lang::Data: return data;
        default: return none;
    }
}

std::string highlight(const std::string& line, Lang lang) {
    const std::string base = P.text;
    if (lang == Lang::Plain) return base + line + ansi::reset;
    const auto& kws = keywords(lang);
    const bool slashComments = lang == Lang::CLike || lang == Lang::Data;
    const bool hashComments = lang == Lang::Python || lang == Lang::Shell || lang == Lang::Data;
    std::string out;
    size_t i = 0;

    const std::string t = trim(line);
    if (lang == Lang::CLike && startsWith(t, "#")) return P.kw + line + ansi::reset;  // préprocesseur
    if (lang == Lang::Shell && (startsWith(t, "::") || startsWith(t, "REM ") || startsWith(t, "rem ")))
        return P.cmt + line + ansi::reset;

    while (i < line.size()) {
        const char c = line[i];
        const char prev = i ? line[i - 1] : 0;
        if ((slashComments && line.compare(i, 2, "//") == 0) || (hashComments && c == '#')) {
            out += P.cmt + line.substr(i);
            break;
        }
        if (slashComments && line.compare(i, 2, "/*") == 0) {
            size_t e = line.find("*/", i + 2);
            size_t end = e == std::string::npos ? line.size() : e + 2;
            out += P.cmt + line.substr(i, end - i);
            i = end;
            continue;
        }
        if (c == '"' || c == '\'' || (c == '`' && lang == Lang::CLike)) {
            size_t j = i + 1;
            while (j < line.size() && line[j] != c) j += line[j] == '\\' ? 2 : 1;
            size_t end = std::min(line.size(), j + 1);
            out += P.str + line.substr(i, end - i);
            i = end;
            continue;
        }
        if (std::isdigit((unsigned char)c) && !std::isalnum((unsigned char)prev) && prev != '_') {
            size_t j = i;
            while (j < line.size() && (std::isalnum((unsigned char)line[j]) || line[j] == '.' || line[j] == '_')) ++j;
            out += P.num + line.substr(i, j - i);
            i = j;
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < line.size() && (std::isalnum((unsigned char)line[j]) || line[j] == '_')) ++j;
            const std::string w = line.substr(i, j - i);
            out += (kws.count(w) ? P.kw : base.c_str()) + w;
            i = j;
            continue;
        }
        out += base;
        out += c;
        ++i;
    }
    return out + ansi::reset;
}

// ---------------------------------------------------------------------------
// Blocs
// ---------------------------------------------------------------------------
bool isFence(const std::string& t) { return startsWith(t, "```") || startsWith(t, "~~~"); }

bool isRule(const std::string& t) {
    if (t.size() < 3) return false;
    const char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    for (char x : t)
        if (x != c && x != ' ') return false;
    return true;
}

bool isTableSeparator(const std::string& line) {
    const std::string t = trim(line);
    if (t.find('-') == std::string::npos || t.find('|') == std::string::npos) return false;
    for (char c : t)
        if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    return true;
}

std::vector<std::string> splitRow(const std::string& line) {
    std::string t = trim(line);
    if (startsWith(t, "|")) t = t.substr(1);
    if (!t.empty() && t.back() == '|') t.pop_back();
    std::vector<std::string> cells;
    std::string cur;
    bool inCode = false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '`') inCode = !inCode;
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
            cur += '|';
            ++i;
        } else if (t[i] == '|' && !inCode) {
            cells.push_back(trim(cur));
            cur.clear();
        } else {
            cur += t[i];
        }
    }
    cells.push_back(trim(cur));
    return cells;
}

struct ListItem {
    size_t indent = 0;
    bool ordered = false;
    std::string number;
    std::string body;
};

bool parseListItem(const std::string& line, ListItem& item) {
    size_t i = 0;
    size_t indent = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) indent += line[i++] == '\t' ? 4 : 1;
    if (i + 1 < line.size() && (line[i] == '-' || line[i] == '*' || line[i] == '+') && line[i + 1] == ' ') {
        item = {indent, false, "", line.substr(i + 2)};
        return true;
    }
    size_t j = i;
    while (j < line.size() && std::isdigit((unsigned char)line[j])) ++j;
    if (j > i && j - i <= 3 && j + 1 < line.size() && (line[j] == '.' || line[j] == ')') && line[j + 1] == ' ') {
        item = {indent, true, line.substr(i, j - i), line.substr(j + 2)};
        return true;
    }
    return false;
}

std::vector<std::string> render(const std::string& text, size_t width);

void renderTable(const std::vector<std::string>& rows, size_t width, std::vector<std::string>& out) {
    std::vector<std::vector<std::string>> cells;
    std::vector<int> align;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r == 1) {  // ligne de séparation : alignements
            for (const auto& c : splitRow(rows[r])) {
                const bool left = startsWith(c, ":"), right = !c.empty() && c.back() == ':';
                align.push_back(left && right ? 1 : right ? 2 : 0);
            }
            continue;
        }
        cells.push_back(splitRow(rows[r]));
    }
    size_t cols = 0;
    for (const auto& row : cells) cols = std::max(cols, row.size());
    if (cols == 0) return;
    align.resize(cols, 0);

    std::vector<size_t> w(cols, 3);
    for (const auto& row : cells)
        for (size_t c = 0; c < row.size(); ++c) w[c] = std::max(w[c], displayLength(plainText(parseInline(row[c]))));
    auto total = [&] {
        size_t t = 1;
        for (size_t x : w) t += x + 3;
        return t;
    };
    while (total() > width) {  // trop large : on réduit la colonne la plus large
        size_t widest = 0;
        for (size_t c = 1; c < cols; ++c)
            if (w[c] > w[widest]) widest = c;
        if (w[widest] <= 3) break;
        --w[widest];
    }

    const std::string b = P.muted;
    auto border = [&](const char* l, const char* m, const char* r) {
        std::string s = b + l;
        for (size_t c = 0; c < cols; ++c) s += repeatStr("─", w[c] + 2) + (c + 1 < cols ? m : r);
        return s + ansi::reset;
    };
    out.push_back(border("┌", "┬", "┐"));
    for (size_t r = 0; r < cells.size(); ++r) {
        std::string line = b + "│" + ansi::reset;
        for (size_t c = 0; c < cols; ++c) {
            const std::string cell = c < cells[r].size() ? cells[r][c] : "";
            const std::string base = r == 0 ? std::string(ansi::bold) + P.text : std::string(P.text);
            line += " " + fitSpans(parseInline(cell), w[c], base, align[c]) + " " + b + "│" + ansi::reset;
        }
        out.push_back(line);
        if (r == 0 && cells.size() > 1) out.push_back(border("├", "┼", "┤"));
    }
    out.push_back(border("└", "┴", "┘"));
}

// Bloc de code, pièce par pièce (utilisé aussi pendant le streaming) : en-tête, lignes, pied.
std::string codeHeader(const std::string& lang, size_t width) {
    const std::string label = lang.empty() ? "" : " " + lang + " ";
    const size_t labelLen = displayLength(label);
    return std::string(P.muted) + "╭─" + label + repeatStr("─", width > labelLen + 2 ? width - labelLen - 2 : 1) +
           ansi::reset;
}

std::vector<std::string> codeLines(std::string line, const std::string& lang, size_t width) {
    const Lang l = langOf(lang);
    const size_t inner = width > 4 ? width - 2 : 2;
    replaceAll(line, "\t", "    ");
    if (line.empty()) return {std::string(P.muted) + "│" + ansi::reset};
    std::vector<std::string> out;
    bool first = true;
    while (!line.empty()) {  // lignes trop longues : suite sur la ligne suivante
        std::string chunk = displayLength(line) > inner ? utf8Prefix(line, inner) : line;
        line = line.substr(chunk.size());
        out.push_back(std::string(P.muted) + (first ? "│ " : "│↳") + ansi::reset + highlight(chunk, l));
        first = false;
    }
    return out;
}

std::string codeFooter(size_t width) {
    return std::string(P.muted) + "╰" + repeatStr("─", width - 1) + ansi::reset;
}

void renderCode(const std::vector<std::string>& code, const std::string& lang, size_t width,
                std::vector<std::string>& out) {
    out.push_back(codeHeader(lang, width));
    for (const auto& line : code)
        for (const auto& l : codeLines(line, lang, width)) out.push_back(l);
    out.push_back(codeFooter(width));
}

std::vector<std::string> render(const std::string& text, size_t width) {
    if (width < 20) width = 20;
    std::vector<std::string> out;
    const auto lines = splitLines(text);
    auto blank = [&] {
        if (!out.empty() && !out.back().empty()) out.push_back("");
    };

    for (size_t i = 0; i < lines.size();) {
        const std::string& raw = lines[i];
        const std::string t = trim(raw);

        if (isFence(t)) {  // bloc de code
            const std::string fence = t.substr(0, 3);
            const std::string lang = trim(t.substr(3));
            std::vector<std::string> code;
            ++i;
            while (i < lines.size() && !startsWith(trim(lines[i]), fence)) code.push_back(lines[i++]);
            if (i < lines.size()) ++i;
            renderCode(code, lang, width, out);
            continue;
        }
        if (t.empty()) {
            blank();
            ++i;
            continue;
        }
        if (isRule(t)) {
            out.push_back(std::string(P.muted) + repeatStr("─", width) + ansi::reset);
            ++i;
            continue;
        }
        if (startsWith(t, "#")) {  // titre
            size_t level = t.find_first_not_of('#');
            if (level != std::string::npos && level <= 6 && t[level] == ' ') {
                const std::string body = trim(t.substr(level));
                std::string base = level == 1   ? std::string(ansi::bold) + P.accent
                                   : level == 2 ? std::string(ansi::bold) + P.text
                                   : level == 3 ? std::string(ansi::bold) + P.text
                                                : std::string(ansi::bold) + P.muted;
                for (const auto& l : wrapSpans(parseInline(body), width, base)) out.push_back(l);
                if (level <= 2) out.push_back("");
                ++i;
                continue;
            }
        }
        if (t.find('|') != std::string::npos && i + 1 < lines.size() && isTableSeparator(lines[i + 1])) {
            std::vector<std::string> rows;
            while (i < lines.size() && trim(lines[i]).find('|') != std::string::npos) rows.push_back(lines[i++]);
            renderTable(rows, width, out);
            continue;
        }
        if (startsWith(t, ">")) {  // citation (rendue récursivement)
            std::string inner;
            while (i < lines.size() && startsWith(trim(lines[i]), ">")) {
                std::string q = trim(lines[i++]).substr(1);
                if (startsWith(q, " ")) q = q.substr(1);
                inner += q + "\n";
            }
            for (const auto& l : render(inner, width - 2))
                out.push_back(std::string(P.accent) + "▎ " + ansi::reset + P.muted + l + ansi::reset);
            continue;
        }
        ListItem item;
        if (parseListItem(raw, item)) {  // élément de liste (+ lignes de suite indentées)
            ++i;
            while (i < lines.size()) {
                const std::string& nx = lines[i];
                ListItem sub;
                if (trim(nx).empty() || parseListItem(nx, sub) || isFence(trim(nx))) break;
                size_t ind = nx.find_first_not_of(" \t");
                if (ind == std::string::npos || ind <= item.indent) break;
                item.body += " " + trim(nx);
                ++i;
            }
            const size_t level = std::min<size_t>(item.indent / 2, 3);
            std::string marker, markerStyled;
            std::string body = item.body;
            if (item.ordered) {
                marker = item.number + ".";
                markerStyled = std::string(P.accent) + marker + ansi::reset;
            } else {
                static const char* bullets[] = {"•", "◦", "▸", "‣"};
                marker = bullets[level];
                markerStyled = std::string(P.muted) + marker + ansi::reset;
            }
            if (startsWith(body, "[ ] ")) {
                body = body.substr(4);
                marker += " ☐";
                markerStyled += std::string(" ") + P.muted + "☐" + ansi::reset;
            } else if (startsWith(body, "[x] ") || startsWith(body, "[X] ")) {
                body = body.substr(4);
                marker += " ☒";
                markerStyled += std::string(" ") + P.ok + "☒" + ansi::reset;
            }
            const size_t pad = level * 2;
            const size_t hang = pad + displayLength(marker) + 1;
            auto wrapped = wrapSpans(parseInline(body), width > hang + 10 ? width - hang : 10, P.text);
            for (size_t k = 0; k < wrapped.size(); ++k)
                out.push_back(k == 0 ? std::string(pad, ' ') + markerStyled + " " + wrapped[k]
                                     : std::string(hang, ' ') + wrapped[k]);
            continue;
        }
        // Paragraphe : chaque ligne du modèle est conservée (retour à la ligne volontaire).
        for (const auto& l : wrapSpans(parseInline(t), width, P.text)) out.push_back(l);
        ++i;
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    while (!out.empty() && out.front().empty()) out.erase(out.begin());
    return out;
}

}  // namespace md
