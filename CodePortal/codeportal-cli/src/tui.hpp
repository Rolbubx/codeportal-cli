// tui.hpp : interface plein écran (FTXUI).
//
// Deux fils :
//   - le fil de l'interface (FTXUI) dessine l'écran et reçoit le clavier et la souris ;
//   - le fil de travail exécute le programme (commandes, agent), comme avant.
//
// Tout ce que le programme écrit avec out() arrive dans le journal affiché à l'écran, couleurs ANSI
// comprises (un petit émulateur gère \r, \n, effacement de ligne, remontée du curseur).
// Les saisies (message, clé masquée, liste de choix, confirmation, question de l'agent) sont des
// demandes que le fil de travail envoie à l'interface ; il attend la réponse.
//
// Disposition :
//   en-tête       ✻ CodePortal · dossier · modèle
//   journal       défilant (molette, PgUp/PgDn), suit le bas tant qu'on n'a pas remonté
//   direct        texte en cours de streaming, ligne de statut animée, barre de progression
//   panneau       question de l'agent, confirmation, liste de choix
//   saisie        cadre arrondi + menu des commandes « / »
//   pied de page  raccourcis utiles · fournisseur et modèle
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util.hpp"

#include "../third_party/ftxui/ftxui.hpp"

namespace tui {

using namespace ftxui;

// ---------------------------------------------------------------------------
// Couleurs : la palette (util.hpp) est faite de séquences ANSI 24 bits ; on les convertit.
// ---------------------------------------------------------------------------
inline Color colorOf(const char* ansi) {
    int r = 200, gr = 200, b = 200, kind = 0;
    if (ansi && sscanf(ansi, "\x1b[%d;2;%d;%d;%dm", &kind, &r, &gr, &b) == 4) return Color::RGB(r, gr, b);
    return Color::Default;
}

// Mémorise la place donnée à un élément par la mise en page (comme reflect(), mais sans l'intersection
// avec le pochoir faite au dessin, qui peut la réduire à rien selon les passes de rendu).
// Si la place change (premier dessin, fenêtre redimensionnée), onChange demande un nouveau dessin.
class BoxSpy : public Node {
public:
    BoxSpy(Element child, Box* box, std::function<void()> onChange)
        : Node(Elements{std::move(child)}), box_(box), onChange_(std::move(onChange)) {}
    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }
    void SetBox(Box box) override {
        const bool changed = box.y_min != box_->y_min || box.y_max != box_->y_max || box.x_max != box_->x_max;
        *box_ = box;
        if (changed && onChange_) onChange_();
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }

private:
    Box* box_;
    std::function<void()> onChange_;
};

inline Element spyBox(Element child, Box* box, std::function<void()> onChange) {
    return std::make_shared<BoxSpy>(std::move(child), box, std::move(onChange));
}

// Une ligne de texte avec séquences SGR (couleurs, gras, italique...) -> élément FTXUI.
inline Element ansiLine(const std::string& line) {
    struct St {
        bool fg = false, bg = false, bold = false, dim = false, italic = false, under = false, inv = false;
        Color fgc, bgc;
    } st;
    Elements parts;
    std::string run;
    auto flush = [&] {
        if (run.empty()) return;
        Element e = text(run);
        if (st.fg) e = e | color(st.fgc);
        if (st.bg) e = e | bgcolor(st.bgc);
        if (st.bold) e = e | bold;
        if (st.dim) e = e | dim;
        if (st.italic) e = e | italic;
        if (st.under) e = e | underlined;
        if (st.inv) e = e | inverted;
        parts.push_back(e);
        run.clear();
    };
    auto basic = [](int n) {  // 30-37 / 90-97
        static const Color table[] = {Color::Black, Color::Red,     Color::Green, Color::Yellow,
                                      Color::Blue,  Color::Magenta, Color::Cyan,  Color::GrayLight};
        static const Color bright[] = {Color::GrayDark,    Color::RedLight,     Color::GreenLight, Color::YellowLight,
                                       Color::BlueLight,   Color::MagentaLight, Color::CyanLight,  Color::White};
        return n >= 90 ? bright[(n - 90) & 7] : table[(n - 30) & 7];
    };
    for (size_t i = 0; i < line.size();) {
        const char c = line[i];
        if (c == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
            size_t j = i + 2;
            while (j < line.size() && !std::isalpha((unsigned char)line[j])) ++j;
            if (j >= line.size()) break;
            if (line[j] == 'm') {
                flush();
                std::vector<int> ps;
                std::string num;
                for (size_t k = i + 2; k <= j; ++k) {
                    if (line[k] == ';' || k == j) {
                        ps.push_back(num.empty() ? 0 : std::atoi(num.c_str()));
                        num.clear();
                    } else {
                        num += line[k];
                    }
                }
                for (size_t k = 0; k < ps.size(); ++k) {
                    const int p = ps[k];
                    if (p == 0) st = St{};
                    else if (p == 1) st.bold = true;
                    else if (p == 2) st.dim = true;
                    else if (p == 3) st.italic = true;
                    else if (p == 4) st.under = true;
                    else if (p == 7) st.inv = true;
                    else if (p == 22) st.bold = st.dim = false;
                    else if (p == 23) st.italic = false;
                    else if (p == 24) st.under = false;
                    else if (p == 27) st.inv = false;
                    else if (p == 39) st.fg = false;
                    else if (p == 49) st.bg = false;
                    else if ((p == 38 || p == 48) && k + 4 < ps.size() && ps[k + 1] == 2) {
                        const Color col = Color::RGB(ps[k + 2], ps[k + 3], ps[k + 4]);
                        if (p == 38) st.fg = true, st.fgc = col;
                        else st.bg = true, st.bgc = col;
                        k += 4;
                    } else if ((p >= 30 && p <= 37) || (p >= 90 && p <= 97)) st.fg = true, st.fgc = basic(p);
                    else if ((p >= 40 && p <= 47) || (p >= 100 && p <= 107)) st.bg = true, st.bgc = basic(p - 10);
                }
            }
            i = j + 1;
            continue;
        }
        if (c == '\t') run += "    ";
        else if ((unsigned char)c >= 0x20 || (unsigned char)c >= 0x80) run += c;
        ++i;
    }
    flush();
    if (parts.empty()) return text("");
    if (parts.size() == 1) return parts[0];
    return hbox(std::move(parts));
}

