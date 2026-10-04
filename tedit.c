/*
 * tedit v6 - modular curses text/code editor for classic UNIX
 *
 * Build:
 *   cc -o tedit tedit.c syntax.c format.c -lcurses
 *
 * Features:
 *   - interactive Ctrl-T menu bar
 *   - open/save/save-as
 *   - smart indentation and tabbing
 *   - simple C/C++ syntax highlighting
 *   - find / replace / replace all
 *   - goto line
 *   - linear undo / redo
 *   - selection mode + copy/cut/paste
 *   - current-line copy/cut still supported
 *   - bracket matching indicator
 *   - line numbers, status bar
 *   - clears terminal on exit
 *
 * Key bindings:
 *   Ctrl-T  menu
 *   Ctrl-O  open
 *   Ctrl-S  save
 *   Ctrl-A  save as
 *   Ctrl-F  find
 *   Ctrl-H  replace
 *   Ctrl-L  goto line
 *   Ctrl-Z  undo
 *   Ctrl-R  redo
 *   Ctrl-B  toggle selection
 *   Ctrl-C  copy selection (or current line if no selection)
 *   Ctrl-X  cut selection (or current line if no selection)
 *   Ctrl-V  paste text
 *   Ctrl-G  help
 *   Ctrl-Q  quit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curses.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "syntax.h"
#include "format.h"

#ifndef TEDIT_SYSTEM_SYNTAX_DIR
#define TEDIT_SYSTEM_SYNTAX_DIR "/usr/local/share/tedit/syntax"
#endif

#define MAX_LINES   20000
#define MAX_LINE    8192
#define NAME_LEN    1024
#define STATUS_LEN  1024
#define SEARCH_LEN  256
#define CLIP_LEN    65536
#define UNDO_DEPTH  64
#define BROWSER_MAX  512
#define PATH_LEN     2048
#define MAX_BUFFERS  8

#define CTRL_KEY(x) ((x) & 0x1f)

#define CP_NORMAL    1
#define CP_KEYWORD   2
#define CP_STRING    3
#define CP_COMMENT   4
#define CP_NUMBER    5
#define CP_PREPROC   6
#define CP_LINENO    7
#define CP_STATUS    8
#define CP_MENU      9
#define CP_MENU_SEL 10
#define CP_SELECT   11
#define CP_MATCH    12

typedef struct {
    char **snap_lines;
    int snap_nlines;
    int snap_cy, snap_cx;
    int snap_rowoff, snap_coloff;
} Snapshot;

typedef struct {
    const char *label;
    int action;
} MenuItem;

enum {
    ACT_NONE = 0,
    ACT_NEW_BUFFER,
    ACT_PREV_BUFFER,
    ACT_NEXT_BUFFER,
    ACT_OPEN,
    ACT_SAVE,
    ACT_SAVE_AS,
    ACT_CLOSE_BUFFER,
    ACT_QUIT,
    ACT_UNDO,
    ACT_REDO,
    ACT_SELECT,
    ACT_COPY,
    ACT_CUT,
    ACT_PASTE,
    ACT_WORD_PREV,
    ACT_WORD_NEXT,
    ACT_INDENT,
    ACT_UNINDENT,
    ACT_COMMENT,
    ACT_DUP_LINE,
    ACT_DELETE_LINE,
    ACT_MOVE_LINE_UP,
    ACT_MOVE_LINE_DOWN,
    ACT_JOIN_LINE,
    ACT_TRIM_WS,
    ACT_UPPERCASE,
    ACT_LOWERCASE,
    ACT_SELECT_WORD,
    ACT_SELECT_LINE,
    ACT_SELECT_ALL,
    ACT_MATCH_BRACKET,
    ACT_BOOKMARK_TOGGLE,
    ACT_BOOKMARK_NEXT,
    ACT_FIND,
    ACT_REPLACE,
    ACT_GOTO,
    ACT_TOGGLE_SYNTAX,
    ACT_TOGGLE_LINES,
    ACT_TOGGLE_AUTOPAIRS,
    ACT_TAB2,
    ACT_TAB4,
    ACT_TAB8,
    ACT_HELP,
    ACT_ABOUT
};

typedef struct {
    char *linev[MAX_LINES];
    int line_count;
    int cursor_y, cursor_x;
    int row_offset, col_offset;
    int dirty;
    char fname[NAME_LEN];
} Buffer;

static Buffer buffers[MAX_BUFFERS];
static int buffer_count = 1;
static int curbuf = 0;
static unsigned char bookmarks[MAX_BUFFERS][MAX_LINES];

#define lines    (buffers[curbuf].linev)
#define nlines   (buffers[curbuf].line_count)
#define cy       (buffers[curbuf].cursor_y)
#define cx       (buffers[curbuf].cursor_x)
#define rowoff   (buffers[curbuf].row_offset)
#define coloff   (buffers[curbuf].col_offset)
#define modified (buffers[curbuf].dirty)
#define filename (buffers[curbuf].fname)

static int tab_width = 4;
static int use_color = 0;
static int autopairs = 0;

static int syntax_enabled = 1;
static int line_numbers = 1;
static char syntax_dir_override[PATH_LEN];

static int selecting = 0;
static int sel_sy = 0, sel_sx = 0;

static char statusmsg[STATUS_LEN];
static char last_search[SEARCH_LEN];

static char *clipboard = NULL;

static Snapshot undo_stack[UNDO_DEPTH];
static int undo_count = 0;
static Snapshot redo_stack[UNDO_DEPTH];
static int redo_count = 0;


static const MenuItem file_menu[] = {
    {"New buffer     ^N", ACT_NEW_BUFFER},
    {"Previous buf   ^P", ACT_PREV_BUFFER},
    {"Next buffer    ^E", ACT_NEXT_BUFFER},
    {"----------------", ACT_NONE},
    {"Open...        ^O", ACT_OPEN},
    {"Save           ^S", ACT_SAVE},
    {"Save As...     ^A", ACT_SAVE_AS},
    {"Close buffer", ACT_CLOSE_BUFFER},
    {"----------------", ACT_NONE},
    {"Quit           ^Q", ACT_QUIT}
};

static const MenuItem edit_menu[] = {
    {"Undo           ^Z", ACT_UNDO},
    {"Redo           ^R", ACT_REDO},
    {"----------------", ACT_NONE},
    {"Select         ^B", ACT_SELECT},
    {"Copy           ^C", ACT_COPY},
    {"Cut            ^X", ACT_CUT},
    {"Paste          ^V", ACT_PASTE},
    {"----------------", ACT_NONE},
    {"Previous word  ^U", ACT_WORD_PREV},
    {"Next word      ^D", ACT_WORD_NEXT},
    {"Indent block", ACT_INDENT},
    {"Unindent block", ACT_UNINDENT},
    {"Comment toggle", ACT_COMMENT},
    {"----------------", ACT_NONE},
    {"Duplicate line", ACT_DUP_LINE},
    {"Delete line", ACT_DELETE_LINE},
    {"Move line up", ACT_MOVE_LINE_UP},
    {"Move line down", ACT_MOVE_LINE_DOWN},
    {"Join with next", ACT_JOIN_LINE},
    {"Trim trailing WS", ACT_TRIM_WS},
    {"Uppercase", ACT_UPPERCASE},
    {"Lowercase", ACT_LOWERCASE},
    {"Select word", ACT_SELECT_WORD},
    {"Select line", ACT_SELECT_LINE},
    {"Select all", ACT_SELECT_ALL}
};

static const MenuItem search_menu[] = {
    {"Find...        ^F", ACT_FIND},
    {"Replace...     ^H", ACT_REPLACE},
    {"Goto line...   ^L", ACT_GOTO},
    {"Matching bracket", ACT_MATCH_BRACKET},
    {"Toggle bookmark", ACT_BOOKMARK_TOGGLE},
    {"Next bookmark", ACT_BOOKMARK_NEXT}
};

static const MenuItem options_menu[] = {
    {"Toggle syntax", ACT_TOGGLE_SYNTAX},
    {"Toggle line numbers", ACT_TOGGLE_LINES},
    {"Toggle auto-pairs", ACT_TOGGLE_AUTOPAIRS},
    {"Tab width: 2", ACT_TAB2},
    {"Tab width: 4", ACT_TAB4},
    {"Tab width: 8", ACT_TAB8}
};

static const MenuItem help_menu[] = {
    {"Keyboard help", ACT_HELP},
    {"About TEDIT", ACT_ABOUT}
};

static const char *menu_names[] = {
    "File", "Edit", "Search", "Options", "Help"
};

static char *dupstr(const char *s)
{
    char *p;
    p = (char *)malloc(strlen(s) + 1);
    if (p != NULL)
        strcpy(p, s);
    return p;
}

static void set_status(const char *s)
{
    strncpy(statusmsg, s, STATUS_LEN - 1);
    statusmsg[STATUS_LEN - 1] = '\0';
}

static void clear_stack(Snapshot *stack, int *count);
static int confirm_yes_no(const char *message);

static void init_buffer(void)
{
    int i;

    for (i = 0; i < MAX_LINES; i++)
        lines[i] = NULL;

    lines[0] = dupstr("");
    nlines = 1;
    cy = cx = 0;
    rowoff = coloff = 0;
    modified = 0;
    filename[0] = '\0';
    memset(bookmarks[curbuf], 0, MAX_LINES);
}

static void free_buffer_index(int index)
{
    int i;
    Buffer *b;

    b = &buffers[index];

    for (i = 0; i < b->line_count; i++) {
        if (b->linev[i] != NULL)
            free(b->linev[i]);
        b->linev[i] = NULL;
    }

    b->line_count = 0;
}

static void free_buffer(void)
{
    free_buffer_index(curbuf);
}

static void reset_edit_history(void)
{
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);
    selecting = 0;
}

static int create_new_buffer(void)
{
    if (buffer_count >= MAX_BUFFERS) {
        set_status("Maximum 8 buffers");
        return 0;
    }

    curbuf = buffer_count;
    buffer_count++;
    init_buffer();
    reset_edit_history();
    set_status("New buffer");
    return 1;
}

static void switch_buffer(int dir)
{
    if (buffer_count <= 1) {
        set_status("Only one buffer");
        return;
    }

    curbuf += dir;

    if (curbuf < 0)
        curbuf = buffer_count - 1;
    if (curbuf >= buffer_count)
        curbuf = 0;

    reset_edit_history();

    sprintf(statusmsg, "Buffer %d/%d: %s",
            curbuf + 1, buffer_count,
            filename[0] ? filename : "[No Name]");
}


static void close_current_buffer(void)
{
    int i;

    if (modified) {
        char msg[STATUS_LEN];

        sprintf(msg, "Buffer %s has unsaved changes. Close without saving? (y/N)",
                filename[0] ? filename : "[No Name]");

        if (!confirm_yes_no(msg)) {
            set_status("Close cancelled");
            return;
        }
    }

    free_buffer_index(curbuf);

    if (buffer_count == 1) {
        init_buffer();
        reset_edit_history();
        set_status("New empty buffer");
        return;
    }

    for (i = curbuf; i < buffer_count - 1; i++) {
        buffers[i] = buffers[i + 1];
        memcpy(bookmarks[i], bookmarks[i + 1], MAX_LINES);
    }

    memset(&buffers[buffer_count - 1], 0, sizeof(Buffer));
    memset(bookmarks[buffer_count - 1], 0, MAX_LINES);
    buffer_count--;

    if (curbuf >= buffer_count)
        curbuf = buffer_count - 1;

    reset_edit_history();
    sprintf(statusmsg, "Closed buffer; now %d/%d", curbuf + 1, buffer_count);
}


static void free_snapshot(Snapshot *s)
{
    int i;
    if (s->snap_lines != NULL) {
        for (i = 0; i < s->snap_nlines; i++)
            if (s->snap_lines[i] != NULL)
                free(s->snap_lines[i]);
        free(s->snap_lines);
    }
    s->snap_lines = NULL;
    s->snap_nlines = 0;
}

static int make_snapshot(Snapshot *s)
{
    int i;

    s->snap_lines = (char **)malloc(sizeof(char *) * nlines);
    if (s->snap_lines == NULL)
        return -1;

    s->snap_nlines = nlines;
    s->snap_cy = cy;
    s->snap_cx = cx;
    s->snap_rowoff = rowoff;
    s->snap_coloff = coloff;

    for (i = 0; i < nlines; i++) {
        s->snap_lines[i] = dupstr(lines[i]);
        if (s->snap_lines[i] == NULL) {
            int j;
            for (j = 0; j < i; j++)
                free(s->snap_lines[j]);
            free(s->snap_lines);
            s->snap_lines = NULL;
            return -1;
        }
    }

    return 0;
}

static void restore_snapshot(const Snapshot *s)
{
    int i;

    free_buffer();

    nlines = s->snap_nlines;
    for (i = 0; i < nlines; i++)
        lines[i] = dupstr(s->snap_lines[i]);

    cy = s->snap_cy;
    cx = s->snap_cx;
    rowoff = s->snap_rowoff;
    coloff = s->snap_coloff;
    modified = 1;
    selecting = 0;
}

static void clear_stack(Snapshot *stack, int *count)
{
    int i;
    for (i = 0; i < *count; i++)
        free_snapshot(&stack[i]);
    *count = 0;
}

static void push_undo(void)
{
    int i;

    if (undo_count == UNDO_DEPTH) {
        free_snapshot(&undo_stack[0]);
        for (i = 1; i < UNDO_DEPTH; i++)
            undo_stack[i - 1] = undo_stack[i];
        undo_count--;
    }

    if (make_snapshot(&undo_stack[undo_count]) == 0)
        undo_count++;

    clear_stack(redo_stack, &redo_count);
}

static void do_undo(void)
{
    Snapshot cur;

    if (undo_count <= 0) {
        set_status("Nothing to undo");
        return;
    }

    if (make_snapshot(&cur) == 0) {
        if (redo_count == UNDO_DEPTH) {
            free_snapshot(&redo_stack[0]);
            memmove(&redo_stack[0], &redo_stack[1],
                    sizeof(Snapshot) * (UNDO_DEPTH - 1));
            redo_count--;
        }
        redo_stack[redo_count++] = cur;
    }

    undo_count--;
    restore_snapshot(&undo_stack[undo_count]);
    free_snapshot(&undo_stack[undo_count]);

    set_status("Undo");
}

static void do_redo(void)
{
    Snapshot cur;

    if (redo_count <= 0) {
        set_status("Nothing to redo");
        return;
    }

    if (make_snapshot(&cur) == 0) {
        if (undo_count == UNDO_DEPTH) {
            free_snapshot(&undo_stack[0]);
            memmove(&undo_stack[0], &undo_stack[1],
                    sizeof(Snapshot) * (UNDO_DEPTH - 1));
            undo_count--;
        }
        undo_stack[undo_count++] = cur;
    }

    redo_count--;
    restore_snapshot(&redo_stack[redo_count]);
    free_snapshot(&redo_stack[redo_count]);

    set_status("Redo");
}


static const SyntaxDef *active_syntax(void)
{
    if (!syntax_enabled)
        return NULL;
    return syntax_detect(filename);
}

static const char *syntax_name(void)
{
    const SyntaxDef *def;
    def = active_syntax();
    return def != NULL ? def->name : "TEXT";
}

static void load_syntax_definitions(void)
{
    char path[PATH_LEN];
    char *home;
    char *envdir;

    syntax_init();

    /* Lowest priority: installation location selected at build time. */
    syntax_load_dir(TEDIT_SYSTEM_SYNTAX_DIR);

    /* Conventional per-user local install location. */
    home = getenv("HOME");
    if (home != NULL) {
        sprintf(path, "%s/.local/share/tedit/syntax", home);
        syntax_load_dir(path);
    }

    /* Convenient when running directly from the source directory. */
    syntax_load_dir("./syntax");

    /* Optional site/user-selected location. */
    envdir = getenv("TEDIT_SYNTAX_DIR");
    if (envdir != NULL && *envdir != '\0')
        syntax_load_dir(envdir);

    /* Highest priority: per-user definitions and overrides. */
    if (home != NULL) {
        sprintf(path, "%s/.tedit/syntax", home);
        syntax_load_dir(path);
    }

    /* Explicit ~/.teditrc syntaxdir wins over every other location. */
    if (syntax_dir_override[0] != '\0') {
        if (syntax_dir_override[0] == '~' &&
            syntax_dir_override[1] == '/' && home != NULL) {
            sprintf(path, "%s/%s", home, syntax_dir_override + 2);
            syntax_load_dir(path);
        } else {
            syntax_load_dir(syntax_dir_override);
        }
    }
}

