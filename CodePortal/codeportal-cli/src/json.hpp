// json.hpp : écriture de chaînes JSON et parseur JSON minimal (réponses des API).
#pragma once

#include <cctype>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// Remplace toute séquence UTF-8 invalide par « � » (U+FFFD). Une seule séquence invalide suffit
// pour qu'une API refuse toute la requête JSON (« invalid unicode code point »).
std::string sanitizeUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool ok = len > 0 && i + len <= s.size();
        for (size_t k = 1; ok && k < len; ++k) ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
        if (ok && len == 2) ok = c >= 0xC2;  // pas d'encodage trop long
        if (ok && len == 3) {
            const unsigned cp = ((c & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) | ((unsigned char)s[i + 2] & 0x3Fu);
            ok = cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF);
        }
        if (ok && len == 4) {
            const unsigned cp = ((c & 0x07u) << 18) | (((unsigned char)s[i + 1] & 0x3Fu) << 12) |
                                (((unsigned char)s[i + 2] & 0x3Fu) << 6) | ((unsigned char)s[i + 3] & 0x3Fu);
            ok = cp >= 0x10000 && cp <= 0x10FFFF;
        }
        if (ok) {
            out.append(s, i, len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

bool isValidUtf8(const std::string& s) { return sanitizeUtf8(s) == s; }

std::string jsonStr(const std::string& raw) {
    const std::string s = sanitizeUtf8(raw);
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += (char)c;
                }
        }
    }
    return o + "\"";
}

struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::string> keys;
    std::vector<JVal> vals;

    const JVal* find(const std::string& k) const {
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == k) return &vals[i];
        return nullptr;
    }
};

// Réécrit une valeur JSON en texte (utilisé pour renvoyer les arguments d'outils aux API).
std::string toJson(const JVal& v) {
    switch (v.type) {
        case JVal::NUL: return "null";
        case JVal::BOOL:
        case JVal::NUM: return v.str;
        case JVal::STR: return jsonStr(v.str);
        case JVal::ARR: {
            std::string o = "[";
            for (size_t i = 0; i < v.arr.size(); ++i) o += (i ? "," : "") + toJson(v.arr[i]);
            return o + "]";
        }
        case JVal::OBJ: {
            std::string o = "{";
            for (size_t i = 0; i < v.keys.size(); ++i) o += (i ? "," : "") + jsonStr(v.keys[i]) + ":" + toJson(v.vals[i]);
            return o + "}";
        }
    }
    return "null";
}

class JsonParser {
public:
    explicit JsonParser(std::string s) : s_(std::move(s)) {}

    bool parse(JVal& out) { return value(out); }

private:
    std::string s_;
    size_t p_ = 0;

    void skipWs() {
        while (p_ < s_.size() && std::isspace((unsigned char)s_[p_])) ++p_;
    }

    bool value(JVal& v) {
        skipWs();
        if (p_ >= s_.size()) return false;
        char c = s_[p_];
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v.type = JVal::STR; return parseString(v.str); }
        if (s_.compare(p_, 4, "true") == 0) { p_ += 4; v.type = JVal::BOOL; v.str = "true"; return true; }
        if (s_.compare(p_, 5, "false") == 0) { p_ += 5; v.type = JVal::BOOL; v.str = "false"; return true; }
        if (s_.compare(p_, 4, "null") == 0) { p_ += 4; v.type = JVal::NUL; return true; }
        if (c == '-' || std::isdigit((unsigned char)c)) {
            size_t start = p_;
            while (p_ < s_.size() && (std::isdigit((unsigned char)s_[p_]) || s_[p_] == '-' ||
                                      s_[p_] == '+' || s_[p_] == '.' || s_[p_] == 'e' || s_[p_] == 'E'))
                ++p_;
            v.type = JVal::NUM;
            v.str = s_.substr(start, p_ - start);
            return true;
        }
        return false;
    }

    unsigned hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4 && p_ < s_.size(); ++i) {
            char h = s_[p_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= h - '0';
            else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
        }
        return v;
    }

    static void appendUtf8(std::string& o, unsigned cp) {
        if (cp < 0x80) {
            o += (char)cp;
        } else if (cp < 0x800) {
            o += (char)(0xC0 | (cp >> 6));
            o += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += (char)(0xE0 | (cp >> 12));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        } else {
            o += (char)(0xF0 | (cp >> 18));
            o += (char)(0x80 | ((cp >> 12) & 0x3F));
            o += (char)(0x80 | ((cp >> 6) & 0x3F));
            o += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool parseString(std::string& out) {
        ++p_;  // guillemet ouvrant
        while (p_ < s_.size()) {
            char c = s_[p_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p_ >= s_.size()) return false;
            char e = s_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.compare(p_, 2, "\\u") == 0) {
                        p_ += 2;
                        unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool object(JVal& v) {
        v.type = JVal::OBJ;
        ++p_;
        skipWs();
        if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
        for (;;) {
            skipWs();
            if (p_ >= s_.size() || s_[p_] != '"') return false;
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (p_ >= s_.size() || s_[p_] != ':') return false;
            ++p_;
            JVal child;
            if (!value(child)) return false;
            v.keys.push_back(key);
            v.vals.push_back(std::move(child));
            skipWs();
            if (p_ >= s_.size()) return false;
            if (s_[p_] == ',') { ++p_; continue; }
            if (s_[p_] == '}') { ++p_; return true; }
            return false;
        }
    }

    bool array(JVal& v) {
        v.type = JVal::ARR;
        ++p_;
        skipWs();
        if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
        for (;;) {
            JVal child;
            if (!value(child)) return false;
            v.arr.push_back(std::move(child));
            skipWs();
            if (p_ >= s_.size()) return false;
            if (s_[p_] == ',') { ++p_; continue; }
            if (s_[p_] == ']') { ++p_; return true; }
            return false;
        }
    }
};
