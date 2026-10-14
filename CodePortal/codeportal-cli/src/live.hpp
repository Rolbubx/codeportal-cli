// live.hpp : affichage en direct, comme Claude Code.
//
//   - Ligne de statut animée : ✻ Pondering… (12s · ↓ 1.2k tokens · esc to interrupt)
//     L'étoile tourne, le verbe scintille, Échap interrompt la génération.
//   - Écriture au fil du streaming : la réflexion (✻ Thinking…) et la réponse (⏺ ...) s'affichent
//     mot par mot, avec retour à la ligne propre, la ligne de statut restant toujours en dessous.
//
// Toutes les écritures passent par un mutex : le fil du spinner et le fil principal ne se
// marchent jamais dessus.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

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

    ~LiveView() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    // Nouvel affichage : on suppose le curseur en début de ligne.
    void reset() {
        std::lock_guard<std::mutex> lk(mu_);
        col_ = 0;
        statusDrawn_ = statusBelow_ = inBlock_ = mdMode_ = false;
        partialLines_ = 0;
        interrupted_ = false;
        tokens_ = 0;
    }

    // ---- ligne de statut ----------------------------------------------------
    void startStatus(const std::string& verb, bool watchEsc) {
        stopStatus();
        {
            std::lock_guard<std::mutex> lk(mu_);
            verb_ = verb;
            watchEsc_ = watchEsc && !g_headless;  // pas de clavier en mode headless
            interrupted_ = false;
            tokens_ = 0;
            start_ = std::chrono::steady_clock::now();
            running_ = true;
            drawStatusLocked();
        }
        thread_ = std::thread([this] { loop(); });
    }

    void stopStatus() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lk(mu_);
        clearStatusLocked();
        std::cout << std::flush;
    }

    void setTokens(size_t n) { tokens_ = n; }
    // Vrai après Échap ou après une demande d'arrêt venant de l'API (requestCancel).
    bool interrupted() const { return interrupted_ || cancelRequested_; }
    void requestCancel() { cancelRequested_ = true; }
    void clearCancel() { cancelRequested_ = false; }
    bool cancelRequested() const { return cancelRequested_; }

    // Ctrl+O pendant que l'agent travaille : `f` est appelée depuis le fil du spinner, hors du verrou,
    // sans interrompre le flux ni l'outil en cours.
    void setExpandHandler(std::function<void()> f) { onExpand_ = std::move(f); }

    // Écrit hors du flux (détail d'une sortie demandé avec Ctrl+O). La ligne de statut est effacée puis
    // redessinée, et le texte en cours de streaming reprend à la ligne suivante.
    void writeAside(const std::function<void()>& f) {
        std::lock_guard<std::mutex> lk(mu_);
        clearStatusLocked();
        erasePartialLocked();
        const bool wordBlock = inBlock_ && !mdMode_;
        if (col_ > 0) std::cout << "\n";
        f();
        if (wordBlock) {
            std::cout << "  ";
            col_ = 2;
        } else {
            col_ = 0;
        }
        restoreStatusLocked();
    }

    // ---- blocs de texte en direct -----------------------------------------
    void beginBlock(Block kind) {
        std::lock_guard<std::mutex> lk(mu_);
        clearStatusLocked();
        if (col_ > 0) std::cout << "\n";
        std::cout << "\n";  // ligne vide entre deux blocs
        col_ = 0;
        kind_ = kind;
        inBlock_ = true;
        hasText_ = false;
        pendingSpace_ = false;
        pendingNL_ = 0;
        blockNewlines_ = 0;
        word_.clear();
        if (kind == Block::Thinking) {
            std::cout << P.muted << ansi::italic << "✻ Thinking…" << ansi::reset << "\n  ";
        } else {
            std::cout << P.text << "⏺" << ansi::reset << " ";
        }
        col_ = 2;
        restoreStatusLocked();
    }

    void write(const std::string& text) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!inBlock_) return;
        clearStatusLocked();
        for (char c : text) {
            if (c == '\r') continue;
            if (c == '\n') {
                flushWordLocked();
                if (hasText_) ++pendingNL_;
                pendingSpace_ = false;
            } else if (c == ' ' || c == '\t') {
                flushWordLocked();
                if (hasText_ && pendingNL_ == 0) pendingSpace_ = true;
            } else {
                word_ += c;
            }
        }
        restoreStatusLocked();
    }

    // ---- bloc Markdown : lignes déjà mises en forme + ligne en cours redessinée ----
    // Chaque ligne terminée est rendue une fois pour toutes (commitLines). La ligne encore incomplète
    // est affichée par setPartial, puis effacée et redessinée à chaque nouveau morceau.
    void beginMarkdown() {
        std::lock_guard<std::mutex> lk(mu_);
        clearStatusLocked();
        if (col_ > 0) std::cout << "\n";
        std::cout << "\n";  // ligne vide entre deux blocs
        col_ = 0;
        kind_ = Block::Answer;
        inBlock_ = true;
        mdMode_ = true;
        mdFirst_ = true;
        partialLines_ = 0;
        blockNewlines_ = 0;
        restoreStatusLocked();
    }

    void commitLines(const std::vector<std::string>& lines) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!mdMode_) return;
        clearStatusLocked();
        erasePartialLocked();
        for (const auto& l : lines) {
            std::cout << mdPrefixLocked(mdFirst_) << l << ansi::reset << "\n";
            mdFirst_ = false;
            ++blockNewlines_;
        }
        restoreStatusLocked();
    }

    void setPartial(const std::vector<std::string>& lines) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!mdMode_) return;
        clearStatusLocked();
        erasePartialLocked();
        for (size_t i = 0; i < lines.size(); ++i)
            std::cout << mdPrefixLocked(mdFirst_ && i == 0) << lines[i] << ansi::reset << "\n";
        partialLines_ = lines.size();
        restoreStatusLocked();
    }

    void endBlock() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!inBlock_) return;
        if (mdMode_) {
            clearStatusLocked();
            erasePartialLocked();
            mdMode_ = false;
            inBlock_ = false;
            restoreStatusLocked();
            return;
        }
        clearStatusLocked();
        flushWordLocked();
        std::cout << "\n";
        ++blockNewlines_;
        col_ = 0;
        inBlock_ = false;
        restoreStatusLocked();
    }

    bool inBlock() const { return inBlock_; }

    // Nombre de lignes occupées par le dernier bloc (pour le réafficher en Markdown).
    size_t lastBlockLines() const { return blockNewlines_; }

