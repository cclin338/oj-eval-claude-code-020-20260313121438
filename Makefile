.PHONY: all clean

all: code

code: main.c buddy.c buddy.h utils.h
	gcc -o code main.c buddy.c -O2 -Wall

clean:
	rm -f code test *.o