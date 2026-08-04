# PortMaster gmloader targets for MiSTer

A survey of the PortMaster library to pick the next games to bring up on the
MiSTer `MISTER_NATIVE_VIDEO` path, now that Maldita Castilla runs at 60 fps.

## How this list was built

1. Pulled `ports.json` from `PortsMaster/PortMaster-Info` (1375 ports).
2. Fetched every candidate launch script (`ports/<dir>/<Name>.sh`) for ports that are
   armhf or arch-unspecified, and kept the ones that actually exec `gmloader`.
   -> **184 gmloader ports**: 68 ready-to-run (game data bundled), 116 need owned assets.
3. Downloaded all 184 port zips and read, from inside each `.apk`/`.port`:
   - `GEN8` chunk of `game.droid`/`data.win`: bytecode version, GM version, window size
   - `ROOM` chunk: the first visible view's **viewport** size — this is the
     `application_surface` size, i.e. what the software blitter actually has to fill
   - `SHDR` chunk: **custom shader count** (0 = stock GM shaders only = "blit-everything" viable)
   - md5 of `lib/armeabi-v7a/libyoyo.so` — the runner binary `patch_libyoyo` hooks
   - presence of `_Z12Function_AddPKc` (modern, rehook applies) vs `_Z12Function_AddPc`
     (pre-2024.14, rehook must be skipped — see CLAUDE.md)

Selection criteria, in order of weight: same/legacy runner binary > small viewport
(fill rate dominates the software path) > zero custom shaders > free game data.

Caveat: 8 legacy ports had unresolvable script paths and are not in the census —
Cally's Caves 4, Neo-Gensokyo, Super Galaxy Squadron EX Turbo, CMDungeon,
Stop The Egg Eggspress, Billy Frontier, Descent, Descent II.

Reference point — **Maldita Castilla**: viewport 288x216 (62 kpx), bytecode 14,
GM 1.0.0.1522, 0 custom shaders, runner `bdb2a81afd`.

## Tier 1 - drop-ins (legacy GMS1.x runner, <=320x240, no custom shaders)

| Game | Viewport | kpx | Bytecode / GM ver | Runner md5 | Size | Notes |
|---|---|---|---|---|---|---|
| Verminian Trap | 320x240 | 77 | 13 / 1.0.0.1130 | `bdb2a81afd` | 8 MB | runner is **byte-identical** to Maldita Castilla; ships a `.port` |
| Gaurodan | 320x240 | 77 | 14 / 1.0.0.1474 | `079993760e` | 42 MB | Locomalito; `.apk` + `gamedata/` layout you already deploy |
| Smoothie Galaxy | 320x240 | 77 | 14 / 1.0.0.1657 | `da4afeac83` | 5 MB | smallest of the three; 1 shader |

**Verminian Trap is the recommended next target.** Same runner binary as the port
already at 60 fps, 320x240 viewport (8% more pixels than Maldita), zero custom
shaders, stock-GM draw pattern. Should be a config change, not a code change.

## Tier 2 - modern runner coverage, at or below Maldita's fill cost

These all use the modern `_Z12Function_AddPKc` runner, so they exercise the
reentrant `Function_Add` rehook path. Fill cost is <= ~1.3x Maldita, so any failure
is a clean runner-compat signal rather than a performance one. All ready-to-run.

`src` is `view` where the size came from the room's viewport, `window` where the
room parse failed and the GEN8 default window size is used instead.