static void load_config(void)
{
    char path[NAME_LEN];
    char buf[256];
    char *home;
    FILE *fp;

    home = getenv("HOME");
    if (home == NULL)
        return;

    sprintf(path, "%s/.teditrc", home);

    fp = fopen(path, "r");
    if (fp == NULL)
        return;

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        char *p;

        p = strchr(buf, '\n');
        if (p != NULL)
            *p = '\0';

        if (!strncmp(buf, "tabwidth=", 9)) {
            int n;
            n = atoi(buf + 9);
            if (n == 2 || n == 4 || n == 8)
                tab_width = n;
        } else if (!strcmp(buf, "linenumbers=on")) {
            line_numbers = 1;
        } else if (!strcmp(buf, "linenumbers=off")) {
            line_numbers = 0;
        } else if (!strcmp(buf, "syntax=on")) {
            syntax_enabled = 1;
        } else if (!strcmp(buf, "syntax=off")) {
            syntax_enabled = 0;
        } else if (!strcmp(buf, "autopairs=on")) {
            autopairs = 1;
        } else if (!strcmp(buf, "autopairs=off")) {
            autopairs = 0;
        } else if (!strncmp(buf, "syntaxdir=", 10)) {
            strncpy(syntax_dir_override, buf + 10, PATH_LEN - 1);
            syntax_dir_override[PATH_LEN - 1] = '\0';
        }
    }

    fclose(fp);
}


static int load_file(const char *name)
{
    FILE *fp;
    char buf[MAX_LINE];
    int len;

    fp = fopen(name, "r");

    if (fp == NULL) {
        free_buffer();
        lines[0] = dupstr("");
        nlines = 1;

        strncpy(filename, name, NAME_LEN - 1);
        filename[NAME_LEN - 1] = '\0';

        modified = 0;
        cy = cx = rowoff = coloff = 0;
        selecting = 0;
        clear_stack(undo_stack, &undo_count);
        clear_stack(redo_stack, &redo_count);

        sprintf(statusmsg, "New file: %s", filename);
        return 0;
    }

    free_buffer();
    nlines = 0;

    while (fgets(buf, sizeof(buf), fp) != NULL && nlines < MAX_LINES) {
        len = (int)strlen(buf);

        while (len > 0 &&
              (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[len - 1] = '\0';
            len--;
        }

        lines[nlines] = dupstr(buf);
        if (lines[nlines] == NULL) {
            fclose(fp);
            set_status("Out of memory while loading");
            return -1;
        }
        nlines++;
    }

    fclose(fp);

    if (nlines == 0) {
        lines[0] = dupstr("");
        nlines = 1;
    }

    strncpy(filename, name, NAME_LEN - 1);
    filename[NAME_LEN - 1] = '\0';

    modified = 0;
    cy = cx = rowoff = coloff = 0;
    selecting = 0;
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);

    sprintf(statusmsg, "Loaded %s", filename);

    return 0;
}

static int save_file_as(const char *name)
{
    FILE *fp;
    int i;

    fp = fopen(name, "w");
    if (fp == NULL) {
        sprintf(statusmsg, "Cannot write %s", name);
        return -1;
    }

    for (i = 0; i < nlines; i++) {
        fputs(lines[i], fp);
        if (i < nlines - 1)
            fputc('\n', fp);
    }

    fclose(fp);

    strncpy(filename, name, NAME_LEN - 1);
    filename[NAME_LEN - 1] = '\0';

    modified = 0;
    sprintf(statusmsg, "Saved %s", filename);

    return 0;
}

static int insert_char_at(int row, int col, int ch)
{
    char *old;
    char *p;
    int len;

    old = lines[row];
    len = (int)strlen(old);

    if (len >= MAX_LINE - 2)
        return -1;

    if (col < 0)
        col = 0;
    if (col > len)
        col = len;

    p = (char *)malloc(len + 2);
    if (p == NULL)
        return -1;

    memcpy(p, old, col);
    p[col] = (char)ch;
    strcpy(p + col + 1, old + col);

    free(old);
    lines[row] = p;

    return 0;
}

static int leading_spaces(const char *s)
{
    int i;
    int count;

    i = 0;
    count = 0;

    while (s[i] == ' ' || s[i] == '\t') {
        if (s[i] == '\t')
            count += tab_width;
        else
            count++;
        i++;
    }

    return count;
}

static int prefix_is_whitespace(const char *s, int upto)
{
    int i;

    for (i = 0; i < upto; i++) {
        if (s[i] != ' ' && s[i] != '\t')
            return 0;
    }

    return 1;
}


static int insert_spaces(int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (insert_char_at(cy, cx, ' ') != 0)
            return -1;
        cx++;
    }

    modified = 1;
    return 0;
}

static void smart_tab(void)
{
    int count;

    push_undo();

    count = tab_width - (cx % tab_width);
    if (count <= 0)
        count = tab_width;

    insert_spaces(count);
}

