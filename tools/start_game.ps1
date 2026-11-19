# start_game.ps1 —— 启动 Mayday Airways 并自动进入单人飞行局（Anomaly 实机测试用）
# 用法:
#   pwsh -NoProfile -File tools/start_game.ps1              # 启动游戏并进局
#   pwsh -NoProfile -File tools/start_game.ps1 -NoLaunch    # 游戏已开, 只执行点击流程
#   pwsh -NoProfile -File tools/start_game.ps1 -NoClick     # 启动并等 Runtime 就绪, 不执行点击
#
# 坐标全部按游戏窗口尺寸的比例计算, 分辨率/窗口化不影响定位。
[CmdletBinding()]
param(
    [switch]$NoLaunch,          # 不启动游戏, 仅对已开窗口执行点击流程
    [switch]$NoClick,           # 启动游戏并等 Runtime 就绪, 但不执行点击
    [int]$WindowWaitSec = 120,  # 等待游戏窗口出现的秒数
    [int]$ReadyTimeoutSec = 180, # 等待 Anomaly Runtime 就绪的秒数
    [string]$GameDir = ''       # 未指定时读取 MAYDAY_GAME_DIR, 再回退到本机默认安装路径
)

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'PowerShell 7+ (pwsh.exe) is required; invoke this script with pwsh -NoProfile -File.'
}

$ErrorActionPreference = 'Stop'
if (-not $GameDir) { $GameDir = $env:MAYDAY_GAME_DIR }
if (-not $GameDir) { $GameDir = 'D:\games\steam\steamapps\common\Mayday Airways Demo' }
$GameDir = [IO.Path]::GetFullPath($GameDir)
$LogPath = Join-Path $GameDir 'Anomaly\logs\anomaly-runtime.log'
# 本次就绪判定只读取启动后的新增日志, 避免命中上一次运行的 confirmed
$LogOffset = 0

# ---- Win32 API ----
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class Win {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
}
"@

[void][Win]::SetProcessDPIAware()

function Get-GameWindow {
    # 注意: 传 $null 会被封送为空字符串 "" 导致匹配失败, 必须用 [NullString]::Value 表示 NULL
    $h = [Win]::FindWindow('UnityWndClass', [NullString]::Value)
    if ($h -eq [IntPtr]::Zero) { return $null }
    return $h
}

# 按窗口比例点击 (rx/ry 为 0~1 的相对坐标)
function Invoke-Click([double]$rx, [double]$ry) {
    $h = Get-GameWindow
    if ($null -eq $h) { throw '未找到游戏窗口 (UnityWndClass)' }
    $r = New-Object 'Win+RECT'
    [void][Win]::GetWindowRect($h, [ref]$r)
    $x = [int]($r.L + ($r.R - $r.L) * $rx)
    $y = [int]($r.T + ($r.B - $r.T) * $ry)
    [void][Win]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 350
    [void][Win]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 180
    [Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
    [Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
    Start-Sleep -Milliseconds 250
    Write-Host ("点击 ({0},{1})" -f $x, $y)
}

# 等待 Anomaly Runtime 初始化完成（运行日志出现 runtime_health=confirmed）
function Wait-RuntimeReady {
    Write-Host '等待 Anomaly Runtime 就绪 (读日志)...'
    $deadline = (Get-Date).AddSeconds($ReadyTimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $LogPath) {
            $text = ''
            try {
                # 运行中的日志可能被短暂占用, 打开失败时下一轮重试
                $stream = [IO.File]::Open($LogPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
                try {
                    if ($LogOffset -gt 0 -and $LogOffset -le $stream.Length) {
                        [void]$stream.Seek($LogOffset, [IO.SeekOrigin]::Begin)
                    }
                    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8)
                    try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
                } finally { $stream.Dispose() }
            } catch [IO.IOException] { }
            if ($text -match 'runtime_health=confirmed') {
                Write-Host 'Anomaly Runtime 已就绪, 游戏初始化完成'
                return $true
            }
        }
        Start-Sleep -Seconds 3
    }
    return $false
}

# ---- 启动游戏 ----
if (-not $NoLaunch) {
    if ($null -ne (Get-GameWindow)) {
        Write-Host '游戏已在运行, 跳过启动'
    } else {
        if (Test-Path -LiteralPath $LogPath) { $LogOffset = (Get-Item -LiteralPath $LogPath).Length }
        Write-Host '启动游戏...'
        Start-Process (Join-Path $GameDir 'MaydayAirways.exe') -WorkingDirectory $GameDir

        # 等待 Unity 窗口出现
        $deadline = (Get-Date).AddSeconds($WindowWaitSec)
        $h = $null
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 2
            $h = Get-GameWindow
            if ($null -ne $h) { break }
        }
        if ($null -eq $h) { throw "$WindowWaitSec 秒内未检测到游戏窗口" }
        Write-Host '游戏窗口已出现'
    }
} else {
    if ($null -eq (Get-GameWindow)) { throw '未找到游戏窗口, 无法执行点击流程' }
}

# 等主菜单就绪 (Runtime 就绪后稍等 UI 动画落位; Discord 弹窗在启动后约 2 秒弹出, 需等它出现再点)
if (-not (Wait-RuntimeReady)) { throw "$ReadyTimeoutSec 秒内未等到 Anomaly Runtime 就绪, 请检查 Anomaly\logs\anomaly-runtime.log" }
Start-Sleep -Seconds 5

# ---- 固定流程: 关 Discord 弹窗 → 开始游戏 → 单人飞行 ----
if ($NoClick) {
    Write-Host '已停在主菜单 (-NoClick), 可截图验证 UI 后手动执行 -NoLaunch 流程'
    exit 0
}

Write-Host '关闭 Discord 弹窗 (点两次兜底, 第二次落空无害)...'
Invoke-Click 0.7164 0.4100     # 弹窗右上角 X
Start-Sleep -Seconds 2
Invoke-Click 0.7164 0.4100     # 二次保险 (弹窗已关时点到背景, 无副作用)
Start-Sleep -Seconds 1

Write-Host '点击 开始游戏...'
Invoke-Click 0.1328 0.4138
Start-Sleep -Seconds 2

Write-Host '点击 单人飞行...'
Invoke-Click 0.6531 0.3113

Write-Host '已选择单人飞行, 等待进局 (约 10 秒)...'
Start-Sleep -Seconds 10
Write-Host '完成。'
