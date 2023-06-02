# Target library
lib := libfs.a

all: $(lib)

CC := gcc
CFLAGS := -Wall -Wextra -Werror

libfs.a: disk.o fs.o
	ar rcs libfs.a disk.o fs.o

disk.o: disk.c disk.h
	$(CC) $(CFLAGS) -c -o $@ disk.c

fs.o: fs.c fs.h disk.o
	$(CC) $(CFLAGS) -c -o $@ fs.c -l disk.o

clean:
	rm -rf libfs.a disk.o fs.o