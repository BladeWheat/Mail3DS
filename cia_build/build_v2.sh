#!/bin/bash
set -e
cd /d/3ds_email
MR=/c/devkitPro/tools/bin/makerom.exe
ROMFS_WIN="D:\\3ds_email\\romfs"

rm -f cia_build/Mail3DS.cia
# -ignoresign: 未签名自制CIA必须带，否则FBI安装报 0xD8E0806A 证书签名/哈希校验失败
# 注意：本机 makerom v0.19.0 有bug，-minor / -ver 的次版本位会被清零，
#       命令行无法直接写出 1.1.0(1040)，先生成再用 patch_version.ps1 补丁TMD版本字段。
"$MR" -f cia -target t -exefslogo -ignoresign \
  -o cia_build/Mail3DS.cia \
  -elf Mail3DS.elf \
  -rsf cia_build/mail3ds_v2.rsf \
  -icon cia_build/mail3ds.smdh \
  -banner cia_build/banner.bnr \
  -DAPP_ROMFS="$ROMFS_WIN" 2>&1

# 补丁 TMD Title Version：1024(1.0.0,0x0400) -> 1040(1.1.0,0x0410)
# 3DS版本号=主*1024+次*16+修订；makerom无法直接写非0次版本，故二进制补丁
powershell -ExecutionPolicy Bypass -File cia_build/patch_version.ps1

ls -la cia_build/Mail3DS.cia
