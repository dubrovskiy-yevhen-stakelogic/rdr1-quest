[CmdletBinding()]
param(
    [string]$Adb,
    [string]$AndroidSdk,
    [string]$WorkDir = 'C:\RDRVRBuild',
    [string]$Serial,
    [switch]$NonInteractive
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$script:PackageName = 'com.rockstargames.rdr'
$script:RemoteSettingsRoot = "/sdcard/Android/data/$($script:PackageName)/files/rdrvr"
$script:SelectedSerial = $null
$script:AdbExe = $null
$script:Utf8 = New-Object System.Text.UTF8Encoding($false)
$script:Commands = New-Object 'System.Collections.Generic.List[object]'
$script:Warnings = New-Object 'System.Collections.Generic.List[string]'
$script:PlatformTools = @{
    FileName = 'platform-tools_r37.0.1-win.zip'
    Uri = 'https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip'
    Sha256 = '45F4D63113E895EBDE0C90F194099A4676B6AC653BD28D54314A9E022BBC1A99'
}
$script:ReportFiles = @('vulkan-trace.json', 'flight.json', 'graphics.cfg', 'vr.cfg', 'weapons.cfg')
$script:MaxSessionFiles = 6
$script:DeviceProperties = @(
    'ro.product.model', 'ro.product.device', 'ro.product.manufacturer',
    'ro.build.fingerprint', 'ro.build.display.id', 'ro.build.version.incremental'
)
# Only these log tags are kept; other apps' messages can contain account data.
$script:LogTagPattern = '^(RdrVr\w*|VrApi|OpenXR|DEBUG|libc|AndroidRuntime|SystemUXController)$'

function Write-Utf8 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text
    )
    [System.IO.File]::WriteAllText($Path, $Text, $script:Utf8)
}

function ConvertTo-NativeArgument {
    param([AllowEmptyString()][string]$Value)
    # Windows CommandLineToArgvW quoting, including trailing backslashes.
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    return '"' + [regex]::Replace(
        [regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [int]$TimeoutSeconds = 60
    )
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo.FileName = $FilePath
    $process.StartInfo.Arguments = ($Arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.StandardOutputEncoding = $script:Utf8
    $process.StartInfo.StandardErrorEncoding = $script:Utf8
    $code = -1
    $stdout = ''
    $stderr = ''
    try {
        $null = $process.Start()
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            # Only the PC-side adb client is terminated; nothing on the headset is stopped.
            $process.Kill()
            $process.WaitForExit()
            $stderr = "Timed out after $TimeoutSeconds seconds. "
        }
        else {
            $code = $process.ExitCode
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr += $stderrTask.GetAwaiter().GetResult()
    }
    catch {
        $stderr += $_.Exception.Message
    }
    finally {
        $process.Dispose()
    }
    return [pscustomobject]@{
        ExitCode = $code
        Lines = @($stdout -split '\r?\n' | Where-Object { $_.Length -gt 0 })
        Text = $stdout
        Error = $stderr.Trim()
    }
}

function Test-AdbCandidate {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $probe = Invoke-Native -FilePath $Path -Arguments @('version') -TimeoutSeconds 30
    return $probe.ExitCode -eq 0 -and $probe.Text -match 'Android Debug Bridge'
}

function Get-PinnedPlatformTools {
    param([Parameter(Mandatory = $true)][string]$ResolvedWorkDir)
    $downloads = Join-Path $ResolvedWorkDir '.downloads'
    $toolRoot = Join-Path (Join-Path $ResolvedWorkDir '.tools') 'platform-tools-37.0.1'
    $adbPath = Join-Path $toolRoot 'platform-tools\adb.exe'
    New-Item -ItemType Directory -Force -Path $downloads | Out-Null
    $archive = Join-Path $downloads $script:PlatformTools.FileName
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $script:PlatformTools.Sha256) {
        $temporary = "$archive.download-$([Guid]::NewGuid().ToString('N'))"
        Write-Host 'Downloading Google Android SDK Platform Tools...'
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $script:PlatformTools.Uri -OutFile $temporary
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash
            if ($hash -ne $script:PlatformTools.Sha256) {
                throw "Platform Tools SHA256 mismatch. Expected $($script:PlatformTools.Sha256), got $hash"
            }
            if (Test-Path -LiteralPath $archive) {
                Move-Item -LiteralPath $archive -Destination "$archive.invalid-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
            }
            Move-Item -LiteralPath $temporary -Destination $archive
        }
        finally {
            if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary -Force }
        }
    }
    $temporaryRoot = "$toolRoot.extract-$([Guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::ExtractToDirectory($archive, $temporaryRoot)
        if (-not (Test-Path -LiteralPath (Join-Path $temporaryRoot 'platform-tools\adb.exe') -PathType Leaf)) {
            throw 'The Platform Tools archive does not contain adb.exe.'
        }
        if (Test-Path -LiteralPath $toolRoot) {
            Move-Item -LiteralPath $toolRoot -Destination "$toolRoot.invalid-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
        }
        Move-Item -LiteralPath $temporaryRoot -Destination $toolRoot
    }
    finally {
        if (Test-Path -LiteralPath $temporaryRoot) { Remove-Item -LiteralPath $temporaryRoot -Recurse -Force }
    }
    return $adbPath
}

