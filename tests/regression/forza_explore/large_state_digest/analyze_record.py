#!/usr/bin/env python3
"""Independently reconstruct received payload multisets and their state updates."""
import collections,hashlib,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
REPORT=ROOT/'docs/proposals/forza_explore_large_trace_record.json'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
 meta=json.loads(REPORT.read_text());work=Path(meta['cwd'])
 req=list(struct.iter_unpack('<Qq',(work/'requests.bin').read_bytes()))
 trace=list(struct.iter_unpack('<Qq',(work/'fulltrace.bin').read_bytes()))
 results=list(struct.iter_unpack('<14Q',(work/'results.bin').read_bytes()))
 actual=[x[0]for x in struct.iter_unpack('<q',(work/'actualstate.bin').read_bytes())]
 expected=[x[0]for x in struct.iter_unpack('<q',(work/'expectedstate.bin').read_bytes())]
 actors=len(results);stride=len(trace)//actors;words=len(actual);part=words//actors
 want=[collections.Counter()for _ in range(actors)];fold=[0]*words
 for sender in range(actors):
  cache=[None,None];recent=0
  def emit(key,delta):
   owner=key//part
   if owner==sender:fold[key]+=delta
   else:want[owner][(key,delta)]+=1
  for key,delta in req[sender::actors]:
   if cache[0]is not None and cache[0][0]==key:cache[0][1]+=delta;recent=0
   elif cache[1]is not None and cache[1][0]==key:cache[1][1]+=delta;recent=1
   elif cache[0]is None:cache[0]=[key,delta];recent=0
   elif cache[1]is None:cache[1]=[key,delta];recent=1
   else:
    victim=1-recent;emit(*cache[victim]);cache[victim]=[key,delta];recent=victim
  for item in cache:
   if item is not None:emit(*item)
 differences=[]
 for owner in range(actors):
  rows=trace[owner*stride:owner*stride+results[owner][0]];got=collections.Counter(rows)
  assert len(rows)==sum(want[owner].values())
  for key,delta in rows:
   assert key<words and key//part==owner
   fold[key]+=delta
  missing=want[owner]-got;excess=got-want[owner]
  if missing or excess:differences.append({'owner':owner,'missing':[{'key':k,'delta':v,'count':n}for(k,v),n in sorted(missing.items())],'excess':[{'key':k,'delta':v,'count':n}for(k,v),n in sorted(excess.items())]})
 mismatches=[{'key':k,'trace_fold':f,'actual':a,'expected':e}for k,(f,a,e)in enumerate(zip(fold,actual,expected))if f!=e or f!=a]
 excerpt=[]
 for owner,index in ((11,119),(15,2914),(15,3152)):
  excerpt.append({'owner':owner,'center_index':index,'entries':[{'index':j,'key':trace[owner*stride+j][0],'delta':trace[owner*stride+j][1]}for j in range(index-2,index+4)]})
 out={'record_manifest':str(REPORT),'record_manifest_sha256':sha(REPORT),'analysis_source':str(Path(__file__).resolve()),'analysis_source_sha256':sha(__file__),
      'dumps':meta['dump_artifacts'],'actors':actors,'stride':stride,'state_words':words,
      'fold_matches_actual_for_every_word':fold==actual,'fold_matches_expected':fold==expected,
      'state_mismatches':mismatches,'payload_multiset_differences':differences,'two_slot_reuse_excerpts':excerpt,
      'inference':'Received values explain all state errors; same-parity two-entry duplicates/mixed payload match premature slot reuse, to be confirmed by ordering regression.'}
 dest=ROOT/'docs/proposals/forza_explore_large_trace_analysis.json';dest.write_text(json.dumps(out,indent=2)+'\n')
 print(json.dumps({k:out[k]for k in ('fold_matches_actual_for_every_word','fold_matches_expected','state_mismatches','payload_multiset_differences')},indent=2))
if __name__=='__main__':main()
