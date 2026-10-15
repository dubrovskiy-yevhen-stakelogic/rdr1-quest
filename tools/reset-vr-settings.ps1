[CmdletBinding()]
param(
    [string]$Adb,
    [string]$AndroidSdk,
    [string]$WorkDir = 'C:\RDRVRBuild',
    [string]$Serial,
    [switch]$IncludeTeleportSpots,
    [switch]$Yes,
    [switch]$NonInteractive
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$script:PackageName = 'com.rockstargames.rdr'
$script:RemoteSettingsRoot = "/sdcard/Android/data/$($script:PackageName)/files/rdrvr"
$script:SelectedSerial = $null
$script:AdbExe = $null
$script:SettingsFiles = @(
    'controls.cfg',
    'controls.cfg.tmp',
    'graphics.cfg',
    'graphics.cfg.tmp',
    'vr.cfg',
    'vr.cfg.tmp',
    'weapons.cfg',
    'weapons.cfg.tmp'
)
$script:TeleportSpotsFile = 'teleport-spots.txt'
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

try {
    $resolvedWorkDir = [System.IO.Path]::GetFullPath($WorkDir.Trim().Trim('"').Trim("'"))
    $driveRoot = [System.IO.Path]::GetPathRoot($resolvedWorkDir)
    if ($resolvedWorkDir.TrimEnd('\') -eq $driveRoot.TrimEnd('\')) { throw 'WorkDir must be a dedicated folder, not a drive root.' }
    if ($NonInteractive.IsPresent -and -not $Yes.IsPresent) {
        throw '-NonInteractive requires -Yes before settings can be removed.'
    }
    $script:AdbExe = Resolve-Adb -ResolvedWorkDir $resolvedWorkDir
    Write-Host "ADB: $($script:AdbExe)"
    Select-Quest

    $package = Invoke-Adb -Arguments @('shell', 'pm', 'path', $script:PackageName) -AllowFailure
    if ($package.ExitCode -ne 0 -or $package.Text -notmatch '(?m)^package:') {
        throw "$($script:PackageName) is not installed on the selected Quest."
    }

    $names = @($script:SettingsFiles)
    if ($IncludeTeleportSpots.IsPresent) { $names += $script:TeleportSpotsFile }
    $present = @($names | Where-Object { Test-RemoteFile -Path "$($script:RemoteSettingsRoot)/$_" })

    Write-Host ''
    Write-Host 'VR settings files in the headset:' -ForegroundColor Yellow
    foreach ($name in $names) {
        $status = if ($present -contains $name) { 'present' } else { 'absent' }
        Write-Host "  $($script:RemoteSettingsRoot)/$name ($status)"
    }
    Write-Host ''
    if (-not $IncludeTeleportSpots.IsPresent) {
        Write-Host "Saved teleport spots ($($script:TeleportSpotsFile)) are kept; add -IncludeTeleportSpots to remove them too."
    }
    Write-Host 'Saved games, reports, game data and the installed APKs are kept. The settings folder itself is kept.'
    if ($present.Count -eq 0) {
        Write-Host 'No VR settings files are present. Nothing was changed; defaults already apply.' -ForegroundColor Green
        exit 0
    }

    if (-not $Yes.IsPresent) {
        Write-Host 'The game is closed first, the files are copied to this PC, then removed from the headset.'
        $answer = Read-Host 'Type RESET to continue'
        if ($answer -cne 'RESET') { throw 'Reset was cancelled; no settings were removed.' }
    }

    $stop = Invoke-Adb -Arguments @('shell', 'am', 'force-stop', $script:PackageName) -AllowFailure
    $processResult = Invoke-Adb -Arguments @('shell', 'pidof', $script:PackageName) -AllowFailure
    if (-not [string]::IsNullOrWhiteSpace($processResult.Text)) {
        throw "$($script:PackageName) is still running after force-stop. PID: $($processResult.Text.Trim())"
    }
    if ($stop.ExitCode -ne 0) { throw "Could not stop $($script:PackageName): $($stop.Text)" }

    # The game saves these files while it runs, so the backup is taken after the stop.
    $backupRoot = Join-Path (Join-Path $resolvedWorkDir 'settings-backups') (Get-Date -Format 'yyyyMMdd-HHmmss')
    New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
    $records = @()
    foreach ($name in $present) {
        $remote = "$($script:RemoteSettingsRoot)/$name"
        $local = Join-Path $backupRoot $name
        $pull = Invoke-Adb -Arguments @('pull', $remote, $local) -AllowFailure
        if ($pull.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $local -PathType Leaf)) {
            throw "Could not back up ${remote}: $($pull.Text)"
        }
        $localHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $local).Hash
        if ((Get-RemoteSha256 -Path $remote) -ne $localHash) { throw "Backup hash mismatch for $name; nothing was removed." }
        $records += [ordered]@{ name = $name; size = (Get-Item -LiteralPath $local).Length; sha256 = $localHash.ToLowerInvariant() }
    }
    $manifest = [ordered]@{
        formatVersion = 1
        package = $script:PackageName
        remoteRoot = $script:RemoteSettingsRoot
        createdUtc = [DateTime]::UtcNow.ToString('o')
        files = $records
    }
    [System.IO.File]::WriteAllText((Join-Path $backupRoot 'backup-manifest.json'),
        ($manifest | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Backup verified: $backupRoot"

    foreach ($name in $present) {
        $path = "$($script:RemoteSettingsRoot)/$name"
        $delete = Invoke-Adb -Arguments @('shell', 'rm', '-f', '--', $path) -AllowFailure
        if ($delete.ExitCode -ne 0) { throw "Could not remove ${path}: $($delete.Text)" }
    }
    foreach ($name in $names) {
        $path = "$($script:RemoteSettingsRoot)/$name"
        if (Test-RemoteFile -Path $path) { throw "Reset verification failed; the file still exists: $path" }
    }

    Write-Host ''
    Write-Host "VR settings reset verified: $($present.Count) file(s) removed. Defaults apply at the next start." -ForegroundColor Green
    Write-Host "Backup: $backupRoot"
    Write-Host 'The game was not launched.'
    exit 0
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
