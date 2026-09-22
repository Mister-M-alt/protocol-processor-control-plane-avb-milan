#!/usr/bin/env python3
"""R233 independent probe: map each rendered F10.2 edge to (source node, target node, label text).
Usage: svg_edge_probe.py <render.svg>...   Endpoints are assigned to the nearest node centre."""
import re, sys, math
import xml.etree.ElementTree as ET

def local(t): return t.rsplit('}', 1)[-1]

def translate(el):
    m = re.search(r'translate\(\s*([-\d.]+)[ ,]+([-\d.]+)\)', el.get('transform', ''))
    return (float(m.group(1)), float(m.group(2))) if m else None

def text_of(el):
    return ' '.join(' '.join(el.itertext()).split())

def probe(path):
    root = ET.parse(path).getroot()
    nodes = {}
    for g in root.iter():
        if local(g.tag) == 'g' and 'node' in (g.get('class') or '').split() and g.get('id', '').startswith('my-svg-state-'):
            name = re.sub(r'^my-svg-state-(.*)-\d+$', r'\1', g.get('id'))
            nodes[name] = translate(g)
    edges = {}
    for p in root.iter():
        if local(p.tag) == 'path' and (p.get('data-id') or '').startswith('edge'):
            nums = [float(x) for x in re.findall(r'-?\d+(?:\.\d+)?', p.get('d'))]
            start, end = (nums[0], nums[1]), (nums[-2], nums[-1])
            near = lambda pt: min(nodes, key=lambda n: math.dist(pt, nodes[n]))
            edges[p.get('data-id')] = [near(start), near(end), None]
    for g in root.iter():
        if local(g.tag) == 'g' and g.get('data-id', '').startswith('edge') and 'label' in (g.get('class') or '').split():
            edges[g.get('data-id')][2] = text_of(g)
    states = sorted(n for n in nodes if not n.startswith('root_start'))
    print(f'== {path}')
    print(f'nodes: {sorted(nodes)}  states: {states}  edges: {len(edges)}')
    for k in sorted(edges):
        s, t, lab = edges[k]
        print(f'{k}: {s} -> {t} | {lab}')
    return edges

res = [probe(p) for p in sys.argv[1:]]
if len(res) == 2:
    a, b = res
    print('== comparison (first vs second)')
    for k in sorted(set(a) | set(b)):
        same_ends = a.get(k, [None]*3)[:2] == b.get(k, [None]*3)[:2]
        same_lab = a.get(k, [None]*3)[2] == b.get(k, [None]*3)[2]
        print(f'{k}: endpoints {"same" if same_ends else "DIFFER"}, label {"same" if same_lab else "DIFFERS"}')