| Game | Res | src | kpx | Shaders | Bytecode / GM ver | Runner md5 |
|---|---|---|---|---|---|---|
| NAi NAi Nights-Nokia Edition | 84x48 | view | 4 | 0 | 17 / 2.0.0.0 | `36c73ba10a` |
| Biomass: Growth | 160x144 | view | 23 | 0 | 16 / 1.0.0.9999 | `8691bb6734` |
| Bumpers & Broadswords Maximum | 160x160 | view | 26 | 0 | 17 / 2.0.0.0 | `8b1382893c` |
| Pocket Wonder Sport | 192x144 | view | 28 | 1 | 17 / 2.0.0.0 | `506dbfab56` |
| Bootleg Box | 192x160 | view | 31 | 0 | 17 / 2.0.0.0 | `7ea7e54956` |
| Bowling Cross | 192x192 | view | 37 | 0 | 17 / 2.0.0.0 | `7ea7e54956` |
| Bumpers & Broadswords | 192x192 | view | 37 | 0 | 17 / 2.0.0.0 | `8b1382893c` |
| I Gacha Head | 256x144 | window | 37 | 0 | 17 / 2.0.0.0 | `ac7f80b09a` |
| King of Machines | 256x160 | view | 41 | 0 | 17 / 2.0.0.0 | `506dbfab56` |
| Bumpers & Broadswords Tournament | 256x192 | view | 49 | 0 | 17 / 2.0.0.0 | `8b1382893c` |
| Botbreak | 192x256 | view | 49 | 0 | 17 / 2.0.0.0 | `7ea7e54956` |
| Cleaving Caliber | 256x240 | view | 61 | 0 | 17 / 2.0.0.0 | `8b1382893c` |
| Hunters | 256x256 | view | 66 | 0 | 17 / 2.0.0.0 | `8b1382893c` |
| Chess is Stupid | 320x240 | window | 77 | 0 | 17 / 2.0.0.0 | `506dbfab56` |
| Dead Ice Deluxe | 320x240 | window | 77 | 0 | 17 / 2.0.0.0 | `506dbfab56` |
| Swapwood Quest R | 320x240 | view | 77 | 0 | 17 / 2.0.0.0 | `a3c6ec15a4` |
| Goopty Goo | 384x216 | view | 83 | 1 | 17 / 2.0.0.0 | `db71adf997` |
| Livid Meadow | 384x216 | view | 83 | 1 | 16 / 1.0.0.9999 | `8691bb6734` |
| Operius | 426x240 | window | 102 | 1 | 17 / 2.0.0.0 | `fbd8591e3d` |

## Tier 3 - blitter stress / showcase

| Game | Viewport | kpx | Shaders | Bytecode / GM ver | Runner md5 |
|---|---|---|---|---|---|
| Sonic 1 SMS Remake | 640x360 | 230 | 2 | 15 / 1.0.0.1760 | `02b6c1dc57` |
| Sonic 2 SMS Remake | 640x360 | 230 | 2 | 15 / 1.0.0.1760 | `02b6c1dc57` |
| Sonic 3 SMS Remake | 640x360 | 230 | 2 | 15 / 1.0.0.1760 | `02b6c1dc57` |
| Grelox | 480x270 | 130 | 0 | 16 / 1.0.0.1804 | `09a06b921d` |
| Super Cakeboy | 480x288 | 138 | 0 | 16 / 1.0.0.9999 | `b2dbaa2fe2` |
| Plague | 480x432 | 207 | 0 | 16 / 1.0.0.9999 | `8691bb6734` |
| Squeek the Meek | 480x432 | 207 | 0 | 16 / 1.0.0.9999 | `231fb70731` |
| Hyper Princess Pitch | 640x480 | 307 | 0 | 16 / 1.0.0.9999 | `8691bb6734` |
| MEGA MAN Sunrise | 640x576 | 369 | 0 | 14 / 1.0.0.1657 | `da4afeac83` |
| Chip n Dale Rescue Rangers: Remastered | 800x480 | 384 | 0 | 16 / 1.0.0.9999 | `8691bb6734` |

The Sonic SMS Remakes size their viewport at runtime (GEN8 window 640x360) and are
the only free ports here with custom shaders (2 each) - a useful torture test for
`blt_stage` state merging and for the shader-override path.

## Runner clusters - test one free port, unlock a family

