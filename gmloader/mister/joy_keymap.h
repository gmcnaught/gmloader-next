// joy_keymap.h -- drive the game's KEYBOARD from the MiSTer joystick mask.
//
// Some GameMaker titles are only playable through their keyboard bindings: Cursed
// Castilla EX's gamepad handling maps the d-pad onto actions (UP = jump, LEFT =
// attack, device-verified 2026-08-23 and 2026-09-27) even though every hop from
// MiSTer to yoyo_gamepads[] is correct. The PortMaster port sidesteps it the same
// way: gptokeyb turns the pad into keys and hacksdl hides the joystick
// (HACKSDL_DEVICE_DISABLE_0=1). This is that, for the MiSTer mask.
//
// GMLOADER_JOY_KEYMAP = comma-separated "bit=key" pairs, e.g.
//   "0=right,1=left,2=down,3=up,4=z,5=x,6=x,7=z,8=esc"
// bit is the MiSTer/OpenBOR joystick bit (0=R 1=L 2=D 3=U, 4.. = CONF_STR buttons);
// key is up/down/left/right/esc/enter/space/tab/backspace/shift/ctrl/alt, a letter
// a-z or a digit 0-9. Several bits may map to one key (it is down while any is).
// Header-only and SDL-free so the parser and the edge logic are host-testable.
#pragma once
#include <stdint.h>
#include <string.h>
#include <ctype.h>

enum { JOYKEY_MAX_BITS = 32 };
enum JoyKey {
    JK_NONE = 0, JK_UP, JK_DOWN, JK_LEFT, JK_RIGHT, JK_ESC, JK_ENTER, JK_SPACE, JK_TAB,
    JK_BACKSPACE, JK_SHIFT, JK_CTRL, JK_ALT,
    JK_CHAR_BASE = 256   // JK_CHAR_BASE + 'a'..'z' / '0'..'9'
};

struct JoyKeymap {
    int n;                        // number of valid pairs
    uint8_t bit[JOYKEY_MAX_BITS];
    int key[JOYKEY_MAX_BITS];
    uint32_t prev;                // key-down set last step, as a bitmask over pair index
};

static inline int joykey_parse_key(const char *s, size_t len) {
    char w[16];
    if (len == 0 || len >= sizeof w) return JK_NONE;
    for (size_t i = 0; i < len; i++) w[i] = (char)tolower((unsigned char)s[i]);
    w[len] = 0;
    if (len == 1 && ((w[0] >= 'a' && w[0] <= 'z') || (w[0] >= '0' && w[0] <= '9')))
        return JK_CHAR_BASE + w[0];
    static const struct { const char *name; int key; } names[] = {
        {"up", JK_UP}, {"down", JK_DOWN}, {"left", JK_LEFT}, {"right", JK_RIGHT},
        {"esc", JK_ESC}, {"escape", JK_ESC}, {"enter", JK_ENTER}, {"return", JK_ENTER},
        {"space", JK_SPACE}, {"tab", JK_TAB}, {"backspace", JK_BACKSPACE},
        {"shift", JK_SHIFT}, {"ctrl", JK_CTRL}, {"control", JK_CTRL}, {"alt", JK_ALT},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(w, names[i].name) == 0) return names[i].key;
    return JK_NONE;
}

// Returns the number of pairs parsed; 0 (map disabled) on any malformed pair, so a
// typo never half-applies a map.
static inline int joykey_parse(JoyKeymap *m, const char *spec) {
    memset(m, 0, sizeof *m);
    if (!spec || !*spec) return 0;
    const char *p = spec;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        if (!isdigit((unsigned char)*p)) { m->n = 0; return 0; }
        int b = 0;
        while (isdigit((unsigned char)*p)) b = b * 10 + (*p++ - '0');
        if (*p != '=' || b >= JOYKEY_MAX_BITS || m->n >= JOYKEY_MAX_BITS) { m->n = 0; return 0; }
        const char *k = ++p;
        while (*p && *p != ',' && *p != ' ') p++;
        int key = joykey_parse_key(k, (size_t)(p - k));
        if (key == JK_NONE) { m->n = 0; return 0; }
        m->bit[m->n] = (uint8_t)b; m->key[m->n] = key; m->n++;
    }
    return m->n;
}

// One poll: for every distinct key in the map, report a press/release via cb when its
// state changes (down = any mapped bit set in `mask`). Keys are reported once per
// change, never repeated while held.
template <typename Cb>
static inline void joykey_step(JoyKeymap *m, uint32_t mask, Cb cb) {
    uint32_t now = 0;
    for (int i = 0; i < m->n; i++) {
        // A key's state is owned by its FIRST pair index; later pairs for the same key
        // OR into it.
        int owner = i;
        for (int j = 0; j < i; j++) if (m->key[j] == m->key[i]) { owner = j; break; }
        if (mask & (1u << m->bit[i])) now |= 1u << owner;
    }
    for (int i = 0; i < m->n; i++) {
        bool first = true;
        for (int j = 0; j < i; j++) if (m->key[j] == m->key[i]) { first = false; break; }
        if (!first) continue;
        const bool was = (m->prev >> i) & 1u, is = (now >> i) & 1u;
        if (was != is) cb(m->key[i], is ? 1 : 0);
    }
    m->prev = now;
}