static void smart_outdent_for_brace(void)
{
    char *line;
    int remove_count;
    int len;

    line = lines[cy];

    if (!prefix_is_whitespace(line, cx))
        return;

    remove_count = cx % tab_width;
    if (remove_count == 0)
        remove_count = tab_width;

    if (remove_count > cx)
        remove_count = cx;

    len = (int)strlen(line);

    memmove(line + cx - remove_count,
            line + cx,
            len - cx + 1);

    cx -= remove_count;
    modified = 1;
}

static void insert_char(int ch)
{
    push_undo();

    if (format_outdent_char(active_syntax(), ch) &&
        prefix_is_whitespace(lines[cy], cx))
        smart_outdent_for_brace();

    if (insert_char_at(cy, cx, ch) == 0) {
        cx++;
        modified = 1;
    }
}

static void delete_backward(void)
{
    char *line;
    int len;

    push_undo();

    line = lines[cy];
    len = (int)strlen(line);

    if (cx > 0) {
        int remove_count;

        remove_count = 1;

        if (prefix_is_whitespace(line, cx)) {
            remove_count = cx % tab_width;
            if (remove_count == 0)
                remove_count = tab_width;
            if (remove_count > cx)
                remove_count = cx;
        }

        memmove(line + cx - remove_count,
                line + cx,
                len - cx + 1);

        cx -= remove_count;
        modified = 1;
        return;
    }

    if (cy > 0) {
        char *prev;
        char *joined;
        int plen;
        int i;

        prev = lines[cy - 1];
        plen = (int)strlen(prev);

        joined = (char *)malloc(plen + len + 1);
        if (joined == NULL)
            return;

        strcpy(joined, prev);
        strcat(joined, line);

        free(prev);
        free(line);

        lines[cy - 1] = joined;

        for (i = cy; i < nlines - 1; i++)
            lines[i] = lines[i + 1];

        lines[nlines - 1] = NULL;
        nlines--;

        cy--;
        cx = plen;
        modified = 1;
    }
}

static void delete_forward(void)
{
    char *line;
    int len;

    push_undo();

    line = lines[cy];
    len = (int)strlen(line);

    if (cx < len) {
        memmove(line + cx,
                line + cx + 1,
                len - cx);

        modified = 1;
        return;
    }

    if (cy < nlines - 1) {
        char *next;
        char *joined;
        int i;

        next = lines[cy + 1];

        joined = (char *)malloc(strlen(line) + strlen(next) + 1);
        if (joined == NULL)
            return;

        strcpy(joined, line);
        strcat(joined, next);

        free(line);
        free(next);

        lines[cy] = joined;

        for (i = cy + 1; i < nlines - 1; i++)
            lines[i] = lines[i + 1];

        lines[nlines - 1] = NULL;
        nlines--;

        modified = 1;
    }
}

static void insert_newline(void)
{
    char *line;
    char *left;
    char *right;
    int len;
    int i;
    int indent;

    if (nlines >= MAX_LINES)
        return;

    push_undo();

    line = lines[cy];
    len = (int)strlen(line);

    left = (char *)malloc(cx + 1);
    right = (char *)malloc(len - cx + 1);

    if (left == NULL || right == NULL) {
        if (left != NULL)
            free(left);
        if (right != NULL)
            free(right);
        return;
    }

    memcpy(left, line, cx);
    left[cx] = '\0';
    strcpy(right, line + cx);

    indent = format_next_indent(active_syntax(), left,
                                leading_spaces(line), tab_width);

    free(line);

    for (i = nlines; i > cy + 1; i--)
        lines[i] = lines[i - 1];

    lines[cy] = left;
    lines[cy + 1] = right;
    nlines++;

    cy++;
    cx = 0;

    if (indent > 0)
        insert_spaces(indent);

    modified = 1;
}

static void normalize_selection(int *sy, int *sx, int *ey, int *ex)
{
    if (!selecting) {
        *sy = *ey = cy;
        *sx = 0;
        *ex = (int)strlen(lines[cy]);
        return;
    }

    if (sel_sy < cy || (sel_sy == cy && sel_sx <= cx)) {
        *sy = sel_sy; *sx = sel_sx;
        *ey = cy;     *ex = cx;
    } else {
        *sy = cy;     *sx = cx;
        *ey = sel_sy; *ex = sel_sx;
    }
}

static int pos_selected(int row, int col)
{
    int sy, sx, ey, ex;

    if (!selecting)
        return 0;

    normalize_selection(&sy, &sx, &ey, &ex);

    if (row < sy || row > ey)
        return 0;

    if (sy == ey)
        return row == sy && col >= sx && col < ex;

    if (row == sy)
        return col >= sx;
    if (row == ey)
        return col < ex;

    return 1;
}

static void set_clipboard_text(const char *s)
{
    if (clipboard != NULL)
        free(clipboard);

    clipboard = dupstr(s);
}

static void copy_selection_or_line(void)
{
    int sy, sx, ey, ex;
    int size;
    int row;
    char *buf;
    int pos;

    normalize_selection(&sy, &sx, &ey, &ex);

    size = 1;
    for (row = sy; row <= ey; row++)
        size += (int)strlen(lines[row]) + 1;

    buf = (char *)malloc(size);
    if (buf == NULL) {
        set_status("Out of memory");
        return;
    }

    pos = 0;

    for (row = sy; row <= ey; row++) {
        int start;
        int end;
        int len;

        start = 0;
        end = (int)strlen(lines[row]);

        if (row == sy)
            start = sx;
        if (row == ey)
            end = ex;

        if (end < start)
            end = start;

        len = end - start;

        if (len > 0) {
            memcpy(buf + pos, lines[row] + start, len);
            pos += len;
        }

        if (row < ey)
            buf[pos++] = '\n';
    }

    buf[pos] = '\0';
    set_clipboard_text(buf);
    free(buf);

    set_status(selecting ? "Selection copied" : "Line copied");
}

static void delete_selection(void)
{
    int sy, sx, ey, ex;

    if (!selecting)
        return;

    normalize_selection(&sy, &sx, &ey, &ex);

    push_undo();

    if (sy == ey) {
        char *line = lines[sy];
        int len = (int)strlen(line);

        memmove(line + sx, line + ex, len - ex + 1);
        cy = sy;
        cx = sx;
    } else {
        char *newfirst;
        int tail_len;
        int i;
        int remove_count;

        tail_len = (int)strlen(lines[ey]) - ex;

        newfirst = (char *)malloc(sx + tail_len + 1);
        if (newfirst == NULL)
            return;

        memcpy(newfirst, lines[sy], sx);
        strcpy(newfirst + sx, lines[ey] + ex);

        free(lines[sy]);

        for (i = sy + 1; i <= ey; i++)
            free(lines[i]);

        lines[sy] = newfirst;

        remove_count = ey - sy;

        for (i = sy + 1; i + remove_count < nlines; i++)
            lines[i] = lines[i + remove_count];

        for (; i < nlines; i++)
            lines[i] = NULL;

        nlines -= remove_count;
        cy = sy;
        cx = sx;
    }

    selecting = 0;
    modified = 1;
}

static void cut_selection_or_line(void)
{
    if (selecting) {
        copy_selection_or_line();
        delete_selection();
        set_status("Selection cut");
    } else {
        selecting = 1;
        sel_sy = cy;
        sel_sx = 0;
        cx = (int)strlen(lines[cy]);
        copy_selection_or_line();
        delete_selection();
        set_status("Line cut");
    }
}

static void paste_text(void)
{
    char *copy;
    char *p;
    char *seg;
    int first;

    if (clipboard == NULL) {
        set_status("Clipboard empty");
        return;
    }

    push_undo();

    if (selecting)
        delete_selection();

    copy = dupstr(clipboard);
    if (copy == NULL)
        return;

    p = copy;
    first = 1;

    while ((seg = strchr(p, '\n')) != NULL) {
        int i;

        *seg = '\0';

        if (!first)
            insert_newline();

        for (i = 0; p[i] != '\0'; i++)
            insert_char_at(cy, cx++, p[i]);

        modified = 1;
        p = seg + 1;
        first = 0;
    }

    if (!first)
        insert_newline();

    {
        int i;
        for (i = 0; p[i] != '\0'; i++)
            insert_char_at(cy, cx++, p[i]);
    }

    modified = 1;
    free(copy);
    set_status("Pasted");
}


static void move_word_left(void)
{
    if (cx == 0 && cy > 0) {
        cy--;
        cx = (int)strlen(lines[cy]);
    }

    while (cx > 0 &&
           isspace((unsigned char)lines[cy][cx - 1]))
        cx--;

    while (cx > 0 &&
          (isalnum((unsigned char)lines[cy][cx - 1]) ||
           lines[cy][cx - 1] == '_'))
        cx--;
}

static void move_word_right(void)
{
    int len;

    len = (int)strlen(lines[cy]);

    if (cx >= len && cy < nlines - 1) {
        cy++;
        cx = 0;
        len = (int)strlen(lines[cy]);
    }

    while (cx < len &&
           isspace((unsigned char)lines[cy][cx]))
        cx++;

    while (cx < len &&
          (isalnum((unsigned char)lines[cy][cx]) ||
           lines[cy][cx] == '_'))
        cx++;
}

static void selected_line_range(int *start, int *end)
{
    int sy, sx, ey, ex;

    if (!selecting) {
        *start = *end = cy;
        return;
    }

    normalize_selection(&sy, &sx, &ey, &ex);
    *start = sy;
    *end = ey;

    if (ex == 0 && ey > sy)
        (*end)--;
}

static void indent_block(void)
{
    int start, end;
    int row;
    int i;

    selected_line_range(&start, &end);
    push_undo();

    for (row = start; row <= end; row++) {
        char *old;
        char *p;
        int len;

        old = lines[row];
        len = (int)strlen(old);

        if (len + tab_width >= MAX_LINE)
            continue;

        p = (char *)malloc(len + tab_width + 1);
        if (p == NULL)
            continue;

        for (i = 0; i < tab_width; i++)
            p[i] = ' ';

        strcpy(p + tab_width, old);
        free(old);
        lines[row] = p;
    }

    if (cy >= start && cy <= end)
        cx += tab_width;

    if (selecting)
        sel_sx += tab_width;

    modified = 1;
    set_status("Indented");
}

static void unindent_block(void)
{
    int start, end;
    int row;

    selected_line_range(&start, &end);
    push_undo();

    for (row = start; row <= end; row++) {
        char *line;
        int remove;
        int len;

        line = lines[row];
        len = (int)strlen(line);
        remove = 0;

        if (line[0] == '\t') {
            remove = 1;
        } else {
            while (remove < tab_width &&
                   remove < len &&
                   line[remove] == ' ')
                remove++;
        }

        if (remove > 0)
            memmove(line, line + remove, len - remove + 1);
    }

    if (cy >= start && cy <= end) {
        if (cx >= tab_width)
            cx -= tab_width;
        else
            cx = 0;
    }

    if (selecting) {
        if (sel_sx >= tab_width)
            sel_sx -= tab_width;
        else
            sel_sx = 0;
    }

    modified = 1;
    set_status("Unindented");
}

