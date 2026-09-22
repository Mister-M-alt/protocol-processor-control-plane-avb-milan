import re, sys, html
import xml.etree.ElementTree as ET
NS = {'svg': 'http://www.w3.org/2000/svg', 'x': 'http://www.w3.org/1999/xhtml'}
def text_of(el):
    return re.sub(r'\s+', ' ', ''.join(el.itertext())).strip()
def summarize(path):
    root = ET.parse(path).getroot()
    out = {'edge_labels': [], 'states': [], 'edge_paths': 0}
    for g in root.iter('{http://www.w3.org/2000/svg}g'):
        cls = g.get('class', '')
        if cls == 'edgeLabel':
            t = text_of(g)
            if t:
                out['edge_labels'].append(t)
        if 'statediagram-state' in cls:
            out['states'].append(text_of(g))
        if cls == 'edgePaths':
            out['edge_paths'] = sum(1 for p in g.iter('{http://www.w3.org/2000/svg}path'))
    out['start_nodes'] = sum(1 for g in root.iter('{http://www.w3.org/2000/svg}g') if g.get('class','').startswith('node default'))
    return out
for p in sys.argv[1:]:
    s = summarize(p)
    print(f'== {p.split("/")[-1]}')
    print(f'   state nodes: {s["states"]}  initial-pseudostate groups: {s["start_nodes"]}  edge paths: {s["edge_paths"]}')
    for i, l in enumerate(s['edge_labels'], 1):
        print(f'   edge label {i}: {l}')