// ---------------------------------------------------------------------------
// État partagé entre les deux fils (protégé par mu).
// ---------------------------------------------------------------------------
enum class Kind { Prompt, Masked, Pick, Choose, Question };

struct Request {
    Kind kind = Kind::Prompt;
    std::string title;               // question, libellé, titre de la liste
    std::string detail;              // texte secondaire (chemin, commande...)
    std::vector<std::string> items;  // options, éléments de la liste
    std::string current;             // élément actuel (liste)
    bool async = false;              // question de l'agent qui n'arrête pas son travail
    size_t sel = 0;
    std::promise<std::string> answer;
};

struct MenuItem {
    std::string name, args, desc;
};

// Points d'accroche fournis par ui.hpp (commandes, suggestion, modèle...).
struct Hooks {
    std::function<std::vector<MenuItem>(const std::string&)> menu;  // menu « / » pour la saisie actuelle
    std::function<std::string(const std::string&)> argHint;         // aide sur les arguments
    std::function<std::string()> placeholder;                       // suggestion « Try ... »
};

struct PlanStep {
    char state = ' ';  // ' ' à faire, '>' en cours, 'x' fait
    std::string text;
};

struct State {
    std::mutex mu;
    App* app = nullptr;
    std::atomic<bool> running{false};
    std::atomic<bool> redrawPending{false};
    Hooks hooks;
    std::string model, cwd;  // copie publiée par le fil de travail (setInfo)
    bool autoRun = false;

    // Journal (lignes ANSI) et émulateur minimal du terminal.
    std::vector<std::string> lines;
    std::vector<Element> cache;
    size_t row = 0;      // ligne du curseur (== lines.size() : nouvelle ligne)
    std::string cur;     // contenu en cours de la ligne du curseur
    bool curDirty = false;
    std::string esc;     // séquence d'échappement incomplète
    int scroll = 0;      // lignes remontées depuis le bas
    Box logBox;

    // Zone en direct.
    std::vector<std::string> partial;  // lignes ANSI en cours (streaming)
    bool status = false;
    std::string verb;
    std::chrono::steady_clock::time_point statusStart;
    size_t tokens = 0;
    bool watchEsc = false;
    std::atomic<bool> interrupted{false};
    bool busy = false;  // un tour de l'agent est en cours
    std::chrono::steady_clock::time_point busyStart;
    std::vector<PlanStep> plan;

    // Saisie.
    std::wstring buf;
    size_t cursor = 0;
    std::vector<std::wstring> history;
    size_t histPos = 0;
    std::wstring draft;
    size_t menuSel = 0;
    bool menuClosed = false;

    std::shared_ptr<Request> req;                     // demande bloquante du fil de travail
    std::deque<std::shared_ptr<Request>> questions;   // questions de l'agent sans attente
    std::vector<std::string> notes;                   // messages tapés pendant que l'agent travaille
};

inline State& S() {
    static State s;
    return s;
}

inline bool active() { return S().running; }

// Demande un nouveau dessin (sans inonder la file d'événements).
inline void redraw() {
    State& s = S();
    if (!s.app || !s.running) return;
    if (!s.redrawPending.exchange(true)) s.app->PostEvent(Event::Custom);
}

// ---------------------------------------------------------------------------
// Journal : émulateur de terminal réduit au strict nécessaire.
// ---------------------------------------------------------------------------
namespace detail {

inline void storeLocked(State& s, size_t row, const std::string& text) {
    if (row < s.lines.size()) {
        s.lines[row] = text;
        s.cache[row] = nullptr;
    } else {
        s.lines.push_back(text);
        s.cache.push_back(nullptr);
    }
}

inline void trimLocked(State& s) {
    const size_t maxLines = 6000, drop = 1000;
    if (s.lines.size() <= maxLines || s.row < drop) return;
    s.lines.erase(s.lines.begin(), s.lines.begin() + (long)drop);
    s.cache.erase(s.cache.begin(), s.cache.begin() + (long)drop);
    s.row -= drop;
}

inline void csiLocked(State& s, const std::string& params, char final) {
    const int n = params.empty() ? 1 : std::max(1, std::atoi(params.c_str()));
    switch (final) {
        case 'm':  // couleurs : gardées dans le texte
            s.cur += "\x1b[" + params + "m";
            break;
        case 'A':  // remonte
        case 'B': {  // descend
            if (s.curDirty || (s.row == s.lines.size() && !s.cur.empty())) storeLocked(s, s.row, s.cur);
            long r = (long)s.row + (final == 'A' ? -n : n);
            s.row = (size_t)std::clamp<long>(r, 0, (long)s.lines.size());
            s.cur.clear();
            s.curDirty = false;
            break;
        }
        case 'K':  // efface la ligne
            s.cur.clear();
            s.curDirty = true;
            break;
        case 'J':
            if (params == "2") {  // efface l'écran
                s.lines.clear();
                s.cache.clear();
                s.row = 0;
                s.scroll = 0;
            } else if (s.row < s.lines.size()) {  // efface jusqu'à la fin
                s.lines.resize(s.row);
                s.cache.resize(s.row);
            }
            s.cur.clear();
            s.curDirty = false;
            break;
        default:  // H, G... : sans effet sur le journal
            break;
    }
}

inline void writeLocked(State& s, const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        const char c = data[i];
        if (!s.esc.empty()) {
            s.esc += c;
            if (s.esc.size() == 2 && c != '[') {
                s.esc.clear();
                continue;
            }
            if (s.esc.size() > 2 && std::isalpha((unsigned char)c)) {
                csiLocked(s, s.esc.substr(2, s.esc.size() - 3), c);
                s.esc.clear();
            } else if (s.esc.size() > 32) {
                s.esc.clear();
            }
            continue;
        }
        if (c == '\x1b') {
            s.esc = "\x1b";
        } else if (c == '\n') {
            if (s.row >= s.lines.size()) storeLocked(s, s.lines.size(), s.cur);
            else if (s.curDirty) storeLocked(s, s.row, s.cur);
            ++s.row;
            s.cur.clear();
            s.curDirty = false;
            trimLocked(s);
        } else if (c == '\r') {
            s.cur.clear();
        } else {
            s.cur += c;
            s.curDirty = true;
        }
    }
}

}  // namespace detail

