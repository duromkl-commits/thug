"""Max Payne (2001) data: RAS archives and the MaxFX KFS mesh / SKD skin streams.

RAS: "RAS\0", seed, then an encrypted header, file table and directory table
(per-byte: rotate left i%5, xor (i+3)*6, add the low byte of a Park-Miller
style seed); entries are stored, or "RA->" + sizes + Okumura LZSS.
KFS/SKD: a tagged stream (0x0c object header, 0x0d string, 0x09 float,
0x12/0x13/0x14 int32/16/8, 0x11 byte, 0x0e bool, 0x15/0x16 vec2/3, 0x1a 4x3
matrix, row vectors). Coordinates are left handed, y up, metres."""
import struct, sys, os
import numpy as np
def c_div(a,b):
    q=abs(a)//abs(b); return q if (a>=0)==(b>=0) else -q
def dec(buf,seed,mode=0):
    if seed==0: seed=1
    out=bytearray(len(buf))
    for i,b in enumerate(buf):
        seed=(seed*171)-(c_div(seed,177)*30269)
        seed=((seed+2**31)%2**32)-2**31
        s=i%5
        if mode==0:
            b=((b<<s)|(b>>(8-s)))&0xff if s else b
            b^=((i+3)*6)&0xff
            b=(b+seed)&0xff
        else:
            b=(b-seed)&0xff
            b^=((i+3)*6)&0xff
            b=((b>>s)|(b<<(8-s)))&0xff if s else b
        out[i]=b
    return bytes(out)
def lzss(src,n):
    N,F,T=4096,18,2
    buf=bytearray(b' '*N); r=N-F; out=bytearray(); i=0; flags=0
    while len(out)<n and i<len(src):
        flags>>=1
        if not flags&0x100:
            flags=src[i]|0xff00; i+=1
        if flags&1:
            c=src[i]; i+=1; out.append(c); buf[r]=c; r=(r+1)&(N-1)
        else:
            a=src[i]; b=src[i+1]; i+=2
            a|=(b&0xf0)<<4; l=(b&0x0f)+T
            for k in range(l+1):
                c=buf[(a+k)&(N-1)]; out.append(c); buf[r]=c; r=(r+1)&(N-1)
    return bytes(out[:n])
def entries(path):
    f=open(path,'rb'); h=f.read(0x2c)
    seed=struct.unpack('<i',h[4:8])[0]
    nf,nd,fts,dts=struct.unpack('<4i',dec(h[8:0x2c],seed)[:16])
    ft=dec(f.read(fts),seed); dt=dec(f.read(dts),seed)
    dirs=[];p=0
    for k in range(nd):
        e=dt.index(b'\0',p); dirs.append(dt[p:e].decode('latin1')); p=e+1+16
    out=[];p=0;off=0x2c+fts+dts
    for k in range(nf):
        e=ft.index(b'\0',p); name=ft[p:e].decode('latin1'); p=e+1
        size,esize,u1,di,u2,meth=struct.unpack('<6i',ft[p:p+24]); p+=24+16
        out.append((dirs[di]+name,size,esize,meth,off)); off+=esize
    return out
def read(path,e):
    f=open(path,'rb'); f.seek(e[4]); d=f.read(e[2])
    if d[:4]==b'RA->':
        u,c=struct.unpack('<2i',d[4:12]); return lzss(d[12:],u)
    return d
def tokens(d):
    i=0;n=len(d)
    while i<n:
        t=d[i];i+=1
        if t==0x0c:
            yield ('obj',struct.unpack('<HHii',d[i:i+12]),i-1);i+=12
        elif t==0x0d:
            # string: int token for length then bytes
            lt=d[i];i+=1
            if lt==0x14: L=d[i];i+=1
            elif lt==0x13: L=struct.unpack('<H',d[i:i+2])[0];i+=2
            elif lt==0x12: L=struct.unpack('<I',d[i:i+4])[0];i+=4
            else: raise Exception('strlen tag %x at %d'%(lt,i))
            yield ('str',d[i:i+L].decode('latin1'),i);i+=L
        elif t==0x09: yield ('f',struct.unpack('<f',d[i:i+4])[0],i);i+=4
        elif t==0x14: yield ('i',d[i],i);i+=1
        elif t==0x13: yield ('i',struct.unpack('<H',d[i:i+2])[0],i);i+=2
        elif t==0x12: yield ('i',struct.unpack('<I',d[i:i+4])[0],i);i+=4
        elif t==0x1a: yield ('m',struct.unpack('<12f',d[i:i+48]),i);i+=48
        elif t==0x16: yield ('v3',struct.unpack('<3f',d[i:i+12]),i);i+=12
        elif t==0x15: yield ('v2',struct.unpack('<2f',d[i:i+8]),i);i+=8
        elif t==0x11: yield ('u8',d[i],i);i+=1
        elif t==0x03: yield ('u32',struct.unpack('<I',d[i:i+4])[0],i);i+=4
        elif t==0x0e: yield ('b',d[i],i);i+=1
        else:
            yield ('?%02x'%t,d[i:i+16].hex(),i-1); return
