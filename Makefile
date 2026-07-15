CC = gcc
CFLAGS = -Wall -Wextra -O2

all: ver1 ver2 ver3

ver1: ver1.c
	$(CC) $(CFLAGS) -o ver1 ver1.c

ver2: ver2.c
	$(CC) $(CFLAGS) -o ver2 ver2.c

ver3: ver3.c
	$(CC) $(CFLAGS) -o ver3 ver3.c

clean:
	rm -f ver1 ver2 ver3

.PHONY: all clean
