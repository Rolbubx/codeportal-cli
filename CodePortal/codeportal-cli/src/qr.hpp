// qr.hpp : petit encodeur de QR codes (mode octets, versions 1 à 40), pour afficher dans le terminal le lien
// que le téléphone scanne (/web phone). Algorithme de la norme ISO/IEC 18004, d'après la structure de la
// bibliothèque « QR Code generator » de Project Nayuki (licence MIT).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace qr {

enum class Ecc { Low = 0, Medium = 1, Quartile = 2, High = 3 };

class Code {
public:
    // Encode `text` (UTF-8) au plus petit niveau de version possible. Vide (size() == 0) si trop long.
    static Code encode(const std::string& text, Ecc ecl = Ecc::Medium) {
        const std::vector<uint8_t> bytes(text.begin(), text.end());
        for (int ver = 1; ver <= 40; ++ver) {
            const int ccBits = ver <= 9 ? 8 : 16;
            const long capacity = (long)dataCodewords(ver, ecl) * 8;
            const long needed = 4 + ccBits + (long)bytes.size() * 8;
            if ((long)bytes.size() >= (1L << ccBits) || needed > capacity) continue;

            std::vector<bool> bits;
            auto put = [&](uint32_t v, int n) {
                for (int i = n - 1; i >= 0; --i) bits.push_back(((v >> i) & 1) != 0);
            };
            put(0x4, 4);  // mode octets
            put((uint32_t)bytes.size(), ccBits);
            for (uint8_t b : bytes) put(b, 8);
            put(0, (int)std::min<long>(4, capacity - (long)bits.size()));  // terminateur
            put(0, (int)((8 - bits.size() % 8) % 8));
            for (uint8_t pad = 0xEC; (long)bits.size() < capacity; pad ^= 0xEC ^ 0x11) put(pad, 8);

            std::vector<uint8_t> data(bits.size() / 8, 0);
            for (size_t i = 0; i < bits.size(); ++i)
                if (bits[i]) data[i >> 3] |= (uint8_t)(1 << (7 - (i & 7)));
            return Code(ver, ecl, data);
        }
        return Code();
    }

    int size() const { return size_; }
    bool dark(int x, int y) const { return x >= 0 && y >= 0 && x < size_ && y < size_ && modules_[(size_t)y][(size_t)x]; }

private:
    int version_ = 0, size_ = 0;
    Ecc ecl_ = Ecc::Medium;
    std::vector<std::vector<bool>> modules_, isFunction_;

    Code() = default;

    Code(int ver, Ecc ecl, const std::vector<uint8_t>& data) : version_(ver), size_(ver * 4 + 17), ecl_(ecl) {
        modules_.assign((size_t)size_, std::vector<bool>((size_t)size_, false));
        isFunction_ = modules_;
        drawFunctionPatterns();
        drawCodewords(addEccAndInterleave(data));
        // Le masque qui donne le motif le plus régulier (pénalité la plus basse).
        int best = 0;
        long bestPenalty = -1;
        for (int m = 0; m < 8; ++m) {
            applyMask(m);
            drawFormatBits(m);
            const long p = penalty();
            if (bestPenalty < 0 || p < bestPenalty) {
                bestPenalty = p;
                best = m;
            }
            applyMask(m);  // annule (XOR)
        }
        applyMask(best);
        drawFormatBits(best);
        isFunction_.clear();
    }

