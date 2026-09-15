# Build and validate the standalone C codec from the workspace root.
# make: library and CLI; make test: C tests; make check: build, all tests,
# the coverage gate and the provenance tool's tests; make sbom / manifest /
# reproduce: the delivery artefacts (DLV-2) — the SPDX document, the record
# of a build, and the rebuild that confirms a manifest byte for byte.
PYTHON ?= python3
BUILD ?= build
CODEC_BUILD = $(if $(filter /%,$(BUILD)),$(BUILD),adpat/c/$(BUILD))
GATE_ARGS ?=
FLOOR_ARGS ?=
.PHONY: all test check clean bench plot sbom manifest reproduce gate ratio-floor

all test clean:
	$(MAKE) -C adpat/c $@

check:
	$(MAKE) -C adpat/c check
	$(PYTHON) -B tools/test_provenance.py
	$(PYTHON) -B -m unittest discover -s tools -p 'test_check_ratio_floor.py'

# Locked floors (LOCKS.md, 2026-09-14). gate: the CI proxy, the first 8 MiB of
# enwik8 cut from the tracked zip, encode + decode, well under 120 s.
# ratio-floor: the full enwik8 floor (static/corpus/enwik8 must be unzipped);
# add FLOOR_ARGS='--fixture enwik9=/path/to/enwik9' for the enwik9 floor.
gate: all
	unzip -p static/corpus/enwik8.zip enwik8 | head -c 8388608 > "$(CODEC_BUILD)/enwik8-8MiB"
	$(PYTHON) -B tools/check_ratio_floor.py --binary "$(abspath $(CODEC_BUILD))/cadapt" \
	    --fixture enwik8-8MiB="$(abspath $(CODEC_BUILD))/enwik8-8MiB" --verify --timeout 120 $(GATE_ARGS)

ratio-floor: all
	$(PYTHON) -B tools/check_ratio_floor.py --binary "$(abspath $(CODEC_BUILD))/cadapt" --verify $(FLOOR_ARGS)

sbom:
	$(PYTHON) -B tools/provenance.py sbom

manifest: all
	$(PYTHON) -B tools/provenance.py manifest --build "$(CODEC_BUILD)"

reproduce:
	$(PYTHON) -B tools/provenance.py reproduce

# Benchmarks are owned by the standalone codecbench tool, using an external
# cadapt command adapter. These compatibility targets report the migration.
bench plot:
	@echo "Use codecbench run/history with an external cadapt adapter; see README.md."
	@false
