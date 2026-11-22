#include "cui_internal.h"

cui__lang cui__builtin_lang_for(const char *language);
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

cui__lang cui__lang_for(const char *language) {
    if (language && !strcmp(language, "java"))
        return (cui__lang){.kw=java_keywords, .types=java_types, .line_comment="//", .block_comments=true};
    if (language && !strcmp(language, "kotlin"))
        return (cui__lang){.kw=kotlin_keywords, .types=kotlin_types, .line_comment="//", .block_comments=true, .backtick=true};
    return cui__builtin_lang_for(language);
}

int cui__tokenize_lang(const char *language,const char *text,int bytes,bool *block,cui__tok *tokens,int capacity) {
    if (language && (!strcmp(language,"java") || !strcmp(language,"kotlin"))) {
        cui__lang syntax=cui__lang_for(language);
        return cui__tokenize(&syntax,false,text,bytes,block,tokens,capacity < 256 ? capacity : 256);
    }
    return cui__builtin_tokenize_lang(language,text,bytes,block,tokens,capacity);
}
