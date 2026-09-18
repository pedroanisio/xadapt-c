# The C codec: a static library, the cadapt command, and its tests.
#
# Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
#
#   make            build libadapt.a and cadapt
#   make test       build and run the C unit tests on the committed vectors
#   make coverage   an instrumented build, every suite on it, and the gcov
#                   branch gate: 80 % for the library, 60 % for the CLI
#   make check      build the library and CLI, then run C and Python tests
#                   and the coverage gate — the fast suite, bounded for CI
#   make check-long the CLI suite's long cases too (megabytes under --max:
#                   ADAPT_LONG_TESTS=1), for a landing, not every push
#   make sanitize   the C suites under AddressSanitizer and
#                   UndefinedBehaviorSanitizer, every fault fatal
#   make install    the command, manual, library and header under PREFIX
#                   (default /usr/local; DESTDIR for a staging root)
#   make clean
#
# Outputs go to build/ here (git-ignored, as the crate's target/ is), where
# the benchmark looks for cadapt; BUILD= chooses another directory. CC and
# CFLAGS are honoured; the defaults are the strictest the code is written
# for. Two switches worth adding to CFLAGS on another platform:
# -DADAPT_NO_MULTIVERSION builds the per-bit path for the baseline
# instruction set only (the default on x86-64 compiles it twice and binds
# the variant through an ifunc, which wants a glibc-style loader);
# -DADAPT_NO_HUGEPAGES allocates the model's tables with calloc instead of
# an anonymous mapping. The coverage gate needs GCC and its gcov; the
# sanitizer run needs clang (SANITIZER_CC= names another compiler).

CC      ?= cc
AR      ?= ar
PYTHON  ?= python3
# The encoder's proof runs on a POSIX thread, a block or two behind the
# coder, when ADAPT_THREADS is defined; THREADS= builds the in-line proof
# instead, with nothing beyond the C standard library.
THREADS ?= -DADAPT_THREADS -pthread
CFLAGS  ?= -std=c99 -O3 -Wall -Wextra -Wpedantic -Werror $(THREADS)
LDFLAGS ?=
BUILD   ?= build
TEST_THREADS = $(if $(findstring -DADAPT_THREADS,$(CFLAGS)),1,0)

LIB   = $(BUILD)/libadapt.a
BIN   = $(BUILD)/cadapt
# Every suite under tests/ is built and run: the lists come from the files,
# never from a typed list (a suite left off one ran nowhere, and nothing
# said so). A suite with dependencies of its own has its rule below; any
# other builds by the pattern rule against the header and the library, and
# runs with the arguments ARGS_<suite> names, none by default. The two
# large suites run as shards (see their mains) and are the exceptions.
SUITES = $(patsubst tests/%.c,%,$(wildcard tests/test_*.c))
TESTS = $(addprefix $(BUILD)/,$(SUITES))
SHARDED = test_adapt_decode test_adapt_encode

all: $(LIB) $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

# The model header and the sections it includes, in order: a change to any
# of them rebuilds every unit that compiles the model.
MODEL_H = adapt_model.h adapt_constants.h adapt_states.h adapt_coder.h adapt_tables.h adapt_model_state.h adapt_model_byte.h adapt_model_bit.h adapt_model_new.h adapt_blocks.h adapt_crc32.h

$(BUILD)/adapt_decode.o: adapt_decode.c adapt.h $(MODEL_H) adapt_adaptive.h adapt_bmp.h adapt_text.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ adapt_decode.c

$(BUILD)/adapt_encode.o: adapt_encode.c adapt.h $(MODEL_H) adapt_adaptive.h adapt_bmp.h adapt_text.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ adapt_encode.c

$(BUILD)/adapt_archive.o: adapt_archive.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ adapt_archive.c

$(BUILD)/cadapt.o: cadapt.c adapt.h cadapt_platform.h cadapt_fs.h cadapt_folder.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ cadapt.c

