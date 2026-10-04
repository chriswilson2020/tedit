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
#include <time.h>
#include "syntax.h"
#include "format.h"

extern FILE *popen(const char *command, const char *type);
extern int pclose(FILE *stream);

#ifndef TEDIT_SYSTEM_SYNTAX_DIR
#define TEDIT_SYSTEM_SYNTAX_DIR "/usr/local/share/tedit/syntax"
#endif

#define MAX_LINES   20000
#define MAX_LINE    8192
#define NAME_LEN    1024
#define STATUS_LEN  1024
#define SEARCH_LEN  256
#define CLIP_LEN    65536
#define UNDO_DEPTH  256
#define BROWSER_MAX  512
#define PATH_LEN     2048
#define MAX_BUFFERS  8
#define OUTPUT_MAX   256
#define OUTPUT_LINE  512
#define RECENT_MAX    16

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
    char **cap_lines;
    int cap_nlines;
    int cap_cy, cap_cx;
    int cap_rowoff, cap_coloff;
    int cap_modified;
    int valid;
} UndoCapture;

typedef struct {
    int start_row;
    char **old_lines;
    int old_count;
    char **new_lines;
    int new_count;
    int before_cy, before_cx;
    int before_rowoff, before_coloff;
    int after_cy, after_cx;
    int after_rowoff, after_coloff;
    int before_modified;
    int after_modified;
} EditOp;

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
    ACT_COLUMN_SELECT,
    ACT_TABS_TO_SPACES,
    ACT_SPACES_TO_TABS,
    ACT_SORT_LINES,
    ACT_TRANSPOSE_CHARS,
    ACT_REPEAT_LAST,
    ACT_MATCH_BRACKET,
    ACT_BOOKMARK_TOGGLE,
    ACT_BOOKMARK_NEXT,
    ACT_PROJECT_ROOT,
    ACT_PROJECT_OPEN,
    ACT_BUILD,
    ACT_CLEAN,
    ACT_RUN,
    ACT_FIND_FILES,
    ACT_NEXT_RESULT,
    ACT_TOGGLE_OUTPUT,
    ACT_SPLIT_TOGGLE,
    ACT_SPLIT_SWITCH,
    ACT_SYMBOLS_TOGGLE,
    ACT_SYMBOL_LIST,
    ACT_NEXT_SYMBOL,
    ACT_PREV_SYMBOL,
    ACT_OPEN_RECENT,
    ACT_RECOVER,
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
static int column_selecting = 0;
static int sel_sy = 0, sel_sx = 0;
static int last_repeat_action = ACT_NONE;

static char statusmsg[STATUS_LEN];
static char last_search[SEARCH_LEN];

static char *clipboard = NULL;

static char project_root[PATH_LEN];
static char project_build[256];
static char project_clean[256];
static char project_run[256];

static char output_lines[OUTPUT_MAX][OUTPUT_LINE];
static int output_count = 0;
static int output_visible = 0;
static int output_cursor = 0;

static int split_enabled = 0;
static int split_other = -1;
static int symbol_sidebar_enabled = 0;

static int session_enabled = 1;
static int recovery_enabled = 1;
static int backup_enabled = 1;
static int recovery_interval = 30;
static time_t last_recovery_time = 0;

static char recent_files[RECENT_MAX][PATH_LEN];
static int recent_count = 0;

static EditOp undo_stack[UNDO_DEPTH];
static int undo_count = 0;
static EditOp redo_stack[UNDO_DEPTH];
static int redo_count = 0;
static UndoCapture pending_undo;


static const MenuItem file_menu[] = {
    {"New buffer     ^N", ACT_NEW_BUFFER},
    {"Previous buf   ^P", ACT_PREV_BUFFER},
    {"Next buffer    ^E", ACT_NEXT_BUFFER},
    {"----------------", ACT_NONE},
    {"Open...        ^O", ACT_OPEN},
    {"Open recent...", ACT_OPEN_RECENT},
    {"Recover autosave...", ACT_RECOVER},
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
    {"Select all", ACT_SELECT_ALL},
    {"Column select", ACT_COLUMN_SELECT},
    {"----------------", ACT_NONE},
    {"Tabs -> spaces", ACT_TABS_TO_SPACES},
    {"Spaces -> tabs", ACT_SPACES_TO_TABS},
    {"Sort selected lines", ACT_SORT_LINES},
    {"Transpose chars", ACT_TRANSPOSE_CHARS},
    {"Repeat last edit", ACT_REPEAT_LAST}
};

static const MenuItem search_menu[] = {
    {"Find...        ^F", ACT_FIND},
    {"Replace...     ^H", ACT_REPLACE},
    {"Goto line...   ^L", ACT_GOTO},
    {"Matching bracket", ACT_MATCH_BRACKET},
    {"Toggle bookmark", ACT_BOOKMARK_TOGGLE},
    {"Next bookmark", ACT_BOOKMARK_NEXT}
};

static const MenuItem project_menu[] = {
    {"Detect project root", ACT_PROJECT_ROOT},
    {"Open project file...", ACT_PROJECT_OPEN},
    {"Build", ACT_BUILD},
    {"Clean", ACT_CLEAN},
    {"Run", ACT_RUN},
    {"Find in files...", ACT_FIND_FILES},
    {"Next result/error", ACT_NEXT_RESULT},
    {"Toggle output pane", ACT_TOGGLE_OUTPUT}
};

static const MenuItem view_menu[] = {
    {"Toggle split", ACT_SPLIT_TOGGLE},
    {"Switch split pane", ACT_SPLIT_SWITCH},
    {"Toggle symbols", ACT_SYMBOLS_TOGGLE},
    {"Symbol list...", ACT_SYMBOL_LIST},
    {"Next symbol", ACT_NEXT_SYMBOL},
    {"Previous symbol", ACT_PREV_SYMBOL},
    {"Toggle output pane", ACT_TOGGLE_OUTPUT}
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
    "File", "Edit", "Search", "Project", "View", "Options", "Help"
};

#define MENU_COUNT 7


static char *dupstr(const char *s)
{
    char *p;
    p = (char *)malloc(strlen(s) + 1);
    if (p != NULL)
        strcpy(p, s);
    return p;
}

static int prompt_input(const char *prompt, char *out, int outlen);
static void path_parent(char *path);
static int file_browser(char *out, int outlen);
static int load_file(const char *name);
static int path_exists(const char *path);
static int create_new_buffer(void);
static void scroll_screen(void);
static void reset_edit_history(void);
static void finalize_pending_undo(void);
static void free_capture(UndoCapture *cap);
static void clear_stack(EditOp *stack, int *count);
static int confirm_yes_no(const char *message);
static void compute_bracket_match(void);

static void set_status(const char *s)
{
    strncpy(statusmsg, s, STATUS_LEN - 1);
    statusmsg[STATUS_LEN - 1] = '\0';
}



static void ensure_user_tedit_dirs(void)
{
    char path[PATH_LEN];
    char *home;

    home = getenv("HOME");
    if (home == NULL)
        return;

    sprintf(path, "%s/.tedit", home);
    mkdir(path, 0700);

    sprintf(path, "%s/.tedit/recovery", home);
    mkdir(path, 0700);
}

static void recent_save(void)
{
    char path[PATH_LEN];
    char *home;
    FILE *fp;
    int i;

    home = getenv("HOME");
    if (home == NULL)
        return;

    ensure_user_tedit_dirs();
    sprintf(path, "%s/.tedit/recent", home);

    fp = fopen(path, "w");
    if (fp == NULL)
        return;

    for (i = 0; i < recent_count; i++)
        fprintf(fp, "%s\n", recent_files[i]);

    fclose(fp);
}

