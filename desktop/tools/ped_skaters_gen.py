import re,sys,binascii
S=sys.argv[1]
have=set(open(S+'/pedfiles.txt').read().split())
mesh={}
for fn in ['cas_ped_m.q','cas_ped_f.q']:
    cat=None;did=None
    for l in open(S+'/'+fn,encoding='latin-1'):
        l=l.strip()
        m=re.match(r'^(ped_\w+) = \[$',l)
        if m: cat=m.group(1);continue
        m=re.match(r'^desc_id = (.+)$',l)
        if m: did=m.group(1);continue
        m=re.match(r'^mesh = "(.+)"$',l)
        if m and cat and did: mesh[(cat,did.lower())]=m.group(1).replace('\\','/').lower()
t=open(S+'/profile_ped.q',encoding='latin-1').read().split('\n')
blocks=[];i=0
while i<len(t):
    m=re.match(r'^([A-Za-z_0-9]+) = \{$',t[i])
    if m:
        d=1;j=i+1;body=[]
        while j<len(t) and d:
            d+=t[j].count('{')-t[j].count('}')
            if d: body.append(t[j])
            j+=1
        blocks.append((m.group(1),body));i=j
    else: i+=1
def crc(s): return binascii.crc32(s.lower().encode())^0xffffffff
out=[];drop=[]
for name,body in blocks:
    l=name.lower()
    if 'multistage' in l or 'random' in l or l.startswith('sample_') or l.endswith('_profile'): continue
    parts=[b for b in body if re.match(r'^(ped_\w+) = \{',b)]
    if not parts: continue
    if all('randomized_desc_id' in b or 'random_index' in b for b in parts if not b.startswith('ped_board')): continue
    if not any(re.match(r'^ped_(body|m_head|f_head)',b) for b in parts): continue
    ok=True;nb=[]
    for b in body:
        if b.startswith('ped_board'): continue
        m=re.match(r'^(ped_\w+) = \{ desc_id = (.+) \}$',b)
        if m:
            cat,did=m.groups()
            ms=mesh.get((cat,did.lower()))
            if not ms or ms not in have: ok=False;break
            if ' ' in did or did.startswith('#'):
                b='%s = { desc_id = #%08x }'%(cat,crc(did)) if not did.startswith('#') else b
        elif re.match(r'^ped_\w+ = \{',b) and 'randomized_desc_id' not in b:
            ok=False;break
        nb.append(b)
    if not ok: drop.append(name);continue
    female=any(b.startswith('ped_f_') for b in parts) or bool(re.search(r'_F_|Bikini|Dancer|woman|Girl|Bride|Hula|Skank|Babushka|Stroller',name))
    nb.append('ped_board = { desc_id = Skateboard }')
    disp=re.sub(r'^(Ped_|PedPro_|Skate_)','',name)
    if name.startswith('PedPro_'): disp='Ped '+disp
    if name.startswith('Skate_'): disp=disp+' (Skating)'
    disp=disp.replace('_',' ')
    disp=re.sub(r' 0(\d)$',r' \1',disp)
    out.append((name,disp,female,' '.join(nb)))
sys.stderr.write('kept %d dropped %d: %s\n'%(len(out),len(drop),' '.join(drop)))
print('// Generated from scripts\\\\profile_ped.qb (named peds whose models ship with THUG)')
print('struct PietonJouable { const char *nom, *affiche; int femme; const char *apparence; };')
print('static const PietonJouable s_pietons[] = {')
for n,d,f,a in out:
    print('\t{ "%s", "%s", %d, "%s" },'%(n,d,int(f),a.replace('"','\\"')))
print('};')
