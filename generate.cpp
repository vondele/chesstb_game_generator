#include <chess.hpp>
#include <probe/probe.h>

#include "evaluator.h"
#include "material.h"
#include "material_key.h"
#include "material_queues.h"
#include "options.h"
#include "slot_generator.h"
#include "slot_state.h"
#include "slot_writer.h"
#include "timers.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

std::string current_date() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    gmtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y.%m.%d", &tm);
    return buf;
}

std::set<char> parse_results(const std::string& s) {
    std::set<char> r;
    for (char c : s) {
        char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (u == 'W' || u == 'D' || u == 'L') r.insert(u);
    }
    return r;
}

namespace {

void advance_slot_in_material(SlotState& state,
                              const std::string& key,
                              Probe_Tables& tables,
                              Timers& timers,
                              std::vector<SlotState>& finished,
                              std::map<std::string, std::vector<SlotState>>& other_children) {
    while (true) {
        // Terminal before move: finalize.
        if (state.plies > 1000 ||
            state.board.isGameOver().second != chess::GameResult::NONE) {
            finished.push_back(std::move(state));
            return;
        }

        evaluator::ProbeInfo current = evaluator::probe_board(&timers, tables, state.board);
        auto move_opt = evaluator::select_move(&timers, tables, state.board, current);
        if (!move_opt) {
            finished.push_back(std::move(state));
            return;
        }

        chess::Move move = *move_opt;
        std::string san = chess::uci::moveToSan(state.board, move);
        std::string comment = pgn::score_comment(current);
        state.game.moves.emplace_back(san, comment);

        state.board.makeMove(move);
        ++state.plies;

        // Terminal after move: finalize.
        if (state.plies > 1000 ||
            state.board.isGameOver().second != chess::GameResult::NONE) {
            finished.push_back(std::move(state));
            return;
        }

        std::string child_key = profile_from_board(state.board);
        if (child_key != key) {
            other_children[child_key].push_back(std::move(state));
            return;
        }
        // Same material: continue following this slot's line.
    }
}

void process_batch(const std::string& key,
                   std::vector<SlotState>& batch,
                   const Options& opt,
                   const std::vector<std::string>& materials,
                   const std::set<char>& allowed,
                   Probe_Tables& tables,
                   MaterialQueues& queues,
                   SlotWriter& writer,
                   Timers& timers,
                   const std::string& date,
                   std::atomic<size_t>& finished_count,
                   std::mutex& print_mutex,
                   const material::WeightedSampler* material_sampler) {
    std::vector<SlotState> finished;
    std::map<std::string, std::vector<SlotState>> other_children;

    finished.reserve(batch.size());

    for (SlotState& state : batch) {
        advance_slot_in_material(state, key, tables, timers,
                                 finished, other_children);
    }

    // Route children of other materials to their queues.
    for (auto& kv : other_children) {
        queues.push_all(kv.first, std::move(kv.second));
    }

    // Finalize finished slots.  Short draws are regenerated with the next attempt.
    for (SlotState& state : finished) {
        if (state.initial_result == evaluator::Result::DRAW && state.plies < opt.min_plies &&
            opt.input_fens_path.empty()) {
            auto regen = generate_slot_start(state.slot, state.attempt + 1, opt, materials,
                                             allowed, tables, date, &timers,
                                             material_sampler);
            if (!regen) {
                throw std::runtime_error("Failed to regenerate draw slot " +
                                         std::to_string(state.slot));
            }
            queues.push(profile_from_board(regen->board), std::move(*regen));
        } else {
            state.game.ply_count = state.plies;
            state.game.result = pgn::result_string(state.board);
            writer.write(state.slot, pgn::render(state.game));
            size_t done = finished_count.fetch_add(1) + 1;
            if (done % 10 == 0 || done >= opt.count) {
                std::lock_guard<std::mutex> lk(print_mutex);
                std::cout << "\rGenerated game " << done << "/" << opt.count << std::flush;
            }
        }
    }
}

}  // namespace

void worker_loop(const Options& opt,
                 const std::vector<std::string>& materials,
                 const std::set<char>& allowed,
                 Probe_Tables& tables,
                 MaterialQueues& queues,
                 SlotWriter& writer,
                 Timers& timers,
                 const std::string& date,
                 std::atomic<size_t>& finished_count,
                 std::mutex& print_mutex,
                 const material::WeightedSampler* material_sampler) {
    while (true) {
        auto batch = queues.pop_next();
        if (!batch) break;
        process_batch(batch->first, batch->second, opt, materials, allowed,
                      tables, queues, writer, timers, date, finished_count,
                      print_mutex, material_sampler);
    }
}

