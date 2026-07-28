#ifndef JOY_SCRIPT_PARSE_H
#define JOY_SCRIPT_PARSE_H
/*
 * Scene-script parser for the bench joystick driver.
 *
 * Script grammar (one directive per line; '#' starts a comment, blank lines
 * are ignored):
 *
 *     settle <ms>          how long to wait after the LAST step before the
 *                          scene is declared settled (optional, default 0)
 *     <at_ms> <mask>       hold <mask> from <at_ms> onward. <at_ms> is
 *                          milliseconds from driver start and must be
 *                          non-decreasing. <mask> accepts decimal or 0x hex.
 *
 * Mask bits (mister_joy_shm.h):
 *   bit0=right bit1=left bit2=down bit3=up
 *   bit4=Sword bit5=Action bit6=Item1 bit7=Item2 bit8=Pause
 */
#include <stdint.h>
#include <stddef.h>

#define JOY_SCRIPT_MAX_STEPS 256
#define JOY_SCRIPT_MASK_MAX  0x1FFu

typedef struct { uint32_t at_ms; uint32_t mask; } JoyScriptStep;

typedef struct {
    JoyScriptStep steps[JOY_SCRIPT_MAX_STEPS];
    size_t        n;
    uint32_t      settle_ms;
} JoyScript;

/* Error codes (all negative). *err_line receives the 1-based offending line,
 * EXCEPT for JOY_SCRIPT_ERR_ARGS, which has no line context (the failure is
 * in the arguments to JoyScript_ParseText() itself, before any line is
 * read); that path sets *err_line to 0 as a sentinel. For every other error
 * *err_line is always set, so 0 is never ambiguous with a real line number
 * (lines are 1-based). In all cases *err_line is only written when err_line
 * is non-NULL. */
#define JOY_SCRIPT_ERR_ARGS       (-1)
#define JOY_SCRIPT_ERR_LONG_LINE  (-2)
#define JOY_SCRIPT_ERR_SETTLE     (-3)
#define JOY_SCRIPT_ERR_SYNTAX     (-4)
#define JOY_SCRIPT_ERR_MASK_RANGE (-5)
#define JOY_SCRIPT_ERR_NOT_MONO   (-6)
#define JOY_SCRIPT_ERR_TOO_MANY   (-7)
/* A numeric literal (settle <ms>, <at_ms>, or <mask>) does not fit in a
 * uint32_t: either strtoul() reported ERANGE, or (on a host where
 * `unsigned long` is wider than 32 bits, e.g. amd64/arm64) the parsed value
 * exceeds UINT32_MAX even though strtoul() didn't itself overflow. Distinct
 * from JOY_SCRIPT_ERR_MASK_RANGE, which is a <mask> that fits in a uint32_t
 * but falls outside the documented 0x000-0x1FF button range. */
#define JOY_SCRIPT_ERR_RANGE      (-8)

int JoyScript_ParseText(const char *text, JoyScript *out, int *err_line);

#endif /* JOY_SCRIPT_PARSE_H */
