"""Return to Castle Wolfenstein model readers: MDS (skeletal bodies), MDC (compressed MD3 heads and
hats) and MD3. Coordinates stay in Quake units and axes."""
import struct, numpy as np
def axis(p,y,r):
    p,y,r=np.radians([p,y,r]); sp,cp=np.sin(p),np.cos(p); sy,cy=np.sin(y),np.cos(y); sr,cr=np.sin(r),np.cos(r)
    f=np.array([cp*cy,cp*sy,-sp])
    rt=np.array([-sr*sp*cy+cr*sy, -sr*sp*sy-cr*cy, -sr*cp])
    u=np.array([cr*sp*cy+sr*sy, cr*sp*sy-sr*cy, cr*cp])
    return np.array([f,-rt,u])
S2A=360.0/65536
def read(d):
    ident,ver,name,lodS,lodB,nF,nB,oF,oB,torsoP,nS,oS,nT,oT,oE=struct.unpack_from('<4si64sffiiiiiiiiii',d,0)
    bones=[]
    for i in range(nB):
        nm,par,tw,pd,fl=struct.unpack_from('<64siffi',d,oB+80*i)
        bones.append((nm.split(b'\0')[0].decode(),par,pd,fl))
    fsz=4*(6+3+1+3)+12*nB
    def frame(f):
        o=oF+fsz*f
        po=np.array(struct.unpack_from('<3f',d,o+40))
        comp=[struct.unpack_from('<6h',d,o+52+12*b) for b in range(nB)]
        return po,comp
    surfs=[]
    o=oS
    for s in range(nS):
        (sid,snm,shd,shi,minl,ohd,nv,ov,nt,ot,ocm,nbr,obr,oe)=struct.unpack_from('<i64s64siiiiiiiiiii',d,o)
        verts=[]; p=o+ov
        for v in range(nv):
            nx,ny,nz,u,t,nw,fp,fd=struct.unpack_from('<5fiif',d,p); p+=32
            ws=[]
            for k in range(nw):
                bi,bw,ox,oy,oz=struct.unpack_from('<if3f',d,p); p+=20
                ws.append((bi,bw,np.array([ox,oy,oz])))
            verts.append(((nx,ny,nz),(u,t),ws))
        tris=[struct.unpack_from('<3i',d,o+ot+12*k) for k in range(nt)]
        surfs.append((snm.split(b'\0')[0].decode(),shd.split(b'\0')[0].decode(),verts,tris))
        o+=oe
    tags=[]
    for i in range(nT):
        nm,tw,bi=struct.unpack_from('<64sfi',d,oT+72*i); tags.append((nm.split(b'\0')[0].decode(),bi))
    return dict(bones=bones,nF=nF,frame=frame,surfs=surfs,tags=tags)
def pose(m,f,transpose=False):
    po,comp=m['frame'](f)
    T=[None]*len(m['bones']); R=[None]*len(m['bones'])
    for i,(nm,par,pd,fl) in enumerate(m['bones']):
        a=comp[i]
        M=axis(a[0]*S2A,a[1]*S2A,a[2]*S2A)
        R[i]=M.T if transpose else M
        if par<0: T[i]=po.copy()
        else:
            fwd=axis(a[4]*S2A,a[5]*S2A,0)[0]
            T[i]=T[par]+pd*fwd
    return T,R
def verts(m,T,R,mode):
    out=[];disp=[]
    for snm,shd,vs,tris in m['surfs']:
        P=[]
        for n,uv,ws in vs:
            ps=[ (T[b]+(R[b]@off if mode==0 else off@R[b])) for b,w,off in ws]
            P.append(sum(w*p for (b,w,off),p in zip(ws,ps)))
            if len(ps)>1: disp.append(max(np.linalg.norm(p-P[-1]) for p in ps))
        out.append(np.array(P))
    return out,(np.mean(disp) if disp else 0)