// Tampon de flux branché sur out() : tout ce que le programme écrit arrive dans le journal.
class TranscriptBuf : public std::streambuf {
protected:
    std::streamsize xsputn(const char* data, std::streamsize n) override {
        {
            std::lock_guard<std::mutex> lk(S().mu);
            detail::writeLocked(S(), data, (size_t)n);
        }
        redraw();
        return n;
    }
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            const char c = (char)ch;
            xsputn(&c, 1);
        }
        return ch;
    }
};

// Écrit directement dans le journal (depuis le fil de l'interface, verrou déjà pris).
inline void logLocked(const std::string& text) { detail::writeLocked(S(), text.data(), text.size()); }

// ---------------------------------------------------------------------------
// API du fil de travail
// ---------------------------------------------------------------------------
inline void setPartial(std::vector<std::string> lines) {
    {
        std::lock_guard<std::mutex> lk(S().mu);
        S().partial = std::move(lines);
    }
    redraw();
}

inline void startStatus(const std::string& verb, bool watchEsc) {
    {
        std::lock_guard<std::mutex> lk(S().mu);
        S().status = true;
        S().verb = verb;
        S().watchEsc = watchEsc;
        S().tokens = 0;
        S().statusStart = std::chrono::steady_clock::now();
        S().interrupted = false;
    }
    redraw();
}

inline void stopStatus() {
    {
        std::lock_guard<std::mutex> lk(S().mu);
        S().status = false;
    }
    redraw();
}

inline void setTokens(size_t n) {
    std::lock_guard<std::mutex> lk(S().mu);
    S().tokens = n;
}

inline bool interrupted() { return S().interrupted; }
inline void clearInterrupt() { S().interrupted = false; }

inline void setBusy(bool busy) {
    {
        std::lock_guard<std::mutex> lk(S().mu);
        S().busy = busy;
        if (busy) S().busyStart = std::chrono::steady_clock::now();
    }
    redraw();
}

inline void setPlan(std::vector<PlanStep> plan) {
    {
        std::lock_guard<std::mutex> lk(S().mu);
        S().plan = std::move(plan);
    }
    redraw();
}

// Informations affichées dans l'en-tête et le pied de page (copiées : l'interface ne lit pas l'état global).
inline void setInfo(const std::string& model, const std::string& cwd, bool autoRun) {
    {
        std::lock_guard<std::mutex> lk(S().mu);
        S().model = model;
        S().cwd = cwd;
        S().autoRun = autoRun;
    }
    redraw();
}

inline std::vector<std::string> takeNotes() {
    std::lock_guard<std::mutex> lk(S().mu);
    std::vector<std::string> n;
    n.swap(S().notes);
    return n;
}

inline void setHistory(std::vector<std::wstring> h) {
    std::lock_guard<std::mutex> lk(S().mu);
    S().history = std::move(h);
}

inline void addHistory(const std::wstring& w) {
    std::lock_guard<std::mutex> lk(S().mu);
    auto& h = S().history;
    if (h.empty() || h.back() != w) h.push_back(w);
}

// Envoie une demande à l'interface et attend la réponse.
inline std::string request(std::shared_ptr<Request> r) {
    if (!active()) return "";
    auto fut = r->answer.get_future();
    {
        std::lock_guard<std::mutex> lk(S().mu);
        State& s = S();
        if (r->kind != Kind::Prompt) {  // la saisie en cours sert à la demande
            s.buf.clear();
            s.cursor = 0;
        }
        for (size_t i = 0; i < r->items.size(); ++i)
            if (!r->current.empty() && r->items[i] == r->current) r->sel = i;
        s.req = r;
        s.histPos = s.history.size();
        s.menuSel = 0;
        s.menuClosed = false;
    }
    redraw();
    return fut.get();
}

inline std::string prompt() {
    auto r = std::make_shared<Request>();
    r->kind = Kind::Prompt;
    return request(r);
}

inline std::string masked(const std::string& label) {
    auto r = std::make_shared<Request>();
    r->kind = Kind::Masked;
    r->title = label;
    return request(r);
}

inline std::string pick(const std::string& title, const std::vector<std::string>& items, const std::string& current) {
    auto r = std::make_shared<Request>();
    r->kind = Kind::Pick;
    r->title = title;
    r->items = items;
    r->current = current;
    return request(r);
}

// Choix entre quelques options (confirmation). Renvoie l'option choisie, "" si Échap.
inline std::string choose(const std::string& title, const std::string& detail, const std::vector<std::string>& options) {
    auto r = std::make_shared<Request>();
    r->kind = Kind::Choose;
    r->title = title;
    r->detail = detail;
    r->items = options;
    return request(r);
}

// Question de l'agent. Bloquante : renvoie la réponse ("" si l'utilisateur l'ignore).
inline std::string ask(const std::string& question, const std::vector<std::string>& options) {
    auto r = std::make_shared<Request>();
    r->kind = Kind::Question;
    r->title = question;
    r->items = options;
    return request(r);
}

// Question de l'agent sans attente : la réponse arrivera plus tard comme message de l'utilisateur.
inline void askLater(const std::string& question, const std::vector<std::string>& options) {
    auto r = std::make_shared<Request>();
    r->kind = Kind::Question;
    r->title = question;
    r->items = options;
    r->async = true;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        S().questions.push_back(r);
    }
    redraw();
}

inline size_t pendingQuestions() {
    std::lock_guard<std::mutex> lk(S().mu);
    return S().questions.size();
}

inline void quit() {
    if (S().app) S().app->Exit();
}

