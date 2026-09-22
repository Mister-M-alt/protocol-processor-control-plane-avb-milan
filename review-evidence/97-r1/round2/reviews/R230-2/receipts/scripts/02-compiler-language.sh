#!/usr/bin/env bash
# Control: which caller settings make this GCC print translated diagnostics,
# and which LC_ALL values suppress translation even with LANGUAGE set.
set -u
source "$(dirname "$0")/settings.sh"
mkdir -p "$S/cc" && cd "$S/cc" || exit 1
printf '%s\n' 'static_assert(1 == 2, "R230 probe message");' 'int main() {}' > probe.cpp
for k in $ORDER; do
  $RUN --name "02-cc-$k" --cwd "$S/cc" ${SETTING[$k]} --expect-rc 1 -- \
    g++ -std=c++17 -fsyntax-only probe.cpp
done
$RUN --name 02-cc-LC_ALL_C-LANGUAGE_de --cwd "$S/cc" --set LC_ALL=C --set LANGUAGE=de --expect-rc 1 -- g++ -std=c++17 -fsyntax-only probe.cpp
$RUN --name 02-cc-LC_ALL_C-MIXED --cwd "$S/cc" --set LC_ALL=C --set LANG=de_DE.UTF-8 --set LANGUAGE=de:fr --set LC_MESSAGES=fr_FR.UTF-8 --expect-rc 1 -- g++ -std=c++17 -fsyntax-only probe.cpp
$RUN --name 02-cc-LC_ALL_POSIX-LANGUAGE_de --cwd "$S/cc" --set LC_ALL=POSIX --set LANGUAGE=de --expect-rc 1 -- g++ -std=c++17 -fsyntax-only probe.cpp
$RUN --name 02-cc-LC_ALL_C.UTF-8-LANGUAGE_de --cwd "$S/cc" --set LC_ALL=C.UTF-8 --set LANGUAGE=de --expect-rc 1 -- g++ -std=c++17 -fsyntax-only probe.cpp
echo "== summary: error line per setting"
for f in "$RC"/02-cc-*.log; do
  printf '%-40s %s\n' "$(basename "$f" .log)" "$(grep -m1 'error' "$f")"
done
