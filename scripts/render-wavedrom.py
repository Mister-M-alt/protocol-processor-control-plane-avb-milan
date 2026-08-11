#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-W-2.0
"""Render every embedded ```wavedrom block to a committed SVG.

GitHub renders Mermaid natively but NOT WaveDrom, so each block's rendered SVG is
committed under docs/diagrams/wavedrom/ and embedded by the host document; the fenced
JSON stays the editable source (collapsed in a <details> right below the image).

Naming: each block takes the id of the nearest preceding `<a id="fig-..."></a>` anchor
in its document -> docs/diagrams/wavedrom/<anchor>.svg.

Modes:
  render (default) - (re)write all SVGs
  --check          - fail if any committed SVG differs from a fresh render
                     (the wavedrompy output is deterministic)

Uses the `wavedrom` Python package (wavedrompy). If it is not importable, a local
virtualenv is bootstrapped at .venv-wavedrom/ (gitignored) and the script re-executes
itself inside it.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTDIR = os.path.join(ROOT, "docs", "diagrams", "wavedrom")
VENV = os.path.join(ROOT, ".venv-wavedrom")
ANCHOR = re.compile(r'<a id="(fig-[a-z0-9-]+)"></a>')


def ensure_wavedrom():
    try:
        import wavedrom  # noqa: F401
        return
    except ModuleNotFoundError:
        pass
    vpy = os.path.join(VENV, "bin", "python")
    if not os.path.exists(vpy):
        print("bootstrapping .venv-wavedrom (one-time) ...")
        import venv
        venv.create(VENV, with_pip=True)
        subprocess.run([vpy, "-m", "pip", "install", "--quiet", "wavedrom"],
                       check=True)
    if os.path.realpath(sys.executable) != os.path.realpath(vpy):
        os.execv(vpy, [vpy, os.path.abspath(__file__)] + sys.argv[1:])


def collect_blocks():
    """-> list of (md_path, anchor, json_source)"""
    blocks = []
    for base, _dirs, files in os.walk(os.path.join(ROOT, "docs")):
        if os.path.commonpath([base, OUTDIR]) == OUTDIR:
            continue
        for name in sorted(files):
            if not name.endswith(".md"):
                continue
            path = os.path.join(base, name)
            anchor, inblock, buf = None, False, []
            with open(path, encoding="utf-8") as fh:
                for line in fh:
                    if not inblock:
                        m = ANCHOR.search(line)
                        if m:
                            anchor = m.group(1)
                        if line.rstrip() == "```wavedrom":
                            if anchor is None:
                                sys.exit(f"FAIL: wavedrom block without a preceding "
                                         f"fig anchor in {path}")
                            inblock, buf = True, []
                    elif line.rstrip() == "```":
                        blocks.append((path, anchor, "".join(buf)))
                        inblock = False
                    else:
                        buf.append(line)
    dupes = {a for _, a, _ in blocks
             if sum(1 for _, b, _ in blocks if b == a) > 1}
    if dupes:
        sys.exit(f"FAIL: anchor(s) used by more than one wavedrom block: {dupes}")
    return blocks


def main() -> int:
    ensure_wavedrom()
    import wavedrom

    check = "--check" in sys.argv
    os.makedirs(OUTDIR, exist_ok=True)
    blocks, stale = collect_blocks(), []
    for path, anchor, src in blocks:
        try:
            svg = wavedrom.render(src).tostring()
        except Exception as exc:
            sys.exit(f"FAIL: {anchor} in {os.path.relpath(path, ROOT)}: {exc}")
        out = os.path.join(OUTDIR, anchor + ".svg")
        old = open(out, encoding="utf-8").read() if os.path.exists(out) else None
        if check:
            if svg != old:
                stale.append(anchor)
        elif svg != old:
            with open(out, "w", encoding="utf-8") as fh:
                fh.write(svg)
            print(f"rendered {os.path.relpath(out, ROOT)}")

    if check and stale:
        print(f"WAVEDROM STALE: {', '.join(stale)} "
              f"(run: python3 scripts/render-wavedrom.py)")
        return 1
    print(f"wavedrom: {len(blocks)} blocks "
          f"{'checked, OK' if check else 'rendered'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
