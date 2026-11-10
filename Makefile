CC = gcc
CFLAGS = -g -std=c99 -Wall -Wvla -fsanitize=address,undefined

mysh: mysh.o arraylist.o
	$(CC) $(CFLAGS) $^ -o $@

mysh.o: mysh.c arraylist.h
arraylist.o: arraylist.c arraylist.h

%.o: %.c
	$(CC) $(CFLAGS) -c $<

test: mysh
	./mysh test_basic.sh

tar: clean
	tar -cvf p3.tar mysh.c arraylist.c arraylist.h Makefile README test_basic.sh

clean:
	rm -f *.o mysh p3.tar

.PHONY: test tar clean
