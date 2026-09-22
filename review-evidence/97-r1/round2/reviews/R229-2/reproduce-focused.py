#!/usr/bin/env python3
"""R229 independent probes. Usage: rtk proxy python3 reproduce-focused.py CLONE NEW_OUTPUT.

Uses installed tools only; archives source into disposable scratch. Sequential
compiles, outer make -j1, effective Verilator -j8. Never edits CLONE.
"""
import argparse
import difflib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('clone', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--resume', action='store_true', help='resume this runner after a recorded harness interruption')
args = parser.parse_args()
source = args.clone.resolve()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=args.resume)
scratch = Path((out/'scratch.txt').read_text().strip()) if args.resume else Path(tempfile.mkdtemp(prefix='r229-r2-'))
(out/'scratch.txt').write_text(str(scratch)+'\n')
head = '5c45845ad15bd7995f20c81d7fd61501e5ca9d7e'
prior = '1eb20dc4911880de10b745cc284e7dde306788b5'
def git(*argv):
    return subprocess.check_output(['git', '-C', str(source), *argv])
assert git('rev-parse','HEAD').decode().strip() == head
base_env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', MAKEFLAGS='-j1',
                CMAKE_BUILD_PARALLEL_LEVEL='8', R229_SENTINEL='inherited-reviewer-value')
base_env.pop('LC_ALL',None)
base_env.update(LANG='en_US.UTF-8', LANGUAGE='de')
real_verilator = shutil.which('verilator')
real_cxx = shutil.which('g++')
assert real_verilator and real_cxx
wrapper = out/'verilator8'
wrapper.write_text('''#!/usr/bin/env python3
import json, os, subprocess, sys
from pathlib import Path
a=sys.argv[1:]
original=list(a)
if '--getenv' not in a and '--version' not in a:
    for i,v in enumerate(a):
        if v in ('-j','--build-jobs','--verilate-jobs'):
            n=int(a[i+1]); a[i+1]=str(8 if n==0 else min(n,8))
        elif v.startswith('-j') and len(v)>2 and v[2:].isdigit():
            n=int(v[2:]);a[i]='-j'+str(8 if n==0 else min(n,8))
    if '-j' not in a and not any(v.startswith('-j') for v in a):a += ['-j','8']
with open(Path(__file__).with_name('verilator-invocations.jsonl'),'a') as f:
    f.write(json.dumps({'cwd':os.getcwd(),'original':original,'effective':a,
        'env':{k:os.environ.get(k) for k in ('LANG','LANGUAGE','LC_ALL','LC_MESSAGES','R229_SENTINEL')}})+'\\n')
raise SystemExit(subprocess.call([REAL,*a]))
'''.replace('REAL', repr(real_verilator)))
wrapper.chmod(0o755)
cxx_wrapper=out/'capture-cxx.py'
cxx_wrapper.write_text('''import hashlib,json,os,subprocess,sys,time
from pathlib import Path
assert sys.argv[1]=='quoted compiler marker'
a=sys.argv[2:]
r=subprocess.run([REAL,*a],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
records=Path(__file__).with_name('compiler');records.mkdir(exist_ok=True)
stamp=str(time.time_ns())
(records/(stamp+'.log')).write_bytes(r.stdout)
row={'cwd':os.getcwd(),'argv':a,'returncode':r.returncode,
     'env':{k:os.environ.get(k) for k in ('LANG','LANGUAGE','LC_ALL','LC_MESSAGES','R229_SENTINEL')},
     'diagnostics':r.stdout.decode(errors='replace')}
(records/(stamp+'.json')).write_text(json.dumps(row,indent=2)+'\\n')
sys.stdout.buffer.write(r.stdout)
raise SystemExit(r.returncode)
'''.replace('REAL',repr(real_cxx)))
results=json.loads((out/'results.json').read_text()) if args.resume else []
def run(name, command, cwd, expect=0, env=None):
    previous=next((r for r in results if r['name']==name),None)
    if previous:
        assert previous['returncode']==expect
        return (out/(name+'.log')).read_text(errors='replace')
    started=time.time()
    with (out/(name+'.log')).open('wb') as f:
        p=subprocess.run(command,cwd=cwd,env=env or base_env,stdout=f,stderr=subprocess.STDOUT)
    row={'name':name,'argv':command,'cwd':str(cwd),'returncode':p.returncode,
         'expected':expect,'seconds':round(time.time()-started,3),
         'environment_overrides':{k:(env or base_env).get(k) for k in ('LANG','LANGUAGE','LC_ALL','LC_MESSAGES','R229_SENTINEL','MAKEFLAGS','PYTHONDONTWRITEBYTECODE')}}
    (out/(name+'.json')).write_text(json.dumps(row,indent=2)+'\n');results.append(row)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(name,'rc',p.returncode,'expected',expect,flush=True)
    assert p.returncode==expect, name
    return (out/(name+'.log')).read_text(errors='replace')
def copy(name, rev=head):
    dst=scratch/name
    if args.resume and dst.exists():return dst
    dst.mkdir()
    with tarfile.open(fileobj=io.BytesIO(git('archive',rev)),mode='r:') as archive:
        archive.extractall(dst,filter='data')
    return dst
def mutate(tree,file,old,new,name):
    if args.resume and (out/(name+'.patch')).exists():return
    f=tree/file;s=f.read_text();assert s.count(old)==1,(file,old)
    changed=s.replace(old,new);f.write_text(changed)
    (out/(name+'.patch')).write_text(''.join(difflib.unified_diff(s.splitlines(True),changed.splitlines(True),fromfile='a/'+file,tofile='b/'+file)))
