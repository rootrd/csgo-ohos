# CS:GO 鸿蒙夜间自动推进哨兵
# 用户解锁手机的瞬间自动：启动应用 → 采集日志 → 若进程死亡自动重启再采集
# 全部日志存 E:\csgo\night-logs\，早上分析用
$ErrorActionPreference = "Continue"
$hdc = "C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\26.0.0\toolchains\hdc.exe"
$hap = "E:\csgo\hap\entry\build\default\outputs\default\entry-default-signed.hap"
$out = "E:\csgo\night-logs"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$deadline = (Get-Date).AddHours(9)

function Log($msg) {
    $stamp = Get-Date -Format "HH:mm:ss"
    "[$stamp] $msg" | Tee-Object -FilePath "$out\sentinel.log" -Append
}

Log "夜间哨兵启动，运行至 $deadline"

while ((Get-Date) -lt $deadline) {
    & $hdc shell "power-shell wakeup" 2>&1 | Out-Null

    $r = (& $hdc shell "aa start -a EntryAbility -b com.csgosource.ohos" 2>&1 | Out-String)
    if ($r -match "successfully") {
        $ts = Get-Date -Format "HHmmss"
        Log "应用已启动，等待 150 秒采集..."
        Start-Sleep -Seconds 150

        & $hdc shell "cat /data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo/logs/stdio.log" 2>&1 | Out-File "$out\stdio-$ts.log"
        & $hdc shell "cat /data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo/logs/launcher.log" 2>&1 | Out-File "$out\launcher-$ts.log"
        & $hdc shell "cat /data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo/logs/appspawn_d3d9.log" 2>&1 | Out-File "$out\dxvk-$ts.log"
        & $hdc shell "hilog -x" 2>&1 | Select-String -Pattern "A0C001|SDL/XCOMP|CSGOHOS" | Out-File "$out\hilog-$ts.log"

        $pid2 = (& $hdc shell "pidof com.csgosource.ohos" 2>&1 | Out-String).Trim()
        if ($pid2 -eq "") {
            Log "进程已退出，10 秒后自动重启再采集"
            Start-Sleep -Seconds 10
        } else {
            Log "进程存活 (pid=$pid2)，保持运行并每 60 秒复查"
            Start-Sleep -Seconds 60
        }
    } else {
        Log "屏幕锁定中，30 秒后重试（解锁后自动继续）..."
        Start-Sleep -Seconds 30
    }
}
Log "哨兵结束"