The 184 ports ship 22 distinct `libyoyo.so` builds. Most owned-asset ports sit on a
runner that some free port also uses, so the free one validates the whole family.

| Runner md5 | Free proxy (ready-to-run) | Paid ports | Headliners |
|---|---|---|---|
| `8691bb6734` | Biomass: Growth, Chip n Dale Rescue Rangers: Remastered | 33 | ASSAULT SHELL, Beemis: The Curse of God, Blastius, Brave Dogs Road |
| `bf02ba999b` | Infested | 18 | Beyond a Total Loss, Cally's Caves 3, Cally's Trials, Circa Infinity |
| `506dbfab56` | Chess is Stupid, Crystals of Amalgam | 13 | Destructivator, Elec Dude, Super Hydorah, Mable and the Wood |
| `09a06b921d` | Grelox | 9 | Demon of Sakura Pass, Golden Hornet, Liz & Laz, Missile Dancer |
| `33150d825b` | **none** | 5 | AM2R, MiniDoom, Mystik Belle, Tower Fortress |
| `079993760e` | Gaurodan, PacMan ROM | 5 | Coin Crypt, Dead Knight, Lost Ethereal, SneakR |
| `46ca3d6371` | **none** | 5 | Fire Arrow Plus, Grimstorm, Riddled Corpses, The Aspect |
| `e793b95989` | Luminous Slash | 3 | Bunny's Flowers, Non-Stop Space Probe, Space Gladiators |
| `ac7f80b09a` | Guns and Guns, I Gacha Head | 3 | Climb, Super Raft Boat Classic, Y E L L O W - J A C K E T |
| `65e896873c` | **none** | 3 | Dungeon Souls, Pure Metal: Feature 1, Xenon Valkyrie |
| `1358452d0d` | **none** | 3 | Klorets, Relic Hunters, Star vs. the Game |
| `a3c6ec15a4` | Snaklipse, Swapwood Quest R | 3 | Lode, Spacewing War, Z-Warp |
| `db71adf997` | Goopty Goo, Pingo | 2 | Beard Blade, ITTA |
| `18166aea99` | Pepper Pengui | 2 | Flaskoman, Residentvania |
| `8aec93e64f` | **none** | 2 | Ghostris, Jump on Head |
| `4c4dc25f3b` | **none** | 1 | Disc Room |
| `b1363b515c` | **none** | 1 | Jump Off The Bridge |
| `2a65ce1830` | Battalion | 1 | LOVE 3 |
| `7ea7e54956` | Bootleg Box, Botbreak | 1 | SATAN LOVES CAKE |
| `da4afeac83` | MEGA MAN Sunrise, Smoothie Galaxy | 1 | Space Moth DX |
| `4e27abc3dc` | **none** | 1 | Super Mega Zero |
| `e1b18cf134` | **none** | 1 | VA-11 Hall-A: Cyberpunk Bartender Action |

Clusters with no free proxy need an owned copy of one of their games before the
runner can be tested at all.

## Appendix A - all 68 ready-to-run gmloader ports

`kpx` is viewport pixels/1000 (the software fill cost). `L` marks the legacy
`_Z12Function_AddPc` runners. Rows with res `0x0` had a data file this parser
could not read.

