LLVM_CXX_CANDIDATE := $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang++ /usr/local/opt/llvm/bin/clang++))
GCC_CXX_CANDIDATE := $(firstword $(wildcard /opt/homebrew/bin/g++ /usr/local/bin/g++))
LLVM_CXX ?= $(if $(LLVM_CXX_CANDIDATE),$(LLVM_CXX_CANDIDATE),clang++)
GCC_CXX ?= $(if $(GCC_CXX_CANDIDATE),$(GCC_CXX_CANDIDATE),g++)
PREFERRED_CXX := $(if $(LLVM_CXX_CANDIDATE),$(LLVM_CXX),$(GCC_CXX))
ifeq ($(origin CXX),default)
CXX := $(PREFERRED_CXX)
endif
CXX ?= c++
BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= 4
LLVM_BUILD_DIR ?= build-llvm
GCC_BUILD_DIR ?= build-gcc
FAST_BUILD_DIR ?= build-fast
LLVM_FAST_BUILD_DIR ?= build-llvm-fast
GCC_FAST_BUILD_DIR ?= build-gcc-fast
LLVM_PGO_BUILD_DIR ?= build-llvm-pgo
GCC_PGO_BUILD_DIR ?= build-gcc-pgo
LLVM_PGO_DATA_DIR ?= $(LLVM_PGO_BUILD_DIR)/pgo-data
GCC_PGO_DATA_DIR ?= $(GCC_PGO_BUILD_DIR)/pgo-data
LLVM_VERSION_MAJOR := $(shell $(LLVM_CXX) -dumpversion 2>/dev/null | cut -d. -f1)
GCC_VERSION_MAJOR := $(shell $(GCC_CXX) -dumpfullversion -dumpversion 2>/dev/null | cut -d. -f1)

LLVM_BACKEND ?= tbb
GCC_BACKEND ?= tbb

LLVM_BENCHMARK_PHASE ?= clang$(LLVM_VERSION_MAJOR)_$(LLVM_BACKEND)
GCC_BENCHMARK_PHASE ?= gcc$(GCC_VERSION_MAJOR)_$(GCC_BACKEND)

LLVM_PROFDATA ?= $(shell \
	compiler_path="$$(command -v "$(LLVM_CXX)" 2>/dev/null)"; \
	compiler_dir="$$(dirname "$$compiler_path")"; \
	if [ -x "$$compiler_dir/llvm-profdata" ]; then \
		printf '%s\n' "$$compiler_dir/llvm-profdata"; \
	elif command -v "llvm-profdata-$(LLVM_VERSION_MAJOR)" >/dev/null 2>&1; then \
		command -v "llvm-profdata-$(LLVM_VERSION_MAJOR)"; \
	else \
		command -v llvm-profdata 2>/dev/null; \
	fi)
EXTRA_CMAKE_ARGS ?=

PGO_DENSE_INSTANCE ?= dataset/MDP/MDG-a_n2000/MDG-a_21_n2000_m200.txt
PGO_SPARSE_INSTANCE ?= dataset/GBP/crack.graph.qsc
PGO_GBP_PATH_INSTANCE ?= dataset/GBP/bcsstk29.graph.qsc
PGO_GBP_CONSTRUCTION_INSTANCE ?= dataset/GBP/cti.graph.qsc
PGO_DENSE_STARTS ?= 12
PGO_SPARSE_STARTS ?= 8
PGO_GBP_PATH_STARTS ?= 20
PGO_GBP_CONSTRUCTION_STARTS ?= 20

