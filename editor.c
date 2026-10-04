/*
 * tedit_v4.c - portable curses text/code editor for classic UNIX
 *
 * Build:
 *   cc -o tedit tedit_v4.c -lcurses
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

#define MAX_LINES   20000
#define MAX_LINE    8192
#define NAME_LEN    1024
#define STATUS_LEN  1024
#define SEARCH_LEN  256
#define CLIP_LEN    65536
#define UNDO_DEPTH  64

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
    char **lines;
    int nlines;
    int cy, cx;
    int rowoff, coloff;
} Snapshot;

typedef struct {
    const char *label;
    int action;
} MenuItem;

enum {
    ACT_NONE = 0,
    ACT_OPEN,
    ACT_SAVE,
    ACT_SAVE_AS,
    ACT_QUIT,
    ACT_UNDO,
    ACT_REDO,
    ACT_SELECT,
    ACT_COPY,
    ACT_CUT,
    ACT_PASTE,
    ACT_FIND,
    ACT_REPLACE,
    ACT_GOTO,
    ACT_TOGGLE_SYNTAX,
    ACT_TOGGLE_LINES,
    ACT_TAB2,
    ACT_TAB4,
    ACT_TAB8,
    ACT_HELP,
    ACT_ABOUT
};

static char *lines[MAX_LINES];
static int nlines = 1;
static int cy = 0, cx = 0;
static int rowoff = 0, coloff = 0;
static int modified = 0;
static int quit_armed = 0;
static int tab_width = 4;
static int use_color = 0;
static int ansi_color_fallback = 0;
static int syntax_enabled = 1;
static int line_numbers = 1;

static int selecting = 0;
static int sel_sy = 0, sel_sx = 0;

static char filename[NAME_LEN];
static char statusmsg[STATUS_LEN];
static char last_search[SEARCH_LEN];

static char *clipboard = NULL;

static Snapshot undo_stack[UNDO_DEPTH];
static int undo_count = 0;
static Snapshot redo_stack[UNDO_DEPTH];
static int redo_count = 0;

static const char *c_keywords[] = {
    "auto","break","case","char","const","continue","default","do",
    "double","else","enum","extern","float","for","goto","if","int",
    "long","register","return","short","signed","sizeof","static",
    "struct","switch","typedef","union","unsigned","void","volatile",
    "while","class","namespace","template","public","private","protected",
    "virtual","bool","true","false","new","delete","this","try","catch",
    0
};

static const MenuItem file_menu[] = {
    {"Open...        ^O", ACT_OPEN},
    {"Save           ^S", ACT_SAVE},
    {"Save As...     ^A", ACT_SAVE_AS},
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
    {"Paste          ^V", ACT_PASTE}
};

static const MenuItem search_menu[] = {
    {"Find...        ^F", ACT_FIND},
    {"Replace...     ^H", ACT_REPLACE},
    {"Goto line...   ^L", ACT_GOTO}
};

static const MenuItem options_menu[] = {
    {"Toggle syntax", ACT_TOGGLE_SYNTAX},
    {"Toggle line numbers", ACT_TOGGLE_LINES},
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

static void init_buffer(void)
{
    int i;
    for (i = 0; i < MAX_LINES; i++)
        lines[i] = NULL;
    lines[0] = dupstr("");
    nlines = 1;
}

static void free_buffer(void)
{
    int i;
    for (i = 0; i < nlines; i++) {
        if (lines[i] != NULL)
            free(lines[i]);
        lines[i] = NULL;
    }
}

static void free_snapshot(Snapshot *s)
{
    int i;
    if (s->lines != NULL) {
        for (i = 0; i < s->nlines; i++)
            if (s->lines[i] != NULL)
                free(s->lines[i]);
        free(s->lines);
    }
    s->lines = NULL;
    s->nlines = 0;
}

static int make_snapshot(Snapshot *s)
{
    int i;

    s->lines = (char **)malloc(sizeof(char *) * nlines);
    if (s->lines == NULL)
        return -1;

    s->nlines = nlines;
    s->cy = cy;
    s->cx = cx;
    s->rowoff = rowoff;
    s->coloff = coloff;

    for (i = 0; i < nlines; i++) {
        s->lines[i] = dupstr(lines[i]);
        if (s->lines[i] == NULL) {
            int j;
            for (j = 0; j < i; j++)
                free(s->lines[j]);
            free(s->lines);
            s->lines = NULL;
            return -1;
        }
    }

    return 0;
}

static void restore_snapshot(const Snapshot *s)
{
    int i;

    free_buffer();

    nlines = s->nlines;
    for (i = 0; i < nlines; i++)
        lines[i] = dupstr(s->lines[i]);

    cy = s->cy;
    cx = s->cx;
    rowoff = s->rowoff;
    coloff = s->coloff;
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

static int is_c_file(void)
{
    const char *p;

    if (filename[0] == '\0')
        return 0;

    p = strrchr(filename, '.');
    if (p == NULL)
        return 0;

    return !strcmp(p, ".c")   || !strcmp(p, ".h")   ||
           !strcmp(p, ".cc")  || !strcmp(p, ".hh")  ||
           !strcmp(p, ".cpp") || !strcmp(p, ".hpp");
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

static int trimmed_line_ends_with(const char *s, char ch)
{
    int len;

    len = (int)strlen(s);

    while (len > 0 && isspace((unsigned char)s[len - 1]))
        len--;

    return len > 0 && s[len - 1] == ch;
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

    if (ch == '}' && prefix_is_whitespace(lines[cy], cx))
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

    indent = leading_spaces(line);
    if (trimmed_line_ends_with(left, '{'))
        indent += tab_width;

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

static int is_keyword(const char *s, int len)
{
    int i;

    for (i = 0; c_keywords[i] != 0; i++) {
        if ((int)strlen(c_keywords[i]) == len &&
            !strncmp(s, c_keywords[i], len))
            return 1;
    }

    return 0;
}

static int scan_block_comment_state_to(int upto_row)
{
    int row;
    int i;
    int in_comment;
    const char *s;

    in_comment = 0;

    for (row = 0; row < upto_row; row++) {
        s = lines[row];
        i = 0;

        while (s[i] != '\0') {
            if (in_comment) {
                if (s[i] == '*' && s[i + 1] == '/') {
                    in_comment = 0;
                    i += 2;
                } else {
                    i++;
                }
            } else {
                if (s[i] == '/' && s[i + 1] == '*') {
                    in_comment = 1;
                    i += 2;
                } else if (s[i] == '"' || s[i] == '\'') {
                    int q;
                    q = s[i++];
                    while (s[i] != '\0') {
                        if (s[i] == '\\' && s[i + 1] != '\0')
                            i += 2;
                        else if (s[i] == q) {
                            i++;
                            break;
                        } else {
                            i++;
                        }
                    }
                } else if (s[i] == '/' && s[i + 1] == '/') {
                    break;
                } else {
                    i++;
                }
            }
        }
    }

    return in_comment;
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

static void draw_c_line(int y, int row, const char *s, int *block_comment, int gutter)
{
    int i;
    int len;
    int visible_start;
    int visible_end;

    i = 0;
    len = (int)strlen(s);
    visible_start = coloff;
    visible_end = coloff + (COLS - gutter - 1);

    while (i < len) {
        int start;
        int pair;

        start = i;
        pair = CP_NORMAL;

        if (*block_comment) {
            pair = CP_COMMENT;
            while (i < len) {
                if (s[i] == '*' && i + 1 < len && s[i + 1] == '/') {
                    i += 2;
                    *block_comment = 0;
                    break;
                }
                i++;
            }
        } else if (s[i] == '/' && i + 1 < len && s[i + 1] == '*') {
            pair = CP_COMMENT;
            *block_comment = 1;
            i += 2;
            while (i < len) {
                if (s[i] == '*' && i + 1 < len && s[i + 1] == '/') {
                    i += 2;
                    *block_comment = 0;
                    break;
                }
                i++;
            }
        } else if (s[i] == '/' && i + 1 < len && s[i + 1] == '/') {
            pair = CP_COMMENT;
            i = len;
        } else if (s[i] == '"' || s[i] == '\'') {
            int q;
            q = s[i++];
            pair = CP_STRING;
            while (i < len) {
                if (s[i] == '\\' && i + 1 < len)
                    i += 2;
                else if (s[i] == q) {
                    i++;
                    break;
                } else {
                    i++;
                }
            }
        } else if (s[i] == '#' && prefix_is_whitespace(s, i)) {
            pair = CP_PREPROC;
            i = len;
        } else if (isdigit((unsigned char)s[i])) {
            pair = CP_NUMBER;
            i++;
            while (i < len &&
                  (isalnum((unsigned char)s[i]) ||
                   s[i] == '.' || s[i] == 'x' || s[i] == 'X'))
                i++;
        } else if (isalpha((unsigned char)s[i]) || s[i] == '_') {
            i++;
            while (i < len &&
                  (isalnum((unsigned char)s[i]) || s[i] == '_'))
                i++;
            if (is_keyword(s + start, i - start))
                pair = CP_KEYWORD;
        } else {
            i++;
        }

        {
            int j;
            int ds = start < visible_start ? visible_start : start;
            int de = i > visible_end ? visible_end : i;

            for (j = ds; j < de; j++)
                draw_char_with_attrs(y,
                                     gutter + (j - visible_start),
                                     row,
                                     j,
                                     s[j],
                                     pair);
        }
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

static void ansi_overlay_c_line(int screen_y, int row,
                                const char *s, int *block_comment,
                                int gutter)
{
    int i;
    int len;
    int visible_start;
    int visible_end;

    i = 0;
    len = (int)strlen(s);
    visible_start = coloff;
    visible_end = coloff + (COLS - gutter - 1);

    while (i < len) {
        int start;
        int pair;

        start = i;
        pair = CP_NORMAL;

        if (*block_comment) {
            pair = CP_COMMENT;
            while (i < len) {
                if (s[i] == '*' && i + 1 < len && s[i + 1] == '/') {
                    i += 2;
                    *block_comment = 0;
                    break;
                }
                i++;
            }
        } else if (s[i] == '/' && i + 1 < len && s[i + 1] == '*') {
            pair = CP_COMMENT;
            *block_comment = 1;
            i += 2;
            while (i < len) {
                if (s[i] == '*' && i + 1 < len && s[i + 1] == '/') {
                    i += 2;
                    *block_comment = 0;
                    break;
                }
                i++;
            }
        } else if (s[i] == '/' && i + 1 < len && s[i + 1] == '/') {
            pair = CP_COMMENT;
            i = len;
        } else if (s[i] == '"' || s[i] == '\'') {
            int q;
            q = s[i++];
            pair = CP_STRING;

            while (i < len) {
                if (s[i] == '\\' && i + 1 < len)
                    i += 2;
                else if (s[i] == q) {
                    i++;
                    break;
                } else {
                    i++;
                }
            }
        } else if (s[i] == '#' && prefix_is_whitespace(s, i)) {
            pair = CP_PREPROC;
            i = len;
        } else if (isdigit((unsigned char)s[i])) {
            pair = CP_NUMBER;
            i++;
            while (i < len &&
                  (isalnum((unsigned char)s[i]) ||
                   s[i] == '.' || s[i] == 'x' || s[i] == 'X'))
                i++;
        } else if (isalpha((unsigned char)s[i]) || s[i] == '_') {
            i++;
            while (i < len &&
                  (isalnum((unsigned char)s[i]) || s[i] == '_'))
                i++;

            if (is_keyword(s + start, i - start))
                pair = CP_KEYWORD;
        } else {
            i++;
        }

        if (pair != CP_NORMAL &&
            i > visible_start && start < visible_end) {
            int ds;
            int de;

            ds = start < visible_start ? visible_start : start;
            de = i > visible_end ? visible_end : i;

            if (de > ds) {
                int j;
                int run_start;

                /*
                 * Do not paint over selected cells; keep curses reverse
                 * highlighting visible.  Emit contiguous non-selected runs.
                 */
                j = ds;
                while (j < de) {
                    while (j < de && pos_selected(row, j))
                        j++;

                    run_start = j;

                    while (j < de && !pos_selected(row, j))
                        j++;

                    if (j > run_start) {
                        ansi_overlay_span(
                            screen_y,
                            gutter + (run_start - visible_start),
                            s + run_start,
                            j - run_start,
                            pair
                        );
                    }
                }
            }
        }
    }
}