// ---------------------------------------------------------------------------
// Rendu
// ---------------------------------------------------------------------------
namespace detail {

inline const char* kGlyphs[] = {"·", "✢", "✷", "✶", "✻", "✽", "✻", "✶", "✷", "✢"};

inline long frameNo() {
    using namespace std::chrono;
    return (long)(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 90);
}

inline std::string fmtTokens(size_t n) {
    if (n < 1000) return std::to_string(n);
    char b[32];
    snprintf(b, sizeof b, "%.1fk", n / 1000.0);
    return b;
}

inline std::vector<std::string> chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

// Verbe scintillant : un reflet clair parcourt le mot.
inline Element shimmer(const std::string& word, long pos) {
    Elements el;
    auto cs = chars(word);
    for (long i = 0; i < (long)cs.size(); ++i) {
        const long d = i > pos ? i - pos : pos - i;
        el.push_back(text(cs[i]) | color(colorOf(d == 0 ? P.shimmerHi : d == 1 ? P.shimmerMid : P.accent)));
    }
    return hbox(std::move(el));
}

inline Element muted(const std::string& s) { return text(s) | color(colorOf(P.muted)); }

// Barre de progression indéterminée : un segment lumineux glisse de gauche à droite.
inline Element sweepBar(int width, long frame) {
    width = std::max(width, 10);
    const int seg = std::max(6, width / 6);
    const int span = width + seg;
    const int head = (int)(frame % span);
    std::string before, lit, after;
    for (int x = 0; x < width; ++x) {
        const bool on = x <= head && x > head - seg;
        (on ? lit : (x < head ? before : after)) += "━";
    }
    return hbox({text(before) | color(colorOf(P.border)), text(lit) | color(colorOf(P.accent)),
                 text(after) | color(colorOf(P.border))});
}

inline Element planBar(const std::vector<PlanStep>& plan, int width) {
    size_t done = 0;
    const PlanStep* now = nullptr;
    for (const auto& st : plan) {
        if (st.state == 'x') ++done;
        else if (st.state == '>' && !now) now = &st;
    }
    if (!now)
        for (const auto& st : plan)
            if (st.state != 'x') {
                now = &st;
                break;
            }
    const float ratio = plan.empty() ? 0.f : (float)done / (float)plan.size();
    const int barW = std::clamp(width / 3, 10, 40);
    const int filled = (int)(ratio * barW + 0.5f);
    std::string on, off;
    for (int i = 0; i < barW; ++i) (i < filled ? on : off) += "━";
    Elements row = {text("  "), text(on) | color(colorOf(P.ok)), text(off) | color(colorOf(P.border)),
                    text(" " + std::to_string(done) + "/" + std::to_string(plan.size())) | bold |
                        color(colorOf(P.text))};
    if (done == plan.size()) row.push_back(muted("  all steps done"));
    else if (now) row.push_back(hbox({muted("  ◐ "), text(utf8Prefix(now->text, (size_t)std::max(10, width - barW - 16))) |
                                                         color(colorOf(P.text))}));
    return hbox(std::move(row));
}

// Saisie avec curseur (bloc inversé), défilement horizontal si la ligne est trop longue.
inline Element inputLine(const State& s, bool maskedInput, const std::string& placeholder, bool focused, int width) {
    const size_t maxW = (size_t)std::max(10, width);
    std::wstring shown = s.buf;
    if (maskedInput) shown.assign(s.buf.size(), L'*');
    if (shown.empty()) {
        const std::string ph = utf8Prefix(placeholder, maxW - 1);
        if (!focused || ph.empty()) return muted(ph.empty() ? " " : ph);
        auto cs = chars(ph);
        std::string rest;
        for (size_t i = 1; i < cs.size(); ++i) rest += cs[i];
        return hbox({text(cs[0]) | inverted | color(colorOf(P.muted)), muted(rest)});
    }
    size_t view = 0;
    if (s.cursor + 1 > maxW) view = s.cursor + 1 - maxW;
    const std::wstring visible = shown.substr(view, maxW);
    const size_t c = s.cursor - view;
    const std::string before = toUtf8(visible.substr(0, c));
    const std::string at = c < visible.size() ? toUtf8(visible.substr(c, 1)) : " ";
    const std::string after = c + 1 < visible.size() ? toUtf8(visible.substr(c + 1)) : "";
    const Color tc = colorOf(P.text);
    if (!focused) return text(toUtf8(visible)) | color(tc);
    return hbox({text(before) | color(tc), text(at) | inverted, text(after) | color(tc)});
}

inline Element optionList(const Request& r, int maxRows, bool numbered) {
    Elements rows;
    const size_t n = r.items.size();
    size_t top = 0;
    if (r.sel >= (size_t)maxRows) top = r.sel - (size_t)maxRows + 1;
    for (size_t i = top; i < n && i < top + (size_t)maxRows; ++i) {
        const bool sel = i == r.sel;
        std::string label = (numbered ? std::to_string(i + 1) + ". " : std::string()) + r.items[i];
        Element e = text(label);
        e = sel ? e | bold | color(colorOf(P.accent)) : e | color(colorOf(P.text));
        Elements row = {text(sel ? " ❯ " : "   ") | color(colorOf(P.accent)), e};
        if (!r.current.empty() && r.items[i] == r.current) row.push_back(muted("  (current)"));
        rows.push_back(hbox(std::move(row)));
    }
    if (n > (size_t)maxRows) rows.push_back(muted("   " + std::to_string(r.sel + 1) + "/" + std::to_string(n)));
    return vbox(std::move(rows));
}

}  // namespace detail

// Requête affichée dans le panneau : demande bloquante, sinon première question en attente.
inline Request* panelRequestLocked(State& s) {
    if (s.req && s.req->kind != Kind::Prompt) return s.req.get();
    if (!s.questions.empty()) return s.questions.front().get();
    return nullptr;
}

// Éléments du menu « / » qui correspondent à la saisie.
inline std::vector<MenuItem> menuLocked(State& s) {
    if (!s.req || s.req->kind != Kind::Prompt || s.menuClosed || !s.hooks.menu) return {};
    if (!s.questions.empty()) return {};
    return s.hooks.menu(toUtf8(s.buf));
}

