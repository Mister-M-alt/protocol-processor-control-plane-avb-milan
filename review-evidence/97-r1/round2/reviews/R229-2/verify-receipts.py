#!/usr/bin/env python3
"""Read-only verification: rtk proxy python3 verify-receipts.py CLONE RECEIPTS."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

source=Path(sys.argv[1]).resolve()
out=Path(sys.argv[2]).resolve()
def read(name):return json.loads((out/name).read_text())
def git(*args):return subprocess.check_output(['git','-C',str(source),*args])
head='5c45845ad15bd7995f20c81d7fd61501e5ca9d7e'
prior='1eb20dc4911880de10b745cc284e7dde306788b5'
assert git('rev-parse','HEAD').decode().strip()==head
assert git('rev-parse','HEAD^{tree}').decode().strip()=='8768f7e640ee62fd62862bec7117155fcab3ddbb'
scope=read('scope-and-retention.json')
changed=git('diff','--name-only',prior,head).decode().splitlines()
assert sorted(changed)==sorted(scope['changed_since_prior'])
for ref in [scope['base'],prior,head]:
    assert git('rev-parse',ref+':hdl').decode().strip()=='abf21fefc041df67029d227e67391558fd15ddb3'
assert git('show',prior+':tb/pp_top/sim_main.cpp')==git('show',head+':tb/pp_top/sim_main.cpp')
assert git('show',prior+':tb/pp_top/pp_top_wrap.sv')==git('show',head+':tb/pp_top/pp_top_wrap.sv')
oldmake=git('show',prior+':tb/pp_top/Makefile').decode()
newmake=git('show',head+':tb/pp_top/Makefile').decode()
assert oldmake.split('# Compile the actual bench')[0]==newmake.split('# Compile the actual bench')[0]
bench=git('show',head+':tb/pp_top/sim_main.cpp').decode().splitlines(True)
assert ''.join(bench[:79]+bench[84:])==git('show',scope['base']+':tb/pp_top/sim_main.cpp').decode()
helper=git('show',head+':tb/pp_top/fixture_guards.py').decode()
helper=helper.replace('import os\n','').replace('        # The diagnostic checks below require the compiler\'s English wording.\n        compiler_env = os.environ.copy()\n        compiler_env["LC_ALL"] = "C"\n','').replace('                env=compiler_env,\n','')
assert helper==git('show',prior+':tb/pp_top/fixture_guards.py').decode()
files=read('initial-files.json')
for name,row in files.items():
    p=source/name
    assert hashlib.sha256(p.read_bytes()).hexdigest()==row['sha256'],name
    assert oct(p.lstat().st_mode)==row['mode'],name
    assert git('hash-object','--',name).decode().strip()==row['blob'],name
assert git('ls-files','--stage')==(out/'initial-index.txt').read_bytes()
assert git('ls-tree','-r','HEAD')==(out/'initial-tree.txt').read_bytes()
assert not git('diff','--name-only')
assert not git('diff','--cached','--name-only')
assert not git('ls-files','--unmerged')
assert not git('status','--porcelain=v2')
assert subprocess.run(['git','-C',str(source),'symbolic-ref','-q','HEAD'],stdout=subprocess.DEVNULL).returncode==1
index=Path(git('rev-parse','--git-path','index').decode().strip())
if not index.is_absolute():index=source/index
assert hashlib.sha256(index.read_bytes()).hexdigest()==(out/'initial-raw-index.sha256').read_text().strip()
downloads=read('downloaded-evidence.json')
for row in downloads:
    b=(out/row['file']).read_bytes()
    assert hashlib.sha1(b'blob '+str(len(b)).encode()+b'\0'+b).hexdigest()==row['git_blob']
    assert hashlib.sha256(b).hexdigest()==row['sha256']
author=out/'public-evidence/round2/author'
manifest=json.loads((author/'MANIFEST.json').read_text())
published={r['file']:r for r in read('public-evidence/MANIFEST.json')}
checked=0
published_checked=0
for name,row in published.items():
    p=out/'public-evidence'/name
    if p.exists():
        assert hashlib.sha256(p.read_bytes()).hexdigest()==row['published_sha256'],name
        published_checked+=1
for name,row in manifest.items():
    p=author/name
    if not p.exists():continue
    entry=published['round2/author/'+name]
    assert row['sha256']==entry['original_sha256'],name
    if not entry['path_redacted']:
        assert len(p.read_bytes())==row['bytes'],name
    checked+=1
results=read('focused/results.json')
assert len(results)==18,len(results)
assert all(row['returncode']==row['expected'] for row in results)
compile_records=[json.loads(p.read_text()) for p in sorted((out/'focused/compiler').glob('*.json'))]
syntax=[row for row in compile_records if '-fsyntax-only' in row['argv']]
for row in syntax:
    if '/prior/' not in row['cwd']:
        assert row['env']['LC_ALL']=='C'
        assert row['env']['R229_SENTINEL']=='inherited-reviewer-value'
normal=[row for row in syntax if '/normal/' in row['cwd']]
assert len(normal)==16,len(normal)
wire='SRP VID fixture must differ from product default 2 in the 16-bit wire value'
class_d='SRP VID fixture must differ from product default 2 in the 12-bit class-D value'
expectations={None:(), '5A3C':(), '0002':(wire,class_d), '1002':(class_d,)}
for row in normal:
    defines=[a.split('=0x')[1] for a in row['argv'] if a.startswith('-DPP_TOP_SRP_DOM_DEF_VID=0x')]
    value=defines[0] if defines else None
    expected=expectations[value]
    errors=[l for l in row['diagnostics'].splitlines() if 'error:' in l]
    assert len(errors)==len(expected)
    assert bool(row['returncode'])==bool(expected)
    assert all(any('static assertion' in l and msg in l for l in errors) for msg in expected)
calls=[json.loads(l) for l in (out/'focused/verilator-invocations.jsonl').read_text().splitlines()]
models=[c for c in calls if '--getenv' not in c['effective']]
for call in models:
    a=call['effective'];assert 1<=int(a[a.index('-j')+1])<=8
    assert call['env']['LC_ALL'] in (None,'fr_FR.UTF-8')
    assert call['env']['R229_SENTINEL']=='inherited-reviewer-value'
    if '--Mdir' in a and a[a.index('--Mdir')+1].startswith('/tmp/pp-top-vid-guards-'):
        assert not Path(a[a.index('--Mdir')+1]).exists()
for n in range(7,13):
    assert not any('/'+f'{n:02d}'+'-' in c['cwd'] for c in calls)
assert (out/'focused/build_tally.txt').read_text().splitlines()==['1391 0','20 0']
runtime=(out/'focused/14-default-and-fixture.log').read_text()
assert runtime.count('1411 checks: 1411 PASS, 0 FAIL')==1
assert '[build default, SRP_DOM_DEF_VID_P 0x0002] 1391 checks, 0 failures' in runtime or re.search(r'\[build default.*1391 checks, 0 failures',runtime)
for name,expected in [('07-missing-binding','[build fixture, SRP_DOM_DEF_VID_P 0x5a3c] 20 checks, 13 failures'),('08-child-default','1411 checks: 1411 PASS, 0 FAIL')]:
    root=out/'public-evidence/reviews/R229-1'
    record=json.loads((root/(name+'.json')).read_text());assert record['head']==prior
    assert expected in (root/(name+'.log')).read_text()
    sr=json.loads((root/(name+'-source.json')).read_text())
    assert hashlib.sha256((source/sr['path']).read_bytes()).hexdigest()==sr['before']
manager=out/'public-evidence/round2/manager'
completed=json.loads((manager/'full-native/complete.json').read_text())
assert completed['head']==head and completed['exit_code']==0 and completed['finished']
native=json.loads((manager/'full-native/results.json').read_text())
assert native['head']==head and len(native['results'])==9
assert all(r['exit_code']==0 for r in native['results'])
jobs=[]
for run,event in [(35708824838,'push'),(35708829801,'pull_request')]:
    meta=read(f'hosted-{run}-run.json')
    assert meta['head_sha']==head and meta['event']==event and meta['conclusion']=='success' and meta['status']=='completed'
    run_jobs=read(f'hosted-{run}-jobs.json')['jobs'];assert len(run_jobs)==3
    assert all(j['conclusion']=='success' and j['status']=='completed' for j in run_jobs)
    jobs.extend(run_jobs)
    for job in run_jobs:
        log=(out/f'hosted-log-{job["id"]}.txt').read_text()
        expected_sha=head if event=='push' else '6439edc9e2a2fa06c4f9ce2e35d4ccaa273e321d'
        assert re.search(r'git log -1 --format=%H\n[^\n]*'+expected_sha,log)
        if job['name']=='suites':
            for expected in ['Verilator 5.050','PASS pp_top (1411 checks: 1411 PASS, 0 FAIL)','suites: 14943 checks total, 0 failing']:assert expected in log
merge=read('hosted-pr-merge-commit.json')
assert merge['tree']['sha']=='8768f7e640ee62fd62862bec7117155fcab3ddbb'
assert [p['sha'] for p in merge['parents']]==['8452f564294300a82d56eed464276576f65f4d58',head]
print(json.dumps({'source_integrity':'PASS','tracked_files':len(files),'downloaded_blobs_verified':len(downloads),'published_manifest_entries_verified':published_checked,'original_author_manifest_links_verified':checked,'focused_completed_probes':len(results),'recorded_syntax_compiler_calls':len(syntax),'corrected_normal_four_case_sets':len(normal)//4,'all_recorded_job_caps_and_temp_cleanup':'PASS','retained_R229_1_M25_M31':'PASS','manager_completed_native_commands':len(native['results']),'completed_hosted_jobs':len(jobs),'hosted_PR_merge_tree_equals_reviewed_tree':True},indent=2))