static const char *comment_prefix(void)
{
    return format_comment_prefix(active_syntax());
}

static int line_comment_pos(const char *line)
{
    int i;
    i = 0;
    while (line[i] == ' ' || line[i] == '\t')
        i++;
    return i;
}

static void toggle_comment_block(void)
{
    int start, end;
    int row;
    int all_commented;
    const char *prefix;
    int plen;

    selected_line_range(&start, &end);
    prefix = comment_prefix();

    if (prefix == NULL) {
        set_status("This syntax has no line-comment rule");
        return;
    }

    plen = (int)strlen(prefix);
    all_commented = 1;

    for (row = start; row <= end; row++) {
        int pos;
        pos = line_comment_pos(lines[row]);

        if (strncmp(lines[row] + pos, prefix, plen) != 0) {
            all_commented = 0;
            break;
        }
    }

    push_undo();

    for (row = start; row <= end; row++) {
        char *line;
        int pos;
        int len;

        line = lines[row];
        pos = line_comment_pos(line);
        len = (int)strlen(line);

        if (all_commented) {
            if (!strncmp(line + pos, prefix, plen))
                memmove(line + pos, line + pos + plen,
                        len - pos - plen + 1);
        } else {
            char *p;

            if (len + plen >= MAX_LINE)
                continue;

            p = (char *)malloc(len + plen + 1);
            if (p == NULL)
                continue;

            memcpy(p, line, pos);
            memcpy(p + pos, prefix, plen);
            strcpy(p + pos + plen, line + pos);

            free(line);
            lines[row] = p;
        }
    }

    modified = 1;
    set_status(all_commented ? "Uncommented" : "Commented");
}

static int pair_for(int ch)
{
    switch (ch) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '"': return '"';
    case '\'': return '\'';
    }
    return 0;
}

static void insert_typed_char(int ch)
{
    int close;
    int len;

    len = (int)strlen(lines[cy]);

    if (autopairs &&
       (ch == ')' || ch == ']' || ch == '}' || ch == '"' || ch == '\'') &&
        cx < len && lines[cy][cx] == ch) {
        cx++;
        return;
    }

    close = autopairs ? pair_for(ch) : 0;

    if (close != 0) {
        push_undo();

        if (format_outdent_char(active_syntax(), ch) &&
            prefix_is_whitespace(lines[cy], cx))
            smart_outdent_for_brace();

        if (insert_char_at(cy, cx, ch) == 0) {
            if (insert_char_at(cy, cx + 1, close) == 0) {
                cx++;
                modified = 1;
                return;
            }
        }

        return;
    }

    insert_char(ch);
}


static void duplicate_line(void)
{
    int i;

    if (nlines >= MAX_LINES) {
        set_status("Maximum line count reached");
        return;
    }

    push_undo();

    for (i = nlines; i > cy + 1; i--)
        lines[i] = lines[i - 1];

    lines[cy + 1] = dupstr(lines[cy]);
    if (lines[cy + 1] == NULL) {
        for (i = cy + 1; i < nlines; i++)
            lines[i] = lines[i + 1];
        set_status("Out of memory");
        return;
    }

    nlines++;
    cy++;
    modified = 1;
    set_status("Line duplicated");
}

static void delete_current_line(void)
{
    int i;

    push_undo();

    if (nlines == 1) {
        free(lines[0]);
        lines[0] = dupstr("");
        cx = 0;
        modified = 1;
        set_status("Line cleared");
        return;
    }

    free(lines[cy]);

    for (i = cy; i < nlines - 1; i++)
        lines[i] = lines[i + 1];

    lines[nlines - 1] = NULL;
    nlines--;

    if (cy >= nlines)
        cy = nlines - 1;
    if (cx > (int)strlen(lines[cy]))
        cx = (int)strlen(lines[cy]);

    modified = 1;
    set_status("Line deleted");
}

static void move_current_line(int dir)
{
    char *tmp;
    int target;

    target = cy + dir;

    if (target < 0 || target >= nlines) {
        set_status("Cannot move line further");
        return;
    }

    push_undo();

    tmp = lines[cy];
    lines[cy] = lines[target];
    lines[target] = tmp;
    cy = target;

    modified = 1;
    set_status(dir < 0 ? "Line moved up" : "Line moved down");
}

static void join_with_next_line(void)
{
    char *joined;
    int len1;
    int len2;
    int i;

    if (cy >= nlines - 1) {
        set_status("No next line");
        return;
    }

    len1 = (int)strlen(lines[cy]);
    len2 = (int)strlen(lines[cy + 1]);

    if (len1 + len2 + 1 >= MAX_LINE) {
        set_status("Joined line would be too long");
        return;
    }

    push_undo();

    joined = (char *)malloc(len1 + len2 + 2);
    if (joined == NULL) {
        set_status("Out of memory");
        return;
    }

    strcpy(joined, lines[cy]);
    if (len1 > 0 && len2 > 0 &&
        !isspace((unsigned char)joined[len1 - 1]) &&
        !isspace((unsigned char)lines[cy + 1][0]))
        strcat(joined, " ");
    strcat(joined, lines[cy + 1]);

    free(lines[cy]);
    free(lines[cy + 1]);
    lines[cy] = joined;

    for (i = cy + 1; i < nlines - 1; i++)
        lines[i] = lines[i + 1];

    lines[nlines - 1] = NULL;
    nlines--;
    cx = len1;
    modified = 1;
    set_status("Lines joined");
}

static void trim_trailing_whitespace(void)
{
    int start;
    int end;
    int row;
    int changed;

    selected_line_range(&start, &end);
    push_undo();
    changed = 0;

    for (row = start; row <= end; row++) {
        int len;

        len = (int)strlen(lines[row]);

        while (len > 0 &&
              (lines[row][len - 1] == ' ' || lines[row][len - 1] == '\t')) {
            lines[row][--len] = '\0';
            changed = 1;
        }
    }

    if (changed) {
        modified = 1;
        set_status("Trailing whitespace removed");
    } else {
        set_status("No trailing whitespace");
    }
}

static void transform_case(int upper)
{
    int sy, sx, ey, ex;
    int row;

    normalize_selection(&sy, &sx, &ey, &ex);
    push_undo();

    for (row = sy; row <= ey; row++) {
        int start;
        int end;
        int i;

        start = (row == sy) ? sx : 0;
        end = (row == ey) ? ex : (int)strlen(lines[row]);

        if (!selecting) {
            start = 0;
            end = (int)strlen(lines[row]);
        }

        for (i = start; i < end; i++) {
            unsigned char ch;

            ch = (unsigned char)lines[row][i];
            lines[row][i] = (char)(upper ? toupper(ch) : tolower(ch));
        }
    }

    modified = 1;
    set_status(upper ? "Uppercase" : "Lowercase");
}

static void select_current_word(void)
{
    int len;
    int start;
    int end;

    len = (int)strlen(lines[cy]);
    start = cx;
    end = cx;

    if (start == len && start > 0)
        start--;

    while (start > 0 &&
          (isalnum((unsigned char)lines[cy][start - 1]) ||
           lines[cy][start - 1] == '_'))
        start--;

    end = cx;
    while (end < len &&
          (isalnum((unsigned char)lines[cy][end]) ||
           lines[cy][end] == '_'))
        end++;

    if (end <= start) {
        set_status("No word at cursor");
        return;
    }

    selecting = 1;
    sel_sy = cy;
    sel_sx = start;
    cx = end;
    set_status("Word selected");
}

static void select_current_line(void)
{
    selecting = 1;
    sel_sy = cy;
    sel_sx = 0;
    cx = (int)strlen(lines[cy]);
    set_status("Line selected");
}

static void select_all_text(void)
{
    selecting = 1;
    sel_sy = 0;
    sel_sx = 0;
    cy = nlines - 1;
    cx = (int)strlen(lines[cy]);
    set_status("All selected");
}

static void goto_matching_bracket(void)
{
    compute_bracket_match();

    if (bracket_match_row < 0) {
        set_status("No matching bracket");
        return;
    }

    cy = bracket_match_row;
    cx = bracket_match_col;
    scroll_screen();
    set_status("Matching bracket");
}

static void toggle_bookmark(void)
{
    bookmarks[curbuf][cy] = bookmarks[curbuf][cy] ? 0 : 1;
    set_status(bookmarks[curbuf][cy] ? "Bookmark set" : "Bookmark cleared");
}

static void next_bookmark(void)
{
    int i;

    for (i = cy + 1; i < nlines; i++) {
        if (bookmarks[curbuf][i]) {
            cy = i;
            cx = 0;
            scroll_screen();
            set_status("Next bookmark");
            return;
        }
    }

    for (i = 0; i <= cy; i++) {
        if (bookmarks[curbuf][i]) {
            cy = i;
            cx = 0;
            scroll_screen();
            set_status("Next bookmark");
            return;
        }
    }

    set_status("No bookmarks");
}

static void scroll_screen(void)
{
    int gutter;
    int textrows;
    int textcols;

    gutter = line_numbers ? 6 : 0;
    textrows = LINES - 3;
    textcols = COLS - gutter - 1;

    if (textrows < 1)
        textrows = 1;
    if (textcols < 1)
        textcols = 1;

    if (cy < rowoff)
        rowoff = cy;

    if (cy >= rowoff + textrows)
        rowoff = cy - textrows + 1;

    if (cx < coloff)
        coloff = cx;

    if (cx >= coloff + textcols)
        coloff = cx - textcols + 1;

    if (rowoff < 0)
        rowoff = 0;
    if (coloff < 0)
        coloff = 0;
}

static int bracket_match_row = -1;
static int bracket_match_col = -1;

static int matching_bracket(char c)
{
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    }
    return 0;
}

static int bracket_dir(char c)
{
    if (c == '(' || c == '[' || c == '{')
        return 1;
    if (c == ')' || c == ']' || c == '}')
        return -1;
    return 0;
}

static void compute_bracket_match(void)
{
    char c;
    int target;
    int dir;
    int depth;
    int r, cidx;

    bracket_match_row = -1;
    bracket_match_col = -1;

    if (cy < 0 || cy >= nlines)
        return;

    if (cx < (int)strlen(lines[cy]))
        c = lines[cy][cx];
    else if (cx > 0)
        c = lines[cy][cx - 1];
    else
        return;

    target = matching_bracket(c);
    dir = bracket_dir(c);

    if (!target || !dir)
        return;

    depth = 0;

    if (dir > 0) {
        for (r = cy; r < nlines; r++) {
            int start = (r == cy ? cx + 1 : 0);
            for (cidx = start; cidx < (int)strlen(lines[r]); cidx++) {
                char x = lines[r][cidx];
                if (x == c) depth++;
                else if (x == target) {
                    if (depth == 0) {
                        bracket_match_row = r;
                        bracket_match_col = cidx;
                        return;
                    }
                    depth--;
                }
            }
        }
    } else {
        for (r = cy; r >= 0; r--) {
            int start = (r == cy ? cx - 1 : (int)strlen(lines[r]) - 1);
            for (cidx = start; cidx >= 0; cidx--) {
                char x = lines[r][cidx];
                if (x == c) depth++;
                else if (x == target) {
                    if (depth == 0) {
                        bracket_match_row = r;
                        bracket_match_col = cidx;
                        return;
                    }
                    depth--;
                }
            }
        }
    }
}

