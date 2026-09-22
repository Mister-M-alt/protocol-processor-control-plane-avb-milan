#!/usr/bin/env python3
import subprocess,json,pathlib,hashlib,os,stat
OUT=pathlib.Path(__file__).resolve().parent
REPO=pathlib.Path('$VALIDATION_STORAGE/reviews/r229-100-r1')
ENV=os.environ.copy();ENV['GIT_OPTIONAL_LOCKS']='0'
def run(args):return subprocess.check_output(args,cwd=REPO,env=ENV)
manifest=[]
for raw in run(['git','ls-files','-z']).split(b'\0')[:-1]:
 p=REPO/os.fsdecode(raw);st=p.lstat();manifest.append({'path':os.fsdecode(raw),'kind':'symlink' if stat.S_ISLNK(st.st_mode) else 'regular' if stat.S_ISREG(st.st_mode) else 'other','mode':oct(stat.S_IMODE(st.st_mode)),'sha256':hashlib.sha256(os.readlink(p).encode() if p.is_symlink() else p.read_bytes()).hexdigest()})
(OUT/'final-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
initial=json.loads((OUT/'initial-identity.json').read_text());first=json.loads((OUT/'initial-manifest.json').read_text())
idx=REPO/run(['git','rev-parse','--git-path','index']).decode().strip()
identity={'head':run(['git','rev-parse','HEAD']).decode().strip(),'tree':run(['git','rev-parse','HEAD^{tree}']).decode().strip(),'status':run(['git','status','--porcelain=v2','--untracked-files=all']).decode(),'index_sha256':hashlib.sha256(idx.read_bytes()).hexdigest(),'index_entries_sha256':hashlib.sha256(run(['git','ls-files','--stage','-z'])).hexdigest(),'tracked_files':len(manifest),'tracked_manifest_unchanged':manifest==first,'detached':subprocess.run(['git','symbolic-ref','-q','HEAD'],cwd=REPO,env=ENV,stdout=subprocess.PIPE).returncode==1,'cached_diff':run(['git','diff','--cached','--raw','HEAD']).decode(),'working_diff':run(['git','diff','--raw']).decode(),'unmerged_entries':run(['git','ls-files','--unmerged']).decode()}
identity['index_byte_exact']=identity['index_sha256']==initial['index_sha256'];identity['index_entries_unchanged']=identity['index_entries_sha256']==initial['index_entries_sha256']
(OUT/'final-identity.json').write_text(json.dumps(identity,indent=2)+'\n');print(json.dumps(identity,indent=2))
assert identity['head']==initial['head'] and identity['tracked_manifest_unchanged'] and identity['index_byte_exact'] and identity['index_entries_unchanged'] and identity['detached']
assert not any(identity[k] for k in ['status','cached_diff','working_diff','unmerged_entries'])
