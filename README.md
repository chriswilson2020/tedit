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

### Quick start (personal install)

If you do not have administrator/root access:

    make
    make install-user

This installs:

    ~/.local/bin/tedit
    ~/.local/share/tedit/syntax/*.conf
    ~/.local/share/tedit/teditrc.example

Make sure ~/.local/bin is on your PATH.

For sh/bash/ksh:

    export PATH="$HOME/.local/bin:$PATH"

For csh/tcsh:

    set path = ( $HOME/.local/bin $path )

You can then run:

    tedit

### Install into ~/bin instead

If you already use ~/bin and want TEDIT there:

    make
    make install PREFIX=$HOME

This installs:

    ~/bin/tedit
    ~/share/tedit/syntax/*.conf
    ~/share/tedit/teditrc.example

Make sure ~/bin is on your PATH.

### System-wide install

For all users on the machine:

    make
    sudo make install

or, on systems without sudo:

    su
    make install
    exit

The default system install location is:

    /usr/local/bin/tedit
    /usr/local/share/tedit/syntax/*.conf
    /usr/local/share/tedit/teditrc.example

If you run `make install` without sufficient permissions, you may see an error such as:

    mkdir: cannot create directory '/usr/local/share/tedit': Permission denied

In that case use `make install-user`, `make install PREFIX=$HOME`, or run the system install with the appropriate administrator privileges.

### Custom prefix

To install somewhere else:

    make clean
    make PREFIX=/opt/tedit
    make install PREFIX=/opt/tedit

Use the same PREFIX for both build and install because TEDIT compiles the system syntax path into the binary.

### Package staging with DESTDIR

For packaging:

    make
    make install DESTDIR=/tmp/tedit-package

With the default PREFIX this creates:

    /tmp/tedit-package/usr/local/bin/tedit
    /tmp/tedit-package/usr/local/share/tedit/syntax/

### Uninstall

For a default system install:

    make uninstall

For a custom prefix:

    make uninstall PREFIX=/opt/tedit

For a personal ~/.local install:

    make uninstall PREFIX=$HOME/.local

For a ~/bin-style install:

    make uninstall PREFIX=$HOME

The uninstall target removes only TEDIT's own installed files and shipped syntax definitions.

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