$(BUILD)/cadapt_folder.o: cadapt_folder.c cadapt_folder.h cadapt_fs.h cadapt_platform.h adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ cadapt_folder.c

$(BUILD)/cadapt_fs.o: cadapt_fs.c cadapt_fs.h cadapt_platform.h adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ cadapt_fs.c

$(BUILD)/test_adapt_decode.o: tests/test_adapt_decode.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ tests/test_adapt_decode.c

$(BUILD)/test_adapt_encode.o: tests/test_adapt_encode.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ tests/test_adapt_encode.c

$(BUILD)/test_adapt_archive.o: tests/test_adapt_archive.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ tests/test_adapt_archive.c

$(LIB): $(BUILD)/adapt_decode.o $(BUILD)/adapt_encode.o $(BUILD)/adapt_archive.o
	$(AR) rcs $@ $(BUILD)/adapt_decode.o $(BUILD)/adapt_encode.o $(BUILD)/adapt_archive.o

$(BIN): $(BUILD)/cadapt.o $(BUILD)/cadapt_folder.o $(BUILD)/cadapt_fs.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/cadapt.o $(BUILD)/cadapt_folder.o $(BUILD)/cadapt_fs.o $(LIB) $(LDFLAGS)

$(BUILD)/test_adapt_decode: $(BUILD)/test_adapt_decode.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/test_adapt_decode.o $(LIB) $(LDFLAGS)

$(BUILD)/test_adapt_encode: $(BUILD)/test_adapt_encode.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/test_adapt_encode.o $(LIB) $(LDFLAGS)

$(BUILD)/test_adapt_archive: $(BUILD)/test_adapt_archive.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/test_adapt_archive.o $(LIB) $(LDFLAGS)

$(BUILD)/test_adapt_adaptive: tests/test_adapt_adaptive.c adapt.h adapt_adaptive.h adapt_bmp.h $(LIB)
	$(CC) $(CFLAGS) -o $@ tests/test_adapt_adaptive.c $(LIB) $(LDFLAGS)

$(BUILD)/test_decode_budget: tests/test_decode_budget.c adapt.h $(LIB)
	$(CC) $(CFLAGS) -o $@ tests/test_decode_budget.c $(LIB) $(LDFLAGS)

$(BUILD)/test_fuzz_decode: tests/test_fuzz_decode.c fuzz/fuzz_decode.c fuzz/fuzz_decode.h adapt.h $(LIB)
	$(CC) $(CFLAGS) -o $@ tests/test_fuzz_decode.c fuzz/fuzz_decode.c $(LIB) $(LDFLAGS)
$(BUILD)/test_adapt_text: tests/test_adapt_text.c adapt.h adapt_text.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_adapt_text.c $(LDFLAGS)

$(BUILD)/test_model_dispatch: tests/test_model_dispatch.c adapt.h $(MODEL_H) adapt_adaptive.h adapt_bmp.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_model_dispatch.c $(LDFLAGS)

$(BUILD)/test_profile_conversion: tests/test_profile_conversion.c adapt.h $(LIB)
	$(CC) $(CFLAGS) -o $@ tests/test_profile_conversion.c $(LIB) $(LDFLAGS)

$(BUILD)/test_p1_compatibility: tests/test_p1_compatibility.c adapt.h $(LIB)
	$(CC) $(CFLAGS) -o $@ tests/test_p1_compatibility.c $(LIB) $(LDFLAGS)

$(BUILD)/test_cadapt_fs: tests/test_cadapt_fs.c cadapt_fs.h cadapt_platform.h $(BUILD)/cadapt_fs.o $(LIB)
	$(CC) $(CFLAGS) -o $@ tests/test_cadapt_fs.c $(BUILD)/cadapt_fs.o $(LIB) $(LDFLAGS)

# a suite with no rule of its own: the source against the header and the library
$(BUILD)/test_%: tests/test_%.c adapt.h $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) $(LDFLAGS)

