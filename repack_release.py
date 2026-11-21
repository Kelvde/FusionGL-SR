#!/usr/bin/env python3
# FusionGL 正式版打包: 等长替换包名/显示名, 骨架结构不动。
import zipfile, sys, zlib, struct

BASE = "/sdcard/Download/DSH-Go/base_installed_tester.apk"
NEWSO = "/root/workspace/FusionGL-build/build-ffx/libMobileGL.so"
OUT = sys.argv[1]
LIB = "lib/arm64-v8a/libMobileGL.so"

def patch_strings(b):
    """等长替换: 显示名与库引用。UTF-8 与 UTF-16LE 双处理。"""
    pairs = [
        ("FusionGL Test", "FusionGL     "),   # 14 -> 14 (空格填充)
        ("FusionFG",      "FusionGL"),        # 8 -> 8
    ]
    for old, new in pairs:
        assert len(old) == len(new), (old, len(old), len(new))
        b = b.replace(old.encode(), new.encode())
        b = b.replace(old.encode("utf-16-le"), new.encode("utf-16-le"))
    return b

def patch_package(b):
    """只替换独立 package 属性串(top.fusiongl.tester -> top.fusiongl.plugin)。"""
    old = "top.fusiongl.tester".encode("utf-16-le") + b"\x00\x00"
    new = "top.fusiongl.plugin".encode("utf-16-le") + b"\x00\x00"
    assert len(old) == len(new)
    n = b.count(old)
    b = b.replace(old, new)
    print(f"  package rename x{n}")
    assert n >= 1, "package string not found"
    return b

zin = zipfile.ZipFile(BASE)
entries = [(i.filename, zin.read(i.filename), i.date_time, i.external_attr, i.internal_attr) for i in zin.infolist()]
zin.close()
out = open(OUT, "wb"); central = []
for name, data, dt, ea, ia in entries:
    nb = name.encode(); extra = b""
    if name == "AndroidManifest.xml": data = patch_package(patch_strings(data))
    elif name == "resources.arsc":    data = patch_package(patch_strings(data))
    elif name == LIB:                 data = open(NEWSO, "rb").read()
    method = zipfile.ZIP_STORED if name == "resources.arsc" else zipfile.ZIP_DEFLATED
    if method == zipfile.ZIP_STORED: payload=data; crc=zipfile.crc32(data)&0xffffffff; csize=len(data)
    else:
        co=zlib.compressobj(6,zlib.DEFLATED,-15); payload=co.compress(data)+co.flush()
        crc=zipfile.crc32(data)&0xffffffff; csize=len(payload)
    hdr=out.tell(); need=(4-((hdr+30+len(nb))%4))%4
    if name=="resources.arsc" and need: extra=b"\x00"*need
    out.write(struct.pack("<IHHHHHIIIHH",0x04034b50,20,0,method,0,0,crc,csize,len(data),len(nb),len(extra))+nb+extra+payload)
    central.append((name,nb,method,crc,csize,len(data),extra,hdr,ea))
cd=out.tell()
for name,nb,method,crc,csize,usize,extra,hdr,ea in central:
    out.write(struct.pack("<IHHHHHHIIIHHHHHII",0x02014b50,20,20,0,method,0,0,crc,csize,usize,len(nb),len(extra),0,0,0,ea,hdr)+nb+extra)
csz=out.tell()-cd
out.write(struct.pack("<IHHHHIIH",0x06054b50,0,0,len(central),len(central),csz,cd,0)); out.close()
print("built", OUT)
