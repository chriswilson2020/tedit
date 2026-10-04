#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "syntax.h"

#define MAX_SYNTAX_DEFS 64

static SyntaxDef defs[MAX_SYNTAX_DEFS];
static int def_count = 0;

static void trim(char *s)
{
    char *p;
    int len;

    while (*s && isspace((unsigned char)*s))
        memmove(s, s + 1, strlen(s));

    len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }

    p = strchr(s, '\r');
    if (p != NULL)
        *p = '\0';
    p = strchr(s, '\n');
    if (p != NULL)
        *p = '\0';
}

static void copy_text(char *dst, int dstlen, const char *src)
{
    if (dstlen <= 0)
        return;
    strncpy(dst, src, dstlen - 1);
    dst[dstlen - 1] = '\0';
}

static int eq_nocase(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int word_eq(const SyntaxDef *def, const char *a, int alen,
                   const char *b)
{
    int i;
    int blen;

    blen = (int)strlen(b);
    if (alen != blen)
        return 0;

    for (i = 0; i < alen; i++) {
        int ca;
        int cb;
        ca = (unsigned char)a[i];
        cb = (unsigned char)b[i];
        if (def != NULL && def->case_insensitive) {
            ca = tolower(ca);
            cb = tolower(cb);
        }
        if (ca != cb)
            return 0;
    }

    return 1;
}

static int list_has_token(const char *list, const char *token)
{
    char buf[SYNTAX_TEXT_LEN];
    char *p;
    char *start;

    copy_text(buf, sizeof(buf), list);
    start = buf;

    for (;;) {
        p = strchr(start, ',');
        if (p != NULL)
            *p = '\0';
        trim(start);
        if (*start != '\0' && !strcmp(start, token))
            return 1;
        if (p == NULL)
            break;
        start = p + 1;
    }

    return 0;
}

static int ends_with(const char *s, const char *suffix)
{
    int ls;
    int lx;
    ls = (int)strlen(s);
    lx = (int)strlen(suffix);
    return lx <= ls && !strcmp(s + ls - lx, suffix);
}

static const char *base_name(const char *s)
{
    const char *p;
    p = strrchr(s, '/');
    return p == NULL ? s : p + 1;
}

static int match_extensions(const char *list, const char *filename)
{
    char buf[SYNTAX_TEXT_LEN];
    char *p;
    char *start;

    copy_text(buf, sizeof(buf), list);
    start = buf;

    for (;;) {
        p = strchr(start, ',');
        if (p != NULL)
            *p = '\0';
        trim(start);
        if (*start != '\0' && ends_with(filename, start))
            return 1;
        if (p == NULL)
            break;
        start = p + 1;
    }

    return 0;
}

static void add_word_list(char dest[][SYNTAX_WORD_LEN], int *count,
                          int max_count, const char *value)
{
    char buf[2048];
    char *p;
    char *start;

    copy_text(buf, sizeof(buf), value);
    start = buf;

    while (*start && *count < max_count) {
        p = strchr(start, ',');
        if (p != NULL)
            *p = '\0';
        trim(start);
        if (*start != '\0') {
            copy_text(dest[*count], SYNTAX_WORD_LEN, start);
            (*count)++;
        }
        if (p == NULL)
            break;
        start = p + 1;
    }
}

static int bool_value(const char *s, int default_value)
{
    if (eq_nocase(s, "yes") || eq_nocase(s, "on") || !strcmp(s, "1"))
        return 1;
    if (eq_nocase(s, "no") || eq_nocase(s, "off") || !strcmp(s, "0"))
        return 0;
    return default_value;
}

static void syntax_defaults(SyntaxDef *d)
{
    memset(d, 0, sizeof(*d));
    copy_text(d->strings, sizeof(d->strings), "\"'");
    d->numbers = 1;
    d->escape_strings = 1;
}

static SyntaxDef *find_by_name(const char *name)
{
    int i;
    for (i = 0; i < def_count; i++) {
        if (!strcmp(defs[i].name, name))
            return &defs[i];
    }
    return NULL;
}

static int parse_file(const char *path)
{
    FILE *fp;
    char line[4096];
    SyntaxDef temp;
    SyntaxDef *dest;

    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;

    syntax_defaults(&temp);

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *eq;
        char *key;
        char *value;

        trim(line);
        if (line[0] == '\0' || line[0] == ';')
            continue;
        if (line[0] == '#' && strchr(line, '=') == NULL)
            continue;

        eq = strchr(line, '=');
        if (eq == NULL)
            continue;

        *eq = '\0';
        key = line;
        value = eq + 1;
        trim(key);
        trim(value);

        if (!strcmp(key, "name"))
            copy_text(temp.name, sizeof(temp.name), value);
        else if (!strcmp(key, "extensions"))
            copy_text(temp.extensions, sizeof(temp.extensions), value);
        else if (!strcmp(key, "filenames"))
            copy_text(temp.filenames, sizeof(temp.filenames), value);
        else if (!strcmp(key, "line_comment"))
            copy_text(temp.line_comment, sizeof(temp.line_comment), value);
        else if (!strcmp(key, "block_comment_start"))
            copy_text(temp.block_start, sizeof(temp.block_start), value);
        else if (!strcmp(key, "block_comment_end"))
            copy_text(temp.block_end, sizeof(temp.block_end), value);
        else if (!strcmp(key, "strings"))
            copy_text(temp.strings, sizeof(temp.strings), value);
        else if (!strcmp(key, "preprocessor"))
            copy_text(temp.preprocessor, sizeof(temp.preprocessor), value);
        else if (!strcmp(key, "case_insensitive"))
            temp.case_insensitive = bool_value(value, 0);
        else if (!strcmp(key, "numbers"))
            temp.numbers = bool_value(value, 1);
        else if (!strcmp(key, "escape_strings"))
            temp.escape_strings = bool_value(value, 1);
        else if (!strcmp(key, "doubled_quotes"))
            temp.doubled_quotes = bool_value(value, 0);
        else if (!strcmp(key, "keywords"))
            add_word_list(temp.keywords, &temp.keyword_count,
                          SYNTAX_MAX_KEYWORDS, value);
        else if (!strcmp(key, "indent_after"))
            add_word_list(temp.indent_after, &temp.indent_after_count,
                          SYNTAX_MAX_RULES, value);
        else if (!strcmp(key, "outdent_before"))
            add_word_list(temp.outdent_before, &temp.outdent_before_count,
                          SYNTAX_MAX_RULES, value);
        else if (!strcmp(key, "outdent_chars"))
            copy_text(temp.outdent_chars, sizeof(temp.outdent_chars), value);
    }

    fclose(fp);

    if (temp.name[0] == '\0')
        return -1;

    dest = find_by_name(temp.name);
    if (dest == NULL) {
        if (def_count >= MAX_SYNTAX_DEFS)
            return -1;
        dest = &defs[def_count++];
    }

    *dest = temp;
    return 0;
}

