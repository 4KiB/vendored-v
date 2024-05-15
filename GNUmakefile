all: build

build: src/example/example

clean:
	cd src/example; git clean -fdx

rebuild-v:
	v self

static: clean build
static: VFLAGS := -freestanding

vendor:
	src/vendor/bin/build

src/example/example: src/vendor/v/v
	cd src/example; v $(VFLAGS) .
	ls -lh $@

src/vendor/v/v: vendor
	true

export PATH := $(PWD)/src/vendor/bin:$(PATH)
export MAKEFLAGS += --no-print-directory

.SILENT:
.PHONY: src/example/example