inline Element renderLocked(State& s) {
    using namespace detail;
    const Dimensions dim = Terminal::Size();
    const int W = std::max(40, dim.dimx);
    g_tuiCols = std::max(30, W - 2);
    g_tuiRows = dim.dimy;
    const long frame = frameNo();
    const Color accent = colorOf(P.accent), border = colorOf(P.border), textc = colorOf(P.text);

    // En-tête
    const std::string model = s.model;
    std::string cwd = s.cwd;
    const size_t cwdMax = (size_t)std::max(10, W - (int)displayLength(model) - 22);
    if (displayLength(cwd) > cwdMax) cwd = "…" + cwd.substr(cwd.size() - cwdMax + 1);
    Element header = hbox({text(" ✻ ") | color(accent) | bold, text("CodePortal") | bold | color(textc), muted("  " + cwd),
                           filler(), muted(model + " ")});

    // Journal visible
    const size_t total = s.lines.size() + ((s.row >= s.lines.size() && !s.cur.empty()) ? 1 : 0);
    const int boxH = s.logBox.y_max - s.logBox.y_min + 1;
    const int H = std::max(3, boxH > 0 ? boxH : dim.dimy - 10);
    const int maxScroll = std::max(0, (int)total - H);
    s.scroll = std::clamp(s.scroll, 0, maxScroll);
    const size_t end = total - (size_t)s.scroll;
    const size_t start = end > (size_t)H ? end - (size_t)H : 0;
    Elements logRows;
    for (size_t i = start; i < end; ++i) {
        if (i == s.row && (s.curDirty || i >= s.lines.size())) {
            logRows.push_back(ansiLine(s.cur));
            continue;
        }
        if (!s.cache[i]) s.cache[i] = ansiLine(s.lines[i]);
        logRows.push_back(s.cache[i]);
    }
    if (s.scroll > 0 && !logRows.empty())
        logRows.back() = hbox({filler(), text(" ↓ " + std::to_string(s.scroll) + " more lines · PgDn / End ") |
                                             inverted | color(colorOf(P.muted))});
    App* app = s.app;
    Element log = spyBox(vbox({filler(), vbox(std::move(logRows))}), &s.logBox, [app] {
                      if (app) app->PostEvent(Event::Custom);
                  }) | flex;

    // Zone en direct
    Elements live;
    if (!s.partial.empty()) {
        const size_t maxPartial = (size_t)std::max(4, H / 2);
        const size_t from = s.partial.size() > maxPartial ? s.partial.size() - maxPartial : 0;
        for (size_t i = from; i < s.partial.size(); ++i) live.push_back(ansiLine(s.partial[i]));
    }
    if (s.status) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - s.statusStart).count();
        const std::string verb = s.verb + "…";
        const long span = (long)displayLength(verb) + 8;
        std::string info = "(" + std::to_string(secs) + "s";
        if (s.tokens) info += " · ↓ " + fmtTokens(s.tokens) + " tokens";
        if (s.watchEsc) info += " · esc to interrupt";
        info += ")";
        live.push_back(text(""));
        live.push_back(hbox({text(kGlyphs[(frame / 2) % 10]) | color(accent), text(" "), shimmer(verb, frame % span - 4),
                             text(" "), muted(info)}));
    }
    if (s.busy) {
        if (!s.plan.empty()) live.push_back(planBar(s.plan, W));
        else if (s.status) live.push_back(hbox({text("  "), sweepBar(std::clamp(W / 3, 10, 40), frame / 1)}));
    }

    // Panneau (question, confirmation, liste)
    Elements panel;
    Request* pr = panelRequestLocked(s);
    std::string inputLabel;
    if (pr) {
        const int maxRows = std::clamp(dim.dimy / 3, 3, 10);
        Elements body;
        Color frameColor = accent;
        if (pr->kind == Kind::Question) {
            body.push_back(hbox({text(" ? ") | bold | color(accent), paragraph(pr->title) | bold | color(textc)}));
            if (pr->async) body.push_back(muted("   The agent keeps working while you answer."));
            if (!pr->items.empty()) body.push_back(optionList(*pr, maxRows, true));
            inputLabel = pr->items.empty() ? "your answer" : "or type your own answer";
        } else if (pr->kind == Kind::Choose) {
            frameColor = colorOf(P.num);
            body.push_back(hbox({text(" ▲ ") | bold | color(frameColor), paragraph(pr->title) | bold | color(textc)}));
            if (!pr->detail.empty())
                for (const auto& l : splitLines(pr->detail)) body.push_back(hbox({text("   "), muted(l)}));
            body.push_back(optionList(*pr, maxRows, true));
        } else if (pr->kind == Kind::Pick) {
            body.push_back(hbox({text(" " + pr->title) | bold | color(textc)}));
            Request filtered;
            filtered.current = pr->current;
            const std::string f = toUtf8(s.buf);
            std::vector<size_t> idx;
            for (size_t i = 0; i < pr->items.size(); ++i) {
                std::string a = pr->items[i], b = f;
                for (auto& c : a) c = (char)std::tolower((unsigned char)c);
                for (auto& c : b) c = (char)std::tolower((unsigned char)c);
                if (b.empty() || a.find(b) != std::string::npos) {
                    filtered.items.push_back(pr->items[i]);
                    idx.push_back(i);
                }
            }
            filtered.sel = std::min(pr->sel, filtered.items.empty() ? 0 : filtered.items.size() - 1);
            if (filtered.items.empty()) body.push_back(muted("   no match"));
            else body.push_back(optionList(filtered, maxRows, false));
            inputLabel = "filter";
        } else if (pr->kind == Kind::Masked) {
            body.push_back(hbox({text(" ◆ ") | color(accent), text(pr->title) | bold | color(textc)}));
            body.push_back(muted("   The key is hidden while you type and never shown in the conversation."));
            inputLabel = "key";
        }
        panel.push_back(vbox(std::move(body)) | borderRounded | color(frameColor));
    }

    // Saisie
    const bool promptMode = s.req && s.req->kind == Kind::Prompt;
    const bool typing = (s.req && s.req->kind != Kind::Choose) || s.busy || (pr && pr->kind == Kind::Question);
    std::string placeholder;
    if (pr && pr->kind == Kind::Question) placeholder = pr->items.empty() ? "Type your answer…" : "Enter picks the selected option, or type an answer";
    else if (s.req && s.req->kind == Kind::Masked) placeholder = "Paste the key and press Enter";
    else if (s.req && s.req->kind == Kind::Pick) placeholder = "Type to filter";
    else if (s.req && s.req->kind == Kind::Choose) placeholder = "↑↓ or 1-" + std::to_string(s.req->items.size()) + " then Enter";
    else if (s.busy) placeholder = "Message the agent while it works (sent at its next step)";
    else if (promptMode && s.hooks.placeholder) placeholder = s.hooks.placeholder();
    const bool command = !s.buf.empty() && s.buf[0] == L'/';
    const int inW = W - 8;
    Element prefix = text(s.busy && !s.req ? "» " : "> ") | bold | color(command ? accent : (s.busy && !s.req ? colorOf(P.muted) : textc));
    Element inputRow = hbox({text(" "), prefix,
                             inputLine(s, s.req && s.req->kind == Kind::Masked, placeholder, typing || promptMode, inW) | flex});
    Element inputBox = inputRow | borderRounded | color(pr ? accent : border);
    if (!inputLabel.empty()) inputBox = window(text(" " + inputLabel + " ") | color(colorOf(P.muted)), inputRow, ROUNDED) | color(accent);

    // Menu des commandes ou aide, puis pied de page
    Elements below;
    auto menu = menuLocked(s);
    if (s.menuSel >= menu.size()) s.menuSel = 0;
    for (size_t i = 0; i < menu.size(); ++i) {
        const bool sel = i == s.menuSel;
        const std::string name = menu[i].name + (menu[i].args.empty() ? "" : " " + menu[i].args);
        below.push_back(hbox({text(sel ? "  ❯ " : "    ") | color(accent),
                              text(name) | (sel ? bold : nothing) | color(sel ? accent : textc),
                              text(std::string((size_t)std::max(2, 26 - (int)displayLength(name)), ' ')),
                              text(menu[i].desc) | color(sel ? textc : colorOf(P.muted))}));
    }
    std::string hint = (promptMode && s.hooks.argHint) ? s.hooks.argHint(toUtf8(s.buf)) : "";
    std::string left;
    if (pr && pr->kind == Kind::Question) left = (pr->items.empty() ? "" : "↑↓ choose · ") + std::string("Enter send · Esc skip");
    else if (pr && pr->kind == Kind::Choose) left = "↑↓ choose · Enter confirm · Esc deny";
    else if (pr && pr->kind == Kind::Pick) left = "↑↓ move · type to filter · Enter choose · Esc skip";
    else if (pr && pr->kind == Kind::Masked) left = "Enter save · Esc cancel";
    else if (s.busy) left = "esc to interrupt · PgUp/PgDn scroll";
    else if (!hint.empty()) left = hint;
    else left = "? shortcuts · / commands · PgUp/PgDn scroll";
    std::string right = model;
    if (s.autoRun) right += " · auto-run";
    if (s.busy) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - s.busyStart).count();
        right = "working " + std::to_string(secs) + "s · " + right;
    }
    if (!s.notes.empty()) right = std::to_string(s.notes.size()) + " queued · " + right;
    if (!s.questions.empty() && !(pr && pr->async)) right = "question waiting · " + right;
    if ((int)(displayLength(left) + displayLength(right)) + 6 > W) right.clear();
    Element footer = hbox({text("  "), muted(left), filler(), muted(right + " ")});

    Elements all = {header, separatorLight() | color(border), log};
    for (auto& e : live) all.push_back(e);
    all.push_back(text(""));
    for (auto& e : panel) all.push_back(e);
    all.push_back(inputBox);
    for (auto& e : below) all.push_back(e);
    all.push_back(footer);
    // Thème clair : fond clair sur tout l'écran, quel que soit le fond du terminal.
    int tr = 0, tg = 0, tb = 0, kind = 0;
    const bool light = sscanf(P.text, "\x1b[%d;2;%d;%d;%dm", &kind, &tr, &tg, &tb) == 4 && tr + tg + tb < 300;
    // Couleur du texte par défaut : celle du thème (les enfants colorés gardent la leur).
    Element root = vbox(std::move(all)) | color(textc);
    if (light) root = root | bgcolor(Color::RGB(250, 249, 245));
    return root;
}

