# Build and validate the standalone C codec from the workspace root.
# make: library and CLI; make test: C tests; make check: build and all tests.
.PHONY: all test check clean

all test check clean:
	$(MAKE) -C adpat/c $@