function Resolve-Adb {
    param([Parameter(Mandatory = $true)][string]$ResolvedWorkDir)
    if (-not [string]::IsNullOrWhiteSpace($Adb)) {
        $requested = [System.IO.Path]::GetFullPath($Adb.Trim().Trim('"').Trim("'"))
        if (-not (Test-AdbCandidate -Path $requested)) { throw "-Adb is not a working ADB executable: $requested" }
        return $requested
    }
    $candidates = New-Object 'System.Collections.Generic.List[string]'
    $candidates.Add((Join-Path $ResolvedWorkDir '.tools\platform-tools-37.0.1\platform-tools\adb.exe'))
    foreach ($sdk in @($AndroidSdk, $env:ANDROID_SDK_ROOT, $env:ANDROID_HOME)) {
        if (-not [string]::IsNullOrWhiteSpace([string]$sdk)) {
            $candidates.Add((Join-Path $sdk 'platform-tools\adb.exe'))
        }
    }
    $candidates.Add((Join-Path $ResolvedWorkDir '.android-sdk\platform-tools\adb.exe'))
    $localAppData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
    if (-not [string]::IsNullOrWhiteSpace($localAppData)) {
        $candidates.Add((Join-Path $localAppData 'Android\Sdk\platform-tools\adb.exe'))
    }
    $pathAdb = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($null -ne $pathAdb) { $candidates.Add($pathAdb.Source) }
    foreach ($candidate in $candidates) {
        if (Test-AdbCandidate -Path $candidate) { return (Get-Item -LiteralPath $candidate).FullName }
    }
    $downloaded = Get-PinnedPlatformTools -ResolvedWorkDir $ResolvedWorkDir
    if (-not (Test-AdbCandidate -Path $downloaded)) { throw "The downloaded ADB does not run: $downloaded" }
    return $downloaded
}

function Invoke-Adb {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$WithoutSerial,
        [int]$TimeoutSeconds = 60
    )
    $allArguments = @()
    if (-not $WithoutSerial.IsPresent) {
        if ([string]::IsNullOrWhiteSpace([string]$script:SelectedSerial)) { throw 'No Quest has been selected.' }
        $allArguments += @('-s', $script:SelectedSerial)
    }
    $allArguments += $Arguments
    $started = [DateTime]::UtcNow
    $result = Invoke-Native -FilePath $script:AdbExe -Arguments $allArguments -TimeoutSeconds $TimeoutSeconds
    $script:Commands.Add([pscustomobject]@{
        startedUtc = $started.ToString('o')
        command = 'adb ' + ($Arguments -join ' ')
        exitCode = $result.ExitCode
        stderr = $result.Error
    })
    return $result
}

