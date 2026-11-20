#!/usr/bin/env python3
"""Symbolize 'libnba2k16_clean_opt.so+0xOFFSET' lines from a port.log against
the .so's dynsym table (ARM ELF32). Usage: python symbolize.py <so> <port.log>"""
import re, sys, struct, bisect

def dynsyms(path):
    d = open(path,'rb').read()
    def u16(o): return struct.unpack_from('<H',d,o)[0]
    def u32(o): return struct.unpack_from('<I',d,o)[0]
    def u64(o): return struct.unpack_from('<Q',d,o)[0]
    assert d[:4]==b'\x7fELF' and d[4]==1
    e_phoff=u32(28); e_phentsize=u16(42); e_phnum=u16(44)
    dynoff=dynsz=0
    for i in range(e_phnum):
        p=e_phoff+i*e_phentsize
        p_type,p_off,p_vaddr,p_filesz=u32(p),u32(p+4),u32(p+8),u32(p+16)
        if p_type==2: dynoff,dynsz=p_off,p_filesz
    tags={}
    i=dynoff
    while i<dynoff+dynsz:
        tag,val=u32(i),u32(i+4); i+=8
        if tag==0: break
        tags.setdefault(tag,val)
    strtab,symtab=tags[5],tags[6]; strsz=tags[10]
    cnt=(strtab-symtab)//16
    out=[]
    for k in range(cnt):
        o=symtab+k*16
        name,val,sz,info,other,shndx=struct.unpack_from('<IIIBBH',d,o)
        if val and shndx!=0:
            e=d.index(b'\0',strtab+name)
            nm=d[strtab+name:e].decode('utf8','replace')
            out.append((val,sz,nm))
    return sorted(out)

def symify(syms, off):
    i=bisect.bisect_right(syms,(off,10**18,''))-1
    for j in range(i,max(-1,i-8),-1):
        v,sz,nm=syms[j]
        if v<=off and (sz==0 or off-v<max(sz,0x1000)):
            return f"{nm}+0x{off-v:x}"
    return f"sub+0x{off:x}"

so=sys.argv[1]
lines=open(sys.argv[2],'rb').read().decode('utf8','replace').splitlines()
syms=dynsyms(so)
print(f"# {len(syms)} defined dynsyms", file=sys.stderr)
pat=re.compile(r'libnba2k16_clean_opt\.so\+0x([0-9a-fA-F]+)')
for L in lines:
    m=pat.search(L)
    if m:
        L=L+f"  <{symify(syms,int(m.group(1),16))}>"
    print(L)
