import os, sys, subprocess, numpy as np
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from pe import *
n,TVA,tvsz,traw,trsz=sec('.text')
T=np.frombuffer(B[traw:traw+trsz],dtype=np.uint8)
_rel=None
def rel_targets():
    global _rel
    if _rel is None:
        import os
        if os.path.exists('rel.npy'):
            _rel=np.load('rel.npy')
        else:
            d=np.frombuffer(B[traw:traw+trsz-3] + b'',dtype=np.uint8)
            # int32 at every byte offset
            v=(T[:-3].astype(np.int64) | (T[1:-2].astype(np.int64)<<8) | (T[2:-1].astype(np.int64)<<16) | (T[3:].astype(np.int64)<<24))
            v=np.where(v>=2**31, v-2**32, v)
            pos=np.arange(len(v),dtype=np.int64)+TVA
            _rel=(pos+4+v).astype(np.int64)   # target if this were a rel32 ending the instruction
            np.save('rel.npy',_rel)
    return _rel
def refs(target):
    r=rel_targets()
    idx=np.nonzero(r==target)[0]
    return [int(i)+TVA for i in idx]   # rva of the disp32 field
def dis(start,length=0x200,count=None):
    """Intel-syntax disassembly of [start, start+length): capstone if installed (pip install capstone), else objdump or
    llvm-objdump on the PATH."""
    o=r2o(start); data=bytes(B[o:o+length])
    lines=[]
    try:
        import capstone
        md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        for ins in md.disasm(data, start):
            lines.append(' %x:\t%-24s\t%s %s'%(ins.address, ins.bytes.hex(' '), ins.mnemonic, ins.op_str))
    except ImportError:
        import tempfile, shutil
        tool=shutil.which('objdump') or shutil.which('llvm-objdump')
        if tool is None:
            raise RuntimeError('no disassembler: pip install capstone (or put objdump / llvm-objdump on the PATH)')
        with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as t:
            t.write(data); blob=t.name
        r=subprocess.run([tool,'-D','-b','binary','-m','i386:x86-64','-M','intel','--adjust-vma=%#x'%start,blob],capture_output=True,text=True).stdout
        os.remove(blob)
        lines=[x for x in r.splitlines() if ':\t' in x]
    if count: lines=lines[:count]
    return '\n'.join(lines)
# function start via .pdata
import bisect
_pd=None
def pdata():
    global _pd
    if _pd is None:
        n,va,vsz,raw,rsz=sec('.pdata')
        a=np.frombuffer(B[raw:raw+rsz],dtype=np.uint32).reshape(-1,3)
        a=a[a[:,0]!=0]
        _pd=a[np.argsort(a[:,0])]
    return _pd
def func_of(rva):
    p=pdata()
    i=np.searchsorted(p[:,0],rva,side='right')-1
    if i<0: return None
    b,e,u=p[i]
    if b<=rva<e: 
        # follow chained unwind info to primary
        return int(b),int(e)
    return None
def func_start(rva):
    # walk chained unwind (UNW_FLAG_CHAININFO) back to root function
    p=pdata()
    f=func_of(rva)
    if not f: return None
    b,e=f
    i=np.searchsorted(p[:,0],b)
    u=int(p[i][2])
    for _ in range(10):
        flags=B[r2o(u)]>>3
        if flags & 4:
            cnt=B[r2o(u)+2]; off=u+4+((cnt+1)&~1)*2
            b2=u32(off); u=u32(off+8); b=b2
        else: break
    return b
