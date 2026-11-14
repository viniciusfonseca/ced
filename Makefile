.DEFAULT_GOAL := build

BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= 4
ARGS ?= .

.PHONY: all deps configure build test run clean

all: build

deps:
	git submodule update --init --recursive

configure:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" -DBUILD_TESTING=ON

build: configure
	cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"

test: build
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure

run: build
	"$(BUILD_DIR)/ced" $(ARGS)

clean:
	@if [ -d "$(BUILD_DIR)" ]; then cmake --build "$(BUILD_DIR)" --target clean; fi
