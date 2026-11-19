#!/usr/bin/env python3
"""Repeat the unchanged diagnostic guest with host-only payload recording."""
import hashlib,json,os,re,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
APP=ROOT/'build64-forza-explore/tests/regression/forza_explore/large_state_digest'
REPORT=ROOT/'docs/proposals/forza_explore_large_trace_record.json'
def fp(p):
 p=Path(p).resolve();return {'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size}
def main():
 prior=json.loads((ROOT/'docs/proposals/forza_explore_large_digest.json').read_text())
 for item in prior['artifacts']:
  if fp(item['path'])!=item:raise RuntimeError('Prior captured file changed '+item['path'])
 artifacts=prior['artifacts']+[fp(ROOT/'tests/regression/forza_explore/large_state_digest/main_record.cpp'),fp(Path(__file__).resolve()),fp(APP/'forza_large_msg_digest_record')]
 work=APP/'runs/host-record-n65536-permute';work.mkdir(parents=True,exist_ok=True);log=work/'run.log'
 cmd=[str(APP/'forza_large_msg_digest_record'),'-k',str(APP/'kernel-msg_digest.vxbin'),'-b','262144','-n','65536','-p','permute','-s','17','-t','600']
 env=os.environ.copy();env.pop('DEBUG',None);env['VORTEX_DRIVER']='simx';env['LD_LIBRARY_PATH']=str(ROOT/'build64-forza-explore/sw/runtime');env['CONFIGS']=prior['configs']
 print('START unchanged guest/full trace record',flush=True);start=time.monotonic();timeout=False
 with log.open('w') as f:
  try:rc=subprocess.run(cmd,cwd=work,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=610).returncode
  except subprocess.TimeoutExpired:rc=-1;timeout=True
 for item in artifacts:
  if fp(item['path'])!=item:raise RuntimeError('Captured file changed '+item['path'])
 text=log.read_text();rows=[x for x in text.splitlines() if x.startswith(('TRACE_','DIGEST ','state[','FAILED:','PASSED:'))]
 report={'command':cmd,'configs':prior['configs'],'cwd':str(work),'log':str(log),'returncode':rc,'timed_out':timeout,'wall_seconds':time.monotonic()-start,'guest_and_driver_unchanged_from_prior':True,'prior_manifest':str(ROOT/'docs/proposals/forza_explore_large_digest.json'),'artifacts':artifacts,'dump_artifacts':[fp(x) for x in work.glob('*.bin')],'diagnostic_lines':rows}
 REPORT.write_text(json.dumps(report,indent=2)+'\n');print('END record',rc,'wall',round(report['wall_seconds'],3),flush=True)
 for row in rows:print(row,flush=True)
if __name__=='__main__':main()
