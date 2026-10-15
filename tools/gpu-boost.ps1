[CmdletBinding()]
param(
    [string]$Adb,
    [string]$AndroidSdk,
    [string]$WorkDir = 'C:\RDRVRBuild',
    [string]$Serial,
    [switch]$Off,
    [switch]$Status,
    [switch]$NonInteractive
)

# Starts, stops or shows the GPU boost helper (tools/rdrvr_boost.sh) on the Quest.
# The helper raises the GPU clock only while the game runs and asks for it in its
# GRAPHICS menu; a headset restart stops it. The game is never launched.

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$script:HelperRemote = '/data/local/tmp/rdrvr_boost.sh'
$script:SelectedSerial = $null
$script:AdbExe = $null
$script:PlatformTools = @{
    FileName = 'platform-tools_r37.0.1-win.zip'
    Uri = 'https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip'
    Sha256 = '45F4D63113E895EBDE0C90F194099A4676B6AC653BD28D54314A9E022BBC1A99'
}

function Invoke-NativeCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $raw = @(& $FilePath @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    $lines = @($raw | ForEach-Object { $_.ToString() })
    $text = $lines -join [Environment]::NewLine
    if ($exitCode -ne 0 -and -not $AllowFailure.IsPresent) {
        throw "Command failed ($exitCode): $FilePath $($Arguments -join ' ')`n$text"
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Lines = $lines; Text = $text }
}

function Test-AdbCandidate {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $probe = Invoke-NativeCapture -FilePath $Path -Arguments @('version') -AllowFailure
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
        [switch]$AllowFailure
    )
    $allArguments = @()
    if (-not $WithoutSerial.IsPresent) {
        if ([string]::IsNullOrWhiteSpace([string]$script:SelectedSerial)) { throw 'No Quest has been selected.' }
        $allArguments += @('-s', $script:SelectedSerial)
    }
    $allArguments += $Arguments
    return Invoke-NativeCapture -FilePath $script:AdbExe -Arguments $allArguments -AllowFailure:$AllowFailure
}

function Select-Quest {
    Invoke-Adb -Arguments @('start-server') -WithoutSerial -AllowFailure | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    $guidanceShown = $false
    while ($true) {
        $result = Invoke-Adb -Arguments @('devices', '-l') -WithoutSerial -AllowFailure
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
    $model = (Invoke-Adb -Arguments @('shell', 'getprop', 'ro.product.model')).Text.Trim()
    $manufacturer = (Invoke-Adb -Arguments @('shell', 'getprop', 'ro.product.manufacturer')).Text.Trim()
    $identity = "$manufacturer $model".Trim()
    if ($identity -notmatch '(?i)quest|oculus|meta') {
        throw "The selected device does not identify itself as a Meta Quest: '$identity'."
    }
    Write-Host "Selected Quest: $($script:SelectedSerial) ($identity)"
}

function Test-RemoteFile {
    param([Parameter(Mandatory = $true)][string]$Path)
    $result = Invoke-Adb -Arguments @('shell', 'test', '-f', $Path) -AllowFailure
    return $result.ExitCode -eq 0
}

function Get-RemoteSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    $result = Invoke-Adb -Arguments @('shell', 'toybox', 'sha256sum', $Path) -AllowFailure
    if ($result.ExitCode -ne 0) { throw "Could not hash $Path on the headset: $($result.Text)" }
    $hash = ($result.Text.Trim() -split '\s+')[0]
    if ($hash -notmatch '^[0-9a-fA-F]{64}$') { throw "Unexpected sha256sum output for ${Path}: $($result.Text)" }
    return $hash.ToUpperInvariant()
}

function Get-GpuLevel {
    return (Invoke-Adb -Arguments @('shell', 'getprop', 'debug.oculus.gpuLevel')).Text.Trim()
}

function Get-HelperStatus {
    $result = Invoke-Adb -Arguments @('shell', 'sh', $script:HelperRemote, 'status') -AllowFailure
    return [pscustomobject]@{
        Running = $result.Text -match '(?m)^helper: running'
        Text = $result.Text.Trim()
    }
}

