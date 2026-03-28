# Top-level Makefile.am for GroveDB C++

ACLOCAL_AMFLAGS = -I m4

# Subdirectories to build
SUBDIRS = third_party/blake3 src include

# Conditional subdirectories
if BUILD_BENCHMARKS
SUBDIRS += bench
endif

if BUILD_TESTS
SUBDIRS += tests
endif

# Extra files to include in distribution
EXTRA_DIST = \
    README.md \
    corpus/corpus.json \
    tools \
    docs

# Clean build artifacts
clean-local:
	-rm -rf build-aux
	-rm -f config.h.in~

# Distribution hook
dist-hook:
	@if test -d "$(srcdir)/.git"; then \
		git --git-dir="$(srcdir)/.git" rev-parse HEAD > "$(distdir)/.git-id"; \
	fi
