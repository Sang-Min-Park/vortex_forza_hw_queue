#!/usr/bin/env python3
"""Matched L1-ON capacity controls for the frozen typed fast backend."""
import csv,datetime,hashlib,json,os,re,shutil,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
BUILD=ROOT/'build64-forza-explore-l1'
BASE=BUILD/'tests/regression/forza_explore'
APP=BASE/'large_state_fast'
REPORT=ROOT/'docs/proposals/forza_explore_large_selected_l1_on'
def fp(path):
 p=Path(path).resolve();return {'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
def fields(line):return dict(re.findall(r'(\w+)=([^\s]+)',line))
def main():
 snap=APP/'artifacts/msg_raw_fast_compact';snap.mkdir(parents=True,exist_ok=True)
 for original,final in [('forza_large_msg_raw_fast_compact','forza_large_msg_raw_fast_compact'),('kernel-msg_raw_fast_compact.vxbin','kernel.vxbin'),('kernel.elf','kernel.elf'),('kernel.dump','kernel.dump')]:shutil.copy2(APP/original,snap/final)
 original=json.loads((BASE/'large_state/large-state-manifest.json').read_text())
 source=ROOT/'tests/regression/forza_explore/large_state_fast'
 artifacts=[source/n for n in ('main.cpp','kernel.cpp','common.h','Makefile')]
 artifacts += [Path(__file__).resolve()]+list(snap.iterdir())
 captures=[fp(x) for x in artifacts]+original['shared_artifacts']+[a for a in original['artifacts'] if '/gpu_compact/' in a['path']]
 report={'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'configs':original['configs'],
         'selection':'typed compact backend selected after actual L1-OFF exact-state passes; GPU uses matching compact8B inputs',
         'units':'distinct_state_lines counts64Bsectors;L2line128B/sector64B,capacity1MiB',
         'artifacts':captures,'build_log':'/tmp/forza-large-fast-on-build.log','runs':[]}
 env=os.environ.copy();env.pop('DEBUG',None);env['CONFIGS']=original['configs'];env['VORTEX_DRIVER']='simx';env['LD_LIBRARY_PATH']=str(BUILD/'sw/runtime')
 def verify():
  for item in captures:
   if fp(item['path'])!=item:raise RuntimeError('Changed captured artifact '+item['path'])
 def save():
  REPORT.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
  rows=[{'cache_policy':'l1_on','cohort':r['cohort'],**r['result'],'wall_seconds':r['wall_seconds'],'log':r['log']} for r in report['runs'] if r.get('result')]
  if rows:
   with REPORT.with_suffix('.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
 for mode in ('gpu_compact','msg_raw_fast_compact'):
  folder=BASE/'large_state/artifacts/gpu_compact' if mode=='gpu_compact' else snap
  for cohort,size,count,pattern in [('smoke',262144,64,'random'),('capacity',262144,65536,'permute'),('capacity',2097152,65536,'permute')]:
   verify();work=APP/'selected_runs'/f'{cohort}-{mode}-b{size}-n{count}';work.mkdir(parents=True,exist_ok=True);log=work/'run.log'
   cmd=[str(folder/('forza_large_'+mode)),'-k',str(folder/'kernel.vxbin'),'-b',str(size),'-n',str(count),'-p',pattern,'-s','17','-t','600']
   print('START L1ON',mode,size,count,flush=True);start=time.monotonic();timeout=False
   with log.open('w') as f:
    try:rc=subprocess.run(cmd,cwd=work,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=610).returncode
    except subprocess.TimeoutExpired:rc=-1;timeout=True
   lines=log.read_text().splitlines();rs=[fields(x) for x in lines if x.startswith('RESULT ')];ins=[fields(x) for x in lines if x.startswith('INPUT ')]
   row={'mode':mode,'cohort':cohort,'state_bytes':size,'updates':count,'command':cmd,'cwd':str(work),'log':str(log),'wall_seconds':time.monotonic()-start,'returncode':rc,'timed_out':timeout,'passed':rc==0 and bool(rs) and any(x.startswith('PASSED:') for x in lines),'result':rs[-1] if rs else None,'input':ins[-1] if ins else None}
   report['runs'].append(row);verify();save();print('PASS' if row['passed'] else 'FAIL','L1ON',mode,size,'wall',round(row['wall_seconds'],3),row['result'],flush=True)
   if not row['passed']:raise SystemExit('Selected matched run failed; inspect '+str(log))
 report['completed_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();verify();save();print('COMPLETE matched L1ON controls',flush=True)
if __name__=='__main__':main()
