CC=cc
CFLAGS=
PREFIX=/usr/local

OBJS=tedit.o syntax.o format.o

all: tedit

tedit: $(OBJS)
	$(CC) $(CFLAGS) -o tedit $(OBJS) -lcurses

tedit.o: tedit.c syntax.h format.h
	$(CC) $(CFLAGS) -c tedit.c

syntax.o: syntax.c syntax.h
	$(CC) $(CFLAGS) -c syntax.c

format.o: format.c format.h syntax.h
	$(CC) $(CFLAGS) -c format.c

install: tedit
	mkdir -p $(PREFIX)/bin
	mkdir -p $(PREFIX)/share/tedit/syntax
	cp tedit $(PREFIX)/bin/tedit
	cp syntax/*.conf $(PREFIX)/share/tedit/syntax/

clean:
	rm -f tedit $(OBJS)
