#!/usr/bin/env python3
"""Record transactions, exact prefix responses, and strong native controls."""
import argparse,csv,datetime,hashlib,json,os,re,shutil,subprocess,time
from pathlib import Path
SOURCE=Path(__file__).resolve().parent
ROOT=SOURCE.parents[2]
MODES=['gpu_field_core','software_split']
CONFIG='-DVX_CFG_FORZA_MMIO_VECTOR_ENABLE -DVX_CFG_FORZA_MMIO_ENABLE -DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE -DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
def fp(p):
 p=Path(p).resolve();return {'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
def fields(s):return dict(re.findall(r'(\w+)=([^\s]+)',s))
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--stage',choices=['build','smoke','run','all'],default='all');p.add_argument('--modes',default=','.join(MODES));p.add_argument('--quick',action='store_true');a=p.parse_args();selected=a.modes.split(',')
 if any(x not in MODES for x in selected):p.error('Unknown mode')
 build=a.build_dir.resolve();app=build/'tests/regression/forza_pgas_record_roles';libs=build/'sw/runtime';manifest=app/'record-manifest.json';report=ROOT/'docs/proposals/forza_pgas_record_roles_results';env=os.environ.copy();env.pop('DEBUG',None);env.update(CONFIGS=CONFIG,VORTEX_DRIVER='simx',LD_LIBRARY_PATH=str(libs))
 def save(m):
  manifest.write_text(json.dumps(m,indent=2)+'\n');report.with_suffix('.json').write_text(json.dumps(m,indent=2)+'\n');rows=[{'cohort':r['cohort'],**r['result'],'wall_seconds':r['wall_seconds'],'log':r['log']}for r in m['runs']if r.get('result')]
  if rows:
   with report.with_suffix('.csv').open('w',newline='')as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
 if a.stage in ['build','all']:
  app.mkdir(parents=True,exist_ok=True);(app/'logs').mkdir(exist_ok=True);shutil.copy2(SOURCE/'Makefile',app/'Makefile');required=[libs/n for n in ['libsimx.so','libvortex.so','libvortex-simx.so']]+[build/'sw/kernel/libvortex2.a'];files=list(SOURCE.glob('*.cpp'))+list(SOURCE.glob('*.h'))+[SOURCE/'Makefile',SOURCE/'README.md',SOURCE/'run_matrix.py',ROOT/'sw/common/forza_mmio_abi.h']
  m={'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'build_dir':str(build),'configs':CONFIG,'source_artifacts':[fp(x)for x in files],'shared_artifacts':[fp(x)for x in required],'builds':[],'artifacts':[],'runs':[]}
  for mode in MODES:
   cmd=['make','-C',str(app),'MODE='+mode,'-o',str(libs/'libvortex.so'),'-o',str(build/'sw/kernel/libvortex2.a')];log=app/'logs'/('build-'+mode+'.log')
   with log.open('w')as f:subprocess.run(cmd+['clean'],env=env,stdout=f,stderr=subprocess.STDOUT,check=True);subprocess.run(cmd+['-j2'],env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
   dest=app/'artifacts'/mode;dest.mkdir(parents=True,exist_ok=True)
   for original,final in [('forza_record_roles_'+mode,'forza_record_roles_'+mode),('kernel.vxbin','kernel.vxbin'),('kernel.elf','kernel.elf'),('kernel.dump','kernel.dump')]:shutil.copy2(app/original,dest/final);m['artifacts'].append(fp(dest/final))
   m['builds'].append({'mode':mode,'commands':[cmd+['clean'],cmd+['-j2']],'log':str(log)});save(m);print('BUILT',mode,flush=True)
 else:m=json.loads(manifest.read_text())
 def verify():
  for item in m['source_artifacts']+m['shared_artifacts']+m['artifacts']:
   if fp(item['path'])!=item:raise RuntimeError('Frozen artifact changed: '+item['path'])
 if a.stage=='build':verify();return
 cases=[('smoke',mode,64,n,pattern)for n,pattern in[(0,'random'),(1,'random'),(128,'random'),(128,'hot')]for mode in selected]
 if a.stage!='smoke':cases +=[('performance',mode,size,4096,pattern)for size in([64]if a.quick else[64,256,1024])for pattern in['random','hotset','hot']for mode in selected]
 done={(r['cohort'],r['mode'],r['keys'],r['requests'],r['pattern'])for r in m['runs']}
 for cohort,mode,keys,n,pattern in cases:
  if(cohort,mode,keys,n,pattern)in done:continue
  verify();work=app/'runs'/f'{cohort}-{mode}-k{keys}-u{n}-{pattern}-wide';work.mkdir(parents=True,exist_ok=True);log=work/'run.log';binary=app/'artifacts'/mode/('forza_record_roles_'+mode);cmd=[str(binary),'-k',str(binary.parent/'kernel.vxbin'),'-n',str(keys),'-u',str(n),'-p',pattern,'-d','wide','-s','17','-t','600'];print('START',cohort,mode,keys,n,pattern,flush=True);start=time.monotonic();timeout=False
  with log.open('w')as f:
   try:rc=subprocess.run(cmd,cwd=work,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=610).returncode
   except subprocess.TimeoutExpired:rc=-1;timeout=True
  lines=log.read_text().splitlines();rs=[fields(s)for s in lines if s.startswith('RESULT ')];ins=[fields(s)for s in lines if s.startswith('INPUT ')];row={'cohort':cohort,'mode':mode,'keys':keys,'requests':n,'pattern':pattern,'delta_distribution':'wide','command':cmd,'cwd':str(work),'log':str(log),'returncode':rc,'timed_out':timeout,'wall_seconds':time.monotonic()-start,'passed':rc==0 and bool(rs)and any(s.startswith('PASSED:')for s in lines),'result':rs[-1]if rs else None,'input':ins[-1]if ins else None};m['runs'].append(row);verify();save(m);print('PASS'if row['passed']else'FAIL',mode,'wall',round(row['wall_seconds'],3),row['result'],flush=True)
  if not row['passed']:raise SystemExit('Run failed: '+str(log))
 m['completed_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();save(m);print('COMPLETE',len(m['runs']),'cases',flush=True)
if __name__=='__main__':main()
