# TEDIT 7.0 development stages

## Stage 1
Visible buffer tabs, close-buffer support, richer line/block editing,
selection helpers, matching-bracket jump and line bookmarks.

## Stage 2
Project detection, .tedit-project commands, Build/Clean/Run, captured output,
find-in-files and file:line navigation.

## Stage 3
Incremental search, recent files, sessions, backups and recovery snapshots.

## Stage 4
Dual-pane MC-style navigator with copy/move/delete/rename/mkdir, hidden-file
toggle, sorting, quick path and persistent directory bookmarks.

## Stage 5
Split-buffer view, function/symbol sidebar and symbol navigation, optional
mouse support.

## Stage 6
Rectangular selection, tabs/spaces conversion, line sorting, transpose,
repeat-last-edit and additional editing operations.

## Stage 7
Undo/redo rewritten as line-patch operations so history stores changed regions
instead of complete-buffer snapshots.

## Stage 8
LF/CRLF/CR preservation and conversion plus ASCII/UTF-8/8-bit detection.

## Stage 9
Interactive output/results browser and mouse interaction with menus, buffer tabs,
symbols, cursor positioning and scroll wheel where supported.

## Stage 10
Session layout restore and fixes ensuring undo history is finalised before
switching or closing buffers.

## Release
7.0.0 is the first integrated release of the above work.
