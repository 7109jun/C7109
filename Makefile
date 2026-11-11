# Temporary dev build; the official build uses Cmake7109.txt once stage 5 lands.
CXX ?= g++
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter
SRC := $(wildcard compiler/frontend/*.cpp compiler/ir/*.cpp compiler/codegen/*.cpp compiler/pkg/*.cpp) compiler/driver/main.cpp
STDSRC := $(sort $(wildcard stdlib/*.c7))
STD_LICENSE ?= GPL-3.0-only WITH C7109-runtime-library-exception
# `make` builds the compiler and the standard library package together. c7c finds std.l7 beside its executable.
all: c7c std.l7
c7c: $(SRC) $(wildcard compiler/*/*.hpp compiler/*/*.def)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)
# W4 stage 2: the standard library is built by C7109's own build system (stdlib/Cmake7109.txt, dogfooding). The build file's
# output must stay inside stdlib/, so the finished package is copied to ./std.l7 (c7c finds it beside its executable).
# Reproducible: the packer sorts entries and uses fixed timestamps, so two builds are byte-identical.
std.l7: c7c stdlib/Cmake7109.txt $(STDSRC)
	rm -f stdlib/std.l7
	./c7c pack stdlib --no-cache
	cp stdlib/std.l7 $@.tmp && mv $@.tmp $@ && rm -f stdlib/std.l7
test: all
	@bash tests/run.sh
# Every code example in docs/*.md is compiled/run against the real compiler (tools/doccheck.py).
doccheck: all
	python3 tools/doccheck.py
clean:
	rm -f c7c c7c_asan std.l7 std.l7.tmp stdlib/std.l7
.PHONY: all test doccheck clean

# Memory-safety sweep: rebuild the compiler with ASan+UBSan and push every test input through it.
asan: std.l7
	$(CXX) -std=c++20 -g -O0 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o c7c_asan $(SRC)
	@bad=0; for f in tests/cases/[0-9]*.c7 tests/cases/neg_*.c7; do ./c7c_asan parse $$f -Itests/cases >/dev/null 2>/tmp/asan.err; grep -q "Sanitizer\|runtime error" /tmp/asan.err && { echo "SAN $$f"; bad=1; }; done; \
	for f in tests/sema/*.c7; do ./c7c_asan check $$f >/dev/null 2>/tmp/asan.err; grep -q "Sanitizer\|runtime error" /tmp/asan.err && { echo "SAN $$f"; bad=1; }; done; \
	for f in tests/pp/*.c7 tests/cases/neg_pp_ops.c7; do ./c7c_asan pp $$f >/dev/null 2>/tmp/asan.err; grep -q "Sanitizer\|runtime error" /tmp/asan.err && { echo "SAN pp $$f"; bad=1; }; done; \
	for f in tests/run/*.c7 tests/dbg/*.c7; do ./c7c_asan build $$f -o /tmp/san.out >/dev/null 2>/tmp/asan.err; grep -q "Sanitizer\|runtime error" /tmp/asan.err && { echo "SAN $$f"; bad=1; }; done; \
	for f in tests/run/*.c7 tests/dbg/*.c7 tests/chk/*.c7; do ./c7c_asan build $$f --debug -fbounds-check -fdebug-alloc -o /tmp/san.out >/dev/null 2>/tmp/asan.err; grep -q "Sanitizer\|runtime error" /tmp/asan.err && { echo "SAN $$f (checks on)"; bad=1; }; done; \
	for f in tests/toml/valid/*.toml tests/toml/invalid/*.toml tests/toml/stricter/*.toml; do ./c7c_asan tool toml-json $$f >/dev/null 2>/tmp/asan.err; grep -q "Sanitizer\|runtime error" /tmp/asan.err && { echo "SAN $$f"; bad=1; }; done; \
	for f in tests/build/valid/*.txt tests/build/invalid/*.txt; do ./c7c_asan tool buildfile-json $$f >/dev/null 2>/tmp/asan.err; grep -q "Sanitizer\|runtime error" /tmp/asan.err && { echo "SAN $$f"; bad=1; }; done; \
	mkdir -p /tmp/c7asan && printf 'i32 one() { return 1; }\n' > /tmp/c7asan/lib.c7 && ./c7c_asan tool l7-pack /tmp/c7asan/p.l7 Seed 1.0.0 src/lib.c7=/tmp/c7asan/lib.c7 2>/tmp/asan.err && for sub in l7-verify zip-list; do ./c7c_asan tool $$sub /tmp/c7asan/p.l7 >/dev/null 2>/tmp/asan.err; grep -q "Sanitizer\|runtime error" /tmp/asan.err && { echo "SAN $$sub p.l7"; bad=1; }; done; \
	python3 -I tests/fuzz_pkg.py 500 3 ./c7c_asan || bad=1; \
	echo "asan sweep bad=$$bad"; [ $$bad -eq 0 ]
.PHONY: asan

# Long mutation-fuzz run of the stage-5 parsers (override N / SEED): make fuzz N=20000 SEED=9
N ?= 5000
SEED ?= 1
fuzz: c7c
	python3 -I tests/fuzz_pkg.py $(N) $(SEED)
.PHONY: fuzz

# ABI measurement (slow, ~2 min): results go to bench/abi/results/*.json, tables in bench/abi/RESULTS.md
abi-bench: c7c
	python3 bench/abi/run.py 5
.PHONY: abi-bench
