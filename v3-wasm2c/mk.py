import re,glob,mmap,collections,sys
roots=['w2c_WebClient_f151529']+[l.strip() for l in open('roots.txt') if l.strip()]
pat=re.compile(rb'^(?:static )?(?:u32|u64|f32|f64|void|v128) (w2c_WebClient_f\d+)\(', re.M)
maps={}; order={}
for f in glob.glob('WC_*.c'):
    fh=open(f,'rb'); m=mmap.mmap(fh.fileno(),0,access=mmap.ACCESS_READ); maps[f]=m
    h=list(pat.finditer(m))
    for i,x in enumerate(h):
        order[x.group(1).decode()]=(f,x.start(),h[i+1].start() if i+1<len(h) else len(m))
cp=re.compile(rb'\b(w2c_WebClient_f\d+)\(instance')
q=collections.deque(roots); seq=[]; ss=set()
while q:
    n=q.popleft()
    if n in ss: continue
    d=order.get(n)
    if not d: continue
    ss.add(n); seq.append(n)
    for c in set(cp.findall(maps[d[0]][d[1]:d[2]])):
        c=c.decode()
        if c not in ss: q.append(c)
s=open('WC_0.c',encoding='utf8',errors='replace').read()
out=[b'/* slice */\n']
for n in seq: out.append(maps[order[n][0]][order[n][1]:order[n][2]]); out.append(b'\n')
out.append("".join(re.findall(r'static void wrap_w2c_WebClient_f\d+\([^)]*\) \{\n[^\n]*\n\}\n', s)).encode())
out.append("\n".join(re.findall(r'FUNC_TYPE_EXTERN_T\(w2c_WebClient_t\d+\) = "[^"]*";', s)).encode())
for pm in (r'const wasm_elem_segment_expr_t elem_segment_exprs_w2c_WebClient_e0\[\] = \{.*?\n\};', r'const u8 data_segment_data_w2c_WebClient_d\d+\[\] = \{.*?\n\};'):
    out.append(("\n".join(re.findall(pm, s, re.S))+"\n").encode())
out.append(b"\n/* init */\n")
for fn in ('init_globals','init_tables','init_memories','init_elem_instances','init_data_instances','init_instance_import'):
    m=re.search(r'\nstatic void '+fn+r'\(.*?\n\}\n', s, re.S)
    if m: out.append(m.group(0).encode())
m=re.search(r'\nvoid wasm2c_WebClient_instantiate\(.*?\n\}\n', s, re.S); out.append(m.group(0).encode())
# stubs for wrappers referenced but missing
txt=b''.join(out).decode('utf8','replace')
ia=txt.find('const wasm_elem_segment_expr_t'); ii=txt.find('/* init */')
need=set(re.findall(r'\bwrap_\w+', txt[ia:ii])); have=set(re.findall(r'static void (wrap_\w+)\(', txt))
st="\n".join(f"static void {n}(void* i){{(void)i;}}" for n in sorted(need-have))
txt=txt[:ia]+st+"\n"+txt[ia:]
open('sliced.c','w').write(txt)
print("funcs",len(seq))
