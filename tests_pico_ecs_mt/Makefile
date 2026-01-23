CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c99 -pthread -g -O0 -fsanitize=thread
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
	./$(TARGET)

.PHONY: all clean run
