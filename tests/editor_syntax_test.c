#include "workspace.h"
#include "cui_internal.h"
#include <assert.h>

enum { EXPECTED_WORD = 0, EXPECTED_KEYWORD = 1, EXPECTED_TYPE = 2, EXPECTED_STRING = 3, EXPECTED_NUMBER = 4, EXPECTED_COMMENT = 5, EXPECTED_KEY = 6, EXPECTED_DIRECTIVE = 7, EXPECTED_CONSTANT = 9 };

static void expect(const char *language, const char *line, const char *lexeme, int kind) {
    cui__tok editor[128], diff[128];
    cui__lang syntax = cui__lang_for(language);
    bool block = false;
    int length = (int)strlen(line);
    int count = cui__tokenize(&syntax, false, line, length, &block, editor, 128);
    block = false;
    assert(count == cui__tokenize_lang(language, line, length, &block, diff, 128));
    int position = 0;
    bool found = false;
    for (int i = 0; i < count; i++) {
        assert(editor[i].start == diff[i].start && editor[i].len == diff[i].len && editor[i].kind == diff[i].kind);
        assert(editor[i].start == position && editor[i].len > 0);
        position += editor[i].len;
        if (editor[i].kind == kind && editor[i].len == (int)strlen(lexeme) &&
            !memcmp(line + editor[i].start, lexeme, (size_t)editor[i].len)) found = true;
    }
    assert(position == length && found);
}

int main(void) {
    assert(!strcmp(ced_language(".github/workflows/ci.yaml"), "yaml"));
    assert(!strcmp(ced_language("config.yml"), "yaml"));
    assert(!strcmp(ced_language("server.mjs"), "js"));
    expect("yaml", "%YAML 1.2", "%YAML 1.2", EXPECTED_DIRECTIVE);
    expect("yaml", "---", "---", EXPECTED_DIRECTIVE);
    expect("yaml", "  - chave composta: false", "chave composta", EXPECTED_KEY);
    expect("yaml", "ação: ativo", "ação", EXPECTED_KEY);
    expect("yaml", "retries: -2.5e+3", "-2.5e+3", EXPECTED_NUMBER);
    expect("yaml", "values: [false, null, 0x2a]", "false", EXPECTED_CONSTANT);
    expect("yaml", "values: [false, null, 0x2a]", "0x2a", EXPECTED_NUMBER);
    expect("yaml", "url: https://example.test/a#section # note", "https://example.test/a#section", EXPECTED_WORD);
    expect("yaml", "url: https://example.test/a#section # note", "# note", EXPECTED_COMMENT);
    expect("yaml", "name: 'O''Brien # keep'", "'O''Brien # keep'", EXPECTED_STRING);
    expect("yaml", "value: \"escaped \\\" # keep\"", "\"escaped \\\" # keep\"", EXPECTED_STRING);
    expect("yaml", "{\"quoted-key\":true}", "\"quoted-key\"", EXPECTED_KEY);
    expect("yaml", "defaults: &base", "&base", EXPECTED_TYPE);
    expect("yaml", "copy: *base", "*base", EXPECTED_TYPE);
    expect("yaml", "description: |-", "|-", EXPECTED_KEYWORD);
    expect(ced_language("server.mjs"), "export const name = 'CED';", "export", EXPECTED_KEYWORD);
    return 0;
}