function Select-Quest {
    Invoke-Adb -Arguments @('start-server') -WithoutSerial | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    $guidanceShown = $false
    while ($true) {
        $result = Invoke-Adb -Arguments @('devices', '-l') -WithoutSerial
        $devices = @()
        foreach ($line in $result.Lines) {
            if ($line -match '^(\S+)\s+(device|unauthorized|offline|no permissions|authorizing|connecting)\b\s*(.*)$') {
                $devices += [pscustomobject]@{ Serial = $Matches[1]; State = $Matches[2]; Detail = $Matches[3] }
            }
        }
        $ready = @($devices | Where-Object { $_.State -eq 'device' })
        if (-not [string]::IsNullOrWhiteSpace($Serial)) { $ready = @($ready | Where-Object { $_.Serial -eq $Serial }) }
        if ($ready.Count -gt 0) { break }
        if (-not $guidanceShown) {
            Write-Host 'Waiting for the Quest headset (up to 1 minute): connect it with a USB-C data cable, put it on and accept Allow USB debugging.' -ForegroundColor Yellow
            $guidanceShown = $true
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            if (@($devices | Where-Object { $_.State -eq 'unauthorized' }).Count -gt 0) {
                throw 'The headset is connected but USB debugging is not allowed yet. Put on the headset and accept Allow USB debugging.'
            }
            if (@($devices | Where-Object { $_.State -eq 'offline' }).Count -gt 0) {
                throw 'The headset connection is offline. Reconnect the USB cable and try again.'
            }
            if (-not [string]::IsNullOrWhiteSpace($Serial)) { throw "The requested device $Serial was not found by ADB." }
            throw 'No Quest headset was found over ADB. Check Developer Mode, the USB-C data cable and the Allow USB debugging prompt.'
        }
        Start-Sleep -Seconds 2
    }
    if ($ready.Count -eq 1) {
        $script:SelectedSerial = $ready[0].Serial
    }
    elseif ($NonInteractive.IsPresent) {
        throw 'More than one ADB device is connected. Pass -Serial with -NonInteractive.'
    }
    else {
        Write-Host 'Connected ADB devices:'
        for ($index = 0; $index -lt $ready.Count; $index++) {
            Write-Host "  [$($index + 1)] $($ready[$index].Serial) $($ready[$index].Detail)"
        }
        $selection = Read-Host 'Choose the Quest headset by number'
        [int]$number = 0
        if (-not [int]::TryParse($selection, [ref]$number) -or $number -lt 1 -or $number -gt $ready.Count) {
            throw 'Invalid device selection.'
        }
        $script:SelectedSerial = $ready[$number - 1].Serial
    }
    $model = ((Invoke-Adb -Arguments @('shell', 'getprop', 'ro.product.model')).Text).Trim()
    $manufacturer = ((Invoke-Adb -Arguments @('shell', 'getprop', 'ro.product.manufacturer')).Text).Trim()
    $identity = "$manufacturer $model".Trim()
    if ($identity -notmatch '(?i)quest|oculus|meta') {
        throw "The selected device does not identify itself as a Meta Quest: '$identity'."
    }
    Write-Host "Selected Quest: $identity"
    return $identity
}

function Save-Capture {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [scriptblock]$Filter,
        [int]$TimeoutSeconds = 60
    )
    Write-Host "Reading $Name ..."
    $result = Invoke-Adb -Arguments $Arguments -TimeoutSeconds $TimeoutSeconds
    $lines = @($result.Lines)
    if ($null -ne $Filter) { $lines = @($lines | Where-Object $Filter) }
    if ($result.ExitCode -ne 0) {
        $script:Warnings.Add("$Name is unavailable or incomplete (adb exit $($result.ExitCode)).")
    }
    Write-Utf8 -Path (Join-Path $script:BundleRoot $Name) -Text (($lines -join "`n") + "`n")
    return $result
}

