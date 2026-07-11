CXX ?= c++
BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= 4

BENCHMARK_PHASE ?= optimized
BENCHMARK_THREADS ?= 4
SOLVER_TIME_LIMIT ?= 90
BENCHMARK_TIMEOUT ?= 105

.PHONY: all configure build test benchmark clean help

all: build

configure:
	cmake -S . -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DCMAKE_CXX_COMPILER="$(CXX)"

build: configure
	cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"

test: build
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure

benchmark: build
	python3 benchmark.py "$(BENCHMARK_PHASE)" \
		--executable "$(BUILD_DIR)/main" \
		--threads "$(BENCHMARK_THREADS)" \
		--solver-time-limit "$(SOLVER_TIME_LIMIT)" \
		--timeout "$(BENCHMARK_TIMEOUT)"

clean:
	cmake -E remove_directory "$(BUILD_DIR)"

help:
	@echo "Targets:"
	@echo "  make              Configure and build the Release executable"
	@echo "  make test         Build and run the regression tests"
	@echo "  make benchmark    Build and run the ten-instance benchmark"
	@echo "  make clean        Remove BUILD_DIR"
	@echo ""
	@echo "Useful overrides:"
	@echo "  CXX, BUILD_DIR, BUILD_TYPE, JOBS"
	@echo "  BENCHMARK_PHASE, BENCHMARK_THREADS, SOLVER_TIME_LIMIT, BENCHMARK_TIMEOUT"
