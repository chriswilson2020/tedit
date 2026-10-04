# TEDIT 7.0

TEDIT is a small, conventional, menu-driven programmer's editor for classic
UNIX and modern Unix-like systems. It is deliberately written in portable C
and plain curses, with an ANSI syntax-colour fallback for systems such as IRIX
where old curses libraries may report no colour support even though the
terminal can display it.

It is intended to sit somewhere between nano/JOE/mcedit and a full IDE:
familiar editing, real menus, project/build tools, multiple buffers and
programming features, without requiring ncurses-specific APIs, C++, Rust,
Python, Lua or a plugin runtime.

## Highlights

- real pull-down menus; shortcuts continue to work while a menu is open
- visible multi-buffer tabs and up to 8 open buffers
- close-buffer and unsaved-change protection
- split-buffer view
- Curses/ANSI syntax highlighting driven by external configuration files
- modular indentation and comment rules
- syntax definitions for C/C++/headers, Python, shell, Makefile, Perl,
  Fortran, Ada, JSON, Rust, Go, JavaScript and TypeScript
- smart indentation and optional auto-pairs
- incremental search, replace and goto-line
- project-wide find-in-files
- project root detection and project file browser
- Build, Clean and Run commands with captured output
- navigable compiler/search output: choose a result and press Enter to jump
  to file and line
- function/symbol sidebar and navigable symbol list
- dual-pane MC-style file navigator
- file navigator copy/move/delete/rename/mkdir
- hidden-file toggle, name/size/date sorting, quick path and directory bookmarks
- recent files
- operation-based line-patch undo/redo rather than a stack of complete buffers
- linear and rectangular/column selections
- cut/copy/paste
- duplicate/delete/move/join line
- indent/unindent and comment/uncomment blocks
- tabs-to-spaces and leading-spaces-to-tabs
- uppercase/lowercase, sort selected lines, transpose characters
- select word/line/all and repeat last editing command
- bracket matching and jump-to-matching-bracket
- line bookmarks
- session restore
- timed crash-recovery snapshots
- optional backup files using the traditional `file~` convention
- LF, CRLF and CR line-ending detection/preservation/conversion
- ASCII / UTF-8 / arbitrary 8-bit text detection
- optional mouse support when the curses implementation supplies it
- preserves keyboard-only operation on old systems

## Build

The normal build is:

    make

Or directly:

    cc -c syntax.c
    cc -c format.c
    cc -c tedit.c
    cc -o tedit tedit.o syntax.o format.o -lcurses

The repository CI builds with GCC in strict C89 mode.

### IRIX

TEDIT is intentionally compatible with the old curses interface used on IRIX.
The syntax highlighter uses ANSI escape sequences for colour when IRIX curses
does not provide curses colour support.

Some MIPSpro installations print an old FlexLM licence message during
compilation even when compilation continues successfully. The useful test is
whether the object files and final `tedit` binary are produced.

If your system discourages compiling on an NFS-mounted home directory, copy the
source tree to a local temporary directory, build there, then install/copy the
result.

## Installation

### Personal installation

Without administrator access:

    make
    make install-user

This installs:

    ~/.local/bin/tedit
    ~/.local/share/tedit/syntax/*.conf
    ~/.local/share/tedit/teditrc.example
    ~/.local/share/tedit/tedit-project.example

Make sure `~/.local/bin` is on PATH.

For sh/bash/ksh:

    export PATH="$HOME/.local/bin:$PATH"

For csh/tcsh:

    set path = ( $HOME/.local/bin $path )

### Install into ~/bin

If you already use `~/bin`:

    make
    make install PREFIX=$HOME

This produces:

    ~/bin/tedit
    ~/share/tedit/syntax/*.conf
    ~/share/tedit/teditrc.example
    ~/share/tedit/tedit-project.example

### System-wide installation

For all users:

    make
    sudo make install

or on a traditional system without sudo:

    su
    make install
    exit

The default installation is:

    /usr/local/bin/tedit
    /usr/local/share/tedit/syntax/*.conf
    /usr/local/share/tedit/teditrc.example
    /usr/local/share/tedit/tedit-project.example

If `make install` reports a permission error under `/usr/local`, use
`make install-user`, install with `PREFIX=$HOME`, or perform the system
install with administrator privileges.

### Custom prefix and packaging

Use the same PREFIX for build and install because the system syntax directory
is compiled into TEDIT:

    make clean
    make PREFIX=/opt/tedit
    make install PREFIX=/opt/tedit

DESTDIR is supported:

    make
    make install DESTDIR=/tmp/tedit-package

### Uninstall

    make uninstall

For a user install:

    make uninstall PREFIX=$HOME/.local

For a `~/bin` style install:

    make uninstall PREFIX=$HOME

## User configuration

TEDIT works without a user configuration file. To customise it, copy the
example to `~/.teditrc`.

Typical configuration:

    tabwidth=4
    linenumbers=on
    syntax=on
    autopairs=on
    session=on
    recovery=on
    recovery_interval=30
    backup=on

Optional syntax override:

    syntaxdir=~/.tedit/syntax

A system install does **not** create files in users' home directories.

## Syntax definition search order

Later definitions with the same language name override earlier ones:

1. build-time system syntax directory, normally
   `/usr/local/share/tedit/syntax`
2. `~/.local/share/tedit/syntax`
3. `./syntax`
4. `$TEDIT_SYNTAX_DIR`
5. `~/.tedit/syntax`
6. `syntaxdir=...` in `~/.teditrc`

Adding a language does not require recompiling TEDIT.

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

## Projects

TEDIT detects a project root by looking upward for common markers including
`.tedit-project`, `.git`, `Makefile`, `GNUmakefile`, `configure` and
`CMakeLists.txt`.

A project can override the default commands with a file named
`.tedit-project` in the project root:

    build=make
    clean=make clean
    run=./myprogram

The repository contains `tedit-project.example`.

Build/run output is captured inside TEDIT. The output/results browser lets you
select lines in the conventional `file:line:message` form and press Enter to
jump directly to that source location.

## File navigator

Open uses a two-pane navigator inspired by Midnight Commander.

Keys inside the navigator:

    Tab         switch pane
    Up/Down     move selection
    Enter       enter a directory or open a file
    Backspace   parent directory
    c           copy selected file/directory to other pane
    m           move selected file/directory to other pane
    d           delete selected file/directory
    r           rename
    n           create directory
    g           go to path
    h           toggle hidden files
    s           cycle name / size / date sorting
    b           bookmark current directory
    j           jump to directory bookmark
    Esc         cancel

## Sessions, backups and recovery

When enabled, TEDIT records the open file set and cursor positions in:

    ~/.tedit/session

It also restores view state such as split view and the symbol sidebar.

Recovery snapshots are periodically written under:

    ~/.tedit/recovery/

They are removed after a normal clean exit. If a terminal, SSH connection or
process dies, use **File -> Recover autosave...** on the next run.

With `backup=on`, saving an existing file first copies its previous contents
to:

    filename~

## Portability

The core intentionally avoids depending on a modern language runtime or regex
library. Mouse support is compiled only when the curses implementation exposes
the relevant API; keyboard operation remains the baseline.

TEDIT currently targets classic and modern Unix-like systems, with IRIX as an
explicit compatibility target.
