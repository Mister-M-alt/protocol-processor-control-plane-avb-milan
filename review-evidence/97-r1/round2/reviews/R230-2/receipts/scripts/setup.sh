#!/usr/bin/env bash
# Scratch setup executed once before the probes (commands as run). Nothing here
# writes to the review clone: the head is a --no-hardlinks clone, the prior head
# (1eb20dc) a git archive export, and the German/French/English UTF-8 locales are
# generated with glibc's localedef into a scratch LOCPATH (no privileges).
set -eu
S=/tmp/r230-100-r2-scratch
mkdir -p "$S/locales" "$S/tmp"
localedef -i de_DE -f UTF-8 "$S/locales/de_DE.UTF-8"
localedef -i fr_FR -f UTF-8 "$S/locales/fr_FR.UTF-8"
localedef -i en_US -f UTF-8 "$S/locales/en_US.UTF-8"
git clone --quiet --no-hardlinks $VALIDATION_STORAGE/reviews/r230-100-r2 "$S/head"
git -C "$S/head" checkout --quiet --detach 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e
mkdir -p "$S/prior"
git -C $VALIDATION_STORAGE/reviews/r230-100-r2 archive 1eb20dc4911880de10b745cc284e7dde306788b5 | tar -x -C "$S/prior"
# Receipt order: 00-identity.sh, 01-evidence.sh, 02-compiler-language.sh,
# 03-gate-matrix.sh, 04-env-capture.sh (04-analyze.py), 05-inprocess-isolation.py
# (via run.py, MIXED setting), 06-regression.sh, 07-gate-mutants.sh, 08-full-make.sh,
# 09-m25-m31.sh, 10-rtl-boundary.sh, 11-docs-python.sh, 12-python-versions.sh,
# 13-legacy-encodings.sh, 14-conformance.sh, 15-job-cap.py, 16-final-integrity.sh.