static void draw_char_with_attrs(int y, int x, int row, int col, int ch, int pair)
{
    int selected;
    int matched;

    selected = pos_selected(row, col);
    matched = (row == bracket_match_row && col == bracket_match_col);

    if (use_color) {
        if (selected)
            attron(COLOR_PAIR(CP_SELECT));
        else if (matched)
            attron(COLOR_PAIR(CP_MATCH));
        else
            attron(COLOR_PAIR(pair));
    } else {
        if (selected)
            attron(A_REVERSE);
        else if (matched)
            attron(A_BOLD | A_REVERSE);
    }

    mvaddch(y, x, ch);

    if (use_color) {
        if (selected)
            attroff(COLOR_PAIR(CP_SELECT));
        else if (matched)
            attroff(COLOR_PAIR(CP_MATCH));
        else
            attroff(COLOR_PAIR(pair));
    } else {
        if (selected)
            attroff(A_REVERSE);
        else if (matched)
            attroff(A_BOLD | A_REVERSE);
    }
}


static void draw_plain_line(int y, int row, const char *s, int gutter)
{
    int len;
    int j;
    int start;
    int end;

    len = (int)strlen(s);
    start = coloff;
    end = coloff + (COLS - gutter - 1);

    if (start > len)
        return;
    if (end > len)
        end = len;

    for (j = start; j < end; j++)
        draw_char_with_attrs(y, gutter + (j - start), row, j, s[j], CP_NORMAL);
}


static const char *ansi_sgr_for_pair(int pair)
{
    switch (pair) {
    case CP_KEYWORD: return "36";   /* cyan */
    case CP_STRING:  return "33";   /* yellow */
    case CP_COMMENT: return "32";   /* green */
    case CP_NUMBER:  return "35";   /* magenta */
    case CP_PREPROC: return "34";   /* blue */
    default:         return NULL;
    }
}

/*
 * IRIX curses may report no colour support even when the terminal's
 * terminfo entry says colours=8.  In that case, draw the normal curses
 * screen first and then overlay syntax-coloured token spans using ANSI
 * SGR + absolute cursor positioning.
 */
static void ansi_overlay_span(int screen_y, int screen_x,
                              const char *s, int len, int pair)
{
    const char *code;

    if (len <= 0)
        return;

    code = ansi_sgr_for_pair(pair);
    if (code == NULL)
        return;

    printf("\033[%d;%dH\033[%sm", screen_y + 1, screen_x + 1, code);
    fwrite(s, 1, len, stdout);
    printf("\033[0m");
}

typedef struct {
    int screen_y;
    int row;
    int gutter;
    int visible_start;
    int visible_end;
    const char *line;
} OverlayContext;

static int style_to_pair(int style)
{
    switch (style) {
    case SYNTAX_STYLE_KEYWORD: return CP_KEYWORD;
    case SYNTAX_STYLE_STRING:  return CP_STRING;
    case SYNTAX_STYLE_COMMENT: return CP_COMMENT;
    case SYNTAX_STYLE_NUMBER:  return CP_NUMBER;
    case SYNTAX_STYLE_PREPROC: return CP_PREPROC;
    default:                   return CP_NORMAL;
    }
}

static void syntax_overlay_callback(int start, int len, int style, void *user)
{
    OverlayContext *ctx;
    int ds;
    int de;
    int end;
    int j;
    int run_start;
    int pair;

    ctx = (OverlayContext *)user;
    end = start + len;
    ds = start < ctx->visible_start ? ctx->visible_start : start;
    de = end > ctx->visible_end ? ctx->visible_end : end;

    if (de <= ds)
        return;

    pair = style_to_pair(style);
    if (pair == CP_NORMAL)
        return;

    j = ds;
    while (j < de) {
        while (j < de && pos_selected(ctx->row, j))
            j++;
        run_start = j;
        while (j < de && !pos_selected(ctx->row, j))
            j++;

        if (j > run_start) {
            ansi_overlay_span(ctx->screen_y,
                              ctx->gutter + (run_start - ctx->visible_start),
                              ctx->line + run_start,
                              j - run_start,
                              pair);
        }
    }
}

static void ansi_overlay_syntax(void)
{
    const SyntaxDef *def;
    SyntaxState state;
    int y;
    int filerow;
    int textrows;
    int gutter;
    int row;

    if (!syntax_enabled)
        return;

    def = active_syntax();
    if (def == NULL)
        return;

    gutter = line_numbers ? 6 : 0;
    textrows = LINES - 3;
    if (textrows < 1)
        textrows = 1;

    syntax_state_reset(&state);

    /* Reconstruct multiline-comment state above the visible window. */
    for (row = 0; row < rowoff && row < nlines; row++)
        syntax_highlight_line(def, lines[row], &state, NULL, NULL);

    for (y = 0; y < textrows; y++) {
        OverlayContext ctx;

        filerow = rowoff + y;
        if (filerow >= nlines)
            break;

        ctx.screen_y = y + 1;
        ctx.row = filerow;
        ctx.gutter = gutter;
        ctx.visible_start = coloff;
        ctx.visible_end = coloff + (COLS - gutter - 1);
        ctx.line = lines[filerow];

        syntax_highlight_line(def, lines[filerow], &state,
                              syntax_overlay_callback, &ctx);
    }

    printf("\033[0m");
    printf("\033[%d;%dH",
           (cy - rowoff) + 2,
           gutter + cx - coloff + 1);
    fflush(stdout);
}

static void draw_screen(void)
{
    int y;
    int filerow;
    int textrows;
    int gutter;
    char buf[STATUS_LEN];
    int name_room;
    scroll_screen();
    compute_bracket_match();
    erase();

    if (use_color)
        attron(COLOR_PAIR(CP_MENU));
    else
        attron(A_REVERSE);

    mvaddstr(0, 0, " File  Edit  Search  Options  Help ");
    {
        int bx;
        int bi;

        bx = 35;

        for (bi = 0; bi < buffer_count && bx < COLS - 4; bi++) {
            const char *bn;
            const char *slash;
            char tab[64];

            bn = buffers[bi].fname[0] ? buffers[bi].fname : "[No Name]";
            slash = strrchr(bn, '/');
            if (slash != NULL)
                bn = slash + 1;

            sprintf(tab, "%c%d:%.*s%s%c",
                    bi == curbuf ? '[' : ' ',
                    bi + 1,
                    18,
                    bn,
                    buffers[bi].dirty ? "*" : "",
                    bi == curbuf ? ']' : ' ');

            if (bx + (int)strlen(tab) < COLS - 1)
                mvaddstr(0, bx, tab);

            bx += (int)strlen(tab) + 1;
        }
    }
    clrtoeol();

    if (use_color)
        attroff(COLOR_PAIR(CP_MENU));
    else
        attroff(A_REVERSE);

    gutter = line_numbers ? 6 : 0;
    textrows = LINES - 3;
    if (textrows < 1)
        textrows = 1;

    for (y = 0; y < textrows; y++) {
        filerow = rowoff + y;

        if (line_numbers) {
            if (use_color)
                attron(COLOR_PAIR(CP_LINENO));
            else
                attron(A_BOLD);

            if (filerow < nlines)
                mvprintw(y + 1, 0, "%5d ", filerow + 1);
            else
                mvaddstr(y + 1, 0, "    ~ ");

            if (use_color)
                attroff(COLOR_PAIR(CP_LINENO));
            else
                attroff(A_BOLD);
        }

        if (filerow >= nlines)
            continue;

        draw_plain_line(y + 1, filerow, lines[filerow], gutter);
    }

    if (use_color)
        attron(COLOR_PAIR(CP_STATUS));
    else
        attron(A_REVERSE);

    name_room = COLS - 48;
    if (name_room < 10)
        name_room = 10;

    sprintf(buf, " %-*.*s %s %s %s Ln %d/%d Col %d TAB:%d ",
            name_room, name_room,
            filename[0] ? filename : "[No Name]",
            modified ? "[+]" : "",
            selecting ? "[SEL]" : "",
            syntax_enabled ? syntax_name() : "TEXT",
            cy + 1, nlines, cx + 1, tab_width);

    mvaddnstr(LINES - 2, 0, buf, COLS);
    clrtoeol();

    if (use_color)
        attroff(COLOR_PAIR(CP_STATUS));
    else
        attroff(A_REVERSE);

    mvaddstr(LINES - 1, 0,
             "^T Menu ^S Save ^F Find ^H Repl ^B Sel ^Z Undo ^Q Quit");

    if (statusmsg[0] != '\0') {
        int pos;
        pos = COLS - (int)strlen(statusmsg) - 1;
        if (pos > 0)
            mvaddnstr(LINES - 1, pos, statusmsg, COLS - pos);
    }

    move((cy - rowoff) + 1,
         gutter + cx - coloff);

    refresh();

    ansi_overlay_syntax();
}

static int prompt_input(const char *prompt, char *out, int outlen)
{
    int ch;
    int len;

    out[0] = '\0';
    len = 0;

    for (;;) {
        move(LINES - 1, 0);
        clrtoeol();
        mvaddstr(LINES - 1, 0, (char *)prompt);
        addnstr(out, COLS - (int)strlen(prompt) - 1);
        refresh();

        ch = getch();

        if (ch == 27) {
            out[0] = '\0';
            return 0;
        }

        if (ch == '\n' || ch == '\r') {
            out[len] = '\0';
            return 1;
        }

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                out[len] = '\0';
            }
        } else if (ch >= 32 && ch <= 126 && len < outlen - 1) {
            out[len++] = (char)ch;
            out[len] = '\0';
        }
    }
}

static void do_save_as(void)
{
    char name[NAME_LEN];

    if (!prompt_input("Save as: ", name, sizeof(name)) || !name[0]) {
        set_status("Save As cancelled");
        return;
    }

    save_file_as(name);
}

static void do_save(void)
{
    if (!filename[0])
        do_save_as();
    else
        save_file_as(filename);
}


typedef struct {
    char name[NAME_LEN];
    int isdir;
    long size;
    unsigned long mode;
} BrowserEntry;

static int browser_cmp(const void *a, const void *b)
{
    const BrowserEntry *ea;
    const BrowserEntry *eb;

    ea = (const BrowserEntry *)a;
    eb = (const BrowserEntry *)b;

    if (ea->isdir != eb->isdir)
        return eb->isdir - ea->isdir;

    return strcmp(ea->name, eb->name);
}

