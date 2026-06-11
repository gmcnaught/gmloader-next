#!/bin/bash
#
# MiSTer Scripts-menu launcher for gmloader (Maldita Castilla).
# Copy to /media/fat/Scripts/ — it then appears in the MiSTer menu under
# "Scripts" as "Maldita Castilla". Runs the GameMaker loader with the MiSTer
# software blitter, which is enabled via gmloader.json ("blitter": 2) — no env
# vars needed. To A/B a different blitter level or a toggle, export it before the
# run, e.g.  GMLOADER_BLITTER=0  (GL path) or  GMLOADER_BLITTER_PROF=1.
#
GMDIR=/media/fat/games/gmloader
cd "$GMDIR" || { echo "gmloader dir not found: $GMDIR"; sleep 3; exit 1; }

# Avoid a double-launch / ETXTBSY when re-running from the menu.
pkill -9 -f "gmloader -c" 2>/dev/null
sleep 1

export LD_LIBRARY_PATH="$GMDIR/mesa:$GMDIR"
echo "Launching gmloader (Maldita Castilla)... log: /tmp/gmloader.log"
./gmloader -c gmloader.json 2>&1 | tee /tmp/gmloader.log
