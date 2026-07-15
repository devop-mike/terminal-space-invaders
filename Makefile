CC = gcc
CFLAGS = -Wall -Wextra -O2

all: invaders

invaders: main.c
	$(CC) $(CFLAGS) -o invaders main.c

clean:
	rm -f invaders

.PHONY: all clean