// ---------------------------------------------------------------------------
// Clavier
// ---------------------------------------------------------------------------
namespace detail {

inline void resolveLocked(State& s, std::shared_ptr<Request> r, const std::string& answer) {
    if (!r) return;
    if (s.req == r) s.req.reset();
    for (auto it = s.questions.begin(); it != s.questions.end(); ++it)
        if (*it == r) {
            s.questions.erase(it);
            break;
        }
    s.buf.clear();
    s.cursor = 0;
    s.menuClosed = false;
    s.menuSel = 0;
    s.histPos = s.history.size();
    try {
        r->answer.set_value(answer);
    } catch (...) {
    }
}

inline void setBufLocked(State& s, const std::wstring& w) {
    s.buf = w;
    s.cursor = s.buf.size();
}

// Édition commune : caractères, effacement, déplacements. Retourne true si l'événement est traité.
inline bool editLocked(State& s, const Event& e) {
    if (e.is_character()) {
        const std::wstring w = toWide(e.character());
        for (wchar_t c : w) {
            if (c < 32 && c != L'\t') continue;
            s.buf.insert(s.buf.begin() + (long)s.cursor, c == L'\t' ? L' ' : c);
            ++s.cursor;
        }
        s.menuClosed = false;
        s.menuSel = 0;
        return true;
    }
    if (e == Event::Backspace) {
        if (s.cursor > 0) s.buf.erase(--s.cursor, 1);
        s.menuClosed = false;
        return true;
    }
    if (e == Event::Delete) {
        if (s.cursor < s.buf.size()) s.buf.erase(s.cursor, 1);
        return true;
    }
    if (e == Event::ArrowLeft) {
        if (s.cursor > 0) --s.cursor;
        return true;
    }
    if (e == Event::ArrowRight) {
        if (s.cursor < s.buf.size()) ++s.cursor;
        return true;
    }
    if (e == Event::Home || e == Event::CtrlA) {
        s.cursor = 0;
        return true;
    }
    if (e == Event::End || e == Event::CtrlE) {
        s.cursor = s.buf.size();
        return true;
    }
    if (e == Event::CtrlU) {
        setBufLocked(s, L"");
        return true;
    }
    if (e == Event::CtrlW) {
        size_t p = s.cursor;
        while (p > 0 && s.buf[p - 1] == L' ') --p;
        while (p > 0 && s.buf[p - 1] != L' ') --p;
        s.buf.erase(p, s.cursor - p);
        s.cursor = p;
        return true;
    }
    if (e == Event::ArrowLeftCtrl) {
        while (s.cursor > 0 && s.buf[s.cursor - 1] == L' ') --s.cursor;
        while (s.cursor > 0 && s.buf[s.cursor - 1] != L' ') --s.cursor;
        return true;
    }
    if (e == Event::ArrowRightCtrl) {
        while (s.cursor < s.buf.size() && s.buf[s.cursor] != L' ') ++s.cursor;
        while (s.cursor < s.buf.size() && s.buf[s.cursor] == L' ') ++s.cursor;
        return true;
    }
    return false;
}

inline void historyLocked(State& s, int dir) {
    if (dir < 0 && s.histPos > 0) {
        if (s.histPos == s.history.size()) s.draft = s.buf;
        setBufLocked(s, s.history[--s.histPos]);
        s.menuClosed = true;
    } else if (dir > 0 && s.histPos < s.history.size()) {
        ++s.histPos;
        setBufLocked(s, s.histPos == s.history.size() ? s.draft : s.history[s.histPos]);
        s.menuClosed = true;
    }
}

// Message tapé pendant que l'agent travaille : mis de côté et affiché dans le journal.
inline void queueNoteLocked(State& s, const std::string& note) {
    s.notes.push_back(note);
    logLocked(std::string("\n") + P.muted + "» " + note + ansi::reset + "\n" + P.muted +
              "  ⎿  queued: the agent reads it at its next step" + ansi::reset + "\n");
}

}  // namespace detail

