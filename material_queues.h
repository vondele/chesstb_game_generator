#pragma once

// Thread-safe ordered queue manager for the generator.
//
// All pending SlotState objects are kept in a single priority queue ordered by
// game progression (earliest positions first).  Workers pop one position at a
// time, follow its game line through the current material as far as possible,
// and queue children back for their own materials.

#include "material_key.h"
#include "slot_state.h"

#include <chess.hpp>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

enum class ProgressOutcome {
    kLater,
    kSame,
    kEarlier,
};

constexpr chess::Bitboard flip_vertical(chess::Bitboard b) {
    b = ((b << 56)) | ((b << 40) & 0x00FF000000000000ULL) |
        ((b << 24) & 0x0000FF0000000000ULL) | ((b << 8) & 0x000000FF00000000ULL) |
        ((b >> 8) & 0x00000000FF000000ULL) | ((b >> 24) & 0x0000000000FF0000ULL) |
        ((b >> 40) & 0x000000000000FF00ULL) | ((b >> 56));
    return b;
}

inline ProgressOutcome game_progress(const chess::Board& board1, const chess::Board& board2) {
    // Pawns first.
    for (const auto& color : {chess::Color::WHITE, chess::Color::BLACK}) {
        auto pc1 = board1.pieces(chess::PieceType::PAWN, color).count();
        auto pc2 = board2.pieces(chess::PieceType::PAWN, color).count();
        if (pc1 < pc2) return ProgressOutcome::kLater;
        if (pc1 > pc2) return ProgressOutcome::kEarlier;
    }

    // Pawn structure (pawn counts are equal here).
    for (const auto& color : {chess::Color::WHITE, chess::Color::BLACK}) {
        auto pawns1 = board1.pieces(chess::PieceType::PAWN, color);
        auto pawns2 = board2.pieces(chess::PieceType::PAWN, color);
        if (color == chess::Color::BLACK) {
            pawns1 = flip_vertical(pawns1);
            pawns2 = flip_vertical(pawns2);
        }
        while (pawns1) {
            chess::Square sq1 = chess::Square(pawns1.pop());
            chess::Square sq2 = chess::Square(pawns2.pop());
            if (sq1 < sq2) return ProgressOutcome::kEarlier;
            if (sq1 > sq2) return ProgressOutcome::kLater;
        }
    }

    // Other pieces by value.
    for (const auto& piece_type : {chess::PieceType::QUEEN, chess::PieceType::ROOK,
                                   chess::PieceType::BISHOP, chess::PieceType::KNIGHT}) {
        for (const auto& color : {chess::Color::WHITE, chess::Color::BLACK}) {
            auto pc1 = board1.pieces(piece_type, color).count();
            auto pc2 = board2.pieces(piece_type, color).count();
            if (pc1 < pc2) return ProgressOutcome::kLater;
            if (pc1 > pc2) return ProgressOutcome::kEarlier;
        }
    }

    // Light-squared bishops (dark bishops are implicitly checked by the sum).
    constexpr chess::Bitboard kLightSquares = 0x55AA55AA55AA55AA;
    for (const auto& color : {chess::Color::WHITE, chess::Color::BLACK}) {
        auto pc1 = (board1.pieces(chess::PieceType::BISHOP, color) & kLightSquares).count();
        auto pc2 = (board2.pieces(chess::PieceType::BISHOP, color) & kLightSquares).count();
        if (pc1 < pc2) return ProgressOutcome::kLater;
        if (pc1 > pc2) return ProgressOutcome::kEarlier;
    }

    // En passant square.
    chess::Square ep1 = board1.enpassantSq();
    chess::Square ep2 = board2.enpassantSq();
    if (ep1 != chess::Square::underlying::NO_SQ && ep2 == chess::Square::underlying::NO_SQ)
        return ProgressOutcome::kEarlier;
    if (ep1 == chess::Square::underlying::NO_SQ && ep2 != chess::Square::underlying::NO_SQ)
        return ProgressOutcome::kLater;
    if (ep1 != chess::Square::underlying::NO_SQ && ep2 != chess::Square::underlying::NO_SQ) {
        if (ep1 < ep2) return ProgressOutcome::kEarlier;
        if (ep1 > ep2) return ProgressOutcome::kLater;
    }

    // Castling rights.
    chess::Board::CastlingRights cr1 = board1.castlingRights();
    chess::Board::CastlingRights cr2 = board2.castlingRights();
    for (const auto& color : {chess::Color::WHITE, chess::Color::BLACK}) {
        for (const auto& side : {chess::Board::CastlingRights::Side::KING_SIDE,
                                 chess::Board::CastlingRights::Side::QUEEN_SIDE}) {
            if (cr1.has(color, side) && !cr2.has(color, side)) return ProgressOutcome::kEarlier;
            if (!cr1.has(color, side) && cr2.has(color, side)) return ProgressOutcome::kLater;
        }
    }

    return ProgressOutcome::kSame;
}

struct LaterSlotState {
    bool operator()(const SlotState& lhs, const SlotState& rhs) const {
        ProgressOutcome outcome = game_progress(lhs.board, rhs.board);
        if (outcome == ProgressOutcome::kLater) return true;
        if (outcome == ProgressOutcome::kEarlier) return false;
        chess::PackedBoard pl = chess::Board::Compact::encode(lhs.board);
        chess::PackedBoard pr = chess::Board::Compact::encode(rhs.board);
        return pl > pr;
    }
};

class MaterialQueues {
public:
    void push(const std::string&, SlotState state) {
        push_all("", std::vector<SlotState>{std::move(state)});
    }

    void push_all(const std::string&, std::vector<SlotState> states) {
        if (states.empty()) return;
        std::lock_guard<std::mutex> lk(mu_);
        for (SlotState& s : states) {
            pq_.push(std::move(s));
        }
    }

    // Returns the next pending position, or nullopt when the queue is empty.
    // The material name returned is the canonical profile of the popped board.
    std::optional<std::pair<std::string, std::vector<SlotState>>> pop_next() {
        std::lock_guard<std::mutex> lk(mu_);
        if (pq_.empty()) return std::nullopt;

        SlotState s = pq_.top();
        pq_.pop();
        std::string name = profile_from_board(s.board);
        return std::make_pair(name, std::vector<SlotState>{std::move(s)});
    }

    bool empty() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pq_.empty();
    }

private:
    mutable std::mutex mu_;
    std::priority_queue<SlotState, std::vector<SlotState>, LaterSlotState> pq_;
};
