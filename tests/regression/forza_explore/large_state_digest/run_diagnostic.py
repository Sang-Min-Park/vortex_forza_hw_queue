#!/usr/bin/env python3
"""Run the isolated incoming trace diagnostic after the performance queue is idle."""
import hashlib,json,os,re,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
SOURCE=Path(__file__).resolve().parent
APP=ROOT/'build64-forza-explore/tests/regression/forza_explore/large_state_digest'
REPORT=ROOT/'docs/proposals/forza_explore_large_digest.json'
def fp(p):
 p=Path(p).resolve();return {'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size}
def main():
 env=os.environ.copy();env.pop('DEBUG',None);env['VORTEX_DRIVER']='simx';env['LD_LIBRARY_PATH']=str(ROOT/'build64-forza-explore/sw/runtime')
 config='-DVX_CFG_EXT_FORZA_ENABLE -DVX_CFG_EXT_A_ENABLE -DVX_CFG_L2_ENABLE -DVX_CFG_NUM_CORES=2 -DVX_CFG_NUM_WARPS=2 -DVX_CFG_NUM_THREADS=4 -DVX_CFG_DCACHE_DISABLE'
 env['CONFIGS']=config
 artifacts=list(SOURCE.glob('*.cpp'))+list(SOURCE.glob('*.h'))+[SOURCE/'Makefile',SOURCE/'README.md',Path(__file__).resolve()]
 artifacts += [APP/'kernel.elf',APP/'kernel.dump',APP/'kernel-msg_digest.vxbin',APP/'forza_large_msg_digest']
 artifacts += list((ROOT/'tests/regression/forza_explore/runtime/inc').glob('*.h'))
 artifacts += [ROOT/'build64-forza-explore/sw/runtime'/x for x in ('libvortex.so','libvortex-simx.so','libsimx.so')]
 report={'configs':config,'artifacts':[fp(x) for x in artifacts],'original_frozen_manifest':str(ROOT/'build64-forza-explore/tests/regression/forza_explore/large_state/large-state-manifest.json'),'runs':[]}
 for count,pattern in ((64,'random'),(65536,'permute')):
  work=APP/'runs'/f'n{count}-{pattern}';work.mkdir(parents=True,exist_ok=True);log=work/'run.log'
  cmd=[str(APP/'forza_large_msg_digest'),'-k',str(APP/'kernel-msg_digest.vxbin'),'-b','262144','-n',str(count),'-p',pattern,'-s','17','-t','600']
  print('START digest',count,pattern,flush=True);start=time.monotonic();timeout=False
  with log.open('w') as f:
   try:rc=subprocess.run(cmd,cwd=work,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=610).returncode
   except subprocess.TimeoutExpired:rc=-1;timeout=True
  text=log.read_text();rows=[dict(re.findall(r'(\w+)=([^\s]+)',line)) for line in text.splitlines() if line.startswith('DIGEST ')]
  report['runs'].append({'command':cmd,'log':str(log),'wall_seconds':time.monotonic()-start,'returncode':rc,'timed_out':timeout,'payload_digest_rows':rows,'all_state_pass':'PASSED:' in text,'state_error_lines':[x for x in text.splitlines() if x.startswith('state[')]})
  for item in report['artifacts']:
   if fp(item['path'])!=item:raise RuntimeError('Artifact changed '+item['path'])
  REPORT.write_text(json.dumps(report,indent=2)+'\n')
  print('END digest',count,'rc',rc,'payload_bad',sum(r['status']!='PASS' for r in rows),'all_state_pass','PASSED:' in text,flush=True)
  if count==64 and rc:raise SystemExit('Diagnostic smoke failed')
if __name__=='__main__':main()
