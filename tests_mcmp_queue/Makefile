CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c99 -pthread -g -O3 -fsanitize=thread -DCACHE_SIZE 64
LDFLAGS = -pthread -fsanitize=thread

TARGET = tests
SOURCES = main.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

run: $(TARGET)
	MallocNanoZone=0 ./$(TARGET)

.PHONY: all clean run