# The two large C suites run as shards, k/n (see their mains).
ENCODE_SHARDS = 0 1 2 3
DECODE_SHARDS = 0 1 2

# The suites at once, under make -j: each run's report goes to its own
# log, printed when it fails, and the summary lines afterwards. A run is
# a file touched on success, never phony: a pattern rule does not apply
# to a phony target.
test: $(TESTS)
	rm -f $(foreach r,$(TESTRUNS),$(BUILD)/runs/$(r) $(BUILD)/runs/$(r).log) && mkdir -p $(BUILD)/runs
	$(MAKE) -j $(foreach r,$(TESTRUNS),$(BUILD)/runs/$(r))
	@for r in $(TESTRUNS); do tail -1 $(BUILD)/runs/$$r.log; done
TESTRUNS = $(filter-out $(SHARDED),$(SUITES)) $(foreach k,$(DECODE_SHARDS),decode-$(k)) $(foreach k,$(ENCODE_SHARDS),encode-$(k))
# the arguments a suite takes: the vectors, for the four that read them
ARGS_test_decode_budget = tests/vectors
ARGS_test_fuzz_decode = tests/vectors
ARGS_test_adapt_text = tests/vectors
ARGS_test_p1_compatibility = tests/vectors
$(BUILD)/runs/test_%: ; @$(BUILD)/test_$* $(ARGS_test_$*) > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/decode-%: ; @$(BUILD)/test_adapt_decode tests/vectors $*/$(words $(DECODE_SHARDS)) > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/encode-%: ; @$(BUILD)/test_adapt_encode tests/vectors $*/$(words $(ENCODE_SHARDS)) > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }

# The plain suites and the coverage pass run at once: they build into
# different directories and share nothing.
check: all
	$(MAKE) -j check-parts
check-parts: test coverage $(BUILD)/runs/gcov-tests $(BUILD)/runs/safety $(BUILD)/runs/probe $(BUILD)/runs/platform \
             $(BUILD)/runs/examples
# Every part of `make check` but the coverage pass, plus the CLI suite on
# the plain build (under `make check` it runs inside the coverage pass):
# what a compiler without gcov, or another platform, runs. The workflows
# call this and nothing inline, so a part added above is gated everywhere
# (tools/test_gates.py holds the two lists to each other).
check-suites: all
	$(MAKE) -j test $(BUILD)/runs/safety $(BUILD)/runs/probe $(BUILD)/runs/platform $(BUILD)/runs/examples $(BUILD)/runs/cli
$(BUILD)/runs/cli: all | $(BUILD)/runs ; @ADAPT_TEST_THREADS=$(TEST_THREADS) $(PYTHON) -B tests/test_cadapt.py "$(BIN)" > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
# This explicit rule is phony: an old success marker must never skip the gate's tests.
$(BUILD)/runs/gcov-tests: | $(BUILD)/runs ; @$(PYTHON) -B tests/test_check_gcov.py > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/safety: all | $(BUILD)/runs ; @ADAPT_TEST_THREADS=$(TEST_THREADS) $(PYTHON) -B tests/test_cadapt_safety.py "$(BIN)" > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/probe: all | $(BUILD)/runs ; @$(PYTHON) -B tests/test_allocation_probe.py "$(BIN)" > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
# The examples in examples/ are the interface's executable form: each is a
# program a user copies, built against the header and the library alone,
# and each exits 1 where the interface departs from what adapt.h says.
EXAMPLES = roundtrip stream folder contract
EXAMPLE_BINS = $(foreach e,$(EXAMPLES),$(BUILD)/example-$(e))
$(BUILD)/example-%: examples/%.c examples/example.h adapt.h $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ examples/$*.c $(LIB) $(LDFLAGS)
examples: $(EXAMPLE_BINS)
$(BUILD)/runs/examples: $(EXAMPLE_BINS) | $(BUILD)/runs ; @for e in $(EXAMPLE_BINS); do $$e > $@.log 2>&1 || { cat $@.log; exit 1; }; done && touch $@
# The feature macros against the system headers: a compile, not a run, so it
# does not need the binary.
$(BUILD)/runs/platform: | $(BUILD)/runs ; @$(PYTHON) -B tests/test_platform_header.py > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs: ; mkdir -p $@

