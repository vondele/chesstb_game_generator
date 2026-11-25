#pragma once

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct Options {
    fs::path chesstb_dir = "/dataspace/chesstb_data";
    fs::path output = "tb_optimal_games.pgn";
    size_t count = 1000;
    int concurrency = static_cast<int>(std::thread::hardware_concurrency());
    std::vector<std::string> material_include;
    std::vector<std::string> material_exclude;
    std::string results = "W,D,L";
    int min_plies = 5;
    int max_pieces = 6;
    size_t cache_mib = 8192;
    unsigned seed = 0;
    fs::path endgame_counts_path;
    fs::path input_fens_path;
};

inline Options parse_args(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--chesstb" && i + 1 < argc) {
            opt.chesstb_dir = argv[++i];
        } else if (a == "--output" && i + 1 < argc) {
            opt.output = argv[++i];
        } else if (a == "--count" && i + 1 < argc) {
            opt.count = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--concurrency" && i + 1 < argc) {
            opt.concurrency = std::atoi(argv[++i]);
        } else if (a == "--material" && i + 1 < argc) {
            std::string s = argv[++i];
            size_t start = 0;
            while (start < s.size()) {
                size_t end = s.find(',', start);
                if (end == std::string::npos) end = s.size();
                opt.material_include.push_back(s.substr(start, end - start));
                start = end + 1;
            }
        } else if (a == "--exclude" && i + 1 < argc) {
            std::string s = argv[++i];
            size_t start = 0;
            while (start < s.size()) {
                size_t end = s.find(',', start);
                if (end == std::string::npos) end = s.size();
                opt.material_exclude.push_back(s.substr(start, end - start));
                start = end + 1;
            }
        } else if (a == "--results" && i + 1 < argc) {
            opt.results = argv[++i];
        } else if (a == "--min-plies" && i + 1 < argc) {
            opt.min_plies = std::atoi(argv[++i]);
        } else if (a == "--max-pieces" && i + 1 < argc) {
            opt.max_pieces = std::atoi(argv[++i]);
        } else if (a == "--cache" && i + 1 < argc) {
            opt.cache_mib = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--seed" && i + 1 < argc) {
            opt.seed = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--endgame-counts" && i + 1 < argc) {
            opt.endgame_counts_path = argv[++i];
        } else if (a == "--input-fens" && i + 1 < argc) {
            opt.input_fens_path = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --chesstb DIR       chesstb root with wdl/dtc/dtm50 subdirs\n"
                      << "  --output FILE       output PGN (default: tb_optimal_games.pgn)\n"
                      << "  --count N           games to generate\n"
                      << "  --concurrency N     worker threads\n"
                      << "  --material LIST     comma-separated include filter\n"
                      << "  --exclude LIST      comma-separated exclude filter\n"
                      << "  --results W,D,L     allowed results\n"
                      << "  --min-plies N       minimum plies for decisive games\n"
                      << "  --max-pieces N      maximum total pieces (default 6)\n"
                      << "  --cache MiB         decoded block cache budget (default 8192)\n"
                      << "  --seed N            RNG seed\n"
                      << "  --endgame-counts FILE   per-material frequency weights\n"
                      << "  --input-fens FILE   read starting FENs from file (one per line)\n";
            std::exit(0);
        } else {
            std::cerr << "Unknown option: " << a << "\n";
            std::exit(2);
        }
    }
    return opt;
}
