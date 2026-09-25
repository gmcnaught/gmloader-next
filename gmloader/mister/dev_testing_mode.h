/* Env-gated (GMLOADER_TESTING_MODE=1) switch for Maldita Castilla's own
 * developer mode, for reaching a later stage quickly when profiling.
 *
 * The game ships the code; it is just never enabled. obj_control's Step event
 * (decoded from the device build's bytecode) reads:
 *
 *   if (global.testing_mode == 1) {
 *       if keyboard_check_pressed(vk_backspace) game_restart();
 *       if keyboard_check_pressed(ord("J")) && room_next(room) != -1 {
 *           global.extra_live_activated = 1;
 *           global.blood_gem_activated = 1;
 *           room_goto_next();
 *       }
 *       if keyboard_check_pressed(ord("L")) ... lives ...
 *   }
 *
 * and scr_initial_values sets global.testing_mode = 0. This module re-asserts
 * it to 1 every step, and supplies the J key the MiSTer has no way to send
 * (the core's pad has five buttons and no keyboard):
 *
 *   - Item 1 + Item 2 pressed together = one J press (skip to the next room).
 *     Both buttons are hidden from the game while the chord is held.
 *   - GMLOADER_DEVSKIP_TO=<room index> presses J by itself, once every
 *     DEVSKIP_GAP steps, while GMLOADER_DEVSKIP_FROM (default 4, the first room
 *     after title_screen) <= room < target, then disarms. Room indices of the
 *     device build: map_2=11, level_2_1=12 .. level_2_4=15, map_3=18.
 *
 * Side effect of every skip, from the game's own code: extra_live_activated
 * and blood_gem_activated are set, so the arrival state is not the same as
 * reaching the room by play.
 *
 * Unset (the production case) installs nothing and DevTestingMode_Step
 * returns after one branch. */
#ifndef DEV_TESTING_MODE_H
#define DEV_TESTING_MODE_H
#include <stdint.h>

struct so_module;
void patch_dev_testing_mode(struct so_module *mod);

/* Once per update_inputs(), after the live pad masks are read and before they
 * become gamepad state. May clear the chord bits in masks[0]. */
void DevTestingMode_Step(uint32_t *mask_p0);

#endif /* DEV_TESTING_MODE_H */
