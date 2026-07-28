#include "joy_script_parse.h"
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    return p;
}

int JoyScript_ParseText(const char *text, JoyScript *out, int *err_line) {
    if (!text || !out) return JOY_SCRIPT_ERR_ARGS;
    memset(out, 0, sizeof(*out));

    const char *p = text;
    int   line_no = 0;
    long  last_ms = -1;

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

        if (strncmp(s, "settle", 6) == 0 && (s[6] == ' ' || s[6] == '\t')) {
            char *end;
            unsigned long v = strtoul(s + 6, &end, 10);
            if (end == s + 6 || *skip_ws(end) != '\0') {
                if (err_line) *err_line = line_no;
                return JOY_SCRIPT_ERR_SETTLE;
            }
            out->settle_ms = (uint32_t)v;
            continue;
        }

        char *end;
        unsigned long at = strtoul(s, &end, 10);
        if (end == s) {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_SYNTAX;
        }
        const char *m = skip_ws(end);
        if (*m == '\0') {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_SYNTAX;
        }
        unsigned long mask = strtoul(m, &end, 0);   /* base 0: accepts 0x hex */
        if (end == m || *skip_ws(end) != '\0') {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_SYNTAX;
        }
        if (mask > JOY_SCRIPT_MASK_MAX) {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_MASK_RANGE;
        }
        if ((long)at < last_ms) {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_NOT_MONO;
        }
        if (out->n >= JOY_SCRIPT_MAX_STEPS) {
            if (err_line) *err_line = line_no;
            return JOY_SCRIPT_ERR_TOO_MANY;
        }
        last_ms = (long)at;
        out->steps[out->n].at_ms = (uint32_t)at;
        out->steps[out->n].mask  = (uint32_t)mask;
        out->n++;
    }
    return 0;
}
