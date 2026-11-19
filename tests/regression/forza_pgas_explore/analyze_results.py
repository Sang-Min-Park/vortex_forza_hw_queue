#!/usr/bin/env python3
"""Verify frozen PGAS measurements and expose interface/placement comparisons."""
import collections,csv,datetime,hashlib,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
MANIFEST=ROOT/'docs/proposals/forza_pgas_l1_off.json'
def fp(p):
 p=Path(p).resolve();return {'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
def main():
 m=json.loads(MANIFEST.read_text());checked=m['source_artifacts']+m['shared_artifacts']+m['artifacts']
 for i in checked:
  assert fp(i['path'])==i,i['path']
 datasets=collections.defaultdict(list);logs=[];rows=[]
 for r in m['runs']:
  assert r['passed'] and r['returncode']==0 and not r['timed_out']
  text=Path(r['log']).read_text();checksums=re.findall(r'^PASSED: .*checksum=([0-9a-f]+)$',text,re.M)
  assert len(checksums)==1,r['log'];cmd=r['command'];pattern=cmd[cmd.index('-p')+1]
  x=r['result'];dataset=(x['state_bytes'],x['updates'],pattern)
  datasets[dataset].append({'mode':x['mode'],'input_hash':x['hash'],'state_checksum':checksums[0]})
  logs.append(fp(r['log']));rows.append({'cohort':r['cohort'],'pattern':pattern,**x,'state_checksum':checksums[0],'wall_seconds':r['wall_seconds'],'log':r['log']})
 for key,variants in datasets.items():
  assert len({v['input_hash']for v in variants})==1,key
  assert len({v['state_checksum']for v in variants})==1,key
 performance={}
 for pattern in ('random','skew'):
  perf={x['mode']:x for x in rows if x['cohort']=='performance'and x['pattern']==pattern}
  cycles={k:int(v['device_cycles_post_readback_including_flush'])for k,v in perf.items()}
  best_native=min(('gpu_global','gpu_local'),key=lambda k:cycles[k])
  performance[pattern]={'cycles':cycles,'best_native':best_native,'same_local_state_software_over_csr':cycles['sw_owner']/cycles['hw_owner_csr_local'],'same_local_state_software_over_mmio':cycles['sw_owner']/cycles['hw_owner_mmio_local'],'csr_local_over_best_native':cycles['hw_owner_csr_local']/cycles[best_native],'mmio_local_over_best_native':cycles['hw_owner_mmio_local']/cycles[best_native],'mmio_over_csr_global':cycles['hw_owner_mmio_global']/cycles['hw_owner_csr_global'],'mmio_over_csr_local':cycles['hw_owner_mmio_local']/cycles['hw_owner_csr_local']}
 out={'verified_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'manifest':fp(MANIFEST),'analysis_source':fp(__file__),'all_exact_runs_pass':True,'runs':len(rows),'source_shared_binary_hash_checks':len(checked),'checked_artifacts':checked,'log_artifacts':logs,'datasets':[{'state_bytes':k[0],'updates':k[1],'pattern':k[2],'variants':v}for k,v in datasets.items()],'performance':performance,'scope':'Two cores; 32KiB total resident state versus1MiB shared L2. Resident-capacity experiment is not a >L2working set. Prefix spill explicitly validated, not forced DRAMmisses.'}
 (ROOT/'docs/proposals/forza_pgas_analysis.json').write_text(json.dumps(out,indent=2)+'\n')
 with(ROOT/'docs/proposals/forza_pgas_enriched.csv').open('w',newline='')as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
 print(json.dumps({'runs':len(rows),'captured_hashes':len(checked),'performance':performance},indent=2))
if __name__=='__main__':main()
