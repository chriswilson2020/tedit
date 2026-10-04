CC=cc
CFLAGS=
PREFIX=/usr/local
DESTDIR=

BINDIR=$(PREFIX)/bin
DATADIR=$(PREFIX)/share/tedit
SYNTAXDIR=$(DATADIR)/syntax

CPPFLAGS=-DTEDIT_SYSTEM_SYNTAX_DIR=\"$(SYNTAXDIR)\"

OBJS=tedit.o syntax.o format.o
SYNTAX_FILES=ada.conf c.conf fortran.conf go.conf javascript.conf json.conf make.conf perl.conf python.conf rust.conf shell.conf typescript.conf

all: tedit

tedit: $(OBJS)
	$(CC) $(CFLAGS) -o tedit $(OBJS) -lcurses

tedit.o: tedit.c syntax.h format.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c tedit.c

syntax.o: syntax.c syntax.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c syntax.c

format.o: format.c format.h syntax.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c format.c

install: tedit
	mkdir -p $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(SYNTAXDIR)
	mkdir -p $(DESTDIR)$(DATADIR)
	cp tedit $(DESTDIR)$(BINDIR)/tedit
	chmod 755 $(DESTDIR)$(BINDIR)/tedit
	cp syntax/*.conf $(DESTDIR)$(SYNTAXDIR)/
	chmod 644 $(DESTDIR)$(SYNTAXDIR)/*.conf
	cp teditrc.example $(DESTDIR)$(DATADIR)/teditrc.example
	chmod 644 $(DESTDIR)$(DATADIR)/teditrc.example

install-user: tedit
	$(MAKE) install PREFIX=$(HOME)/.local

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/tedit
	@for f in $(SYNTAX_FILES); do rm -f $(DESTDIR)$(SYNTAXDIR)/$$f; done
	rm -f $(DESTDIR)$(DATADIR)/teditrc.example
	-rmdir $(DESTDIR)$(SYNTAXDIR) 2>/dev/null
	-rmdir $(DESTDIR)$(DATADIR) 2>/dev/null

clean:
	rm -f tedit $(OBJS)

.PHONY: all install install-user uninstall clean
