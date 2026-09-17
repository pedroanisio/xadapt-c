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
TESTS = $(BUILD)/test_adapt_adaptive $(BUILD)/test_adapt_decode $(BUILD)/test_adapt_encode $(BUILD)/test_adapt_archive $(BUILD)/test_decode_budget $(BUILD)/test_fuzz_decode $(BUILD)/test_adapt_text $(BUILD)/test_model_dispatch $(BUILD)/test_profile_conversion $(BUILD)/test_p1_compatibility

all: $(LIB) $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/adapt_decode.o: adapt_decode.c adapt.h adapt_model.h adapt_adaptive.h adapt_bmp.h adapt_text.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ adapt_decode.c

$(BUILD)/adapt_encode.o: adapt_encode.c adapt.h adapt_model.h adapt_adaptive.h adapt_bmp.h adapt_text.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ adapt_encode.c

$(BUILD)/adapt_archive.o: adapt_archive.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ adapt_archive.c

$(BUILD)/cadapt.o: cadapt.c adapt.h cadapt_platform.h cadapt_fs.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ cadapt.c

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

$(BIN): $(BUILD)/cadapt.o $(BUILD)/cadapt_fs.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/cadapt.o $(BUILD)/cadapt_fs.o $(LIB) $(LDFLAGS)

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

$(BUILD)/test_model_dispatch: tests/test_model_dispatch.c adapt.h adapt_model.h adapt_adaptive.h adapt_bmp.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_model_dispatch.c $(LDFLAGS)

$(BUILD)/test_profile_conversion: tests/test_profile_conversion.c adapt.h $(LIB)
	$(CC) $(CFLAGS) -o $@ tests/test_profile_conversion.c $(LIB) $(LDFLAGS)

$(BUILD)/test_p1_compatibility: tests/test_p1_compatibility.c adapt.h $(LIB)
	$(CC) $(CFLAGS) -o $@ tests/test_p1_compatibility.c $(LIB) $(LDFLAGS)

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
TESTRUNS = adaptive $(foreach k,$(DECODE_SHARDS),decode-$(k)) $(foreach k,$(ENCODE_SHARDS),encode-$(k)) archive budget fuzz text dispatch profile compatibility
$(BUILD)/runs/adaptive: ; @$(BUILD)/test_adapt_adaptive > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/decode-%: ; @$(BUILD)/test_adapt_decode tests/vectors $*/$(words $(DECODE_SHARDS)) > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/encode-%: ; @$(BUILD)/test_adapt_encode tests/vectors $*/$(words $(ENCODE_SHARDS)) > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/archive: ; @$(BUILD)/test_adapt_archive > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/budget: ; @$(BUILD)/test_decode_budget tests/vectors > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/fuzz: ; @$(BUILD)/test_fuzz_decode tests/vectors > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/text: ; @$(BUILD)/test_adapt_text tests/vectors > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/dispatch: ; @$(BUILD)/test_model_dispatch > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/compatibility: ; @$(BUILD)/test_p1_compatibility tests/vectors > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/profile: ; @$(BUILD)/test_profile_conversion > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }

# The plain suites and the coverage pass run at once: they build into
# different directories and share nothing.
check: all
	$(MAKE) -j check-parts
check-parts: test coverage $(BUILD)/runs/gcov-tests $(BUILD)/runs/safety $(BUILD)/runs/probe
# This explicit rule is phony: an old success marker must never skip the gate's tests.
$(BUILD)/runs/gcov-tests: | $(BUILD)/runs ; @$(PYTHON) -B tests/test_check_gcov.py > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/safety: all | $(BUILD)/runs ; @ADAPT_TEST_THREADS=$(TEST_THREADS) $(PYTHON) -B tests/test_cadapt_safety.py "$(BIN)" > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(BUILD)/runs/probe: all | $(BUILD)/runs ; @$(PYTHON) -B tests/test_allocation_probe.py "$(BIN)" > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
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
COVUNITS = adapt_decode adapt_encode adapt_archive cadapt cadapt_fs

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
CLI_CLASSES = AllocationTest AdaptiveTest VerificationTest MetadataTest MemoryContractTest InstructionSetTest FolderTest \
              PresetTest SurfaceTest CommandTest

