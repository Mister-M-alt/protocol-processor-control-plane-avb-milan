"""Prove physical tracked bytes, modes, index and frozen HEAD without checkout writes."""
from pathlib import Path
import datetime
import hashlib
import json
import os
import stat
import subprocess
import sys

OUT=Path(__file__).resolve().parent
REPO=Path(sys.argv[1]).resolve()
def git(*args):
    return subprocess.run(['rtk','proxy','git','--no-replace-objects',*args],cwd=REPO,
       env=os.environ|{'GIT_OPTIONAL_LOCKS':'0'},capture_output=True,check=True).stdout
head=git('rev-parse','HEAD').decode().strip();tree=git('rev-parse','HEAD^{tree}').decode().strip()
assert head=='bc997e7c00e50e3d59b97987950bfbf6cc442182'
assert tree=='bd67eaac225513f84ecf3a228cf58a818e67b4da'
rows=[]
for row in git('ls-tree','-rz','HEAD').split(b'\0'):
    if not row:continue
    meta,n=row.split(b'\t',1);mode,kind,oid=meta.decode().split();name=n.decode();p=REPO/name
    st=p.lstat();assert kind=='blob' and stat.S_ISREG(st.st_mode),name
    data=p.read_bytes();actual=hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()
    assert actual==oid,name
    actual_mode='100755' if st.st_mode & stat.S_IXUSR else '100644';assert mode==actual_mode,name
    rows.append({'path':name,'mode':mode,'blob':oid,'sha256':hashlib.sha256(data).hexdigest()})
index=git('ls-files','--stage');assert index==(OUT/'receipts/initial-index.txt').read_bytes()
expected=''.join(f"{x['mode']} {x['blob']} 0\t{x['path']}\n" for x in rows).encode()
assert index==expected
status=git('status','--porcelain=v2','--branch');assert status==(OUT/'receipts/initial-status.txt').read_bytes()
assert not git('status','--porcelain=v1','--untracked-files=all')
assert not git('diff','--binary','HEAD') and not git('diff','--cached','--binary','HEAD')
assert not git('for-each-ref','refs/replace')
(OUT/'receipts/final-tracked-files.json').write_text(json.dumps(rows,indent=2)+'\n')
(OUT/'receipts/final-status.txt').write_bytes(status)
(OUT/'receipts/final-index.txt').write_bytes(index)
summary={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'head':head,'tree':tree,
         'tracked_files_checked_directly':len(rows),'executable_files':sum(x['mode']=='100755' for x in rows),
         'all_physical_blobs_and_modes_match_head':True,'index_equals_head_and_initial':True,
         'index_listing_sha256':hashlib.sha256(index).hexdigest(),'detached_and_clean':True,
         'no_untracked_files':True,'no_replace_refs':True,
         'candidate_agents_or_contributing':[x['path'] for x in rows if Path(x['path']).name in ['AGENTS.md','CONTRIBUTING.md']]}
(OUT/'receipts/final-integrity.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