def make(target='fixture-guards'):
    return ['make','-j1',target,'VERILATOR='+str(wrapper),
            'CXX=python3 '+str(cxx_wrapper)+" 'quoted compiler marker'"]
tools={}
for name,cmd in [('verilator',[real_verilator,'--version']),('gcc',[real_cxx,'--version']),('python',['python3','--version']),('make',['make','--version']),('locales',['locale','-a'])]:
    tools[name]=subprocess.check_output(cmd,text=True)
tools['gcc_catalogs']={lang:[str(x) for x in Path('/usr/share/locale',lang,'LC_MESSAGES').glob('gcc*')] for lang in ['de','fr']}
(out/'tools.json').write_text(json.dumps(tools,indent=2)+'\n')
normal=copy('normal'); old=copy('prior',prior)
for language in ['de','fr']:
    env=dict(base_env,LANGUAGE=language)
    text=run('01-prior-'+language,make(),old/'tb/pp_top',expect=2,env=env)
    assert 'FAIL: fixture 0002' in text
    text=run('02-head-'+language,make(),normal/'tb/pp_top',env=env)
    assert 'fixture guards: 4 cases PASS' in text
env=dict(base_env,LC_ALL='fr_FR.UTF-8',LANGUAGE='de:fr',LC_MESSAGES='de_DE.UTF-8')
run('03-head-inherited-lc-all',make(),normal/'tb/pp_top',env=env)
# Match the actual independent guards while preserving every other source byte.
s=(normal/'tb/pp_top/sim_main.cpp').read_text()
guard_lines=s.splitlines(keepends=True)
for name,msg in [('04-remove-wire','16-bit wire'),('05-remove-class-d','12-bit class-D')]:
    i=next(i for i,l in enumerate(guard_lines) if 'SRP VID fixture must differ' in l and msg in l)
    fragment=''.join(guard_lines[i-1:i+1]);assert 'static_assert' in fragment
    tree=copy(name);mutate(tree,'tb/pp_top/sim_main.cpp',fragment,'',name)
    text=run(name,make(),tree/'tb/pp_top',expect=2);assert 'FAIL: fixture 0002' in text
tree=copy('06-extra-error')
fragment='#ifdef PP_TOP_SRP_DOM_DEF_VID\nconstexpr uint16_t    SRP_DEF_VID = PP_TOP_SRP_DOM_DEF_VID;\n'
assert s.count(fragment)==1
mutate(tree,'tb/pp_top/sim_main.cpp',fragment,fragment+'#if PP_TOP_SRP_DOM_DEF_VID == 2\n#error R229_unrelated_error\n#endif\n','06-extra-error')
text=run('06-extra-error',make(),tree/'tb/pp_top',expect=2)
assert 'R229_unrelated_error' in text and 'FAIL: fixture 0002' in text
helper='tb/pp_top/fixture_guards.py'
mutants=[
 ('07-remove-normalization','        compiler_env["LC_ALL"] = "C"\n',''),
 ('08-remove-env-argument','                env=compiler_env,\n',''),
 ('09-mutate-caller','        compiler_env = os.environ.copy()\n','        compiler_env = os.environ\n'),
 ('10-drop-inherited-inputs','        compiler_env = os.environ.copy()\n','        compiler_env = {}\n'),
 ('11-normalize-verilator','[args.verilator, *vflags, "--Mdir", tmp], check=True','[args.verilator, *vflags, "--Mdir", tmp], check=True, env={**os.environ, "LC_ALL": "C"}'),
 ('12-drop-compiler-argument','*shlex.split(args.cxx), "-std=c++17",','*shlex.split(args.cxx)[:-1], "-std=c++17",'),
]
for name,before,after in mutants:
    tree=copy(name);mutate(tree,helper,before,after,name)
    env=dict(base_env,LANG='C',LANGUAGE='C',LC_ALL='C')
    text=run(name,make(),tree/'tb/pp_top',expect=2,env=env)
    assert 'FAILED (' in text and 'AssertionError' in text
    assert 'fixture guard default' not in text
run('13-required-wiring',['make','-j1','-n','run','VERILATOR='+str(wrapper)],normal/'tb/pp_top')
text=run('14-default-and-fixture',make('run'),normal/'tb/pp_top')
assert text.count('1411 checks: 1411 PASS, 0 FAIL')==1
(out/'build_tally.txt').write_bytes((normal/'tb/pp_top/obj_dir/build_tally.txt').read_bytes())
run('15-docs',['make','-j1','links','matrix','modmatrix'],normal)
run('16-diff-check',['git','diff','--check',prior,head],source)
# Record every tracked source hash in each scratch tree, excluding generated outputs.
paths=git('ls-files','-z').decode().strip('\0').split('\0')
differences={}
for tree in sorted(scratch.iterdir()):
    if not tree.is_dir():continue
    differences[tree.name]=[f for f in paths if not (tree/f).exists() or (tree/f).read_bytes()!=(source/f).read_bytes()]
(out/'scratch-source-differences.json').write_text(json.dumps(differences,indent=2)+'\n')
calls=[json.loads(l) for l in (out/'verilator-invocations.jsonl').read_text().splitlines()]
for call in calls:
    a=call['effective']
    if '--getenv' in a: continue
    assert '-j' in a and 1<=int(a[a.index('-j')+1])<=8
    for opt in ['--build-jobs','--verilate-jobs']:
        if opt in a: assert 1<=int(a[a.index(opt)+1])<=8
(out/'job-cap-check.json').write_text(json.dumps({'invocations':len(calls),'all_effective_compiles_capped_at_8':True},indent=2)+'\n')
print('ALL FOCUSED PROBES COMPLETED',flush=True)