static void recent_load(void)
{
    char path[PATH_LEN];
    char line[PATH_LEN];
    char *home;
    FILE *fp;

    recent_count = 0;
    home = getenv("HOME");
    if (home == NULL)
        return;

    sprintf(path, "%s/.tedit/recent", home);
    fp = fopen(path, "r");
    if (fp == NULL)
        return;

    while (recent_count < RECENT_MAX &&
           fgets(line, sizeof(line), fp) != NULL) {
        int len;

        len = (int)strlen(line);
        while (len > 0 &&
              (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (line[0] != '\0') {
            strncpy(recent_files[recent_count], line, PATH_LEN - 1);
            recent_files[recent_count][PATH_LEN - 1] = '\0';
            recent_count++;
        }
    }

    fclose(fp);
}

static void recent_add(const char *name)
{
    char full[PATH_LEN];
    char cwd[PATH_LEN];
    int i;
    int found;

    if (name == NULL || name[0] == '\0')
        return;

    if (name[0] == '/') {
        strncpy(full, name, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
    } else if (getcwd(cwd, sizeof(cwd)) != NULL) {
        sprintf(full, "%s/%s", cwd, name);
    } else {
        strncpy(full, name, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
    }

    found = -1;
    for (i = 0; i < recent_count; i++) {
        if (!strcmp(recent_files[i], full)) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        for (i = found; i > 0; i--)
            strcpy(recent_files[i], recent_files[i - 1]);
    } else {
        if (recent_count < RECENT_MAX)
            recent_count++;
        for (i = recent_count - 1; i > 0; i--)
            strcpy(recent_files[i], recent_files[i - 1]);
    }

    strncpy(recent_files[0], full, PATH_LEN - 1);
    recent_files[0][PATH_LEN - 1] = '\0';
    recent_save();
}

static int recent_dialog(char *out, int outlen)
{
    int selected;
    int ch;
    int i;

    if (recent_count <= 0) {
        set_status("No recent files");
        return 0;
    }

    selected = 0;

    for (;;) {
        erase();
        attron(A_REVERSE);
        mvaddstr(0, 0, " TEDIT Recent Files ");
        clrtoeol();
        attroff(A_REVERSE);

        for (i = 0; i < recent_count && i < LINES - 4; i++) {
            if (i == selected)
                attron(A_REVERSE);

            mvprintw(i + 2, 2, "%2d  ", i + 1);
            addnstr(recent_files[i], COLS - 8);

            if (i == selected)
                attroff(A_REVERSE);
        }

        mvaddstr(LINES - 1, 0, "Enter open  Up/Down move  Esc cancel");
        refresh();

        ch = getch();

        if (ch == 27)
            return 0;
        if (ch == KEY_UP && selected > 0)
            selected--;
        else if (ch == KEY_DOWN && selected + 1 < recent_count)
            selected++;
        else if (ch == '\n' || ch == '\r') {
            strncpy(out, recent_files[selected], outlen - 1);
            out[outlen - 1] = '\0';
            return 1;
        }
    }
}

static void do_open_recent(void)
{
    char path[PATH_LEN];

    if (modified) {
        set_status("Unsaved changes: save or use a new buffer first");
        return;
    }

    if (recent_dialog(path, sizeof(path)))
        load_file(path);
}

static void backup_existing_file(const char *name)
{
    char backup[PATH_LEN];
    FILE *in;
    FILE *out;
    char buf[4096];
    size_t n;

    if (!backup_enabled || !path_exists(name))
        return;

    sprintf(backup, "%s~", name);

    in = fopen(name, "rb");
    if (in == NULL)
        return;

    out = fopen(backup, "wb");
    if (out == NULL) {
        fclose(in);
        return;
    }

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);

    fclose(out);
    fclose(in);
}

static void write_recovery_files(void)
{
    char *home;
    int bi;

    if (!recovery_enabled)
        return;

    home = getenv("HOME");
    if (home == NULL)
        return;

    ensure_user_tedit_dirs();

    for (bi = 0; bi < buffer_count; bi++) {
        Buffer *b;
        char path[PATH_LEN];
        char meta[PATH_LEN];
        FILE *fp;
        int row;

        b = &buffers[bi];

        if (!b->dirty)
            continue;

        sprintf(path, "%s/.tedit/recovery/buffer%d.tmp", home, bi);
        fp = fopen(path, "w");
        if (fp != NULL) {
            for (row = 0; row < b->line_count; row++) {
                fputs(b->linev[row], fp);
                if (row < b->line_count - 1)
                    fputc('\n', fp);
            }
            fclose(fp);
        }

        sprintf(meta, "%s/.tedit/recovery/buffer%d.meta", home, bi);
        fp = fopen(meta, "w");
        if (fp != NULL) {
            fprintf(fp, "%s\n", b->fname[0] ? b->fname : "[No Name]");
            fclose(fp);
        }
    }
}

static void clear_recovery_files(void)
{
    char *home;
    int bi;

    home = getenv("HOME");
    if (home == NULL)
        return;

    for (bi = 0; bi < MAX_BUFFERS; bi++) {
        char path[PATH_LEN];

        sprintf(path, "%s/.tedit/recovery/buffer%d.tmp", home, bi);
        unlink(path);
        sprintf(path, "%s/.tedit/recovery/buffer%d.meta", home, bi);
        unlink(path);
    }
}

static void maybe_write_recovery(void)
{
    time_t now;

    if (!recovery_enabled)
        return;

    now = time(NULL);

    if (last_recovery_time == 0 ||
        now - last_recovery_time >= recovery_interval) {
        write_recovery_files();
        last_recovery_time = now;
    }
}

static void do_recover(void)
{
    char *home;
    char oldcwd[PATH_LEN];
    char dir[PATH_LEN];
    char path[PATH_LEN];

    home = getenv("HOME");
    if (home == NULL) {
        set_status("HOME is not set");
        return;
    }

    ensure_user_tedit_dirs();
    sprintf(dir, "%s/.tedit/recovery", home);

    if (getcwd(oldcwd, sizeof(oldcwd)) == NULL)
        oldcwd[0] = '\0';

    if (chdir(dir) != 0) {
        set_status("No recovery directory");
        return;
    }

    if (file_browser(path, sizeof(path))) {
        load_file(path);
        modified = 1;
        set_status("Recovered autosave; use Save As");
    } else {
        set_status("Recovery cancelled");
    }

    if (oldcwd[0] != '\0')
        chdir(oldcwd);
}

static void save_session(void)
{
    char *home;
    char path[PATH_LEN];
    FILE *fp;
    int bi;

    if (!session_enabled)
        return;

    home = getenv("HOME");
    if (home == NULL)
        return;

    ensure_user_tedit_dirs();
    sprintf(path, "%s/.tedit/session", home);

    fp = fopen(path, "w");
    if (fp == NULL)
        return;

    fprintf(fp, "current=%d\n", curbuf);

    for (bi = 0; bi < buffer_count; bi++) {
        if (buffers[bi].fname[0] != '\0')
            fprintf(fp, "file=%s\t%d\t%d\n",
                    buffers[bi].fname,
                    buffers[bi].cursor_y,
                    buffers[bi].cursor_x);
    }

    fclose(fp);
}

static void restore_session(void)
{
    char *home;
    char path[PATH_LEN];
    char line[PATH_LEN + 64];
    FILE *fp;
    int loaded;

    if (!session_enabled)
        return;

    home = getenv("HOME");
    if (home == NULL)
        return;

    sprintf(path, "%s/.tedit/session", home);
    fp = fopen(path, "r");
    if (fp == NULL)
        return;

    loaded = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (!strncmp(line, "file=", 5)) {
            char *name;
            char *t1;
            char *t2;
            int sy;
            int sx;

            name = line + 5;
            t1 = strchr(name, '\t');
            if (t1 == NULL)
                continue;
            *t1++ = '\0';

            t2 = strchr(t1, '\t');
            if (t2 == NULL)
                continue;
            *t2++ = '\0';

            sy = atoi(t1);
            sx = atoi(t2);

            {
                int len;
                len = (int)strlen(t2);
                while (len > 0 &&
                      (t2[len - 1] == '\n' || t2[len - 1] == '\r'))
                    t2[--len] = '\0';
            }

            if (!path_exists(name))
                continue;

            if (loaded > 0) {
                if (!create_new_buffer())
                    break;
            }

            load_file(name);
            cy = sy;
            if (cy < 0) cy = 0;
            if (cy >= nlines) cy = nlines - 1;
            cx = sx;
            if (cx < 0) cx = 0;
            if (cx > (int)strlen(lines[cy]))
                cx = (int)strlen(lines[cy]);

            loaded++;
        }
    }

    fclose(fp);

    if (loaded > 0) {
        curbuf = 0;
        set_status("Session restored");
    }
}

static int output_pane_height(void)
{
    int h;

    if (!output_visible || output_count <= 0)
        return 0;

    h = LINES / 3;
    if (h < 4)
        h = 4;
    if (h > 10)
        h = 10;
    if (h > LINES - 6)
        h = LINES - 6;

    return h > 0 ? h : 0;
}


static int editor_total_text_rows(void)
{
    int rows;

    rows = LINES - 3 - output_pane_height();
    if (rows < 1)
        rows = 1;

    return rows;
}

static int editor_active_text_rows(void)
{
    int rows;

    rows = editor_total_text_rows();

    if (split_enabled && buffer_count > 1) {
        rows = (rows - 1) / 2;
        if (rows < 1)
            rows = 1;
    }

    return rows;
}

static int symbol_sidebar_width(void)
{
    if (!symbol_sidebar_enabled || COLS < 72)
        return 0;
    return 30;
}

static int editor_text_columns(int gutter)
{
    int cols;

    cols = COLS - gutter - 1 - symbol_sidebar_width();
    if (cols < 1)
        cols = 1;

    return cols;
}

static int line_looks_like_symbol(const char *line)
{
    const char *p;
    char word[32];
    int wi;

    p = line;
    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '\0' || *p == '#' || *p == ';')
        return 0;

    wi = 0;
    while (*p && wi < (int)sizeof(word) - 1 &&
          (isalnum((unsigned char)*p) || *p == '_')) {
        word[wi++] = (char)tolower((unsigned char)*p);
        p++;
    }
    word[wi] = '\0';

    if (!strcmp(word, "def") || !strcmp(word, "class") ||
        !strcmp(word, "fn") || !strcmp(word, "func") ||
        !strcmp(word, "function") || !strcmp(word, "subroutine") ||
        !strcmp(word, "program") || !strcmp(word, "module") ||
        !strcmp(word, "procedure") || !strcmp(word, "package") ||
        !strcmp(word, "interface") || !strcmp(word, "struct") ||
        !strcmp(word, "enum") || !strcmp(word, "type"))
        return 1;

    /*
     * Conservative C/C++ style function heuristic: a line containing a
     * parameter list, but not an ordinary control statement.
     */
    if (strchr(line, '(') != NULL && strchr(line, ')') != NULL) {
        if (strcmp(word, "if") && strcmp(word, "for") &&
            strcmp(word, "while") && strcmp(word, "switch") &&
            strcmp(word, "return") && strcmp(word, "sizeof"))
            return 1;
    }

    return 0;
}

static int collect_symbols(int *rows, char names[][80], int max_symbols)
{
    int row;
    int count;

    count = 0;

    for (row = 0; row < nlines && count < max_symbols; row++) {
        const char *p;

        if (!line_looks_like_symbol(lines[row]))
            continue;

        p = lines[row];
        while (*p == ' ' || *p == '\t')
            p++;

        rows[count] = row;
        strncpy(names[count], p, 79);
        names[count][79] = '\0';
        count++;
    }

    return count;
}

static void draw_symbol_sidebar(int start_y, int rows)
{
    int symbol_rows[128];
    char names[128][80];
    int count;
    int i;
    int width;
    int x;

    width = symbol_sidebar_width();
    if (width <= 0 || rows <= 0)
        return;

    x = COLS - width;
    count = collect_symbols(symbol_rows, names, 128);

    attron(A_REVERSE);
    mvaddstr(start_y, x, " Symbols ");
    {
        int k;
        for (k = 9; k < width; k++)
            addch(' ');
    }
    attroff(A_REVERSE);

    for (i = 1; i < rows; i++) {
        int si;
        char linebuf[96];

        move(start_y + i, x);
        {
            int k;
            for (k = 0; k < width; k++)
                addch(' ');
        }

        si = i - 1;
        if (si >= count)
            continue;

        sprintf(linebuf, "%4d %.22s", symbol_rows[si] + 1, names[si]);
        mvaddnstr(start_y + i, x, linebuf, width - 1);
    }
}

static void symbol_list_dialog(void)
{
    int symbol_rows[128];
    char names[128][80];
    int count;
    int selected;
    int top;
    int ch;

    count = collect_symbols(symbol_rows, names, 128);
    if (count <= 0) {
        set_status("No symbols found");
        return;
    }

    selected = 0;
    top = 0;

    for (;;) {
        int visible;
        int i;

        visible = LINES - 4;
        if (visible < 1)
            visible = 1;

        if (selected < top)
            top = selected;
        if (selected >= top + visible)
            top = selected - visible + 1;

        erase();
        attron(A_REVERSE);
        mvaddstr(0, 0, " TEDIT Symbols ");
        clrtoeol();
        attroff(A_REVERSE);

        for (i = 0; i < visible; i++) {
            int si;
            char rowbuf[96];

            si = top + i;
            if (si >= count)
                break;

            sprintf(rowbuf, "%5d  %s", symbol_rows[si] + 1, names[si]);

            if (si == selected)
                attron(A_REVERSE);
            mvaddnstr(i + 2, 2, rowbuf, COLS - 4);
            if (si == selected)
                attroff(A_REVERSE);
        }

        mvaddstr(LINES - 1, 0, "Enter jump  Up/Down move  Esc cancel");
        refresh();
        ch = getch();

        if (ch == 27)
            return;
        if (ch == KEY_UP && selected > 0)
            selected--;
        else if (ch == KEY_DOWN && selected + 1 < count)
            selected++;
        else if (ch == '\n' || ch == '\r') {
            cy = symbol_rows[selected];
            cx = 0;
            scroll_screen();
            set_status("Jumped to symbol");
            return;
        }
    }
}

static void jump_symbol(int dir)
{
    int symbol_rows[128];
    char names[128][80];
    int count;
    int i;

    count = collect_symbols(symbol_rows, names, 128);
    if (count <= 0) {
        set_status("No symbols found");
        return;
    }

    if (dir > 0) {
        for (i = 0; i < count; i++) {
            if (symbol_rows[i] > cy) {
                cy = symbol_rows[i];
                cx = 0;
                scroll_screen();
                set_status("Next symbol");
                return;
            }
        }
        cy = symbol_rows[0];
    } else {
        for (i = count - 1; i >= 0; i--) {
            if (symbol_rows[i] < cy) {
                cy = symbol_rows[i];
                cx = 0;
                scroll_screen();
                set_status("Previous symbol");
                return;
            }
        }
        cy = symbol_rows[count - 1];
    }

    cx = 0;
    scroll_screen();
    set_status(dir > 0 ? "Next symbol" : "Previous symbol");
}

static void toggle_split(void)
{
    if (buffer_count < 2) {
        set_status("Open at least two buffers to split");
        return;
    }

    split_enabled = !split_enabled;

    if (split_enabled) {
        split_other = (curbuf + 1) % buffer_count;
        set_status("Split view enabled");
    } else {
        split_other = -1;
        set_status("Split view disabled");
    }

    scroll_screen();
}

static void switch_split_pane(void)
{
    int tmp;

    if (!split_enabled || split_other < 0 ||
        split_other >= buffer_count || split_other == curbuf) {
        set_status("Split view is not active");
        return;
    }

    tmp = curbuf;
    curbuf = split_other;
    split_other = tmp;
    reset_edit_history();
    selecting = 0;
    scroll_screen();
    set_status("Switched split pane");
}

static void output_clear(void)
{
    output_count = 0;
    output_cursor = 0;
}

static void output_add(const char *s)
{
    int len;

    if (output_count >= OUTPUT_MAX)
        return;

    strncpy(output_lines[output_count], s, OUTPUT_LINE - 1);
    output_lines[output_count][OUTPUT_LINE - 1] = '\0';

    len = (int)strlen(output_lines[output_count]);
    while (len > 0 &&
          (output_lines[output_count][len - 1] == '\n' ||
           output_lines[output_count][len - 1] == '\r')) {
        output_lines[output_count][--len] = '\0';
    }

    output_count++;
}

static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int project_marker_exists(const char *dir)
{
    char p[PATH_LEN];

    sprintf(p, "%s/.tedit-project", dir);
    if (path_exists(p)) return 1;
    sprintf(p, "%s/.git", dir);
    if (path_exists(p)) return 1;
    sprintf(p, "%s/Makefile", dir);
    if (path_exists(p)) return 1;
    sprintf(p, "%s/GNUmakefile", dir);
    if (path_exists(p)) return 1;
    sprintf(p, "%s/configure", dir);
    if (path_exists(p)) return 1;
    sprintf(p, "%s/CMakeLists.txt", dir);
    if (path_exists(p)) return 1;

    return 0;
}

static void load_project_commands(void)
{
    char cfg[PATH_LEN];
    char line[512];
    FILE *fp;

    strcpy(project_build, "make");
    strcpy(project_clean, "make clean");
    strcpy(project_run, "./a.out");

    if (project_root[0] == '\0')
        return;

    sprintf(cfg, "%s/.tedit-project", project_root);
    fp = fopen(cfg, "r");
    if (fp == NULL)
        return;

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *nl;

        nl = strchr(line, '\n');
        if (nl != NULL)
            *nl = '\0';

        if (!strncmp(line, "build=", 6)) {
            strncpy(project_build, line + 6, sizeof(project_build) - 1);
            project_build[sizeof(project_build) - 1] = '\0';
        } else if (!strncmp(line, "clean=", 6)) {
            strncpy(project_clean, line + 6, sizeof(project_clean) - 1);
            project_clean[sizeof(project_clean) - 1] = '\0';
        } else if (!strncmp(line, "run=", 4)) {
            strncpy(project_run, line + 4, sizeof(project_run) - 1);
            project_run[sizeof(project_run) - 1] = '\0';
        }
    }

    fclose(fp);
}