static void ansi_overlay_syntax(void)
{
    int y;
    int filerow;
    int textrows;
    int gutter;
    int block_comment;

    if (!ansi_color_fallback || !syntax_enabled || !is_c_file())
        return;

    gutter = line_numbers ? 6 : 0;
    textrows = LINES - 3;
    if (textrows < 1)
        textrows = 1;

    block_comment = scan_block_comment_state_to(rowoff);

    for (y = 0; y < textrows; y++) {
        filerow = rowoff + y;

        if (filerow >= nlines)
            break;

        ansi_overlay_c_line(y + 1, filerow, lines[filerow],
                            &block_comment, gutter);
    }

    /* Restore default attributes and place the cursor back in the editor. */
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
    int block_comment;
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
    clrtoeol();

    if (use_color)
        attroff(COLOR_PAIR(CP_MENU));
    else
        attroff(A_REVERSE);

    gutter = line_numbers ? 6 : 0;
    textrows = LINES - 3;
    if (textrows < 1)
        textrows = 1;

    block_comment = scan_block_comment_state_to(rowoff);

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

        if (syntax_enabled && is_c_file() && use_color)
            draw_c_line(y + 1, filerow, lines[filerow], &block_comment, gutter);
        else
            draw_plain_line(y + 1, filerow, lines[filerow], gutter);
    }

    if (use_color)
        attron(COLOR_PAIR(CP_STATUS));
    else
        attron(A_REVERSE);

    name_room = COLS - 48;
    if (name_room < 10)
        name_room = 10;

    sprintf(buf, " %-*.*s %s %s Ln %d/%d Col %d TAB:%d %s ",
            name_room, name_room,
            filename[0] ? filename : "[No Name]",
            modified ? "[+]" : "",
            selecting ? "[SEL]" : "",
            cy + 1, nlines, cx + 1, tab_width,
            syntax_enabled ? "SYN" : "TXT");

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

    if (ansi_color_fallback)
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

