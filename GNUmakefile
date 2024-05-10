all: example

example: src/example/example
src/example/example: src/example/example.v | vendor
	v $<

vendor:
	@src/vendor/bin/build

export PATH := $(PWD)/src/vendor/bin:$(PATH)
export MAKEFLAGS += --no-print-director
