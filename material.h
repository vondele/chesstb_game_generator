#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chess/piece_config.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace material {

// Build a chesstb Piece_Config from a material string and return its canonical
// name. This lets KvKQ resolve to KQK, KvKR to KRK, etc. We filter table
// availability by canonical name so non-canonical orientations are included.
inline std::string canonical_chesstb_name(const std::string& mat) {
    size_t v = mat.find('v');
    std::string w = (v == std::string::npos) ? "" : mat.substr(1, v - 1);
    std::string b = (v == std::string::npos) ? "" : mat.substr(v + 2);

    std::array<Piece, MAX_MAN> pieces;
    size_t n = 0;
    pieces[n++] = WHITE_KING;
    for (char c : w) {
        switch (c) {
            case 'Q': pieces[n++] = WHITE_QUEEN; break;
            case 'R': pieces[n++] = WHITE_ROOK; break;
            case 'B': pieces[n++] = WHITE_BISHOP; break;
            case 'N': pieces[n++] = WHITE_KNIGHT; break;
            case 'P': pieces[n++] = WHITE_PAWN; break;
        }
    }
    pieces[n++] = BLACK_KING;
    for (char c : b) {
        switch (c) {
            case 'Q': pieces[n++] = BLACK_QUEEN; break;
            case 'R': pieces[n++] = BLACK_ROOK; break;
            case 'B': pieces[n++] = BLACK_BISHOP; break;
            case 'N': pieces[n++] = BLACK_KNIGHT; break;
            case 'P': pieces[n++] = BLACK_PAWN; break;
        }
    }

    Piece_Config ps(Const_Span<Piece>(pieces.data(), n));
    return ps.name();
}

inline std::vector<std::string> filter_available(const std::vector<std::string>& mats,
                                                  const fs::path& wdl_dir) {
    std::vector<std::string> out;
    for (const auto& mat : mats) {
        if (fs::exists(wdl_dir / (canonical_chesstb_name(mat) + ".lzw")))
            out.push_back(mat);
    }
    return out;
}

struct WeightResult {
    std::vector<double> weights;
    size_t matched = 0;
};

// Thread-safe weighted sampler over a small set of material weights.
// Uses precomputed cumulative weights and binary search.
struct WeightedSampler {
    std::vector<double> cumulative;
    double total = 0.0;

    explicit WeightedSampler(const std::vector<double>& weights) {
        cumulative.reserve(weights.size());
        double sum = 0.0;
        for (double w : weights) {
            sum += w;
            cumulative.push_back(sum);
        }
        total = sum;
    }

    bool valid() const { return total > 0.0; }

    size_t operator()(std::mt19937_64& rng) const {
        std::uniform_real_distribution<double> dist(0.0, total);
        double u = dist(rng);
        return static_cast<size_t>(std::upper_bound(cumulative.begin(), cumulative.end(), u) -
                                   cumulative.begin());
    }
};

// Load per-material frequency weights from a file.  Each line is
// "<material> <count>".  Blank lines and lines starting with '#' are ignored.
// Duplicate materials have their counts summed.  Materials in the generation
// pool that are not listed receive weight 0.
inline std::optional<WeightResult> load_endgame_weights(const fs::path& path,
                                                         const std::vector<std::string>& materials,
                                                         std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "Failed to open endgame counts file: " + path.string();
        return std::nullopt;
    }

    std::unordered_map<std::string, uint64_t> counts;
    std::string line;
    size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        if (line[start] == '#') continue;

        std::istringstream iss(line.substr(start));
        std::string mat;
        uint64_t cnt = 0;
        if (!(iss >> mat >> cnt)) {
            error = "Invalid line " + std::to_string(line_no) + " in " + path.string();
            return std::nullopt;
        }
        counts[mat] += cnt;
    }

    WeightResult result;
    result.weights.reserve(materials.size());
    double total = 0.0;
    for (const auto& mat : materials) {
        auto it = counts.find(mat);
        double w = 0.0;
        if (it != counts.end()) {
            w = static_cast<double>(it->second);
            ++result.matched;
        }
        result.weights.push_back(w);
        total += w;
    }

    if (total <= 0.0) {
        error = "No positive endgame weights match the current material pool";
        return std::nullopt;
    }

    return result;
}

// Generate canonical material names, e.g. "KQvK", "KRBvKNP".
// Materials include normal pawns only (no frozen-pair 'p' marker).
// Total piece count (including the two kings) is in [3, max_pieces].
inline std::vector<std::string> generate_combinations(int max_pieces,
                                                       const std::vector<std::string>& include,
                                                       const std::vector<std::string>& exclude) {
    std::vector<std::string> combos;

    std::function<void(std::string&, std::string&)> gen = [&](std::string& w, std::string& b) {
        int total = static_cast<int>(w.size() + b.size() + 2);
        if (total > max_pieces) return;
        if (total >= 3) {
            std::string ws = w;
            std::string bs = b;
            // Sort white pieces by QRBNP order.
            std::sort(ws.begin(), ws.end(), [](char a, char b) {
                return std::string("QRBNP").find(a) < std::string("QRBNP").find(b);
            });
            // Sort black pieces by qrbnp order.
            std::sort(bs.begin(), bs.end(), [](char a, char b) {
                return std::string("qrbnp").find(a) < std::string("qrbnp").find(b);
            });
            for (char& c : bs) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            combos.push_back("K" + ws + "vK" + bs);
        }
        if (total == max_pieces) return;
        // Add a piece to white or black side.
        for (char p : std::string("QRBNP")) {
            w.push_back(p);
            gen(w, b);
            w.pop_back();
        }
        for (char p : std::string("qrbnp")) {
            b.push_back(p);
            gen(w, b);
            b.pop_back();
        }
    };

    std::string w, b;
    gen(w, b);

    std::sort(combos.begin(), combos.end());
    combos.erase(std::unique(combos.begin(), combos.end()), combos.end());

    if (!include.empty()) {
        std::set<std::string> inc(include.begin(), include.end());
        combos.erase(std::remove_if(combos.begin(), combos.end(),
                                    [&](const std::string& s) { return inc.find(s) == inc.end(); }),
                     combos.end());
    }
    if (!exclude.empty()) {
        std::set<std::string> exc(exclude.begin(), exclude.end());
        combos.erase(std::remove_if(combos.begin(), combos.end(),
                                    [&](const std::string& s) { return exc.find(s) != exc.end(); }),
                     combos.end());
    }

    return combos;
}

}  // namespace material
