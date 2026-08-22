#pragma once

// Canonical chesstb material profile name from a chess-library board.

#include "bridge.h"

#include <chess.hpp>
#include <chess/piece_config.h>
#include <chess/position.h>

#include <string>
#include <vector>

inline std::string profile_from_board(const chess::Board& board) {
    Position pos = bridge::to_chesstb_position(board);

    std::vector<Piece> pieces;
    pieces.reserve(16);
    for (int sq = 0; sq < SQUARE_NB; ++sq) {
        Piece p = pos.m_squares[sq];
        if (p != PIECE_NONE) pieces.push_back(p);
    }

    Piece_Config pc(Const_Span<Piece>(pieces.data(), pieces.size()));
    return pc.name();
}
