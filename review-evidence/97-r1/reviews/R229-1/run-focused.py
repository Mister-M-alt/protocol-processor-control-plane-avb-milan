#!/usr/bin/env python3
import pathlib,subprocess,json,os,tempfile,tarfile,io,time,difflib,hashlib
OUT=pathlib.Path(__file__).resolve().parent
REPO=pathlib.Path('$VALIDATION_STORAGE/reviews/r229-100-r1')
HEAD='1eb20dc4911880de10b745cc284e7dde306788b5'
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip()==HEAD
SCRATCH=pathlib.Path(tempfile.mkdtemp(prefix='r229-pr100-'))
(OUT/'scratch-path.txt').write_text(str(SCRATCH)+'\n')
archive=subprocess.check_output(['git','archive',HEAD],cwd=REPO)
ENV=os.environ.copy();ENV.update({'R229_RECEIPTS':str(OUT),'MAKEFLAGS':'','MFLAGS':''})
V=str(OUT/'probe-tools/verilator'); C=str(OUT/'probe-tools/cxx-record')
results=[]
def checkout(name):
    d=SCRATCH/name;d.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive)) as t:t.extractall(d,filter='data')
    return d

def run(name,argv,cwd,expected=0):
    env=ENV.copy();env['R229_PROBE']=name
    start=time.time()
    with (OUT/(name+'.log')).open('wb') as f: r=subprocess.run(argv,cwd=cwd,env=env,stdout=f,stderr=subprocess.STDOUT)
    row={'name':name,'argv':argv,'cwd':str(cwd),'head':HEAD,'start':start,'seconds':time.time()-start,'returncode':r.returncode,'expected':expected}
    results.append(row);(OUT/(name+'.json')).write_text(json.dumps(row,indent=2)+'\n');(OUT/'focused-results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(row),flush=True)
    if r.returncode!=expected:raise RuntimeError(name)
    return (OUT/(name+'.log')).read_text()

def mutation(name,old,new,path='tb/pp_top/sim_main.cpp'):
    d=checkout(name);f=d/path;before=f.read_text();assert before.count(old)==1
    after=before.replace(old,new);f.write_text(after)
    (OUT/(name+'.patch')).write_text(''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile='a/'+path,tofile='b/'+path)))
    (OUT/(name+'-source.json')).write_text(json.dumps({'path':path,'before':hashlib.sha256(before.encode()).hexdigest(),'after':hashlib.sha256(after.encode()).hexdigest()},indent=2)+'\n')
    return d

normal=checkout('normal')
run('01-fixture-guards',['make','-j1','fixture-guards','VERILATOR='+V,'CXX='+C],normal/'tb/pp_top')
for name,expr,msg in [
 ('02-remove-wire','SRP_DEF_VID != 2','16-bit wire'),
 ('03-remove-class-d','(SRP_DEF_VID & 0x0FFFu) != 2','12-bit class-D')]:
    old='static_assert('+expr+',\n              "SRP VID fixture must differ from product default 2 in the '+msg+' value");\n'
    d=mutation(name,old,'')
    run(name,['make','-j1','fixture-guards','VERILATOR='+V,'CXX='+C],d/'tb/pp_top',2)
d=mutation('04-extra-error','#ifdef PP_TOP_SRP_DOM_DEF_VID\n','#ifdef PP_TOP_SRP_DOM_DEF_VID\n#if PP_TOP_SRP_DOM_DEF_VID == 0x0002\n#error R229 unrelated compiler error\n#endif\n')
run('04-extra-error',['make','-j1','fixture-guards','VERILATOR='+V,'CXX='+C],d/'tb/pp_top',2)
run('05-ordinary-builds',['make','-j1','VERILATOR='+V],normal/'tb/pp_top')
print('FOCUSED INITIAL PROBES COMPLETE',flush=True)
