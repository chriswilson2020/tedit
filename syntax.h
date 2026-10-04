#ifndef TEDIT_SYNTAX_H
#define TEDIT_SYNTAX_H

#define SYNTAX_NAME_LEN 64
#define SYNTAX_TEXT_LEN 512
#define SYNTAX_MAX_KEYWORDS 256
#define SYNTAX_WORD_LEN 64
#define SYNTAX_MAX_RULES 64

enum {
    SYNTAX_STYLE_NORMAL = 0,
    SYNTAX_STYLE_KEYWORD,
    SYNTAX_STYLE_STRING,
    SYNTAX_STYLE_COMMENT,
    SYNTAX_STYLE_NUMBER,
    SYNTAX_STYLE_PREPROC
};

typedef struct SyntaxDef {
    char name[SYNTAX_NAME_LEN];
    char extensions[SYNTAX_TEXT_LEN];
    char filenames[SYNTAX_TEXT_LEN];
    char line_comment[16];
    char block_start[16];
    char block_end[16];
    char strings[16];
    char preprocessor[16];
    int case_insensitive;
    int numbers;
    int escape_strings;
    int doubled_quotes;
    char keywords[SYNTAX_MAX_KEYWORDS][SYNTAX_WORD_LEN];
    int keyword_count;
    char indent_after[SYNTAX_MAX_RULES][SYNTAX_WORD_LEN];
    int indent_after_count;
    char outdent_before[SYNTAX_MAX_RULES][SYNTAX_WORD_LEN];
    int outdent_before_count;
    char outdent_chars[16];
} SyntaxDef;

typedef struct SyntaxState {
    int in_block_comment;
    char string_quote;
} SyntaxState;

typedef void (*SyntaxSpanCallback)(int start, int len, int style, void *user);

void syntax_init(void);
int syntax_load_dir(const char *path);
const SyntaxDef *syntax_detect(const char *filename);
int syntax_count(void);
const SyntaxDef *syntax_at(int index);
void syntax_state_reset(SyntaxState *state);
void syntax_highlight_line(const SyntaxDef *def, const char *line,
                           SyntaxState *state,
                           SyntaxSpanCallback callback, void *user);

#endif