void syntax_init(void)
{
    def_count = 0;
    memset(defs, 0, sizeof(defs));
}

int syntax_load_dir(const char *path)
{
    DIR *dp;
    struct dirent *de;
    int loaded;

    dp = opendir(path);
    if (dp == NULL)
        return 0;

    loaded = 0;
    while ((de = readdir(dp)) != NULL) {
        char full[2048];
        struct stat st;
        int len;

        len = (int)strlen(de->d_name);
        if (len < 6 || strcmp(de->d_name + len - 5, ".conf"))
            continue;

        sprintf(full, "%s/%s", path, de->d_name);
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        if (parse_file(full) == 0)
            loaded++;
    }

    closedir(dp);
    return loaded;
}

const SyntaxDef *syntax_detect(const char *filename)
{
    const char *b;
    int i;

    if (filename == NULL || *filename == '\0')
        return NULL;

    b = base_name(filename);

    for (i = def_count - 1; i >= 0; i--) {
        if (defs[i].filenames[0] != '\0' &&
            list_has_token(defs[i].filenames, b))
            return &defs[i];
    }

    for (i = def_count - 1; i >= 0; i--) {
        if (defs[i].extensions[0] != '\0' &&
            match_extensions(defs[i].extensions, b))
            return &defs[i];
    }

    return NULL;
}

int syntax_count(void)
{
    return def_count;
}

const SyntaxDef *syntax_at(int index)
{
    if (index < 0 || index >= def_count)
        return NULL;
    return &defs[index];
}

