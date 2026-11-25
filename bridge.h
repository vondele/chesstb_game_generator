#pragma once

// Bridge between chess-library and chesstb types. Both number squares a1 = 0.

#include <chess.hpp>

#include "chesstb.h"

namespace bridge {

// Both order piece types P, N, B, R, Q, K: chess-library from 0, chesstb from 1.
inline chesstb::Piece to_chesstb_piece(chess::PieceType pt, chess::Color c) {
    return chesstb::piece_make(static_cast<chesstb::Color>(static_cast<int>(c.internal())),
                               static_cast<chesstb::Piece_Type>(static_cast<int>(pt.internal()) + 1));
}

inline chesstb::Square ep_square(const chess::Board& board) {
    const chess::Square ep = board.enpassantSq();
    return ep == chess::Square::NO_SQ ? chesstb::SQ_END : static_cast<chesstb::Square>(ep.index());
}

inline chesstb::Position to_chesstb_position(const chess::Board& board) {
    chesstb::Position pos;
    pos.clear();
    for (chess::Color c : {chess::Color::WHITE, chess::Color::BLACK}) {
        for (auto pt : {chess::PieceType::PAWN, chess::PieceType::KNIGHT, chess::PieceType::BISHOP,
                        chess::PieceType::ROOK, chess::PieceType::QUEEN, chess::PieceType::KING})
            pos.m_pieces[to_chesstb_piece(pt, c)] = chesstb::Bitboard(board.pieces(pt, c).getBits());
        pos.m_pieces[chesstb::piece_occupy(static_cast<chesstb::Color>(static_cast<int>(c.internal())))] =
            chesstb::Bitboard(board.us(c).getBits());
    }
    pos.set_turn(static_cast<chesstb::Color>(static_cast<int>(board.sideToMove().internal())));

    const auto cr = board.castlingRights();
    for (chess::Color c : {chess::Color::WHITE, chess::Color::BLACK})
        for (auto side : {chess::Board::CastlingRights::Side::KING_SIDE,
                          chess::Board::CastlingRights::Side::QUEEN_SIDE})
            if (cr.has(c, side))
                pos.m_castling_rooks |= static_cast<chesstb::Square>(
                    chess::Square(cr.getRookFile(c, side), chess::Rank::rank(chess::Rank::RANK_1, c)).index());
    return pos;
}

}  // namespace bridge
