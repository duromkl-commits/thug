"""THAW PC (ReTHAWed) .skin.wpc / .tex.wpc readers, for thaw_to_thug.py."""
import struct,numpy as np
def read_skin(fn):
    d=open(fn,'rb').read()
    sz=struct.unpack_from('<I',d,0x24)[0];S=0x20+sz;B=S+8+struct.unpack_from('<I',d,S+4)[0]
    nsec,o1,o2,_,o3,o4=struct.unpack_from('<6I',d,B+0x80)
    nmat=struct.unpack_from('<H',d,0x22)[0]
    mats={}
    for i in range(nmat):
        m=0x30+i*0x120
        mats[struct.unpack_from('<I',d,m)[0]]=d[m:m+0x120]
    meshes=[];o=B+o4
    nm=sum(struct.unpack_from('<I',d,B+o2+k*0x68+0x44)[0] for k in range(nsec))
    for _ in range(nm):
        assert d[o]==0x40,hex(o)
        mat,=struct.unpack_from('<I',d,o+0x14); st=d[o+0x18]
        nv=struct.unpack_from('<H',d,o+0x26)[0]; ni=struct.unpack_from('<I',d,o+0x28)[0]
        io=struct.unpack_from('<I',d,o+0x5c)[0]; vo=struct.unpack_from('<I',d,o+0x7c)[0]
        V=d[B+vo:B+vo+nv*st]
        a=np.frombuffer(V,dtype=np.dtype({'names':['p','w','b','n','c','uv'],'formats':[('<f4',3),'<u4',('<u2',4),'<u4','<u4',('<f4',2)],'offsets':[0,12,16,24,28,32],'itemsize':st}))
        idx=struct.unpack_from('<%dH'%ni,d,B+io+4)
        meshes.append(dict(mat=mat,v=a,idx=idx,hdr=d[o:o+0xe0]))
        o+=0xe0
    return mats,meshes
def weights(w):
    return np.stack([(w&0x7ff)/1023.0,((w>>11)&0x7ff)/1023.0,((w>>22)&0x3ff)/511.0],1)
def dds(w,h,fourcc,data):
    import io
    from PIL import Image
    hd=struct.pack('<4sIIIIIII44xII4sIIIII4xIIII','DDS '.encode(),124,0x81007,h,w,len(data),0,1,32,4,fourcc,0,0,0,0,0,0x1000,0,0,0)
    return Image.open(io.BytesIO(hd+data)).convert('RGBA')
def read_tex(fn):
    from PIL import Image
    d=open(fn,'rb').read()
    n=struct.unpack_from('<H',d,6)[0];p=8;out={}
    for _ in range(n):
        mg,v,hs=struct.unpack_from('<IHH',d,p);assert mg==0xabadd00d,hex(p)
        ck,w,h,w2,h2,lv,bpp,fm=struct.unpack_from('<IHHHHBBH',d,p+8)
        q=p+8+16
        img=None
        for l in range(lv):
            if fm==0:
                pitch,rows=struct.unpack_from('<HH',d,q);n_=pitch*rows
            else:
                n_,=struct.unpack_from('<I',d,q)
            q+=4
            if l==0:
                raw=d[q:q+n_]
                if fm==0:
                    a=np.frombuffer(raw,np.uint8).reshape(rows,pitch//4,4)[:h,:w,[2,1,0,3]]
                    img=Image.fromarray(np.ascontiguousarray(a),'RGBA')
                elif fm==1: img=dds(w,h,b'DXT1',raw)
                elif fm==5: img=dds(w,h,b'DXT5',raw)
                elif fm==3: img=dds(w,h,b'DXT3',raw)
                else: raise Exception('fmt %d'%fm)
            q+=n_
        out[ck]=(img,fm);p=q
    assert p==len(d),(p,len(d))
    return out