| Game | Res | kpx | src | Shd | BC | GM ver | Runner | L | MB | Genres |
|---|---|---|---|---|---|---|---|---|---|---|
| Santa's Surprise | 16x16 | 0 | window | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 103 | action,platformer,puzzle |
| NAi NAi Nights-Nokia Edition | 84x48 | 4 | view | 0 | 17 | 2.0.0.0 | `36c73ba10a` |  | 10 | adventure,rpg |
| Biomass: Growth | 160x144 | 23 | view | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 39 | platformer |
| Bumpers & Broadswords Maximum | 160x160 | 26 | view | 0 | 17 | 2.0.0.0 | `8b1382893c` |  | 17 | rpg |
| Pocket Wonder Sport | 192x144 | 28 | view | 1 | 17 | 2.0.0.0 | `506dbfab56` |  | 20 | sports |
| Bootleg Box | 192x160 | 31 | view | 0 | 17 | 2.0.0.0 | `7ea7e54956` |  | 13 | other |
| Bowling Cross | 192x192 | 37 | view | 0 | 17 | 2.0.0.0 | `7ea7e54956` |  | 15 | adventure |
| Bumpers & Broadswords | 192x192 | 37 | view | 0 | 17 | 2.0.0.0 | `8b1382893c` |  | 11 | action |
| I Gacha Head | 256x144 | 37 | window | 0 | 17 | 2.0.0.0 | `ac7f80b09a` |  | 28 | puzzle |
| King of Machines | 256x160 | 41 | view | 0 | 17 | 2.0.0.0 | `506dbfab56` |  | 21 | action |
| Bumpers & Broadswords Tournament | 256x192 | 49 | view | 0 | 17 | 2.0.0.0 | `8b1382893c` |  | 13 | rpg |
| Botbreak | 192x256 | 49 | view | 0 | 17 | 2.0.0.0 | `7ea7e54956` |  | 34 | arcade |
| Cleaving Caliber | 256x240 | 61 | view | 0 | 17 | 2.0.0.0 | `8b1382893c` |  | 10 | action |
| Maldita Castilla | 288x216 | 62 | view | 0 | 14 | 1.0.0.1522 | `bdb2a81afd` | L | 50 | action |
| Hunters | 256x256 | 66 | view | 0 | 17 | 2.0.0.0 | `8b1382893c` |  | 12 | action |
| Chess is Stupid | 320x240 | 77 | window | 0 | 17 | 2.0.0.0 | `506dbfab56` |  | 18 | arcade |
| Dead Ice Deluxe | 320x240 | 77 | window | 0 | 17 | 2.0.0.0 | `506dbfab56` |  | 18 | sports |
| Gaurodan | 320x240 | 77 | view | 0 | 14 | 1.0.0.1474 | `079993760e` | L | 42 | arcade |
| Smoothie Galaxy | 320x240 | 77 | view | 1 | 14 | 1.0.0.1657 | `da4afeac83` | L | 5 | adventure |
| Swapwood Quest R | 320x240 | 77 | view | 0 | 17 | 2.0.0.0 | `a3c6ec15a4` |  | 32 | action,puzzle |
| Verminian Trap | 320x240 | 77 | view | 0 | 13 | 1.0.0.1130 | `bdb2a81afd` | L | 8 | arcade |
| Goopty Goo | 384x216 | 83 | view | 1 | 17 | 2.0.0.0 | `db71adf997` |  | 15 | platformer |
| Livid Meadow | 384x216 | 83 | view | 1 | 16 | 1.0.0.9999 | `8691bb6734` |  | 18 | platformer,adventure |
| Operius | 426x240 | 102 | window | 1 | 17 | 2.0.0.0 | `fbd8591e3d` |  | 12 | arcade |
| Grelox | 480x270 | 130 | view | 0 | 16 | 1.0.0.1804 | `09a06b921d` |  | 39 | arcade,platformer |
| Super Cakeboy | 480x288 | 138 | view | 0 | 16 | 1.0.0.9999 | `b2dbaa2fe2` |  | 12 | platformer |
| Plague | 480x432 | 207 | view | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 19 | platformer |
| Squeek the Meek | 480x432 | 207 | view | 0 | 16 | 1.0.0.9999 | `231fb70731` |  | 48 | arcade,strategy |
| Sonic 1 SMS Remake | 640x360 | 230 | window | 2 | 15 | 1.0.0.1760 | `02b6c1dc57` |  | 40 | action |
| Sonic 2 SMS Remake | 640x360 | 230 | window | 2 | 15 | 1.0.0.1760 | `02b6c1dc57` |  | 50 | action |
| Sonic 3 SMS Remake | 640x360 | 230 | window | 2 | 15 | 1.0.0.1760 | `02b6c1dc57` |  | 21 | action |
| ESTIGMA | 640x380 | 243 | view | 7 | 17 | 2.0.0.0 | `506dbfab56` |  | 55 | arcade,strategy,other |
| Hoopervania | 512x480 | 246 | view | 5 | 16 | 1.0.0.9999 | `8691bb6734` |  | 63 | adventure,platformer |
| Hyper Princess Pitch | 640x480 | 307 | view | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 28 | arcade |
| Tempus Locus | 640x480 | 307 | view | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 19 | puzzle,platformer |
| Crystals of Amalgam | 800x448 | 358 | view | 0 | 17 | 2.0.0.0 | `506dbfab56` |  | 25 | arcade,puzzle |
| IRON FERRET  | 640x576 | 369 | view | 0 | 17 | 2.0.0.0 | `506dbfab56` |  | 15 | arcade |
| Jester's Helper | 640x576 | 369 | view | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 9 | arcade |
| Kikai | 640x576 | 369 | view | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 23 | action |
| MEGA MAN Sunrise | 640x576 | 369 | view | 0 | 14 | 1.0.0.1657 | `da4afeac83` | L | 18 | action,platformer |
| Chip n Dale Rescue Rangers: Remastered | 800x480 | 384 | view | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 91 | arcade,platformer |
| Battalion | 840x480 | 403 | view | 0 | 17 | 2.0.0.0 | `2a65ce1830` |  | 9 | action,strategy |
| Lineoff | 640x640 | 410 | view | 0 | 17 | 2.0.0.0 | `fbd8591e3d` |  | 4 | arcade |
| Tricky Keys 2 | 800x600 | 480 | view | 0 | 17 | 2.0.0.0 | `a3c6ec15a4` |  | 25 | platformer,puzzle |
| Gravity Blocks | 960x540 | 518 | window | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 13 | arcade |
| Pingo | 960x544 | 522 | view | 0 | 17 | 2.0.0.0 | `db71adf997` |  | 44 | puzzle |
| PacMan ROM | 800x704 | 563 | view | 1 | 14 | 1.0.0.1657 | `079993760e` | L | 13 | arcade |
| GUMCHU GIRL | 800x720 | 576 | view | 1 | 17 | 2.0.0.0 | `506dbfab56` |  | 52 | platformer |
| GUM GIRL Gameboy Edition | 800x720 | 576 | view | 0 | 17 | 2.0.0.0 | `506dbfab56` |  | 30 | platformer,puzzle,strategy |
| Everpatch | 1024x576 | 590 | view | 0 | 17 | 2.0.0.0 | `506dbfab56` |  | 6 | adventure,platformer |
| NOT VERY PRO WRESTLING | 1024x576 | 590 | view | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 10 | sports |
| Reverse Bros  | 1024x576 | 590 | view | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 8 | puzzle |
| Snaklipse | 768x768 | 590 | view | 0 | 17 | 2.0.0.0 | `a3c6ec15a4` |  | 10 | arcade |
| Sulka | 768x768 | 590 | view | 0 | 17 | 2.0.0.0 | `7ea7e54956` |  | 13 | platformer |
| Curseball | 960x720 | 691 | view | 0 | 17 | 2.0.0.0 | `8b1382893c` |  | 15 | action |
| Pepper Pengui | 960x720 | 691 | view | 0 | 17 | 2.0.0.0 | `18166aea99` |  | 11 | arcade,strategy |
| Sokobrawn | 960x768 | 737 | window | 0 | 16 | 1.0.0.9999 | `8691bb6734` |  | 4 | puzzle |
| Kleebuu Craves Fruit Salad | 1152x648 | 746 | view | 0 | 17 | 2.0.0.0 | `7ea7e54956` |  | 103 | platformer |
| Tricky Keys | 1024x768 | 786 | window | 0 | 17 | 2.0.0.0 | `f78362dca7` |  | 21 | platformer,puzzle |
| FAITH Demon Siege | 1280x720 | 922 | view | 1 | 16 | 1.0.0.9999 | `8691bb6734` |  | 43 | arcade,strategy |
| Forgodden | 1280x720 | 922 | window | 7 | 17 | 2.0.0.0 | `7ea7e54956` |  | 53 | action |
| Luminous Slash | 1280x720 | 922 | window | 2 | 17 | 2.0.0.0 | `e793b95989` |  | 16 | arcade |
| Streets of Claus | 1280x720 | 922 | view | 0 | 17 | 2.0.0.0 | `36c73ba10a` |  | 11 | arcade,other |
| OverGun | 1440x810 | 1166 | view | 0 | 17 | 2.0.0.0 | `506dbfab56` |  | 10 | platformer,strategy,other |
| Mire | 1440x816 | 1175 | view | 0 | 17 | 2.0.0.0 | `ac7f80b09a` |  | 40 | adventure,platformer |
| Guns and Guns | 1600x920 | 1472 | view | 1 | 17 | 2.0.0.0 | `ac7f80b09a` |  | 14 | action |
| Hallowed Candy | 0x0 | ? | window | None | None | None | `8691bb6734` |  | 38 | arcade |
| Infested | 0x0 | ? | window | None | None | None | `bf02ba999b` |  | 56 | adventure |

