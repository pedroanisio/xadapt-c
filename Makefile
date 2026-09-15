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
.PHONY: all test check check-parts clean bench plot sbom manifest reproduce gate ratio-floor corpus corpus-floor

all test clean:
	$(MAKE) -C adpat/c $@

# The gate is held to CHECK_SECONDS of wall time (LOCKS.md, 2026-09-15):
# it runs under `timeout`, so a suite that grows past the budget fails the
# gate rather than stretching it. The codec's check and the tools' tests
# run at once under make -j; each writes its own log, printed on failure.
CHECK_SECONDS ?= 120
check:
	rm -rf _tmp/check && mkdir -p _tmp/check
	timeout $(CHECK_SECONDS) $(MAKE) -j check-parts || { echo "make check exceeded $(CHECK_SECONDS) s or failed (see _tmp/check/*.log)"; exit 1; }
	@for p in codec provenance floor-tool fetch-corpus; do grep -hE "^ok |^OK$$|checks passed$$" _tmp/check/$$p.log | tail -3; done
check-parts: _tmp/check/codec _tmp/check/provenance _tmp/check/floor-tool _tmp/check/fetch-corpus
_tmp/check/codec: ; @$(MAKE) -C adpat/c check > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
_tmp/check/provenance: ; @$(PYTHON) -B tools/test_provenance.py > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
_tmp/check/floor-tool: ; @$(PYTHON) -B -m unittest discover -s tools -p 'test_check_ratio_floor.py' > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
_tmp/check/fetch-corpus: ; @$(PYTHON) -B -m unittest discover -s tools -p 'test_fetch_corpus.py' > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }

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

# The mixed acceptance corpus (DLV-3, LOCKS.md 2026-09-15): corpus fetches
# Silesia and Canterbury into static/corpus/mixed (git-ignored, pinned by
# the manifest); corpus-floor checks every file against its locked floor
# under the default command and under the -6 tier of --max, the two
# commands a lever must not worsen. About 10 minutes for both.
corpus:
	$(PYTHON) -B tools/fetch_corpus.py

corpus-floor: all
	$(PYTHON) -B tools/fetch_corpus.py --check > /dev/null
	$(PYTHON) -B tools/check_ratio_floor.py --binary "$(abspath $(CODEC_BUILD))/cadapt" --verify \
	    --floor tools/corpus_floor_default.json --corpus static/corpus/mixed $(FLOOR_ARGS)
	$(PYTHON) -B tools/check_ratio_floor.py --binary "$(abspath $(CODEC_BUILD))/cadapt" --verify \
	    --floor tools/corpus_floor_max6.json --corpus static/corpus/mixed $(FLOOR_ARGS)

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
