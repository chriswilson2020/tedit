#include <string.h>
#include <ctype.h>
#include "format.h"

static void trimmed_copy(const char *line, char *out, int outlen)
{
    const char *start;
    int len;

    start = line;
    while (*start && isspace((unsigned char)*start))
        start++;

    len = (int)strlen(start);
    while (len > 0 && isspace((unsigned char)start[len - 1]))
        len--;

    if (len >= outlen)
        len = outlen - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

static int char_equal(const SyntaxDef *def, int a, int b)
{
    if (def != NULL && def->case_insensitive) {
        a = tolower((unsigned char)a);
        b = tolower((unsigned char)b);
    }
    return a == b;
}

static int token_equal(const SyntaxDef *def, const char *a,
                       const char *b, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        if (!char_equal(def, a[i], b[i]))
            return 0;
    }
    return 1;
}

static int starts_rule(const SyntaxDef *def, const char *line,
                       const char *rule)
{
    char buf[1024];
    int rlen;
    trimmed_copy(line, buf, sizeof(buf));
    rlen = (int)strlen(rule);
    if ((int)strlen(buf) < rlen)
        return 0;
    if (!token_equal(def, buf, rule, rlen))
        return 0;
    if (isalnum((unsigned char)rule[rlen - 1]) &&
        (isalnum((unsigned char)buf[rlen]) || buf[rlen] == '_'))
        return 0;
    return 1;
}

static int ends_rule(const SyntaxDef *def, const char *line,
                     const char *rule)
{
    char buf[1024];
    int len;
    int rlen;
    int start;

    trimmed_copy(line, buf, sizeof(buf));
    len = (int)strlen(buf);
    rlen = (int)strlen(rule);
    if (rlen > len)
        return 0;
    start = len - rlen;
    if (!token_equal(def, buf + start, rule, rlen))
        return 0;
    if (start > 0 && isalnum((unsigned char)rule[0]) &&
        (isalnum((unsigned char)buf[start - 1]) || buf[start - 1] == '_'))
        return 0;
    return 1;
}

const char *format_comment_prefix(const SyntaxDef *def)
{
    if (def == NULL || def->line_comment[0] == '\0')
        return NULL;
    return def->line_comment;
}

int format_should_indent_after(const SyntaxDef *def, const char *line)
{
    int i;
    if (def == NULL)
        return 0;
    for (i = 0; i < def->indent_after_count; i++) {
        if (ends_rule(def, line, def->indent_after[i]))
            return 1;
    }
    return 0;
}

int format_should_outdent_before(const SyntaxDef *def, const char *line)
{
    int i;
    if (def == NULL)
        return 0;
    for (i = 0; i < def->outdent_before_count; i++) {
        if (starts_rule(def, line, def->outdent_before[i]))
            return 1;
    }
    return 0;
}

int format_outdent_char(const SyntaxDef *def, int ch)
{
    if (def == NULL || def->outdent_chars[0] == '\0')
        return 0;
    return strchr(def->outdent_chars, ch) != NULL;
}

int format_next_indent(const SyntaxDef *def, const char *line,
                       int current_indent, int tab_width)
{
    int indent;
    indent = current_indent;
    if (format_should_indent_after(def, line))
        indent += tab_width;
    if (indent < 0)
        indent = 0;
    return indent;
}