CC     = gcc
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -DNDEBUG -O2 #-g

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CC = clang
endif

DEPS   = ../pico_ecs.h ../pico_ecs_mt.h

all: example

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

example: example.o $(DEPS)
	$(CC) -o example example.o #-g

run: example
	@./example

clean:
	rm -f *.o example

.PHONY: all clean
