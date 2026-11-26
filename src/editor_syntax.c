#include "cui_internal.h"

cui__lang cui__builtin_lang_for(const char *language);
int cui__builtin_tokenize(const cui__lang *, bool, const char *, int, bool *, cui__tok *, int);
int cui__builtin_tokenize_lang(const char *, const char *, int, bool *, cui__tok *, int);

static const char *const java_keywords[] = {
    "abstract", "assert", "break", "case", "catch", "class", "const", "continue", "default", "do", "else",
    "enum", "exports", "extends", "final", "finally", "for", "goto", "if", "implements", "import",
    "instanceof", "interface", "module", "native", "new", "open", "opens", "package", "permits", "private",
    "protected", "provides", "public", "record", "requires", "return", "sealed", "static", "strictfp", "super",
    "switch", "synchronized", "this", "throw", "throws", "to", "transient", "transitive", "try", "uses",
    "var", "volatile", "when", "while", "with", "yield", NULL
};
static const char *const java_types[] = {
    "boolean", "byte", "char", "double", "float", "int", "long", "short", "void", NULL
};
static const char *const kotlin_keywords[] = {
    "abstract", "actual", "annotation", "as", "break", "by", "catch", "class", "companion", "const",
    "constructor", "continue", "crossinline", "data", "delegate", "do", "dynamic", "else", "enum",
    "expect", "external", "final", "finally", "for", "fun", "get", "if", "import", "in", "infix",
    "init", "inline", "inner", "interface", "internal", "is", "lateinit", "noinline", "object", "open",
    "operator", "out", "override", "package", "private", "protected", "public", "reified", "return",
    "sealed", "set", "super", "suspend", "tailrec", "this", "throw", "try", "typealias", "val", "var",
    "vararg", "value", "when", "where", "while", NULL
};
static const char *const kotlin_types[] = {"Any", "Nothing", "Unit", NULL};

/* Token kinds match the palette in the pinned CUI's cui_code.c. */
enum { TK_TEXT, TK_KEYWORD, TK_TYPE, TK_STRING, TK_NUMBER, TK_COMMENT, TK_FUNC, TK_PREPROC, TK_PUNCT, TK_CONST };
static const char *const yaml_marker[] = {NULL};

static bool yaml_language(const char *name) {
    return name && (!strcmp(name, "yaml") || !strcmp(name, "yml"));
}

static bool yaml_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static int yaml_skip_space(const char *line, int at, int end) {
    while (at < end && yaml_space(line[at])) at++;
    return at;
}

static bool yaml_flow(char c) { return c == '[' || c == ']' || c == '{' || c == '}' || c == ','; }

static bool yaml_separated(const char *line, int at, int end, bool flow) {
    return at == end || yaml_space(line[at]) || (flow && yaml_flow(line[at]));
}

static int yaml_quoted_end(const char *line, int at, int end) {
    char quote = line[at++];
    while (at < end) {
        char c = line[at++];
        if (at < end && ((quote == '"' && c == '\\') || (c == '\'' && quote == c && line[at] == c))) at++;
        else if (c == quote) break;
    }
    return at;
}

static bool yaml_digit(char c) { return c >= '0' && c <= '9'; }

static bool yaml_number(const char *value, int length) {
    int i = 0, digits = 0;
    if (i < length && (value[i] == '-' || value[i] == '+')) i++;
    if (i == 0 && length > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'o')) {
        bool hex = value[1] == 'x';
        for (i = 2; i < length; i++) {
            char c = value[i];
            if (c >= '0' && c <= (hex ? '9' : '7')) continue;
            if (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) continue;
            return false;
        }
        return true;
    }
    while (i < length && yaml_digit(value[i])) { i++; digits++; }
    if (i < length && value[i] == '.') {
        i++;
        while (i < length && yaml_digit(value[i])) { i++; digits++; }
    }
    if (!digits) return false;
    if (i < length && (value[i] == 'e' || value[i] == 'E')) {
        i++;
        if (i < length && (value[i] == '+' || value[i] == '-')) i++;
        int exponent = i;
        while (i < length && yaml_digit(value[i])) i++;
        if (exponent == i) return false;
    }
    return i == length;
}