static void do_open(void)
{
    char name[NAME_LEN];

    if (modified) {
        set_status("Unsaved changes: save first");
        return;
    }

    if (!prompt_input("Open file: ", name, sizeof(name)) || !name[0]) {
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

    mvaddstr(1, 2, "TEDIT v4 - portable curses editor");
    mvaddstr(3, 2, "Ctrl-T       Menu bar");
    mvaddstr(4, 2, "Ctrl-O       Open");
    mvaddstr(5, 2, "Ctrl-S       Save");
    mvaddstr(6, 2, "Ctrl-A       Save As");
    mvaddstr(7, 2, "Ctrl-F       Find");
    mvaddstr(8, 2, "Ctrl-H       Replace");
    mvaddstr(9, 2, "Ctrl-L       Goto line");
    mvaddstr(10, 2, "Ctrl-Z       Undo");
    mvaddstr(11, 2, "Ctrl-R       Redo");
    mvaddstr(12, 2, "Ctrl-B       Start/stop selection");
    mvaddstr(13, 2, "Ctrl-C       Copy selection/current line");
    mvaddstr(14, 2, "Ctrl-X       Cut selection/current line");
    mvaddstr(15, 2, "Ctrl-V       Paste");
    mvaddstr(16, 2, "Ctrl-Q       Quit");
    mvaddstr(18, 2, "Tab          Spaces to next tab stop");
    mvaddstr(19, 2, "Enter        Smart-indent new line");
    mvaddstr(20, 2, "}            Auto-outdent in leading whitespace");
    mvaddstr(22, 2, "Press any key.");

    refresh();
    getch();
}

static void about_screen(void)
{
    erase();

    mvaddstr(2, 4, "TEDIT v4");
    mvaddstr(4, 4, "Portable curses code editor for classic UNIX.");
    mvaddstr(5, 4, "Designed to compile on IRIX using plain curses.");
    mvaddstr(7, 4, "Press any key.");

    refresh();
    getch();
}

static void quit_editor(void)
{
    if (modified && !quit_armed) {
        quit_armed = 1;
        set_status("Unsaved changes. Ctrl-Q again to quit.");
        return;
    }

    erase();
    refresh();
    endwin();

    printf("\033[2J\033[H");
    fflush(stdout);

    free_buffer();
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);

    if (clipboard != NULL)
        free(clipboard);

    exit(0);
}

