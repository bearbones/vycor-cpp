#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ABI report/worker/freshness end-to-end regression."""
import argparse, json, pathlib, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--binary',required=True);a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='vycor_abi_gate_') as t:
 root=pathlib.Path(t)
 for f in ['a.cpp','b.cpp']:(root/f).write_text('#include "packet.h"\n')
 header='#ifdef PACKED\n#pragma pack(push, 1)\n#endif\nstruct Packet { char tag; long payload; };\n#ifdef PACKED\n#pragma pack(pop)\n#endif\n'
 (root/'packet.h').write_text(header)
 def db(packed=True):
  rows=[]
  for i,f in enumerate(['a.cpp','b.cpp']):rows.append(dict(directory=t,file=str(root/f),arguments=['clang++','-std=c++17',*(['-DPACKED'] if packed and i==0 else []),'-c',str(root/f)]))
  (root/'compile_commands.json').write_text(json.dumps(rows))
 def run(extra=(),code=1,fmt='json'):
  cmd=[a.binary,'check','--build-path',t,'--project-root',t,'--source',str(root/'a.cpp'),'--source',str(root/'b.cpp'),'--checks=-all,abi-layout-divergence','--threads=1','--format='+fmt,*extra]
  r=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
  assert r.returncode==code,(r.returncode,r.stderr,r.stdout)
  data=json.loads(r.stdout)
  if fmt=='json':assert data['summary']['failed']==0,data
  return data
 db();original=run();assert len(original['findings'])==1,original
 assert original['findings'][0]['severity']=='error'
 for extra in [('--threads=2',),('--checkpoint',str(root/'journal')),('--isolate-workers','--workers=2'),('--isolate-workers','--workers=2','--checkpoint',str(root/'journal'))]:assert run(extra)==original,extra
 assert run(fmt='sarif')['runs'][0]['results'][0]['ruleId']=='abi-layout-divergence'
 run(('--write-baseline',str(root/'baseline')),code=0)
 assert not run(('--baseline',str(root/'baseline')),code=0)['findings']
 db(False);assert not run(('--checkpoint',str(root/'journal')),code=0)['findings']
 db();(root/'packet.h').write_text('struct Packet { int value; };\n')
 assert not run(('--checkpoint',str(root/'journal')),code=0)['findings']
 print('PASS: ABI JSON/SARIF, parallel/workers, baseline and fresh flag/header changes')
