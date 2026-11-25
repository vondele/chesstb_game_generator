#pragma once

// Canonical chesstb material profile name from a chess-library board.

#include "bridge.h"

#include <chess.hpp>

#include <string>
#include <vector>

inline std::string profile_from_board(const chess::Board& board) {
    const chesstb::Position pos = bridge::to_chesstb_position(board);

    std::vector<chesstb::Piece> pieces;
    for (chesstb::Color c : {chesstb::WHITE, chesstb::BLACK})
        for (int t = chesstb::PAWN; t <= chesstb::KING; ++t) {
            const chesstb::Piece pc = chesstb::piece_make(c, static_cast<chesstb::Piece_Type>(t));
            for (size_t i = pos.piece_bb(pc).num_set_bits(); i; --i)
                pieces.push_back(pc);
        }

    chesstb::Piece_Config pc(chesstb::Const_Span<chesstb::Piece>(pieces.data(), pieces.size()));
    return pc.name();
}
