#pragma once

// Per-slot state and PGN helpers for the generator.

#include "evaluator.h"

#include <chess.hpp>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pgn {

inline std::string result_string(const chess::Board& board) {
    auto [reason, result] = board.isGameOver();
    (void)reason;
    if (result == chess::GameResult::DRAW) return "1/2-1/2";
    bool stm_is_white = (board.sideToMove() == chess::Color::WHITE);
    bool stm_wins = (result == chess::GameResult::WIN);
    if (stm_is_white == stm_wins)
        return "1-0";
    else
        return "0-1";
}

inline std::string score_comment(const evaluator::ProbeInfo& info) {
    // Prefer the rule-50-aware metric used for move selection.
    if (info.has_dtm50) {
        if (info.dtm50_result == evaluator::Result::WIN) {
            return "+M" + std::to_string(info.dtm50) + "/245 0.000s";
        } else if (info.dtm50_result == evaluator::Result::LOSS) {
            return "-M" + std::to_string(std::abs(info.dtm50)) + "/245 0.000s";
        } else {
            // Cursed/blessed or rule-50 draw: report as a draw.
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%+.2f/245 0.000s", info.dtc / 100.0);
            return buf;
        }
    }

    // Fallback to non-rule-50 DTM.
    if (info.result == evaluator::Result::WIN) {
        return "+M" + std::to_string(std::abs(info.dtm)) + "/245 0.000s";
    } else if (info.result == evaluator::Result::LOSS) {
        return "-M" + std::to_string(std::abs(info.dtm)) + "/245 0.000s";
    } else {
        // Draw: use signed DTC/DTZ in centipawn-like format.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%+.2f/245 0.000s", info.dtc / 100.0);
        return buf;
    }
}

struct Game {
    std::string material;
    std::string fen;
    std::string date;
    std::vector<std::pair<std::string, std::string>> moves;  // SAN, comment
    std::string result;
    int start_move_number = 1;
    bool white_starts = true;
    int ply_count = 0;
    size_t round = 1;
};

inline std::string render(const Game& game) {
    size_t v = game.material.find('v');
    std::string white = (v == std::string::npos) ? game.material : game.material.substr(0, v);
    std::string black = (v == std::string::npos) ? "" : game.material.substr(v + 1);

    std::ostringstream out;
    out << "[Event \"TB Optimal Generation\"]\n";
    out << "[Site \"" << game.material << "\"]\n";
    out << "[Date \"" << game.date << "\"]\n";
    out << "[Round \"" << game.round << "\"]\n";
    out << "[White \"" << white << "\"]\n";
    out << "[Black \"" << black << "\"]\n";
    out << "[Result \"" << game.result << "\"]\n";
    out << "[FEN \"" << game.fen << "\"]\n";
    out << "[PlyCount \"" << game.ply_count << "\"]\n\n";

    constexpr int kMaxColumn = 80;
    int col = 0;
    auto emit = [&](const std::string& token) {
        if (col > 0 && col + 1 + static_cast<int>(token.size()) > kMaxColumn) {
            out << "\n";
            col = 0;
        }
        if (col > 0) {
            out << " ";
            ++col;
        }
        out << token;
        col += static_cast<int>(token.size());
    };

    int move_number = game.start_move_number;
    bool white_to_move = game.white_starts;
    for (size_t i = 0; i < game.moves.size(); ++i) {
        std::string token;
        if (white_to_move) {
            token = std::to_string(move_number) + ". " + game.moves[i].first +
                    " { " + game.moves[i].second + " }";
        } else if (i == 0) {
            // Black to move at start: print ellipsis.
            token = std::to_string(move_number) + "... " + game.moves[i].first +
                    " { " + game.moves[i].second + " }";
        } else {
            token = game.moves[i].first + " { " + game.moves[i].second + " }";
        }
        emit(token);
        if (!white_to_move) {
            ++move_number;
        }
        white_to_move = !white_to_move;
    }
    emit(game.result);
    out << "\n\n";
    return out.str();
}

}  // namespace pgn

struct SlotState {
    size_t slot = 0;
    chess::Board board;
    pgn::Game game;
    int plies = 0;
    evaluator::Result initial_result = evaluator::Result::DRAW;
    int attempt = 0;
};
