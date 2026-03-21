# md4s — Streaming markdown parser for C
# Single-file library. Just drop md4s.h + md4s.c into your project.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2
AR      ?= ar

.PHONY: all clean test

all: libmd4s.a

libmd4s.a: md4s.o
	$(AR) rcs $@ $^

md4s.o: md4s.c md4s.h
	$(CC) $(CFLAGS) -c -o $@ md4s.c

test: tests/run_tests
	@./tests/run_tests

tests/run_tests: tests/test_main.c tests/test_md4s.c md4s.c md4s.h tests/test.h
	$(CC) $(CFLAGS) -I. -g3 -O0 -o $@ tests/test_main.c tests/test_md4s.c md4s.c

clean:
	rm -f md4s.o libmd4s.a tests/run_tests
