CC     = gcc
CFLAGS = -std=c99 -g -Wall -Wextra -Wpedantic

ifeq ($(OS),Windows_NT)
    CLIBS = -lmingw32 -lwinmm -lpthread
else
    CLIBS = -lpthread
endif

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    CC = clang
endif

SRCS   = main.c

DEPS   = ../pico_tpool.h ../pico_unit.h
OBJS   = $(SRCS:.c=.o)

all: tests

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

tests: $(OBJS)
	$(CC) -o tests $(OBJS) $(CLIBS)

.PHONY: clean

clean:
	rm -f tests *.o
