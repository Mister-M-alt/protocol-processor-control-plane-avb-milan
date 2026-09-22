#!/usr/bin/env bash
# Documentation gates that need no install (links, compliance matrix, module
# matrix, uPC map, stale exports) at the exact head, Python syntax of the new
# files without writing bytecode, and the oldest Python grammar that accepts
# each tracked .py file (ast feature_version), to compare the new regression
# with the tree's existing baseline.
set -u
source "$(dirname "$0")/settings.sh"
H="$S/head"
$RUN --name 11-check-links --cwd "$H" --expect-rc 0 -- python3 -B scripts/check-links.py
$RUN --name 11-check-matrix --cwd "$H" --expect-rc 0 -- python3 -B scripts/check-matrix.py
$RUN --name 11-gen-matrix-check --cwd "$H" --expect-rc 0 -- python3 -B scripts/gen_matrix.py --check
$RUN --name 11-check-upc-map --cwd "$H" --expect-rc 0 -- python3 -B scripts/check_upc_map.py
$RUN --name 11-make-stale --cwd "$H" --expect-rc 0 -- make -s stale
for n in 11-check-links 11-check-matrix 11-gen-matrix-check 11-check-upc-map 11-make-stale; do
  echo "   $n tail: $(tail -2 "$RC/$n.log" | tr '\n' ' ' | cut -c1-200)"
done
echo "== syntax (no bytecode written)"
(cd "$H/tb/pp_top" && python3 -B -c "import ast,sys; [ast.parse(open(f).read(), f) for f in sys.argv[1:]]; print('parsed', *sys.argv[1:])" fixture_guards.py test_fixture_guards.py)
echo "== oldest accepted grammar per tracked .py file (ast.parse feature_version)"
(cd "$H" && git ls-files '*.py' | python3 -B -c "
import ast, sys
rows = []
for f in sys.stdin.read().split():
    src = open(f).read()
    low = None
    for minor in range(4, 15):
        try:
            ast.parse(src, f, feature_version=(3, minor)); low = minor; break
        except SyntaxError:
            pass
    rows.append((low, f))
for low, f in sorted(rows, key=lambda r: (-(r[0] or 99), r[1]))[:12]:
    print(f'   3.{low}  {f}')
print('   files:', len(rows), ' max minimum:', '3.%d' % max(r[0] for r in rows))
")
echo "== grammar feature in the new test that sets its minimum"
grep -n "with (" "$H/tb/pp_top/test_fixture_guards.py"
echo "== diff --check prior..head and base..head"
git -C $VALIDATION_STORAGE/reviews/r230-100-r2 diff --check 1eb20dc4911880de10b745cc284e7dde306788b5 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e; echo "   rc=$?"
git -C $VALIDATION_STORAGE/reviews/r230-100-r2 diff --check 8452f564294300a82d56eed464276576f65f4d58 5c45845ad15bd7995f20c81d7fd61501e5ca9d7e; echo "   rc=$?"
echo "== head clone status --porcelain --ignored: $(git -C "$H" status --porcelain --ignored | wc -l) lines"
