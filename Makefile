# Build and validate the standalone C codec from the workspace root.
# make: library and CLI; make test: C tests; make check: build, all tests,
# the coverage gate and the provenance tool's tests; make sbom / manifest /
# reproduce: the delivery artefacts (DLV-2) — the SPDX document, the record
# of a build, and the rebuild that confirms a manifest byte for byte.
PYTHON ?= python3
BUILD ?= build
CODEC_BUILD = $(if $(filter /%,$(BUILD)),$(BUILD),adpat/c/$(BUILD))
BENCH_ARGS ?=
.PHONY: all test check clean bench plot sbom manifest reproduce

all test clean:
	$(MAKE) -C adpat/c $@

check:
	$(MAKE) -C adpat/c check
	$(PYTHON) -B tools/test_provenance.py

sbom:
	$(PYTHON) -B tools/provenance.py sbom

manifest: all
	$(PYTHON) -B tools/provenance.py manifest --build "$(CODEC_BUILD)"

reproduce:
	$(PYTHON) -B tools/provenance.py reproduce

# Optional corpus measurements; ordinary builds do not run benchmarks.
bench: all
	$(PYTHON) -B bench/telemetry.py run --binary "$(abspath $(CODEC_BUILD))/cadapt" $(BENCH_ARGS)

plot:
	$(PYTHON) -B bench/telemetry.py plot
