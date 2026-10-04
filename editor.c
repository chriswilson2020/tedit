/*
 * tedit.c - small portable curses editor
 *
 * Build: cc -o tedit tedit.c -lcurses
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <curses.h>

#define MAX_LINES 20000
#define MAX_LINE 8192
#define NAME_LEN 1024
#define STATUS_LEN 1024
#define SEARCH_LEN 256

#define CTRL_KEY(x) ((x) & 0x1f)

#define CP_NORMAL 1
#define CP_KEYWORD 2
#define CP_STRING 3
#define CP_COMMENT 4
#define CP_NUMBER 5
#define CP_PREPROC 6
#define CP_LINENO 7
#define CP_STATUS 8
#define CP_MENU 9

static char *lines[MAX_LINES];
static int nlines = 1;
static int cy = 0, cx = 0;
static int rowoff = 0, coloff = 0;
static int modified = 0;
static int quit_armed = 0;
static int tab_width = 4;
static int use_color = 0;

static char filename[NAME_LEN];
static char statusmsg[STATUS_LEN];
static char last_search[SEARCH_LEN];

static const char *c_keywords[] = {
    "auto","break","case","char","const","continue","default","do",
    "double","else","enum","extern","float","for","goto","if","int",
    "long","register","return","short","signed","sizeof","static",
    "struct","switch","typedef","union","unsigned","void","volatile",
    "while","class","namespace","template","public","private","protected",
    "virtual","bool","true","false","new","delete","this","try","catch",
    0
};

static char *dupstr(const char *s)
{
    char *p = (char *)malloc(strlen(s) + 1);
    if (p != NULL) strcpy(p, s);
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
    for (i = 0; i < MAX_LINES; i++) lines[i] = NULL;
    lines[0] = dupstr("");
    nlines = 1;
}

static void free_buffer(void)
{
    int i;
    for (i = 0; i < nlines; i++) {
        if (lines[i]) free(lines[i]);
        lines[i] = NULL;
    }
}

static int is_c_file(void)
{
    const char *p;
    if (!filename[0]) return 0;
    p = strrchr(filename, '.');
    if (!p) return 0;
    return !strcmp(p, ".c") || !strcmp(p, ".h") ||
    !strcmp(p, ".cc") || !strcmp(p, ".hh") ||
    !strcmp(p, ".cpp") || !strcmp(p, ".hpp");
}

static int load_file(const char *name)
{
    FILE *fp;
    char buf[MAX_LINE];
    int len;

    fp = fopen(name, "r");
    if (!fp) {
        free_buffer();
        lines[0] = dupstr("");
        nlines = 1;
        strncpy(filename, name, NAME_LEN - 1);
        filename[NAME_LEN - 1] = '\0';
        modified = 0;
        cy = cx = rowoff = coloff = 0;
        sprintf(statusmsg, "New file: %s", filename);
        return 0;
    }

    free_buffer();
    nlines = 0;

    while (fgets(buf, sizeof(buf), fp) != NULL && nlines < MAX_LINES) {
        len = (int)strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';

        lines[nlines] = dupstr(buf);
        if (!lines[nlines]) {
            fclose(fp);
            set_status("Out of memory");
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
    sprintf(statusmsg, "Loaded %s", filename);
    return 0;
}

static int save_file_as(const char *name)
{
    FILE *fp;
    int i;

    fp = fopen(name, "w");
    if (!fp) {
        sprintf(statusmsg, "Cannot write %s", name);
        return -1;
    }

    for (i = 0; i < nlines; i++) {
        fputs(lines[i], fp);
        if (i < nlines - 1) fputc('\n', fp);
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
    char *old = lines[row];
    char *p;
    int len = (int)strlen(old);

    if (len >= MAX_LINE - 2) return -1;
    if (col < 0) col = 0;
    if (col > len) col = len;

    p = (char *)malloc(len + 2);
    if (!p) return -1;

    memcpy(p, old, col);
    p[col] = (char)ch;
    strcpy(p + col + 1, old + col);

    free(old);
    lines[row] = p;
    return 0;
}

static int leading_spaces(const char *s)
{
    int i = 0, count = 0;
    while (s[i] == ' ' || s[i] == '\t') {
        if (s[i] == '\t') count += tab_width;
        else count++;
        i++;
    }
    return count;
}

static int prefix_is_whitespace(const char *s, int upto)
{
    int i;
    for (i = 0; i < upto; i++)
        if (s[i] != ' ' && s[i] != '\t') return 0;
        return 1;
}

static int trimmed_line_ends_with(const char *s, char ch)
{
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
    return len > 0 && s[len - 1] == ch;
}

static int insert_spaces(int count)
{
    int i;
    for (i = 0; i < count; i++) {
        if (insert_char_at(cy, cx, ' ') != 0) return -1;
        cx++;
    }
    modified = 1;
    return 0;
}

static void smart_tab(void)
{
    int count = tab_width - (cx % tab_width);
    if (count <= 0) count = tab_width;
    insert_spaces(count);
}

static void smart_outdent_for_brace(void)
{
    char *line = lines[cy];
    int remove_count, len;

    if (!prefix_is_whitespace(line, cx)) return;

    remove_count = cx % tab_width;
    if (remove_count == 0) remove_count = tab_width;
    if (remove_count > cx) remove_count = cx;

    len = (int)strlen(line);
    memmove(line + cx - remove_count, line + cx, len - cx + 1);
    cx -= remove_count;
    modified = 1;
}

static void insert_char(int ch)
{
    if (ch == '}' && prefix_is_whitespace(lines[cy], cx))
        smart_outdent_for_brace();

    if (insert_char_at(cy, cx, ch) == 0) {
        cx++;
        modified = 1;
    }
}

static void delete_backward(void)
{
    char *line = lines[cy];
    int len = (int)strlen(line);

    if (cx > 0) {
        int remove_count = 1;

        if (prefix_is_whitespace(line, cx)) {
            remove_count = cx % tab_width;
            if (remove_count == 0) remove_count = tab_width;
            if (remove_count > cx) remove_count = cx;
        }

        memmove(line + cx - remove_count, line + cx, len - cx + 1);
        cx -= remove_count;
        modified = 1;
        return;
    }

    if (cy > 0) {
        char *prev = lines[cy - 1];
        char *joined;
        int plen = (int)strlen(prev);
        int i;

        joined = (char *)malloc(plen + len + 1);
        if (!joined) return;

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
    char *line = lines[cy];
    int len = (int)strlen(line);

    if (cx < len) {
        memmove(line + cx, line + cx + 1, len - cx);
        modified = 1;
        return;
    }

    if (cy < nlines - 1) {
        char *next = lines[cy + 1];
        char *joined;
        int i;

        joined = (char *)malloc(strlen(line) + strlen(next) + 1);
        if (!joined) return;

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
    char *line, *left, *right;
    int len, i, indent;

    if (nlines >= MAX_LINES) return;

    line = lines[cy];
    len = (int)strlen(line);

    left = (char *)malloc(cx + 1);
    right = (char *)malloc(len - cx + 1);

    if (!left || !right) {
        if (left) free(left);
        if (right) free(right);
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
    if (indent > 0) insert_spaces(indent);

    modified = 1;
}

static void scroll_screen(void)
{
    int textrows = LINES - 3;
    int textcols = COLS - 7;

    if (textrows < 1) textrows = 1;
    if (textcols < 1) textcols = 1;

    if (cy < rowoff) rowoff = cy;
    if (cy >= rowoff + textrows) rowoff = cy - textrows + 1;
    if (cx < coloff) coloff = cx;
    if (cx >= coloff + textcols) coloff = cx - textcols + 1;

    if (rowoff < 0) rowoff = 0;
    if (coloff < 0) coloff = 0;
}

static int is_keyword(const char *s, int len)
{
    int i;
    for (i = 0; c_keywords[i]; i++) {
        if ((int)strlen(c_keywords[i]) == len &&
            !strncmp(s, c_keywords[i], len))
            return 1;
    }
    return 0;
}

static int scan_block_comment_state_to(int upto_row)
{
    int row, i, in_comment = 0;
    const char *s;

    for (row = 0; row < upto_row; row++) {
        s = lines[row];
        i = 0;

        while (s[i]) {
            if (in_comment) {
                if (s[i] == '*' && s[i + 1] == '/') {
                    in_comment = 0;
                    i += 2;
                } else i++;
            } else {
                if (s[i] == '/' && s[i + 1] == '*') {
                    in_comment = 1;
                    i += 2;
                } else if (s[i] == '"' || s[i] == '\'') {
                    int q = s[i++];
                    while (s[i]) {
                        if (s[i] == '\\' && s[i + 1]) i += 2;
                        else if (s[i] == q) { i++; break; }
                        else i++;
                    }
                } else if (s[i] == '/' && s[i + 1] == '/') {
                    break;
                } else i++;
            }
        }
    }

    return in_comment;
}

static void draw_c_line(int y, const char *s, int *block_comment)
{
    int i = 0, len = (int)strlen(s);
    int visible_start = coloff;
    int visible_end = coloff + (COLS - 7);

    while (i < len) {
        int start = i;
        int pair = CP_NORMAL;

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
            int q = s[i++];
            pair = CP_STRING;
            while (i < len) {
                if (s[i] == '\\' && i + 1 < len) i += 2;
                else if (s[i] == q) { i++; break; }
                else i++;
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

        if (i > visible_start && start < visible_end) {
            int ds = start < visible_start ? visible_start : start;
            int de = i > visible_end ? visible_end : i;

            if (de > ds) {
                if (use_color) attron(COLOR_PAIR(pair));
                mvaddnstr(y, 6 + (ds - visible_start), s + ds, de - ds);
                if (use_color) attroff(COLOR_PAIR(pair));
            }
        }
    }
}

static void draw_screen(void)
{
    int y, filerow, textrows;
    int block_comment;
    char buf[STATUS_LEN];
    int name_room;

    scroll_screen();
    erase();

    if (use_color) attron(COLOR_PAIR(CP_MENU));
    else attron(A_REVERSE);

    mvaddstr(0, 0, " File  Edit  Search  Options  Help ");
    clrtoeol();

    if (use_color) attroff(COLOR_PAIR(CP_MENU));
    else attroff(A_REVERSE);

    textrows = LINES - 3;
    if (textrows < 1) textrows = 1;

    block_comment = scan_block_comment_state_to(rowoff);

    for (y = 0; y < textrows; y++) {
        filerow = rowoff + y;

        if (use_color) attron(COLOR_PAIR(CP_LINENO));
        else attron(A_BOLD);

        if (filerow < nlines)
            mvprintw(y + 1, 0, "%5d ", filerow + 1);
        else
            mvaddstr(y + 1, 0, "    ~ ");

        if (use_color) attroff(COLOR_PAIR(CP_LINENO));
        else attroff(A_BOLD);

        if (filerow >= nlines) continue;

        if (is_c_file() && use_color) {
            draw_c_line(y + 1, lines[filerow], &block_comment);
        } else {
            int len = (int)strlen(lines[filerow]);
            if (coloff < len)
                mvaddnstr(y + 1, 6, lines[filerow] + coloff, COLS - 7);
        }
    }

    if (use_color) attron(COLOR_PAIR(CP_STATUS));
    else attron(A_REVERSE);

    name_room = COLS - 35;
    if (name_room < 10) name_room = 10;

    sprintf(buf, " %-*.*s %s  Ln %d/%d Col %d  TAB:%d ",
            name_room, name_room,
            filename[0] ? filename : "[No Name]",
            modified ? "[+]" : "",
            cy + 1, nlines, cx + 1, tab_width);

    mvaddnstr(LINES - 2, 0, buf, COLS);
    clrtoeol();

    if (use_color) attroff(COLOR_PAIR(CP_STATUS));
    else attroff(A_REVERSE);

    mvaddstr(LINES - 1, 0,
             "^O Open  ^S Save  ^F Find  F3 Next  ^G Help  ^Q Quit");

    if (statusmsg[0]) {
        int pos = COLS - (int)strlen(statusmsg) - 1;
        if (pos > 0)
            mvaddnstr(LINES - 1, pos, statusmsg, COLS - pos);
    }

    move((cy - rowoff) + 1, 6 + cx - coloff);
    refresh();
}

static int prompt_input(const char *prompt, char *out, int outlen)
{
    int ch, len = 0;

    out[0] = '\0';

    for (;;) {
        move(LINES - 1, 0);
        clrtoeol();
        mvaddstr(LINES - 1, 0, prompt);
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
            if (len > 0) out[--len] = '\0';
        } else if (ch >= 32 && ch <= 126 && len < outlen - 1) {
            out[len++] = (char)ch;
            out[len] = '\0';
        }
    }
}

static void do_save(void)
{
    char name[NAME_LEN];

    if (!filename[0]) {
        if (!prompt_input("Save as: ", name, sizeof(name)) || !name[0]) {
            set_status("Save cancelled");
            return;
        }
        save_file_as(name);
    } else {
        save_file_as(filename);
    }
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

    if (!needle[0]) return 0;

    for (row = start_row; row < nlines; row++) {
        p = strstr(lines[row] + (row == start_row ? start_col : 0), needle);
        if (p) {
            cy = row;
            cx = (int)(p - lines[row]);
            scroll_screen();
            return 1;
        }
    }

    for (row = 0; row <= start_row && row < nlines; row++) {
        p = strstr(lines[row], needle);
        if (p && (row < start_row || (int)(p - lines[row]) < start_col)) {
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

    if (!query[0]) return;

    strncpy(last_search, query, SEARCH_LEN - 1);
    last_search[SEARCH_LEN - 1] = '\0';

    if (find_from_position(last_search, cy, cx + 1))
        sprintf(statusmsg, "Found: %s", last_search);
    else
        sprintf(statusmsg, "Not found: %s", last_search);
}

static void find_next(void)
{
    if (!last_search[0]) {
        set_status("No previous search");
        return;
    }

    if (find_from_position(last_search, cy, cx + 1))
        sprintf(statusmsg, "Found: %s", last_search);
    else
        sprintf(statusmsg, "Not found: %s", last_search);
}

static void help_screen(void)
{
    erase();
    mvaddstr(1, 2, "TEDIT v2 - portable curses editor");
    mvaddstr(3, 2, "Ctrl-O       Open file");
    mvaddstr(4, 2, "Ctrl-S       Save / Save As");
    mvaddstr(5, 2, "Ctrl-F       Find");
    mvaddstr(6, 2, "F3           Find next");
    mvaddstr(7, 2, "Ctrl-Q       Quit");
    mvaddstr(8, 2, "Tab          Spaces to next tab stop");
    mvaddstr(9, 2, "Enter        Carry indentation; +1 level after {");
    mvaddstr(10, 2, "}            Auto-outdent in leading whitespace");
    mvaddstr(11, 2, "Backspace    Smart unindent in leading whitespace");
    mvaddstr(13, 2, "C/C++ files get simple syntax highlighting.");
    mvaddstr(15, 2, "Press any key.");
    refresh();
    getch();
}

static void process_key(int ch)
{
    int len = (int)strlen(lines[cy]);

    if (ch != CTRL_KEY('q')) quit_armed = 0;

    switch (ch) {
        case CTRL_KEY('q'):
            if (modified && !quit_armed) {
                quit_armed = 1;
                set_status("Unsaved changes. Ctrl-Q again to quit.");
                return;
            }
            endwin();
            free_buffer();
            exit(0);

        case CTRL_KEY('s'): do_save(); break;
        case CTRL_KEY('o'): do_open(); break;
        case CTRL_KEY('f'): do_find(); break;
        case CTRL_KEY('g'): help_screen(); break;

        #ifdef KEY_F
        case KEY_F(3): find_next(); break;
        #endif

        case KEY_LEFT:
            if (cx > 0) cx--;
            else if (cy > 0) { cy--; cx = (int)strlen(lines[cy]); }
            break;

        case KEY_RIGHT:
            if (cx < len) cx++;
            else if (cy < nlines - 1) { cy++; cx = 0; }
            break;

        case KEY_UP:
            if (cy > 0) cy--;
            if (cx > (int)strlen(lines[cy])) cx = (int)strlen(lines[cy]);
            break;

        case KEY_DOWN:
            if (cy < nlines - 1) cy++;
            if (cx > (int)strlen(lines[cy])) cx = (int)strlen(lines[cy]);
            break;

        #ifdef KEY_HOME
        case KEY_HOME: cx = 0; break;
        #endif

        #ifdef KEY_END
        case KEY_END: cx = (int)strlen(lines[cy]); break;
        #endif

        #ifdef KEY_PPAGE
        case KEY_PPAGE:
            cy -= LINES - 4;
            if (cy < 0) cy = 0;
            if (cx > (int)strlen(lines[cy])) cx = (int)strlen(lines[cy]);
            break;
        #endif

        #ifdef KEY_NPAGE
        case KEY_NPAGE:
            cy += LINES - 4;
            if (cy >= nlines) cy = nlines - 1;
            if (cx > (int)strlen(lines[cy])) cx = (int)strlen(lines[cy]);
            break;
        #endif

        case KEY_BACKSPACE:
        case 127:
        case 8:
            delete_backward();
            break;

            #ifdef KEY_DC
        case KEY_DC:
            delete_forward();
            break;
            #endif

        case '\n':
        case '\r':
            insert_newline();
            break;

        case '\t':
            smart_tab();
            break;

        default:
            if (ch >= 32 && ch <= 126) insert_char(ch);
            break;
    }

    scroll_screen();
}

static void init_colors_if_possible(void)
{
    use_color = 0;

    #ifdef COLOR_BLACK
    if (has_colors()) {
        start_color();
        init_pair(CP_NORMAL,  COLOR_WHITE,   COLOR_BLACK);
        init_pair(CP_KEYWORD, COLOR_CYAN,    COLOR_BLACK);
        init_pair(CP_STRING,  COLOR_YELLOW,  COLOR_BLACK);
        init_pair(CP_COMMENT, COLOR_GREEN,   COLOR_BLACK);
        init_pair(CP_NUMBER,  COLOR_MAGENTA, COLOR_BLACK);
        init_pair(CP_PREPROC, COLOR_BLUE,    COLOR_BLACK);
        init_pair(CP_LINENO,  COLOR_CYAN,    COLOR_BLACK);
        init_pair(CP_STATUS,  COLOR_BLACK,   COLOR_CYAN);
        init_pair(CP_MENU,    COLOR_BLACK,   COLOR_WHITE);
        use_color = 1;
    }
    #endif
}

int main(int argc, char **argv)
{
    int ch;

    filename[0] = '\0';
    statusmsg[0] = '\0';
    last_search[0] = '\0';

    init_buffer();
    if (argc > 1) load_file(argv[1]);

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
