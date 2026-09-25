#!/bin/sh
# apt_use_snapshot.sh — point apt at the snapshot.debian.org archive the image was
# built from, instead of the live bullseye mirrors.
#
# Why: bullseye is past end of life and deb.debian.org/debian-security has
# started returning 404 for files its own index still lists (2026-09:
# libsdl2-dev, libexpat1, perl, python3.9, openssl, ...), so every
# `apt-get install` in the CI images and Dockerfile.gmloader-build fails. The
# official debian images ship a commented `# deb http://snapshot.debian.org/...`
# line per source, pinned to the image's own build date; those archives are
# immutable, so index and pool always agree, and they match the package
# versions already installed in the image.
#
# Sources without a snapshot comment are left untouched (no-op on an image that
# does not carry them). snapshot Release files carry an expired Valid-Until,
# hence check-valid-until=no. POSIX sh: runs in the -slim images before bash
# is guaranteed.
set -e
for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.list; do
    [ -f "$f" ] || continue
    grep -q '^# deb http://snapshot.debian.org/' "$f" || continue
    sed -i -e 's|^deb |#live deb |' \
           -e 's|^# deb http://snapshot.debian.org/|deb [check-valid-until=no] http://snapshot.debian.org/|' "$f"
    echo "apt_use_snapshot: $f"
    grep '^deb ' "$f"
done