static int detect_project_root(void)
{
    char path[PATH_LEN];

    if (filename[0] != '\0') {
        strncpy(path, filename, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';

        if (path[0] != '/') {
            char cwd[PATH_LEN];
            char joined[PATH_LEN];

            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                sprintf(joined, "%s/%s", cwd, path);
                strncpy(path, joined, sizeof(path) - 1);
                path[sizeof(path) - 1] = '\0';
            }
        }

        path_parent(path);
    } else if (getcwd(path, sizeof(path)) == NULL) {
        strcpy(path, ".");
    }

    for (;;) {
        char old[PATH_LEN];

        if (project_marker_exists(path)) {
            strncpy(project_root, path, sizeof(project_root) - 1);
            project_root[sizeof(project_root) - 1] = '\0';
            load_project_commands();
            sprintf(statusmsg, "Project: %s", project_root);
            return 1;
        }

        strcpy(old, path);
        path_parent(path);

        if (!strcmp(old, path))
            break;
    }

    project_root[0] = '\0';
    set_status("No project root found");
    return 0;
}

static int ensure_project(void)
{
    if (project_root[0] != '\0')
        return 1;
    return detect_project_root();
}

static void run_project_command(const char *cmd, const char *label)
{
    FILE *fp;
    char shellcmd[PATH_LEN + 512];
    char line[OUTPUT_LINE];
    char oldcwd[PATH_LEN];
    int status;

    if (!ensure_project())
        return;

    if (getcwd(oldcwd, sizeof(oldcwd)) == NULL)
        oldcwd[0] = '\0';

    if (chdir(project_root) != 0) {
        set_status("Cannot enter project directory");
        return;
    }

    sprintf(shellcmd, "%s 2>&1", cmd);
    fp = popen(shellcmd, "r");

    output_clear();

    if (fp == NULL) {
        output_add("Unable to start command.");
        output_visible = 1;
        if (oldcwd[0] != '\0')
            chdir(oldcwd);
        return;
    }

    sprintf(line, "$ %s", cmd);
    output_add(line);

    while (fgets(line, sizeof(line), fp) != NULL)
        output_add(line);

    status = pclose(fp);

    sprintf(line, "[%s finished: status %d]", label, status);
    output_add(line);
    output_visible = 1;
    output_cursor = 0;

    if (oldcwd[0] != '\0')
        chdir(oldcwd);

    sprintf(statusmsg, "%s finished", label);
}

