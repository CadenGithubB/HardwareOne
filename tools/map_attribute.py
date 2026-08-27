import re,sys,collections
path,filt,topn=sys.argv[1],(sys.argv[2] if len(sys.argv)>2 else ''),int(sys.argv[3]) if len(sys.argv)>3 else 40
tot=collections.defaultdict(collections.Counter)
lines=open(path,errors='replace').read().split('\n')
in_map=False
for i,l in enumerate(lines):
    if l.startswith('Linker script and memory map'): in_map=True; continue
    if not in_map: continue
    m=re.match(r'^ (\.[\w.$*]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$', l)
    if m: sec,size,obj=m.group(1),int(m.group(3),16),m.group(4).strip()
    else:
        m2=re.match(r'^ (\.[\w.$*]+)\s*$', l)
        if not (m2 and i+1<len(lines)): continue
        n=re.match(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$', lines[i+1])
        if not n: continue
        sec,size,obj=m2.group(1),int(n.group(2),16),n.group(3).strip()
    if size==0 or ('.obj' not in obj and '.o' not in obj): continue
    mm=re.search(r'\(([^)]+)\)\s*$', obj)
    name=mm.group(1) if mm else obj.split('/')[-1]
    lib=obj.split('(')[0].split('/')[-1] if '(' in obj else '-'
    c=tot[(lib,name)]
    if '.str' in sec: c['str']+=size
    elif sec.startswith('.text') or sec.startswith('.literal') or sec.startswith('.iram'): c['text']+=size
    elif sec.startswith('.rodata'): c['rodata']+=size
    elif sec.startswith('.data') or sec.startswith('.dram'): c['data']+=size
    elif sec.startswith('.bss') or sec.startswith('.noinit'): c['bss']+=size
    c['FLASH']=c['text']+c['rodata']+c['data']
rows=sorted(tot.items(), key=lambda kv:-kv[1]['FLASH'])
grand=sum(v['FLASH'] for v in tot.values())
print(f"# grand flash(text+rodata+data, excl merged strings) = {grand}")
n=0
for (lib,name),c in rows:
    if filt and filt not in lib and filt not in name: continue
    print(f"{c['FLASH']:8d}  text={c['text']:7d} ro={c['rodata']:6d} data={c['data']:6d} bss={c['bss']:6d} str~{c['str']:8d}  {lib}({name})")
    n+=1
    if n>=topn: break
