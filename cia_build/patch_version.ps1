<<<<<<< HEAD
=======
<<<<<<< HEAD
=======
<<<<<<< HEAD
>>>>>>> cb02601b9040b431421dc9202cb8064d869f1903
>>>>>>> 83d91e5ea2997eb8a09b02ef775c4d1aa70edec9
﻿# 将 Mail3DS.cia 的 TMD Title Version 补丁为 1040 (1.1.0)
# 3DS版本号编码：主版本*1024 + 次版本*16 + 修订号
#   1.1.0 = 1*1024 + 1*16 + 0 = 1040 = 0x0410
# 原因：本机 makerom v0.19.0 会把次版本位清零，命令行无法直接生成非0次版本。
<<<<<<< HEAD
=======
<<<<<<< HEAD
=======
=======
﻿# 将 Mail3DS.cia 的 TMD Title Version 补丁为 1088 (1.1.0)
# 原因：本机 makerom v0.19.0 会把次版本位(bit6-9)清零，命令行无法直接生成非0次版本。
>>>>>>> b539909e02e0db3af02993044d9b3d26125d2552
>>>>>>> cb02601b9040b431421dc9202cb8064d869f1903
>>>>>>> 83d91e5ea2997eb8a09b02ef775c4d1aa70edec9
# 未签名自制CIA仅改TMD版本字段，不影响CXI内容哈希，FBI(CFW)可正常安装。
$p = "D:\3ds_email\cia_build\Mail3DS.cia"
$b = [System.IO.File]::ReadAllBytes($p)

# TitleID 0004000003A17000 的大端字节
$pat = @(0x00,0x04,0x00,0x00,0x03,0xA1,0x70,0x00)
$last = -1
for ($i = 0x2000; $i -lt 0x8000; $i++) {
    $m = $true
    for ($j = 0; $j -lt 8; $j++) { if ($b[$i+$j] -ne $pat[$j]) { $m = $false; break } }
    if ($m) { $last = $i }
}
if ($last -lt 0) { Write-Error "TitleID not found, abort."; exit 1 }

# TMD 中 TitleVersion 位于 TitleID(0x18C) 之后 0x50 字节，即偏移 0x1DC
$vo = $last + 0x50
$cur = [int]$b[$vo]*256 + [int]$b[$vo+1]
<<<<<<< HEAD
=======
<<<<<<< HEAD
=======
<<<<<<< HEAD
>>>>>>> cb02601b9040b431421dc9202cb8064d869f1903
>>>>>>> 83d91e5ea2997eb8a09b02ef775c4d1aa70edec9
if ($cur -eq 1040) { Write-Host "Already 1.1.0 (1040), no change."; exit 0 }

# 目标 1040 = 0x0410，大端写回
$b[$vo]   = 0x04
$b[$vo+1] = 0x10
[System.IO.File]::WriteAllBytes($p, $b)
Write-Host ("Patched TMD version {0} -> 1040 (1.1.0)" -f $cur)
<<<<<<< HEAD
=======
<<<<<<< HEAD
=======
=======
if ($cur -eq 1088) { Write-Host "Already 1.1.0 (1088), no change."; exit 0 }

# 目标 1088 = 0x0440，大端写回
$b[$vo]   = 0x04
$b[$vo+1] = 0x40
[System.IO.File]::WriteAllBytes($p, $b)
Write-Host ("Patched TMD version {0} -> 1088 (1.1.0)" -f $cur)
>>>>>>> b539909e02e0db3af02993044d9b3d26125d2552
>>>>>>> cb02601b9040b431421dc9202cb8064d869f1903
>>>>>>> 83d91e5ea2997eb8a09b02ef775c4d1aa70edec9
