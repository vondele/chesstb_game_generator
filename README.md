# Tablebase-Optimal Endgame Game Generator

This tool generates chess endgame training games that are optimal according to a
Syzygy-like tablebase. It uses:

- **[chesstb](https://github.com/noobpwnftw/chesstb)** for WDL, DTC, and DTM50 probes.
- **[chess-library](https://github.com/Disservin/chess-library)** for board state, legal-move generation, SAN conversion, and
game-over detection.

The generator supports endgames with up to 6 pieces (configurable), including
non-canonical material orientations such as `KvKQ` alongside `KQvK`.

## Tablebase data

The generator needs chesstb tables laid out with `wdl/`, `dtc/`, and `dtm50/` subdirectories. A convenient source is the Hugging Face bucket `noobpwnftw/chesstb`, which can be mounted read-only with `hf-mount` and a local SSD cache:

```bash
RUST_LOG=hf_mount=debug hf-mount \
  --hf-token $(hf auth token) \
  --read-only \
  --cache-dir /mnt/ssd/chesstb/hf_cache \
  --cache-size 50000000000 \
  --read-fetch-timeout-ms 120000 \
  bucket noobpwnftw/chesstb /mnt/ssd/chesstb/hf
```

After mounting, point the generator at the mount point, or at the `full/` subdirectory if you want the raw generator output:

```bash
./generate --chesstb /mnt/ssd/chesstb/hf/full ...
```

There is a size/speed trade-off between the two layouts. Probing the `full/` tables is fast if you have enough local storage to cache them, since every material carries complete frames (no dropped STM colors) and DTM50 is available throughout. If local space is limited, the shipping-format tables provide a space-optimized alternative; the prober then reconstructs dropped STM frames via one-ply derivation where needed.

## Build

### Prerequisites

The build expects two sibling repositories to be checked out next to this project:

- **[chesstb](https://github.com/noobpwnftw/chesstb)** — provides WDL, DTC, and DTM50 probing.
- **[chess-library](https://github.com/Disservin/chess-library)** — header-only board/move library.

With the default Makefile settings the expected layout is:

```text
/workspace/
├── chesstb_game_generator/      # this repository
├── chesstb/                     # tablebase probing library
└── chess-library/               # chess board/move library
```

### Compiling

From this directory run:

```bash
make -j
```

The default target builds `generate`. The Makefile compiles the driver and the
chesstb probe sources separately from the bundled C compression libraries (LZ4,
LZMA, zstd). `chess-library` is used as a header-only dependency.

If your checkouts live elsewhere, override the directory variables on the
command line:

```bash
make -j CHESS_LIBRARY_DIR=/path/to/chess-library CHESSTB_DIR=/path/to/chesstb
```

A few warnings from the bundled C compression libraries are expected and can be
ignored.

## Invocation

```bash
./generate [options]
```

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `--chesstb DIR` | `/dataspace/chesstb_data` | Root directory containing `wdl/`, `dtc/`, and `dtm50/` subdirectories. |
| `--output FILE` | `tb_optimal_games.pgn` | Output PGN file. |
| `--count N` | `1000` | Number of games to generate. |
| `--concurrency N` | Hardware concurrency | Number of worker threads. |
| `--material LIST` | all available | Comma-separated include filter, e.g. `KQvK,KvKQ`. |
| `--exclude LIST` | none | Comma-separated exclude filter. |
| `--results W,D,L` | `W,D,L` | Allowed result classes (`W` = white wins, `D` = draw, `L` = white loses). |
| `--min-plies N` | `5` | Minimum game length for decisive games; minimum length for draw games. |
| `--max-pieces N` | `6` | Maximum total piece count (including both kings). |
| `--cache MiB` | `8192` | Decoded block cache budget for chesstb. |
| `--seed N` | random | RNG seed. |
| `--endgame-counts FILE` | none | Per-material frequency weights. Listed materials are sampled with probability proportional to their count; unlisted pool materials receive weight 0. |
| `--input-fens FILE` | none | Read starting FENs from a file (one per line) instead of generating random positions. Unsupported positions (e.g. castling rights, material not in pool) are skipped. Mutually exclusive with `--endgame-counts`. |

### Examples

Generate 1000 games from the full 6-man pool:

```bash
./generate --count 1000 --max-pieces 6 --output games.pgn
```

Generate only games from queen-versus-king positions, including both
orientations:

```bash
./generate --count 100 --material KQvK,KvKQ --output kqk.pgn
```

Generate only decisive games that last at least 20 plies:

```bash
./generate --count 500 --results W,L --min-plies 20 --output decisive.pgn
```

Generate games with starting-material frequencies taken from a counts file:

```bash
./generate --count 1000 --endgame-counts endgames_counts.txt --output weighted.pgn
```

Generate games from a file of starting FENs:

```bash
./generate --count 10 --input-fens positions.fens --max-pieces 6 --output from_fens.pgn
```

## Implementation Details and Assumptions

### Concurrency

`generate` uses a game-progression scheduler:

1. All starting positions are generated in parallel.
2. Pending positions are kept in a single global priority queue ordered by the
   game-progression relation (`EarlierBoard`), so positions that can only occur
   earlier in a game are processed first.
3. Worker threads pop one position at a time and follow its game line through
   the current material as far as possible before the line changes material.
4. When a move changes material, the resulting position is queued back according
   to its game-progression priority.  When a slot finishes, its PGN is written
   through a `SlotWriter` that preserves strict slot order.

Short draws that fail `--min-plies` are regenerated with the next attempt for
the same slot.

Per-slot seeding is deterministic, so running the same `--seed` and `--count`
with different `--concurrency` values still produces byte-identical output.

### Tablebase Probing

`Probe_Tables` is configured with paths to the `wdl/`, `dtc/`, and `dtm50/`
directories. The probe logic prefers DTM50 when available and falls back to DTC.

Table loading is lazy. The probe functions (`probe`, `probe_wdl`, etc.) are
thread-safe and use chesstb's internal `cached_open()` cache: each missing key
is built by one thread at a time and only the fully constructed object is
published to the shared cache.

Path setup (`add_*_path`) and cache invalidation (`scan_paths`,
`invalidate_tables`) are **not** thread-safe, so this generator adds all paths
before spawning worker threads and never mutates the table configuration during
probing.

### Material Pool

Materials are generated recursively as strings like `KQvK` or `KRBvKNP`. Only
plain pawns (`P`/`p`) are emitted; the frozen-pair pawn marker `p` used by some
checkers-like tablebase notations is not supported.

Availability is determined by canonical tablebase name. The generator converts
each material string to a chesstb `Piece_Config` and calls `Piece_Config::name()`,
so `KQvK` and `KvKQ` both resolve to `KQK` and are kept if `KQK.lzw` exists.

### Move Selection

For every position the generator preserves the current WDL from the
side-to-move's perspective:

- **Winning**: WDL-filter children to losing replies, then pick the one with the
  smallest rule-50-aware DTM50 (fastest mate within the current 50-move window).
- **Losing**: every child is winning for the opponent, so the generator
  distance-probes them all and picks the largest DTM50 (longest delay before
  mate).
- **Drawing**: WDL-filter children to plain draws; if any exist, they are used
  directly without an expensive distance probe.  Otherwise all children are
  distance-probed and those whose rule-50-aware result is a draw are kept.  The
  heuristic tie-breaker favours safe material, favourable exchanges,
  centralization, and avoids repetitions.

Cursed wins and blessed losses are treated as draws, so the generator never
chases a mate that would forfeit under the 50-move rule. The comment attached to
each SAN move reflects the same rule-50-aware evaluation of the position before
the move.

### PGN Output

PGN is rendered manually. Each game includes:

- `Event`, `Site` (material string), `Date`, `White`, `Black`, `Result`, and
  `FEN` tags.
- SAN moves with score comments such as `+M5/245 0.000s`, `-M3/245 0.000s`, or
  `+0.00/245 0.000s` for draws.
- Correct move numbering when the start position has Black to move (`1...`).

### Error Handling

Exceptions raised by chesstb are allowed to propagate to the top-level handler
rather than being silently swallowed, so upstream tablebase or conversion bugs
remain visible.

## Benchmarks

All runs below use `--max-pieces 6 --seed 123` and the rule-50-aware DTM50
move selector.

| Count | Concurrency | `--cache` | Wall time | Aggregate probe throughput | Latency | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|
| 100 | 8 | 8192 MiB | 28.5 s | 200.9 probes/s | 4.977 ms | 14.6 GB |
| 500 | 8 | 8192 MiB | 153.8 s | 149.4 probes/s | 6.694 ms | 35.5 GB |
| 1000 | 4 | 8192 MiB | 412.7 s | 163.4 probes/s | 6.122 ms | 42.2 GB |
| 1000 | 8 | 8192 MiB | 233.2 s | 155.3 probes/s | 6.440 ms | 48.0 GB |
| 1000 | 16 | 8192 MiB | 152.1 s | 147.3 probes/s | 6.790 ms | 53.5 GB |
| 1000 | 8 | 32768 MiB | 206.0 s | 173.3 probes/s | 5.772 ms | 76.1 GB |
| 5000 | 8 | 8192 MiB | 931.3 s | 184.7 probes/s | 5.413 ms | 110.5 GB |

Notes:

- Move selection uses the rule-50-aware DTM50 metric, so mate announcements in
  the PGN match the actual game outcome (verified: 0 inconsistencies in 1000
  games).
- All 1000-game outputs are byte-identical across thread counts and cache sizes
  for the same code version (verified with `diff` between 4, 8, and 16 threads
  and between 8 GiB and 32 GiB caches).
- The WDL-first move filter skips most distance probes for losing positions and
  many distance probes for drawn positions.  The reported per-probe latency is
  higher than the naive full-probe version because the remaining distance probes
  face more cache pressure from the cheap WDL probes; the net effect is still a
  roughly 25-30% wall-time reduction for mixed `W,D,L` workloads.
- Using 16 threads reduces wall time but lowers per-probe throughput due to
  scheduler contention; a larger cache improves throughput at the cost of
  memory.
