TEDIT v6.0.1 - modular classic-UNIX curses editor
================================================

This release refactors syntax highlighting and indentation/comment formatting
out of the editor core.  Languages are now data files, not hard-coded editor
modes.

SOURCE LAYOUT
-------------
  tedit.c          editor/UI/buffers/file browser
  syntax.c/.h      language-definition loader + generic lexer
  format.c/.h      generic comment/indent formatting rules
  syntax/*.conf    language definitions
  Makefile         IRIX-friendly build/install

BUILD ON IRIX
-------------
  make

or directly:
  cc -c syntax.c
  cc -c format.c
  cc -c tedit.c
  cc -o tedit tedit.o syntax.o format.o -lcurses

Run from the source directory and ./syntax is loaded automatically.
For a normal installation:
  make install

Default PREFIX is /usr/local.  Override it if required:
  make PREFIX=/usr/people/chris/local install

SYNTAX SEARCH ORDER
-------------------
Definitions are loaded in this order; later definitions with the same name
override earlier ones:

  /usr/local/share/tedit/syntax
  ./syntax
  $TEDIT_SYNTAX_DIR
  ~/.tedit/syntax
  syntaxdir=... from ~/.teditrc   (highest priority)

ADDING A LANGUAGE
-----------------
Create a .conf file.  No recompilation is needed.

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
  strings              characters that can quote strings, e.g. "'`
  preprocessor         token highlighted to end-of-line at first non-space
  case_insensitive     yes/no
  numbers              yes/no
  escape_strings       yes/no for backslash escaping
  doubled_quotes       yes/no for Ada/Fortran-style doubled quote escaping
  keywords             comma-separated; may occur multiple times
  indent_after         comma-separated line-ending tokens
  outdent_before       comma-separated line-start tokens
  outdent_chars        individual closing characters such as }]

The parser is deliberately simple: no regex library, no ncurses dependency,
and no modern C requirement.  It is intended to stay lightweight enough for
IRIX and other classic UNIX systems.

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
Syntax colour rendering still uses the ANSI overlay fallback that was proven
to work on the SGI O2 when IRIX curses reports has_colors()==0.

Formatting is now language-driven for:
  - line comment/uncomment
  - newline indentation after configured tokens
  - character outdent for configured closing characters

This is intentionally a small declarative formatter, not a full source-code
pretty-printer.  More formatting rules can now be added to the engine without
putting language-specific branches back into tedit.c.