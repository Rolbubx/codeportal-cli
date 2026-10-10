// live.hpp : affichage en direct, comme Claude Code.
//
//   - Ligne de statut animée : ✻ Pondering… (12s · ↓ 1.2k tokens · esc to interrupt)
//     L'étoile tourne, le verbe scintille, Échap interrompt la génération.
//   - Écriture au fil du streaming : la réflexion (✻ Thinking…) et la réponse (⏺ ...) s'affichent
//     au fur et à mesure dans la zone en direct, puis sont versées dans le journal une fois terminées.
//
// Le dessin lui-même est fait par l'interface (tui.hpp) ; LiveView ne fait que la tenir à jour.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "tui.hpp"
#include "util.hpp"

const std::vector<std::string>& spinnerVerbs() {
    static const std::vector<std::string> verbs = {
        "Thinking",    "Pondering",  "Musing",    "Brewing",   "Cogitating", "Crafting",
        "Noodling",    "Percolating", "Ruminating", "Simmering", "Conjuring",  "Tinkering",
        "Synthesizing", "Mulling",   "Wrangling", "Untangling", "Computing",  "Portaling"};
    return verbs;
}

std::string randomVerb() {
    static std::mt19937 rng{std::random_device{}()};
    const auto& v = spinnerVerbs();
    return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng)];
}

std::string formatTokens(size_t n) {
    if (n < 1000) return std::to_string(n);
    char buf[32];
    snprintf(buf, sizeof buf, "%.1fk", n / 1000.0);
    return buf;
}

std::vector<std::string> utf8Chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        size_t len = 1;
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

// Texte « scintillant » : un reflet clair parcourt le mot. `pos` avance à chaque image.
std::string shimmerText(const std::string& text, long pos, const char* base) {
    std::string out;
    auto chars = utf8Chars(text);
    for (long i = 0; i < (long)chars.size(); ++i) {
        long d = i > pos ? i - pos : pos - i;
        out += (d == 0 ? P.shimmerHi : d == 1 ? P.shimmerMid : base);
        out += chars[i];
    }
    return out + ansi::reset;
}

enum class Block { Thinking, Answer };

class LiveView {
public:
    static LiveView& get() {
        static LiveView v;
        return v;
    }

    // Nouvel affichage (début d'une étape de l'agent).
    void reset() {
        inBlock_ = mdMode_ = false;
        text_.clear();
        tui::setPartial({});
        tui::clearInterrupt();
        tui::setTokens(0);
    }

    // ---- ligne de statut ----------------------------------------------------
    void startStatus(const std::string& verb, bool watchEsc) { tui::startStatus(verb, watchEsc); }
    void stopStatus() { tui::stopStatus(); }
    void setTokens(size_t n) { tui::setTokens(n); }
    bool interrupted() const { return tui::interrupted(); }

    // ---- blocs de texte brut (réflexion) ------------------------------------
    void beginBlock(Block kind) {
        out() << "\n";  // ligne vide entre deux blocs
        kind_ = kind;
        inBlock_ = true;
        mdMode_ = false;
        text_.clear();
        showText();
    }

    void write(const std::string& text) {
        if (!inBlock_ || mdMode_) return;
        text_ += text;
        showText();
    }

    // ---- bloc Markdown : lignes déjà mises en forme + ligne en cours redessinée ----
    // Chaque ligne terminée est versée dans le journal (commitLines). La ligne encore incomplète
    // est affichée dans la zone en direct par setPartial, et remplacée à chaque nouveau morceau.
    void beginMarkdown() {
        out() << "\n";
        kind_ = Block::Answer;
        inBlock_ = true;
        mdMode_ = true;
        mdFirst_ = true;
        tui::setPartial({});
    }

    void commitLines(const std::vector<std::string>& lines) {
        if (!mdMode_) return;
        for (const auto& l : lines) {
            out() << prefix(mdFirst_) << l << ansi::reset << "\n";
            mdFirst_ = false;
        }
        out() << std::flush;
    }

    void setPartial(const std::vector<std::string>& lines) {
        if (!mdMode_) return;
        std::vector<std::string> shown;
        for (size_t i = 0; i < lines.size(); ++i) shown.push_back(prefix(mdFirst_ && i == 0) + lines[i] + ansi::reset);
        tui::setPartial(std::move(shown));
    }

    void endBlock() {
        if (!inBlock_) return;
        if (!mdMode_) {  // la réflexion terminée part dans le journal
            for (const auto& l : textLines()) out() << l << "\n";
            out() << std::flush;
        }
        tui::setPartial({});
        inBlock_ = mdMode_ = false;
        text_.clear();
    }

    bool inBlock() const { return inBlock_; }

private:
    Block kind_ = Block::Answer;
    bool inBlock_ = false;
    bool mdMode_ = false;
    bool mdFirst_ = true;  // la prochaine ligne affichée est la première du bloc (⏺)
    std::string text_;     // texte brut du bloc en cours

    std::string prefix(bool first) const {
        return first ? std::string(P.text) + "⏺" + ansi::reset + " " : std::string("  ");
    }

    size_t lineWidth() const { return std::max<size_t>(30, std::min<size_t>(consoleWidth() - 4, 110)); }

    // Bloc brut mis en forme : en-tête, paragraphes coupés à la largeur, style du bloc.
    std::vector<std::string> textLines() const {
        std::vector<std::string> out;
        const std::string style = kind_ == Block::Thinking ? std::string(P.muted) + ansi::italic : std::string(P.text);
        if (kind_ == Block::Thinking) out.push_back(std::string(P.muted) + ansi::italic + "✻ Thinking…" + ansi::reset);
        bool first = kind_ != Block::Thinking;
        bool blank = false;
        for (const auto& para : splitLines(text_)) {
            if (trim(para).empty()) {
                blank = true;
                continue;
            }
            if (blank && out.size() > 1) out.push_back("");
            blank = false;
            for (const auto& l : wrapWords(para, lineWidth())) {
                out.push_back((first ? std::string(P.text) + "⏺" + ansi::reset + " " : std::string("  ")) + style + l +
                              ansi::reset);
                first = false;
            }
        }
        return out;
    }

    void showText() { tui::setPartial(textLines()); }
};

// Spinner simple (vérification de clé, liste des modèles, exécution d'outils).
class Spinner {
public:
    explicit Spinner(std::string label, bool watchEsc = false) : label_(std::move(label)), watchEsc_(watchEsc) {}
    ~Spinner() { stop(); }

    void start() {
        LiveView::get().startStatus(label_, watchEsc_);
        started_ = true;
    }

    void stop() {
        if (!started_) return;
        LiveView::get().stopStatus();
        started_ = false;
    }

private:
    std::string label_;
    bool watchEsc_ = false;
    bool started_ = false;
};
