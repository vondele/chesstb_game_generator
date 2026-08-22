#pragma once

#include <chess.hpp>
#include <chess/attack.h>
#include <probe/probe.h>

#include "bridge.h"
#include "timers.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <vector>

namespace evaluator {

enum class Result { WIN, DRAW, LOSS };

struct ProbeInfo {
    Result result = Result::DRAW;
    int dtm = 0;   // signed: +N = side-to-move mates in N; -N = mated in N; 0 for draw.
    int dtc = 0;   // signed DTZ/DTC: +N = wins in N plies; -N = loses in N; 0 for draw.
    int dtm50 = 0; // signed DTM50 respecting the current 50-move counter.
    bool has_dtm = false;
    bool has_dtc = false;
    bool has_dtm50 = false;
    Result dtm50_result = Result::DRAW; // rule-50-aware result (cursed/blessed -> DRAW).
};

inline Result wdl_to_result(WDL_Entry w) {
    switch (w) {
        case WDL_Entry::WIN:
            return Result::WIN;
        case WDL_Entry::LOSE:
            return Result::LOSS;
        default:
            // DRAW, CURSED_WIN, and BLESSED_LOSS are all draws in practice.
            // CURSED_WIN/BLESSED_LOSS are WDL wins/losses that cannot be
            // realized within the 50-move window, so we never treat them as
            // decisive starting positions or chase them during play.
            return Result::DRAW;
    }
}

inline WDL_Entry probe_wdl_board(Timers* timers, Probe_Tables& tables, const chess::Board& board) {
    auto t0 = std::chrono::steady_clock::now();

    Position pos = bridge::to_chesstb_position(board);
    Square ep = SQ_END;
    if (board.enpassantSq() != chess::Square::NO_SQ)
        ep = bridge::to_chesstb_square(board.enpassantSq());

    // WDL is independent of the halfmove clock; the chesstb wrapper rejects
    // nonzero rule50 for Fathom semantics, so pass 0.
    WDL_Entry w = tables.probe_wdl(pos, ep, 0);

    if (timers) {
        auto t1 = std::chrono::steady_clock::now();
        timers->wdl_probe_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        timers->wdl_probe_count.fetch_add(1);
    }
    return w;
}

// Sign a distance value according to the WDL result.  CURSED_WIN and
// BLESSED_LOSS are treated as draws here (consistent with wdl_to_result),
// so they produce a signed distance of 0.
inline int signed_distance(uint16_t value, WDL_Entry wdl) {
    int v = static_cast<int>(value);
    switch (wdl) {
        case WDL_Entry::WIN:  return v;
        case WDL_Entry::LOSE: return -v;
        default:              return 0;
    }
}

inline ProbeInfo probe_board(Timers* timers, Probe_Tables& tables, const chess::Board& board) {
    auto t0 = std::chrono::steady_clock::now();

    Position pos = bridge::to_chesstb_position(board);
    Square ep = SQ_END;
    if (board.enpassantSq() != chess::Square::NO_SQ)
        ep = bridge::to_chesstb_square(board.enpassantSq());

    Probe_Result r = tables.probe(pos, ep, board.halfMoveClock());
    ProbeInfo info;

    if (r.status == Probe_Result::Status::OK) {
        info.result = wdl_to_result(r.wdl);
        info.has_dtc = r.has_dtc;
        info.has_dtm = r.has_dtm;
        if (r.has_dtc) info.dtc = signed_distance(r.dtc, r.wdl);
        if (r.has_dtm) info.dtm = signed_distance(r.dtm, r.wdl);

        if (r.has_dtm50) {
            info.has_dtm50 = true;
            // Treat cursed/blessed as DRAW under the current 50-move counter.
            WDL_Entry w50 = r.dtm50_wdl;
            if (w50 == WDL_Entry::CURSED_WIN || w50 == WDL_Entry::BLESSED_LOSS)
                w50 = WDL_Entry::DRAW;
            info.dtm50_result = wdl_to_result(w50);
            info.dtm50 = signed_distance(r.dtm50, r.dtm50_wdl);
        }
    }

    if (timers) {
        auto t1 = std::chrono::steady_clock::now();
        timers->probe_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        timers->probe_count.fetch_add(1);
    }
    return info;
}

// Piece values for static eval.
inline int piece_value(chess::PieceType pt) {
    switch (pt.internal()) {
        case chess::PieceType::PAWN: return 1;
        case chess::PieceType::KNIGHT: return 3;
        case chess::PieceType::BISHOP: return 3;
        case chess::PieceType::ROOK: return 5;
        case chess::PieceType::QUEEN: return 9;
        default: return 0;
    }
}

inline int material_diff(const chess::Board& board, chess::Color color) {
    int diff = 0;
    for (auto pt : { chess::PieceType::PAWN, chess::PieceType::KNIGHT, chess::PieceType::BISHOP,
                     chess::PieceType::ROOK, chess::PieceType::QUEEN }) {
        diff += static_cast<int>(board.pieces(pt, color).count()) * piece_value(pt);
        diff -= static_cast<int>(board.pieces(pt, ~color).count()) * piece_value(pt);
    }
    return diff;
}

// Simple center-distance score: corner=0, center=3.
inline int center_score(chess::Square sq) {
    int f = sq.file();
    int r = sq.rank();
    int df = std::min(f, 7 - f);
    int dr = std::min(r, 7 - r);
    return std::min(df, dr);
}

inline int centralization(const chess::Board& board, chess::Color color) {
    int score = 0;
    auto occ = board.occ();
    while (occ) {
        chess::Square sq(occ.pop());
        chess::Piece p = board.at(sq);
        if (p != chess::Piece::NONE && p.color() == color)
            score += center_score(sq);
    }
    return score;
}

inline int hanging_penalty(const chess::Board& board, chess::Color color) {
    int penalty = 0;
    auto occ = board.occ();
    while (occ) {
        chess::Square sq(occ.pop());
        chess::Piece p = board.at(sq);
        if (p == chess::Piece::NONE || p.color() != color) continue;
        chess::PieceType pt = p.type();
        if (pt == chess::PieceType::KING) continue;

        auto attackers = chess::attacks::attackers(board, ~color, sq);
        if (!attackers) continue;
        int piece_val = piece_value(pt);
        int min_att = std::numeric_limits<int>::max();
        auto a = attackers;
        while (a) {
            chess::Square asq(a.pop());
            min_att = std::min(min_att, piece_value(board.at<chess::PieceType>(asq)));
        }
        auto defenders = chess::attacks::attackers(board, color, sq);
        if (!defenders) {
            penalty += piece_val;
        } else if (min_att < piece_val) {
            penalty += piece_val - min_att;
        }
    }
    return penalty;
}

struct MoveEval {
    chess::Move move;
    Result child_result = Result::DRAW;
    int child_dtm = 0;    // from child stm perspective
    int child_dtc = 0;    // from child stm perspective
    int child_dtm50 = 0;           // from child stm perspective, rule-50 aware
    Result child_dtm50_result = Result::DRAW;  // rule-50-aware child result
    int safe_mat = 0;
    int exchange_score = 0;
    int central = 0;
    bool is_rep = false;
    bool has_dtm = false;
    bool has_dtc = false;
    bool has_dtm50 = false;
};

inline std::optional<chess::Move> select_move(Timers* timers,
                                               Probe_Tables& tables,
                                               chess::Board& board,
                                               const ProbeInfo& current) {
    chess::Movelist moves;
    chess::movegen::legalmoves(moves, board);
    if (moves.size() == 0) return std::nullopt;

    // Rule-50-aware result. If DTM50 is available, trust its verdict (cursed
    // wins and blessed losses become draws); otherwise fall back to plain WDL.
    auto effective_result = [](const ProbeInfo& info) -> Result {
        if (info.has_dtm50) return info.dtm50_result;
        return info.result;
    };
    auto child_effective_result = [](const MoveEval& e) -> Result {
        if (e.has_dtm50) return e.child_dtm50_result;
        return e.child_result;
    };

    const Result eff = effective_result(current);

    // Stage 1: cheap WDL-only probe where it can filter children (WIN and
    // DRAW positions), plus static heuristics for every child.
    struct WdlEval {
        chess::Move move;
        chess::Board child;
        WDL_Entry wdl;
        bool is_rep;
        int safe_mat;
        int exchange_score;
        int central;
    };

    std::vector<WdlEval> wdl_evals;
    wdl_evals.reserve(moves.size());

    int init_diff = material_diff(board, board.sideToMove());
    int exchange_mult = (init_diff >= 0) ? 1 : -1;

    for (const auto& move : moves) {
        chess::Board child = board;
        child.makeMove(move);

        WdlEval e;
        e.move = move;
        e.child = child;
        // WDL filtering helps only in WIN positions (find LOSE children) and
        // DRAW positions (find DRAW children). In LOSS positions every child
        // is WDL WIN for the opponent, so the probe would add no information.
        e.wdl = (eff == Result::WIN || eff == Result::DRAW)
                    ? probe_wdl_board(timers, tables, child)
                    : WDL_Entry::ILLEGAL;
        e.is_rep = child.isRepetition();

        int mat_after = material_diff(child, board.sideToMove());
        int hang = hanging_penalty(child, board.sideToMove());
        e.safe_mat = mat_after - hang;
        e.exchange_score = static_cast<int>(child.occ().count()) * exchange_mult;
        e.central = centralization(child, board.sideToMove());

        wdl_evals.push_back(std::move(e));
    }

    // Build the candidate list.
    //
    // - WIN: keep only WDL LOSE children; distance-probe them.
    // - LOSS: every child is WDL WIN for the opponent; distance-probe them all.
    // - DRAW: WDL DRAW children are guaranteed draws, so if any exist use only
    //   them and skip distance probes.  Otherwise every move is a plain win/loss
    //   that may be a cursed/blessed DTM50 draw; distance-probe them all.
    struct Candidate {
        const WdlEval* we;
        bool needs_distance_probe;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(wdl_evals.size());

    if (eff == Result::WIN) {
        for (const auto& e : wdl_evals)
            if (e.wdl == WDL_Entry::LOSE)
                candidates.push_back({&e, true});
    } else if (eff == Result::LOSS) {
        for (const auto& e : wdl_evals)
            candidates.push_back({&e, true});
    } else {
        // DRAW: single pass.  Collect WDL DRAW children first; if none exist,
        // fall back to full distance probing of every child.
        for (const auto& e : wdl_evals)
            if (e.wdl == WDL_Entry::DRAW)
                candidates.push_back({&e, false});
        if (candidates.empty())
            for (const auto& e : wdl_evals)
                candidates.push_back({&e, true});
    }

    // Safety net for unexpected cases (should not happen for sound tables).
    if (candidates.empty()) {
        for (const auto& e : wdl_evals)
            candidates.push_back({&e, true});
    }

    // Stage 2: distance probes only on candidates that need them.  WDL DRAW
    // children in a DRAW position are known draws and bypass the full probe.
    std::vector<MoveEval> evals;
    evals.reserve(candidates.size());
    for (const Candidate& c : candidates) {
        MoveEval e;
        e.move = c.we->move;
        e.is_rep = c.we->is_rep;
        e.safe_mat = c.we->safe_mat;
        e.exchange_score = c.we->exchange_score;
        e.central = c.we->central;

        if (c.needs_distance_probe) {
            ProbeInfo child_info = probe_board(timers, tables, c.we->child);
            e.child_result = child_info.result;
            e.child_dtm = child_info.dtm;
            e.child_dtc = child_info.dtc;
            e.child_dtm50 = child_info.dtm50;
            e.child_dtm50_result = child_info.dtm50_result;
            e.has_dtm = child_info.has_dtm;
            e.has_dtc = child_info.has_dtc;
            e.has_dtm50 = child_info.has_dtm50;
        } else {
            // WDL DRAW child used directly in a DRAW position.
            e.child_result = Result::DRAW;
            e.child_dtm = 0;
            e.child_dtc = 0;
            e.child_dtm50 = 0;
            e.child_dtm50_result = Result::DRAW;
            e.has_dtm = false;
            e.has_dtc = false;
            e.has_dtm50 = false;
        }

        evals.push_back(std::move(e));
    }

    // Filter to moves that preserve the effective WDL from our perspective.
    std::vector<MoveEval> best;
    for (const auto& e : evals) {
        bool ok = false;
        const Result child_eff = child_effective_result(e);
        switch (eff) {
            case Result::WIN:
                ok = (child_eff == Result::LOSS);
                break;
            case Result::LOSS:
                ok = (child_eff == Result::WIN);
                break;
            case Result::DRAW:
                ok = (child_eff == Result::DRAW);
                break;
        }
        if (ok) best.push_back(e);
    }
    // No move preserves a clean win/loss under rule50: fall back to a drawing
    // move if any, otherwise keep all legal moves.
    if (best.empty() && eff != Result::DRAW) {
        for (const auto& e : evals) {
            if (child_effective_result(e) == Result::DRAW) best.push_back(e);
        }
    }
    if (best.empty()) best = std::move(evals);

    switch (eff) {
        case Result::WIN:
        case Result::LOSS: {
            // Prefer the rule-50-aware DTM50 distance, then plain DTM, then DTC.
            // child_* values are from the child's point of view; negate to get the
            // current player's signed distance. Ascending: fastest mate when
            // winning, longest delay when losing.
            auto signed_dist = [](const MoveEval& e) -> int {
                if (e.has_dtm50) return -e.child_dtm50;
                if (e.has_dtm) return -e.child_dtm;
                return -e.child_dtc;
            };
            std::sort(best.begin(), best.end(), [&](const MoveEval& a, const MoveEval& b) {
                return signed_dist(a) < signed_dist(b);
            });
            break;
        }
        case Result::DRAW: {
            // Heuristic sort, reverse of Python key.  We intentionally do NOT
            // use child_dtc as a tie-breaker here because WDL DRAW children may
            // never have been distance-probed.
            std::sort(best.begin(), best.end(), [](const MoveEval& a, const MoveEval& b) {
                if (a.safe_mat != b.safe_mat) return a.safe_mat > b.safe_mat;
                if (a.exchange_score != b.exchange_score) return a.exchange_score > b.exchange_score;
                if (a.is_rep != b.is_rep) return !a.is_rep;
                return a.central > b.central;
            });
            break;
        }
    }

    return best.front().move;
}

}  // namespace evaluator
