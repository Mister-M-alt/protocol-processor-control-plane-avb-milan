#!/usr/bin/env python3
"""R224 Makefile control test using synthetic binaries, not RTL evidence."""
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

out = Path(__file__).resolve().parent
head = 'ea93023fbdd31cbf718da0d8f1aab4d750bfd21a'
root = Path(tempfile.mkdtemp(prefix='r224-pr96-tally-'))
with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['git', 'archive', head]))) as tf:
    tf.extractall(root, filter='data')
fake = root / 'fake-verilator'
fake.write_text(r'''#!/usr/bin/env python3
import pathlib,sys
args=sys.argv[1:]
fixture='--Mdir' in args
where=pathlib.Path('obj_vid' if fixture else 'obj_dir')
where.mkdir(exist_ok=True)
name=args[args.index('-o')+1]
code="""#!/usr/bin/env python3
import os,pathlib,sys
kind=__KIND__
mode=os.environ['R224_TALLY_MODE']
checks=20 if kind=='fixture' else 1391
failed=mode==kind+'_fails'
if mode!=kind+'_missing':
    with pathlib.Path('obj_dir/build_tally.txt').open('a') as f:
        f.write(f'{checks} {int(failed)}\\n')
        if mode=='extra_row' and kind=='fixture': f.write('7 0\\n')
print(f'[build {kind}] {checks} checks, {int(failed)} failures')
sys.exit(1 if failed else 0)
"""
p=where/name
p.write_text(code.replace('__KIND__',repr('fixture' if fixture else 'default')))
p.chmod(0o755)
''')
fake.chmod(0o755)
cwd = root / 'tb/pp_top'
results=[]
for mode in ['success','default_fails','fixture_fails','default_missing','fixture_missing','extra_row','stale_row']:
    (cwd/'obj_dir').mkdir(exist_ok=True)
    if mode=='stale_row': (cwd/'obj_dir/build_tally.txt').write_text('8888 99\n')
    cmd=['make','-j1','VERILATOR='+str(fake)]
    r=subprocess.run(cmd,cwd=cwd,env={**os.environ,'R224_TALLY_MODE':mode},capture_output=True,text=True)
    text=r.stdout+r.stderr
    canonical=re.findall(r'(?m)^\d+ checks: \d+ PASS, \d+ FAIL$',text)
    expected_success=mode in ['success','stale_row']
    assert (r.returncode==0)==expected_success,(mode,r.returncode,text)
    assert canonical==(['1411 checks: 1411 PASS, 0 FAIL'] if expected_success else []),(mode,canonical)
    path=out/'tally-probes';path.mkdir(exist_ok=True)
    (path/(mode+'.log')).write_text(text)
    results.append(dict(mode=mode,command=cmd,cwd=str(cwd),exit_code=r.returncode,canonical=canonical))
(out/'tally-probe-results.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
