#pragma once

// Generate (or regenerate) a starting SlotState for a given output slot.
// Deterministic per-slot seeding matches the old generator exactly.

#include "evaluator.h"
#include "material.h"
#include "options.h"
#include "slot_state.h"
#include "timers.h"

#include <chess.hpp>
#include <probe/probe.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace random_board {

// Parse a material string like "KQvK" or "KRBvKNP" and return the list of
// piece characters for each side (excluding the kings). Black pieces are
// returned lowercase so they are stored correctly in FEN.
inline std::pair<std::vector<char>, std::vector<char>> parse_material(const std::string& mat) {
    size_t v = mat.find('v');
    std::string w = (v == std::string::npos) ? mat.substr(1) : mat.substr(1, v - 1);
    std::string b = (v == std::string::npos) ? "" : mat.substr(v + 2);
    for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return { std::vector<char>(w.begin(), w.end()), std::vector<char>(b.begin(), b.end()) };
}

inline chess::Piece char_to_piece(char c) {
    switch (c) {
        case 'Q': return chess::Piece(chess::Piece::WHITEQUEEN);
        case 'R': return chess::Piece(chess::Piece::WHITEROOK);
        case 'B': return chess::Piece(chess::Piece::WHITEBISHOP);
        case 'N': return chess::Piece(chess::Piece::WHITEKNIGHT);
        case 'P': return chess::Piece(chess::Piece::WHITEPAWN);
        case 'q': return chess::Piece(chess::Piece::BLACKQUEEN);
        case 'r': return chess::Piece(chess::Piece::BLACKROOK);
        case 'b': return chess::Piece(chess::Piece::BLACKBISHOP);
        case 'n': return chess::Piece(chess::Piece::BLACKKNIGHT);
        case 'p': return chess::Piece(chess::Piece::BLACKPAWN);
        default: return chess::Piece::NONE;
    }
}

// Build a FEN piece-placement string for the given square assignment.
// squares[i] = piece char or '.' for empty; index 0 = a1.
inline std::string build_placement(const std::array<char, 64>& squares) {
    std::string fen;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            char c = squares[rank * 8 + file];
            if (c == '.') {
                ++empty;
            } else {
                if (empty > 0) {
                    fen += static_cast<char>('0' + empty);
                    empty = 0;
                }
                fen += c;
            }
        }
        if (empty > 0) fen += static_cast<char>('0' + empty);
        if (rank > 0) fen += '/';
    }
    return fen;
}

// Generate a random legal position for the given material.
// Returns an empty optional if too many attempts fail (should be rare).
inline std::optional<chess::Board> generate(const std::string& material,
                                            std::mt19937_64& rng,
                                            int max_attempts = 10000) {
    auto [wchars, bchars] = parse_material(material);

    std::array<char, 64> squares;
    squares.fill('.');

    std::uniform_int_distribution<int> sq_dist(0, 63);
    std::uniform_int_distribution<int> turn_dist(0, 1);

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        squares.fill('.');

        // Place white king.
        int wk = sq_dist(rng);
        squares[wk] = 'K';

        // Place black king, not adjacent.
        int bk;
        do { bk = sq_dist(rng); } while (bk == wk || chess::Square::distance(chess::Square(bk), chess::Square(wk)) <= 1);
        squares[bk] = 'k';

        // Place white pieces.
        bool ok = true;
        for (char pc : wchars) {
            int sq;
            int inner = 0;
            do {
                sq = sq_dist(rng);
                if (++inner > 1000) { ok = false; break; }
            } while (squares[sq] != '.' || (pc == 'P' && (sq < 8 || sq >= 56)));
            if (!ok) break;
            squares[sq] = pc;
        }
        if (!ok) continue;

        // Place black pieces.
        for (char pc : bchars) {
            int sq;
            int inner = 0;
            do {
                sq = sq_dist(rng);
                if (++inner > 1000) { ok = false; break; }
            } while (squares[sq] != '.' || (pc == 'p' && (sq < 8 || sq >= 56)));
            if (!ok) break;
            squares[sq] = pc;
        }
        if (!ok) continue;

        std::string fen = build_placement(squares);
        fen += (turn_dist(rng) == 0 ? " w - - 0 1" : " b - - 0 1");

        chess::Board board;
        if (!board.setFen(fen)) continue;

        // Match Python filter: board must be valid, side to move not in check,
        // and side not to move must not be giving check.
        if (board.inCheck()) continue;
        chess::Color opp = ~board.sideToMove();
        if (board.isAttacked(board.kingSq(opp), board.sideToMove())) continue;
        if (board.isGameOver().second != chess::GameResult::NONE) continue;

        return board;
    }

    return std::nullopt;
}

}  // namespace random_board

constexpr int MAX_SLOT_ATTEMPTS = 100000;

inline uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline bool allowed_result(evaluator::Result res, const std::set<char>& allowed) {
    switch (res) {
        case evaluator::Result::WIN: return allowed.count('W');
        case evaluator::Result::DRAW: return allowed.count('D');
        case evaluator::Result::LOSS: return allowed.count('L');
    }
    return false;
}

inline std::optional<SlotState> generate_slot_start(size_t slot,
                                                     int start_attempt,
                                                     const Options& opt,
                                                     const std::vector<std::string>& materials,
                                                     const std::set<char>& allowed,
                                                     Probe_Tables& tables,
                                                     const std::string& date,
                                                     Timers* timers,
                                                     const material::WeightedSampler* material_sampler = nullptr) {
    std::uniform_int_distribution<size_t> mat_dist(0, materials.size() - 1);

    uint64_t base_seed = splitmix64(static_cast<uint64_t>(opt.seed));

    for (int attempt = start_attempt; attempt < MAX_SLOT_ATTEMPTS; ++attempt) {
        uint64_t s = base_seed
                   + static_cast<uint64_t>(slot) * MAX_SLOT_ATTEMPTS
                   + static_cast<uint64_t>(attempt);
        std::mt19937_64 rng(splitmix64(s));
        const std::string& mat = material_sampler
                                     ? materials[(*material_sampler)(rng)]
                                     : materials[mat_dist(rng)];

        auto board_opt = random_board::generate(mat, rng);
        if (!board_opt) continue;
        chess::Board board = *board_opt;

        evaluator::ProbeInfo initial = evaluator::probe_board(timers, tables, board);
        if (!allowed_result(initial.result, allowed)) continue;

        if (initial.result != evaluator::Result::DRAW && initial.has_dtm &&
            std::abs(initial.dtm) < opt.min_plies) {
            continue;
        }

        SlotState state;
        state.slot = slot;
        state.game.round = slot + 1;
        state.board = board;
        state.plies = 0;
        state.attempt = attempt;
        state.initial_result = initial.result;
        state.game.material = mat;
        state.game.fen = board.getFen();
        state.game.date = date;
        state.game.start_move_number = board.fullMoveNumber();
        state.game.white_starts = (board.sideToMove() == chess::Color::WHITE);
        return state;
    }
    return std::nullopt;
}
