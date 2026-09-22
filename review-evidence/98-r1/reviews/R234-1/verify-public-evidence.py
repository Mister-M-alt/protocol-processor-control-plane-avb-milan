"""Verify downloaded factual archives; never reads historical review reports."""
from pathlib import Path
import hashlib
import json
import re
import struct

OUT=Path(__file__).resolve().parent
H=OUT/'public/history96'
manifest={x['file']:x for x in json.loads((H/'MANIFEST.json').read_text())}
sums={}
for line in (H/'reviews/R223-1/receipts/SHA256SUMS').read_text().splitlines():
    digest,_,name=line.partition('  ')
    sums[name.strip().lstrip('*').removeprefix('./')]=digest
verified=[]
for p in sorted(H.rglob('*')):
    if not p.is_file() or p.name in ['MANIFEST.json','SHA256SUMS']:continue
    name=str(p.relative_to(H));digest=hashlib.sha256(p.read_bytes()).hexdigest()
    assert digest==manifest[name]['published_sha256']
    row={'file':name,'sha256':digest,'manifest_match':True}
    if name.startswith('reviews/R223-1/receipts/'):
        assert digest==sums[name.removeprefix('reviews/R223-1/receipts/')]
        row['sha256sums_match']=True
    verified.append(row)
(OUT/'receipts/history96-hashes.json').write_text(json.dumps(verified,indent=2)+'\n')
def author_tree(name):
    return {x['path']:(x['sha'],x['mode']) for x in json.loads((OUT/'public'/name).read_text())['tree']
            if x['type']=='blob' and x['path'].startswith('review-evidence/98-r1/author/')}
a=author_tree('tree-c5f372ed.json');b=author_tree('tree-94f47697.json');assert a==b
ar=OUT/'public/archive/review-evidence/98-r1'
m={x['file']:x for x in json.loads((ar/'MANIFEST.json').read_text())};pngs=[]
author_sums={}
for line in (ar/'author/initial/SHA256SUMS').read_text().splitlines():
    digest,_,name=line.partition('  ');author_sums[name.strip().removeprefix('./')]=digest
for p in sorted((ar/'author/initial/render').glob('*.png')):
    blob=p.read_bytes();width,height=struct.unpack('>II',blob[16:24])
    row=m[str(p.relative_to(ar))];digest=hashlib.sha256(blob).hexdigest()
    assert digest==row['original_sha256']==row['published_sha256'] and not row['path_redacted']
    assert digest==author_sums[str(p.relative_to(ar/'author/initial'))]
    pngs.append(row|{'width':width,'height':height,'author_sha256sums_match':True})
(OUT/'receipts/archive-continuity.json').write_text(json.dumps({
    'unchanged_author_blob_mode_count':len(a),'source_archive':'c5f372ed8fc83bdd345137c435c34cc57c34ec85',
    'completion_archive':'94f47697f765e96a3c930dc09606fb7b559d3187','pngs':pngs},indent=2)+'\n')
native=json.loads((ar/'manager/full-native/results.json').read_text())
complete=json.loads((ar/'manager/full-native/complete.json').read_text())
assert native['head']==complete['head']=='bc997e7c00e50e3d59b97987950bfbf6cc442182'
assert complete['exit_code']==0 and len(native['results'])==9
assert all(x['exit_code']==0 for x in native['results'])
sweep=(ar/'manager/full-native/04.log').read_text()
rows=re.findall(r'^PASS (\w+) \((\d+) checks: (\d+) PASS, (\d+) FAIL\)$',sweep,re.M)
assert len(rows)==30 and sum(int(x[1]) for x in rows)==14943
assert all(x[1]==x[2] and x[3]=='0' for x in rows)
assert 'suites: 14943 checks total, 0 failing' in sweep
lint=(ar/'manager/full-native/03.log').read_text()
yosys=(ar/'manager/full-native/07.log').read_text()
summary={'head':native['head'],'base':native['base'],'completed':complete['finished'],
         'all_nine_commands_exit_zero':True,'suites':len(rows),'checks':14943,'failures':0,
         'lint_ok':len(re.findall('^LINT OK',lint,re.M)),
         'yosys_ok':len(re.findall('^YOSYS OK',yosys,re.M)),
         'yosys_xilinx_ok':len(re.findall('^YOSYS XILINX OK',yosys,re.M)),
         'historical_files_verified':len(verified),'author_files_unchanged_between_archives':len(a)}
(OUT/'receipts/public-evidence-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