static int yaml_scalar_kind(const char *value, int length) {
    static const char *const constants[] = {
        "~", "null", "Null", "NULL", "true", "True", "TRUE", "false", "False", "FALSE",
        ".inf", ".Inf", ".INF", ".nan", ".NaN", ".NAN", NULL
    };
    if (yaml_number(value, length)) return TK_NUMBER;
    for (int i = 0; constants[i]; i++)
        if (strlen(constants[i]) == (size_t)length && !memcmp(value, constants[i], (size_t)length)) return TK_CONST;
    return TK_TEXT;
}

static int yaml_tokenize(const char *line, int length, cui__tok *tokens, int capacity) {
    int at = 0, count = 0, flow = 0;
    while (at < length && count < capacity) {
        int start = at;
        char c = line[at++];
        int kind = TK_TEXT;
        if (yaml_space(c)) {
            /* Keep tabs separate for CUI's code-block renderer. */
            while (at < length && line[at] == c) at++;
        } else if (c == '#') {
            at = length;
            kind = TK_COMMENT;
        } else if (start == 0 && length >= 3 && (!memcmp(line, "---", 3) || !memcmp(line, "...", 3))
                   && yaml_separated(line, 3, length, false)) {
            at = 3;
            kind = TK_PREPROC;
        } else if (start == 0 && c == '%') {
            while (at < length && !(line[at] == '#' && yaml_space(line[at - 1]))) at++;
            kind = TK_PREPROC;
        } else if (c == '"' || c == '\'') {
            at = yaml_quoted_end(line, start, length);
            int tail = yaml_skip_space(line, at, length);
            bool key = tail < length && line[tail] == ':' &&
                (flow || yaml_separated(line, tail + 1, length, false));
            kind = key ? TK_FUNC : TK_STRING;
        } else if (yaml_flow(c) || (c == ':' && flow) ||
                   ((c == ':' || c == '-' || c == '?') && yaml_separated(line, at, length, flow > 0))) {
            if (c == '[' || c == '{') flow++;
            if (flow && (c == ']' || c == '}')) flow--;
            kind = TK_PUNCT;
        } else if (c == '&' || c == '*' || c == '!' || c == '|' || c == '>') {
            while (at < length && !yaml_separated(line, at, length, flow > 0)) at++;
            kind = c == '|' || c == '>' ? TK_KEYWORD : TK_TYPE;
        } else {
            at = start;
            bool key = false;
            while (at < length) {
                char next = line[at];
                if (next == ':' && yaml_separated(line, at + 1, length, flow > 0)) { key = true; break; }
                if ((flow && yaml_flow(next)) || (next == '#' && at > start && yaml_space(line[at - 1]))) break;
                at++;
            }
            while (at > start && yaml_space(line[at - 1])) at--;
            kind = key ? TK_FUNC : yaml_scalar_kind(line + start, at - start);
        }
        tokens[count++] = (cui__tok){start, at - start, (uint8_t)kind};
    }
    return count;
}

cui__lang cui__lang_for(const char *language) {
    if (yaml_language(language)) {
        /* This marker travels with the language descriptor in the editor. */
        cui__lang syntax = cui__builtin_lang_for("text");
        syntax.kw = yaml_marker;
        return syntax;
    }
    if (language && !strcmp(language, "java"))
        return (cui__lang){.kw=java_keywords, .types=java_types, .line_comment="//", .block_comments=true};
    if (language && !strcmp(language, "kotlin"))
        return (cui__lang){.kw=kotlin_keywords, .types=kotlin_types, .line_comment="//", .block_comments=true, .backtick=true};
    return cui__builtin_lang_for(language);
}

int cui__tokenize(const cui__lang *syntax, bool json, const char *text, int bytes, bool *block, cui__tok *tokens, int capacity) {
    if (syntax->kw == yaml_marker) return yaml_tokenize(text, bytes, tokens, capacity);
    return cui__builtin_tokenize(syntax, json, text, bytes, block, tokens, capacity);
}

int cui__tokenize_lang(const char *language,const char *text,int bytes,bool *block,cui__tok *tokens,int capacity) {
    if (yaml_language(language) || (language && (!strcmp(language,"java") || !strcmp(language,"kotlin")))) {
        cui__lang syntax=cui__lang_for(language);
        return cui__tokenize(&syntax,false,text,bytes,block,tokens,capacity < 256 ? capacity : 256);
    }
    return cui__builtin_tokenize_lang(language,text,bytes,block,tokens,capacity);
}