int main(int argc, char** argv) {
    try {
        Options opt = parse_args(argc, argv);
        if (!opt.input_fens_path.empty() && !opt.endgame_counts_path.empty()) {
            std::cerr << "Error: --input-fens and --endgame-counts are mutually exclusive.\n";
            return 1;
        }
        if (opt.seed == 0) opt.seed = std::random_device{}();

        auto materials = material::generate_combinations(opt.max_pieces,
                                                          opt.material_include,
                                                          opt.material_exclude);
        if (materials.empty()) {
            std::cerr << "Error: No valid material filters found.\n";
            return 1;
        }

        materials = material::filter_available(materials, opt.chesstb_dir / "wdl");
        if (materials.empty()) {
            std::cerr << "Error: No requested materials have available chesstb tables in "
                      << opt.chesstb_dir / "wdl" << "\n";
            return 1;
        }

        std::set<std::string> canonical_materials;
        for (const auto& mat : materials) {
            canonical_materials.insert(material::canonical_chesstb_name(mat));
        }

        const material::WeightedSampler* material_sampler_ptr = nullptr;
        std::optional<material::WeightedSampler> material_sampler;
        if (!opt.endgame_counts_path.empty()) {
            std::string err;
            auto wres = material::load_endgame_weights(opt.endgame_counts_path, materials, err);
            if (!wres) {
                std::cerr << "Error: " << err << "\n";
                return 1;
            }
            std::cout << "Loaded weights for " << wres->matched << "/" << materials.size()
                      << " pool materials from " << opt.endgame_counts_path << ".\n";
            material_sampler.emplace(wres->weights);
            if (!material_sampler->valid()) {
                std::cerr << "Error: No positive endgame weights match the current material pool\n";
                return 1;
            }
            material_sampler_ptr = &*material_sampler;
        }

        std::set<char> allowed = parse_results(opt.results);

        std::cout << "Pool size: " << materials.size() << " material profiles.\n";
        std::cout << "Generating " << opt.count << " games matching ["
                  << opt.results << "] (min " << opt.min_plies
                  << " plies) on " << opt.concurrency << " cores...\n";

        Probe_Tables tables;
        tables.add_wdl_path(opt.chesstb_dir / "wdl");
        tables.add_dtc_path(opt.chesstb_dir / "dtc");
        tables.add_dtm50_path(opt.chesstb_dir / "dtm50");
        tables.set_block_cache_bytes(opt.cache_mib * 1024 * 1024);

        if (!opt.output.parent_path().empty()) {
            fs::create_directories(opt.output.parent_path());
        }

        std::ofstream out(opt.output, std::ios::binary);
        if (!out) {
            std::cerr << "Failed to open output file: " << opt.output << "\n";
            return 1;
        }
        SlotWriter writer(out);

        std::string date = current_date();
        MaterialQueues queues;
        Timers timers;
        std::atomic<size_t> finished_count{0};
        std::mutex print_mutex;

        auto start = std::chrono::steady_clock::now();

        // Phase 1: produce starting positions.
        if (!opt.input_fens_path.empty()) {
            std::string err;
            auto fens = load_fens(opt.input_fens_path, err);
            if (fens.empty()) {
                throw std::runtime_error(err);
            }
            std::cout << "Loaded " << fens.size() << " FENs from " << opt.input_fens_path
                      << ".\n";

            std::vector<SlotState> starts;
            starts.reserve(opt.count);
            for (const auto& fen : fens) {
                if (starts.size() >= opt.count) break;
                auto state = create_slot_from_fen(starts.size(), fen, canonical_materials,
                                                  allowed, tables, date, &timers);
                if (!state) continue;
                starts.push_back(std::move(*state));
            }
            if (starts.size() < opt.count) {
                throw std::runtime_error("Only " + std::to_string(starts.size()) +
                                         " valid FENs found, needed " + std::to_string(opt.count));
            }
            for (auto& s : starts) {
                queues.push(profile_from_board(s.board), std::move(s));
            }
        } else {
            // Phase 1: generate all starting positions in parallel.
            std::vector<std::optional<SlotState>> starts(opt.count);
            std::atomic<size_t> next_slot{0};
            std::atomic<bool> gen_error{false};
            std::string gen_error_msg;
            std::mutex gen_error_mu;
            std::vector<std::thread> gen_threads;

            auto gen_worker = [&]() {
                try {
                    while (!gen_error.load(std::memory_order_acquire)) {
                        size_t slot = next_slot.fetch_add(1, std::memory_order_relaxed);
                        if (slot >= opt.count) break;
                        auto state = generate_slot_start(slot, 0, opt, materials, allowed,
                                                         tables, date, &timers,
                                                         material_sampler_ptr);
                        if (!state) {
                            gen_error.store(true, std::memory_order_release);
                            std::lock_guard<std::mutex> lk(gen_error_mu);
                            gen_error_msg = "Failed to generate starting position for slot " +
                                            std::to_string(slot);
                            break;
                        }
                        starts[slot] = std::move(*state);
                    }
                } catch (const std::exception& e) {
                    gen_error.store(true, std::memory_order_release);
                    std::lock_guard<std::mutex> lk(gen_error_mu);
                    gen_error_msg = e.what();
                }
            };

            for (int i = 0; i < opt.concurrency; ++i) {
                gen_threads.emplace_back(gen_worker);
            }
            for (auto& t : gen_threads) t.join();

            if (gen_error.load(std::memory_order_acquire)) {
                throw std::runtime_error(gen_error_msg);
            }

            for (size_t slot = 0; slot < opt.count; ++slot) {
                if (!starts[slot]) {
                    throw std::runtime_error("Missing starting position for slot " +
                                             std::to_string(slot));
                }
                queues.push(profile_from_board(starts[slot]->board),
                            std::move(*starts[slot]));
            }
        }

        // Phase 2: drain queues with a worker pool.
        std::vector<std::thread> workers;
        for (int i = 0; i < opt.concurrency; ++i) {
            workers.emplace_back(worker_loop, std::cref(opt), std::cref(materials),
                                 std::cref(allowed), std::ref(tables), std::ref(queues),
                                 std::ref(writer), std::ref(timers), std::cref(date),
                                 std::ref(finished_count), std::ref(print_mutex),
                                 material_sampler_ptr);
        }
        for (auto& t : workers) t.join();

        writer.flush();

        auto end = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(end - start).count();

        // Print probe throughput report.
        uint64_t probe_ns = timers.probe_ns.load();
        uint64_t probe_count = timers.probe_count.load();
        double probe_s = probe_ns / 1e9;
        double probes_per_s = probe_s > 0 ? (probe_count / probe_s) : 0.0;
        double ms_per_probe = probe_count > 0 ? (probe_s * 1000.0 / probe_count) : 0.0;

        uint64_t wdl_probe_ns = timers.wdl_probe_ns.load();
        uint64_t wdl_probe_count = timers.wdl_probe_count.load();
        double wdl_probe_s = wdl_probe_ns / 1e9;
        double wdl_probes_per_s = wdl_probe_s > 0 ? (wdl_probe_count / wdl_probe_s) : 0.0;
        double wdl_ms_per_probe = wdl_probe_count > 0 ? (wdl_probe_s * 1000.0 / wdl_probe_count) : 0.0;

        std::cout << "\nDistance probe report:\n"
                  << "  probe calls:    " << probe_count << "\n"
                  << "  probe time:     " << std::fixed << std::setprecision(1) << probe_s << " s\n"
                  << "  throughput:     " << std::setprecision(1) << probes_per_s << " probes/s\n"
                  << "  latency:        " << std::setprecision(3) << ms_per_probe << " ms/probe\n";

        std::cout << "\nWDL probe report:\n"
                  << "  wdl calls:      " << wdl_probe_count << "\n"
                  << "  wdl time:       " << std::fixed << std::setprecision(1) << wdl_probe_s << " s\n"
                  << "  throughput:     " << std::setprecision(1) << wdl_probes_per_s << " probes/s\n"
                  << "  latency:        " << std::setprecision(3) << wdl_ms_per_probe << " ms/probe\n";

        std::cout << "\nCompleted " << finished_count.load() << " games in "
                  << std::fixed << std::setprecision(1) << secs << "s. Output saved to "
                  << opt.output << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
