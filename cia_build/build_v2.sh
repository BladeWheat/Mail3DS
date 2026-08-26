#!/bin/bash
set -e
cd /d/3ds_email
MR=/c/devkitPro/tools/bin/makerom.exe
ROMFS_WIN="D:\\3ds_email\\romfs"

rm -f cia_build/Mail3DS.cia
# -ignoresign: 未签名自制CIA必须带，否则FBI安装报 0xD8E0806A 证书签名/哈希校验失败
<<<<<<< HEAD
# 注意：本机 makerom v0.19.0 有bug，-minor / -ver 的次版本位会被清零，
#       命令行无法直接写出 1.1.0(1040)，先生成再用 patch_version.ps1 补丁TMD版本字段。
=======
<<<<<<< HEAD
# 注意：本机 makerom v0.19.0 有bug，-minor / -ver 的次版本位会被清零，
#       命令行无法直接写出 1.1.0(1040)，先生成再用 patch_version.ps1 补丁TMD版本字段。
=======
# 注意：本机 makerom v0.19.0 有bug，-minor / -ver 的次版本位(bit6-9)会被清零，
#       命令行无法直接写出 1.1.0(1088)，先生成再用 patch_version.ps1 补丁TMD版本字段。
>>>>>>> b539909e02e0db3af02993044d9b3d26125d2552
>>>>>>> 0c9a3e6cf4fea35295c939f7fa5a163fcc8c4cf0
"$MR" -f cia -target t -exefslogo -ignoresign \
  -o cia_build/Mail3DS.cia \
  -elf 3ds_email.elf \
  -rsf cia_build/mail3ds_v2.rsf \
  -icon cia_build/mail3ds.smdh \
  -banner cia_build/banner.bnr \
<<<<<<< HEAD
=======
<<<<<<< HEAD
>>>>>>> 0c9a3e6cf4fea35295c939f7fa5a163fcc8c4cf0
  -DAPP_ROMFS="$ROMFS_WIN" 2>&1

# 补丁 TMD Title Version：1024(1.0.0,0x0400) -> 1040(1.1.0,0x0410)
# 3DS版本号=主*1024+次*16+修订；makerom无法直接写非0次版本，故二进制补丁
powershell -ExecutionPolicy Bypass -File cia_build/patch_version.ps1

<<<<<<< HEAD
=======
=======
<<<<<<< HEAD
  -DAPP_ROMFS="$ROMFS_WIN" 2>&1

# 补丁 TMD Title Version：1024(1.0.0, 0x0400) -> 1088(1.1.0, 0x0440)
powershell -ExecutionPolicy Bypass -File cia_build/patch_version.ps1

=======
  -DAPP_ROMFS="$ROMFS_WIN" \
  -major 1 -minor 1 -micro 0 2>&1
echo "exit=$?"
>>>>>>> e6e126917f750c2e0f6d4511b56a950a3c33e637
>>>>>>> b539909e02e0db3af02993044d9b3d26125d2552
>>>>>>> 0c9a3e6cf4fea35295c939f7fa5a163fcc8c4cf0
ls -la cia_build/Mail3DS.cia