inline bool onEvent(State& s, Event e) {
    using namespace detail;
    if (e == Event::Custom) return true;  // demande de dessin (FTXUI 7 ne redessine que si l'événement est traité)
    const int page = std::max(3, s.logBox.y_max - s.logBox.y_min - 1);

    // Défilement du journal
    if (e.is_mouse()) {
        auto& m = e.mouse();
        if (m.button == Mouse::WheelUp) s.scroll += 3;
        else if (m.button == Mouse::WheelDown) s.scroll = std::max(0, s.scroll - 3);
        return true;
    }
    if (e == Event::PageUp) {
        s.scroll += page;
        return true;
    }
    if (e == Event::PageDown) {
        s.scroll = std::max(0, s.scroll - page);
        return true;
    }

    Request* pr = panelRequestLocked(s);
    std::shared_ptr<Request> prShared;
    if (pr) prShared = (s.req && s.req.get() == pr) ? s.req : s.questions.front();
    const bool promptMode = s.req && s.req->kind == Kind::Prompt;

    // Ctrl+C : vide la ligne, sinon annule / interrompt, sinon quitte.
    if (e == Event::CtrlC) {
        if (!s.buf.empty()) setBufLocked(s, L"");
        else if (pr) resolveLocked(s, prShared, "");
        else if (s.busy) s.interrupted = true;
        else if (s.app) s.app->Exit();
        return true;
    }

    // Panneau : question de l'agent
    if (pr && pr->kind == Kind::Question) {
        if (e == Event::Escape) {
            const bool async = pr->async;
            const std::string q = pr->title;
            resolveLocked(s, prShared, "");
            if (async) queueNoteLocked(s, "(I skipped your question: " + q + ")");
            return true;
        }
        if ((e == Event::ArrowUp || e == Event::ArrowDown) && !pr->items.empty() && s.buf.empty()) {
            const size_t n = pr->items.size();
            pr->sel = e == Event::ArrowUp ? (pr->sel + n - 1) % n : (pr->sel + 1) % n;
            return true;
        }
        if (s.buf.empty() && e.is_character() && !pr->items.empty()) {
            const std::string ch = e.character();
            if (ch.size() == 1 && ch[0] >= '1' && ch[0] <= '9' && (size_t)(ch[0] - '1') < pr->items.size()) {
                pr->sel = (size_t)(ch[0] - '1');
                return true;
            }
        }
        if (e == Event::Return) {
            std::string answer = trim(toUtf8(s.buf));
            if (answer.empty() && !pr->items.empty()) answer = pr->items[pr->sel];
            if (answer.empty()) return true;
            const bool async = pr->async;
            const std::string q = pr->title;
            logLocked(std::string("\n") + P.accent + "? " + ansi::reset + P.muted + q + ansi::reset + "\n" + P.muted +
                      "  ⎿  " + ansi::reset + P.text + answer + ansi::reset + "\n");
            resolveLocked(s, prShared, answer);
            if (async) {
                const std::string note = "Answer to your question \"" + q + "\": " + answer;
                if (s.req && s.req->kind == Kind::Prompt && !s.busy) resolveLocked(s, s.req, note);
                else s.notes.push_back(note);
            }
            return true;
        }
        return editLocked(s, e);
    }

    // Panneau : confirmation
    if (pr && pr->kind == Kind::Choose) {
        const size_t n = pr->items.size();
        if (e == Event::Escape) {
            resolveLocked(s, prShared, "");
            return true;
        }
        if (e == Event::ArrowUp || e == Event::ArrowLeft) pr->sel = (pr->sel + n - 1) % n;
        else if (e == Event::ArrowDown || e == Event::ArrowRight || e == Event::Tab) pr->sel = (pr->sel + 1) % n;
        else if (e == Event::Return) resolveLocked(s, prShared, pr->items[pr->sel]);
        else if (e.is_character()) {
            const std::string ch = e.character();
            if (ch.size() == 1 && ch[0] >= '1' && ch[0] <= '9' && (size_t)(ch[0] - '1') < n) {
                resolveLocked(s, prShared, pr->items[(size_t)(ch[0] - '1')]);
            } else if (ch == "y" || ch == "Y" || ch == "n" || ch == "N") {
                for (const auto& it : pr->items)
                    if (!it.empty() && std::tolower((unsigned char)it[0]) == std::tolower((unsigned char)ch[0])) {
                        resolveLocked(s, prShared, it);
                        break;
                    }
            }
        }
        return true;
    }

    // Panneau : liste de choix (le texte tapé filtre la liste)
    if (pr && pr->kind == Kind::Pick) {
        std::vector<size_t> idx;
        std::string f = toUtf8(s.buf);
        for (auto& c : f) c = (char)std::tolower((unsigned char)c);
        for (size_t i = 0; i < pr->items.size(); ++i) {
            std::string a = pr->items[i];
            for (auto& c : a) c = (char)std::tolower((unsigned char)c);
            if (f.empty() || a.find(f) != std::string::npos) idx.push_back(i);
        }
        if (e == Event::Escape) {
            resolveLocked(s, prShared, "");
            return true;
        }
        if (e == Event::ArrowUp) {
            if (pr->sel > 0) --pr->sel;
            return true;
        }
        if (e == Event::ArrowDown) {
            if (pr->sel + 1 < idx.size()) ++pr->sel;
            return true;
        }
        if (e == Event::Return) {
            if (!idx.empty()) resolveLocked(s, prShared, pr->items[idx[std::min(pr->sel, idx.size() - 1)]]);
            return true;
        }
        if (editLocked(s, e)) {
            pr->sel = 0;
            return true;
        }
        return false;
    }

    // Panneau : clé masquée
    if (pr && pr->kind == Kind::Masked) {
        if (e == Event::Escape) resolveLocked(s, prShared, "");
        else if (e == Event::Return) resolveLocked(s, prShared, trim(toUtf8(s.buf)));
        else editLocked(s, e);
        return true;
    }

    // Saisie principale (au repos) ou message pour l'agent (pendant qu'il travaille)
    auto menu = menuLocked(s);
    if (e == Event::Escape) {
        if (!menu.empty()) s.menuClosed = true;
        else if ((s.busy || s.status) && s.buf.empty()) s.interrupted = true;
        else setBufLocked(s, L"");
        return true;
    }
    if (e == Event::ArrowUp) {
        if (!menu.empty()) s.menuSel = (s.menuSel + menu.size() - 1) % menu.size();
        else historyLocked(s, -1);
        return true;
    }
    if (e == Event::ArrowDown) {
        if (!menu.empty()) s.menuSel = (s.menuSel + 1) % menu.size();
        else historyLocked(s, +1);
        return true;
    }
    if (e == Event::Tab) {
        if (!menu.empty()) setBufLocked(s, toWide(menu[s.menuSel].name + (menu[s.menuSel].args.empty() ? "" : " ")));
        return true;
    }
    if (e == Event::End && s.buf.empty()) {
        s.scroll = 0;
        return true;
    }
    if (e == Event::Return) {
        std::string line = trim(toUtf8(s.buf));
        if (promptMode && !menu.empty() && line != menu[s.menuSel].name) {  // Entrée dans le menu : complète
            const MenuItem& m = menu[s.menuSel];
            setBufLocked(s, toWide(m.name));
            if (!m.args.empty() && m.args[0] == '<') {  // argument obligatoire : on le laisse taper
                setBufLocked(s, s.buf + L" ");
                return true;
            }
            line = m.name;
        }
        s.scroll = 0;
        if (promptMode) {
            resolveLocked(s, s.req, line);
        } else if (!line.empty()) {  // l'agent (ou une commande) travaille : message mis de côté
            if (line[0] == '/') {
                logLocked(std::string(P.err) + "✗ " + ansi::reset + P.err + "Commands wait until the agent is done." +
                          ansi::reset + "\n");
                return true;
            }
            const std::wstring w = s.buf;
            if (s.history.empty() || s.history.back() != w) s.history.push_back(w);
            setBufLocked(s, L"");
            queueNoteLocked(s, line);
        }
        return true;
    }
    if (e == Event::Character('?') && s.buf.empty() && promptMode) {
        resolveLocked(s, s.req, "?");
        return true;
    }
    return editLocked(s, e);
}

