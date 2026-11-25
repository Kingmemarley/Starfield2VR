import re, struct, sys
sys.path.insert(0,__import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from pe import *
import pickle, os
CACHE='rtti.pkl'
def build():
    # type descriptors: '.?AV' / '.?AU' names in .data (TypeDescriptor lives in .data)
    tds={}
    for m in re.finditer(rb'\.\?A[VU][^\x00]{1,400}?\x00', B):
        off=m.start(); rva=o2r(off)
        if rva is None: continue
        td=rva-0x10
        tds[td]=m.group()[:-1].decode('latin1')
    # COLs in .rdata: sig 1, typedesc rva in tds, self rva == own rva
    n,va,vsz,raw,rsz=sec('.rdata')
    cols={}
    data=B[raw:raw+rsz]
    for i in range(0,len(data)-24,4):
        if data[i:i+4]!=b'\x01\0\0\0': continue
        tdr,cdr,selfr=struct.unpack_from('<III',data,i+12)
        if selfr==va+i and tdr in tds:
            off_,cdoff=struct.unpack_from('<II',data,i+4)
            cols[va+i]=(tdr,off_,cdr)
    # vtables: qword == IMAGEBASE+colrva
    colva={IMAGEBASE+c:c for c in cols}
    vts={}
    for i in range(0,len(data)-8,8):
        q=struct.unpack_from('<Q',data,i)[0]
        if q in colva:
            vts[va+i+8]=colva[q]
    return tds,cols,vts
if os.path.exists(CACHE):
    tds,cols,vts=pickle.load(open(CACHE,'rb'))
else:
    tds,cols,vts=build(); pickle.dump((tds,cols,vts),open(CACHE,'wb'))
def vtables_of(name):
    out=[]
    for vt,c in vts.items():
        tdr,off_,cdr=cols[c]
        if tds[tdr]==name: out.append((vt,off_))
    return sorted(out,key=lambda x:x[1])
def bases(col):
    tdr,off_,cdr=cols[col]
    nb=u32(cdr+8); arr=u32(cdr+12)
    res=[]
    for i in range(nb):
        bd=u32(arr+4*i); res.append(tds.get(u32(bd),'?'))
    return res
if __name__=='__main__':
    print(len(tds),len(cols),len(vts))
