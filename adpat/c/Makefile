# The C codec: a static library, the cadapt command, and its tests.
#
# Authored by Pedro Anisio de Luna e Silva (pedroanisio@faz.ai), 2026-09-12.
#
#   make            build libadapt.a and cadapt
#   make test       build and run the C unit tests on the committed vectors
#   make check      build the library and CLI, then run C and Python tests
#   make clean
#
# Outputs go to build/ here (git-ignored, as the crate's target/ is), where
# the benchmark looks for cadapt; BUILD= chooses another directory. CC and
# CFLAGS are honoured; the defaults are the strictest the code is written
# for.

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

LIB   = $(BUILD)/libadapt.a
BIN   = $(BUILD)/cadapt
TESTS = $(BUILD)/test_adapt_decode $(BUILD)/test_adapt_encode

all: $(LIB) $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/adapt_decode.o: adapt_decode.c adapt.h adapt_model.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ adapt_decode.c

$(BUILD)/adapt_encode.o: adapt_encode.c adapt.h adapt_model.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ adapt_encode.c

$(BUILD)/cadapt.o: cadapt.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ cadapt.c

$(BUILD)/test_adapt_decode.o: tests/test_adapt_decode.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ tests/test_adapt_decode.c

$(BUILD)/test_adapt_encode.o: tests/test_adapt_encode.c adapt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ tests/test_adapt_encode.c

$(LIB): $(BUILD)/adapt_decode.o $(BUILD)/adapt_encode.o
	$(AR) rcs $@ $(BUILD)/adapt_decode.o $(BUILD)/adapt_encode.o

$(BIN): $(BUILD)/cadapt.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/cadapt.o $(LIB) $(LDFLAGS)

$(BUILD)/test_adapt_decode: $(BUILD)/test_adapt_decode.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/test_adapt_decode.o $(LIB) $(LDFLAGS)

$(BUILD)/test_adapt_encode: $(BUILD)/test_adapt_encode.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/test_adapt_encode.o $(LIB) $(LDFLAGS)

test: $(TESTS)
	$(BUILD)/test_adapt_decode tests/vectors
	$(BUILD)/test_adapt_encode tests/vectors

check: all test
	$(PYTHON) -B tests/test_check_gcov.py

clean:
	rm -rf $(BUILD)

.PHONY: all test check clean