# The CLI suite's long cases on the ordinary build, after the fast check.
check-long: check
	ADAPT_LONG_TESTS=1 $(PYTHON) -B tests/test_cadapt.py "$(BIN)"

# The instrumented build lives beside the ordinary one and starts afresh
# each time, so counts never accumulate across runs. One instantiation of
# the per-bit path (ADAPT_NO_MULTIVERSION): the instruction-set clones the
# machine does not run would count as branches never taken.
COV = $(BUILD)-cov
# -O1: the instrumented per-bit path runs 1.6 times faster than at -O0 and
# writes the same bytes; the branches gcov counts are the source's either way.
COVFLAGS = -std=c99 -O1 --coverage -DADAPT_NO_MULTIVERSION -Wall -Wextra -Wpedantic -Werror $(THREADS)
# every unit of the codec and the command line: the files, never a list
# (check-gcov.py groups them by name: adapt_* the library, cadapt* the CLI)
COVUNITS = $(basename $(wildcard *.c))

# The suites run at once, each process writing its counts under its own
# GCOV_PREFIX (GCOV_PREFIX_STRIP drops the object directory's path, so the
# counts land at gcda/<run>/<unit>.gcda); gcov-tool merges the trees and
# gcov reads the sum. The two large C suites run as shards (k/n, see their
# mains) and the CLI suite as its classes; no test shares state with
# another. The instrumented path is 5 to 9 times slower per byte, so this
# is what keeps `make check` within a few minutes.
COVDIR = $(abspath $(COV))
COVSTRIP = $(words $(subst /, ,$(COVDIR)))
COVRUN = GCOV_PREFIX_STRIP=$(COVSTRIP) GCOV_PREFIX=$(COVDIR)/gcda
# the CLI suite's classes, from the suite itself (those with a test in
# them); every C suite, from SUITES, as `make test` runs them
CLI_CLASSES = $(shell $(PYTHON) -B tests/test_cadapt.py --classes)

coverage:
	rm -rf $(COV)
	$(MAKE) BUILD=$(COV) CFLAGS="$(COVFLAGS)" LDFLAGS="--coverage" all $(addprefix $(COV)/,$(SUITES))
	mkdir -p $(COV)/gcda
	$(MAKE) -j $(COV)/gcda/done
	cd $(COV) && for u in $(COVUNITS); do gcov -b -c --json-format -o . $$u.o > /dev/null && gunzip -f $$u.gcov.json.gz; done
	$(PYTHON) -B check-gcov.py $(foreach u,$(COVUNITS),$(COV)/$(u).gcov.json)