static void path_parent(char *path)
{
    int len;
    char *p;

    len = (int)strlen(path);

    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }

    p = strrchr(path, '/');

    if (p == NULL) {
        strcpy(path, ".");
    } else if (p == path) {
        path[1] = '\0';
    } else {
        *p = '\0';
    }
}

static void path_join(char *out, const char *dir, const char *name)
{
    if (!strcmp(dir, "/"))
        sprintf(out, "/%s", name);
    else
        sprintf(out, "%s/%s", dir, name);
}

static int browser_load(const char *path, BrowserEntry *entries, int max_entries)
{
    DIR *dp;
    struct dirent *de;
    int count;

    dp = opendir(path);
    if (dp == NULL)
        return -1;

    count = 0;

    while ((de = readdir(dp)) != NULL && count < max_entries) {
        char full[PATH_LEN];
        struct stat st;

        if (!strcmp(de->d_name, "."))
            continue;

        strncpy(entries[count].name, de->d_name, NAME_LEN - 1);
        entries[count].name[NAME_LEN - 1] = '\0';

        path_join(full, path, de->d_name);

        if (stat(full, &st) == 0) {
            entries[count].isdir = S_ISDIR(st.st_mode) ? 1 : 0;
            entries[count].size = (long)st.st_size;
            entries[count].mode = (unsigned long)st.st_mode;
        } else {
            entries[count].isdir = 0;
            entries[count].size = 0;
            entries[count].mode = 0;
        }

        count++;
    }

    closedir(dp);

    qsort(entries, count, sizeof(BrowserEntry), browser_cmp);
    return count;
}


static void draw_hline_ascii(int y, int x, int width)
{
    int i;

    if (width < 2)
        return;

    mvaddch(y, x, '+');

    for (i = 1; i < width - 1; i++)
        addch('-');

    addch('+');
}

static void draw_box_ascii(int y, int x, int height, int width)
{
    int i;

    if (height < 2 || width < 2)
        return;

    draw_hline_ascii(y, x, width);

    for (i = 1; i < height - 1; i++) {
        mvaddch(y + i, x, '|');
        mvaddch(y + i, x + width - 1, '|');
    }

    draw_hline_ascii(y + height - 1, x, width);
}

static void human_size(long bytes, char *out, int outlen)
{
    if (bytes < 1024L)
        sprintf(out, "%ld B", bytes);
    else if (bytes < 1024L * 1024L)
        sprintf(out, "%ld KB", bytes / 1024L);
    else
        sprintf(out, "%ld MB", bytes / (1024L * 1024L));

    out[outlen - 1] = '\0';
}

static void browser_preview_text(const char *path,
                                 int y, int x, int height, int width)
{
    FILE *fp;
    char buf[256];
    int row;

    fp = fopen(path, "r");

    if (fp == NULL) {
        mvaddnstr(y, x, "(preview unavailable)", width);
        return;
    }

    row = 0;

    while (row < height && fgets(buf, sizeof(buf), fp) != NULL) {
        int i;

        for (i = 0; buf[i] != '\0'; i++) {
            unsigned char c;

            c = (unsigned char)buf[i];

            if (c == '\n' || c == '\r') {
                buf[i] = '\0';
                break;
            }

            if (c < 32 && c != '\t') {
                strcpy(buf, "(binary/non-text file)");
                row = height - 1;
                break;
            }
        }

        mvaddnstr(y + row, x, buf, width);
        row++;
    }

    fclose(fp);
}

static int file_browser(char *out, int outlen)
{
    BrowserEntry entries[BROWSER_MAX];
    char path[PATH_LEN];
    char full[PATH_LEN];
    int count;
    int selected;
    int top;
    int ch;

    if (getcwd(path, sizeof(path)) == NULL)
        strcpy(path, ".");

    selected = 0;
    top = 0;

    for (;;) {
        int i;
        int body_y;
        int body_h;
        int left_x;
        int left_w;
        int right_x;
        int right_w;
        int list_rows;

        count = browser_load(path, entries, BROWSER_MAX);

        if (count < 0) {
            set_status("Cannot open directory");
            return 0;
        }

        if (selected >= count)
            selected = count > 0 ? count - 1 : 0;

        body_y = 3;
        body_h = LINES - 6;

        if (body_h < 8)
            body_h = 8;

        left_x = 1;
        left_w = (COLS * 3) / 5;

        if (left_w < 28)
            left_w = COLS - 2;

        right_x = left_x + left_w;
        right_w = COLS - right_x - 1;

        if (right_w < 22) {
            right_w = 0;
            left_w = COLS - 2;
        }

        list_rows = body_h - 2;

        if (selected < top)
            top = selected;

        if (selected >= top + list_rows)
            top = selected - list_rows + 1;

        if (top < 0)
            top = 0;

        erase();

        attron(A_REVERSE);
        mvaddstr(0, 0, " TEDIT Open File ");
        clrtoeol();
        attroff(A_REVERSE);

        mvaddstr(1, 1, "Path: ");
        mvaddnstr(1, 7, path, COLS - 8);

        draw_box_ascii(body_y, left_x, body_h, left_w);

        if (right_w > 0)
            draw_box_ascii(body_y, right_x, body_h, right_w);

        mvaddstr(body_y, left_x + 2, " Files ");

        if (right_w > 0)
            mvaddstr(body_y, right_x + 2, " Info / Preview ");

        for (i = 0; i < list_rows; i++) {
            int idx;
            char display[NAME_LEN + 32];
            char sizebuf[32];
            int avail;

            idx = top + i;

            if (idx >= count)
                break;

            if (entries[idx].isdir) {
                sprintf(display, "/ %-*.*s",
                        left_w - 6, left_w - 6,
                        entries[idx].name);
            } else {
                human_size(entries[idx].size, sizebuf, sizeof(sizebuf));
                avail = left_w - 16;

                if (avail < 6)
                    avail = 6;

                sprintf(display, "  %-*.*s %8s",
                        avail, avail,
                        entries[idx].name,
                        sizebuf);
            }

            if (idx == selected)
                attron(A_REVERSE);

            mvaddnstr(body_y + 1 + i,
                      left_x + 1,
                      display,
                      left_w - 2);

            if (idx == selected)
                attroff(A_REVERSE);
        }

        if (right_w > 0 && count > 0) {
            int info_y;
            char sizebuf[32];

            path_join(full, path, entries[selected].name);
            info_y = body_y + 2;

            mvaddstr(info_y, right_x + 2, "Name:");
            mvaddnstr(info_y + 1,
                      right_x + 2,
                      entries[selected].name,
                      right_w - 4);

            mvaddstr(info_y + 3, right_x + 2, "Type:");
            mvaddstr(info_y + 4,
                     right_x + 2,
                     entries[selected].isdir ? "Directory" : "File");

            if (!entries[selected].isdir) {
                human_size(entries[selected].size, sizebuf, sizeof(sizebuf));

                mvaddstr(info_y + 6, right_x + 2, "Size:");
                mvaddstr(info_y + 7, right_x + 2, sizebuf);

                if (body_h > 15) {
                    mvaddstr(info_y + 9, right_x + 2, "Preview:");
                    browser_preview_text(full,
                                         info_y + 10,
                                         right_x + 2,
                                         body_h - 13,
                                         right_w - 4);
                }
            } else {
                mvaddstr(info_y + 6,
                         right_x + 2,
                         "Enter to browse");
            }
        }

        attron(A_REVERSE);
        mvaddstr(LINES - 2, 0,
                 " Up/Down Move  Enter Open/Browse  Backspace Parent  Esc Cancel ");
        clrtoeol();
        attroff(A_REVERSE);

        mvaddstr(LINES - 1, 1,
                 "Directories are shown first. File sizes appear in the left panel.");

        refresh();
        ch = getch();

        if (ch == 27)
            return 0;

        if (ch == KEY_UP) {
            if (selected > 0)
                selected--;
        } else if (ch == KEY_DOWN) {
            if (selected + 1 < count)
                selected++;
#ifdef KEY_PPAGE
        } else if (ch == KEY_PPAGE) {
            selected -= list_rows;

            if (selected < 0)
                selected = 0;
#endif
#ifdef KEY_NPAGE
        } else if (ch == KEY_NPAGE) {
            selected += list_rows;

            if (selected >= count)
                selected = count > 0 ? count - 1 : 0;
#endif
#ifdef KEY_HOME
        } else if (ch == KEY_HOME) {
            selected = 0;
#endif
#ifdef KEY_END
        } else if (ch == KEY_END) {
            selected = count > 0 ? count - 1 : 0;
#endif
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            path_parent(path);
            selected = 0;
            top = 0;
        } else if ((ch == '\n' || ch == '\r') && count > 0) {
            path_join(full, path, entries[selected].name);

            if (entries[selected].isdir) {
                strncpy(path, full, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
                selected = 0;
                top = 0;
            } else {
                strncpy(out, full, outlen - 1);
                out[outlen - 1] = '\0';
                return 1;
            }
        }
    }
}

static void do_open(void)
{
    char name[PATH_LEN];

    if (modified) {
        set_status("Unsaved changes: save first");
        return;
    }

    if (!file_browser(name, sizeof(name))) {
        set_status("Open cancelled");
        return;
    }

    load_file(name);
}

static int find_from_position(const char *needle, int start_row, int start_col)
{
    int row;
    char *p;

    if (!needle[0])
        return 0;

    for (row = start_row; row < nlines; row++) {
        p = strstr(lines[row] + (row == start_row ? start_col : 0), needle);

        if (p != NULL) {
            cy = row;
            cx = (int)(p - lines[row]);
            scroll_screen();
            return 1;
        }
    }

    for (row = 0; row <= start_row && row < nlines; row++) {
        p = strstr(lines[row], needle);

        if (p != NULL &&
           (row < start_row || (int)(p - lines[row]) < start_col)) {
            cy = row;
            cx = (int)(p - lines[row]);
            scroll_screen();
            return 1;
        }
    }

    return 0;
}

static void do_find(void)
{
    char query[SEARCH_LEN];

    if (!prompt_input("Find: ", query, sizeof(query))) {
        set_status("Find cancelled");
        return;
    }

    if (!query[0])
        return;

    strncpy(last_search, query, SEARCH_LEN - 1);
    last_search[SEARCH_LEN - 1] = '\0';

    if (find_from_position(last_search, cy, cx + 1))
        sprintf(statusmsg, "Found: %s", last_search);
    else
        sprintf(statusmsg, "Not found: %s", last_search);
}

static void do_replace(void)
{
    char findbuf[SEARCH_LEN];
    char replbuf[SEARCH_LEN];
    char mode[16];

    if (!prompt_input("Find: ", findbuf, sizeof(findbuf)) || !findbuf[0]) {
        set_status("Replace cancelled");
        return;
    }

    if (!prompt_input("Replace with: ", replbuf, sizeof(replbuf))) {
        set_status("Replace cancelled");
        return;
    }

    if (!prompt_input("Mode [1=next, a=all]: ", mode, sizeof(mode))) {
        set_status("Replace cancelled");
        return;
    }

    push_undo();

    if (mode[0] == 'a' || mode[0] == 'A') {
        int row;
        int count;

        count = 0;

        for (row = 0; row < nlines; row++) {
            char *p;

            while ((p = strstr(lines[row], findbuf)) != NULL) {
                int prelen;
                int newlen;
                char *newp;

                prelen = (int)(p - lines[row]);
                newlen = (int)strlen(lines[row])
                       - (int)strlen(findbuf)
                       + (int)strlen(replbuf);

                if (newlen >= MAX_LINE - 1)
                    break;

                newp = (char *)malloc(newlen + 1);
                if (newp == NULL)
                    break;

                memcpy(newp, lines[row], prelen);
                strcpy(newp + prelen, replbuf);
                strcpy(newp + prelen + strlen(replbuf),
                       p + strlen(findbuf));

                free(lines[row]);
                lines[row] = newp;
                count++;
            }
        }

        modified = 1;
        sprintf(statusmsg, "Replaced %d occurrence(s)", count);
    } else {
        if (find_from_position(findbuf, cy, cx)) {
            char *line;
            int oldlen, flen, rlen;
            char *newp;

            line = lines[cy];
            oldlen = (int)strlen(line);
            flen = (int)strlen(findbuf);
            rlen = (int)strlen(replbuf);

            if (oldlen - flen + rlen < MAX_LINE - 1) {
                newp = (char *)malloc(oldlen - flen + rlen + 1);
                if (newp != NULL) {
                    memcpy(newp, line, cx);
                    memcpy(newp + cx, replbuf, rlen);
                    strcpy(newp + cx + rlen, line + cx + flen);

                    free(line);
                    lines[cy] = newp;
                    modified = 1;
                    set_status("Replaced next occurrence");
                }
            }
        } else {
            set_status("Text not found");
        }
    }
}

static void do_goto(void)
{
    char buf[32];
    int line;

    if (!prompt_input("Goto line: ", buf, sizeof(buf)) || !buf[0]) {
        set_status("Goto cancelled");
        return;
    }

    line = atoi(buf);

    if (line < 1)
        line = 1;
    if (line > nlines)
        line = nlines;

    cy = line - 1;

    if (cx > (int)strlen(lines[cy]))
        cx = (int)strlen(lines[cy]);

    scroll_screen();
    sprintf(statusmsg, "Line %d", line);
}

static void help_screen(void)
{
    erase();

    mvaddstr(1, 2, "TEDIT v6 - modular curses editor");
    mvaddstr(3, 2, "^T Menu   ^N New   ^P Prev buffer   ^E Next buffer");
    mvaddstr(4, 2, "^O Open   ^S Save  ^A Save As       ^Q Quit");
    mvaddstr(5, 2, "^F Find   ^H Replace   ^L Goto");
    mvaddstr(6, 2, "^Z Undo   ^R Redo");
    mvaddstr(7, 2, "^B Select ^C Copy      ^X Cut       ^V Paste");
    mvaddstr(8, 2, "^U Prev word           ^D Next word");
    mvaddstr(10,2, "Edit menu: indent/unindent block and comment toggle");
    mvaddstr(11,2, "Syntax: C/H, Fortran, Ada, JSON, Rust, Go, JS/TS + existing modes");
    mvaddstr(13,2, "Tab/Enter/} use smart C-style indentation.");
    mvaddstr(15,2, "Press any key.");

    refresh();
    getch();
}

static void about_screen(void)
{
    erase();

    mvaddstr(2, 4, "TEDIT v7.0-dev");
    mvaddstr(4, 4, "Portable curses code editor for classic UNIX.");
    mvaddstr(5, 4, "Designed to compile on IRIX using plain curses.");
    mvaddstr(7, 4, "Press any key.");

    refresh();
    getch();
}


static int unsaved_buffer_count(void)
{
    int i;
    int count;

    count = 0;

    for (i = 0; i < buffer_count; i++) {
        if (buffers[i].dirty)
            count++;
    }

    return count;
}

static int confirm_yes_no(const char *message)
{
    int ch;

    for (;;) {
        move(LINES - 1, 0);
        clrtoeol();
        mvaddnstr(LINES - 1, 0, (char *)message, COLS - 1);
        refresh();

        ch = getch();

        if (ch == 'y' || ch == 'Y')
            return 1;

        if (ch == 'n' || ch == 'N' ||
            ch == 27 || ch == '\n' || ch == '\r')
            return 0;
    }
}

static void quit_editor(void)
{
    int dirty;

    dirty = unsaved_buffer_count();

    if (dirty > 0) {
        char msg[STATUS_LEN];

        if (dirty == 1)
            sprintf(msg, "1 buffer has unsaved changes. Quit without saving? (y/N)");
        else
            sprintf(msg, "%d buffers have unsaved changes. Quit without saving? (y/N)",
                    dirty);

        if (!confirm_yes_no(msg)) {
            set_status("Quit cancelled");
            draw_screen();
            return;
        }
    }

    erase();
    refresh();
    endwin();

    printf("\033[2J\033[H");
    fflush(stdout);

    {
        int bi;
        for (bi = 0; bi < buffer_count; bi++)
            free_buffer_index(bi);
    }

    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);

    if (clipboard != NULL)
        free(clipboard);

    exit(0);
}

