TEDIT v6.0.2 - modular classic-UNIX curses editor
================================================

TEDIT is a lightweight curses editor aimed at classic UNIX systems as well as
modern BSD/Linux machines. Syntax highlighting and basic formatting rules are
loaded from external language definition files rather than hard-coded into the
editor.

SOURCE LAYOUT
-------------
  tedit.c          editor/UI/buffers/file browser
  syntax.c/.h      language-definition loader + generic lexer
  format.c/.h      generic comment/indent formatting rules
  syntax/*.conf    language definitions
  Makefile         portable build/install rules
  teditrc.example  example per-user configuration

BUILD
-----
  make

On IRIX the compiler may print its usual MIPSpro licence message. If object
files are produced and the final link succeeds, TEDIT is built.

Run from the source directory with:
  ./tedit

INSTALLATION
------------

System-wide installation for all users:

  make
  make install

The default prefix is /usr/local, producing:

  /usr/local/bin/tedit
  /usr/local/share/tedit/syntax/*.conf
  /usr/local/share/tedit/teditrc.example

On systems requiring administrator privileges, run the install command through
the system's normal privilege mechanism, for example su or sudo.

Personal installation without administrator access:

  make
  make install-user

This installs to:

  ~/.local/bin/tedit
  ~/.local/share/tedit/syntax/*.conf
  ~/.local/share/tedit/teditrc.example

Make sure ~/.local/bin is on PATH.

A custom prefix is also supported:

  make clean
  make PREFIX=/opt/tedit
  make install PREFIX=/opt/tedit

The selected syntax directory is compiled into TEDIT, so when changing PREFIX
for a build, use the same PREFIX for both make and make install.

PACKAGING / DESTDIR
-------------------
DESTDIR is supported for package staging without changing the runtime prefix:

  make
  make install DESTDIR=/tmp/tedit-package

With the default PREFIX this stages files under:

  /tmp/tedit-package/usr/local/bin
  /tmp/tedit-package/usr/local/share/tedit

UNINSTALL
---------
Remove only the files shipped by TEDIT:

  make uninstall

For a custom prefix:

  make uninstall PREFIX=/opt/tedit

The uninstall target removes TEDIT's known syntax files individually rather
than deleting every .conf file in the syntax directory, so locally-added
language definitions are left alone.

MULTIUSER CONFIGURATION
-----------------------
A system install requires no per-user files. Every user can run TEDIT using the
shared binary and shared syntax definitions immediately.

Users may optionally create:

  ~/.teditrc
  ~/.tedit/syntax/*.conf

A useful starting point is:

  cp /usr/local/share/tedit/teditrc.example ~/.teditrc

For a personal ~/.local install, the example is at:

  ~/.local/share/tedit/teditrc.example

TEDIT does not create or modify users' home directories during a system
install.

SYNTAX SEARCH ORDER
-------------------
Definitions are loaded in this order. Later definitions with the same language
name override earlier ones:

  build-time system syntax directory
  ~/.local/share/tedit/syntax
  ./syntax
  $TEDIT_SYNTAX_DIR
  ~/.tedit/syntax
  syntaxdir=... from ~/.teditrc   (highest priority)

The build-time system directory defaults to:

  /usr/local/share/tedit/syntax

and follows PREFIX when TEDIT is built using the supplied Makefile.

ADDING A LANGUAGE
-----------------
Create a .conf file. No recompilation is required.

Example:

  name=Example
  extensions=.ex,.example
  filenames=Examplefile
  line_comment=//
  block_comment_start=/*
  block_comment_end=*/
  strings="'
  preprocessor=#
  case_insensitive=no
  numbers=yes
  escape_strings=yes
  doubled_quotes=no
  keywords=if,else,while,return
  indent_after={
  outdent_before=}
  outdent_chars=}

Supported keys:
  name                 required display/override name
  extensions           comma-separated filename suffixes
  filenames            comma-separated exact basenames
  line_comment         line-comment token; omit for none
  block_comment_start  opening block-comment token
  block_comment_end    closing block-comment token
  strings              characters that can quote strings
  preprocessor         token highlighted to end-of-line at first non-space
  case_insensitive     yes/no
  numbers              yes/no
  escape_strings       yes/no for backslash escaping
  doubled_quotes       yes/no for Ada/Fortran-style doubled quote escaping
  keywords             comma-separated; may occur multiple times
  indent_after         comma-separated line-ending tokens
  outdent_before       comma-separated line-start tokens
  outdent_chars        individual closing characters such as }]

SHIPPED DEFINITIONS
-------------------
  C/C++/headers
  Python
  Shell
  Makefile
  Perl
  Fortran
  Ada
  JSON
  Rust
  Go
  JavaScript
  TypeScript

NOTES
-----
Syntax colour rendering uses the ANSI overlay fallback that works on SGI IRIX
even when old curses reports has_colors()==0.

Formatting is language-driven for line comment/uncomment, newline indentation,
and configured closing-character outdent. It is intentionally lightweight
rather than a full source-code pretty-printer.