void syntax_state_reset(SyntaxState *state)
{
    if (state != NULL) {
        state->in_block_comment = 0;
        state->string_quote = 0;
    }
}

static int is_keyword(const SyntaxDef *def, const char *s, int len)
{
    int i;
    if (def == NULL)
        return 0;
    for (i = 0; i < def->keyword_count; i++) {
        if (word_eq(def, s, len, def->keywords[i]))
            return 1;
    }
    return 0;
}

static int is_string_quote(const SyntaxDef *def, int ch)
{
    if (def == NULL)
        return 0;
    return strchr(def->strings, ch) != NULL;
}

static void emit_span(SyntaxSpanCallback cb, int start, int end,
                      int style, void *user)
{
    if (cb != NULL && end > start)
        cb(start, end - start, style, user);
}

void syntax_highlight_line(const SyntaxDef *def, const char *line,
                           SyntaxState *state,
                           SyntaxSpanCallback callback, void *user)
{
    int i;
    int len;

    if (def == NULL || line == NULL || state == NULL)
        return;

    i = 0;
    len = (int)strlen(line);

    while (i < len) {
        int start;

        start = i;

        if (state->in_block_comment && def->block_end[0] != '\0') {
            const char *p;
            p = strstr(line + i, def->block_end);
            if (p == NULL) {
                emit_span(callback, i, len, SYNTAX_STYLE_COMMENT, user);
                return;
            }
            i = (int)(p - line) + (int)strlen(def->block_end);
            emit_span(callback, start, i, SYNTAX_STYLE_COMMENT, user);
            state->in_block_comment = 0;
            continue;
        }

        if (def->line_comment[0] != '\0' &&
            !strncmp(line + i, def->line_comment,
                     strlen(def->line_comment))) {
            emit_span(callback, i, len, SYNTAX_STYLE_COMMENT, user);
            return;
        }

        if (def->block_start[0] != '\0' &&
            !strncmp(line + i, def->block_start,
                     strlen(def->block_start))) {
            const char *p;
            int after;
            after = i + (int)strlen(def->block_start);
            p = strstr(line + after, def->block_end);
            if (p == NULL) {
                emit_span(callback, i, len, SYNTAX_STYLE_COMMENT, user);
                state->in_block_comment = 1;
                return;
            }
            i = (int)(p - line) + (int)strlen(def->block_end);
            emit_span(callback, start, i, SYNTAX_STYLE_COMMENT, user);
            continue;
        }

        if (def->preprocessor[0] != '\0') {
            int j;
            j = 0;
            while (j < len && (line[j] == ' ' || line[j] == '\t'))
                j++;
            if (i == j && !strncmp(line + i, def->preprocessor,
                                   strlen(def->preprocessor))) {
                emit_span(callback, i, len, SYNTAX_STYLE_PREPROC, user);
                return;
            }
        }

        if (is_string_quote(def, (unsigned char)line[i])) {
            int q;
            q = (unsigned char)line[i++];
            while (i < len) {
                if (def->escape_strings && line[i] == '\\' && i + 1 < len) {
                    i += 2;
                } else if ((unsigned char)line[i] == q) {
                    if (def->doubled_quotes && i + 1 < len &&
                        (unsigned char)line[i + 1] == q) {
                        i += 2;
                    } else {
                        i++;
                        break;
                    }
                } else {
                    i++;
                }
            }
            emit_span(callback, start, i, SYNTAX_STYLE_STRING, user);
            continue;
        }

        if (def->numbers &&
            (isdigit((unsigned char)line[i]) ||
             (line[i] == '-' && i + 1 < len &&
              isdigit((unsigned char)line[i + 1])))) {
            i++;
            while (i < len &&
                  (isalnum((unsigned char)line[i]) || line[i] == '.' ||
                   line[i] == '_' || line[i] == '+' || line[i] == '-'))
                i++;
            emit_span(callback, start, i, SYNTAX_STYLE_NUMBER, user);
            continue;
        }

        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            i++;
            while (i < len &&
                  (isalnum((unsigned char)line[i]) || line[i] == '_'))
                i++;
            if (is_keyword(def, line + start, i - start))
                emit_span(callback, start, i, SYNTAX_STYLE_KEYWORD, user);
            continue;
        }

        i++;
    }
}