# NOTE: This is here for OpenBSD; `make` prefers GNUmakefile.
#       https://github.com/vlang/v/issues/10954

CC ?= cc
VFLAGS ?=
CFLAGS ?=
LDFLAGS ?=

.PHONY: all

all: v

v:
	$(CC) $(CFLAGS) -std=gnu11 -w -o v1 vc/v.c -lm -lexecinfo -lpthread $(LDFLAGS)
	./v1 -no-parallel -o v2 $(VFLAGS) cmd/v
	./v2 -o v $(VFLAGS) cmd/v
	rm -rf v1 v2
	@echo "V has been successfully built"
	./v run ./cmd/tools/detect_tcc.v
