# 把 DepotDownloader 下载的 CS:GO 2019 资源打成鸿蒙启动页可导入的 zip
# 用法: powershell -File pack-resource-zip.ps1 <DepotDownloader输出目录> [输出zip]
# 依赖: 系统自带 Compress-Archive（大目录较慢但零依赖）；或自行换 7z
param(
    [Parameter(Mandatory=$true)][string]$SourceDir,
    [string]$OutZip = "E:\csgo\csgo-2019-resources.zip"
)

$csgo = Join-Path $SourceDir "csgo"
$platform = Join-Path $SourceDir "platform"
if (-not (Test-Path $csgo)) { Write-Error "缺少 $csgo"; exit 1 }
if (-not (Test-Path $platform)) { Write-Error "缺少 $platform"; exit 1 }

$stage = "$env:TEMP\csgo-zip-stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item $csgo "$stage\csgo" -Recurse
Copy-Item $platform "$stage\platform" -Recurse

Write-Host "压缩中（约几个 GB，耐心等待）..."
Compress-Archive -Path "$stage\csgo", "$stage\platform" -DestinationPath $OutZip -Force
Remove-Item $stage -Recurse -Force
$sizeGb = (Get-Item $OutZip).Length / 1GB
Write-Host ("完成: {0} ({1:N2} GB)" -f $OutZip, $sizeGb)
