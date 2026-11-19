from pathlib import Path
import csv,datetime,hashlib,json,os,re,subprocess,time
root=Path('/export/nethomes/spark3100/vortex-forza-hwqueue');build=root/'build64-forza-record';docs=root/'docs/proposals'
a=json.loads((docs/'forza_pgas_record_results.json').read_text());b=json.loads((docs/'forza_pgas_record_gpu_opt_results.json').read_text());frozen={}
for m in(a,b):
 for x in m['source_artifacts']+m['shared_artifacts']+m['artifacts']:frozen[x['path']]=x
script=Path(__file__).resolve();manifest={'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'script':{'path':str(script),'sha256':hashlib.sha256(script.read_bytes()).hexdigest()},'frozen_artifacts':list(frozen.values()),'runs':[]}
env=os.environ.copy();env['VORTEX_DRIVER']='simx';env['LD_LIBRARY_PATH']=str(build/'sw/runtime');env.pop('DEBUG',None)
def verify():
 for x in frozen.values():assert hashlib.sha256(Path(x['path']).read_bytes()).hexdigest()==x['sha256'],x['path']
def save():
 (docs/'forza_pgas_record_repeats.json').write_text(json.dumps(manifest,indent=2)+'\n')
 rows=[{'cohort':r['cohort'],**r['result'],'wall_seconds':r['wall_seconds'],'log':r['log']}for r in manifest['runs']if r['result']]
 if rows:
  with(docs/'forza_pgas_record_repeats.csv').open('w',newline='')as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
for cohort,keys,seed in [('new_seed',64,29),('new_seed',1024,29),('exact_repeat',64,17)]:
 for mode in('gpu_lock','gpu_warp','gpu_owner','engine'):
  verify();opt=mode in('gpu_warp','gpu_owner');app=build/'tests/regression'/('forza_pgas_record_gpu_opt'if opt else'forza_pgas_record');binary=app/'artifacts'/mode/('forza_record_opt_'+mode if opt else'forza_record_'+mode);work=build/'tests/regression/forza_pgas_record_repeats'/f'{cohort}-{mode}-k{keys}-s{seed}';work.mkdir(parents=True,exist_ok=True);log=work/'run.log';cmd=[str(binary),'-k',str(binary.parent/'kernel.vxbin'),'-n',str(keys),'-u','4096','-p','hot','-d','wide','-s',str(seed),'-t','600'];print('START',cohort,keys,seed,mode,flush=True);start=time.monotonic()
  with log.open('w')as f:rc=subprocess.run(cmd,cwd=work,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=610).returncode
  lines=log.read_text().splitlines();result=[dict(re.findall(r'(\w+)=([^\s]+)',x))for x in lines if x.startswith('RESULT ')];passed=rc==0 and bool(result)and any(x.startswith('PASSED:')for x in lines);r={'cohort':cohort,'keys':keys,'seed':seed,'mode':mode,'optimized_control':opt,'command':cmd,'cwd':str(work),'log':str(log),'log_sha256':hashlib.sha256(log.read_bytes()).hexdigest(),'returncode':rc,'wall_seconds':time.monotonic()-start,'passed':passed,'result':result[-1]if result else None};manifest['runs'].append(r);verify();save();print('PASS'if passed else'FAIL',r,flush=True)
  if not passed:raise SystemExit(str(log))
manifest['completed_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();save();print('COMPLETE',len(manifest['runs']),flush=True)
