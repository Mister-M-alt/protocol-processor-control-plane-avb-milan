import hashlib,json,os,subprocess,sys,time
from pathlib import Path
assert sys.argv[1]=='quoted compiler marker'
a=sys.argv[2:]
r=subprocess.run(['/usr/bin/g++',*a],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
records=Path(__file__).with_name('compiler');records.mkdir(exist_ok=True)
stamp=str(time.time_ns())
(records/(stamp+'.log')).write_bytes(r.stdout)
row={'cwd':os.getcwd(),'argv':a,'returncode':r.returncode,
     'env':{k:os.environ.get(k) for k in ('LANG','LANGUAGE','LC_ALL','LC_MESSAGES','R229_SENTINEL')},
     'diagnostics':r.stdout.decode(errors='replace')}
(records/(stamp+'.json')).write_text(json.dumps(row,indent=2)+'\n')
sys.stdout.buffer.write(r.stdout)
raise SystemExit(r.returncode)
