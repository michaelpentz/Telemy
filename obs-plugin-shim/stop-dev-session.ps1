param(
    [switch]$ForceStopObs,
    [switch]$ForceStopCore,
    [switch]$ForceIfNeeded,
    [int]$ObsGracefulTimeoutSeconds = 20
)

$ErrorActionPreference = "Stop"

function Stop-ObsProcess {
    param(
        [switch]$Force,
        [int]$TimeoutSeconds = 8
    )
    $obs = Get-Process obs64 -ErrorAction SilentlyContinue
    if (-not $obs) {
        return
    }

    if ($Force) {
        $obs | Stop-Process -Force -ErrorAction SilentlyContinue
        return
    }

    foreach ($p in $obs) {
        try {
            [void]$p.CloseMainWindow()
        } catch {
            # ignore and evaluate below
        }
    }
    Start-Sleep -Milliseconds 300
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Process obs64 -ErrorAction SilentlyContinue) -and ((Get-Date) -lt $deadline)) {
        Start-Sleep -Milliseconds 300
    }
}

Write-Host "Stopping dev session processes..."

Stop-ObsProcess -Force:$ForceStopObs -TimeoutSeconds $ObsGracefulTimeoutSeconds
if ((Get-Process obs64 -ErrorAction SilentlyContinue) -and (-not $ForceStopObs)) {
    if ($ForceIfNeeded) {
        Write-Warning "OBS did not exit in graceful timeout; forcing stop."
        Stop-ObsProcess -Force
    } else {
        throw "OBS is still running after graceful stop. Re-run with -ForceStopObs or -ForceIfNeeded."
    }
}

$core = Get-Process obs-telemetry-bridge -ErrorAction SilentlyContinue
if ($core) {
    if ($ForceStopCore) {
        $core | Stop-Process -Force -ErrorAction SilentlyContinue
    } else {
        $core | Stop-Process -ErrorAction SilentlyContinue
    }
}
if ((Get-Process obs-telemetry-bridge -ErrorAction SilentlyContinue) -and (-not $ForceStopCore)) {
    if ($ForceIfNeeded) {
        Write-Warning "obs-telemetry-bridge did not exit; forcing stop."
        Get-Process obs-telemetry-bridge -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    } else {
        throw "obs-telemetry-bridge is still running. Re-run with -ForceStopCore or -ForceIfNeeded."
    }
}

Write-Host "Dev session stopped."
