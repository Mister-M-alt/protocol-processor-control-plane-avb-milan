"""R234 reproducible, read-only candidate audit and isolated documentation probes.
Run: rtk proxy python3 review-probes.py /path/to/frozen/checkout
No installs, RTL builds, author scripts, or checkout writes.
"""
import datetime
import hashlib
import io
import json
from pathlib import Path
import re
import stat
import subprocess
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET

OUT = Path(__file__).resolve().parent
REPO = Path(sys.argv[1]).resolve()
HEAD = 'bc997e7c00e50e3d59b97987950bfbf6cc442182'
BASE = '8452f564294300a82d56eed464276576f65f4d58'
FIRST = '88a4eb4f0765e7e8c1c41f35599f169ea74a2aaf'
ARCH = 'docs/architecture/10_srp_engine.md'
NOTE = 'tb/srp_encoder/README.md'
ARCHIVE = OUT/'public/archive/review-evidence/98-r1'
RECEIPTS = OUT/'receipts'
RENDER = OUT/'render'
RENDER.mkdir(exist_ok=True)

def cmd(args, cwd=REPO, expected=0, name=None):
    full = ['rtk', 'proxy', *args]
    t = datetime.datetime.now(datetime.timezone.utc).isoformat()
    p = subprocess.run(full, cwd=cwd, capture_output=True, timeout=180)
    if name:
        (RECEIPTS/name).write_bytes(p.stdout+p.stderr)
    with (RECEIPTS/'probe-commands.jsonl').open('a') as f:
        f.write(json.dumps({'utc':t,'argv':full,'cwd':str(cwd),'exit_code':p.returncode,
                            'expected':expected,'log':name})+'\n')
    assert p.returncode == expected, (full, p.returncode, p.stderr.decode(errors='replace'))
    return p.stdout

def git(*a): return cmd(['git',*a])
def blob(rev, path): return git('show', f'{rev}:{path}')
def sha(b): return hashlib.sha256(b).hexdigest()
def tree(rev):
    rows = {}
    for row in git('ls-tree','-rz',rev).split(b'\0'):
        if not row: continue
        meta,path=row.split(b'\t',1)
        mode,kind,oid=meta.decode().split()
        rows[path.decode()]=(mode,kind,oid)
    return rows
def fence(b):
    return re.search(rb'<a id="fig-10-domsm".*?```mermaid\n(.*?)```',b,re.S)[1]

assert git('rev-parse','HEAD').decode().strip()==HEAD
trees={r:tree(r) for r in [BASE,FIRST,HEAD]}
changes={r:[p for p in sorted(set(trees[r])|set(trees[HEAD]))
            if trees[r].get(p)!=trees[HEAD].get(p)] for r in [BASE,FIRST]}
assert changes[BASE]==[ARCH,NOTE]
assert changes[FIRST]==[NOTE]
assert all(trees[BASE][p][0]==trees[HEAD][p][0] for p in trees[HEAD])
assert git('rev-parse',BASE+'^{tree}')==git('rev-parse','ea93023fbdd31cbf718da0d8f1aab4d750bfd21a^{tree}')
fences={r:fence(blob(r,ARCH)) for r in [BASE,FIRST,HEAD]}
assert fences[FIRST]==fences[HEAD]
for rev,n in [(BASE,'base-8452f56'),(FIRST,'head-88a4eb4')]:
    assert fences[rev]==(ARCHIVE/f'author/initial/render/F10.2-{n}.mmd').read_bytes()
assert fences[BASE].replace(b'LINK_DOWN then LINK_UP / back to defaults',
    b'LINK_DOWN / restore defaults, declared again on the next LINK_UP')==fences[HEAD]
assert blob(BASE,NOTE).replace(b'- The revert edge reads "LINK_DOWN then LINK_UP / back to defaults": the',
    b'- The F10.2 revert edge (ADOPTED to DEFAULTS) fires on LINK_DOWN: the')==blob(HEAD,NOTE)
assets=[p for p in trees[HEAD] if p.startswith('docs/diagrams/') and p.endswith(('.svg','.png','.drawio'))]
asset_hits=[p for p in assets if re.search(rb'F10\.2|fig-10-domsm|ADOPTED|DEFAULTS|Domain FSM',blob(HEAD,p),re.I)]
all_text_hits={}
for term in ['LINK_DOWN then LINK_UP / back to defaults','LINK_DOWN / restore defaults, declared again on the next LINK_UP']:
    all_text_hits[term]=[p for p in trees[HEAD] if (REPO/p).is_file() and term.encode() in (REPO/p).read_bytes()]