static void execute_action(int action)
{
    switch (action) {
    case ACT_NEW_BUFFER:
        create_new_buffer();
        break;
    case ACT_PREV_BUFFER:
        switch_buffer(-1);
        break;
    case ACT_NEXT_BUFFER:
        switch_buffer(1);
        break;
    case ACT_OPEN: do_open(); break;
    case ACT_SAVE: do_save(); break;
    case ACT_SAVE_AS: do_save_as(); break;
    case ACT_CLOSE_BUFFER: close_current_buffer(); break;
    case ACT_QUIT: quit_editor(); break;
    case ACT_UNDO: do_undo(); break;
    case ACT_REDO: do_redo(); break;
    case ACT_SELECT:
        selecting = !selecting;
        if (selecting) {
            sel_sy = cy; sel_sx = cx;
            set_status("Selection started");
        } else {
            set_status("Selection cleared");
        }
        break;
    case ACT_COPY: copy_selection_or_line(); break;
    case ACT_CUT: cut_selection_or_line(); break;
    case ACT_PASTE: paste_text(); break;
    case ACT_WORD_PREV: move_word_left(); break;
    case ACT_WORD_NEXT: move_word_right(); break;
    case ACT_INDENT: indent_block(); break;
    case ACT_UNINDENT: unindent_block(); break;
    case ACT_COMMENT: toggle_comment_block(); break;
    case ACT_DUP_LINE: duplicate_line(); break;
    case ACT_DELETE_LINE: delete_current_line(); break;
    case ACT_MOVE_LINE_UP: move_current_line(-1); break;
    case ACT_MOVE_LINE_DOWN: move_current_line(1); break;
    case ACT_JOIN_LINE: join_with_next_line(); break;
    case ACT_TRIM_WS: trim_trailing_whitespace(); break;
    case ACT_UPPERCASE: transform_case(1); break;
    case ACT_LOWERCASE: transform_case(0); break;
    case ACT_SELECT_WORD: select_current_word(); break;
    case ACT_SELECT_LINE: select_current_line(); break;
    case ACT_SELECT_ALL: select_all_text(); break;
    case ACT_MATCH_BRACKET: goto_matching_bracket(); break;
    case ACT_BOOKMARK_TOGGLE: toggle_bookmark(); break;
    case ACT_BOOKMARK_NEXT: next_bookmark(); break;
    case ACT_FIND: do_find(); break;
    case ACT_REPLACE: do_replace(); break;
    case ACT_GOTO: do_goto(); break;
    case ACT_TOGGLE_SYNTAX:
        syntax_enabled = !syntax_enabled;
        set_status(syntax_enabled ? "Syntax highlighting on" :
                                    "Syntax highlighting off");
        break;
    case ACT_TOGGLE_LINES:
        line_numbers = !line_numbers;
        set_status(line_numbers ? "Line numbers on" : "Line numbers off");
        break;
    case ACT_TOGGLE_AUTOPAIRS:
        autopairs = !autopairs;
        set_status(autopairs ? "Auto-pairs on" : "Auto-pairs off");
        break;
    case ACT_TAB2:
        tab_width = 2; set_status("Tab width 2"); break;
    case ACT_TAB4:
        tab_width = 4; set_status("Tab width 4"); break;
    case ACT_TAB8:
        tab_width = 8; set_status("Tab width 8"); break;
    case ACT_HELP: help_screen(); break;
    case ACT_ABOUT: about_screen(); break;
    default: break;
    }
}

static const MenuItem *get_menu(int menu_index, int *count)
{
    switch (menu_index) {
    case 0:
        *count = sizeof(file_menu) / sizeof(file_menu[0]);
        return file_menu;
    case 1:
        *count = sizeof(edit_menu) / sizeof(edit_menu[0]);
        return edit_menu;
    case 2:
        *count = sizeof(search_menu) / sizeof(search_menu[0]);
        return search_menu;
    case 3:
        *count = sizeof(options_menu) / sizeof(options_menu[0]);
        return options_menu;
    case 4:
        *count = sizeof(help_menu) / sizeof(help_menu[0]);
        return help_menu;
    }

    *count = 0;
    return NULL;
}

static int menu_x_position(int menu_index)
{
    int i;
    int x;

    x = 1;

    for (i = 0; i < menu_index; i++)
        x += (int)strlen(menu_names[i]) + 2;

    return x;
}

static int first_selectable(const MenuItem *items, int count)
{
    int i;
    for (i = 0; i < count; i++)
        if (items[i].action != ACT_NONE)
            return i;
    return 0;
}

static int next_selectable(const MenuItem *items, int count, int current, int dir)
{
    int i;

    i = current;

    for (;;) {
        i += dir;

        if (i < 0)
            i = count - 1;
        if (i >= count)
            i = 0;

        if (items[i].action != ACT_NONE)
            return i;

        if (i == current)
            return current;
    }
}

static void draw_menu_bar_selected(int selected_menu)
{
    int i;
    int x;

    move(0, 0);
    clrtoeol();

    x = 1;

    for (i = 0; i < 5; i++) {
        if (i == selected_menu) {
            if (use_color)
                attron(COLOR_PAIR(CP_MENU_SEL));
            else
                attron(A_REVERSE | A_BOLD);
        } else {
            if (use_color)
                attron(COLOR_PAIR(CP_MENU));
            else
                attron(A_REVERSE);
        }

        mvprintw(0, x, " %s ", menu_names[i]);

        if (use_color) {
            if (i == selected_menu)
                attroff(COLOR_PAIR(CP_MENU_SEL));
            else
                attroff(COLOR_PAIR(CP_MENU));
        } else {
            if (i == selected_menu)
                attroff(A_REVERSE | A_BOLD);
            else
                attroff(A_REVERSE);
        }

        x += (int)strlen(menu_names[i]) + 2;
    }
}

