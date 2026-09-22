import pathlib,json,urllib.request,hashlib,concurrent.futures,datetime
out=pathlib.Path(__file__).resolve().parent
items=json.loads((out/'download-items.json').read_text())
def fetch(it):
 url,name=it
 b=urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'R234-public-review'}),timeout=60).read()
 p=out/'public'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b)
 return {'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'url':url,'file':name,'sha256':hashlib.sha256(b).hexdigest(),'bytes':len(b)}
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as ex:
 records=list(ex.map(fetch,items))
with (out/'receipts/downloads.jsonl').open('a') as f:
 for r in records:f.write(json.dumps(r)+'\n')
print('Downloaded',len(records),'public factual artifacts')
