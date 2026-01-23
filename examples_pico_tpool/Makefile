CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread -g
LDFLAGS = -pthread

# Targets
EXAMPLE = example

# Source files
EXAMPLE_SRC = example.c

# Object files
EXAMPLE_OBJ = $(EXAMPLE_SRC:.c=.o)

# Build all targets
all: $(EXAMPLE)

# Build example
$(EXAMPLE): $(EXAMPLE_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile object files
%.o: %.c ../pico_tpool.h
	$(CC) $(CFLAGS) -c $< -o $@

# Run the example
run: $(EXAMPLE)
	./$(EXAMPLE)

# Clean build artifacts
clean:
	rm -f $(EXAMPLE) *.o

.PHONY: all clean run
