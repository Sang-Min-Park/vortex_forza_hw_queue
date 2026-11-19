#!/usr/bin/env python3
"""Record transactions, exact prefix responses, and strong native controls."""
import argparse,csv,datetime,hashlib,json,os,re,shutil,subprocess,time
from pathlib import Path
SOURCE=Path(__file__).resolve().parent
ROOT=SOURCE.parents[2]
MODES=['gpu_lock','gpu_warp','gpu_owner','warp_mmio32','warp_isa32','warp_wait32']
CONFIG='-DVX_CFG_FORZA_ISA_ENABLE -DVX_CFG_FORZA_MMIO_VECTOR_ENABLE -DVX_CFG_FORZA_MMIO_ENABLE -DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE -DVX_CFG_DCACHE_DISABLE -DVX_CFG_L2_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
def fp(p):
 p=Path(p).resolve();return {'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
def fields(s):return dict(re.findall(r'(\w+)=([^\s]+)',s))
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--stage',choices=['build','smoke','run','all'],default='all');p.add_argument('--modes',default=','.join(MODES));p.add_argument('--quick',action='store_true');p.add_argument('--cohort',default='validated');p.add_argument('--cores',type=int,default=2);p.add_argument('--skip-smoke',action='store_true');p.add_argument('--seed',type=int,default=17);p.add_argument('--report-prefix',type=Path);a=p.parse_args();selected=a.modes.split(',')
 if any(x not in MODES for x in selected):p.error('Unknown mode')
 if a.cores not in [2,4,8]:p.error('cores must be2,4,8')
 config=CONFIG.replace('-DVX_CFG_NUM_CORES=2','-DVX_CFG_NUM_CORES='+str(a.cores))
 build=a.build_dir.resolve();app=build/('tests/regression/forza_pgas_warp_message_parallel_'+a.cohort);libs=build/'sw/runtime';manifest=app/'record-manifest.json';report=a.report_prefix.resolve() if a.report_prefix else ROOT/('docs/proposals/forza_pgas_warp_message_parallel_results'+('' if a.cores==2 else '_c'+str(a.cores)));env=os.environ.copy();env.pop('DEBUG',None);env.update(CONFIGS=config,VORTEX_DRIVER='simx',LD_LIBRARY_PATH=str(libs))
 def save(m):
  manifest.write_text(json.dumps(m,indent=2)+'\n');report.with_suffix('.json').write_text(json.dumps(m,indent=2)+'\n');rows=[{'cohort':r['cohort'],**r['result'],'wall_seconds':r['wall_seconds'],'log':r['log']}for r in m['runs']if r.get('result')]
  if rows:
   with report.with_suffix('.csv').open('w',newline='')as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
 if a.stage in ['build','all']:
  
  if manifest.exists():raise RuntimeError('Use a fresh cohort; frozen manifest already exists')
  app.mkdir(parents=True,exist_ok=True);(app/'logs').mkdir(exist_ok=True);shutil.copy2(SOURCE/'Makefile',app/'Makefile');required=[libs/n for n in ['libsimx.so','libvortex.so','libvortex-simx.so']]+[build/'sw/kernel/libvortex2.a'];files=list(SOURCE.glob('*.cpp'))+list(SOURCE.glob('*.h'))+[SOURCE/'Makefile',SOURCE/'README.md',SOURCE/'run_matrix.py',ROOT/'sw/common/forza_mmio_abi.h',ROOT/'sw/common/forza_isa_abi.h',ROOT/'sw/common/forza_abi.h',ROOT/'VX_config.toml']+[ROOT/x for x in ['sim/simx/decode.cpp','sim/simx/sfu_unit.cpp','sim/simx/types.h','sim/simx/core.cpp','sim/simx/forza/forza_zone.cpp','sim/simx/forza/forza_zone.h','sim/simx/forza/forza_fabric.cpp','sim/simx/forza/forza_fabric.h','sim/simx/forza/forza_types.h']]
  m={'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'build_dir':str(build),'cohort':a.cohort,'configs':config,'source_artifacts':[dict(fp(x),snapshot=str(app/'source-freeze'/x.relative_to(ROOT))) for x in files],'shared_artifacts':[fp(x)for x in required],'builds':[],'artifacts':[],'runs':[]}
  for item in m['source_artifacts']:
   snapshot=Path(item['snapshot']);snapshot.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(item['path'],snapshot)
  for mode in selected:
   cmd=['make','-C',str(app),'MODE='+mode,'-o',str(libs/'libvortex.so'),'-o',str(build/'sw/kernel/libvortex2.a')];log=app/'logs'/('build-'+mode+'.log')
   with log.open('w')as f:subprocess.run(cmd+['clean'],env=env,stdout=f,stderr=subprocess.STDOUT,check=True);subprocess.run(cmd+['-j2'],env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
   dest=app/'artifacts'/mode;dest.mkdir(parents=True,exist_ok=True)
   for original,final in [('forza_warp_parallel_'+mode,'forza_warp_parallel_'+mode),('kernel.vxbin','kernel.vxbin'),('kernel.elf','kernel.elf'),('kernel.dump','kernel.dump')]:shutil.copy2(app/original,dest/final);m['artifacts'].append(fp(dest/final))
   m['builds'].append({'mode':mode,'commands':[cmd+['clean'],cmd+['-j2']],'log':str(log)});save(m);print('BUILT',mode,flush=True)
 else:m=json.loads(manifest.read_text())
 def verify():
  for item in m['source_artifacts']+m['shared_artifacts']+m['artifacts']:
   if fp(item['path'])!={k:item[k] for k in ['path','bytes','sha256']}:raise RuntimeError('Frozen artifact changed: '+item['path'])
   if item.get('snapshot') and fp(item['snapshot'])['sha256']!=item['sha256']:raise RuntimeError('Frozen source snapshot changed: '+item['snapshot'])
 if a.stage=='build':verify();return
 cases=[] if a.skip_smoke else [('smoke',mode,64,n,pattern)for n,pattern in[(0,'random'),(1,'random'),(7,'random'),(131,'random'),(131,'hot')]for mode in selected]
 if a.stage!='smoke':cases +=[('performance',mode,size,4096,pattern)for size in[64]for pattern in['random','hotset','hot']for mode in selected]
 done={(r['cohort'],r['mode'],r['keys'],r['requests'],r['pattern'],r['seed'])for r in m['runs']}
 for cohort,mode,keys,n,pattern in cases:
  if(cohort,mode,keys,n,pattern,a.seed)in done:continue
  verify();work=app/'runs'/f'{cohort}-{mode}-k{keys}-u{n}-{pattern}-seed{a.seed}-wide';work.mkdir(parents=True,exist_ok=True);log=work/'run.log';binary=app/'artifacts'/mode/('forza_warp_parallel_'+mode);cmd=[str(binary),'-k',str(binary.parent/'kernel.vxbin'),'-n',str(keys),'-u',str(n),'-p',pattern,'-d','wide','-s',str(a.seed),'-t','600'];print('START',cohort,mode,keys,n,pattern,flush=True);start=time.monotonic();timeout=False
  with log.open('w')as f:
   try:rc=subprocess.run(cmd,cwd=work,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=610).returncode
   except subprocess.TimeoutExpired:rc=-1;timeout=True
  lines=log.read_text().splitlines();rs=[fields(s)for s in lines if s.startswith('RESULT ')];ins=[fields(s)for s in lines if s.startswith('INPUT ')];row={'cohort':cohort,'mode':mode,'keys':keys,'requests':n,'pattern':pattern,'delta_distribution':'wide','seed':a.seed,'command':cmd,'cwd':str(work),'log':str(log),'returncode':rc,'timed_out':timeout,'wall_seconds':time.monotonic()-start,'passed':rc==0 and bool(rs)and any(s.startswith('PASSED:')for s in lines),'result':rs[-1]if rs else None,'input':ins[-1]if ins else None};m['runs'].append(row);verify();save(m);print('PASS'if row['passed']else'FAIL',mode,'wall',round(row['wall_seconds'],3),row['result'],flush=True)
  if not row['passed']:raise SystemExit('Run failed: '+str(log))
 m['completed_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();save(m);print('COMPLETE',len(m['runs']),'cases',flush=True)
if __name__=='__main__':main()
