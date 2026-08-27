import re,sys,pickle
lines=open(sys.argv[1],errors='replace').read().split('\n')
rows=[]
for i,l in enumerate(lines):
    m=re.match(r'^ (\.[\w.$*]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$', l)
    if m: sec,addr,size,obj=m.group(1),int(m.group(2),16),int(m.group(3),16),m.group(4).strip()
    else:
        m2=re.match(r'^ (\.[\w.$*]+)\s*$', l)
        if not (m2 and i+1<len(lines)): continue
        n=re.match(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$', lines[i+1])
        if not n: continue
        sec,addr,size,obj=m2.group(1),int(n.group(1),16),int(n.group(2),16),n.group(3).strip()
    if size==0 or '.obj' not in obj: continue
    if addr==0: continue                      # discarded by --gc-sections
    if not sec.startswith(('.text','.literal','.rodata','.data','.iram','.dram','.bss','.ext_ram')): continue
    if '.str' in sec: continue
    mm=re.search(r'\(([^)]+)\)\s*$', obj)
    rows.append((mm.group(1) if mm else obj.split('/')[-1], sec, size))
pickle.dump(rows,open(sys.argv[2],'wb')); print("live sections:",len(rows),"bytes:",sum(r[2] for r in rows))
