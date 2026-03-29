# GroveDB C++ Implementation

Complete C++ implementation of GroveDB v4.0.0 with full Rust parity.
Features SPV proof generation/verification, Merk trees, transactions, batch operations, and replication.

## Build

### CMake (Recommended)

```bash
cmake -S . -B build
cpu_count="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
jobs="$(( cpu_count > 1 ? cpu_count - 1 : 1 ))"
cmake --build build -j"${jobs}"
```

### Autotools (Unix/Linux)

```bash
./autogen.sh
./configure
cpu_count="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
jobs="$(( cpu_count > 1 ? cpu_count - 1 : 1 ))"
make -j"${jobs}"
make check      # Run tests
```

**Configure options:**
- `--with-rocksdb=PATH` - Use RocksDB at PATH (default: auto-detect; RocksDB is required)
- `--enable-debug` - Enable debug build
- `--disable-tests` - Disable test building
- `--disable-benchmarks` - Disable benchmark building

## Run

**CMake build:**
```bash
./build/harness corpus/corpus.json
```

**Autotools build:**
```bash
./src/harness corpus/corpus.json
```

## Tests

Test suite covers API contracts, Rust parity, and functional verification.

**CMake:**
```bash
cd build
ctest --output-on-failure
```

**Autotools:**
```bash
make check
```

Categories:
- **API Contract Tests**: Null/invalid input handling, lifecycle checks
- **Parity Tests**: Rust fixture comparison (storage, Merk, proofs, GroveDB facade)
- **Functional Tests**: Chunk operations, replication, version gating

See [tests/README.md](tests/README.md) for details.

## Benchmarks

Release builds required for meaningful numbers (Rust `cargo bench` is optimized).
Examples below use the CMake release build path (`build-release/`).
For autotools builds, run the binaries from `bench/` after `./configure && make`.

**Insertion (facade-level):**
**CMake:**
```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cpu_count="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
jobs="$(( cpu_count > 1 ? cpu_count - 1 : 1 ))"
cmake --build build-release -j"${jobs}"
./build-release/bench_insertion 10000 10 1 1
```

**Autotools:**
```bash
./configure --disable-tests
cpu_count="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
jobs="$(( cpu_count > 1 ? cpu_count - 1 : 1 ))"
make -j"${jobs}"
./bench/bench_insertion 10000 10 1 1
```

**Insertion (storage microbenchmark):**
**CMake:**
```bash
./build-release/bench_insertion_storage 10000 10 1 1
```

**Autotools:**
```bash
./bench/bench_insertion_storage 10000 10 1 1
```

**Corpus proof generation:**
**CMake:**
```bash
./build-release/bench_corpus corpus/corpus.json 1000
```

**Autotools:**
```bash
./bench/bench_corpus corpus/corpus.json 1000
```

**Rust insertion benchmark (repo-local):**
```bash
bash tools/run_rust_insertion_bench.sh 1 2 10
```

**Rust corpus proof benchmark:**
```bash
CARGO_TARGET_DIR=tools/corpus/target \
cargo bench --manifest-path tools/corpus/Cargo.toml \
  --bench corpus_proof_benchmark -- --warm-up-time 1 --measurement-time 2 --sample-size 10 --noplot
```

## Project Structure

```
├── include/          # Public headers (element, merk, proof, query, etc.)
├── src/              # Implementation (~29.5k LOC)
├── tests/            # Contract + parity + functional tests
├── bench/            # Benchmarks (corpus, insertion, storage)
├── tools/            # Rust fixture generators, test runners
├── docs/             # Feature matrix, policies, performance baselines
├── corpus/           # Canonical test corpus
└── third_party/      # blake3
```

## Documentation

- **[rust-cpp-feature-matrix.md](docs/rust-cpp-feature-matrix.md)** - Feature parity matrix
- **[grovedb-facade-spec.md](docs/grovedb-facade-spec.md)** - Facade API/behavior contract
- **[api-contract-matrix.md](docs/api-contract-matrix.md)** - API coverage
- **[compat-invariants.md](docs/compat-invariants.md)** - Cross-language invariants
- **[acceptance-criteria.md](docs/acceptance-criteria.md)** - Objective cutover gates
- **[accepted-divergences.md](docs/accepted-divergences.md)** - Active divergences
- **[runbook-facade-parity.md](docs/runbook-facade-parity.md)** - Parity/perf execution runbook
- **[perf/](docs/perf/)** - Performance baselines and thresholds

## Tools

- **tools/corpus/** - Rust corpus generator
- **tools/insertion/** - Repo-local Rust insertion benchmark crate
- **tools/rust-storage-tools/** - Rust binaries for parity fixture generation
- **tools/run_parity_ctest.sh** - Test runner (semantic/byte/contract gates)
- **tools/run_rust_insertion_bench.sh** - Rust insertion benchmark runner (writes `docs/perf/raw/`)
- **tools/run_fixture_regen_check.sh** - Canonical corpus determinism + drift check
- **tools/check_*.sh** - Policy enforcement (divergence expiry, perf thresholds, error strings)

## Dependencies

- **RocksDB** - Storage backend (required by the current autotools build; auto-detected by default)
- **blake3** - Hash function (bundled in third_party/)
- **Rust toolchain** - Required for parity tests (grovedb v4.0.0)
