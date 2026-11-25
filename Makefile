.DEFAULT_GOAL := build

BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= 4
ARGS ?= .
PREFIX ?= /usr/local
DESTDIR ?=
APPDIR ?= /Applications
EXTENSIONS ?= ON
UNAME_S := $(shell uname -s)
WINDOWS_TARGETS := windows-x86 windows-x64 windows-arm64

.PHONY: all deps configure build windows $(WINDOWS_TARGETS) test run install install-cli install-macos clean

all: build

deps:
	git submodule update --init --recursive

configure:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" -DBUILD_TESTING=ON -DCED_ENABLE_EXTENSIONS="$(EXTENSIONS)"

build: configure
	cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"

windows: $(WINDOWS_TARGETS)

$(WINDOWS_TARGETS): windows-%:
	docker buildx build -f "docker/Dockerfile.windows-$*" --build-arg "BUILD_JOBS=$(JOBS)" --output "type=local,dest=dist/$@" .

test: build
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure

run: build
	"$(BUILD_DIR)/ced" $(ARGS)

install: install-cli
ifeq ($(UNAME_S),Linux)
	install -d "$(DESTDIR)$(PREFIX)/share/applications" "$(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps"
	install -m 644 packaging/dev.ced.Editor.desktop "$(DESTDIR)$(PREFIX)/share/applications/dev.ced.Editor.desktop"
	install -m 644 assets/ced-256.png "$(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/dev.ced.Editor.png"
endif

install-cli: build
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 "$(BUILD_DIR)/ced" "$(DESTDIR)$(PREFIX)/bin/ced"

ifeq ($(UNAME_S),Darwin)
install: install-macos

install-macos: install-cli
	sh packaging/install-macos.sh "$(BUILD_DIR)/CED.app" "$(DESTDIR)$(APPDIR)" "$(DESTDIR)"
else
install-macos:
	@echo "install-macos requires macOS."
	@exit 1
endif

clean:
	@if [ -d "$(BUILD_DIR)" ]; then cmake --build "$(BUILD_DIR)" --target clean; fi