static int find_in_tree(const char *dir, const char *needle, int depth)
{
    DIR *dp;
    struct dirent *de;

    if (depth > 24 || output_count >= OUTPUT_MAX)
        return 0;

    dp = opendir(dir);
    if (dp == NULL)
        return 0;

    while ((de = readdir(dp)) != NULL && output_count < OUTPUT_MAX) {
        char path[PATH_LEN];
        struct stat st;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") ||
            !strcmp(de->d_name, ".git") || !strcmp(de->d_name, ".svn") ||
            !strcmp(de->d_name, "node_modules"))
            continue;

        sprintf(path, "%s/%s", dir, de->d_name);

        if (stat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            find_in_tree(path, needle, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            FILE *fp;
            char line[OUTPUT_LINE];
            int lineno;

            fp = fopen(path, "r");
            if (fp == NULL)
                continue;

            lineno = 0;
            while (fgets(line, sizeof(line), fp) != NULL &&
                   output_count < OUTPUT_MAX) {
                lineno++;

                if (strstr(line, needle) != NULL) {
                    char result[OUTPUT_LINE];
                    char *rel;

                    rel = path;
                    if (project_root[0] != '\0' &&
                        !strncmp(path, project_root, strlen(project_root))) {
                        rel = path + strlen(project_root);
                        if (*rel == '/')
                            rel++;
                    }

                    sprintf(result, "%s:%d:%s", rel, lineno, line);
                    output_add(result);
                }
            }

            fclose(fp);
        }
    }

    closedir(dp);
    return output_count;
}

static void do_find_in_files(void)
{
    char query[SEARCH_LEN];

    if (!ensure_project())
        return;

    if (!prompt_input("Find in project: ", query, sizeof(query)) || !query[0]) {
        set_status("Find in files cancelled");
        return;
    }

    output_clear();
    find_in_tree(project_root, query, 0);
    output_visible = 1;
    output_cursor = 0;

    sprintf(statusmsg, "%d project match(es)", output_count);
}

static int jump_to_output_location(void)
{
    int attempts;

    if (output_count <= 0) {
        set_status("No output results");
        return 0;
    }

    for (attempts = 0; attempts < output_count; attempts++) {
        char buf[OUTPUT_LINE];
        char *p1;
        char *p2;
        int line;
        char full[PATH_LEN];

        if (output_cursor >= output_count)
            output_cursor = 0;

        strncpy(buf, output_lines[output_cursor], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        output_cursor++;

        p1 = strchr(buf, ':');
        if (p1 == NULL)
            continue;
        *p1++ = '\0';

        p2 = strchr(p1, ':');
        if (p2 == NULL)
            continue;
        *p2 = '\0';

        line = atoi(p1);
        if (line <= 0)
            continue;

        if (buf[0] == '/') {
            strncpy(full, buf, sizeof(full) - 1);
            full[sizeof(full) - 1] = '\0';
        } else if (project_root[0] != '\0') {
            sprintf(full, "%s/%s", project_root, buf);
        } else {
            strncpy(full, buf, sizeof(full) - 1);
            full[sizeof(full) - 1] = '\0';
        }

        if (path_exists(full)) {
            load_file(full);
            cy = line - 1;
            if (cy < 0) cy = 0;
            if (cy >= nlines) cy = nlines - 1;
            cx = 0;
            scroll_screen();
            sprintf(statusmsg, "%s:%d", buf, line);
            return 1;
        }
    }

    set_status("No navigable file:line result");
    return 0;
}

static void do_project_open(void)
{
    char oldcwd[PATH_LEN];
    char name[PATH_LEN];

    if (!ensure_project())
        return;

    if (getcwd(oldcwd, sizeof(oldcwd)) == NULL)
        oldcwd[0] = '\0';

    if (chdir(project_root) != 0) {
        set_status("Cannot enter project directory");
        return;
    }

    if (file_browser(name, sizeof(name)))
        load_file(name);
    else
        set_status("Open cancelled");

    if (oldcwd[0] != '\0')
        chdir(oldcwd);
}


static int prompt_input(const char *prompt, char *out, int outlen);
static void path_parent(char *path);
static int file_browser(char *out, int outlen);
static int load_file(const char *name);
static void clear_stack(EditOp *stack, int *count);
static void finalize_pending_undo(void);
static int confirm_yes_no(const char *message);
static void scroll_screen(void);
static void compute_bracket_match(void);
static int bracket_match_row = -1;
static int bracket_match_col = -1;

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
    finalize_pending_undo();
    free_capture(&pending_undo);
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);
    selecting = 0;
    column_selecting = 0;
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

    split_enabled = 0;
    split_other = -1;

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


static void free_line_vector(char **v, int count)
{
    int i;

    if (v == NULL)
        return;

    for (i = 0; i < count; i++) {
        if (v[i] != NULL)
            free(v[i]);
    }

    free(v);
}

static void free_capture(UndoCapture *cap)
{
    if (cap == NULL || !cap->valid)
        return;

    free_line_vector(cap->cap_lines, cap->cap_nlines);
    memset(cap, 0, sizeof(*cap));
}

static int capture_current(UndoCapture *cap)
{
    int i;

    memset(cap, 0, sizeof(*cap));

    cap->cap_lines = (char **)malloc(sizeof(char *) * nlines);
    if (cap->cap_lines == NULL)
        return -1;

    cap->cap_nlines = nlines;
    cap->cap_cy = cy;
    cap->cap_cx = cx;
    cap->cap_rowoff = rowoff;
    cap->cap_coloff = coloff;
    cap->cap_modified = modified;

    for (i = 0; i < nlines; i++) {
        cap->cap_lines[i] = dupstr(lines[i]);

        if (cap->cap_lines[i] == NULL) {
            free_line_vector(cap->cap_lines, i);
            memset(cap, 0, sizeof(*cap));
            return -1;
        }
    }

    cap->valid = 1;
    return 0;
}

static void free_edit_op(EditOp *op)
{
    if (op == NULL)
        return;

    free_line_vector(op->old_lines, op->old_count);
    free_line_vector(op->new_lines, op->new_count);
    memset(op, 0, sizeof(*op));
}

static void clear_stack(EditOp *stack, int *count)
{
    int i;

    for (i = 0; i < *count; i++)
        free_edit_op(&stack[i]);

    *count = 0;
}

static char **copy_line_range(char **src, int start, int count)
{
    char **v;
    int i;

    if (count <= 0)
        return NULL;

    v = (char **)malloc(sizeof(char *) * count);
    if (v == NULL)
        return NULL;

    for (i = 0; i < count; i++) {
        v[i] = dupstr(src[start + i]);

        if (v[i] == NULL) {
            free_line_vector(v, i);
            return NULL;
        }
    }

    return v;
}

static void push_edit_op(EditOp *op)
{
    int i;

    if (op->old_count == 0 && op->new_count == 0) {
        free_edit_op(op);
        return;
    }

    if (undo_count == UNDO_DEPTH) {
        free_edit_op(&undo_stack[0]);

        for (i = 1; i < UNDO_DEPTH; i++)
            undo_stack[i - 1] = undo_stack[i];

        memset(&undo_stack[UNDO_DEPTH - 1], 0, sizeof(EditOp));
        undo_count--;
    }

    undo_stack[undo_count++] = *op;
    memset(op, 0, sizeof(*op));
}

static void finalize_pending_undo(void)
{
    EditOp op;
    int prefix;
    int suffix;
    int old_remaining;
    int new_remaining;

    if (!pending_undo.valid)
        return;

    prefix = 0;

    while (prefix < pending_undo.cap_nlines &&
           prefix < nlines &&
           !strcmp(pending_undo.cap_lines[prefix], lines[prefix]))
        prefix++;

    suffix = 0;
    old_remaining = pending_undo.cap_nlines - prefix;
    new_remaining = nlines - prefix;

    while (suffix < old_remaining &&
           suffix < new_remaining &&
           !strcmp(pending_undo.cap_lines[pending_undo.cap_nlines - 1 - suffix],
                   lines[nlines - 1 - suffix]))
        suffix++;

    memset(&op, 0, sizeof(op));
    op.start_row = prefix;
    op.old_count = pending_undo.cap_nlines - prefix - suffix;
    op.new_count = nlines - prefix - suffix;

    if (op.old_count == 0 && op.new_count == 0) {
        free_capture(&pending_undo);
        return;
    }

    op.old_lines = copy_line_range(pending_undo.cap_lines,
                                   prefix, op.old_count);
    op.new_lines = copy_line_range(lines, prefix, op.new_count);

    if ((op.old_count > 0 && op.old_lines == NULL) ||
        (op.new_count > 0 && op.new_lines == NULL)) {
        free_edit_op(&op);
        free_capture(&pending_undo);
        return;
    }

    op.before_cy = pending_undo.cap_cy;
    op.before_cx = pending_undo.cap_cx;
    op.before_rowoff = pending_undo.cap_rowoff;
    op.before_coloff = pending_undo.cap_coloff;
    op.before_modified = pending_undo.cap_modified;

    op.after_cy = cy;
    op.after_cx = cx;
    op.after_rowoff = rowoff;
    op.after_coloff = coloff;
    op.after_modified = modified;

    push_edit_op(&op);
    free_capture(&pending_undo);
}