BENCHMARK_PHASE ?= optimized
BENCHMARK_THREADS ?= 4
BENCHMARK_LANES_PER_WORKER ?= 1
BENCHMARK_STARTS ?= 200000
SOLVER_TIME_LIMIT ?= 90
BENCHMARK_TIMEOUT ?= 105
DETERMINISM_STARTS ?= 4
DETERMINISM_TIMEOUT ?= 180
GBP_BENCHMARK_PHASE ?= gbp10_fixed230_gcc
GBP_PGO_BENCHMARK_PHASE ?= gbp10_fixed230_pgo_gcc
GBP_PORTFOLIO_PHASE ?= gbp10_portfolio4_gcc
GBP_BENCHMARK_THREADS ?= 10
GBP_BENCHMARK_REPETITIONS ?= 3
GBP_BENCHMARK_STARTS ?= 230
GBP_ELITE_GROUP_SIZE ?= 0
GBP_PORTFOLIO_GROUP_SIZE ?= 4
GBP_CONSTRUCTION_ATTEMPTS ?= 200
GBP_BENCHMARK_TIMEOUT ?= 300

.PHONY: all configure build build-llvm build-gcc build-fast build-llvm-fast build-gcc-fast \
	pgo-gcc pgo-llvm test test-compilers benchmark benchmark-fast \
	benchmark-pgo benchmark-pgo-llvm benchmark-pgo-gcc \
	benchmark-llvm benchmark-gcc benchmark-compilers benchmark-gbp-fixed benchmark-gbp-pgo-gcc \
	benchmark-gbp-portfolio \
	check-determinism clean help

all: build

configure:
	cmake -S . -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DCMAKE_CXX_COMPILER="$(CXX)" \
		$(EXTRA_CMAKE_ARGS)

build: configure
	cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"

build-llvm:
	$(MAKE) build CXX="$(LLVM_CXX)" BUILD_DIR="$(LLVM_BUILD_DIR)"

build-gcc:
	$(MAKE) build CXX="$(GCC_CXX)" BUILD_DIR="$(GCC_BUILD_DIR)"

build-fast:
	$(MAKE) build-llvm-fast

build-llvm-fast:
	$(MAKE) build CXX="$(LLVM_CXX)" BUILD_DIR="$(LLVM_FAST_BUILD_DIR)" \
		EXTRA_CMAKE_ARGS="-DFCBQP_ENABLE_LTO=ON -DFCBQP_NATIVE_ARCH=ON"

build-gcc-fast:
	$(MAKE) build CXX="$(GCC_CXX)" BUILD_DIR="$(GCC_FAST_BUILD_DIR)" \
		EXTRA_CMAKE_ARGS="-DFCBQP_ENABLE_LTO=ON -DFCBQP_NATIVE_ARCH=ON"

pgo-gcc:
	cmake -E remove_directory "$(GCC_PGO_DATA_DIR)"
	cmake -S . -B "$(GCC_PGO_BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_CXX_COMPILER="$(GCC_CXX)" -DFCBQP_NATIVE_ARCH=ON \
		-DFCBQP_PGO_GENERATE=ON -DFCBQP_PGO_USE=OFF \
		-DFCBQP_PGO_DIR="$(abspath $(GCC_PGO_DATA_DIR))"
	cmake --build "$(GCC_PGO_BUILD_DIR)" --parallel "$(JOBS)"
	"$(GCC_PGO_BUILD_DIR)/main" max half "$(PGO_DENSE_INSTANCE)" '' 1 1 0 auto 4 "$(PGO_DENSE_STARTS)" >/dev/null
	"$(GCC_PGO_BUILD_DIR)/main" min full "$(PGO_SPARSE_INSTANCE)" '' 1 1 0 auto 4 "$(PGO_SPARSE_STARTS)" >/dev/null
	"$(GCC_PGO_BUILD_DIR)/main" min full "$(PGO_GBP_PATH_INSTANCE)" '' 1 1 0 auto 10 "$(PGO_GBP_PATH_STARTS)" >/dev/null
	"$(GCC_PGO_BUILD_DIR)/main" min full "$(PGO_GBP_CONSTRUCTION_INSTANCE)" '' 1 1 0 auto 10 "$(PGO_GBP_CONSTRUCTION_STARTS)" >/dev/null
	cmake -S . -B "$(GCC_PGO_BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_CXX_COMPILER="$(GCC_CXX)" -DFCBQP_NATIVE_ARCH=ON -DFCBQP_ENABLE_LTO=ON \
		-DFCBQP_PGO_GENERATE=OFF -DFCBQP_PGO_USE=ON \
		-DFCBQP_PGO_DIR="$(abspath $(GCC_PGO_DATA_DIR))"
	cmake --build "$(GCC_PGO_BUILD_DIR)" --parallel "$(JOBS)" --clean-first