## Appendix B - 116 gmloader ports needing owned/purchased game data

Viewport/shader data is unavailable (the port ships no game data), but the runner
binary is in the port zip, so runner compatibility is known up front.

| Game | Runner | Free proxy | Genres |
|---|---|---|---|
| AM2R | `33150d825b` | - | action,adventure,platformer |
| ASSAULT SHELL | `8691bb6734` | Biomass: Growth | action,arcade |
| Beard Blade | `db71adf997` | Goopty Goo | platformer |
| Beemis: The Curse of God | `8691bb6734` | Biomass: Growth | action |
| Beyond a Total Loss | `bf02ba999b` | Infested | action |
| Blastius | `8691bb6734` | Biomass: Growth | arcade |
| Brave Dogs Road | `8691bb6734` | Biomass: Growth | adventure |
| Breaker | `8691bb6734` | Biomass: Growth | arcade |
| Bunny's Flowers | `e793b95989` | Luminous Slash | puzzle |
| Cally's Caves 3 | `bf02ba999b` | Infested | action,adventure,platformer |
| Cally's Trials | `bf02ba999b` | Infested | action,adventure,platformer |
| Circa Infinity | `bf02ba999b` | Infested | platformer |
| Climb | `ac7f80b09a` | Guns and Guns | action |
| Cognizance | `bf02ba999b` | Infested | adventure,platformer |
| Coin Crypt | `079993760e` | Gaurodan | rpg |
| Cursed Castilla (Maldita Castilla EX) | `bf02ba999b` | Infested | platformer |
| Daydreamer | `8691bb6734` | Biomass: Growth | arcade,other |
| Dead Knight | `079993760e` | Gaurodan | platformer |
| Demon of Sakura Pass | `09a06b921d` | Grelox | racing |
| Destructivator | `506dbfab56` | Chess is Stupid | arcade |
| Disc Room | `4c4dc25f3b` | - | action |
| Drink'n'Drive | `8691bb6734` | Biomass: Growth | arcade,racing |
| Dungeon Souls | `65e896873c` | - | action |
| E.Z | `8691bb6734` | Biomass: Growth | platformer |
| Elec Dude | `506dbfab56` | Chess is Stupid | arcade |
| Fire Arrow Plus | `46ca3d6371` | - | action,arcade |
| Flaskoman | `18166aea99` | Pepper Pengui | puzzle,platformer |
| FNAF NES STYLED | `bf02ba999b` | Infested | other |
| Fran Bow | `bf02ba999b` | Infested | adventure |
| Frogger 2 | `bf02ba999b` | Infested | arcade |
| Galaxy Champions TV | `8691bb6734` | Biomass: Growth | arcade |
| Gauge Of Rage | `bf02ba999b` | Infested | action |
| Geometry Crash | `8691bb6734` | Biomass: Growth | rhythm |
| Ghostris | `8aec93e64f` | - | puzzle |
| Gloom Reducer | `8691bb6734` | Biomass: Growth | adventure |
| Goblin Dungeoneer | `8691bb6734` | Biomass: Growth | platformer |
| Golden Hornet | `09a06b921d` | Grelox | arcade |
| Grimstorm | `46ca3d6371` | - | action |
| Hakopalace | `8691bb6734` | Biomass: Growth | arcade,other |
| Halloween Forever | `8691bb6734` | Biomass: Growth | platformer,adventure |
| Haunted Lands Burial Grounds | `8691bb6734` | Biomass: Growth | arcade,platformer |
| iii | `8691bb6734` | Biomass: Growth | platformer,adventure |
| ITTA | `db71adf997` | Goopty Goo | action |
| Jump Off The Bridge | `b1363b515c` | - | platformer |
| Jump on Head | `8aec93e64f` | - | platformer |
| Klorets | `1358452d0d` | - | arcade |
| Lil Tanks | `bf02ba999b` | Infested | arcade,other |
| Liz & Laz | `09a06b921d` | Grelox | platformer |
| Lode | `a3c6ec15a4` | Snaklipse | adventure |
| Lost Ethereal | `079993760e` | Gaurodan | puzzle |
| LOVE 3 | `2a65ce1830` | Battalion | platformer |
| Mable and the Wood | `506dbfab56` | Chess is Stupid | platformer,adventure |
| Magic Vigilante | `8691bb6734` | Biomass: Growth | action |
| MEZZER | `8691bb6734` | Biomass: Growth | action |
| Mimi's Delivery Dash | `8691bb6734` | Biomass: Growth | arcade,platformer,puzzle |
| MiniDoom | `33150d825b` | - | platformer |
| Missile Dancer | `09a06b921d` | Grelox | arcade |
| Mobility | `506dbfab56` | Chess is Stupid | platformer |
| Monolith / Star of Providence | `8691bb6734` | Biomass: Growth | action |
| Mystik Belle | `33150d825b` | - | action,platformer |
| Ne Touchez Pas 5 | `8691bb6734` | Biomass: Growth | action |
| No Transmission | `09a06b921d` | Grelox | action |
| Non-Stop Space Probe | `e793b95989` | Luminous Slash | arcade |
| OCDA | `506dbfab56` | Chess is Stupid | platformer |
| OH MY GOD, look at this knight | `09a06b921d` | Grelox | adventure |
| Omega Strike | `8691bb6734` | Biomass: Growth | action |
| OutlawN | `506dbfab56` | Chess is Stupid | arcade,strategy |
| Panic Room | `8691bb6734` | Biomass: Growth | platformer |
| Pinky | `09a06b921d` | Grelox | puzzle,platformer |
| Pixel Skater Dude | `8691bb6734` | Biomass: Growth | action,sports |
| Please, Don't Touch Anything | `bf02ba999b` | Infested | other |
| Pulstario | `506dbfab56` | Chess is Stupid | other |
| PuPaiPo Space Deluxe | `506dbfab56` | Chess is Stupid | arcade |
| Pure Metal: Feature 1 | `65e896873c` | - | arcade |
| R&Watch-Dobtopus | `8691bb6734` | Biomass: Growth | arcade,other |
| REDO! | `8691bb6734` | Biomass: Growth | platformer |
| Relic Hunters | `1358452d0d` | - | adventure |
| Residentvania | `18166aea99` | Pepper Pengui | action |
| Riddled Corpses | `46ca3d6371` | - | action,arcade,strategy |
| Riptale | `bf02ba999b` | Infested | action,platformer |
| Risk of Rain (2013) | `8691bb6734` | Biomass: Growth | action,platformer |
| Road Warriors 2 | `bf02ba999b` | Infested | action |
| Rotational Shift | `506dbfab56` | Chess is Stupid | arcade |
| SATAN LOVES CAKE | `7ea7e54956` | Bootleg Box | adventure,platformer |
| Secret Tea Garden | `506dbfab56` | Chess is Stupid | simulation,other |
| Shadow Wrangler | `bf02ba999b` | Infested | platformer,puzzle |
| Shrubnaut | `506dbfab56` | Chess is Stupid | platformer |
| SneakR | `079993760e` | Gaurodan | action |
| Space Gladiators | `e793b95989` | Luminous Slash | action |
| Space Moth DX | `da4afeac83` | MEGA MAN Sunrise | action,arcade |
| Spacewing War | `a3c6ec15a4` | Snaklipse | arcade,other |
| Spearmint Mountain | `8691bb6734` | Biomass: Growth | platformer |
| Splinter Zone | `8691bb6734` | Biomass: Growth | platformer,arcade |
| Star vs. the Game | `1358452d0d` | - | adventure |
| Static | `079993760e` | Gaurodan | platformer |
| Super Gaelic Dodgeball | `8691bb6734` | Biomass: Growth | sports |
| Super Hydorah | `506dbfab56` | Chess is Stupid | arcade |
| Super Mega Zero | `4e27abc3dc` | - | platformer |
| Super Mutant Alien Assault | `bf02ba999b` | Infested | action |
| Super Raft Boat Classic | `ac7f80b09a` | Guns and Guns | action |
| Super Robot Ninja Girl | `bf02ba999b` | Infested | action |
| Super Skelemania | `09a06b921d` | Grelox | platformer |
| Super Star Path | `8691bb6734` | Biomass: Growth | arcade |
| SuperXYX | `506dbfab56` | Chess is Stupid | arcade |
| Tamashii | `09a06b921d` | Grelox | platformer,other |
| The Aspect | `46ca3d6371` | - | platformer |
| The Night Shift | `bf02ba999b` | Infested | arcade |
| The Wind | `8691bb6734` | Biomass: Growth | adventure |
| Towards The Pantheon: Escaping Eternity | `8691bb6734` | Biomass: Growth | adventure,rpg |
| Tower Fortress | `33150d825b` | - | action |
| Ubermosh Collection | `33150d825b` | - | arcade,other |
| VA-11 Hall-A: Cyberpunk Bartender Action | `e1b18cf134` | - | visual novel |
| Vertical Drop Heroes HD | `46ca3d6371` | - | platformer |
| Xenon Valkyrie | `65e896873c` | - | rpg |
| Y E L L O W - J A C K E T | `ac7f80b09a` | Guns and Guns | arcade |
| Z-Warp | `a3c6ec15a4` | Snaklipse | arcade |

## Appendix C - practical notes from the port scripts

- A `.port` file is a plain zip with the APK layout (`lib/armeabi-v7a/libyoyo.so` +
  `assets/game.droid`), so `apk_path = "verminiantrap.port"` works with no repackaging.
- Env vars PortMaster sets: `GMLOADER_DEPTH_DISABLE=1` (173/184 scripts),
  `GMLOADER_SAVEDIR` (176), `GMLOADER_PLATFORM` (113 - mostly `os_windows`, `os_linux`
  for a few such as Grelox), `GMLOADER_JSON` (2).
- 131 scripts apply an `xdelta3` patch to the desktop `data.win` before first run; that
  step happens on the porting host, not on the MiSTer.
- No ready-to-run port in this set is mouse-driven - all are gamepad-mapped via gptk,
  which suits the MiSTer joystick path.

_Data collected 2026-08-04 from PortMaster ports.json and the released port zips._