private:
    std::mutex mu_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> interrupted_{false};
    std::atomic<bool> expandRequested_{false};
    std::atomic<bool> cancelRequested_{false};  // arrêt demandé par l'API ; ne se remet pas à zéro entre étapes
    std::function<void()> onExpand_;
    std::atomic<size_t> tokens_{0};
    std::string verb_;
    bool watchEsc_ = false;
    std::chrono::steady_clock::time_point start_;
    size_t frame_ = 0;

    // État du texte : colonne courante et position de la ligne de statut.
    size_t col_ = 0;
    bool statusDrawn_ = false;
    bool statusBelow_ = false;  // statut sur la ligne sous le texte (texte non terminé)

    Block kind_ = Block::Answer;
    bool inBlock_ = false;
    bool hasText_ = false;
    bool pendingSpace_ = false;
    int pendingNL_ = 0;
    size_t blockNewlines_ = 0;
    std::string word_;

    // Mode Markdown
    bool mdMode_ = false;
    bool mdFirst_ = true;       // la prochaine ligne affichée est la première du bloc (⏺)
    size_t partialLines_ = 0;   // lignes occupées par la ligne en cours

    std::string mdPrefixLocked(bool first) const {
        return first ? std::string(P.text) + "⏺" + ansi::reset + " " : std::string("  ");
    }

    // Efface la ligne en cours (le curseur est juste en dessous). Si elle dépasse l'écran,
    // on ne peut plus remonter jusqu'à elle : on la laisse.
    void erasePartialLocked() {
        if (partialLines_ > 0 && partialLines_ + 1 < consoleHeight())
            std::cout << "\x1b[" << partialLines_ << "A\r\x1b[J";
        partialLines_ = 0;
    }

    size_t lineWidth() const { return std::max<size_t>(30, std::min<size_t>(consoleWidth() - 2, 110)); }

    std::string styleLocked() const {
        return kind_ == Block::Thinking ? std::string(P.muted) + ansi::italic : std::string(P.text);
    }

    void loop() {
        while (running_) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!running_) break;
                ++frame_;
                if (watchEsc_) {
                    while (_kbhit()) {
                        wint_t ch = _getwch();
                        if (ch == 27) interrupted_ = true;
                        else if (ch == 15) expandRequested_ = true;  // Ctrl+O
                        else if (ch == 0 || ch == 0xE0) _getwch();
                    }
                }
                drawStatusLocked();
            }
            // Hors du verrou : la fonction d'affichage reprend mu_ pour écrire.
            if (expandRequested_.exchange(false) && onExpand_) onExpand_();
            std::this_thread::sleep_for(std::chrono::milliseconds(90));
        }
    }

    void drawStatusLocked() {
        static const char* glyphs[] = {"·", "✢", "✳", "✶", "✻", "✽", "✻", "✶", "✳", "✢"};
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_).count();
        const std::string verb = verb_ + "…";
        const long span = (long)displayLength(verb) + 8;
        const long pos = (long)(frame_ % (size_t)span) - 4;

        std::string info = "(" + std::to_string(secs) + "s";
        if (tokens_ > 0) info += " · ↓ " + formatTokens(tokens_) + " tokens";
        if (watchEsc_) info += " · esc to interrupt";
        info += ")";
        if (displayLength(verb) + displayLength(info) + 4 > consoleWidth()) info = "(" + std::to_string(secs) + "s)";

        std::cout << "\r\x1b[2K" << P.accent << glyphs[(frame_ / 2) % 10] << ansi::reset << " "
                  << shimmerText(verb, pos, P.accent) << " " << P.muted << info << ansi::reset << std::flush;
        statusDrawn_ = true;
    }

    // Efface la ligne de statut et replace le curseur à la fin du texte.
    void clearStatusLocked() {
        if (!statusDrawn_) return;
        std::cout << "\r\x1b[2K";
        if (statusBelow_) std::cout << "\x1b[1A\x1b[" << (col_ + 1) << "G";
        statusDrawn_ = false;
        statusBelow_ = false;
    }

    // Après une écriture : remet la ligne de statut sous le texte.
    void restoreStatusLocked() {
        if (running_) {
            if (col_ > 0) {
                std::cout << "\n";
                statusBelow_ = true;
            } else {
                statusBelow_ = false;
            }
            drawStatusLocked();
        }
        std::cout << std::flush;
    }

    void blockNewlineLocked() {
        std::cout << "\n  ";
        col_ = 2;
        ++blockNewlines_;
    }

    void flushWordLocked() {
        if (word_.empty()) return;
        for (int k = 0; k < std::min(pendingNL_, 2); ++k) blockNewlineLocked();
        if (pendingNL_ > 0) pendingSpace_ = false;
        pendingNL_ = 0;

        const size_t width = lineWidth();
        std::string w = word_;
        word_.clear();
        size_t wl = displayLength(w);
        if (pendingSpace_) {
            if (col_ + 1 + wl > width) blockNewlineLocked();
            else {
                std::cout << ' ';
                ++col_;
            }
            pendingSpace_ = false;
        } else if (col_ + wl > width && col_ > 2) {
            blockNewlineLocked();
        }
        const std::string style = styleLocked();
        while (col_ + displayLength(w) > width) {  // mot plus long que la ligne : découpe
            std::string head = utf8Prefix(w, width - col_);
            std::cout << style << head << ansi::reset;
            w = w.substr(head.size());
            blockNewlineLocked();
        }
        std::cout << style << w << ansi::reset;
        col_ += displayLength(w);
        hasText_ = true;
    }
};

// Spinner simple (vérification de clé, liste des modèles, exécution d'outils).
class Spinner {
public:
    explicit Spinner(std::string label, bool watchEsc = false) : label_(std::move(label)), watchEsc_(watchEsc) {}
    ~Spinner() { stop(); }

    void start() {
        LiveView::get().reset();
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
