#include "joy_script_parse.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    return p;
}

/* Parses a bare (no explicit sign) unsigned integer token at s, using
 * strtoul() with the given base (0 lets strtoul() detect a "0x..." hex
 * literal; 10 forces decimal only). The result is checked to fit in a
 * uint32_t: strtoul()'s `unsigned long` accumulator is 64 bits wide on this
 * host (amd64/arm64) even though script fields are stored as uint32_t, so a
 * literal like "4294967296" would otherwise parse "successfully" and then
 * get silently truncated by the caller's cast. The same source also builds
 * for a 32-bit armhf target, where `unsigned long` IS uint32_t and overflow
 * instead surfaces via errno == ERANGE; both cases are checked here so the
 * behavior is identical on both widths.
 *
 * A leading '+' or '-' is rejected outright before strtoul() ever sees it:
 * the documented grammar has no signed values, and without this a negative
 * literal could either be silently accepted (e.g. "-1" -> UINT32_MAX, via
 * strtoul()'s unsigned-negation behavior) or, on a 32-bit target, wrap back
 * into an in-range value (e.g. mask "-4294967295" -> 1), evading the mask
 * range check entirely.
 *
 * Returns 1 on success (*out and *endp valid), 0 on syntax error (no digits,
 * or a leading sign), -1 if the literal doesn't fit in a uint32_t.
 */
static int parse_u32(const char *s, int base, uint32_t *out, const char **endp) {
    if (*s == '+' || *s == '-') return 0;
    errno = 0;
    char *end;
    unsigned long v = strtoul(s, &end, base);
    if (end == s) return 0;
    if (errno == ERANGE || v > (unsigned long)UINT32_MAX) return -1;
    *out = (uint32_t)v;
    *endp = end;
    return 1;
}

int JoyScript_ParseText(const char *text, JoyScript *out, int *err_line) {
    if (!text || !out) {
        /* No line has been read yet -- unlike every other error path, there
         * is no line context to report. 0 is never a valid line number
         * (lines are 1-based), so it is an unambiguous sentinel. */
        if (err_line) *err_line = 0;
        return JOY_SCRIPT_ERR_ARGS;
    }
    memset(out, 0, sizeof(*out));

    const char *p = text;
    int      line_no = 0;
    int      have_last = 0;
    uint32_t last_ms = 0;

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char buf[160];
        line_no++;
        if (len >= sizeof(buf)) {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_LONG_LINE;
        }
        memcpy(buf, p, len);
        buf[len] = '\0';
        p = eol ? eol + 1 : p + len;

        const char *s = skip_ws(buf);
        if (*s == '\0' || *s == '#') continue;

        /* "settle" as a distinct word: either followed by whitespace and a
         * value, or standing alone (s[6] == '\0'), which is malformed and
         * must be reported as ERR_SETTLE rather than falling through to the
         * generic at/mask branch below (which would report ERR_SYNTAX). A
         * longer identifier that merely starts with "settle" (e.g.
         * "settleforless 5") is intentionally NOT matched here, so it falls
         * through and is diagnosed as a syntax error on its own terms. */
        if (strncmp(s, "settle", 6) == 0 &&
            (s[6] == '\0' || s[6] == ' ' || s[6] == '\t')) {
            const char *v0 = skip_ws(s + 6);
            uint32_t v; const char *end;
            int r = parse_u32(v0, 10, &v, &end);
            if (r == -1) {
                if (err_line) *err_line = line_no;
                return JOY_SCRIPT_ERR_RANGE;
            }
            if (r == 0 || *skip_ws(end) != '\0') {
                if (err_line) *err_line = line_no;
                return JOY_SCRIPT_ERR_SETTLE;
            }
            out->settle_ms = v;
            continue;
        }

        uint32_t at; const char *end;
        int rat = parse_u32(s, 10, &at, &end);
        if (rat == -1) {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_RANGE;
        }
        if (rat == 0) {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_SYNTAX;
        }
        const char *m = skip_ws(end);
        if (*m == '\0') {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_SYNTAX;
        }
        uint32_t mask; const char *mend;
        int rmask = parse_u32(m, 0, &mask, &mend);   /* base 0: accepts 0x hex */
        if (rmask == -1) {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_RANGE;
        }
        if (rmask == 0 || *skip_ws(mend) != '\0') {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_SYNTAX;
        }
        if (mask > JOY_SCRIPT_MASK_MAX) {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_MASK_RANGE;
        }
        if (have_last && at < last_ms) {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_NOT_MONO;
        }
        if (out->n >= JOY_SCRIPT_MAX_STEPS) {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_TOO_MANY;
        }
        have_last = 1;
        last_ms = at;
        out->steps[out->n].at_ms = at;
        out->steps[out->n].mask  = mask;
        out->n++;
    }
    return 0;
}
