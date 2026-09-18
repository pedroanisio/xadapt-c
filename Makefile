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
.PHONY: all test check check-parts clean install uninstall sanitize sanitize-cli coverage check-suites check-long bench plot sbom manifest reproduce gate ratio-floor corpus corpus-floor memory-gate release test-install ltcb ltcb-release

# Forwarded to the codec's Makefile as written: SUPPORT.md and SECURITY.md
# say `make sanitize`, and the documents' tests hold every cited target to
# the Makefile it is invoked in.
all test clean install uninstall sanitize sanitize-cli coverage check-suites check-long:
	$(MAKE) -C adpat/c $@

# The gate is held to CHECK_SECONDS of wall time (LOCKS.md, 2026-09-15):
# it runs under `timeout`, so a suite that grows past the budget fails the
# gate rather than stretching it. The codec's check and the tools' tests
# run at once under make -j; each writes its own log, printed on failure.
# The suite is parallel, so its wall time is what a loaded host stretches
# (105 s at load 4 on the reference host, 115 s at load 10): the check
# prints its wall time and the load average, so a budget failure can be
# told from a code failure, and CHECK_SECONDS may be raised for the run.
CHECK_SECONDS ?= 120
# The codec is built once, before the parts run: the provenance part runs
# `make all` in the same build directory (tools/test_provenance.py), and
# two makes relinking cadapt at once left the safety part executing a
# half-written binary.
check:
	rm -rf _tmp/check && mkdir -p _tmp/check
	$(MAKE) -C adpat/c BUILD=$(BUILD) all
	@start=$$(date +%s); \
	if timeout $(CHECK_SECONDS) $(MAKE) -j check-parts; then \
	    echo "make check: $$(( $$(date +%s) - start )) s of the $(CHECK_SECONDS) s wall budget, load average$$(uptime | sed 's/.*load average//')"; \
	else \
	    echo "make check exceeded $(CHECK_SECONDS) s or failed after $$(( $$(date +%s) - start )) s at load average$$(uptime | sed 's/.*load average//') (see _tmp/check/*.log)"; exit 1; \
	fi
	@for p in codec provenance floor-tool fetch-corpus docs bench ltcb; do grep -hE "^ok |^OK$$|checks passed$$" _tmp/check/$$p.log | tail -3; done
check-parts: _tmp/check/codec _tmp/check/provenance _tmp/check/floor-tool _tmp/check/fetch-corpus _tmp/check/docs _tmp/check/bench _tmp/check/ltcb _tmp/check/gates
_tmp/check/codec: ; @$(MAKE) -C adpat/c check > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
_tmp/check/provenance: ; @$(PYTHON) -B tools/test_provenance.py > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
_tmp/check/floor-tool: ; @$(PYTHON) -B -m unittest discover -s tools -p 'test_check_*.py' > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
_tmp/check/fetch-corpus: ; @$(PYTHON) -B -m unittest discover -s tools -p 'test_fetch_corpus.py' > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
# The measurement tooling and the LTCB tooling. Both went ungated for weeks and
# both rotted: telemetry drove the encoder with a decode-only option and its
# container reader stopped at version 52, and the LTCB page builder read a
# rendering .gitignore excludes. A suite nothing runs is a suite that is wrong.
# The bench suite needs the built codec, which the codec part builds first.
_tmp/check/bench: _tmp/check/codec ; @$(PYTHON) -B -m unittest discover -s bench -p 'test_*.py' > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
_tmp/check/ltcb: ; @$(PYTHON) -B -m unittest discover -s tools/ltcb -p 'test_*.py' > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
# the documents against the code: tools/test_docs.py needs the built codec, which the codec part builds first
# the gates against what they enumerate (tools/test_gates.py needs the built codec for the profiles)
_tmp/check/gates: _tmp/check/codec ; @$(PYTHON) -B tools/test_gates.py > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
_tmp/check/docs: _tmp/check/codec ; @$(PYTHON) -B -m unittest discover -s tools -p 'test_docs.py' > $@.log 2>&1 && $(PYTHON) -B tools/render_floors.py check >> $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }

# Locked floors (LOCKS.md, 2026-09-14). gate: the CI proxy, the first 8 MiB of
# enwik8 cut from the tracked zip, encoded three times for the median of
# their CPU seconds (the floor's measure) and decoded once, well under
# 120 s. ratio-floor: the full enwik8 floor (static/corpus/enwik8 must be
# unzipped); add FLOOR_ARGS='--fixture enwik9=/path/to/enwik9' for the
# enwik9 floor, and --runs 3 for a re-lock.
gate: all
	unzip -p static/corpus/enwik8.zip enwik8 | head -c 8388608 > "$(CODEC_BUILD)/enwik8-8MiB"
	$(PYTHON) -B tools/check_ratio_floor.py --binary "$(abspath $(CODEC_BUILD))/cadapt" \
	    --fixture enwik8-8MiB="$(abspath $(CODEC_BUILD))/enwik8-8MiB" --verify --runs 3 --timeout 120 $(GATE_ARGS)

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

# The memory promise (RUN-1, RUN-2): the peak resident set of an encode
# and of a decode never exceeds what the container declares decoding
# needs (`cadapt decode --max-memory 1` names the figure) plus 16 MiB for
# the process itself. Measured on the gate's proxy under every profile
# `cadapt --help` names, the locked command and --max -6 (tools/profiles.py
# lists them from the binary); a resident set is bytes, not a timing, so
# this gate holds on any host.
memory-gate: all
	unzip -p static/corpus/enwik8.zip enwik8 | head -c 8388608 > "$(CODEC_BUILD)/enwik8-8MiB"
	$(PYTHON) -B tools/check_memory.py --binary "$(abspath $(CODEC_BUILD))/cadapt" \
	    --fixture enwik8-8MiB="$(abspath $(CODEC_BUILD))/enwik8-8MiB" $(MEMORY_ARGS)
MEMORY_ARGS ?=

# The release artifacts of the committed HEAD (or RELEASE_REV), built from
# an export of that revision alone — nothing reads the working tree — into
# dist/<version>/: the binary tarball in the layout `make install` writes,
# the source tarball, the SBOM, the build manifest, the NOTICE, SHA256SUMS
# and a release manifest (tools/release.sh). test-install proves the flow
# from a clean environment: an export built with cc and make alone,
# installed under a prefix, used, and uninstalled; INSTALL_IMAGE=debian:bookworm-slim
# runs the same inside a fresh container (docs/release-pipeline.md).
RELEASE_REV ?= HEAD
release:
	tools/release.sh --rev $(RELEASE_REV)
INSTALL_IMAGE ?=
test-install:
	tools/test_install.sh $(if $(INSTALL_IMAGE),--docker $(INSTALL_IMAGE),) --rev $(RELEASE_REV)

sbom:
	$(PYTHON) -B tools/provenance.py sbom

manifest: all
	$(PYTHON) -B tools/provenance.py manifest --build "$(CODEC_BUILD)"

# against a release's manifest (dist/<version>/build-manifest.json) or the
# one `make manifest` wrote; the manifest is not tracked (it records the
# commit and the host it was made on, so a committed one is stale by the
# next commit)
MANIFEST ?= build-manifest.json
reproduce:
	$(PYTHON) -B tools/provenance.py reproduce --manifest "$(MANIFEST)"

# The assets of a Large Text Compression Benchmark entry from the committed
# HEAD (docs/ltcb-submission.md): the decompressor zip (the binary alone —
# the sources are not published), the verified containers of the fixtures
# given, hashes and a manifest, under dist/.
#   make ltcb-release RELEASE_ARGS='--enwik8 static/corpus/enwik8 --enwik9 /path/to/enwik9'
RELEASE_ARGS ?=
ltcb-release:
	tools/ltcb_release.sh $(RELEASE_ARGS)

# The LTCB renderings research/ltcb-tables.md links (ignored by git): the
# dashboard, the phase chart, the analysis and the interactive page, from
# the tracked CSVs. Plotting needs the packages in bench/requirements.txt.
ltcb:
	$(PYTHON) -B tools/ltcb/plot_ltcb_tables.py --local research/ltcb-local-entries.csv --output research/ltcb-dashboard.svg
	$(PYTHON) -B tools/ltcb/plot_ltcb_phases.py --local research/ltcb-local-entries.csv --output research/ltcb-phases.svg
	$(PYTHON) -B tools/ltcb/analyze_ltcb.py --local research/ltcb-local-entries.csv --output research/ltcb-analysis.json
	$(PYTHON) -B tools/ltcb/build_ltcb_interactive.py

# Benchmarks are owned by the standalone codecbench tool, using an external
# cadapt command adapter. These compatibility targets report the migration.
bench plot:
	@echo "Use codecbench run/history with an external cadapt adapter; see README.md."
	@false