static void execute_action(int action)
{
    switch (action) {
    case ACT_OPEN: do_open(); break;
    case ACT_SAVE: do_save(); break;
    case ACT_SAVE_AS: do_save_as(); break;
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

        if (ch == 27)
            break;

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
        quit_armed = 0;

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

    case CTRL_KEY('s'):
        do_save();
        break;

    case CTRL_KEY('a'):
        do_save_as();
        break;

    case CTRL_KEY('o'):
        do_open();
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
            insert_char(ch);
        }
        break;
    }

    scroll_screen();
}

static void init_colors_if_possible(void)
{
    use_color = 0;
    ansi_color_fallback = 0;

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
     * SGI IRIX curses can return ERR from start_color()/0 from
     * has_colors() even when an xterm-compatible remote terminal accepts
     * ANSI colour sequences.  The ANSI test program confirmed this setup
     * does, so use the fallback when TERM looks xterm/ansi/vt-like.
     */
    {
        char *term;
        term = getenv("TERM");

        if (term != NULL &&
           (strstr(term, "xterm") != NULL ||
            strstr(term, "ansi")  != NULL ||
            strstr(term, "vt")    != NULL ||
            strstr(term, "screen") != NULL)) {
            ansi_color_fallback = 1;
        }
    }
}

int main(int argc, char **argv)
{
    int ch;

    filename[0] = '\0';
    statusmsg[0] = '\0';
    last_search[0] = '\0';

    init_buffer();

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