scope={'head':HEAD,'tree':git('rev-parse','HEAD^{tree}').decode().strip(),
       'base':BASE,'first_render_commit':FIRST,'changes_to_final':changes,
       'all_modes_unchanged':True,'tracked_count':len(trees[HEAD]),
       'same_pr96_base_tree':True,'fence_sha256':{r:sha(b) for r,b in fences.items()},
       'architecture_blobs':{r:trees[r][ARCH] for r in trees},
       'readme_blobs':{r:trees[r][NOTE] for r in trees},
       'asset_count':len(assets),'asset_term_hits':asset_hits,'label_hits':all_text_hits}
(RECEIPTS/'scope.json').write_text(json.dumps(scope,indent=2)+'\n')

# Verify all downloaded evidence files against the archive manifest. An original
# hash applies only to unredacted bytes; published hashes grade public text.
manifest=json.loads((ARCHIVE/'MANIFEST.json').read_text())
verified=[]
for row in manifest:
    p=ARCHIVE/row['file']
    if p.exists():
        h=sha(p.read_bytes());assert h==row['published_sha256'], row['file']
        if not row['path_redacted']: assert h==row['original_sha256']
        verified.append(row|{'verified_sha256':h})
(RECEIPTS/'archive-hashes.json').write_text(json.dumps(verified,indent=2)+'\n')

# Independently inspect render graph structure, not the author's graph probe.
ns={'s':'http://www.w3.org/2000/svg'}
def svg_summary(p):
    root=ET.parse(p).getroot()
    groups=root.findall('.//s:g',ns)
    nodes=[{'id':g.get('id'),'transform':g.get('transform'),'text':' '.join(''.join(g.itertext()).split())}
           for g in groups if 'node' in g.get('class','').split()]
    edges=[{'id':e.get('id'),'d':e.get('d'),'start':e.get('marker-start'),'end':e.get('marker-end')}
           for e in root.findall('.//s:path',ns) if e.get('data-edge')=='true']
    labels=[{'transform':g.get('transform'),'text':' '.join(''.join(g.itertext()).split())}
            for g in groups if g.get('class')=='edgeLabel' and g.get('transform')]
    return {'viewBox':root.get('viewBox'),'nodes':nodes,'edges':edges,'labels':labels}
base_svg=svg_summary(ARCHIVE/'author/initial/render/F10.2-base-8452f56.svg')
head_svg=svg_summary(ARCHIVE/'author/initial/render/F10.2-head-88a4eb4.svg')
assert len(head_svg['edges'])==5 and len(head_svg['labels'])==5
assert base_svg['nodes']==head_svg['nodes']
assert base_svg['edges']==head_svg['edges']
assert sum(a!=b for a,b in zip(base_svg['labels'],head_svg['labels']))==1
(RECEIPTS/'svg-structure.json').write_text(json.dumps({'base':base_svg,'author_first_head':head_svg},indent=2)+'\n')

# Render the frozen final fence independently with the installed tool.
(RENDER/'F10.2-bc997e7.mmd').write_bytes(fences[HEAD])
(RENDER/'puppeteer.json').write_text('{"args":["--no-sandbox","--disable-gpu"]}\n')
cmd(['mmdc','--version'],name='mmdc-version.log')
for ext,extra in [('svg',[]),('png',['-b','white','-s','1'])]:
    cmd(['mmdc','-p',str(RENDER/'puppeteer.json'),'-i',str(RENDER/'F10.2-bc997e7.mmd'),
         '-o',str(RENDER/f'F10.2-bc997e7.{ext}'),*extra],name=f'render-final-{ext}.log')
final_svg=svg_summary(RENDER/'F10.2-bc997e7.svg')
assert final_svg['nodes']==head_svg['nodes'] and final_svg['edges']==head_svg['edges']
assert final_svg['labels']==head_svg['labels']
(RECEIPTS/'independent-render.json').write_text(json.dumps(final_svg,indent=2)+'\n')

# Use a disposable archive for executable docs checks. No .venv bootstrap:
# WaveDrom is absent from system Python; manager full make check is separate.
with tempfile.TemporaryDirectory(prefix='r234-docs-') as tmp:
    scratch=Path(tmp)
    data=git('archive',HEAD)
    with tarfile.open(fileobj=io.BytesIO(data)) as t:t.extractall(scratch,filter='data')
    cmd(['make','-j1','lint','links','matrix','modmatrix'],cwd=scratch,name='docs-focused.log')
    p=scratch/ARCH
    p.write_bytes(p.read_bytes().replace(b'ADOPTED --> DEFAULTS:',b'ADOPTED --> --> DEFAULTS:'))
    cmd(['make','-j1','lint'],cwd=scratch,expected=2,name='docs-negative-control.log')
    assert b'Parse error on line 5' in (RECEIPTS/'docs-negative-control.log').read_bytes()
cmd(['make','-j1','stale'],name='stale.log')
cmd(['git','diff','--check',BASE,HEAD],name='diff-check.log')
print(json.dumps({'scope':scope,'manifest_files_verified':len(verified),
                  'render_graph_matches':True,'docs_focused':'PASS','negative_control':'expected failure'},indent=2))
