CXX := "g++"
CXXFLAGS := "-O3 -march=native -std=c++17 -Wall -Wextra"
LDFLAGS := "-lpthread"
INCLUDE := "-I."

BIN_DIR := "bin"
TEST_DIR := "test"

default: all

all: setup testcase speedtest accuracy sanity sanity-disabled
  @echo " All binaries compiled successfully"

setup:
  @mkdir -p {{BIN_DIR}}
  @echo "Created {{BIN_DIR}}/ directory"

testcase: setup
  @echo "-> Compiling testcase..."
  {{CXX}} {{CXXFLAGS}} {{INCLUDE}} {{TEST_DIR}}/testcase.cpp -o {{BIN_DIR}}/testcase {{LDFLAGS}}
  @echo "Built {{BIN_DIR}}/testcase"

speedtest: setup
  @echo "-> Compiling speedtest..."
  {{CXX}} {{CXXFLAGS}} {{INCLUDE}} {{TEST_DIR}}/speedtest.cpp -o {{BIN_DIR}}/speedtest {{LDFLAGS}}
  @echo "Built {{BIN_DIR}}/speedtest"

accuracy: setup
  @echo "-> Compiling accuracy.cpp ..."
  {{CXX}} {{CXXFLAGS}} {{INCLUDE}} {{TEST_DIR}}/accuracy.cpp -o {{BIN_DIR}}/accuracy {{LDFLAGS}}

sanity: setup
  @echo "-> Compiling sanity.cpp ..."
  {{CXX}} {{CXXFLAGS}} {{INCLUDE}} {{TEST_DIR}}/sanity.cpp -o {{BIN_DIR}}/sanity {{LDFLAGS}}
  @echo "Built {{BIN_DIR}}/sanity"

sanity-disabled: setup
  @echo "-> Compiling sanity.cpp [LATTE_DISABLE] ..."
  {{CXX}} {{CXXFLAGS}} -DLATTE_DISABLE {{INCLUDE}} {{TEST_DIR}}/sanity.cpp -o {{BIN_DIR}}/sanity_disabled {{LDFLAGS}}
  @echo "Built {{BIN_DIR}}/sanity_disabled"



run-test: testcase
  @echo "-> Running testcase.cpp ..."
  @./{{BIN_DIR}}/testcase

run-speed: speedtest
  @echo "-> Running speedtest.cpp ..."
  @./{{BIN_DIR}}/speedtest

run-accuracy: accuracy
  @echo "-> Running accuracy.cpp ..."
  @./{{BIN_DIR}}/accuracy

run-sanity: sanity sanity-disabled
  @echo "-> Running sanity.cpp ..."
  @./{{BIN_DIR}}/sanity
  @echo "-> Running sanity.cpp [LATTE_DISABLE] ..."
  @./{{BIN_DIR}}/sanity_disabled

run: run-speed run-test run-accuracy run-sanity

clean:
  @rm -rf {{BIN_DIR}}
  @echo "Cleaned {{BIN_DIR}}/"

rebuild: clean all

check:
  @echo "-> Checking Latte.hpp syntax..."
  {{CXX}} {{CXXFLAGS}} -fsyntax-only Latte.hpp
  {{CXX}} {{CXXFLAGS}} -DLATTE_DISABLE -fsyntax-only Latte.hpp
  @echo "Header syntax is valid (enabled and LATTE_DISABLE)"

info:
  @echo "Compiler: {{CXX}}"
  @{{CXX}} --version | head -n1
  @echo "Flags: {{CXXFLAGS}} {{LDFLAGS}}"
  @echo "Architecture: $(uname -m)"
  @echo "CPU Info:"
  @lscpu | grep "Model name" || echo "  (lscpu not available)"

help:
  @just --list
