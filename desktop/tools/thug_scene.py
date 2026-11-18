"""Reader for THUG Xbox scene/skin files (.skin.xbx, .scn.xbx): materials,
sectors with positions, normals, weights, bone indices, uvs, meshes."""
import struct
class R:
    def __init__(s,d,o=0): s.d,s.o=d,o
    def t(s,f):
        v=struct.unpack_from('<'+f,s.d,s.o); s.o+=struct.calcsize('<'+f); return v
    def one(s,f): return s.t(f)[0]
def read_materials(r):
    mats=[]; st=r.o
    n=r.one('I')
    for _ in range(n):
        m=dict(start=r.o)
        m['crc'],m['name'],m['passes'],m['acut']=r.t('4I')
        m['sorted']=r.one('B'); m['draw_order']=r.one('f'); m['single']=r.one('B'); m['nobfc']=r.one('B'); m['zbias']=r.one('i')
        g=r.one('B')
        if g: r.t('fi')
        sp=r.one('f')
        if sp>0: r.t('3f')
        m['tex']=[]
        for p in range(m['passes']):
            tc,fl=r.t('2I'); hc=r.one('B'); col=r.t('3f'); ra=r.one('Q'); r.t('2I2fI')
            m['tex'].append((tc,fl,hc,col,ra))
            if fl&1: r.t('8f')
            if p==0 and fl&2:
                ns=r.one('I')
                for _ in range(ns):
                    nk=r.one('I'); r.one('i'); r.t('%di'%(nk*2))
            if fl&0x800:
                nk,_,_,_=r.t('4i'); r.t('%dI'%(nk*2))
            r.t('4I')
        m['end']=r.o
        mats.append(m)
    return mats,st,r.o
def read_sector(r):
    s={}
    s['crc'],s['bone'],s['flags'],s['nmesh']=r.t('IiiI'); s['bbox']=r.t('6f'); s['bs']=r.t('4f')
    f=s['flags']
    if f&0x800000: r.t('I9f')
    nv,stride=r.t('ii'); s['nv']=nv; s['stride']=stride
    s['pos']=[r.t('3f') for _ in range(nv)]
    if f&4: s['nrm']=[r.t('3f') for _ in range(nv)]
    if f&0x10: s['w']=list(r.t('%dI'%nv)); s['bi']=[r.t('4H') for _ in range(nv)]
    if f&1:
        nt=r.one('i'); s['ntc']=nt; s['uv']=[r.t('%df'%(2*nt)) for _ in range(nv)]
    if f&2: s['col']=list(r.t('%dI'%nv))
    if f&0x800: r.t('%dB'%nv)
    s['meshes']=[]
    for _ in range(s['nmesh']):
        m={}; m['cen']=r.t('4f'); m['bb']=r.t('6f'); m['flags'],m['mat'],m['nlod']=r.t('3I')
        m['lods']=[]
        for _ in range(m['nlod']):
            ni=r.one('i'); m['lods'].append(r.t('%dH'%ni))
        s['meshes'].append(m)
    return s
def read_scene(d):
    r=R(d); vers=r.t('3I'); mats,ms,me=read_materials(r)
    ns=r.one('i'); secs=[read_sector(r) for _ in range(ns)]
    nh=r.one('i')
    return dict(vers=vers,mats=mats,secs=secs,nh=nh,end=r.o,size=len(d),matblock=(ms,me))
if __name__=='__main__':
    import sys
    d=open(sys.argv[1],'rb').read(); sc=read_scene(d)
    print('vers',sc['vers'],'mats',len(sc['mats']),'secs',len(sc['secs']),'nh',sc['nh'],'end',sc['end'],sc['size'])
    for m in sc['mats']: print(' mat %08x name %08x passes %d'%(m['crc'],m['name'],m['passes']),['%08x fl=%x c=%s'%(t[0],t[1],t[3]) for t in m['tex']], 'sorted',m['sorted'],'single',m['single'],'acut',m['acut'])
    for s in sc['secs']:
        print(' sec %08x bone %d flags %x nv %d stride %d ntc %s'%(s['crc'],s['bone'],s['flags'],s['nv'],s['stride'],s.get('ntc')))
        for m in s['meshes']: print('   mesh flags %x mat %08x lods %s'%(m['flags'],m['mat'],[len(l) for l in m['lods']]), 'cen',['%.1f'%v for v in m['cen']])
        xs=s['pos']; print('   bbox',[round(min(a[k] for a in xs),1) for k in range(3)],[round(max(a[k] for a in xs),1) for k in range(3)])
        if 'bi' in s: print('   bones used',sorted(set(b for bi in s['bi'] for b in bi[:3])), 'w0 %08x bi0 %s'%(s['w'][0],s['bi'][0]))