// ---------------------------------------------------------------------------
// Lancement : l'interface tourne sur le fil principal, `work` sur un fil de travail.
// ---------------------------------------------------------------------------

// Ctrl+C : Windows en fait un signal (qui terminerait le programme) au lieu d'une touche.
// On le transforme en événement clavier : vider la ligne, interrompre l'agent ou quitter.
inline BOOL WINAPI onConsoleCtrl(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    if (S().app && S().running) S().app->PostEvent(Event::CtrlC);
    return TRUE;
}

inline void run(Hooks hooks, const std::function<void()>& work) {
    State& s = S();
    s.hooks = std::move(hooks);

    // Taille de secours quand la sortie n'est pas une console (redirigée) : celle de la console.
    {
        HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (con != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(con, &info))
            Terminal::SetFallbackSize({info.srWindow.Right - info.srWindow.Left + 1, info.srWindow.Bottom - info.srWindow.Top + 1});
        if (con != INVALID_HANDLE_VALUE) CloseHandle(con);
    }

    auto app = App::Fullscreen();
    app.ForceHandleCtrlC(false);
    s.app = &app;

    static TranscriptBuf buf;
    std::streambuf* old = out().rdbuf(&buf);
    s.running = true;

    auto root = Renderer([&] {
        std::lock_guard<std::mutex> lk(s.mu);
        s.redrawPending = false;
        return renderLocked(s);
    });
    auto component = CatchEvent(root, [&](Event e) {
        std::lock_guard<std::mutex> lk(s.mu);
        return onEvent(s, e);
    });

    // Animation (étoile, reflet, barre) tant que l'agent travaille.
    std::thread ticker([&] {
        while (s.running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(90));
            bool animate;
            {
                std::lock_guard<std::mutex> lk(s.mu);
                animate = s.status || s.busy;
            }
            if (animate) app.PostEvent(Event::Custom);
        }
    });
    std::thread worker([&] {
        work();
        app.Exit();
    });
    worker.detach();
    // Installé une fois la boucle lancée : passe avant le gestionnaire de signaux posé par FTXUI.
    app.Post([] { SetConsoleCtrlHandler(onConsoleCtrl, TRUE); });

    app.Loop(component);

    s.running = false;
    SetConsoleCtrlHandler(onConsoleCtrl, FALSE);
    ticker.join();
    out().rdbuf(old);
    s.app = nullptr;
}

}  // namespace tui
