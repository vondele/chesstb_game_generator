#pragma once

// Bridge between chess-library and chesstb types.
//
// chesstb uses global-namespace types (Position, Move, Piece, Square, Color).
// chess-library uses chess::Board, chess::Move, chess::Piece, etc.

#include <chess.hpp>

#include "chess/chess.h"
#include "chess/position.h"

namespace bridge {

// chess::Piece::underlying ordering:
//   WHITEPAWN=0, WHITEKNIGHT=1, WHITEBISHOP=2, WHITEROOK=3, WHITEQUEEN=4, WHITEKING=5,
//   BLACKPAWN=6, BLACKKNIGHT=7, BLACKBISHOP=8, BLACKROOK=9, BLACKQUEEN=10, BLACKKING=11
// chesstb Piece ordering:
//   WHITE_KING=1, WHITE_QUEEN=2, WHITE_ROOK=3, WHITE_BISHOP=4, WHITE_KNIGHT=5, WHITE_PAWN=6,
//   BLACK_KING=9, BLACK_QUEEN=10, BLACK_ROOK=11, BLACK_BISHOP=12, BLACK_KNIGHT=13, BLACK_PAWN=14
inline Piece to_chesstb_piece(chess::Piece p) {
    if (p == chess::Piece::NONE) return PIECE_NONE;
    static constexpr Piece map[12] = {
        WHITE_PAWN, WHITE_KNIGHT, WHITE_BISHOP, WHITE_ROOK, WHITE_QUEEN, WHITE_KING,
        BLACK_PAWN, BLACK_KNIGHT, BLACK_BISHOP, BLACK_ROOK, BLACK_QUEEN, BLACK_KING,
    };
    return map[static_cast<int>(p.internal())];
}

inline Square to_chesstb_square(chess::Square sq) {
    return static_cast<Square>(sq.index());
}

inline Color to_chesstb_color(chess::Color c) {
    return static_cast<Color>(static_cast<int>(c.internal()));
}

inline Position to_chesstb_position(const chess::Board& board) {
    Position pos;
    pos.clear();
    for (int i = 0; i < 64; ++i) {
        chess::Square sq(i);
        chess::Piece p = board.at(sq);
        if (p != chess::Piece::NONE) {
            pos.put_piece(to_chesstb_piece(p), to_chesstb_square(sq));
        }
    }
    pos.set_turn(to_chesstb_color(board.sideToMove()));
    return pos;
}

}  // namespace bridge