pgo-llvm:
	cmake -E remove_directory "$(LLVM_PGO_DATA_DIR)"
	cmake -E make_directory "$(LLVM_PGO_DATA_DIR)"
	cmake -S . -B "$(LLVM_PGO_BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_CXX_COMPILER="$(LLVM_CXX)" -DFCBQP_NATIVE_ARCH=ON \
		-DFCBQP_PGO_GENERATE=ON -DFCBQP_PGO_USE=OFF \
		-DFCBQP_PGO_DIR="$(abspath $(LLVM_PGO_DATA_DIR))"
	cmake --build "$(LLVM_PGO_BUILD_DIR)" --parallel "$(JOBS)"
	LLVM_PROFILE_FILE="$(abspath $(LLVM_PGO_DATA_DIR))/dense-%p.profraw" \
		"$(LLVM_PGO_BUILD_DIR)/main" max half "$(PGO_DENSE_INSTANCE)" '' 1 1 0 auto 4 "$(PGO_DENSE_STARTS)" >/dev/null
	LLVM_PROFILE_FILE="$(abspath $(LLVM_PGO_DATA_DIR))/sparse-%p.profraw" \
		"$(LLVM_PGO_BUILD_DIR)/main" min full "$(PGO_SPARSE_INSTANCE)" '' 1 1 0 auto 4 "$(PGO_SPARSE_STARTS)" >/dev/null
	"$(LLVM_PROFDATA)" merge -output="$(LLVM_PGO_DATA_DIR)/merged.profdata" "$(LLVM_PGO_DATA_DIR)"/*.profraw
	cmake -S . -B "$(LLVM_PGO_BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_CXX_COMPILER="$(LLVM_CXX)" -DFCBQP_NATIVE_ARCH=ON -DFCBQP_ENABLE_LTO=ON \
		-DFCBQP_PGO_GENERATE=OFF -DFCBQP_PGO_USE=ON \
		-DFCBQP_PGO_DIR="$(abspath $(LLVM_PGO_DATA_DIR))"
	cmake --build "$(LLVM_PGO_BUILD_DIR)" --parallel "$(JOBS)" --clean-first

test: build
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure

test-compilers:
	$(MAKE) test CXX="$(LLVM_CXX)" BUILD_DIR="$(LLVM_BUILD_DIR)"
	$(MAKE) test CXX="$(GCC_CXX)" BUILD_DIR="$(GCC_BUILD_DIR)"

benchmark: build
	python3 benchmark.py "$(BENCHMARK_PHASE)" \
		--executable "$(BUILD_DIR)/main" \
		--threads "$(BENCHMARK_THREADS)" \
		--epoch-lanes-per-worker "$(BENCHMARK_LANES_PER_WORKER)" \
		--starts "$(BENCHMARK_STARTS)" \
		--solver-time-limit "$(SOLVER_TIME_LIMIT)" \
		--timeout "$(BENCHMARK_TIMEOUT)"

benchmark-fast: build-fast
	$(MAKE) benchmark CXX="$(LLVM_CXX)" BUILD_DIR="$(LLVM_FAST_BUILD_DIR)" \
		EXTRA_CMAKE_ARGS="-DFCBQP_ENABLE_LTO=ON -DFCBQP_NATIVE_ARCH=ON" \
		BENCHMARK_PHASE=optimized_fast

benchmark-pgo: benchmark-pgo-llvm benchmark-pgo-gcc

benchmark-pgo-llvm: pgo-llvm
	python3 benchmark.py optimized_pgo_llvm \
		--executable "$(LLVM_PGO_BUILD_DIR)/main" \
		--threads "$(BENCHMARK_THREADS)" \
		--epoch-lanes-per-worker "$(BENCHMARK_LANES_PER_WORKER)" \
		--starts "$(BENCHMARK_STARTS)" \
		--solver-time-limit "$(SOLVER_TIME_LIMIT)" \
		--timeout "$(BENCHMARK_TIMEOUT)"

benchmark-pgo-gcc: pgo-gcc
	python3 benchmark.py optimized_pgo_gcc \
		--executable "$(GCC_PGO_BUILD_DIR)/main" \
		--threads "$(BENCHMARK_THREADS)" \
		--epoch-lanes-per-worker "$(BENCHMARK_LANES_PER_WORKER)" \
		--starts "$(BENCHMARK_STARTS)" \
		--solver-time-limit "$(SOLVER_TIME_LIMIT)" \
		--timeout "$(BENCHMARK_TIMEOUT)"

benchmark-llvm:
	$(MAKE) benchmark CXX="$(LLVM_CXX)" BUILD_DIR="$(LLVM_BUILD_DIR)" \
		BENCHMARK_PHASE="$(LLVM_BENCHMARK_PHASE)"

benchmark-gcc:
	$(MAKE) benchmark CXX="$(GCC_CXX)" BUILD_DIR="$(GCC_BUILD_DIR)" \
		BENCHMARK_PHASE="$(GCC_BENCHMARK_PHASE)"

benchmark-compilers: benchmark-llvm benchmark-gcc

benchmark-gbp-fixed: build-gcc-fast
	python3 benchmark_gbp.py "$(GBP_BENCHMARK_PHASE)" \
		--executable "$(GCC_FAST_BUILD_DIR)/main" \
		--threads "$(GBP_BENCHMARK_THREADS)" \
		--repetitions "$(GBP_BENCHMARK_REPETITIONS)" \
		--starts "$(GBP_BENCHMARK_STARTS)" \
		--elite-group-size "$(GBP_ELITE_GROUP_SIZE)" \
		--construction-attempts "$(GBP_CONSTRUCTION_ATTEMPTS)" \
		--solver-time-limit 0 \
		--timeout "$(GBP_BENCHMARK_TIMEOUT)"
	python3 tests/check_repeated_benchmark.py \
		"benchmark_results/$(GBP_BENCHMARK_PHASE).csv"

benchmark-gbp-pgo-gcc: pgo-gcc
	python3 benchmark_gbp.py "$(GBP_PGO_BENCHMARK_PHASE)" \
		--executable "$(GCC_PGO_BUILD_DIR)/main" \
		--threads "$(GBP_BENCHMARK_THREADS)" \
		--repetitions "$(GBP_BENCHMARK_REPETITIONS)" \
		--starts "$(GBP_BENCHMARK_STARTS)" \
		--elite-group-size "$(GBP_ELITE_GROUP_SIZE)" \
		--construction-attempts "$(GBP_CONSTRUCTION_ATTEMPTS)" \
		--solver-time-limit 0 \
		--timeout "$(GBP_BENCHMARK_TIMEOUT)"
	python3 tests/check_repeated_benchmark.py \
		"benchmark_results/$(GBP_PGO_BENCHMARK_PHASE).csv"

benchmark-gbp-portfolio: build-gcc-fast
	python3 benchmark_gbp.py "$(GBP_PORTFOLIO_PHASE)" \
		--executable "$(GCC_FAST_BUILD_DIR)/main" \
		--threads "$(GBP_BENCHMARK_THREADS)" \
		--repetitions "$(GBP_BENCHMARK_REPETITIONS)" \
		--starts "$(GBP_BENCHMARK_STARTS)" \
		--elite-group-size "$(GBP_PORTFOLIO_GROUP_SIZE)" \
		--construction-attempts "$(GBP_CONSTRUCTION_ATTEMPTS)" \
		--solver-time-limit 0 \
		--timeout "$(GBP_BENCHMARK_TIMEOUT)" \
		--instance crack.graph \
		--instance cti.graph
	python3 tests/check_repeated_benchmark.py \
		"benchmark_results/$(GBP_PORTFOLIO_PHASE).csv"

check-determinism:
	$(MAKE) benchmark CXX="$(LLVM_CXX)" BUILD_DIR="$(LLVM_BUILD_DIR)" \
		BENCHMARK_PHASE=deterministic_llvm BENCHMARK_STARTS="$(DETERMINISM_STARTS)" \
		SOLVER_TIME_LIMIT=0 BENCHMARK_TIMEOUT="$(DETERMINISM_TIMEOUT)"
	$(MAKE) benchmark CXX="$(GCC_CXX)" BUILD_DIR="$(GCC_BUILD_DIR)" \
		BENCHMARK_PHASE=deterministic_gcc BENCHMARK_STARTS="$(DETERMINISM_STARTS)" \
		SOLVER_TIME_LIMIT=0 BENCHMARK_TIMEOUT="$(DETERMINISM_TIMEOUT)"
	python3 tests/compare_benchmarks.py \
		benchmark_results/deterministic_llvm.csv \
		benchmark_results/deterministic_gcc.csv

clean:
	cmake -E remove_directory "$(BUILD_DIR)"

help:
	@echo "Targets:"
	@echo "  make              Configure and build the Release executable"
	@echo "  make build-llvm    Build with LLVM/libdispatch"
	@echo "  make build-gcc     Build with GCC/TBB"
	@echo "  make build-fast    Preferred LLVM build with LTO and native CPU tuning"
	@echo "  make build-llvm-fast / build-gcc-fast  Compiler-specific LTO/native builds"
	@echo "  make pgo-gcc / pgo-llvm  Train and build a profile-guided executable"
	@echo "  make test         Build and run the regression tests"
	@echo "  make test-compilers  Test both compiler/backend combinations"
	@echo "  make benchmark    Build and run the ten-instance benchmark"
	@echo "  make benchmark-compilers  Benchmark LLVM and GCC sequentially"
	@echo "  make benchmark-fast  Benchmark the LLVM LTO/native build"
	@echo "  make benchmark-gbp-fixed  Run the deterministic 10-core GBP benchmark"
	@echo "  make benchmark-gbp-pgo-gcc  Train GCC PGO and run the GBP benchmark"
	@echo "  make benchmark-gbp-portfolio  Run the deterministic crack/cti portfolio"
	@echo "  make benchmark-pgo   Train and benchmark both PGO builds"
	@echo "  make check-determinism  Compare fixed-work LLVM and GCC results"
	@echo "  make clean        Remove BUILD_DIR"
	@echo ""
	@echo "Useful overrides:"
	@echo "  CXX, LLVM_CXX, GCC_CXX, BUILD_DIR, LLVM_BUILD_DIR, GCC_BUILD_DIR, BUILD_TYPE, JOBS"
	@echo "  FAST_BUILD_DIR, LLVM_FAST_BUILD_DIR, GCC_FAST_BUILD_DIR, EXTRA_CMAKE_ARGS"
	@echo "  PGO_DENSE_INSTANCE, PGO_SPARSE_INSTANCE, PGO_GBP_PATH_INSTANCE, PGO_GBP_CONSTRUCTION_INSTANCE"
	@echo "  PGO_DENSE_STARTS, PGO_SPARSE_STARTS, PGO_GBP_PATH_STARTS, PGO_GBP_CONSTRUCTION_STARTS"
	@echo "  BENCHMARK_PHASE, BENCHMARK_THREADS, BENCHMARK_LANES_PER_WORKER, BENCHMARK_STARTS"
	@echo "  SOLVER_TIME_LIMIT, BENCHMARK_TIMEOUT"
	@echo "  DETERMINISM_STARTS, DETERMINISM_TIMEOUT"
	@echo "  GBP_BENCHMARK_PHASE, GBP_PGO_BENCHMARK_PHASE, GBP_PORTFOLIO_PHASE"
	@echo "  GBP_BENCHMARK_THREADS, GBP_BENCHMARK_REPETITIONS, GBP_PORTFOLIO_GROUP_SIZE"
	@echo "  GBP_BENCHMARK_STARTS, GBP_ELITE_GROUP_SIZE, GBP_CONSTRUCTION_ATTEMPTS, GBP_BENCHMARK_TIMEOUT"