# One run per line, in parallel under make -j; each leaves its counts in
# its own directory and its report in its own log, printed when it fails.
COVRUNS = $(TESTRUNS) safety $(foreach c,$(CLI_CLASSES),cli-$(c))
$(COV)/gcda/encode-%: ; @$(COVRUN)/$(@F) $(COV)/test_adapt_encode tests/vectors $*/$(words $(ENCODE_SHARDS)) > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/decode-%: ; @$(COVRUN)/$(@F) $(COV)/test_adapt_decode tests/vectors $*/$(words $(DECODE_SHARDS)) > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/test_%: ; @$(COVRUN)/$(@F) $(COV)/test_$* $(ARGS_test_$*) > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/safety: ; @ADAPT_TEST_THREADS=$(if $(findstring -DADAPT_THREADS,$(COVFLAGS)),1,0) $(COVRUN)/$(@F) \
	    $(PYTHON) -B tests/test_cadapt_safety.py "$(COVDIR)/cadapt" > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/cli-%: ; @$(COVRUN)/$(@F) $(PYTHON) -B tests/test_cadapt.py "$(COVDIR)/cadapt" $* > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/done: $(foreach r,$(COVRUNS),$(COV)/gcda/$(r))
	@set -e; cd $(COV)/gcda; merged=$(word 1,$(COVRUNS)); for r in $(wordlist 2,$(words $(COVRUNS)),$(COVRUNS)); do \
	    gcov-tool merge -o merged.next $$merged $$r > /dev/null; rm -rf merged; mv merged.next merged; merged=merged; done; \
	    cp merged/*.gcda $(COVDIR)/ && touch done

# The sanitized build, likewise beside the ordinary one: the C suites under
# AddressSanitizer (leaks included) and UndefinedBehaviorSanitizer, with
# every report fatal, so a fault fails the run rather than printing.
SANITIZE_BUILD  ?= $(BUILD)-sanitize
SANITIZER_CC    ?= clang
SANITIZE_CFLAGS ?= -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
                   -Wall -Wextra -Wpedantic -Werror $(THREADS)

sanitize:
	rm -rf $(SANITIZE_BUILD)
	$(MAKE) BUILD=$(SANITIZE_BUILD) CC=$(SANITIZER_CC) CFLAGS="$(SANITIZE_CFLAGS)" test

# The CLI suite against a sanitized binary, leak detection on: the flags
# are SANITIZE_CFLAGS, never restated by a caller.
sanitize-cli:
	$(MAKE) BUILD=$(SANITIZE_BUILD) CC=$(SANITIZER_CC) CFLAGS="$(SANITIZE_CFLAGS)" all
	ASAN_OPTIONS=detect_leaks=1 ADAPT_TEST_THREADS=$(TEST_THREADS) $(PYTHON) -B tests/test_cadapt.py "$(SANITIZE_BUILD)/cadapt"

# Mutations go to a build-local corpus, never to the committed vectors.
FUZZ_CC ?= clang
FUZZ_RUNS ?= 1000
FUZZ_ARTIFACT_DIR ?= /tmp/cadapt-fuzz
FUZZ_CFLAGS ?= -std=c99 -O1 -g -DADAPT_NO_MULTIVERSION -fsanitize=fuzzer,address,undefined \
               -fno-sanitize-recover=all -Wall -Wextra -Wpedantic -Werror

$(BUILD)/fuzz_decode: fuzz/fuzz_decode.c fuzz/fuzz_decode.h adapt_decode.c adapt_encode.c adapt_archive.c adapt.h $(MODEL_H) adapt_adaptive.h adapt_bmp.h | $(BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) adapt_decode.c adapt_encode.c adapt_archive.c fuzz/fuzz_decode.c -o $@

# fuzz_header: the same oracle on bytes whose header checksum is made good
# first, so the header's fields are reached; fuzz_archive: the archive
# unpacker with a sink that holds it to its contract, seeded by
# archive_seeds. FUZZ_MODEL_MEMORY caps the model one execution admits:
# the default 128 MiB reaches every sizing a header can name below it;
# 64 MiB still admits every vector's model (31 MiB normal, 60 MiB max)
# while refusing the large tables that cost a run most of its time.
FUZZ_MODEL_MEMORY ?= ((uint64_t)128 << 20)

$(BUILD)/fuzz_header: fuzz/fuzz_header.c fuzz/fuzz_decode.c fuzz/fuzz_decode.h adapt_decode.c adapt_encode.c adapt_archive.c adapt.h $(MODEL_H) adapt_adaptive.h adapt_bmp.h | $(BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) "-DADAPT_FUZZ_MAX_MODEL_MEMORY=$(FUZZ_MODEL_MEMORY)" -DADAPT_FUZZ_NO_ENTRY adapt_decode.c adapt_encode.c adapt_archive.c fuzz/fuzz_decode.c fuzz/fuzz_header.c -o $@

$(BUILD)/fuzz_archive: fuzz/fuzz_archive.c adapt_archive.c adapt.h | $(BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) adapt_archive.c fuzz/fuzz_archive.c -o $@

$(BUILD)/archive_seeds: fuzz/archive_seeds.c adapt.h $(LIB)
	$(CC) $(CFLAGS) -o $@ fuzz/archive_seeds.c $(LIB) $(LDFLAGS)

fuzz: $(BUILD)/fuzz_decode $(BUILD)/fuzz_header $(BUILD)/fuzz_archive

fuzz-smoke: fuzz
	mkdir -p "$(FUZZ_ARTIFACT_DIR)"
	mkdir -p $(BUILD)/fuzz-corpus
	cp tests/vectors/*.adpt $(BUILD)/fuzz-corpus/
	$(BUILD)/fuzz_decode $(BUILD)/fuzz-corpus -runs=$(FUZZ_RUNS) -max_len=65536 -seed=1 -print_final_stats=1 -artifact_prefix="$(FUZZ_ARTIFACT_DIR)/"

fuzz-header-smoke: $(BUILD)/fuzz_header
	mkdir -p "$(FUZZ_ARTIFACT_DIR)"
	mkdir -p $(BUILD)/fuzz-header-corpus
	cp tests/vectors/*.adpt $(BUILD)/fuzz-header-corpus/
	$(BUILD)/fuzz_header $(BUILD)/fuzz-header-corpus -runs=$(FUZZ_RUNS) -max_len=65536 -seed=1 -print_final_stats=1 -artifact_prefix="$(FUZZ_ARTIFACT_DIR)/"

fuzz-archive-smoke: $(BUILD)/fuzz_archive $(BUILD)/archive_seeds
	mkdir -p "$(FUZZ_ARTIFACT_DIR)"
	mkdir -p $(BUILD)/fuzz-archive-corpus
	$(BUILD)/archive_seeds $(BUILD)/fuzz-archive-corpus
	$(BUILD)/fuzz_archive $(BUILD)/fuzz-archive-corpus -runs=$(FUZZ_RUNS) -max_len=4096 -seed=1 -print_final_stats=1 -artifact_prefix="$(FUZZ_ARTIFACT_DIR)/"

# Installation: the command, its manual, the library and its header under
# PREFIX (DESTDIR ahead of it for a package's staging root). Nothing else
# is installed: the tests, the benchmarks and the fuzz harnesses stay in
# the tree. `make uninstall` removes exactly what `make install` placed.
PREFIX  ?= /usr/local
DESTDIR ?=
INSTALL ?= install
BINDIR  = $(DESTDIR)$(PREFIX)/bin
LIBDIR  = $(DESTDIR)$(PREFIX)/lib
INCDIR  = $(DESTDIR)$(PREFIX)/include
MANDIR  = $(DESTDIR)$(PREFIX)/share/man/man1
install: all
	$(INSTALL) -d $(BINDIR) $(LIBDIR) $(INCDIR) $(MANDIR)
	$(INSTALL) -m 755 $(BIN) $(BINDIR)/cadapt
	$(INSTALL) -m 644 $(LIB) $(LIBDIR)/libadapt.a
	$(INSTALL) -m 644 adapt.h $(INCDIR)/adapt.h
	$(INSTALL) -m 644 cadapt.1 $(MANDIR)/cadapt.1
uninstall:
	rm -f $(BINDIR)/cadapt $(LIBDIR)/libadapt.a $(INCDIR)/adapt.h $(MANDIR)/cadapt.1

clean:
	rm -rf $(BUILD) $(COV) $(SANITIZE_BUILD)

.PHONY: all test coverage sanitize sanitize-cli fuzz fuzz-smoke fuzz-header-smoke fuzz-archive-smoke examples install uninstall check check-parts check-suites check-long clean
.PHONY: $(BUILD)/runs/gcov-tests
