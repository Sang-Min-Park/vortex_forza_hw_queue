#!/usr/bin/env python3
"""Repeat the frozen PGAS benchmark with four independent MMIO command ports."""
import argparse,csv,datetime,hashlib,json,os,re,shutil,subprocess,time
from pathlib import Path
SOURCE=Path(__file__).resolve().parent
ROOT=SOURCE.parents[2]
MODES=[('gpu_global','csr','global'),('gpu_local','csr','local'),('hw_owner','csr','global'),('hw_owner','csr','local'),('hw_owner','mmio','global'),('hw_owner','mmio','local'),('sw_owner','csr','local')]
def label(mode,transport,state):return f'{mode}_{transport}_{state}'if mode=='hw_owner'else mode
def fp(p):
 p=Path(p).resolve();return {'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
def fields(line):return dict(re.findall(r'(\w+)=([^\s]+)',line))
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--stage',choices=('build','smoke','run','all'),default='all');p.add_argument('--cache-policy',choices=('l1_off','l1_on'),default='l1_off');a=p.parse_args()
 build=a.build_dir.resolve();app=build/'tests/regression/forza_pgas_explore';libs=build/'sw/runtime';manifest_path=app/'pgas-vector-manifest.json';report=ROOT/'docs/proposals'/('forza_pgas_vector_'+a.cache_policy)
 config='-DVX_CFG_FORZA_MMIO_VECTOR_ENABLE -DVX_CFG_FORZA_MMIO_ENABLE -DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE -DVX_CFG_L2_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4'
 if a.cache_policy=='l1_off':config+=' -DVX_CFG_DCACHE_DISABLE'
 env=os.environ.copy();env.pop('DEBUG',None);env['CONFIGS']=config;env['VORTEX_DRIVER']='simx';env['LD_LIBRARY_PATH']=str(libs)
 baseline_path=ROOT/'docs/proposals'/('forza_pgas_'+a.cache_policy+'.json')
 baseline=json.loads(baseline_path.read_text())
 def verify_scalar():
  for item in baseline['source_artifacts']+baseline['shared_artifacts']+baseline['artifacts']:
   if fp(item['path'])!=item:raise RuntimeError('Frozen scalar artifact changed '+item['path'])
 verify_scalar()
 def case_key(r):
  cmd=r['command'];return(r['cohort'],r['mode'],cmd[cmd.index('-b')+1],cmd[cmd.index('-n')+1],cmd[cmd.index('-p')+1])
 scalar_runs={case_key(r):r for r in baseline['runs']}
 def save(m):
  manifest_path.write_text(json.dumps(m,indent=2)+'\n');report.with_suffix('.json').write_text(json.dumps(m,indent=2)+'\n');rows=[{'cohort':r['cohort'],**r['result'],'wall_seconds':r['wall_seconds'],'log':r['log']}for r in m['runs']if r.get('result')]
  if rows:
   with report.with_suffix('.csv').open('w',newline='')as f:
    w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
 if a.stage in ('build','all'):
  app.mkdir(parents=True,exist_ok=True);(app/'logs').mkdir(exist_ok=True);shutil.copy2(SOURCE/'Makefile',app/'Makefile')
  required=[libs/n for n in ('libsimx.so','libvortex.so','libvortex-simx.so')]+[build/'sw/kernel/libvortex2.a']
  for path in required:
   if not path.exists():p.error('Shared prerequisite not ready: '+str(path))
  runtime=ROOT/'tests/regression/forza_explore/runtime_ordered'
  files=list(SOURCE.glob('*.cpp'))+list(SOURCE.glob('*.h'))+[SOURCE/'Makefile',SOURCE/'README.md',SOURCE/'run_matrix.py',Path(__file__).resolve(),runtime/'vortex.mk']+list((runtime/'inc').glob('*.h'))
  files +=[ROOT/'sw/common/forza_mmio_abi.h',ROOT/'sw/common/forza_abi.h']
  m={'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'build_dir':str(build),'configs':config,'cache_policy':a.cache_policy,'scalar_baseline':fp(baseline_path),'scalar_config':baseline['configs'],'service_width':{'scalar_mmio_command_lanes_per_cycle':1,'vector_mmio_command_lanes_per_cycle':4,'csr_active_lanes_per_cycle':4,'payload_bits_per_lane':64,'changed_resource':'four independent MMIO command lanes and full-mask response','unchanged_resources':'NoC/cache/local-SRAM capacities and per-actor ownership/input algorithm'},'source_artifacts':[fp(x)for x in files],'shared_artifacts':[fp(x)for x in required],'builds':[],'artifacts':[],'runs':[]}
  for mode,transport,state in MODES:
   name=label(mode,transport,state);cmd=['make','-C',str(app),'MODE='+mode,'TRANSPORT='+transport,'STATE='+state,'-o',str(libs/'libvortex.so'),'-o',str(build/'sw/kernel/libvortex2.a')];log=app/'logs'/('build-'+name+'.log');build_started=datetime.datetime.now(datetime.timezone.utc).isoformat()
   with log.open('w')as f:subprocess.run(cmd+['clean'],env=env,stdout=f,stderr=subprocess.STDOUT,check=True);subprocess.run(cmd+['-j2'],env=env,stdout=f,stderr=subprocess.STDOUT,check=True)
   dest=app/'artifacts'/name;dest.mkdir(parents=True,exist_ok=True)
   for original,final in [('forza_pgas_'+name,'forza_pgas_'+name),('kernel.vxbin','kernel.vxbin'),('kernel.elf','kernel.elf'),('kernel.dump','kernel.dump')]:shutil.copy2(app/original,dest/final);m['artifacts'].append(fp(dest/final))
   old_guest=next(x for x in baseline['artifacts']if '/artifacts/'+name+'/kernel.vxbin' in x['path'])
   old_host=next(x for x in baseline['artifacts']if x['path'].endswith('/artifacts/'+name+'/forza_pgas_'+name))
   comparison={'guest_binary_identical':fp(dest/'kernel.vxbin')['sha256']==old_guest['sha256'],'host_binary_identical':fp(dest/('forza_pgas_'+name))['sha256']==old_host['sha256'],'scalar_guest':old_guest,'scalar_host':old_host}
   m['builds'].append({'label':name,'commands':[cmd+['clean'],cmd+['-j2']],'log':str(log),'started_utc':build_started,'completed_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scalar_binary_comparison':comparison});save(m);print('BUILT',name,'guest_identical',comparison['guest_binary_identical'],flush=True)
 else:m=json.loads(manifest_path.read_text())
 def verify():
  for item in m['source_artifacts']+m['shared_artifacts']+m['artifacts']:
   if fp(item['path'])!=item:raise RuntimeError('Captured artifact changed '+item['path'])
  verify_scalar()
 if a.stage=='build':verify();return
 names=[label(*v)for v in MODES]
 cases=[('smoke',name,32768,count,'random')for count in (0,1,128)for name in names]
 cases +=[('spill_check',name,131072,256,'random')for name in names]
 if a.stage!='smoke':cases +=[('performance',name,32768,4096,pattern)for pattern in ('random','skew')for name in names]
 for cohort,name,size,count,pattern in cases:
  verify();work=app/'runs'/f'{cohort}-{name}-b{size}-n{count}-{pattern}';work.mkdir(parents=True,exist_ok=True);log=work/'run.log';binary=app/'artifacts'/name/('forza_pgas_'+name);cmd=[str(binary),'-k',str(binary.parent/'kernel.vxbin'),'-b',str(size),'-n',str(count),'-p',pattern,'-s','17','-t','600'];print('START',cohort,name,size,count,pattern,flush=True);start=time.monotonic();started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat();timeout=False
  with log.open('w')as f:
   try:rc=subprocess.run(cmd,cwd=work,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=610).returncode
   except subprocess.TimeoutExpired:rc=-1;timeout=True
  lines=log.read_text().splitlines();rs=[fields(x)for x in lines if x.startswith('RESULT ')];ins=[fields(x)for x in lines if x.startswith('INPUT ')];memory=[fields(x)for x in lines if x.startswith('MEMORY ')]
  if memory:
   if rs:rs[-1].update(memory[-1])
   if ins:ins[-1].update(memory[-1])
  row={'cohort':cohort,'mode':name,'command':cmd,'cwd':str(work),'log':str(log),'started_utc':started_utc,'completed_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'returncode':rc,'timed_out':timeout,'wall_seconds':time.monotonic()-start,'passed':rc==0 and bool(rs)and any(x.startswith('PASSED:')for x in lines),'result':rs[-1]if rs else None,'input':ins[-1]if ins else None}
  scalar=scalar_runs[case_key(row)];scalar_log=Path(scalar['log']).read_text();checksum=re.findall(r'^PASSED: .*checksum=([0-9a-f]+)$','\n'.join(lines),re.M);old_checksum=re.findall(r'^PASSED: .*checksum=([0-9a-f]+)$',scalar_log,re.M)
  row['scalar_comparison']={'scalar_run_log':scalar['log'],'input_hash_identical':bool(rs)and rs[-1]['hash']==scalar['result']['hash'],'state_checksum_identical':bool(checksum)and checksum==old_checksum,'scalar_cycles':scalar['result']['device_cycles_post_readback_including_flush'],'unaffected_control':not name.startswith('hw_owner_mmio')}
  if rs:row['scalar_comparison']['cycle_delta']=int(rs[-1]['device_cycles_post_readback_including_flush'])-int(row['scalar_comparison']['scalar_cycles']);row['scalar_comparison']['cycles_identical']=row['scalar_comparison']['cycle_delta']==0
  m['runs'].append(row);verify();save(m);print('PASS'if row['passed']else'FAIL',name,'wall',round(row['wall_seconds'],3),row['result'],'scalar_delta',row['scalar_comparison'].get('cycle_delta'),flush=True)
  if not row['passed']:raise SystemExit('Run failed: '+str(log))
 m['completed_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();save(m);print('COMPLETE PGAS',len(m['runs']),'exact validated runs',flush=True)
if __name__=='__main__':main()
