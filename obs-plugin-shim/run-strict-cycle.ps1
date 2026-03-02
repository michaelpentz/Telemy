param(
    [string]$WorkspaceRoot = "E:\Code\telemyapp\telemy-v0.0.3",
    [string]$RepoRoot = "E:\Code\telemyapp",
    [string]$ObsRoot = "C:\Program Files (x86)\obs-studio",
    [int]$ValidateRetrySeconds = 30,
    [string]$SelfTestActionJson = "",
    [switch]$ValidateForbidCompletionTimeout,
    [switch]$ValidateAllowAfterTimestampFallback,
    [switch]$StopWhenDone
)

$ErrorActionPreference = "Stop"

$devCycle = Join-Path $WorkspaceRoot "obs-plugin-shim\dev-cycle.ps1"
if (-not (Test-Path -LiteralPath $devCycle)) {
    throw "dev-cycle script not found: $devCycle"
}
$stopScript = Join-Path $WorkspaceRoot "obs-plugin-shim\stop-dev-session.ps1"
if (-not (Test-Path -LiteralPath $stopScript)) {
    throw "stop-dev-session script not found: $stopScript"
}

$scriptFailed = $false
try {
    $cycleArgs = @{
        WorkspaceRoot        = $WorkspaceRoot
        RepoRoot             = $RepoRoot
        ObsRoot              = $ObsRoot
        ConfigureObsCef      = $true
        BuildDockApp         = $true
        SkipBuild            = $true
        SkipDeploy           = $true
        ValidationProfile    = "strict"
        ValidateRetrySeconds = $ValidateRetrySeconds
    }
    if ($SelfTestActionJson) { $cycleArgs.SelfTestActionJson = $SelfTestActionJson }
    if ($ValidateForbidCompletionTimeout) { $cycleArgs.ValidateForbidCompletionTimeout = $true }
    if ($ValidateAllowAfterTimestampFallback) { $cycleArgs.ValidateAllowAfterTimestampFallback = $true }
    & $devCycle @cycleArgs
} catch {
    $scriptFailed = $true
    throw
} finally {
    if ($StopWhenDone) {
        try {
            & $stopScript -ForceIfNeeded
        } catch {
            if (-not $scriptFailed) {
                throw
            }
            Write-Warning "Strict run failed and stop-dev-session also failed: $($_.Exception.Message)"
        }
    }
}
