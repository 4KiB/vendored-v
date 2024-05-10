all: example

example: src/example/example

clean:
	@cd src/example; git clean -fdx

rebuild-v:
	@v self

vendor:
	@src/vendor/bin/build

src/vendor/v/v: vendor
	@true

%: %.v src/vendor/v/v | vendor
	v $<

export PATH := $(PWD)/src/vendor/bin:$(PATH)
export MAKEFLAGS += --no-print-director
