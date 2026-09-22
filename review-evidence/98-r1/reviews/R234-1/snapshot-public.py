"""Read-only GitHub status snapshot. Does not request review/comment bodies."""
from pathlib import Path
import datetime
import hashlib
import json
import urllib.request

OUT=Path(__file__).resolve().parent
BASE='https://api.github.com/repos/Mister-M-alt/protocol-processor-control-plane-avb-milan'
HEAD='bc997e7c00e50e3d59b97987950bfbf6cc442182'
items=[(BASE+'/commits/'+HEAD+'/check-runs?per_page=100','hosted-checks-final.json'),
       (BASE+'/actions/runs?head_sha='+HEAD+'&per_page=100','hosted-runs-final.json'),
       (BASE+'/commits/main','donor-main-final.json')]
data={}
for url,name in items:
    b=urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'R234-public-review'}),timeout=40).read()
    (OUT/'public'/name).write_bytes(b)
    t=datetime.datetime.now(datetime.timezone.utc).isoformat()
    with (OUT/'receipts/downloads.jsonl').open('a') as f:
        f.write(json.dumps({'utc':t,'url':url,'file':name,'sha256':hashlib.sha256(b).hexdigest()})+'\n')
    data[name]=json.loads(b)
checks=[{k:x[k] for k in ['id','name','head_sha','status','conclusion','started_at','completed_at','html_url']}
        for x in data['hosted-checks-final.json']['check_runs']]
runs=[{k:x[k] for k in ['id','name','event','head_sha','status','conclusion','created_at','updated_at','html_url']}
      for x in data['hosted-runs-final.json']['workflow_runs']]
main=data['donor-main-final.json']
summary={'observed_utc':t,'head':HEAD,'checks':checks,'runs':runs,
         'live_main':main['sha'],'live_main_tree':main['commit']['tree']['sha'],
         'live_main_message':main['commit']['message'],
         'live_main_parents':[x['sha'] for x in main['parents']],
         'new_main_candidate_validated_by_this_reviewer':False}
(OUT/'receipts/hosted-final-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