    // ---- tables de la norme --------------------------------------------------------------------------
    static int eccPerBlock(int ver, Ecc e) {
        static const int8_t t[4][41] = {
            {-1, 7,  10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28,
             28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
            {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
             26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
            {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30,
             28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
            {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28,
             30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30}};
        return t[(int)e][ver];
    }
    static int numBlocks(int ver, Ecc e) {
        static const int8_t t[4][41] = {
            {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17,
             18, 19, 19, 20, 21, 22, 24, 25},
            {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28,
             29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
            {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38,
             40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
            {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45,
             48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81}};
        return t[(int)e][ver];
    }
    static int rawDataModules(int ver) {
        int r = (16 * ver + 128) * ver + 64;
        if (ver >= 2) {
            const int n = ver / 7 + 2;
            r -= (25 * n - 10) * n - 55;
            if (ver >= 7) r -= 36;
        }
        return r;
    }
    static int dataCodewords(int ver, Ecc e) { return rawDataModules(ver) / 8 - eccPerBlock(ver, e) * numBlocks(ver, e); }

    std::vector<int> alignmentPositions() const {
        if (version_ == 1) return {};
        const int n = version_ / 7 + 2;
        const int step = version_ == 32 ? 26 : (version_ * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
        std::vector<int> r((size_t)n);
        r[0] = 6;
        for (int i = n - 1, pos = size_ - 7; i >= 1; --i, pos -= step) r[(size_t)i] = pos;
        return r;
    }

    // ---- dessin --------------------------------------------------------------------------------------
    void setFn(int x, int y, bool d) {
        modules_[(size_t)y][(size_t)x] = d;
        isFunction_[(size_t)y][(size_t)x] = true;
    }

    void drawFunctionPatterns() {
        for (int i = 0; i < size_; ++i) {
            setFn(6, i, i % 2 == 0);
            setFn(i, 6, i % 2 == 0);
        }
        const int finders[3][2] = {{3, 3}, {size_ - 4, 3}, {3, size_ - 4}};
        for (const auto& f : finders)
            for (int dy = -4; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx) {
                    const int d = std::max(std::abs(dx), std::abs(dy)), x = f[0] + dx, y = f[1] + dy;
                    if (x >= 0 && x < size_ && y >= 0 && y < size_) setFn(x, y, d != 2 && d != 4);
                }
        const auto pos = alignmentPositions();
        const size_t n = pos.size();
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n; ++j) {
                if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                        setFn(pos[i] + dx, pos[j] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
            }
        drawFormatBits(0);  // réserve la place ; les vrais bits sont écrits avec le masque choisi
        if (version_ >= 7) {
            int rem = version_;
            for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
            const long bits = (long)version_ << 12 | rem;
            for (int i = 0; i < 18; ++i) {
                const bool b = ((bits >> i) & 1) != 0;
                const int a = size_ - 11 + i % 3, c = i / 3;
                setFn(a, c, b);
                setFn(c, a, b);
            }
        }
    }

    void drawFormatBits(int mask) {
        static const int eclBits[4] = {1, 0, 3, 2};
        const int data = eclBits[(int)ecl_] << 3 | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        const int bits = (data << 10 | rem) ^ 0x5412;
        auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
        for (int i = 0; i <= 5; ++i) setFn(8, i, bit(i));
        setFn(8, 7, bit(6));
        setFn(8, 8, bit(7));
        setFn(7, 8, bit(8));
        for (int i = 9; i < 15; ++i) setFn(14 - i, 8, bit(i));
        for (int i = 0; i < 8; ++i) setFn(size_ - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i) setFn(8, size_ - 15 + i, bit(i));
        setFn(8, size_ - 8, true);  // module toujours sombre
    }

    // ---- Reed-Solomon --------------------------------------------------------------------------------
    static uint8_t mul(uint8_t x, uint8_t y) {
        int z = 0;
        for (int i = 7; i >= 0; --i) {
            z = (z << 1) ^ ((z >> 7) * 0x11D);
            z ^= ((y >> i) & 1) * x;
        }
        return (uint8_t)z;
    }
    static std::vector<uint8_t> generator(int degree) {
        std::vector<uint8_t> r((size_t)degree, 0);
        r.back() = 1;
        uint8_t root = 1;
        for (int i = 0; i < degree; ++i) {
            for (size_t j = 0; j < r.size(); ++j) {
                r[j] = mul(r[j], root);
                if (j + 1 < r.size()) r[j] ^= r[j + 1];
            }
            root = mul(root, 0x02);
        }
        return r;
    }
    static std::vector<uint8_t> remainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& div) {
        std::vector<uint8_t> r(div.size(), 0);
        for (uint8_t b : data) {
            const uint8_t factor = b ^ r[0];
            r.erase(r.begin());
            r.push_back(0);
            for (size_t i = 0; i < r.size(); ++i) r[i] ^= mul(div[i], factor);
        }
        return r;
    }

    std::vector<uint8_t> addEccAndInterleave(const std::vector<uint8_t>& data) const {
        const int blocks = numBlocks(version_, ecl_), eccLen = eccPerBlock(version_, ecl_);
        const int raw = rawDataModules(version_) / 8;
        const int shortBlocks = blocks - raw % blocks, shortLen = raw / blocks;
        const auto gen = generator(eccLen);
        std::vector<std::vector<uint8_t>> all;
        size_t k = 0;
        for (int i = 0; i < blocks; ++i) {
            const size_t len = (size_t)(shortLen - eccLen + (i < shortBlocks ? 0 : 1));
            std::vector<uint8_t> d(data.begin() + (long)k, data.begin() + (long)(k + len));
            k += len;
            const auto ecc = remainder(d, gen);
            if (i < shortBlocks) d.push_back(0);
            d.insert(d.end(), ecc.begin(), ecc.end());
            all.push_back(d);
        }
        std::vector<uint8_t> out;
        for (size_t i = 0; i < all[0].size(); ++i)
            for (size_t j = 0; j < all.size(); ++j)
                if (i != (size_t)(shortLen - eccLen) || (int)j >= shortBlocks) out.push_back(all[j][i]);
        return out;
    }

    void drawCodewords(const std::vector<uint8_t>& data) {
        size_t i = 0;
        for (int right = size_ - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < size_; ++vert)
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? size_ - 1 - vert : vert;
                    if (!isFunction_[(size_t)y][(size_t)x] && i < data.size() * 8) {
                        modules_[(size_t)y][(size_t)x] = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                        ++i;
                    }
                }
        }
    }

    void applyMask(int m) {
        for (int y = 0; y < size_; ++y)
            for (int x = 0; x < size_; ++x) {
                if (isFunction_[(size_t)y][(size_t)x]) continue;
                bool inv = false;
                switch (m) {
                    case 0: inv = (x + y) % 2 == 0; break;
                    case 1: inv = y % 2 == 0; break;
                    case 2: inv = x % 3 == 0; break;
                    case 3: inv = (x + y) % 3 == 0; break;
                    case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                    case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                    case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                    default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (inv) modules_[(size_t)y][(size_t)x] = !modules_[(size_t)y][(size_t)x];
            }
    }

    // Pénalité simplifiée (règles 1, 2 et 4 de la norme) : suffit à éviter les motifs difficiles à lire.
    long penalty() const {
        long p = 0;
        for (int pass = 0; pass < 2; ++pass)
            for (int a = 0; a < size_; ++a) {
                int run = 1;
                for (int b = 1; b < size_; ++b) {
                    const bool cur = pass ? dark(a, b) : dark(b, a), prev = pass ? dark(a, b - 1) : dark(b - 1, a);
                    if (cur == prev) {
                        ++run;
                        if (run == 5) p += 3;
                        else if (run > 5) ++p;
                    } else {
                        run = 1;
                    }
                }
            }
        long darkCount = 0;
        for (int y = 0; y < size_; ++y)
            for (int x = 0; x < size_; ++x) {
                if (dark(x, y)) ++darkCount;
                if (x + 1 < size_ && y + 1 < size_ && dark(x, y) == dark(x + 1, y) && dark(x, y) == dark(x, y + 1) &&
                    dark(x, y) == dark(x + 1, y + 1))
                    p += 3;
            }
        const long total = (long)size_ * size_;
        const long k = (std::labs(darkCount * 20 - total * 10) + total - 1) / total - 1;
        return p + std::max(0L, k) * 10;
    }
};

// QR code pour le terminal : deux rangées de modules par ligne (▀ avec couleur du haut et du fond),
// sombre sur clair quel que soit le thème, avec une marge blanche autour (nécessaire à la lecture).
inline std::vector<std::string> terminalLines(const Code& c, int margin = 3) {
    std::vector<std::string> lines;
    const int n = c.size();
    for (int y = -margin; y < n + margin; y += 2) {
        std::string line;
        for (int x = -margin; x < n + margin; ++x) {
            const bool top = c.dark(x, y), bottom = c.dark(x, y + 1);
            line += top ? "\x1b[38;2;0;0;0m" : "\x1b[38;2;255;255;255m";
            line += bottom ? "\x1b[48;2;0;0;0m" : "\x1b[48;2;255;255;255m";
            line += "▀";
        }
        line += "\x1b[0m";
        lines.push_back(line);
    }
    return lines;
}

}  // namespace qr
