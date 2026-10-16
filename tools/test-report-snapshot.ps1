# Offline tests: no ADB or headset access.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'collect-report.ps1')
$script:BundleRoot = Join-Path ([IO.Path]::GetTempPath()) ('rdrvr-report-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $script:BundleRoot | Out-Null
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Start-Sleep { param($Milliseconds) }
function Invoke-Adb {
    param([string[]]$Arguments, [int]$TimeoutSeconds)
    $script:Calls.Add(($Arguments -join ' '))
    if ($Arguments[0] -eq 'push') {
        $script:Token = [IO.File]::ReadAllText($Arguments[1])
        Assert ($script:Token -cmatch '^[a-f0-9]{32}$') 'request must be exactly 32 ASCII bytes'
        return [pscustomobject]@{ExitCode = $(if ($script:Mode -eq 'push-failed') {1} else {0}); Text = ''}
    }
    if ($Arguments.Count -gt 1 -and $Arguments[1] -eq 'cat') {
        $script:Reads++
        $token = if ($script:Mode -eq 'stale' -or $script:Reads -eq 1) {'old'} else {$script:Token}
        $pidValue = if ($script:Mode -eq 'restarted') {99} else {42}
        return [pscustomobject]@{ExitCode = 0; Text = ('{"snapshotToken":"' + $token + '","reportPid":' + $pidValue + ',"vr":{"weapon":{"fire":{"triggerEdges":8}}}}')}
    }
    return [pscustomobject]@{ExitCode = 0; Text = ''}
}
foreach ($case in @('fresh', 'stale', 'restarted', 'push-failed')) {
    $script:Mode = $case
    $script:Reads = 0
    $script:Calls = New-Object 'System.Collections.Generic.List[string]'
    $script:Warnings.Clear()
    $result = Request-TraceSnapshot -ExpectedPid '42'
    if ($case -eq 'fresh') {
        Assert ($null -ne $result) 'fresh snapshot should succeed'
        Assert ((Get-ReportField $result 'vr.weapon.fire.triggerEdges') -eq '8') 'retain gameplay counters'
        Assert ($script:Reads -eq 2) 'ignore old file until the matching token arrives'
        Assert ($script:Warnings.Count -eq 0) 'fresh snapshot should not warn'
        Assert (@($script:Calls | Where-Object {$_ -match '^shell touch .+ && mv '}).Count -eq 1) 'publish with device timestamp atomically'
    }
    else {
        Assert ([string]::IsNullOrWhiteSpace($result)) "$case must not claim a fresh snapshot"
        Assert ($script:Warnings.Count -eq 1) "$case must explain the missing snapshot"
    }
    Assert ($script:Reads -le 8) 'polling must be bounded'
    Assert (-not (Test-Path -LiteralPath (Join-Path $script:BundleRoot 'snapshot-request.tmp'))) 'local request cleanup'
    Assert (@($script:Calls | Where-Object {$_ -notmatch '^(push |shell (cat |touch |rm -f ))'}).Count -eq 0) 'only snapshot transport commands are allowed'
}
Write-Host 'Report snapshot tests passed (fresh, stale, restarted, transport failure).'
