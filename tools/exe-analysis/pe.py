import struct, mmap, os
# Starfield.exe (1.16.236): set STARFIELD_EXE, or the default path below is used.
EXE=os.environ.get('STARFIELD_EXE', r'D:\Program Files\Steam\steamapps\common\Starfield\Starfield.exe')
f=open(EXE,'rb'); B=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
pe=struct.unpack_from('<I',B,0x3c)[0]
nsec=struct.unpack_from('<H',B,pe+6)[0]; optsz=struct.unpack_from('<H',B,pe+20)[0]
opt=pe+24
IMAGEBASE=struct.unpack_from('<Q',B,opt+24)[0]
secs=[]
for i in range(nsec):
    o=opt+optsz+i*40
    name=B[o:o+8].rstrip(b'\0').decode()
    vsz,va,rsz,raw=struct.unpack_from('<IIII',B,o+8)
    secs.append((name,va,vsz,raw,rsz))
def r2o(rva):
    for n,va,vsz,raw,rsz in secs:
        if va<=rva<va+max(vsz,rsz): 
            if rva-va<rsz: return raw+rva-va
            return None
    return None
def o2r(off):
    for n,va,vsz,raw,rsz in secs:
        if raw<=off<raw+rsz: return va+off-raw
    return None
def sec(name):
    for s in secs:
        if s[0]==name: return s
def u32(rva): return struct.unpack_from('<I',B,r2o(rva))[0]
def i32(rva): return struct.unpack_from('<i',B,r2o(rva))[0]
def u64(rva): return struct.unpack_from('<Q',B,r2o(rva))[0]
def cstr(rva,n=256):
    o=r2o(rva); e=B.find(b'\0',o,o+n); return B[o:e].decode('latin1')
def rd(rva,n): o=r2o(rva); return B[o:o+n]
if __name__=='__main__':
    print(hex(IMAGEBASE))
    for s in secs: print(s[0],hex(s[1]),hex(s[2]),hex(s[3]),hex(s[4]))
