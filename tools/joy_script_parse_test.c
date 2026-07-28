#include "joy_script_parse.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    JoyScript js; int line = 0;

    /* 1. A minimal two-step script parses. */
    CHECK(JoyScript_ParseText("0 0\n1500 0x008\n", &js, &line) == 0);
    CHECK(js.n == 2);
    CHECK(js.steps[0].at_ms == 0u   && js.steps[0].mask == 0x000u);
    CHECK(js.steps[1].at_ms == 1500u && js.steps[1].mask == 0x008u);

    /* 2. Comments, blank lines and indentation are ignored. */
    CHECK(JoyScript_ParseText("# lead-in\n\n   \n  200 16  \n", &js, &line) == 0);
    CHECK(js.n == 1);
    CHECK(js.steps[0].at_ms == 200u && js.steps[0].mask == 16u);

    /* 3. The settle directive is captured and is not a step. */
    CHECK(JoyScript_ParseText("0 0\nsettle 3000\n", &js, &line) == 0);
    CHECK(js.n == 1);
    CHECK(js.settle_ms == 3000u);

    /* 4. Non-decreasing timestamps are required: a benchmark whose steps run
     *    out of order lands on a different scene than the file describes. */
    CHECK(JoyScript_ParseText("1000 0\n999 0\n", &js, &line) == JOY_SCRIPT_ERR_NOT_MONO);
    CHECK(line == 2);

    /* 5. Equal timestamps ARE allowed (two presses on the same tick). */
    CHECK(JoyScript_ParseText("100 1\n100 2\n", &js, &line) == 0);
    CHECK(js.n == 2);

    /* 6. Masks above bit8 are rejected — they would silently set no button. */
    CHECK(JoyScript_ParseText("0 0x200\n", &js, &line) == JOY_SCRIPT_ERR_MASK_RANGE);
    CHECK(line == 1);

    /* 7. A line missing its mask is a syntax error, not a zero mask. */
    CHECK(JoyScript_ParseText("0\n", &js, &line) == JOY_SCRIPT_ERR_SYNTAX);
    CHECK(line == 1);

    /* 8. Trailing garbage is rejected rather than silently ignored. */
    CHECK(JoyScript_ParseText("0 1 oops\n", &js, &line) == JOY_SCRIPT_ERR_SYNTAX);

    /* 9. An empty script parses to zero steps (the caller rejects it). */
    CHECK(JoyScript_ParseText("# nothing\n", &js, &line) == 0);
    CHECK(js.n == 0);

    /* 10. A file with no trailing newline still parses its last line. */
    CHECK(JoyScript_ParseText("0 0\n50 4", &js, &line) == 0);
    CHECK(js.n == 2 && js.steps[1].mask == 4u);

    if (fails == 0) printf("joy_script_parse_test: all checks passed\n");
    return fails != 0;
}