function Test-KeptLogLine {
    param([Parameter(Mandatory = $true)][string]$Line)
    if ($Line -notmatch '^\S+\s+\S+\s+\d+\s+\d+\s+[VDIWEFS]\s+(.+?)\s*:\s(.*)$') { return $false }
    $tag = $Matches[1].Trim()
    $message = $Matches[2]
    if ($tag -match $script:LogTagPattern) { return $true }
    if ($tag -eq 'Vega' -and $message -match '\[INSIGHTS\] Mode=') { return $true }
    if ($tag -eq 'ActivityManager' -and $message -match 'ANR|com\.rockstargames\.rdr') { return $true }
    return $false
}

function Get-ReportField {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Json,
        [Parameter(Mandatory = $true)][string]$Path
    )
    # Reads the first scalar after the named parent keys; enough for a summary
    # without parsing reports that exceed the Windows PowerShell JSON limit.
    $position = 0
    $parts = $Path.Split('.')
    for ($index = 0; $index -lt $parts.Count; $index++) {
        $key = '"' + [regex]::Escape($parts[$index]) + '"\s*:\s*'
        if ($index -lt $parts.Count - 1) {
            $match = [regex]::new($key + '\{').Match($Json, $position)
            if (-not $match.Success) { return 'n/a' }
            $position = $match.Index + $match.Length
        }
        else {
            $match = [regex]::new($key + '("(?:[^"\\]|\\.)*"|-?[0-9][0-9.eE+-]*|true|false|null)').Match($Json, $position)
            if (-not $match.Success) { return 'n/a' }
            return $match.Groups[1].Value.Trim('"')
        }
    }
    return 'n/a'
}

function Request-TraceSnapshot {
    param([string]$ExpectedPid)
    $token = [Guid]::NewGuid().ToString('N')
    $local = Join-Path $script:BundleRoot 'snapshot-request.tmp'
    $remote = "$($script:RemoteSettingsRoot)/bench-trace-snapshot"
    $staging = "$remote.$token.tmp"
    try {
        Write-Utf8 -Path $local -Text $token
        $push = Invoke-Adb -Arguments @('push', $local, $staging) -TimeoutSeconds 10
        if ($push.ExitCode -ne 0) { throw 'Could not send the diagnostic request.' }
        # Use the device clock, then publish atomically: the loader rejects stale requests.
        $publish = Invoke-Adb -Arguments @('shell', "touch $staging && mv $staging $remote") -TimeoutSeconds 5
        if ($publish.ExitCode -ne 0) { throw 'Could not publish the diagnostic request.' }
        for ($attempt = 0; $attempt -lt 8; $attempt++) {
            Start-Sleep -Milliseconds 1000
            $result = Invoke-Adb -Arguments @('shell', 'cat', "$($script:RemoteSettingsRoot)/vulkan-trace.json") -TimeoutSeconds 5
            if ($result.ExitCode -eq 0 -and
                (Get-ReportField -Json $result.Text -Path 'snapshotToken') -ceq $token -and
                (Get-ReportField -Json $result.Text -Path 'reportPid') -eq $ExpectedPid) {
                return $result.Text
            }
        }
        throw 'No matching live snapshot arrived. Keep the game running and the headset awake, then collect again.'
    }
    catch { $script:Warnings.Add("Fresh gameplay snapshot unavailable: $($_.Exception.Message)"); return $null }
    finally {
        if (Test-Path -LiteralPath $local) { Remove-Item -LiteralPath $local -Force }
        # Remove only our staging file. The loader consumes/expires the published token.
        try { $null = Invoke-Adb -Arguments @('shell', 'rm', '-f', $staging) -TimeoutSeconds 5 }
        catch { $script:Warnings.Add('Could not clean up the temporary diagnostic request.') }
    }
}

if ($MyInvocation.InvocationName -eq '.') { return }

