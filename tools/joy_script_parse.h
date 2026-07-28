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

/* Error codes (all negative). *err_line receives the 1-based offending line. */
#define JOY_SCRIPT_ERR_ARGS       (-1)
#define JOY_SCRIPT_ERR_LONG_LINE  (-2)
#define JOY_SCRIPT_ERR_SETTLE     (-3)
#define JOY_SCRIPT_ERR_SYNTAX     (-4)
#define JOY_SCRIPT_ERR_MASK_RANGE (-5)
#define JOY_SCRIPT_ERR_NOT_MONO   (-6)
#define JOY_SCRIPT_ERR_TOO_MANY   (-7)

int JoyScript_ParseText(const char *text, JoyScript *out, int *err_line);

#endif /* JOY_SCRIPT_PARSE_H */
