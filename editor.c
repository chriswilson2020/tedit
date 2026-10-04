#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curses.h>

#define MAX_LINES 10000
#define MAX_LINE  4096

static char *lines[MAX_LINES];
static int nlines = 1;

static int cy = 0;
static int cx = 0;
static int rowoff = 0;
static int coloff = 0;

static int modified = 0;
static char filename[1024];

static char statusmsg[1024];

static char *dupstr(const char *s)
{
    char *p;
    p = (char *)malloc(strlen(s) + 1);
    if (p != NULL)
        strcpy(p, s);
    return p;
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
    }
}

static int load_file(const char *name)
{
    FILE *fp;
    char buf[MAX_LINE];
    int len;

    fp = fopen(name, "r");

    if (fp == NULL) {
        strncpy(filename, name, sizeof(filename) - 1);
        filename[sizeof(filename) - 1] = '\0';
        sprintf(statusmsg, "New file: %s", filename);
        return 0;
    }

    free_buffer();

    nlines = 0;

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        len = strlen(buf);

        while (len > 0 &&
              (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[len - 1] = '\0';
            len--;
        }

        if (nlines < MAX_LINES)
            lines[nlines++] = dupstr(buf);
    }

    fclose(fp);

    if (nlines == 0) {
        lines[0] = dupstr("");
        nlines = 1;
    }

    strncpy(filename, name, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';

    modified = 0;
    cy = 0;
    cx = 0;

    sprintf(statusmsg, "Loaded %s", filename);

    return 0;
}

static int save_file(void)
{
    FILE *fp;
    int i;

    if (filename[0] == '\0') {
        strcpy(statusmsg, "No filename");
        return -1;
    }

    fp = fopen(filename, "w");

    if (fp == NULL) {
        sprintf(statusmsg, "Cannot write %s", filename);
        return -1;
    }

    for (i = 0; i < nlines; i++) {
        fputs(lines[i], fp);

        if (i < nlines - 1)
            fputc('\n', fp);
    }

    fclose(fp);

    modified = 0;
    sprintf(statusmsg, "Saved %s", filename);

    return 0;
}

static void insert_char(int ch)
{
    char *old;
    char *newp;
    int len;

    old = lines[cy];
    len = strlen(old);

    if (len >= MAX_LINE - 2)
        return;

    newp = (char *)malloc(len + 2);

    if (newp == NULL)
        return;

    memcpy(newp, old, cx);
    newp[cx] = (char)ch;
    strcpy(newp + cx + 1, old + cx);

    free(old);
    lines[cy] = newp;

    cx++;
    modified = 1;
}

static void delete_char(void)
{
    char *line;
    int len;

    line = lines[cy];
    len = strlen(line);

    if (cx > 0) {
        memmove(line + cx - 1,
                line + cx,
                len - cx + 1);

        cx--;
        modified = 1;
        return;
    }

    if (cy > 0) {
        char *prev;
        char *joined;
        int plen;
        int i;

        prev = lines[cy - 1];
        plen = strlen(prev);

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

    line = lines[cy];
    len = strlen(line);

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

    if (nlines >= MAX_LINES)
        return;

    line = lines[cy];
    len = strlen(line);

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

    free(line);

    for (i = nlines; i > cy + 1; i--)
        lines[i] = lines[i - 1];

    lines[cy] = left;
    lines[cy + 1] = right;

    nlines++;

    cy++;
    cx = 0;

    modified = 1;
}

static void scroll_screen(void)
{
    int textrows;
    int textcols;

    textrows = LINES - 2;
    textcols = COLS - 6;

    if (cy < rowoff)
        rowoff = cy;

    if (cy >= rowoff + textrows)
        rowoff = cy - textrows + 1;

    if (cx < coloff)
        coloff = cx;

    if (cx >= coloff + textcols)
        coloff = cx - textcols + 1;
}

static void draw_screen(void)
{
    int y;
    int filerow;
    int textrows;
    int maxtext;
    char tmp[64];
    char status[1024];
    int len;

    erase();

    textrows = LINES - 2;
    maxtext = COLS - 6;

    for (y = 0; y < textrows; y++) {

        filerow = rowoff + y;

        if (filerow >= nlines)
            break;

        sprintf(tmp, "%4d ", filerow + 1);

        attron(A_BOLD);
        mvaddstr(y, 0, tmp);
        attroff(A_BOLD);

        len = strlen(lines[filerow]) - coloff;

        if (len > 0) {
            if (len > maxtext)
                len = maxtext;

            mvaddnstr(y, 5,
                     lines[filerow] + coloff,
                     len);
        }
    }

    attron(A_REVERSE);

    sprintf(status,
            " %-30s %s  Ln %d/%d Col %d ",
            filename[0] ? filename : "[No Name]",
            modified ? "[modified]" : "",
            cy + 1,
            nlines,
            cx + 1);

    mvaddnstr(LINES - 2, 0, status, COLS);

    while ((int)strlen(status) < COLS) {
        addch(' ');
        strcat(status, " ");
    }

    attroff(A_REVERSE);

    mvaddstr(LINES - 1, 0,
             "^S Save  ^Q Quit  Arrows Move  PgUp/PgDn  ^G Help");

    if (statusmsg[0] != '\0') {
        int pos;
        pos = COLS - strlen(statusmsg) - 1;

        if (pos > 0)
            mvaddnstr(LINES - 1, pos, statusmsg,
                      COLS - pos);
    }

    scroll_screen();

    move(cy - rowoff,
         5 + cx - coloff);

    refresh();
}

static void help_screen(void)
{
    erase();

    mvaddstr(1, 2, "TEDIT - portable curses editor");
    mvaddstr(3, 2, "Ctrl-S       Save");
    mvaddstr(4, 2, "Ctrl-Q       Quit");
    mvaddstr(5, 2, "Arrow keys   Move");
    mvaddstr(6, 2, "Home / End   Start/end of line");
    mvaddstr(7, 2, "PgUp/PgDn    Scroll");
    mvaddstr(8, 2, "Backspace    Delete previous character");
    mvaddstr(9, 2, "Delete       Delete next character");
    mvaddstr(10, 2, "Enter        New line");

    mvaddstr(12, 2, "Press any key.");

    refresh();
    getch();
}

static void process_key(int ch)
{
    int len;

    len = strlen(lines[cy]);

    switch (ch) {

    case 17:                    /* Ctrl-Q */
        if (modified) {
            strcpy(statusmsg,
                   "Modified! Press Ctrl-Q again to quit");

            draw_screen();

            ch = getch();

            if (ch != 17)
                return;
        }

        endwin();
        free_buffer();
        exit(0);

    case 19:                    /* Ctrl-S */
        save_file();
        break;

    case 7:                     /* Ctrl-G */
        help_screen();
        break;

    case KEY_LEFT:
        if (cx > 0)
            cx--;
        else if (cy > 0) {
            cy--;
            cx = strlen(lines[cy]);
        }
        break;

    case KEY_RIGHT:
        if (cx < len)
            cx++;
        else if (cy < nlines - 1) {
            cy++;
            cx = 0;
        }
        break;

    case KEY_UP:
        if (cy > 0)
            cy--;

        if (cx > (int)strlen(lines[cy]))
            cx = strlen(lines[cy]);
        break;

    case KEY_DOWN:
        if (cy < nlines - 1)
            cy++;

        if (cx > (int)strlen(lines[cy]))
            cx = strlen(lines[cy]);
        break;

    case KEY_HOME:
        cx = 0;
        break;

    case KEY_END:
        cx = strlen(lines[cy]);
        break;

    case KEY_PPAGE:
        cy -= LINES - 3;

        if (cy < 0)
            cy = 0;

        if (cx > (int)strlen(lines[cy]))
            cx = strlen(lines[cy]);
        break;

    case KEY_NPAGE:
        cy += LINES - 3;

        if (cy >= nlines)
            cy = nlines - 1;

        if (cx > (int)strlen(lines[cy]))
            cx = strlen(lines[cy]);
        break;

    case KEY_BACKSPACE:
    case 127:
    case 8:
        delete_char();
        break;

    case KEY_DC:
        delete_forward();
        break;

    case '\n':
    case '\r':
        insert_newline();
        break;

    case '\t':
        insert_char(' ');
        insert_char(' ');
        insert_char(' ');
        insert_char(' ');
        break;

    default:
        if (ch >= 32 && ch <= 126)
            insert_char(ch);
        break;
    }

    scroll_screen();
}

int main(int argc, char **argv)
{
    int ch;

    filename[0] = '\0';
    statusmsg[0] = '\0';

    init_buffer();

    if (argc > 1)
        load_file(argv[1]);

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);

    for (;;) {
        draw_screen();
        ch = getch();
        process_key(ch);
    }

    return 0;
}