try {
    $resolvedWorkDir = [System.IO.Path]::GetFullPath($WorkDir.Trim().Trim('"').Trim("'"))
    $driveRoot = [System.IO.Path]::GetPathRoot($resolvedWorkDir)
    if ($resolvedWorkDir.TrimEnd('\') -eq $driveRoot.TrimEnd('\')) { throw 'WorkDir must be a dedicated folder, not a drive root.' }
    Write-Host 'Red Dead Redemption VR report collector. Requests a diagnostic snapshot; settings are unchanged and nothing is uploaded.' -ForegroundColor Cyan
    $script:AdbExe = Resolve-Adb -ResolvedWorkDir $resolvedWorkDir
    Write-Host "ADB: $($script:AdbExe)"
    $identity = Select-Quest

    $startedUtc = [DateTime]::UtcNow
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $reportsRoot = Join-Path $resolvedWorkDir 'reports'
    $script:BundleRoot = Join-Path $reportsRoot $stamp
    $filesRoot = Join-Path $script:BundleRoot 'files'
    New-Item -ItemType Directory -Force -Path $filesRoot | Out-Null
    $zipPath = Join-Path $reportsRoot "RDRVR-report-$stamp.zip"

    # The crash buffer is small and is overwritten quickly, so it is read first.
    $null = Save-Capture -Name 'logcat-crash.txt' -Arguments @('logcat', '-b', 'crash', '-d', '-v', 'threadtime')
    $null = Save-Capture -Name 'logcat-filtered.txt' -Arguments @('logcat', '-d', '-v', 'threadtime', '-t', '20000') `
        -Filter { Test-KeptLogLine -Line $_ } -TimeoutSeconds 120
    $null = Save-Capture -Name 'fire-diagnostics.txt' -Arguments @('logcat', '-d', '-v', 'threadtime', '-s', 'RdrVrFire:I', '*:S') `
        -TimeoutSeconds 120

    $package = Save-Capture -Name 'package.txt' -Arguments @('shell', 'dumpsys', 'package', $script:PackageName)
    $paths = Save-Capture -Name 'package-paths.txt' -Arguments @('shell', 'pm', 'path', $script:PackageName)
    $null = Save-Capture -Name 'battery.txt' -Arguments @('shell', 'dumpsys', 'battery')
    $null = Save-Capture -Name 'storage.txt' -Arguments @('shell', 'df', '-k', '/data')
    $properties = @()
    foreach ($name in $script:DeviceProperties) {
        $value = ((Invoke-Adb -Arguments @('shell', 'getprop', $name)).Text).Trim()
        $properties += "[$name]: [$value]"
    }
    Write-Utf8 -Path (Join-Path $script:BundleRoot 'device-properties.txt') -Text (($properties -join "`n") + "`n")
    $debugProperties = Save-Capture -Name 'debug-properties.txt' -Arguments @('shell', 'getprop') `
        -Filter { $_ -match '^\[debug\.rdrvr\.' }
    $debugCount = @($debugProperties.Lines | Where-Object { $_ -match '^\[debug\.rdrvr\.' }).Count
    $pidResult = Invoke-Adb -Arguments @('shell', 'pidof', $script:PackageName)
    $running = $pidResult.ExitCode -eq 0 -and $pidResult.Text -match '\d'
    $freshTrace = $null
    if ($running) {
        Write-Host 'Requesting current gameplay diagnostics (keep the game running)...'
        $freshTrace = Request-TraceSnapshot -ExpectedPid ([regex]::Match($pidResult.Text, '\d+').Value)
        $null = Save-Capture -Name 'memory.txt' -Arguments @('shell', 'dumpsys', 'meminfo', $script:PackageName)
    }
    else { $script:Warnings.Add('The game was not running. Saved reports may contain only startup state; reproduce the issue and collect before closing the game.') }

    $copied = New-Object 'System.Collections.Generic.List[object]'
    $missing = New-Object 'System.Collections.Generic.List[string]'
    $remoteFiles = New-Object 'System.Collections.Generic.List[string]'
    foreach ($name in $script:ReportFiles) { $remoteFiles.Add($name) }
    $sessions = Invoke-Adb -Arguments @('shell', 'ls', '-1', "$($script:RemoteSettingsRoot)/trace-runs")
    if ($sessions.ExitCode -eq 0) {
        # Only plain session report names are accepted; nothing else is pulled.
        $sessionNames = @($sessions.Lines | ForEach-Object { $_.Trim() } |
            Where-Object { $_ -cmatch '^session-[A-Za-z0-9_.-]+\.json$' } | Sort-Object -Descending |
            Select-Object -First $script:MaxSessionFiles)
        foreach ($name in $sessionNames) { $remoteFiles.Add("trace-runs/$name") }
    }
    foreach ($relative in $remoteFiles) {
        $remote = "$($script:RemoteSettingsRoot)/$relative"
        $exists = Invoke-Adb -Arguments @('shell', 'test', '-f', $remote)
        if ($exists.ExitCode -ne 0) { $missing.Add($relative); continue }
        Write-Host "Copying $relative ..."
        $destination = Join-Path $filesRoot ($relative.Replace('/', '\'))
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        if ($relative -eq 'vulkan-trace.json' -and -not [string]::IsNullOrWhiteSpace($freshTrace)) {
            Write-Utf8 -Path $destination -Text $freshTrace
            $pull = [pscustomobject]@{ ExitCode = 0 }
        }
        else { $pull = Invoke-Adb -Arguments @('pull', $remote, $destination) -TimeoutSeconds 120 }
        if ($pull.ExitCode -eq 0 -and (Test-Path -LiteralPath $destination -PathType Leaf)) {
            $copied.Add([pscustomobject]@{
                path = "files/$relative"
                bytes = (Get-Item -LiteralPath $destination).Length
                sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
            })
        }
        else {
            $script:Warnings.Add("Could not copy $relative (adb exit $($pull.ExitCode)).")
        }
    }

    $traceText = ''
    $tracePath = Join-Path $filesRoot 'vulkan-trace.json'
    if (Test-Path -LiteralPath $tracePath -PathType Leaf) {
        $traceText = [System.IO.File]::ReadAllText($tracePath)
    }
    $packageText = $package.Text
    $versionCode = [regex]::Match($packageText, '(?m)^\s*versionCode=(\d+)')
    $versionName = [regex]::Match($packageText, '(?m)^\s*versionName=(\S+)')
    $installed = $paths.ExitCode -eq 0 -and $paths.Text -match '(?m)^package:'
    $fields = @(
        'buildId', 'status', 'hashVerified', 'preflightVerified', 'writerError',
        'gameBootstrap.sessionState', 'lastViewFlags', 'nativeUi.state',
        'stereoReplay.driverSets.identity', 'graphics.persistenceOk', 'graphics.cfgVersion',
        'vr.weapon.fire.triggerEdges', 'vr.weapon.fire.forwarded', 'vr.weapon.fire.fired',
        'vr.weapon.fire.lastRefusal', 'vr.weapon.fire.lastRefusalAgeMs',
        'vr.weapon.diagnostics.total', 'gameBootstrap.nativeInput.filterRejected',
        'gameBootstrap.nativeInput.controlsRejected', 'gameBootstrap.nativeInput.delivery.total'
    )
    $summary = New-Object 'System.Collections.Generic.List[string]'
    $summary.Add('RED DEAD REDEMPTION VR REPORT')
    $summary.Add('')
    $summary.Add("Collected (UTC): $($startedUtc.ToString('o'))")
    $summary.Add("Headset: $identity")
    $summary.Add("Game installed: $installed")
    $summary.Add("Game versionName: $(if ($versionName.Success) { $versionName.Groups[1].Value } else { 'unknown' })")
    $summary.Add("Game versionCode: $(if ($versionCode.Success) { $versionCode.Groups[1].Value } else { 'unknown' })")
    $summary.Add("Game running during collection: $running")
    $summary.Add("Fresh gameplay snapshot confirmed: $(-not [string]::IsNullOrWhiteSpace($freshTrace))")
    $summary.Add('')
    if ($traceText.Length -gt 0) {
        $summary.Add('Report fields (vulkan-trace.json):')
        foreach ($field in $fields) { $summary.Add("  ${field}: $(Get-ReportField -Json $traceText -Path $field)") }
    }
    else {
        $summary.Add('vulkan-trace.json was not found. Start the game once, then collect again.')
    }
    $summary.Add('')
    $summary.Add('Shooting diagnostics: fire-diagnostics.txt and vr.weapon.diagnostics.events in vulkan-trace.json.')
    $summary.Add('Native pad delivery: gameBootstrap.nativeInput.delivery.events (post-update held/pressed buttons and selected controller).')
    $summary.Add('Reproduce several failed trigger presses, then collect while the game is still running.')
    $summary.Add('NO_ENGINE_FIRE means no projectile was observed within 700 ms; hint is context, not a proven cause.')
    $summary.Add("debug.rdrvr properties set: $debugCount")
    if ($debugCount -gt 0) {
        $summary.Add('  Debug properties change the behaviour of the mod. Restart the headset to clear them.')
    }
    $summary.Add("Copied files: $($copied.Count)")
    $summary.Add("Missing files: $(if ($missing.Count -gt 0) { $missing -join ', ' } else { 'none' })")
    $summary.Add("Collection warnings: $($script:Warnings.Count)")
    foreach ($warning in $script:Warnings) { $summary.Add("  - $warning") }
    $summary.Add('')
    $summary.Add('Logs are filtered to the mod, the VR runtime and crash tags. They are a snapshot of the')
    $summary.Add('retained log history and can contain earlier sessions. Review the files before sharing.')
    $summary.Add('Nothing was uploaded, and the game was neither started nor stopped.')
    Write-Utf8 -Path (Join-Path $script:BundleRoot 'summary.txt') -Text (($summary.ToArray() -join "`n") + "`n")

    $manifest = [ordered]@{
        format = 'rdrvr-report'
        formatVersion = 1
        collectedUtc = $startedUtc.ToString('o')
        package = $script:PackageName
        headset = $identity
        gameRunning = [bool]$running
        freshGameplaySnapshot = -not [string]::IsNullOrWhiteSpace($freshTrace)
        files = @($copied.ToArray())
        missingFiles = @($missing.ToArray())
        warnings = @($script:Warnings.ToArray())
        commands = @($script:Commands.ToArray())
        uploads = $false
    }
    Write-Utf8 -Path (Join-Path $script:BundleRoot 'manifest.json') -Text ($manifest | ConvertTo-Json -Depth 5)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $zipPath) { throw "The report archive already exists: $zipPath" }
    [System.IO.Compression.ZipFile]::CreateFromDirectory($script:BundleRoot, $zipPath)
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entryNames = @($archive.Entries | ForEach-Object { $_.FullName })
        if ($entryNames -notcontains 'summary.txt' -or $entryNames -notcontains 'manifest.json') {
            throw 'ZIP verification failed: the report summary is missing.'
        }
    }
    finally { $archive.Dispose() }

    Write-Host ''
    Write-Host "Report saved ($($script:Warnings.Count) collection warnings):" -ForegroundColor Green
    Write-Host "  $zipPath"
    Write-Host "  Folder: $($script:BundleRoot)"
    Write-Host 'Review the files before sharing them. Nothing was uploaded.'
    if (-not $NonInteractive.IsPresent) {
        try { Start-Process -FilePath 'explorer.exe' -ArgumentList @("/select,`"$zipPath`"") }
        catch { Write-Verbose "Explorer could not be opened: $($_.Exception.Message)" }
    }
    exit 0
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