function Write-BoostGuidance {
    Write-Host ''
    Write-Host 'GPU BOOST' -ForegroundColor Green
    Write-Host '  - Run GPU_BOOST.bat once after every headset restart. A restart stops the helper.'
    Write-Host '  - In the VR menu, GRAPHICS > GPU BOOST chooses the level: OFF, 640 or 690 MHz (default 690).'
    Write-Host '    It shows NO HELPER when the helper is not running.'
    Write-Host '  - The boost applies only while the game runs. Home and other apps keep their normal clocks.'
    Write-Host '  - Higher clocks drain the battery faster and warm the headset. When the headset gets hot,'
    Write-Host '    the game shows a red overheat warning and lowers 690 to 640 MHz.'
    Write-Host '  - GPU_BOOST.bat -Off stops the helper; GPU_BOOST.bat -Status shows its state.'
}

try {
    if ($Off.IsPresent -and $Status.IsPresent) { throw 'Use either -Off or -Status, not both.' }
    $resolvedWorkDir = [System.IO.Path]::GetFullPath($WorkDir.Trim().Trim('"').Trim("'"))
    $driveRoot = [System.IO.Path]::GetPathRoot($resolvedWorkDir)
    if ($resolvedWorkDir.TrimEnd('\') -eq $driveRoot.TrimEnd('\')) { throw 'WorkDir must be a dedicated folder, not a drive root.' }
    $helperLocal = Join-Path $PSScriptRoot 'rdrvr_boost.sh'
    if (-not (Test-Path -LiteralPath $helperLocal -PathType Leaf)) {
        throw "The source kit is incomplete. Missing: $helperLocal"
    }
    $script:AdbExe = Resolve-Adb -ResolvedWorkDir $resolvedWorkDir
    Write-Host "ADB: $($script:AdbExe)"
    Select-Quest
    $helperPresent = Test-RemoteFile -Path $script:HelperRemote

    if ($Status.IsPresent) {
        Write-Host ''
        if (-not $helperPresent) {
            $level = Get-GpuLevel
            Write-Host 'The GPU boost helper has not been started on this headset. Run GPU_BOOST.bat to start it.' -ForegroundColor Yellow
            Write-Host "debug.oculus.gpuLevel: $(if ($level) { $level } else { 'not set' })"
            exit 0
        }
        $state = Get-HelperStatus
        Write-Host $state.Text
        if ($state.Running) {
            Write-Host 'The GPU boost helper is running.' -ForegroundColor Green
        }
        else {
            Write-Host 'The GPU boost helper is not running. Run GPU_BOOST.bat to start it.' -ForegroundColor Yellow
        }
        exit 0
    }

    if ($Off.IsPresent) {
        if ($helperPresent) {
            $stop = Invoke-Adb -Arguments @('shell', 'sh', $script:HelperRemote, 'stop') -AllowFailure
            Write-Host $stop.Text.Trim()
            if ($stop.ExitCode -ne 0) {
                throw 'The GPU boost helper did not stop. Restart the headset to restore the normal GPU clocks.'
            }
        }
        else {
            Write-Host 'The GPU boost helper has not been started on this headset; nothing to stop.'
        }
        $level = Get-GpuLevel
        if ($level) {
            Write-Host "The GPU level is still $level. It was set by another tool, not by this helper. Restart the headset to restore the normal GPU clocks." -ForegroundColor Yellow
        }
        else {
            Write-Host 'GPU boost is off. The headset uses its normal GPU clocks.' -ForegroundColor Green
        }
        Write-Host 'The game was not launched.'
        exit 0
    }

    Write-Host 'Copying the GPU boost helper to the headset...'
    $push = Invoke-Adb -Arguments @('push', $helperLocal, $script:HelperRemote) -AllowFailure
    if ($push.ExitCode -ne 0) { throw "Could not copy the helper to the headset: $($push.Text)" }
    $localHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $helperLocal).Hash
    if ((Get-RemoteSha256 -Path $script:HelperRemote) -ne $localHash) { throw 'The helper copy on the headset does not match the kit file.' }
    $start = Invoke-Adb -Arguments @('shell', 'sh', $script:HelperRemote, 'start') -AllowFailure
    if ($start.ExitCode -ne 0) { throw "Could not start the helper: $($start.Text)" }
    $state = $null
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        Start-Sleep -Seconds 2
        $state = Get-HelperStatus
        if ($state.Running) { break }
    }
    Write-Host ''
    Write-Host $state.Text
    if (-not $state.Running) { throw 'The GPU boost helper did not start. Reconnect the headset and run GPU_BOOST.bat again.' }
    Write-Host ''
    Write-Host 'GPU boost helper is running on the headset.' -ForegroundColor Green
    Write-BoostGuidance
    Write-Host ''
    Write-Host 'The game was not launched.'
    exit 0
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
