# patch_smdh_title.ps1
# 作用：bannertool 在 Windows 下传入中文标题会崩溃，
#       所以先用 ASCII 长标题生成 SMDH，再用本脚本把 12 个语言槽位的
#       长标题二进制改写为 UTF-16LE 中文。
# 用法：powershell -ExecutionPolicy Bypass -File patch_smdh_title.ps1 <smdh文件> <中文长标题>
param(
    [Parameter(Mandatory=$true)][string]$SmdhPath,
    [Parameter(Mandatory=$true)][string]$LongTitle
)

$b = [System.IO.File]::ReadAllBytes($SmdhPath)
if ([System.Text.Encoding]::ASCII.GetString($b, 0, 4) -ne "SMDH") {
    Write-Error "不是有效的 SMDH 文件: $SmdhPath"; exit 1
}

$utf16 = [System.Text.Encoding]::Unicode.GetBytes($LongTitle)
if ($utf16.Length -gt 0x100) {
    Write-Error "长标题超过 128 个 UTF-16 字符上限"; exit 1
}

# SMDH 标题区从 0x08 开始；每个语言槽 0x200 字节；
# 槽内：短标题@0x00(0x80)、长标题@0x80(0x100)、发布者@0x180(0x80)
# 长标题槽 i 偏移 = 0x08 + i*0x200 + 0x80 = 0x88 + i*0x200
for ($i = 0; $i -lt 12; $i++) {
    $off = 0x88 + $i * 0x200
    for ($k = 0; $k -lt 0x100; $k++) { $b[$off + $k] = 0 }   # 先清零
    [Array]::Copy($utf16, 0, $b, $off, $utf16.Length)          # 写入 UTF-16LE
}

[System.IO.File]::WriteAllBytes($SmdhPath, $b)
Write-Output ("已写入中文长标题: " + $LongTitle)
