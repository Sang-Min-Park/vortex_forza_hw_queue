#!/usr/bin/env python3
"""Verify width-only and ownership-only PGAS comparisons independently."""
import collections,csv,datetime,hashlib,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
MASK=(1<<64)-1
PRIME=0x100000001b3
COHORTS=[('scalar_contiguous','forza_pgas_l1_off.json',1,'contiguous'),('vector_contiguous','forza_pgas_vector_l1_off.json',4,'contiguous'),('vector_striped','forza_pgas_striped_l1_off.json',4,'striped')]
def fp(path):
 p=Path(path).resolve();return {'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
def mix(x):
 x=(x+0x9e3779b97f4a7c15)&MASK;x=((x^(x>>30))*0xbf58476d1ce4e5b9)&MASK;x=((x^(x>>27))*0x94d049bb133111eb)&MASK;return x^(x>>31)
def dataset_key(r):
 c=r['command'];return(int(c[c.index('-b')+1]),int(c[c.index('-n')+1]),c[c.index('-p')+1],int(c[c.index('-s')+1]))
def reference(key):
 b,n,pattern,seed=key;words=b//8;state=[0]*words;value_hash=0xcbf29ce484222325;counts={p:{'updates':[0]*16,'incoming':[0]*16,'direct':0}for p in ('contiguous','striped')}
 assert pattern in ('random','skew')
 for i in range(n):
  h=mix(i^mix(seed));k=h&(words-1)
  if pattern=='skew'and h%10:k=mix(h)&(max(1,words//128)-1)
  delta=(h>>48)%15-7
  if not delta:delta=1
  state[k]+=delta;value_hash=((value_hash^k)*PRIME)&MASK;value_hash=((value_hash^(delta&MASK))*PRIME)&MASK
  for policy,c in counts.items():
   owner=k//(words//16)if policy=='contiguous'else k&15;c['updates'][owner]+=1
   if owner==i%16:c['direct']+=1
   else:c['incoming'][owner]+=1
 checksum=0xcbf29ce484222325
 for v in state:checksum=((checksum^(v&MASK))*PRIME)&MASK
 return {'input_hash':f'{value_hash:016x}','state_checksum':f'{checksum:016x}','owner_counts':counts}
def main():
 manifests={};rows=[];files={};logs=[];refs={};runmaps={}
 for cohort,filename,width,policy in COHORTS:
  path=ROOT/'docs/proposals'/filename;m=json.loads(path.read_text());manifests[cohort]={'artifact':fp(path),'configs':m['configs'],'started_utc':m['started_utc'],'completed_utc':m['completed_utc']};runmaps[cohort]={}
  for item in m['source_artifacts']+m['shared_artifacts']+m['artifacts']:
   assert fp(item['path'])==item,item['path']
   if item['path']in files:assert files[item['path']]==item
   files[item['path']]=item
  for r in m['runs']:
   assert r['passed']and r['returncode']==0 and not r['timed_out']
   key=dataset_key(r)
   if key not in refs:refs[key]=reference(key)
   expected=refs[key];text=Path(r['log']).read_text();checks=re.findall(r'^PASSED: .*checksum=([0-9a-f]+)$',text,re.M);assert checks==[expected['state_checksum']],r['log'];assert r['result']['hash']==expected['input_hash']
   logs.append(fp(r['log']));row={'architecture_cohort':cohort,'owner_mapping':policy,'mmio_command_lanes_per_cycle':width,'cohort':r['cohort'],'pattern':key[2],**r['result'],'state_checksum':checks[0],'wall_seconds':r['wall_seconds'],'log':r['log']};rows.append(row);runmaps[cohort][(r['cohort'],r['mode'],key)]=row
 width_controls=[]
 for k,v in runmaps['vector_contiguous'].items():
  old=runmaps['scalar_contiguous'][k]
  if not v['mode'].startswith('hw_owner_mmio'):
   assert v['device_cycles_post_readback_including_flush']==old['device_cycles_post_readback_including_flush']
   width_controls.append({'mode':v['mode'],'case':k[2],'cycles':v['device_cycles_post_readback_including_flush'],'identical':True})
 mapping_controls=[]
 for k,v in runmaps['vector_striped'].items():
  old=runmaps['vector_contiguous'][k]
  if v['mode']=='gpu_global':
   assert v['device_cycles_post_readback_including_flush']==old['device_cycles_post_readback_including_flush']
   mapping_controls.append({'case':k[2],'cycles':v['device_cycles_post_readback_including_flush'],'identical':True})
 vector=json.loads((ROOT/'docs/proposals/forza_pgas_vector_l1_off.json').read_text());striped=json.loads((ROOT/'docs/proposals/forza_pgas_striped_l1_off.json').read_text())
 assert all(x['scalar_binary_comparison']['guest_binary_identical']for x in vector['builds'])
 assert next(x for x in striped['builds']if x['label']=='gpu_global')['contiguous_binary_comparison']['guest_binary_identical']
 performance={}
 for pattern in ('random','skew'):
  key=(32768,4096,pattern,17);group={c:{r['mode']:int(r['device_cycles_post_readback_including_flush'])for r in rows if r['architecture_cohort']==c and r['cohort']=='performance'and r['pattern']==pattern}for c,_,_,_ in COHORTS};s=group['vector_striped'];v=group['vector_contiguous'];base=group['scalar_contiguous']
  performance[pattern]={'cycles':group,'owner_counts':refs[key]['owner_counts'],'mapping_speedup_csr_local':v['hw_owner_csr_local']/s['hw_owner_csr_local'],'mapping_speedup_mmio_local':v['hw_owner_mmio_local']/s['hw_owner_mmio_local'],'mapping_speedup_software_local':v['sw_owner']/s['sw_owner'],'striped_csr_local_over_gpu_global':s['hw_owner_csr_local']/s['gpu_global'],'striped_mmio_local_over_gpu_global':s['hw_owner_mmio_local']/s['gpu_global'],'striped_csr_local_over_gpu_local':s['hw_owner_csr_local']/s['gpu_local'],'striped_mmio_local_over_gpu_local':s['hw_owner_mmio_local']/s['gpu_local'],'striped_sw_over_csr_local':s['sw_owner']/s['hw_owner_csr_local'],'striped_sw_over_mmio_local':s['sw_owner']/s['hw_owner_mmio_local'],'width_mmio_global_scalar_over_vector':base['hw_owner_mmio_global']/v['hw_owner_mmio_global'],'width_mmio_local_scalar_over_vector':base['hw_owner_mmio_local']/v['hw_owner_mmio_local']}
 out={'verified_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'analysis_source':fp(__file__),'cohort_manifests':manifests,'all_runs_pass':True,'run_count':len(rows),'cohort_run_counts':dict(collections.Counter(r['architecture_cohort']for r in rows)),'unique_frozen_artifacts':list(files.values()),'log_artifacts':logs,'independent_reference':{str(k):v for k,v in refs.items()},'width_control_exact_cycle_checks':width_controls,'mapping_native_global_exact_cycle_checks':mapping_controls,'all7_width_comparison_guest_binaries_identical':True,'mapping_native_global_guest_binary_identical':True,'performance':performance}
 (ROOT/'docs/proposals/forza_pgas_cohort_analysis.json').write_text(json.dumps(out,indent=2)+'\n')
 columns=[]
 for r in rows:
  for k in r:
   if k not in columns:columns.append(k)
 with(ROOT/'docs/proposals/forza_pgas_cohorts.csv').open('w',newline='')as f:
  w=csv.DictWriter(f,fieldnames=columns);w.writeheader();w.writerows(rows)
 print(json.dumps({'runs':len(rows),'unique_artifacts':len(files),'width_controls':len(width_controls),'mapping_controls':len(mapping_controls),'performance':performance},indent=2))
if __name__=='__main__':main()
