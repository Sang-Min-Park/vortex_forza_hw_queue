#!/usr/bin/env python3
"""Resume frozen large-state artifacts in a bounded, prioritized comparison."""
import argparse, csv, datetime, hashlib, json, os, re, signal, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
REPORT = ROOT / 'docs/proposals/forza_explore_large_priority'
def fp(path):
    p=Path(path).resolve()
    return {'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
def parse(line): return dict(re.findall(r'(\w+)=([^\s]+)',line))
def read_result(log):
    lines=Path(log).read_text().splitlines()
    r=[x for x in lines if x.startswith('RESULT ')]
    i=[x for x in lines if x.startswith('INPUT ')]
    return (parse(r[-1]) if r else None, parse(i[-1]) if i else None,
            any(x.startswith('PASSED:') for x in lines))
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--wait-pid',type=int)
    p.add_argument('--stopped-runner-pid',type=int)
    a=p.parse_args()
    bundles={}
    for policy,suffix in [('l1_off',''),('l1_on','-l1')]:
      for active in (False,True):
        app=ROOT/('build64-forza-explore'+suffix)/'tests/regression/forza_explore'/('large_state_active' if active else 'large_state')
        bundles[(policy,active)]=(app,json.loads((app/'large-state-manifest.json').read_text()))
    out={'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
         'runner':fp(__file__),'completion':'known_one_shot_counts',
         'unit_correction':'distinct_state_lines is 64B sectors; L2 lines are 128B, sectors 64B; L2 capacity is 1MiB',
         'bundles':[{ 'policy':k[0],'active':k[1],'manifest':str(v[0]/'large-state-manifest.json'),
                      'manifest_sha256':fp(v[0]/'large-state-manifest.json')['sha256']} for k,v in bundles.items()],
         'runs':[], 'unmeasured':'Original exhaustive matrix reprioritized; unlisted cases were not measured and imply no result.'}
    def save():
        REPORT.with_suffix('.json').write_text(json.dumps(out,indent=2)+'\n')
        rows=[]
        for r in out['runs']:
            if r.get('result'): rows.append({'cache_policy':r['cache_policy'],'cohort':r['cohort'],**r['result'],'wall_seconds':r['wall_seconds'],'passed':r['passed'],'log':r['log']})
        if rows:
            with REPORT.with_suffix('.csv').open('w',newline='') as f:
                w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    def verify(policy,active):
        app,m=bundles[(policy,active)]
        for item in m['source_artifacts']+m['shared_artifacts']+m['artifacts']:
            if fp(item['path'])!=item: raise RuntimeError('Changed captured artifact '+item['path'])
        return app,m
    def run(policy,label,size,count=65536,pattern='permute',cohort='capacity'):
        active=label.endswith('_active');app,m=verify(policy,active)
        work=app/'priority_runs'/f'{cohort}-{label}-b{size}-n{count}-{pattern}';work.mkdir(parents=True,exist_ok=True)
        log=work/'run.log';binary=app/'artifacts'/label/('forza_large_'+label)
        cmd=[str(binary),'-k',str(binary.parent/'kernel.vxbin'),'-b',str(size),'-n',str(count),'-p',pattern,'-s','17','-t','600']
        env=os.environ.copy();env.pop('DEBUG',None);env['CONFIGS']=m['configs'];env['VORTEX_DRIVER']='simx';env['LD_LIBRARY_PATH']=str(app.parents[3]/'sw/runtime')
        print('START',policy,label,size,count,flush=True);start=time.monotonic();timeout=False
        with log.open('w') as f:
            try: rc=subprocess.run(cmd,cwd=work,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=610).returncode
            except subprocess.TimeoutExpired: rc=-1;timeout=True
        result,inputs,good=read_result(log)
        row={'cache_policy':policy,'cohort':cohort,'label':label,'state_bytes':size,'updates':count,
             'command':cmd,'cwd':str(work),'log':str(log),'wall_seconds':time.monotonic()-start,
             'returncode':rc,'timed_out':timeout,'passed':rc==0 and good and result is not None,'result':result,'input':inputs}
        out['runs'].append(row);verify(policy,active);save()
        print('PASS' if row['passed'] else 'FAIL',policy,label,size,'wall='+str(round(row['wall_seconds'],3)),result,flush=True)
        return row
    if a.wait_pid:
        print('WAIT existing capacity kernel',a.wait_pid,flush=True);start=time.monotonic()
        while True:
            try: state=Path(f'/proc/{a.wait_pid}/stat').read_text().split()[2]
            except FileNotFoundError: break
            if state=='Z':break
            time.sleep(2)
        if a.stopped_runner_pid:
            os.kill(a.stopped_runner_pid,signal.SIGTERM);os.kill(a.stopped_runner_pid,signal.SIGCONT)
        app,_=bundles[('l1_off',False)];log=app/'runs/capacity-msg-b262144-n65536-permute/run.log'
        result,inputs,passed=read_result(log)
        out['runs'].append({'cache_policy':'l1_off','cohort':'initial_capacity','label':'msg','state_bytes':262144,
             'updates':65536,'log':str(log),'wall_seconds_after_observer_start':time.monotonic()-start,'wall_seconds':'',
             'passed':passed and result is not None,'result':result,'input':inputs})
        save();print('INITIAL PASS' if passed else 'INITIAL FAILED',result,flush=True)
    # Native controls first; retain both register-combined and direct paths.
    gpu=[]
    for size in (262144,2097152):
      for label in ('gpu','gpu_cache','gpu_compact'):gpu.append(run('l1_off',label,size))
    message=[]
    for label in ('msg_raw_compact','msg_batch_compact','msg_raw_compact_active','msg_batch_compact_active'):
        if label.endswith('_active'):
            smoke=run('l1_off',label,262144,64,'random','smoke')
            if not smoke['passed']:continue
        message.append(run('l1_off',label,2097152))
    valid=[r for r in message if r['passed']]
    if not valid:raise SystemExit('No successful remote owner control')
    best_message=min(valid,key=lambda r:int(r['result']['device_cycles_post_readback_including_flush']))['label']
    run('l1_off',best_message,262144)
    valid_gpu=[r for r in gpu if r['passed'] and r['state_bytes']==2097152]
    best_gpu=min(valid_gpu,key=lambda r:int(r['result']['device_cycles_post_readback_including_flush']))['label']
    out['selection']={'basis':'minimum completed L1-OFF 2MiB whole-device cycles among measured candidates',
                      'message':best_message,'gpu':best_gpu};save()
    # Matched L1-ON followup uses the selected implementations, with small smokes.
    for label in (best_gpu,best_message):
        smoke=run('l1_on',label,262144,64,'random','smoke')
        if not smoke['passed']:continue
        for size in (262144,2097152):run('l1_on',label,size)
    # One 16B full/short comparison; these optional controls may hit the explicit deadline.
    for label in ('msg_plain','msg_full'):run('l1_off',label,2097152)
    out['completed_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();save()
    print('COMPLETE prioritized matrix',len(out['runs']),flush=True)
if __name__=='__main__':main()
