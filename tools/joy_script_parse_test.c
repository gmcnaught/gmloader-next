#include "joy_script_parse.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Builds "0 0" followed by spaces, padded to exactly total_len characters,
 * then a newline. Used to probe the exact 160-byte line-buffer boundary
 * without the padding itself being a source of syntax errors (trailing
 * whitespace after a valid "at mask" pair is skipped, not rejected). */
static void build_padded_line(char *buf, int total_len) {
    memset(buf, ' ', (size_t)total_len);
    buf[0] = '0';
    buf[1] = ' ';
    buf[2] = '0';
    buf[total_len] = '\n';
    buf[total_len + 1] = '\0';
}

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

    /* 11. JOY_SCRIPT_ERR_ARGS: NULL text or NULL out is reported, and unlike
     *     every other error path it has no line context — *err_line must be
     *     set to a sentinel (0, since real lines are 1-based) rather than
     *     left untouched. */
    line = 999;
    CHECK(JoyScript_ParseText(NULL, &js, &line) == JOY_SCRIPT_ERR_ARGS);
    CHECK(line == 0);
    line = 999;
    CHECK(JoyScript_ParseText("0 0\n", NULL, &line) == JOY_SCRIPT_ERR_ARGS);
    CHECK(line == 0);
    /* NULL err_line must not crash on the ARGS path either. */
    CHECK(JoyScript_ParseText(NULL, &js, NULL) == JOY_SCRIPT_ERR_ARGS);

    /* 12. JOY_SCRIPT_ERR_LONG_LINE: the parser copies each line into a
     *     160-byte stack buffer. 159 content chars is the largest line that
     *     fits (159 chars + NUL == 160); 160 and 161 must both be rejected. */
    {
        char buf[300];
        build_padded_line(buf, 159);
        CHECK(JoyScript_ParseText(buf, &js, &line) == 0);
        CHECK(js.n == 1 && js.steps[0].at_ms == 0u && js.steps[0].mask == 0u);

        build_padded_line(buf, 160);
        CHECK(JoyScript_ParseText(buf, &js, &line) == JOY_SCRIPT_ERR_LONG_LINE);
        CHECK(line == 1);

        build_padded_line(buf, 161);
        CHECK(JoyScript_ParseText(buf, &js, &line) == JOY_SCRIPT_ERR_LONG_LINE);
        CHECK(line == 1);
    }

    /* 13. JOY_SCRIPT_ERR_SETTLE: bare "settle" with no value must not fall
     *     through to the generic at/mask branch and report a generic syntax
     *     error — it must report SETTLE specifically. Same for a value that
     *     doesn't parse as a number. */
    CHECK(JoyScript_ParseText("settle\n", &js, &line) == JOY_SCRIPT_ERR_SETTLE);
    CHECK(line == 1);
    CHECK(JoyScript_ParseText("settle abc\n", &js, &line) == JOY_SCRIPT_ERR_SETTLE);
    CHECK(line == 1);
    CHECK(JoyScript_ParseText("settle -5\n", &js, &line) == JOY_SCRIPT_ERR_SETTLE);
    CHECK(line == 1);

    /* 14. JOY_SCRIPT_ERR_TOO_MANY: JOY_SCRIPT_MAX_STEPS (256) steps parse
     *     cleanly; a 257th step must be rejected rather than silently
     *     dropped or overflowing the fixed-size steps array. */
    {
        char big[JOY_SCRIPT_MAX_STEPS * 8 + 64];
        char *w = big;
        int i;
        for (i = 0; i < JOY_SCRIPT_MAX_STEPS; i++)
            w += sprintf(w, "%d 0\n", i);
        CHECK(JoyScript_ParseText(big, &js, &line) == 0);
        CHECK(js.n == JOY_SCRIPT_MAX_STEPS);

        w += sprintf(w, "%d 0\n", JOY_SCRIPT_MAX_STEPS);
        CHECK(JoyScript_ParseText(big, &js, &line) == JOY_SCRIPT_ERR_TOO_MANY);
        CHECK(line == JOY_SCRIPT_MAX_STEPS + 1);
    }

    /* 15. Overflow of a numeric literal beyond uint32_t range is diagnosed
     *     as JOY_SCRIPT_ERR_RANGE, not silently truncated. Covers at_ms,
     *     mask, and settle_ms. */
    CHECK(JoyScript_ParseText("4294967296 0\n", &js, &line) == JOY_SCRIPT_ERR_RANGE);
    CHECK(line == 1);
    CHECK(JoyScript_ParseText("0 4294967296\n", &js, &line) == JOY_SCRIPT_ERR_RANGE);
    CHECK(line == 1);
    CHECK(JoyScript_ParseText("settle 4294967296\n", &js, &line) == JOY_SCRIPT_ERR_RANGE);
    CHECK(line == 1);

    /* 16. Explicit sign prefixes are rejected outright on at_ms and mask —
     *     the grammar has no signed values. "-1" must NOT silently become
     *     UINT32_MAX, and "+8" must NOT silently become 8. */
    CHECK(JoyScript_ParseText("-1 0\n", &js, &line) == JOY_SCRIPT_ERR_SYNTAX);
    CHECK(line == 1);
    CHECK(JoyScript_ParseText("0 +8\n", &js, &line) == JOY_SCRIPT_ERR_SYNTAX);
    CHECK(line == 1);
    /* A large-magnitude negative mask must not be accepted at all, let
     * alone wrap into the valid 0..0x1FF range on a 32-bit target. */
    CHECK(JoyScript_ParseText("0 -4294967295\n", &js, &line) == JOY_SCRIPT_ERR_SYNTAX);
    CHECK(line == 1);

    /* 17. NULL err_line must not crash on ordinary (non-ARGS) error paths
     *     either. */
    CHECK(JoyScript_ParseText("0 0x200\n", &js, NULL) == JOY_SCRIPT_ERR_MASK_RANGE);

    /* 18. CRLF line endings: the trailing '\r' before each '\n' must be
     *     treated as whitespace, not as trailing garbage or part of the
     *     token. */
    CHECK(JoyScript_ParseText("0 0\r\n100 8\r\n", &js, &line) == 0);
    CHECK(js.n == 2);
    CHECK(js.steps[0].at_ms == 0u   && js.steps[0].mask == 0u);
    CHECK(js.steps[1].at_ms == 100u && js.steps[1].mask == 8u);

    if (fails == 0) printf("joy_script_parse_test: all checks passed\n");
    return fails != 0;
}
