#!/bin/bash
# FusionGL 正式打包脚本 v7+:固化全部步骤,内置三重校验,校验不过不出包。
# 用法: bash /root/workspace/FusionGL-build/build_v7.sh [输出名,默认 FusionGL_TEST_fusion_sr_v7.apk]
set -euo pipefail

B=/root/workspace/FusionGL-build
KS=$B/savefix.jks
BT=/root/workspace/mgl/sdk/build-tools/34.0.0
BASE=/sdcard/Download/DSH-Go/base_installed_tester.apk
SRC=/root/workspace/FusionGL
BUILD=$B/build-ffx
NAME="${1:-FusionGL_TEST_fusion_sr_v7.apk}"
RELEASE=0
case "$NAME" in *release*) RELEASE=1;; esac
UNSIGNED=$B/${NAME%.apk}_unsigned.apk
SIGNED=/sdcard/Download/DSH-Go/$NAME

echo "== [1/6] 同步源码到构建树 =="
RK=$B/mgl-src/MobileGL/MG_Backend/DirectVulkan/Renderer
for f in FfxProbe.cpp FfxProbe.h VulkanRendererDevice.cpp VulkanRendererPresent.cpp \
         fusion_sr_upsample_spv.h fusion_sr_sharpen_spv.h VulkanRendererPipeline.cpp \
         VulkanRendererInternal.h; do
  [ -f "$SRC/$f" ] && cp "$SRC/$f" "$RK/$f"
done
# VulkanRendererDevice 在 Renderer 上一级
cp "$SRC/VulkanRendererDevice.cpp" "$B/mgl-src/MobileGL/MG_Backend/DirectVulkan/" 2>/dev/null || true
find "$B/mgl-src" -name 'VulkanRendererDevice.cpp' -path '*DirectVulkan*' | head -2

echo "== [2/6] ninja 编译 =="
cd "$BUILD"
ninja libMobileGL.so > "$B/latest_ninja.log" 2>&1 || { grep -iE 'error:' "$B/latest_ninja.log" | head -5; exit 1; }

echo "== [3/6] 内核指纹 =="
SO_MD5=$(md5sum "$BUILD/libMobileGL.so" | cut -d' ' -f1)
SO_SIZE=$(stat -c%s "$BUILD/libMobileGL.so")
echo "新内核: $SO_SIZE bytes, md5=$SO_MD5"
# 内核必须比原版大(证明链接进了新代码), 原版=18651976
[ "$SO_SIZE" -gt 20000000 ] || { echo "FATAL: 内核尺寸异常,疑似编了原版"; exit 1; }
echo "DEBUG: file=$(ls -l $BUILD/libMobileGL.so)"
N=$(LC_ALL=C strings "$BUILD/libMobileGL.so" | grep -c 'FfxUpscaleRecordEP17VkCommandBuffer' || true)
echo "DEBUG: matches=$N"
[ "$N" -ge 1 ] || { echo "FATAL: 内核里没有超分代码"; exit 1; }

echo "== [4/6] 重打包(读回校验 so) =="
if [ "$RELEASE" = "1" ]; then
  python3 "$B/repack_release.py" "$UNSIGNED"
  python3 - "$UNSIGNED" "$SO_MD5" <<'PY'
import zipfile, hashlib, struct, sys
OUT,EXPECT=sys.argv[1],sys.argv[2]
zin=zipfile.ZipFile(BASE)
z=zipfile.ZipFile(OUT)
    if method==zipfile.ZIP_STORED: payload=data; crc=zipfile.crc32(data)&0xffffffff; csize=len(data)
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
# 读回校验
z=zipfile.ZipFile(OUT)
got=hashlib.md5(z.read(LIB)).hexdigest()
assert got==EXPECT, f"PACK FAIL: so md5 {got} != {EXPECT}"
# arsc 对齐校验
i=z.getinfo("resources.arsc")
f=open(OUT,'rb'); f.seek(i.header_offset); h=f.read(30)
nl=struct.unpack('<H',h[26:28])[0]; el=struct.unpack('<H',h[28:30])[0]
assert (i.header_offset+30+nl+el)%4==0, "arsc not aligned"
print("pack+verify OK")
PY

echo "== [5/6] 签名 + 签名后二次校验 =="
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:savefix123 --key-pass pass:savefix123 \
  --v1-signing-enabled false --v2-signing-enabled true --v3-signing-enabled true \
  --out "$SIGNED" "$UNSIGNED" >/dev/null
"$BT/apksigner" verify "$SIGNED" >/dev/null || { echo "FATAL: 签名校验失败"; exit 1; }
python3 - "$SIGNED" "$SO_MD5" <<'PY'
import zipfile, sys, hashlib
z=zipfile.ZipFile(sys.argv[1])
got=hashlib.md5(z.read('lib/arm64-v8a/libMobileGL.so')).hexdigest()
assert got==sys.argv[2], f"SIGN FAIL: {got}"
print("signed-verify OK")
PY

echo "== [6/6] DONE =="
ls -lh "$SIGNED"; md5sum "$SIGNED"
echo "APK=$SIGNED"