static void replace_line_patch(int start, int remove_count,
                               char **add_lines, int add_count)
{
    int delta;
    int tail_start;
    int i;

    if (start < 0)
        start = 0;
    if (start > nlines)
        start = nlines;

    if (remove_count < 0)
        remove_count = 0;
    if (start + remove_count > nlines)
        remove_count = nlines - start;

    for (i = start; i < start + remove_count; i++) {
        if (lines[i] != NULL)
            free(lines[i]);
        lines[i] = NULL;
    }

    delta = add_count - remove_count;
    tail_start = start + remove_count;

    if (delta > 0) {
        for (i = nlines - 1; i >= tail_start; i--)
            lines[i + delta] = lines[i];
    } else if (delta < 0) {
        for (i = tail_start; i < nlines; i++)
            lines[i + delta] = lines[i];

        for (i = nlines + delta; i < nlines; i++)
            lines[i] = NULL;
    }

    for (i = 0; i < add_count; i++)
        lines[start + i] = dupstr(add_lines[i]);

    nlines += delta;

    if (nlines <= 0) {
        lines[0] = dupstr("");
        nlines = 1;
    }
}

static void apply_edit_op(const EditOp *op, int undoing)
{
    if (undoing) {
        replace_line_patch(op->start_row,
                           op->new_count,
                           op->old_lines,
                           op->old_count);

        cy = op->before_cy;
        cx = op->before_cx;
        rowoff = op->before_rowoff;
        coloff = op->before_coloff;
        modified = op->before_modified;
    } else {
        replace_line_patch(op->start_row,
                           op->old_count,
                           op->new_lines,
                           op->new_count);

        cy = op->after_cy;
        cx = op->after_cx;
        rowoff = op->after_rowoff;
        coloff = op->after_coloff;
        modified = op->after_modified;
    }

    if (cy < 0)
        cy = 0;
    if (cy >= nlines)
        cy = nlines - 1;
    if (cx < 0)
        cx = 0;
    if (cx > (int)strlen(lines[cy]))
        cx = (int)strlen(lines[cy]);

    selecting = 0;
    column_selecting = 0;
}

static void push_undo(void)
{
    finalize_pending_undo();
    clear_stack(redo_stack, &redo_count);

    free_capture(&pending_undo);
    capture_current(&pending_undo);
}

static void do_undo(void)
{
    EditOp op;
    int i;

    finalize_pending_undo();

    if (undo_count <= 0) {
        set_status("Nothing to undo");
        return;
    }

    undo_count--;
    op = undo_stack[undo_count];
    memset(&undo_stack[undo_count], 0, sizeof(EditOp));

    apply_edit_op(&op, 1);

    if (redo_count == UNDO_DEPTH) {
        free_edit_op(&redo_stack[0]);

        for (i = 1; i < UNDO_DEPTH; i++)
            redo_stack[i - 1] = redo_stack[i];

        redo_count--;
    }

    redo_stack[redo_count++] = op;
    set_status("Undo");
}

static void do_redo(void)
{
    EditOp op;
    int i;

    finalize_pending_undo();

    if (redo_count <= 0) {
        set_status("Nothing to redo");
        return;
    }

    redo_count--;
    op = redo_stack[redo_count];
    memset(&redo_stack[redo_count], 0, sizeof(EditOp));

    apply_edit_op(&op, 0);

    if (undo_count == UNDO_DEPTH) {
        free_edit_op(&undo_stack[0]);

        for (i = 1; i < UNDO_DEPTH; i++)
            undo_stack[i - 1] = undo_stack[i];

        undo_count--;
    }

    undo_stack[undo_count++] = op;
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
        } else if (!strcmp(buf, "session=on")) {
            session_enabled = 1;
        } else if (!strcmp(buf, "session=off")) {
            session_enabled = 0;
        } else if (!strcmp(buf, "recovery=on")) {
            recovery_enabled = 1;
        } else if (!strcmp(buf, "recovery=off")) {
            recovery_enabled = 0;
        } else if (!strcmp(buf, "backup=on")) {
            backup_enabled = 1;
        } else if (!strcmp(buf, "backup=off")) {
            backup_enabled = 0;
        } else if (!strncmp(buf, "recovery_interval=", 18)) {
            int sec;
            sec = atoi(buf + 18);
            if (sec >= 5 && sec <= 3600)
                recovery_interval = sec;
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

        recent_add(filename);
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

    recent_add(filename);
    sprintf(statusmsg, "Loaded %s", filename);

    return 0;
}

