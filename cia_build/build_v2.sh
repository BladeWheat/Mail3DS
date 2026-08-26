#!/bin/bash
set -e
cd /d/3ds_email
MR=/c/devkitPro/tools/bin/makerom.exe
ROMFS_WIN="D:\\3ds_email\\romfs"

rm -f cia_build/Mail3DS.cia
# -ignoresign: 未签名自制CIA必须带，否则FBI安装报 0xD8E0806A 证书签名/哈希校验失败
"$MR" -f cia -target t -exefslogo -ignoresign \
  -o cia_build/Mail3DS.cia \
  -elf 3ds_email.elf \
  -rsf cia_build/mail3ds_v2.rsf \
  -icon cia_build/mail3ds.smdh \
  -banner cia_build/banner.bnr \
  -DAPP_ROMFS="$ROMFS_WIN" \
  -major 1 -minor 0 -micro 0 2>&1
echo "exit=$?"
ls -la cia_build/Mail3DS.cia
