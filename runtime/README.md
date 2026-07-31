# GL runtime closure (vendored)

The surfaceless-Mesa closure the gmloader engine needs at
`games/gmloader/mesa/` on-device (loaded via
`LD_LIBRARY_PATH=<gamedir>/mesa:<gamedir>`). Mesa is MIT-licensed.

Vendored 2026-07-30 from the known-good deploy tree
(`epic-mister-sdl-buffer-output/games/gmloader/mesa/`, the tree
`maldita.castilla-mister/deploy.py --with-runtime` pushes). These files are
armhf builds and rarely change; they were previously untracked in any repo.

Not vendored here (already tracked in the gmloader-next submodule):
- `libGLES_sw.so`      -> `3rdparty/gles2-sw/libGLES_sw.so`
- `libstdc++.so`       -> `lib/armeabi-v7a/libstdc++.so`
