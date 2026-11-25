import sys,re; sys.path.insert(0,__import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from xref import *
from rtti import tds, cols, vts, bases
VTNAME={}
for vt,c in vts.items():
    tdr,off_,cdr=cols[c]; VTNAME[vt]=(tds[tdr],off_)
def vtname(a):
    return VTNAME.get(a)
def annotate(text):
    out=[]
    for l in text.splitlines():
        m=re.search(r'# 0x([0-9a-f]+)',l)
        extra=''
        if m:
            a=int(m.group(1),16)
            if a in VTNAME: extra='   <vt %s +%d>'%VTNAME[a]
            else:
                o=r2o(a)
                if o is not None and sec('.rdata')[1]<=a<sec('.rdata')[1]+sec('.rdata')[2]:
                    s=B[o:o+80]; e=s.find(b'\0')
                    if e>=3 and all(32<=c<127 for c in s[:e]): extra='   "%s"'%s[:e].decode()
                    else:
                        # utf16?
                        try:
                            e2=s.find(b'\0\0')
                        except: pass
        out.append(l+extra)
    return '\n'.join(out)
def dfun(rva,maxlen=None):
    b,e=func_of(rva)
    if maxlen: e=min(e,b+maxlen)
    return annotate(dis(b,e-b))
def writers(glob):
    # instructions with rip-relative disp targeting glob, preceded by mov opcode 89 (store)
    res=[]
    for r in refs(glob):
        op=rd(r-3,3)
        res.append((r, op.hex()))
    return res