def objs(T):
    return [i for i,t in enumerate(T) if t[0]=='obj']
def skeleton(d):
    T=list(tokens(d)); B={}
    order=[]
    for i,t in enumerate(T):
        if t[0]=='obj' and t[1][0]==0:
            n,p,m=T[i+1][1],T[i+2][1],T[i+3][1]
            M=np.eye(4); M[:3,:3]=np.array(m[:9]).reshape(3,3); M[3,:3]=m[9:12]
            B[n]=(p,M); order.append(n)
    W={}
    for n in order:
        p,M=B[n]; W[n]=M@W[p] if p else M
    return order,{n:B[n][0] for n in order},W
def mesh_materials(d):
    T=list(tokens(d)); mats={}; cur=None
    for i,t in enumerate(T):
        if t[0]=='obj' and t[1][0]==16: cur=T[i+1][1]; mats[cur]=[]
        elif t[0]=='obj' and t[1][0]==17:
            j=i+2
            while j<len(T) and T[j][0]!='obj':
                if T[j][0]=='str' and '.' in T[j][1]: mats[cur].append(T[j][1])
                j+=1
    return mats
def mesh(d):
    T=list(tokens(d)); k=0
    mats={}  # material name -> textures
    cur=None
    for i,t in enumerate(T):
        if t[0]=='obj' and t[1][0]==16: cur=T[i+1][1]; mats[cur]=[]
        elif t[0]=='obj' and t[1][0]==17:
            j=i+2
            while j<len(T) and T[j][0]!='obj':
                if T[j][0]=='str' and '.' in T[j][1]: mats[cur].append(T[j][1])
                j+=1
    meshes=[]
    for i,t in enumerate(T):
        if t[0]=='obj' and t[1][0]==0:
            name=T[i+1][1]; m=T[i+3][1]
            M=np.eye(4); M[:3,:3]=np.array(m[:9]).reshape(3,3); M[3,:3]=m[9:12]
            meshes.append([name,M,i])
    out=[]
    for name,M,i0 in meshes:
        j=i0
        while T[j][0]!='obj' or T[j][1][0]!=6:
            j+=1
            if j>=len(T): break
        if j>=len(T): continue
        nv=T[j+1][1]; P=np.array([T[j+2+k][1] for k in range(nv)])
        j=j+2+nv
        assert T[j][1][0]==7; nf=T[j+1][1]; j+=2; F=[]
        for f in range(nf):
            assert T[j][0]=='obj' and T[j][1][0]==8
            c=T[j+1][1]; F.append([T[j+2+q][1] for q in range(c)]); j+=2+c
        nx=lambda j,c: next(q for q in range(j,len(T)) if T[q][0]=='obj' and T[q][1][0]==c)
        j=nx(j,12)
        assert T[j][1][0]==12; nm=T[j+1][1]; mn=[T[j+2+q][1] for q in range(nm)]; j+=2+nm
        cnt=T[j][1]; mi=[T[j+1+q][1] for q in range(cnt)]; j+=1+cnt
        j=nx(j,14)+1; ch=T[j][1]; nuf=T[j+1][1]; j+=2; UF=[]
        for f in range(nuf):
            c=T[j+1][1]; UF.append([T[j+2+q][1] for q in range(c)]); j+=2+c
        while T[j+1][0]!='v3': j+=1
        nu=T[j][1]; UV=np.array([T[j+1+q][1] for q in range(nu)]); j+=1+nu
        Pw=np.c_[P,np.ones(nv)]@M
        out.append(dict(name=name,P=Pw[:,:3],F=F,UF=UF,UV=UV,mat=[mn[q] for q in mi],mats=mats))
    return out
def skd(d):
    T=list(tokens(d)); j=1; rec=[]
    for blk in range(3):
        n=T[j][1]; j+=1
        for q in range(n):
            rec.append((T[j][1],T[j+1][1],T[j+2][1])); j+=3
        if T[j][0]=='str': break
    names=[]
    while T[j][0]!='str': j+=1
    j+=1
    while T[j][0]!='str': j+=1
    while T[j][0]=='str': names.append(T[j][1]); j+=1
    return rec,names