def read_mdc(d):
    ident,ver,name,flags,nF,nT,nS,nSk,oFr,oTN,oTg,oS,oE=struct.unpack_from('<4si64siiiiiiiiii',d,0)
    assert ident in (b'IDPC',), ident
    surfs=[]; o=oS
    for s in range(nS):
        sid,snm,fl,ncf,nbf,nsh,nv,nt,ot,osh,ost,oxyz,oxc,ofb,ofc,oe=struct.unpack_from('<i64siiiiiiiiiiiiii',d,o)
        P=[];N=[]
        for v in range(nv):
            x,y,z,n=struct.unpack_from('<3hH',d,o+oxyz+8*v)
            P.append((x/64.0,y/64.0,z/64.0))
            lat=(n>>8)&255; lng=n&255
            la=lat*2*np.pi/255; lo=lng*2*np.pi/255
            N.append((np.cos(la)*np.sin(lo),np.sin(la)*np.sin(lo),np.cos(lo)))
        st=[struct.unpack_from('<2f',d,o+ost+8*v) for v in range(nv)]
        tris=[struct.unpack_from('<3i',d,o+ot+12*k) for k in range(nt)]
        shd=struct.unpack_from('<64s',d,o+osh)[0].split(b'\0')[0].decode() if nsh else ''
        surfs.append((snm.split(b'\0')[0].decode(),shd,np.array(P),np.array(N),st,tris))
        o+=oe
    return surfs

def mdc_tags(d):
    ident,ver,name,flags,nF,nT,nS,nSk,oFr,oTN,oTg,oS,oE=struct.unpack_from('<4si64siiiiiiiiii',d,0)
    out={}
    for t in range(nT):
        nm=struct.unpack_from('<64s',d,oTN+64*t)[0].split(b'\0')[0].decode()
        x,y,z,a,b,c=struct.unpack_from('<6h',d,oTg+12*t)
        sc=360.0/32700
        out[nm]=(np.array([x,y,z])/64.0, axis(a*sc,b*sc,c*sc))
    return out

def read_md3(d):
    ident,ver,name,flags,nF,nT,nS,nSk,oFr,oTg,oS,oE=struct.unpack_from('<4si64siiiiiiiii',d,0)
    surfs=[];o=oS
    for s in range(nS):
        sid,snm,fl,nfr,nsh,nv,nt,ot,osh,ost,oxyz,oe=struct.unpack_from('<4s64siiiiiiiiii'[:0]+'<i64siiiiiiiiii',d,o)
        P=[];N=[]
        for v in range(nv):
            x,y,z,n=struct.unpack_from('<3hH',d,o+oxyz+8*v)
            P.append((x/64.0,y/64.0,z/64.0))
            lat=(n>>8)&255; lng=n&255; la=lat*2*np.pi/255; lo=lng*2*np.pi/255
            N.append((np.cos(la)*np.sin(lo),np.sin(la)*np.sin(lo),np.cos(lo)))
        st=[struct.unpack_from('<2f',d,o+ost+8*v) for v in range(nv)]
        tris=[struct.unpack_from('<3i',d,o+ot+12*k) for k in range(nt)]
        shd=struct.unpack_from('<64s',d,o+osh)[0].split(b'\0')[0].decode() if nsh else ''
        surfs.append((snm.split(b'\0')[0].decode(),shd,np.array(P),np.array(N),st,tris))
        o+=oe
    return surfs

def md3_tags(d):
    """Frame 0 tags of an MD3: name -> (origin, axis rows)."""
    ident,ver,name,flags,nF,nT,nS,nSk,oFr,oTg,oS,oE=struct.unpack_from('<4si64siiiiiiiii',d,0)
    out={}
    for t in range(nT):
        o=oTg+112*t
        nm=struct.unpack_from('<64s',d,o)[0].split(b'\0')[0].decode()
        v=struct.unpack_from('<12f',d,o+64)
        out[nm]=(np.array(v[:3]),np.array(v[3:]).reshape(3,3))
    return out
