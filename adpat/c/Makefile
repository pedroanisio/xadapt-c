# The C codec: a static library, the cadapt command, and its tests.
#
# Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
#
#   make            build libadapt.a and cadapt
#   make test       build and run the C unit tests on the committed vectors
#   make coverage   an instrumented build, every suite on it, and the gcov
#                   branch gate: 80 % for the library, 60 % for the CLI
#   make check      build the library and CLI, then run C and Python tests
#                   and the coverage gate
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
TESTS = $(BUILD)/test_adapt_adaptive $(BUILD)/test_adapt_decode $(BUILD)/test_adapt_encode $(BUILD)/test_adapt_archive $(BUILD)/test_decode_budget $(BUILD)/test_fuzz_decode

all: $(LIB) $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/adapt_decode.o: adapt_decode.c adapt.h adapt_model.h adapt_adaptive.h adapt_bmp.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ adapt_decode.c

$(BUILD)/adapt_encode.o: adapt_encode.c adapt.h adapt_model.h adapt_adaptive.h adapt_bmp.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ adapt_encode.c

$(BUILD)/adapt_archive.o: adapt_archive.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ adapt_archive.c

$(BUILD)/cadapt.o: cadapt.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ cadapt.c

$(BUILD)/test_adapt_decode.o: tests/test_adapt_decode.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ tests/test_adapt_decode.c

$(BUILD)/test_adapt_encode.o: tests/test_adapt_encode.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ tests/test_adapt_encode.c

$(BUILD)/test_adapt_archive.o: tests/test_adapt_archive.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ tests/test_adapt_archive.c

$(LIB): $(BUILD)/adapt_decode.o $(BUILD)/adapt_encode.o $(BUILD)/adapt_archive.o
	$(AR) rcs $@ $(BUILD)/adapt_decode.o $(BUILD)/adapt_encode.o $(BUILD)/adapt_archive.o

$(BIN): $(BUILD)/cadapt.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/cadapt.o $(LIB) $(LDFLAGS)

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

test: $(TESTS)
	$(BUILD)/test_adapt_adaptive
	$(BUILD)/test_adapt_decode tests/vectors
	$(BUILD)/test_adapt_encode tests/vectors
	$(BUILD)/test_adapt_archive
	$(BUILD)/test_decode_budget tests/vectors
	$(BUILD)/test_fuzz_decode tests/vectors

check: all test
	$(PYTHON) -B tests/test_check_gcov.py
	$(PYTHON) -B tests/test_cadapt.py "$(BIN)"
	ADAPT_TEST_THREADS=$(TEST_THREADS) $(PYTHON) -B tests/test_cadapt_safety.py "$(BIN)"
	$(PYTHON) -B tests/test_allocation_probe.py "$(BIN)"
	$(MAKE) coverage

# The instrumented build lives beside the ordinary one and starts afresh
# each time, so counts never accumulate across runs. One instantiation of
# the per-bit path (ADAPT_NO_MULTIVERSION): the instruction-set clones the
# machine does not run would count as branches never taken.
COV = $(BUILD)-cov
COVFLAGS = -std=c99 -O0 --coverage -DADAPT_NO_MULTIVERSION -Wall -Wextra -Wpedantic -Werror $(THREADS)
COVUNITS = adapt_decode adapt_encode adapt_archive cadapt

coverage:
	rm -rf $(COV)
	$(MAKE) BUILD=$(COV) CFLAGS="$(COVFLAGS)" LDFLAGS="--coverage" all $(COV)/test_adapt_decode \
	    $(COV)/test_adapt_encode $(COV)/test_adapt_archive $(COV)/test_adapt_adaptive $(COV)/test_decode_budget $(COV)/test_fuzz_decode
	$(COV)/test_adapt_decode tests/vectors
	$(COV)/test_adapt_encode tests/vectors
	$(COV)/test_adapt_archive
	$(COV)/test_adapt_adaptive
	$(COV)/test_decode_budget tests/vectors
	$(COV)/test_fuzz_decode tests/vectors
	$(PYTHON) -B tests/test_cadapt.py "$(abspath $(COV))/cadapt"
	ADAPT_TEST_THREADS=$(if $(findstring -DADAPT_THREADS,$(COVFLAGS)),1,0) $(PYTHON) -B tests/test_cadapt_safety.py "$(abspath $(COV))/cadapt"
	cd $(COV) && for u in $(COVUNITS); do gcov -b -c --json-format -o . $$u.o > /dev/null && gunzip -f $$u.gcov.json.gz; done
	$(PYTHON) -B check-gcov.py $(foreach u,$(COVUNITS),$(COV)/$(u).gcov.json)

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

.PHONY: all test coverage sanitize fuzz fuzz-smoke check clean
