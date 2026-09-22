#!/usr/bin/env python3
import pathlib,subprocess,json,os,tempfile,tarfile,io,time,difflib,hashlib
TOOLS=pathlib.Path(__file__).resolve().parent/'probe-tools'
OUT=pathlib.Path(tempfile.mkdtemp(prefix='r229-reproduction-receipts-'))
print('Reproduction receipts:',OUT,flush=True)
REPO=pathlib.Path('$VALIDATION_STORAGE/reviews/r229-100-r1')
HEAD='1eb20dc4911880de10b745cc284e7dde306788b5'
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip()==HEAD
SCRATCH=pathlib.Path(tempfile.mkdtemp(prefix='r229-pr100-'))
(OUT/'scratch-path.txt').write_text(str(SCRATCH)+'\n')
archive=subprocess.check_output(['git','archive',HEAD],cwd=REPO)
ENV=os.environ.copy();ENV.update({'R229_RECEIPTS':str(OUT),'MAKEFLAGS':'','MFLAGS':''})
V=str(TOOLS/'verilator'); C=str(TOOLS/'cxx-record')
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

import shlex,re
normal=checkout('normal')
run('01-fixture-guards',['make','-j1','fixture-guards','VERILATOR='+V,'CXX='+C],normal/'tb/pp_top')
for name,expr,msg in [
 ('02-remove-wire','SRP_DEF_VID != 2','16-bit wire'),
 ('03-remove-class-d','(SRP_DEF_VID & 0x0FFFu) != 2','12-bit class-D')]:
    old='static_assert('+expr+',\n              "SRP VID fixture must differ from product default 2 in the '+msg+' value");\n'
    d=mutation(name,old,'')
    run(name,['make','-j1','fixture-guards','VERILATOR='+V,'CXX='+C],d/'tb/pp_top',2)
d=mutation('04-extra-error-actual','#ifdef PP_TOP_SRP_DOM_DEF_VID\nconstexpr uint16_t    SRP_DEF_VID', '#ifdef PP_TOP_SRP_DOM_DEF_VID\n#if PP_TOP_SRP_DOM_DEF_VID == 0x0002\n#error R229 unrelated compiler error\n#endif\nconstexpr uint16_t    SRP_DEF_VID')
run('04-extra-error',['make','-j1','fixture-guards','VERILATOR='+V,'CXX='+C],d/'tb/pp_top',2)
run('05-ordinary-builds',['make','-j1','VERILATOR='+V],normal/'tb/pp_top')
wire='SRP VID fixture must differ from product default 2 in the 16-bit wire value'
class_d='SRP VID fixture must differ from product default 2 in the 12-bit class-D value'
for value in ('0002','1002'):
    name='06-full-refusal-'+value
    d=checkout(name)
    dry=run(name+'-recipe',['make','-n','run','VERILATOR='+V,'SRP_VID_FIXTURE='+value],d/'tb/pp_top')
    commands=dry.replace('\\\n',' ').splitlines()
    line=next(x for x in commands if x.startswith(V+' ') and '--Mdir obj_vid' in x)
    log=run(name,shlex.split(line),d/'tb/pp_top',2)
    errors=[x for x in log.splitlines() if 'error:' in x]
    messages=[msg for msg in (wire,class_d) if any(msg in x and 'static assertion' in x for x in errors)]
    assert messages==([wire,class_d] if value=='0002' else [class_d]), errors
    assert len(errors)==len(messages), errors
    assert not (d/'tb/pp_top/obj_vid/Vpp_top_vid').exists()
    (OUT/(name+'-diagnostics.json')).write_text(json.dumps({'errors':errors,'messages':messages,'fixture_executable_exists':False},indent=2)+'\n')
d=mutation('07-missing-binding','      .DOM_DEF_VID_P    (SRP_DOM_DEF_VID_P),\n','', 'hdl/top/protocol_processor_top.sv')
log=run('07-missing-binding',['make','-j1','VERILATOR='+V],d/'tb/pp_top',2)
assert '[build default, SRP_DOM_DEF_VID_P 0x0002] 1391 checks, 0 failures' in log
assert '[build fixture, SRP_DOM_DEF_VID_P 0x5a3c] 20 checks, 13 failures' in log
assert len(re.findall(r'^FAIL:',log,re.M))==13
# Each failed observation must remain a DV observation.
assert all('DV' in line for line in log.splitlines() if line.startswith('FAIL:'))
d=mutation('08-child-default',"DOM_DEF_VID_P  = 16'd2", "DOM_DEF_VID_P  = 16'd7", 'hdl/srp/KL_srp_top.sv')
log=run('08-child-default',['make','-j1','VERILATOR='+V],d/'tb/pp_top')
assert '[build default, SRP_DOM_DEF_VID_P 0x0002] 1391 checks, 0 failures' in log
assert '[build fixture, SRP_DOM_DEF_VID_P 0x5a3c] 20 checks, 0 failures' in log
assert '1411 checks: 1411 PASS, 0 FAIL' in log
print('ALL REMAINING PROBES COMPLETE',flush=True)