static void draw_dropdown(int menu_index, int selected_item)
{
    const MenuItem *items;
    int count;
    int i;
    int width;
    int x;
    int y;

    items = get_menu(menu_index, &count);
    if (items == NULL)
        return;

    width = 0;

    for (i = 0; i < count; i++) {
        int len;
        len = (int)strlen(items[i].label);
        if (len > width)
            width = len;
    }

    width += 2;
    x = menu_x_position(menu_index);
    y = 1;

    for (i = 0; i < count; i++) {
        int j;

        move(y + i, x);

        if (i == selected_item && items[i].action != ACT_NONE) {
            if (use_color)
                attron(COLOR_PAIR(CP_MENU_SEL));
            else
                attron(A_REVERSE);
        } else {
            if (use_color)
                attron(COLOR_PAIR(CP_MENU));
            else
                attron(A_NORMAL);
        }

        addch(' ');

        for (j = 0; j < width - 2; j++) {
            if (j < (int)strlen(items[i].label))
                addch(items[i].label[j]);
            else
                addch(' ');
        }

        addch(' ');

        if (i == selected_item && items[i].action != ACT_NONE) {
            if (use_color)
                attroff(COLOR_PAIR(CP_MENU_SEL));
            else
                attroff(A_REVERSE);
        } else {
            if (use_color)
                attroff(COLOR_PAIR(CP_MENU));
            else
                attroff(A_NORMAL);
        }
    }
}


static int shortcut_action(int ch)
{
    switch (ch) {
    case CTRL_KEY('n'): return ACT_NEW_BUFFER;
    case CTRL_KEY('p'): return ACT_PREV_BUFFER;
    case CTRL_KEY('e'): return ACT_NEXT_BUFFER;
    case CTRL_KEY('o'): return ACT_OPEN;
    case CTRL_KEY('s'): return ACT_SAVE;
    case CTRL_KEY('a'): return ACT_SAVE_AS;
    case CTRL_KEY('q'): return ACT_QUIT;
    case CTRL_KEY('z'): return ACT_UNDO;
    case CTRL_KEY('r'): return ACT_REDO;
    case CTRL_KEY('b'): return ACT_SELECT;
    case CTRL_KEY('c'): return ACT_COPY;
    case CTRL_KEY('x'): return ACT_CUT;
    case CTRL_KEY('v'): return ACT_PASTE;
    case CTRL_KEY('u'): return ACT_WORD_PREV;
    case CTRL_KEY('d'): return ACT_WORD_NEXT;
    case CTRL_KEY('f'): return ACT_FIND;
    case CTRL_KEY('h'): return ACT_REPLACE;
    case CTRL_KEY('l'): return ACT_GOTO;
    }

    return ACT_NONE;
}

static void activate_menu(void)
{
    int menu_index;
    int item_index;
    int ch;
    int count;
    const MenuItem *items;

    menu_index = 0;
    items = get_menu(menu_index, &count);
    item_index = first_selectable(items, count);

    for (;;) {
        draw_screen();
        draw_menu_bar_selected(menu_index);

        items = get_menu(menu_index, &count);

        if (item_index >= count || items[item_index].action == ACT_NONE)
            item_index = first_selectable(items, count);

        draw_dropdown(menu_index, item_index);
        refresh();

        ch = getch();

        if (ch == 27 || ch == CTRL_KEY('t'))
            break;

        {
            int action;
            action = shortcut_action(ch);

            if (action != ACT_NONE) {
                execute_action(action);

                /*
                 * Some actions, notably Quit, may return after a cancelled
                 * confirmation.  In all cases close the menu cleanly.
                 */
                break;
            }
        }

        if (ch == KEY_LEFT) {
            menu_index--;
            if (menu_index < 0)
                menu_index = 4;
            items = get_menu(menu_index, &count);
            item_index = first_selectable(items, count);
        } else if (ch == KEY_RIGHT) {
            menu_index++;
            if (menu_index > 4)
                menu_index = 0;
            items = get_menu(menu_index, &count);
            item_index = first_selectable(items, count);
        } else if (ch == KEY_UP) {
            item_index = next_selectable(items, count, item_index, -1);
        } else if (ch == KEY_DOWN) {
            item_index = next_selectable(items, count, item_index, 1);
        } else if (ch == '\n' || ch == '\r') {
            int action;
            action = items[item_index].action;
            if (action != ACT_NONE) {
                execute_action(action);
                break;
            }
        }
    }

    draw_screen();
}

static void process_key(int ch)
{
    int len;

    len = (int)strlen(lines[cy]);

    if (ch != CTRL_KEY('q'))
    
    if (ch == CTRL_KEY('t')) {
        activate_menu();
        return;
    }

    /*
     * IRIX curses may define KEY_BACKSPACE as the same numeric value as
     * ASCII backspace (8).  Handle backspace before the switch so we
     * never create duplicate case labels.
     */
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (selecting)
            delete_selection();
        else
            delete_backward();
        scroll_screen();
        return;
    }

    switch (ch) {
    case CTRL_KEY('q'):
        quit_editor();
        break;

    case CTRL_KEY('n'):
        create_new_buffer();
        break;

    case CTRL_KEY('p'):
        switch_buffer(-1);
        break;

    case CTRL_KEY('e'):
        switch_buffer(1);
        break;

    case CTRL_KEY('s'):
        do_save();
        break;

    case CTRL_KEY('a'):
        do_save_as();
        break;

    case CTRL_KEY('o'):
        do_open();
        break;

    case CTRL_KEY('u'):
        move_word_left();
        break;

    case CTRL_KEY('d'):
        move_word_right();
        break;

    case CTRL_KEY('f'):
        do_find();
        break;

    case CTRL_KEY('h'):
        do_replace();
        break;

    case CTRL_KEY('l'):
        do_goto();
        break;

    case CTRL_KEY('z'):
        do_undo();
        break;

    case CTRL_KEY('r'):
        do_redo();
        break;

    case CTRL_KEY('b'):
        selecting = !selecting;
        if (selecting) {
            sel_sy = cy;
            sel_sx = cx;
            set_status("Selection started");
        } else {
            set_status("Selection cleared");
        }
        break;

    case CTRL_KEY('c'):
        copy_selection_or_line();
        break;

    case CTRL_KEY('x'):
        cut_selection_or_line();
        break;

    case CTRL_KEY('v'):
        paste_text();
        break;

    case CTRL_KEY('g'):
        help_screen();
        break;

    case KEY_LEFT:
        if (cx > 0) {
            cx--;
        } else if (cy > 0) {
            cy--;
            cx = (int)strlen(lines[cy]);
        }
        break;

    case KEY_RIGHT:
        if (cx < len) {
            cx++;
        } else if (cy < nlines - 1) {
            cy++;
            cx = 0;
        }
        break;

    case KEY_UP:
        if (cy > 0)
            cy--;
        if (cx > (int)strlen(lines[cy]))
            cx = (int)strlen(lines[cy]);
        break;

    case KEY_DOWN:
        if (cy < nlines - 1)
            cy++;
        if (cx > (int)strlen(lines[cy]))
            cx = (int)strlen(lines[cy]);
        break;

#ifdef KEY_HOME
    case KEY_HOME:
        cx = 0;
        break;
#endif

#ifdef KEY_END
    case KEY_END:
        cx = (int)strlen(lines[cy]);
        break;
#endif

#ifdef KEY_PPAGE
    case KEY_PPAGE:
        cy -= LINES - 4;
        if (cy < 0)
            cy = 0;
        if (cx > (int)strlen(lines[cy]))
            cx = (int)strlen(lines[cy]);
        break;
#endif

#ifdef KEY_NPAGE
    case KEY_NPAGE:
        cy += LINES - 4;
        if (cy >= nlines)
            cy = nlines - 1;
        if (cx > (int)strlen(lines[cy]))
            cx = (int)strlen(lines[cy]);
        break;
#endif

#ifdef KEY_DC
    case KEY_DC:
        if (selecting)
            delete_selection();
        else
            delete_forward();
        break;
#endif

    case '\n':
    case '\r':
        if (selecting)
            delete_selection();
        insert_newline();
        break;

    case '\t':
        if (selecting)
            delete_selection();
        smart_tab();
        break;

    default:
        if (ch >= 32 && ch <= 126) {
            if (selecting)
                delete_selection();
            insert_typed_char(ch);
        }
        break;
    }

    scroll_screen();
}

static void init_colors_if_possible(void)
{
    use_color = 0;

#ifdef COLOR_BLACK
    if (has_colors()) {
        if (start_color() != ERR) {
            init_pair(CP_NORMAL,    COLOR_WHITE,   COLOR_BLACK);
            init_pair(CP_KEYWORD,   COLOR_CYAN,    COLOR_BLACK);
            init_pair(CP_STRING,    COLOR_YELLOW,  COLOR_BLACK);
            init_pair(CP_COMMENT,   COLOR_GREEN,   COLOR_BLACK);
            init_pair(CP_NUMBER,    COLOR_MAGENTA, COLOR_BLACK);
            init_pair(CP_PREPROC,   COLOR_BLUE,    COLOR_BLACK);
            init_pair(CP_LINENO,    COLOR_CYAN,    COLOR_BLACK);
            init_pair(CP_STATUS,    COLOR_BLACK,   COLOR_CYAN);
            init_pair(CP_MENU,      COLOR_BLACK,   COLOR_WHITE);
            init_pair(CP_MENU_SEL,  COLOR_WHITE,   COLOR_BLUE);
            init_pair(CP_SELECT,    COLOR_BLACK,   COLOR_YELLOW);
            init_pair(CP_MATCH,     COLOR_BLACK,   COLOR_GREEN);

            use_color = 1;
            return;
        }
    }
#endif

    /*
     * Syntax highlighting in v6 is rendered by the ANSI overlay engine.
     * This deliberately avoids relying on IRIX curses colour support.
     * Curses colours, when available, are still used for the editor UI.
     */
}

int main(int argc, char **argv)
{
    int ch;

    {
        int bi;
        memset(buffers, 0, sizeof(buffers));
        for (bi = 0; bi < MAX_BUFFERS; bi++)
            buffers[bi].line_count = 0;
    }

    statusmsg[0] = '\0';
    last_search[0] = '\0';
    syntax_dir_override[0] = '\0';

    curbuf = 0;
    buffer_count = 1;
    init_buffer();
    load_config();
    load_syntax_definitions();

    if (argc > 1)
        load_file(argv[1]);

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);

    init_colors_if_possible();

    for (;;) {
        draw_screen();
        ch = getch();
        process_key(ch);
    }

    return 0;
}