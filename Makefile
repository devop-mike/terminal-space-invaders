CC = gcc
CFLAGS = -Wall -Wextra -O2

all: classic pixelart blit

classic: classic.c
	$(CC) $(CFLAGS) -o classic classic.c

pixelart: pixelart.c
	$(CC) $(CFLAGS) -o pixelart pixelart.c

blit: blit.c
	$(CC) $(CFLAGS) -o blit blit.c

clean:
	rm -f classic pixelart blit

.PHONY: all clean
