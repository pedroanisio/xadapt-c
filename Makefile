# Build and validate the standalone C codec from the workspace root.
# make: library and CLI; make test: C tests; make check: build and all tests.
PYTHON ?= python3
BUILD ?= build
CODEC_BUILD = $(if $(filter /%,$(BUILD)),$(BUILD),adpat/c/$(BUILD))
BENCH_ARGS ?=
.PHONY: all test check clean bench plot

all test check clean:
	$(MAKE) -C adpat/c $@

# Optional corpus measurements; ordinary builds do not run benchmarks.
bench: all
	$(PYTHON) -B bench/telemetry.py run --binary "$(abspath $(CODEC_BUILD))/cadapt" $(BENCH_ARGS)

plot:
	$(PYTHON) -B bench/telemetry.py plot
