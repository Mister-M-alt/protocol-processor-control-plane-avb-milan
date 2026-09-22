import sys, re, xml.etree.ElementTree as ET
S='{http://www.w3.org/2000/svg}'
def run(path, geometry):
    r = ET.parse(path).getroot()
    print('== ' + path.split('/')[-1])
    labels = {}
    for g in r.iter(S+'g'):
        if g.get('class') == 'edgeLabel':
            inner = [c for c in g.iter(S+'g') if c.get('data-id')]
            if inner:
                labels[inner[0].get('data-id')] = re.sub(r'\s+',' ',''.join(g.itertext())).strip()
    nodes = {}
    for g in r.iter(S+'g'):
        cls = g.get('class','').split()
        if cls and cls[0] == 'node':
            m = re.search(r'translate\(([-\d.]+),\s*([-\d.]+)\)', g.get('transform',''))
            name = re.sub(r'\s+',' ',''.join(g.itertext())).strip() or '[*]'
            nodes[name] = (float(m.group(1)), float(m.group(2)))
    for n,(x,y) in nodes.items():
        print('  node %-9s centre (%.1f, %.1f)' % (n, x, y))
    def nearest(pt):
        return min(nodes, key=lambda n: (nodes[n][0]-pt[0])**2 + (nodes[n][1]-pt[1])**2)
    for g in r.iter(S+'g'):
        if g.get('class') == 'edgePaths':
            for p in sorted(g.iter(S+'path'), key=lambda p: p.get('id')):
                nums = [float(v) for v in re.findall(r'-?\d+(?:\.\d+)?', p.get('d'))]
                eid = p.get('id').replace('my-svg-','')
                print('  %s: path starts nearest %-8s ends nearest %-8s | label: %s'
                      % (eid, nearest((nums[0], nums[1])), nearest((nums[-2], nums[-1])), labels.get(eid)))
for p in sys.argv[1:]:
    run(p, True)
