#ifndef TEDIT_FORMAT_H
#define TEDIT_FORMAT_H

#include "syntax.h"

const char *format_comment_prefix(const SyntaxDef *def);
int format_should_indent_after(const SyntaxDef *def, const char *line);
int format_should_outdent_before(const SyntaxDef *def, const char *line);
int format_outdent_char(const SyntaxDef *def, int ch);
int format_next_indent(const SyntaxDef *def, const char *line,
                       int current_indent, int tab_width);

#endif