static int save_file_as(const char *name)
{
    FILE *fp;
    int i;

    finalize_pending_undo();
    backup_existing_file(name);
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
    recent_add(filename);
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

    if (column_selecting) {
        int top;
        int bottom;
        int left;
        int right;

        top = sel_sy < cy ? sel_sy : cy;
        bottom = sel_sy > cy ? sel_sy : cy;
        left = sel_sx < cx ? sel_sx : cx;
        right = sel_sx > cx ? sel_sx : cx;

        return row >= top && row <= bottom &&
               col >= left && col < right;
    }

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

    if (selecting && column_selecting) {
        int top;
        int bottom;
        int left;
        int right;
        int row;
        int size;
        int pos;
        char *buf;

        top = sel_sy < cy ? sel_sy : cy;
        bottom = sel_sy > cy ? sel_sy : cy;
        left = sel_sx < cx ? sel_sx : cx;
        right = sel_sx > cx ? sel_sx : cx;

        size = (bottom - top + 1) * (right - left + 1) + 1;
        buf = (char *)malloc(size);
        if (buf == NULL) {
            set_status("Out of memory");
            return;
        }

        pos = 0;

        for (row = top; row <= bottom; row++) {
            int len;
            int start;
            int end;

            len = (int)strlen(lines[row]);
            start = left < len ? left : len;
            end = right < len ? right : len;

            if (end > start) {
                memcpy(buf + pos, lines[row] + start, end - start);
                pos += end - start;
            }

            if (row < bottom)
                buf[pos++] = '\n';
        }

        buf[pos] = '\0';
        set_clipboard_text(buf);
        free(buf);
        set_status("Column selection copied");
        return;
    }

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

    if (column_selecting) {
        int top;
        int bottom;
        int left;
        int right;
        int row;

        top = sel_sy < cy ? sel_sy : cy;
        bottom = sel_sy > cy ? sel_sy : cy;
        left = sel_sx < cx ? sel_sx : cx;
        right = sel_sx > cx ? sel_sx : cx;

        push_undo();

        for (row = top; row <= bottom; row++) {
            int len;
            int start;
            int end;

            len = (int)strlen(lines[row]);
            start = left < len ? left : len;
            end = right < len ? right : len;

            if (end > start)
                memmove(lines[row] + start,
                        lines[row] + end,
                        len - end + 1);
        }

        cy = top;
        cx = left;
        if (cx > (int)strlen(lines[cy]))
            cx = (int)strlen(lines[cy]);

        selecting = 0;
        column_selecting = 0;
        modified = 1;
        set_status("Column selection deleted");
        return;
    }

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


static void toggle_column_selection(void)
{
    if (selecting && column_selecting) {
        selecting = 0;
        column_selecting = 0;
        set_status("Column selection cleared");
        return;
    }

    selecting = 1;
    column_selecting = 1;
    sel_sy = cy;
    sel_sx = cx;
    set_status("Column selection started");
}

static void tabs_to_spaces(void)
{
    int start;
    int end;
    int row;

    selected_line_range(&start, &end);
    push_undo();

    for (row = start; row <= end; row++) {
        const char *old;
        char *p;
        int i;
        int col;
        int outlen;

        old = lines[row];
        outlen = 0;
        col = 0;

        for (i = 0; old[i] != '\0'; i++) {
            if (old[i] == '\t') {
                int n;
                n = tab_width - (col % tab_width);
                outlen += n;
                col += n;
            } else {
                outlen++;
                col++;
            }
        }

        if (outlen >= MAX_LINE)
            continue;

        p = (char *)malloc(outlen + 1);
        if (p == NULL)
            continue;

        outlen = 0;
        col = 0;

        for (i = 0; old[i] != '\0'; i++) {
            if (old[i] == '\t') {
                int n;
                int j;
                n = tab_width - (col % tab_width);
                for (j = 0; j < n; j++)
                    p[outlen++] = ' ';
                col += n;
            } else {
                p[outlen++] = old[i];
                col++;
            }
        }

        p[outlen] = '\0';
        free(lines[row]);
        lines[row] = p;
    }

    modified = 1;
    set_status("Tabs converted to spaces");
}

static void spaces_to_tabs(void)
{
    int start;
    int end;
    int row;

    selected_line_range(&start, &end);
    push_undo();

    for (row = start; row <= end; row++) {
        char *old;
        char *p;
        int lead;
        int tabs;
        int spaces;
        int rest;
        int outlen;
        int i;

        old = lines[row];
        lead = 0;

        while (old[lead] == ' ')
            lead++;

        tabs = lead / tab_width;
        spaces = lead % tab_width;
        rest = (int)strlen(old + lead);
        outlen = tabs + spaces + rest;

        p = (char *)malloc(outlen + 1);
        if (p == NULL)
            continue;

        outlen = 0;
        for (i = 0; i < tabs; i++)
            p[outlen++] = '\t';
        for (i = 0; i < spaces; i++)
            p[outlen++] = ' ';

        strcpy(p + outlen, old + lead);

        free(lines[row]);
        lines[row] = p;
    }

    modified = 1;
    set_status("Leading spaces converted to tabs");
}

static int line_ptr_cmp(const void *a, const void *b)
{
    const char * const *sa;
    const char * const *sb;

    sa = (const char * const *)a;
    sb = (const char * const *)b;
    return strcmp(*sa, *sb);
}

static void sort_selected_lines(void)
{
    int start;
    int end;

    selected_line_range(&start, &end);

    if (end <= start) {
        set_status("Select more than one line to sort");
        return;
    }

    push_undo();
    qsort(&lines[start], end - start + 1, sizeof(char *), line_ptr_cmp);
    modified = 1;
    set_status("Selected lines sorted");
}

static void transpose_chars(void)
{
    int len;
    int a;
    int b;
    char tmp;

    len = (int)strlen(lines[cy]);

    if (len < 2) {
        set_status("Nothing to transpose");
        return;
    }

    if (cx <= 0) {
        a = 0;
        b = 1;
    } else if (cx >= len) {
        a = len - 2;
        b = len - 1;
    } else {
        a = cx - 1;
        b = cx;
    }

    push_undo();
    tmp = lines[cy][a];
    lines[cy][a] = lines[cy][b];
    lines[cy][b] = tmp;

    if (cx < len)
        cx++;

    modified = 1;
    set_status("Characters transposed");
}

static int repeatable_action(int action)
{
    switch (action) {
    case ACT_INDENT:
    case ACT_UNINDENT:
    case ACT_COMMENT:
    case ACT_DUP_LINE:
    case ACT_DELETE_LINE:
    case ACT_MOVE_LINE_UP:
    case ACT_MOVE_LINE_DOWN:
    case ACT_JOIN_LINE:
    case ACT_TRIM_WS:
    case ACT_UPPERCASE:
    case ACT_LOWERCASE:
    case ACT_TABS_TO_SPACES:
    case ACT_SPACES_TO_TABS:
    case ACT_SORT_LINES:
    case ACT_TRANSPOSE_CHARS:
        return 1;
    }

    return 0;
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
    column_selecting = 0;
    sel_sy = cy;
    sel_sx = start;
    cx = end;
    set_status("Word selected");
}

static void select_current_line(void)
{
    selecting = 1;
    column_selecting = 0;
    sel_sy = cy;
    sel_sx = 0;
    cx = (int)strlen(lines[cy]);
    set_status("Line selected");
}

static void select_all_text(void)
{
    selecting = 1;
    column_selecting = 0;
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
    textrows = LINES - 3 - output_pane_height();
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
    end = coloff + editor_text_columns(gutter);

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
    textrows = editor_active_text_rows();
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
        ctx.visible_end = coloff + editor_text_columns(gutter);
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
    int output_h;
    int status_y;
    scroll_screen();
    compute_bracket_match();
    maybe_write_recovery();
    erase();

    if (use_color)
        attron(COLOR_PAIR(CP_MENU));
    else
        attron(A_REVERSE);

    mvaddstr(0, 0, " File  Edit  Search  Project  View  Options  Help ");
    {
        int bx;
        int bi;

        bx = 51;

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
    textrows = editor_active_text_rows();
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


    if (split_enabled && split_other >= 0 &&
        split_other < buffer_count && split_other != curbuf) {
        int separator_y;
        int bottom_y;
        int bottom_rows;
        int saved_buf;
        int saved_selecting;

        separator_y = textrows + 1;
        bottom_y = separator_y + 1;
        bottom_rows = editor_total_text_rows() - textrows - 1;

        attron(A_REVERSE);
        move(separator_y, 0);
        clrtoeol();
        mvprintw(separator_y, 1, " Other buffer: %s ",
                 buffers[split_other].fname[0] ?
                 buffers[split_other].fname : "[No Name]");
        attroff(A_REVERSE);

        saved_buf = curbuf;
        saved_selecting = selecting;
        curbuf = split_other;
        selecting = 0;

        for (y = 0; y < bottom_rows; y++) {
            filerow = rowoff + y;

            if (line_numbers) {
                if (use_color)
                    attron(COLOR_PAIR(CP_LINENO));
                else
                    attron(A_BOLD);

                if (filerow < nlines)
                    mvprintw(bottom_y + y, 0, "%5d ", filerow + 1);
                else
                    mvaddstr(bottom_y + y, 0, "    ~ ");

                if (use_color)
                    attroff(COLOR_PAIR(CP_LINENO));
                else
                    attroff(A_BOLD);
            }

            if (filerow < nlines)
                draw_plain_line(bottom_y + y, filerow,
                                lines[filerow], gutter);
        }

        curbuf = saved_buf;
        selecting = saved_selecting;
    }

    if (symbol_sidebar_width() > 0)
        draw_symbol_sidebar(1, editor_total_text_rows());

    if (use_color)
        attron(COLOR_PAIR(CP_STATUS));
    else
        attron(A_REVERSE);

    output_h = output_pane_height();
    status_y = LINES - 2 - output_h;

    name_room = COLS - 48;
    if (name_room < 10)
        name_room = 10;

    sprintf(buf, " %-*.*s %s %s %s Ln %d/%d Col %d TAB:%d ",
            name_room, name_room,
            filename[0] ? filename : "[No Name]",
            modified ? "[+]" : "",
            selecting ? (column_selecting ? "[COL]" : "[SEL]") : "",
            syntax_enabled ? syntax_name() : "TEXT",
            cy + 1, nlines, cx + 1, tab_width);

    mvaddnstr(status_y, 0, buf, COLS);
    clrtoeol();

    if (use_color)
        attroff(COLOR_PAIR(CP_STATUS));
    else
        attroff(A_REVERSE);

    mvaddstr(status_y + 1, 0,
             "^T Menu ^S Save ^F Find ^H Repl ^B Sel ^Z Undo ^Q Quit");

    if (statusmsg[0] != '\0') {
        int pos;
        pos = COLS - (int)strlen(statusmsg) - 1;
        if (pos > 0)
            mvaddnstr(status_y + 1, pos, statusmsg, COLS - pos);
    }

    if (output_h > 0) {
        int oy;
        int first;

        first = output_count - output_h + 1;
        if (first < 0)
            first = 0;

        attron(A_REVERSE);
        mvaddstr(status_y + 2, 0, " Output ");
        clrtoeol();
        attroff(A_REVERSE);

        for (oy = 1; oy < output_h; oy++) {
            int oi;

            oi = first + oy - 1;
            move(status_y + 2 + oy, 0);
            clrtoeol();

            if (oi < output_count)
                mvaddnstr(status_y + 2 + oy, 0,
                          output_lines[oi], COLS - 1);
        }
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
    time_t mtime;
} BrowserEntry;

typedef struct {
    char path[PATH_LEN];
    BrowserEntry *entries;
    int count;
    int selected;
    int top;
} BrowserPane;

static int browser_sort_mode = 0;
static int browser_show_hidden = 0;
static char browser_bookmarks[8][PATH_LEN];
static int browser_bookmark_count = 0;

static int browser_cmp(const void *a, const void *b)
{
    const BrowserEntry *ea;
    const BrowserEntry *eb;

    ea = (const BrowserEntry *)a;
    eb = (const BrowserEntry *)b;

    if (ea->isdir != eb->isdir)
        return eb->isdir - ea->isdir;

    if (browser_sort_mode == 1) {
        if (ea->size < eb->size) return 1;
        if (ea->size > eb->size) return -1;
    } else if (browser_sort_mode == 2) {
        if (ea->mtime < eb->mtime) return 1;
        if (ea->mtime > eb->mtime) return -1;
    }

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

static int browser_load(const char *path, BrowserEntry *entries,
                        int max_entries, int show_hidden)
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

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        if (!show_hidden && de->d_name[0] == '.')
            continue;

        strncpy(entries[count].name, de->d_name, NAME_LEN - 1);
        entries[count].name[NAME_LEN - 1] = '\0';

        path_join(full, path, de->d_name);

        if (stat(full, &st) == 0) {
            entries[count].isdir = S_ISDIR(st.st_mode) ? 1 : 0;
            entries[count].size = (long)st.st_size;
            entries[count].mode = (unsigned long)st.st_mode;
            entries[count].mtime = st.st_mtime;
        } else {
            entries[count].isdir = 0;
            entries[count].size = 0;
            entries[count].mode = 0;
            entries[count].mtime = 0;
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

static int copy_file_data(const char *src, const char *dst)
{
    FILE *in;
    FILE *out;
    char buf[8192];
    size_t n;

    in = fopen(src, "rb");
    if (in == NULL)
        return -1;

    out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(out);
            fclose(in);
            return -1;
        }
    }

    fclose(out);
    fclose(in);
    return 0;
}

static int copy_tree(const char *src, const char *dst)
{
    struct stat st;

    if (stat(src, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR *dp;
        struct dirent *de;

        mkdir(dst, st.st_mode & 0777);
        dp = opendir(src);
        if (dp == NULL)
            return -1;

        while ((de = readdir(dp)) != NULL) {
            char s[PATH_LEN];
            char d[PATH_LEN];

            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;

            path_join(s, src, de->d_name);
            path_join(d, dst, de->d_name);

            if (copy_tree(s, d) != 0) {
                closedir(dp);
                return -1;
            }
        }

        closedir(dp);
        return 0;
    }

    return copy_file_data(src, dst);
}

static int remove_tree(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR *dp;
        struct dirent *de;

        dp = opendir(path);
        if (dp == NULL)
            return -1;

        while ((de = readdir(dp)) != NULL) {
            char child[PATH_LEN];

            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;

            path_join(child, path, de->d_name);

            if (remove_tree(child) != 0) {
                closedir(dp);
                return -1;
            }
        }

        closedir(dp);
        return rmdir(path);
    }

    return unlink(path);
}

static void browser_bookmarks_load(void)
{
    char *home;
    char path[PATH_LEN];
    char line[PATH_LEN];
    FILE *fp;

    browser_bookmark_count = 0;
    home = getenv("HOME");
    if (home == NULL)
        return;

    sprintf(path, "%s/.tedit/browser-bookmarks", home);
    fp = fopen(path, "r");
    if (fp == NULL)
        return;

    while (browser_bookmark_count < 8 &&
           fgets(line, sizeof(line), fp) != NULL) {
        int len;

        len = (int)strlen(line);
        while (len > 0 &&
              (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (line[0] != '\0') {
            strncpy(browser_bookmarks[browser_bookmark_count],
                    line, PATH_LEN - 1);
            browser_bookmarks[browser_bookmark_count][PATH_LEN - 1] = '\0';
            browser_bookmark_count++;
        }
    }

    fclose(fp);
}

static void browser_bookmarks_save(void)
{
    char *home;
    char path[PATH_LEN];
    FILE *fp;
    int i;

    home = getenv("HOME");
    if (home == NULL)
        return;

    ensure_user_tedit_dirs();
    sprintf(path, "%s/.tedit/browser-bookmarks", home);

    fp = fopen(path, "w");
    if (fp == NULL)
        return;

    for (i = 0; i < browser_bookmark_count; i++)
        fprintf(fp, "%s\n", browser_bookmarks[i]);

    fclose(fp);
}

static void browser_add_bookmark(const char *path)
{
    int i;

    for (i = 0; i < browser_bookmark_count; i++) {
        if (!strcmp(browser_bookmarks[i], path)) {
            set_status("Directory already bookmarked");
            return;
        }
    }

    if (browser_bookmark_count < 8) {
        strncpy(browser_bookmarks[browser_bookmark_count],
                path, PATH_LEN - 1);
        browser_bookmarks[browser_bookmark_count][PATH_LEN - 1] = '\0';
        browser_bookmark_count++;
    } else {
        for (i = 1; i < 8; i++)
            strcpy(browser_bookmarks[i - 1], browser_bookmarks[i]);
        strncpy(browser_bookmarks[7], path, PATH_LEN - 1);
        browser_bookmarks[7][PATH_LEN - 1] = '\0';
    }

    browser_bookmarks_save();
    set_status("Directory bookmarked");
}

static int browser_choose_bookmark(char *out, int outlen)
{
    int selected;
    int ch;
    int i;

    if (browser_bookmark_count <= 0) {
        set_status("No browser bookmarks");
        return 0;
    }

    selected = 0;

    for (;;) {
        erase();
        attron(A_REVERSE);
        mvaddstr(0, 0, " TEDIT Directory Bookmarks ");
        clrtoeol();
        attroff(A_REVERSE);

        for (i = 0; i < browser_bookmark_count; i++) {
            if (i == selected)
                attron(A_REVERSE);
            mvprintw(i + 2, 2, "%d  ", i + 1);
            addnstr(browser_bookmarks[i], COLS - 8);
            if (i == selected)
                attroff(A_REVERSE);
        }

        mvaddstr(LINES - 1, 0, "Enter jump  Up/Down move  Esc cancel");
        refresh();

        ch = getch();

        if (ch == 27)
            return 0;
        if (ch == KEY_UP && selected > 0)
            selected--;
        else if (ch == KEY_DOWN && selected + 1 < browser_bookmark_count)
            selected++;
        else if (ch == '\n' || ch == '\r') {
            strncpy(out, browser_bookmarks[selected], outlen - 1);
            out[outlen - 1] = '\0';
            return 1;
        }
    }
}

static void browser_draw_pane(BrowserPane *pane, int active,
                              int y, int x, int height, int width)
{
    int rows;
    int i;

    draw_box_ascii(y, x, height, width);

    if (active)
        attron(A_REVERSE);

    mvaddnstr(y, x + 2, pane->path, width - 4);

    if (active)
        attroff(A_REVERSE);

    rows = height - 2;

    if (pane->selected < pane->top)
        pane->top = pane->selected;
    if (pane->selected >= pane->top + rows)
        pane->top = pane->selected - rows + 1;
    if (pane->top < 0)
        pane->top = 0;

    for (i = 0; i < rows; i++) {
        int idx;
        char display[NAME_LEN + 32];
        char sizebuf[32];
        int avail;

        idx = pane->top + i;
        move(y + 1 + i, x + 1);
        {
            int blank;
            for (blank = 0; blank < width - 2; blank++)
                addch(' ');
        }

        if (idx >= pane->count)
            continue;

        avail = width - 15;
        if (avail < 4)
            avail = 4;

        if (pane->entries[idx].isdir) {
            sprintf(display, "/ %-*.*s",
                    avail, avail, pane->entries[idx].name);
        } else {
            human_size(pane->entries[idx].size, sizebuf, sizeof(sizebuf));
            sprintf(display, "  %-*.*s %8s",
                    avail, avail, pane->entries[idx].name, sizebuf);
        }

        if (active && idx == pane->selected)
            attron(A_REVERSE);

        mvaddnstr(y + 1 + i, x + 1, display, width - 2);

        if (active && idx == pane->selected)
            attroff(A_REVERSE);
    }
}

static void browser_reload(BrowserPane *pane)
{
    pane->count = browser_load(pane->path, pane->entries,
                               BROWSER_MAX, browser_show_hidden);

    if (pane->count < 0)
        pane->count = 0;

    if (pane->selected >= pane->count)
        pane->selected = pane->count > 0 ? pane->count - 1 : 0;

    if (pane->selected < 0)
        pane->selected = 0;
}

static int file_browser(char *out, int outlen)
{
    static BrowserEntry left_entries[BROWSER_MAX];
    static BrowserEntry right_entries[BROWSER_MAX];
    BrowserPane pane[2];
    char cwd[PATH_LEN];
    int active;
    int ch;

    if (getcwd(cwd, sizeof(cwd)) == NULL)
        strcpy(cwd, ".");

    memset(&pane, 0, sizeof(pane));

    strncpy(pane[0].path, cwd, PATH_LEN - 1);
    pane[0].path[PATH_LEN - 1] = '\0';
    strncpy(pane[1].path, cwd, PATH_LEN - 1);
    pane[1].path[PATH_LEN - 1] = '\0';

    pane[0].entries = left_entries;
    pane[1].entries = right_entries;
    active = 0;

    browser_bookmarks_load();

    for (;;) {
        int body_y;
        int body_h;
        int left_w;
        int right_w;
        BrowserPane *p;
        BrowserPane *other;

        browser_reload(&pane[0]);
        browser_reload(&pane[1]);

        p = &pane[active];
        other = &pane[1 - active];

        body_y = 2;
        body_h = LINES - 5;
        if (body_h < 6)
            body_h = 6;

        left_w = (COLS - 3) / 2;
        right_w = COLS - left_w - 3;

        erase();

        attron(A_REVERSE);
        mvaddstr(0, 0, " TEDIT File Navigator ");
        clrtoeol();
        attroff(A_REVERSE);

        browser_draw_pane(&pane[0], active == 0,
                          body_y, 1, body_h, left_w);
        browser_draw_pane(&pane[1], active == 1,
                          body_y, left_w + 2, body_h, right_w);

        attron(A_REVERSE);
        mvaddstr(LINES - 2, 0,
                 " Tab Pane  Enter Open  BS Parent  c Copy  m Move  d Delete  r Rename ");
        clrtoeol();
        attroff(A_REVERSE);

        mvaddstr(LINES - 1, 0,
                 " n Mkdir  g Go  b Bookmark  j Jump  h Hidden  s Sort  Esc Cancel");

        refresh();
        ch = getch();

        if (ch == 27)
            return 0;

        if (ch == '\t') {
            active = 1 - active;
            continue;
        }

        if (ch == KEY_UP) {
            if (p->selected > 0)
                p->selected--;
            continue;
        }

        if (ch == KEY_DOWN) {
            if (p->selected + 1 < p->count)
                p->selected++;
            continue;
        }

#ifdef KEY_PPAGE
        if (ch == KEY_PPAGE) {
            p->selected -= body_h - 2;
            if (p->selected < 0)
                p->selected = 0;
            continue;
        }
#endif

#ifdef KEY_NPAGE
        if (ch == KEY_NPAGE) {
            p->selected += body_h - 2;
            if (p->selected >= p->count)
                p->selected = p->count > 0 ? p->count - 1 : 0;
            continue;
        }
#endif

#ifdef KEY_HOME
        if (ch == KEY_HOME) {
            p->selected = 0;
            continue;
        }
#endif

#ifdef KEY_END
        if (ch == KEY_END) {
            p->selected = p->count > 0 ? p->count - 1 : 0;
            continue;
        }
#endif

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            path_parent(p->path);
            p->selected = 0;
            p->top = 0;
            continue;
        }

        if ((ch == '\n' || ch == '\r' || ch == 'o') && p->count > 0) {
            char full[PATH_LEN];

            path_join(full, p->path, p->entries[p->selected].name);

            if (p->entries[p->selected].isdir) {
                strncpy(p->path, full, PATH_LEN - 1);
                p->path[PATH_LEN - 1] = '\0';
                p->selected = 0;
                p->top = 0;
            } else {
                strncpy(out, full, outlen - 1);
                out[outlen - 1] = '\0';
                return 1;
            }

            continue;
        }

        if (ch == 'h' || ch == 'H') {
            browser_show_hidden = !browser_show_hidden;
            pane[0].selected = pane[1].selected = 0;
            pane[0].top = pane[1].top = 0;
            continue;
        }

        if (ch == 's' || ch == 'S') {
            browser_sort_mode++;
            if (browser_sort_mode > 2)
                browser_sort_mode = 0;
            pane[0].selected = pane[1].selected = 0;
            continue;
        }

        if (ch == 'g' || ch == 'G') {
            char path[PATH_LEN];

            if (prompt_input("Go to directory: ", path, sizeof(path)) &&
                path[0] != '\0' && path_exists(path)) {
                strncpy(p->path, path, PATH_LEN - 1);
                p->path[PATH_LEN - 1] = '\0';
                p->selected = 0;
                p->top = 0;
            }
            continue;
        }

        if (ch == 'b' || ch == 'B') {
            browser_add_bookmark(p->path);
            continue;
        }

        if (ch == 'j' || ch == 'J') {
            char path[PATH_LEN];

            if (browser_choose_bookmark(path, sizeof(path))) {
                strncpy(p->path, path, PATH_LEN - 1);
                p->path[PATH_LEN - 1] = '\0';
                p->selected = 0;
                p->top = 0;
            }
            continue;
        }

        if (ch == 'n' || ch == 'N') {
            char name[NAME_LEN];
            char full[PATH_LEN];

            if (prompt_input("New directory: ", name, sizeof(name)) &&
                name[0] != '\0') {
                path_join(full, p->path, name);
                if (mkdir(full, 0755) != 0)
                    set_status("Cannot create directory");
                else
                    set_status("Directory created");
            }
            continue;
        }

        if ((ch == 'r' || ch == 'R') && p->count > 0) {
            char name[NAME_LEN];
            char src[PATH_LEN];
            char dst[PATH_LEN];

            if (prompt_input("Rename to: ", name, sizeof(name)) &&
                name[0] != '\0') {
                path_join(src, p->path, p->entries[p->selected].name);
                path_join(dst, p->path, name);

                if (rename(src, dst) != 0)
                    set_status("Rename failed");
                else
                    set_status("Renamed");
            }
            continue;
        }

        if ((ch == 'd' || ch == 'D') && p->count > 0) {
            char full[PATH_LEN];
            char question[STATUS_LEN];

            path_join(full, p->path, p->entries[p->selected].name);
            sprintf(question, "Delete %s ? (y/N)", p->entries[p->selected].name);

            if (confirm_yes_no(question)) {
                if (remove_tree(full) != 0)
                    set_status("Delete failed");
                else
                    set_status("Deleted");
            }
            continue;
        }

        if ((ch == 'c' || ch == 'C' || ch == 'm' || ch == 'M') &&
            p->count > 0) {
            char src[PATH_LEN];
            char dst[PATH_LEN];
            int moving;

            moving = (ch == 'm' || ch == 'M');
            path_join(src, p->path, p->entries[p->selected].name);
            path_join(dst, other->path, p->entries[p->selected].name);

            if (path_exists(dst)) {
                char question[STATUS_LEN];
                sprintf(question, "Destination exists: overwrite/merge? (y/N)");
                if (!confirm_yes_no(question))
                    continue;
            }

            if (moving && rename(src, dst) == 0) {
                set_status("Moved");
            } else if (copy_tree(src, dst) == 0) {
                if (moving) {
                    if (remove_tree(src) != 0)
                        set_status("Copied but could not remove source");
                    else
                        set_status("Moved");
                } else {
                    set_status("Copied");
                }
            } else {
                set_status(moving ? "Move failed" : "Copy failed");
            }

            continue;
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
    int qlen;
    int ch;
    int orig_y;
    int orig_x;

    query[0] = '\0';
    qlen = 0;
    orig_y = cy;
    orig_x = cx;

    for (;;) {
        draw_screen();
        move(LINES - 1, 0);
        clrtoeol();
        mvaddstr(LINES - 1, 0, "I-search: ");
        addnstr(query, COLS - 12);
        refresh();

        ch = getch();

        if (ch == 27) {
            cy = orig_y;
            cx = orig_x;
            scroll_screen();
            set_status("Incremental search cancelled");
            return;
        }

        if (ch == '\n' || ch == '\r') {
            if (query[0] != '\0') {
                strncpy(last_search, query, SEARCH_LEN - 1);
                last_search[SEARCH_LEN - 1] = '\0';
                sprintf(statusmsg, "Found: %s", query);
            }
            return;
        }

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (qlen > 0)
                query[--qlen] = '\0';
        } else if (ch >= 32 && ch <= 126 && qlen < SEARCH_LEN - 1) {
            query[qlen++] = (char)ch;
            query[qlen] = '\0';
        } else {
            continue;
        }

        cy = orig_y;
        cx = orig_x;

        if (query[0] != '\0') {
            if (!find_from_position(query, orig_y, orig_x))
                set_status("No match");
            else
                set_status("Incremental match");
        }
    }
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

    mvaddstr(2, 4, "TEDIT v7.0-dev stage 7");
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

    save_session();
    clear_recovery_files();

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

    finalize_pending_undo();
    free_capture(&pending_undo);
    clear_stack(undo_stack, &undo_count);
    clear_stack(redo_stack, &redo_count);

    if (clipboard != NULL)
        free(clipboard);

    exit(0);
}

static void execute_action(int action)
{
    if (action != ACT_REPEAT_LAST && repeatable_action(action))
        last_repeat_action = action;

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
        column_selecting = 0;
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
    case ACT_COLUMN_SELECT: toggle_column_selection(); break;
    case ACT_TABS_TO_SPACES: tabs_to_spaces(); break;
    case ACT_SPACES_TO_TABS: spaces_to_tabs(); break;
    case ACT_SORT_LINES: sort_selected_lines(); break;
    case ACT_TRANSPOSE_CHARS: transpose_chars(); break;
    case ACT_REPEAT_LAST:
        if (last_repeat_action != ACT_NONE)
            execute_action(last_repeat_action);
        else
            set_status("No repeatable edit yet");
        break;
    case ACT_MATCH_BRACKET: goto_matching_bracket(); break;
    case ACT_BOOKMARK_TOGGLE: toggle_bookmark(); break;
    case ACT_BOOKMARK_NEXT: next_bookmark(); break;
    case ACT_PROJECT_ROOT: detect_project_root(); break;
    case ACT_PROJECT_OPEN: do_project_open(); break;
    case ACT_BUILD: run_project_command(project_build, "Build"); break;
    case ACT_CLEAN: run_project_command(project_clean, "Clean"); break;
    case ACT_RUN: run_project_command(project_run, "Run"); break;
    case ACT_FIND_FILES: do_find_in_files(); break;
    case ACT_NEXT_RESULT: jump_to_output_location(); break;
    case ACT_TOGGLE_OUTPUT:
        output_visible = !output_visible;
        set_status(output_visible ? "Output pane shown" : "Output pane hidden");
        break;
    case ACT_SPLIT_TOGGLE: toggle_split(); break;
    case ACT_SPLIT_SWITCH: switch_split_pane(); break;
    case ACT_SYMBOLS_TOGGLE:
        symbol_sidebar_enabled = !symbol_sidebar_enabled;
        set_status(symbol_sidebar_enabled ? "Symbol sidebar shown" :
                                              "Symbol sidebar hidden");
        break;
    case ACT_SYMBOL_LIST: symbol_list_dialog(); break;
    case ACT_NEXT_SYMBOL: jump_symbol(1); break;
    case ACT_PREV_SYMBOL: jump_symbol(-1); break;
    case ACT_OPEN_RECENT: do_open_recent(); break;
    case ACT_RECOVER: do_recover(); break;
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
        *count = sizeof(project_menu) / sizeof(project_menu[0]);
        return project_menu;
    case 4:
        *count = sizeof(view_menu) / sizeof(view_menu[0]);
        return view_menu;
    case 5:
        *count = sizeof(options_menu) / sizeof(options_menu[0]);
        return options_menu;
    case 6:
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

    for (i = 0; i < MENU_COUNT; i++) {
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
                menu_index = MENU_COUNT - 1;
            items = get_menu(menu_index, &count);
            item_index = first_selectable(items, count);
        } else if (ch == KEY_RIGHT) {
            menu_index++;
            if (menu_index >= MENU_COUNT)
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

#if defined(KEY_MOUSE) && defined(ALL_MOUSE_EVENTS)
    if (ch == KEY_MOUSE) {
        MEVENT ev;

        if (getmouse(&ev) == OK) {
            int gutter;
            int rows;
            int sidebar;

            gutter = line_numbers ? 6 : 0;
            rows = editor_active_text_rows();
            sidebar = symbol_sidebar_width();

            if (ev.y >= 1 && ev.y <= rows &&
                ev.x >= gutter && ev.x < COLS - sidebar) {
                int nr;
                int nc;

                nr = rowoff + ev.y - 1;
                nc = coloff + ev.x - gutter;

                if (nr >= 0 && nr < nlines) {
                    cy = nr;
                    if (nc < 0) nc = 0;
                    if (nc > (int)strlen(lines[cy]))
                        nc = (int)strlen(lines[cy]);
                    cx = nc;
                    scroll_screen();
                }
            }
        }

        return;
    }
#endif

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
        column_selecting = 0;
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
    project_root[0] = '\0';
    strcpy(project_build, "make");
    strcpy(project_clean, "make clean");
    strcpy(project_run, "./a.out");

    curbuf = 0;
    buffer_count = 1;
    init_buffer();
    load_config();
    load_syntax_definitions();
    recent_load();

    if (argc > 1)
        load_file(argv[1]);
    else
        restore_session();

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);

#if defined(KEY_MOUSE) && defined(ALL_MOUSE_EVENTS)
    mousemask(ALL_MOUSE_EVENTS, NULL);
#endif

    init_colors_if_possible();

    for (;;) {
        draw_screen();
        ch = getch();
        process_key(ch);
    }

    return 0;
}