param(
    [string]$ObsLogDir = "C:\Users\mpent\AppData\Roaming\obs-studio\logs",
    [string]$RequestId = "",
    [switch]$RequirePageReady,
    [switch]$RequireBridgeAssets
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ObsLogDir)) {
    throw "OBS log dir not found: $ObsLogDir"
}

$logs = Get-ChildItem -LiteralPath $ObsLogDir -File |
    Sort-Object LastWriteTime -Descending
if (-not $logs) {
    throw "No OBS logs found in: $ObsLogDir"
}
$log = $null
$content = $null
foreach ($candidate in $logs) {
    $candidateContent = Get-Content -LiteralPath $candidate.FullName
    $hasOnlyCrashNotice = $candidateContent.Count -le 3 -and
        (($candidateContent | Select-String -Pattern "Crash or unclean shutdown detected").Count -gt 0)
    if ($hasOnlyCrashNotice) {
        continue
    }
    $log = $candidate
    $content = $candidateContent
    break
}
if (-not $log) {
    throw "No usable OBS log found (latest files are crash-recovery stubs)"
}

function Assert-LogContains {
    param(
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $matched = $content | Select-String -Pattern $Pattern
    if (-not $matched) {
        throw "Missing log evidence: $Label ($Pattern)"
    }
    return $matched
}

Write-Host "Validating OBS log: $($log.FullName)"

$moduleLoad = Assert-LogContains -Pattern "\[aegis-obs-shim\] module load" -Label "plugin module load"
$ipcConnected = Assert-LogContains -Pattern "\[aegis-obs-shim\] ipc pipe state: connected" -Label "IPC connected"

if ($RequireBridgeAssets) {
    $null = Assert-LogContains -Pattern "bridge assets loaded" -Label "bridge assets load"
}
if ($RequirePageReady) {
    $null = Assert-LogContains -Pattern "CEF page ready" -Label "CEF page ready"
}

if ($RequestId) {
    $queued = Assert-LogContains -Pattern ("dock action result: action_type=.*request_id=" + [regex]::Escape($RequestId) + " status=queued") -Label "queued action result"
    $terminal = Assert-LogContains -Pattern ("dock action result: action_type=.*request_id=" + [regex]::Escape($RequestId) + " status=(completed|failed|rejected)") -Label "terminal action result"
    Write-Host ("  RequestId: " + $RequestId)
    Write-Host ("  Queued:    " + $queued[-1].Line)
    Write-Host ("  Terminal:  " + $terminal[-1].Line)
}

Write-Host "Validation OK."
