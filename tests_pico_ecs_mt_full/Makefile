CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -g

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Darwin)
	CC = clang
endif

SRCS = main.c
OBJS = $(SRCS:.c=.o) pico_ecs_mt_full.o
DEPS = ../pico_ecs_mt_full.h

all: tests

pico_ecs_mt_full.o: ../pico_ecs_mt_full.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

tests: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

run: tests
	./tests

clean:
	rm -f tests *.o