coverage:
	rm -rf $(COV)
	$(MAKE) BUILD=$(COV) CFLAGS="$(COVFLAGS)" LDFLAGS="--coverage" all $(COV)/test_adapt_decode \
	    $(COV)/test_adapt_encode $(COV)/test_adapt_archive $(COV)/test_adapt_adaptive $(COV)/test_decode_budget $(COV)/test_fuzz_decode \
	    $(COV)/test_adapt_text $(COV)/test_profile_conversion $(COV)/test_p1_compatibility
	mkdir -p $(COV)/gcda
	$(MAKE) -j $(COV)/gcda/done
	cd $(COV) && for u in $(COVUNITS); do gcov -b -c --json-format -o . $$u.o > /dev/null && gunzip -f $$u.gcov.json.gz; done
	$(PYTHON) -B check-gcov.py $(foreach u,$(COVUNITS),$(COV)/$(u).gcov.json)

# One run per line, in parallel under make -j; each leaves its counts in
# its own directory and its report in its own log, printed when it fails.
COVRUNS = $(foreach k,$(ENCODE_SHARDS),encode-$(k)) $(foreach k,$(DECODE_SHARDS),decode-$(k)) archive adaptive budget fuzz text profile compatibility \
          safety $(foreach c,$(CLI_CLASSES),cli-$(c))
$(COV)/gcda/encode-%: ; @$(COVRUN)/$(@F) $(COV)/test_adapt_encode tests/vectors $*/$(words $(ENCODE_SHARDS)) > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/decode-%: ; @$(COVRUN)/$(@F) $(COV)/test_adapt_decode tests/vectors $*/$(words $(DECODE_SHARDS)) > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/archive: ; @$(COVRUN)/$(@F) $(COV)/test_adapt_archive > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/adaptive: ; @$(COVRUN)/$(@F) $(COV)/test_adapt_adaptive > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/budget: ; @$(COVRUN)/$(@F) $(COV)/test_decode_budget tests/vectors > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/fuzz: ; @$(COVRUN)/$(@F) $(COV)/test_fuzz_decode tests/vectors > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/text: ; @$(COVRUN)/$(@F) $(COV)/test_adapt_text tests/vectors > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/compatibility: ; @$(COVRUN)/$(@F) $(COV)/test_p1_compatibility tests/vectors > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
$(COV)/gcda/profile: ; @$(COVRUN)/$(@F) $(COV)/test_profile_conversion > $@.log 2>&1 && touch $@ || { cat $@.log; exit 1; }
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

# Mutations go to a build-local corpus, never to the committed vectors.
FUZZ_CC ?= clang
FUZZ_RUNS ?= 1000
FUZZ_CFLAGS ?= -std=c99 -O1 -g -DADAPT_NO_MULTIVERSION -fsanitize=fuzzer,address,undefined \
               -fno-sanitize-recover=all -Wall -Wextra -Wpedantic -Werror

$(BUILD)/fuzz_decode: fuzz/fuzz_decode.c fuzz/fuzz_decode.h adapt_decode.c adapt_encode.c adapt_archive.c adapt.h adapt_model.h adapt_adaptive.h adapt_bmp.h | $(BUILD)
	$(FUZZ_CC) $(FUZZ_CFLAGS) adapt_decode.c adapt_encode.c adapt_archive.c fuzz/fuzz_decode.c -o $@

fuzz: $(BUILD)/fuzz_decode

fuzz-smoke: fuzz
	mkdir -p $(BUILD)/fuzz-corpus
	cp tests/vectors/*.adpt $(BUILD)/fuzz-corpus/
	$(BUILD)/fuzz_decode $(BUILD)/fuzz-corpus -runs=$(FUZZ_RUNS) -max_len=65536 -seed=1 -print_final_stats=1

clean:
	rm -rf $(BUILD) $(COV) $(SANITIZE_BUILD)

.PHONY: all test coverage sanitize fuzz fuzz-smoke check check-parts check-long clean
.PHONY: $(BUILD)/runs/gcov-tests
