[CmdletBinding()]
param(
    [string]$WorkDir = 'C:\RDRVRBuild',
    [string]$Destination,
    [string]$Serial
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# Google Play and Netflix Games install the same game build; either one can be exported.
$script:KnownPackages = @(
    [pscustomobject]@{ Name = 'com.rockstargames.rdr'; Label = 'Google Play (Rockstar)' },
    [pscustomobject]@{ Name = 'com.netflix.NGP.Kamo'; Label = 'Netflix Games' }
)
$script:PackageName = $null
$script:PackageLabel = $null
$script:ExpectedVersionName = '1.58.63226194'
$script:ExpectedVersionCode = '63226194'
$script:SupportedEngineSha256 = '3F80845EDEC32A663C42A8570E3131446813E695076C4BC96E2142C190BC22F3'
$script:EngineEntry = 'lib/arm64-v8a/librdr.so'
$script:RequiredDataEntries = @(
    'act', 'animationres', 'camera', 'flash', 'fonts', 'fragments', 'gringores', 'mapres',
    'naturalmotion', 'navres', 'shaders', 'strings', 'terrainboundres', 'terrainlodres',
    'textures', 'treeres', 'tune', 'vfx', 'vgd'
) | ForEach-Object { "assets/data/$($_)_android.rpf" }
$script:MinimumFreeBytes = 6GB
$script:PlatformToolsName = 'platform-tools_r37.0.1-win.zip'
$script:PlatformToolsUri = 'https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip'
$script:PlatformToolsSha256 = '45F4D63113E895EBDE0C90F194099A4676B6AC653BD28D54314A9E022BBC1A99'
$script:Adb = $null
$script:SelectedSerial = $null
$script:TranscriptStarted = $false

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path.Trim().Trim('"').Trim("'"))
}

function Invoke-NativeCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure,
        [switch]$Echo
    )
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $raw = @(& $FilePath @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $oldPreference }
    $lines = @($raw | ForEach-Object { $_.ToString() })
    $text = $lines -join [Environment]::NewLine
    if ($Echo.IsPresent -and -not [string]::IsNullOrWhiteSpace($text)) {
        Write-Host $text
    }
    if ($exitCode -ne 0 -and -not $AllowFailure.IsPresent) {
        throw "Command failed ($exitCode): $FilePath $($Arguments -join ' ')`n$text"
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Lines = $lines; Text = $text }
}

function Get-VerifiedPlatformTools {
    param([Parameter(Mandatory = $true)][string]$ResolvedWorkDir)
    $downloads = Join-Path $ResolvedWorkDir '.downloads'
    $tools = Join-Path $ResolvedWorkDir '.tools'
    $toolRoot = Join-Path $tools 'platform-tools-37.0.1'
    $adb = Join-Path $toolRoot 'platform-tools\adb.exe'
    New-Item -ItemType Directory -Force -Path $downloads, $tools | Out-Null

    if (Test-Path -LiteralPath $adb -PathType Leaf) {
        $probe = Invoke-NativeCapture -FilePath $adb -Arguments @('version') -AllowFailure
        if ($probe.ExitCode -eq 0 -and $probe.Text -match 'Android Debug Bridge') {
            Write-Host "Using cached ADB: $adb"
            return $adb
        }
        $aside = "$toolRoot.invalid-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
        Move-Item -LiteralPath $toolRoot -Destination $aside
    }

    $archive = Join-Path $downloads $script:PlatformToolsName
    $archiveReady = $false
    if (Test-Path -LiteralPath $archive -PathType Leaf) {
        $archiveReady = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -eq $script:PlatformToolsSha256
        if (-not $archiveReady) {
            $aside = "$archive.invalid-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
            Move-Item -LiteralPath $archive -Destination $aside
        }
    }
    if (-not $archiveReady) {
        $temporary = "$archive.download-$([Guid]::NewGuid().ToString('N'))"
        Write-Host 'Downloading Google Android SDK Platform Tools...'
        try {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -UseBasicParsing -Uri $script:PlatformToolsUri -OutFile $temporary
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash
            if ($hash -ne $script:PlatformToolsSha256) {
                throw "Platform Tools SHA256 mismatch. Expected $($script:PlatformToolsSha256), got $hash"
            }
            Move-Item -LiteralPath $temporary -Destination $archive
        }
        finally {
            if (Test-Path -LiteralPath $temporary -PathType Leaf) {
                Remove-Item -LiteralPath $temporary -Force
            }
        }
    }

    $temporaryRoot = "$toolRoot.extract-$([Guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    try {
        Expand-Archive -LiteralPath $archive -DestinationPath $temporaryRoot
        $temporaryAdb = Join-Path $temporaryRoot 'platform-tools\adb.exe'
        if (-not (Test-Path -LiteralPath $temporaryAdb -PathType Leaf)) {
            throw 'Downloaded Platform Tools archive does not contain adb.exe.'
        }
        if (Test-Path -LiteralPath $toolRoot) {
            $aside = "$toolRoot.old-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
            Move-Item -LiteralPath $toolRoot -Destination $aside
        }
        Move-Item -LiteralPath $temporaryRoot -Destination $toolRoot
    }
    finally {
        if (Test-Path -LiteralPath $temporaryRoot) {
            Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
        }
    }
    $probe = Invoke-NativeCapture -FilePath $adb -Arguments @('version')
    Write-Host $probe.Text
    return $adb
}

function Select-SourceDevice {
    Invoke-NativeCapture -FilePath $script:Adb -Arguments @('start-server') -AllowFailure | Out-Null
    $deadline = [DateTime]::UtcNow.AddMinutes(2)
    $guidanceShown = $false
    $devices = @()
    $ready = @()
    while ($true) {
        $result = Invoke-NativeCapture -FilePath $script:Adb -Arguments @('devices', '-l')
        $devices = @()
        foreach ($line in $result.Lines) {
            if ($line -match '^([^\s]+)\s+(device|unauthorized|offline|no permissions)\b\s*(.*)$') {
                $devices += [pscustomobject]@{
                    Serial = $Matches[1]
                    State = $Matches[2]
                    Detail = $Matches[3]
                }
            }
        }
        $ready = @($devices | Where-Object State -eq 'device')
        if (-not [string]::IsNullOrWhiteSpace($Serial)) {
            $requested = @($ready | Where-Object Serial -eq $Serial)
            if ($requested.Count -eq 1) {
                $script:SelectedSerial = $Serial
                break
            }
        }
        elseif ($ready.Count -gt 0) { break }

        if (-not $guidanceShown) {
            Write-Host ''
            Write-Host 'WAITING FOR USB DEBUGGING...' -ForegroundColor Yellow
            Write-Host 'Seeing the phone in Windows File Explorer is not enough; that is only the MTP file connection.' -ForegroundColor Yellow
            Write-Host 'On the phone: Settings > About phone > Software information > tap Build number 7 times.'
            Write-Host 'Then: Settings > System > Developer options > enable USB debugging.'
            Write-Host 'Keep the phone unlocked and tap Always allow from this computer, then Allow.'
            Write-Host 'The exporter will continue automatically when ADB becomes available.' -ForegroundColor Cyan
            $guidanceShown = $true
        }
        $remaining = [math]::Max(0, [int][math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds))
        if ($remaining -le 0) {
            Write-Progress -Id 7 -Activity 'Waiting for the Android phone/tablet' -Completed
            if (@($devices | Where-Object State -eq 'unauthorized').Count -gt 0) {
                throw 'USB debugging is still unauthorized. Unlock the phone and approve the Allow USB debugging prompt.'
            }
            if (@($devices | Where-Object State -eq 'offline').Count -gt 0) {
                throw 'The Android ADB connection is offline. Reconnect the USB cable, unlock the phone, and retry.'
            }
            throw 'Windows may see the phone for file transfer, but ADB still cannot see it. Enable USB debugging, approve this computer on the phone, and retry. If no prompt appears, use another data-capable USB cable or install the phone manufacturer ADB driver.'
        }
        $stateText = if (@($devices | Where-Object State -eq 'unauthorized').Count -gt 0) {
            'Phone found; approve the USB debugging prompt on it'
        } else { 'No ADB device yet; enable USB debugging on the unlocked phone' }
        Write-Progress -Id 7 -Activity 'Waiting for the Android phone/tablet' `
            -Status "$stateText ($remaining seconds remaining)" `
            -PercentComplete ([math]::Min(100, [math]::Max(0, (120 - $remaining) * 100 / 120)))
        Start-Sleep -Seconds 2
    }
    Write-Progress -Id 7 -Activity 'Waiting for the Android phone/tablet' -Completed

    if ([string]::IsNullOrWhiteSpace([string]$script:SelectedSerial)) {
        if ($ready.Count -eq 1) { $script:SelectedSerial = $ready[0].Serial }
        else {
            Write-Host 'Connected Android devices:'
            for ($index = 0; $index -lt $ready.Count; $index++) {
                Write-Host "  [$($index + 1)] $($ready[$index].Serial) $($ready[$index].Detail)"
            }
            $answer = Read-Host 'Choose the phone/tablet containing the Google Play game'
            [int]$number = 0
            if (-not [int]::TryParse($answer, [ref]$number) -or
                $number -lt 1 -or $number -gt $ready.Count) {
                throw 'Invalid device selection.'
            }
            $script:SelectedSerial = $ready[$number - 1].Serial
        }
    }
    $model = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
        '-s', $script:SelectedSerial, 'shell', 'getprop', 'ro.product.model'
    )
    $manufacturer = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
        '-s', $script:SelectedSerial, 'shell', 'getprop', 'ro.product.manufacturer'
    )
    if (("$($manufacturer.Text) $($model.Text)") -match '(?i)\b(?:oculus|quest)\b') {
        throw 'The selected device is a Quest headset. Disconnect it and connect the Android phone/tablet that has Red Dead Redemption installed.'
    }
    $script:DeviceModel = "$($manufacturer.Text.Trim()) $($model.Text.Trim())"
    Write-Host "Selected source device: $($script:SelectedSerial) ($($script:DeviceModel))" -ForegroundColor Green
}

function Get-RemoteApkPaths {
    param([Parameter(Mandatory = $true)][string]$Package)
    $pathResult = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
        '-s', $script:SelectedSerial, 'shell', 'pm', 'path', $Package
    ) -AllowFailure
    return @($pathResult.Lines | ForEach-Object {
        if ($_ -match '^package:(/.+\.apk)\s*$') { $Matches[1].Trim() }
    } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
}

function Select-InstalledGame {
    $installed = @()
    foreach ($known in $script:KnownPackages) {
        $paths = @(Get-RemoteApkPaths -Package $known.Name)
        if ($paths.Count -gt 0) {
            $installed += [pscustomobject]@{ Name = $known.Name; Label = $known.Label; Paths = $paths }
        }
    }
    if ($installed.Count -eq 0) {
        $list = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
            '-s', $script:SelectedSerial, 'shell', 'pm', 'list', 'packages'
        ) -AllowFailure
        $similar = @($list.Lines | Where-Object { $_ -match '(?i)rockstar|netflix|rdr|reddead|kamo' })
        $hint = if ($similar.Count -gt 0) { "`nPossibly related packages on this device:`n  $($similar -join "`n  ")" } else { '' }
        throw "Red Dead Redemption is not installed on this device (looked for $(($script:KnownPackages | ForEach-Object Name) -join ', ')). Install it from Google Play or from the Netflix app, launch it once, let it finish downloading, then retry.$hint"
    }
    $choice = $installed[0]
    if ($installed.Count -gt 1) {
        Write-Host 'More than one Red Dead Redemption install was found:'
        for ($index = 0; $index -lt $installed.Count; $index++) {
            Write-Host "  [$($index + 1)] $($installed[$index].Label) - $($installed[$index].Name)"
        }
        $answer = Read-Host 'Choose which one to export'
        [int]$number = 0
        if (-not [int]::TryParse($answer, [ref]$number) -or $number -lt 1 -or $number -gt $installed.Count) {
            throw 'Invalid selection.'
        }
        $choice = $installed[$number - 1]
    }
    $script:PackageName = $choice.Name
    $script:PackageLabel = $choice.Label

    $dump = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
        '-s', $script:SelectedSerial, 'shell', 'dumpsys', 'package', $script:PackageName
    ) -AllowFailure
    $script:VersionName = [regex]::Match($dump.Text, '(?m)^\s*versionName=([^\s]+)').Groups[1].Value
    $script:VersionCode = [regex]::Match($dump.Text, '(?m)^\s*versionCode=(\d+)').Groups[1].Value
    Write-Host "Found $($script:PackageLabel): $($script:PackageName) $($script:VersionName) ($($script:VersionCode)), $($choice.Paths.Count) installed APK file(s)." -ForegroundColor Green
    if ($script:VersionName -ne $script:ExpectedVersionName) {
        Write-Host "WARNING: the VR mod supports version $($script:ExpectedVersionName). This copy is $($script:VersionName). The export continues so the files can be checked." -ForegroundColor Yellow
    }
    return $choice.Paths
}

function Get-ZipEntrySha256 {
    param([Parameter(Mandatory = $true)]$Entry)
    $stream = $Entry.Open()
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '') }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Export-ExternalFolder {
    param(
        [Parameter(Mandatory = $true)][string]$RemoteDir,
        [Parameter(Mandatory = $true)][string]$LocalDir
    )
    $probe = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
        '-s', $script:SelectedSerial, 'shell', 'ls', $RemoteDir
    ) -AllowFailure
    if ($probe.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($probe.Text) -or
        $probe.Text -match '(?i)no such file|permission denied') {
        return $false
    }
    Write-Host "Exporting $RemoteDir"
    New-Item -ItemType Directory -Force -Path $LocalDir | Out-Null
    $pull = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
        '-s', $script:SelectedSerial, 'pull', $RemoteDir, $LocalDir
    ) -AllowFailure -Echo
    if ($pull.ExitCode -ne 0) {
        Write-Host "WARNING: could not copy $RemoteDir`n$($pull.Text)" -ForegroundColor Yellow
        return $false
    }
    return $true
}

function Export-Game {
    param([Parameter(Mandatory = $true)][string[]]$RemotePaths)
    $resolvedDestination = if ([string]::IsNullOrWhiteSpace($Destination)) {
        Join-Path $resolvedWorkDir ("RDR-export-{0}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    } else { Get-FullPath $Destination }
    if (Test-Path -LiteralPath $resolvedDestination) {
        if (@(Get-ChildItem -LiteralPath $resolvedDestination -Force).Count -ne 0) {
            throw "Export destination is not empty: $resolvedDestination"
        }
    }
    else { New-Item -ItemType Directory -Path $resolvedDestination | Out-Null }

    $drive = New-Object System.IO.DriveInfo ([System.IO.Path]::GetPathRoot($resolvedDestination))
    if ($drive.AvailableFreeSpace -lt $script:MinimumFreeBytes) {
        throw ("Not enough free space on {0}: {1:N1} GB free, about {2:N0} GB needed. Free some space or pass -WorkDir on another drive." -f `
            $drive.Name, ($drive.AvailableFreeSpace / 1GB), ($script:MinimumFreeBytes / 1GB))
    }

    Write-Host 'The game is about 3 GB. Copying takes a few minutes; keep the phone unlocked and connected.' -ForegroundColor Cyan
    foreach ($remotePath in $RemotePaths) {
        Write-Host "Exporting $remotePath"
        $pull = Invoke-NativeCapture -FilePath $script:Adb -Arguments @(
            '-s', $script:SelectedSerial, 'pull', $remotePath, $resolvedDestination
        ) -AllowFailure -Echo
        if ($pull.ExitCode -ne 0) {
            throw "APK export failed. Keep the device unlocked and connected, then retry.`n$($pull.Text)"
        }
    }
    $localApks = @(Get-ChildItem -LiteralPath $resolvedDestination -Filter '*.apk' -File)
    if ($localApks.Count -ne $RemotePaths.Count -or @($localApks | Where-Object Length -le 0).Count -ne 0) {
        throw "Export verification failed: expected $($RemotePaths.Count) APK files, found $($localApks.Count)."
    }
    if (@($localApks | Where-Object Name -eq 'base.apk').Count -ne 1) {
        throw 'Exported set does not contain exactly one base.apk.'
    }

    # Play Asset Delivery packs that are not install-time splits, and any OBB, live in shared storage.
    $extraRoot = Join-Path $resolvedDestination 'external'
    $gotObb = Export-ExternalFolder -RemoteDir "/sdcard/Android/obb/$($script:PackageName)" -LocalDir (Join-Path $extraRoot 'obb')
    $gotPacks = Export-ExternalFolder -RemoteDir "/sdcard/Android/data/$($script:PackageName)/files/assetpacks" -LocalDir (Join-Path $extraRoot 'assetpacks')

    Write-Host 'Verifying exported files...'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $engineHashes = @()
    $foundData = @{}
    $apkReport = @()
    foreach ($apk in $localApks) {
        try { $archive = [System.IO.Compression.ZipFile]::OpenRead($apk.FullName) }
        catch { throw "Exported file is not a readable APK: $($apk.FullName)`n$($_.Exception.Message)" }
        try {
            foreach ($entry in $archive.Entries) {
                if ($entry.FullName -eq $script:EngineEntry) {
                    $engineHashes += [pscustomobject]@{ Apk = $apk.Name; Sha256 = (Get-ZipEntrySha256 -Entry $entry) }
                }
                if ($script:RequiredDataEntries -contains $entry.FullName) { $foundData[$entry.FullName] = $apk.Name }
            }
        }
        finally { $archive.Dispose() }
        $apkReport += [pscustomobject]@{ Name = $apk.Name; Bytes = $apk.Length }
    }
    if (Test-Path -LiteralPath $extraRoot) {
        foreach ($file in @(Get-ChildItem -LiteralPath $extraRoot -Recurse -File -Filter '*_android.rpf')) {
            $key = "assets/data/$($file.Name)"
            if ($script:RequiredDataEntries -contains $key -and -not $foundData.ContainsKey($key)) {
                $foundData[$key] = $file.FullName.Substring($resolvedDestination.Length + 1)
            }
        }
    }
    $missingData = @($script:RequiredDataEntries | Where-Object { -not $foundData.ContainsKey($_) })
    $engineSha = if ($engineHashes.Count -eq 1) { $engineHashes[0].Sha256 } else { $null }
    $engineMatches = ($engineSha -eq $script:SupportedEngineSha256)

    $report = [ordered]@{
        tool = 'export-rdr-apks'
        exportedUtc = [DateTime]::UtcNow.ToString('o')
        device = $script:DeviceModel
        source = $script:PackageLabel
        packageName = $script:PackageName
        versionName = $script:VersionName
        versionCode = $script:VersionCode
        apks = $apkReport
        obbCopied = $gotObb
        assetPacksCopied = $gotPacks
        engineApk = if ($engineHashes.Count -eq 1) { $engineHashes[0].Apk } else { $null }
        engineSha256 = $engineSha
        engineMatchesSupportedBuild = $engineMatches
        missingDataFiles = $missingData
    }
    $reportPath = Join-Path $resolvedDestination 'export-report.json'
    $report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $reportPath -Encoding UTF8

    Write-Host ''
    if ($engineHashes.Count -ne 1) {
        throw "Exported set is not usable on Quest: expected exactly one APK containing $($script:EngineEntry), found $($engineHashes.Count). Use a real 64-bit ARM Android phone/tablet (not an emulator) and export again. Report: $reportPath"
    }
    if ($missingData.Count -gt 0) {
        throw "Export is incomplete; these game data files were not found:`n  $($missingData -join "`n  ")`nLaunch the game on the phone once and let it finish downloading all data, then export again. Report: $reportPath"
    }
    Write-Host 'EXPORT COMPLETED.' -ForegroundColor Green
    Write-Host "Folder: $resolvedDestination"
    Write-Host "Game: $($script:PackageLabel), $($script:PackageName) $($script:VersionName)"
    if ($engineMatches) {
        Write-Host 'The game engine matches the version supported by the VR mod.' -ForegroundColor Green
    }
    else {
        Write-Host 'The game engine does NOT match the version supported by the VR mod; the builder will refuse this export.' -ForegroundColor Yellow
        Write-Host 'Keep the files. export-report.json lists only file names, sizes and a hash, and contains no game data.' -ForegroundColor Yellow
    }
    Write-Host 'Keep base.apk and every split_*.apk (and the external folder, if present) together. Do not rename them.' -ForegroundColor Cyan
    Start-Process -FilePath 'explorer.exe' -ArgumentList @($resolvedDestination)
    return $resolvedDestination
}

$failure = $null
try {
    $resolvedWorkDir = Get-FullPath $WorkDir
    $root = [System.IO.Path]::GetPathRoot($resolvedWorkDir)
    if ($resolvedWorkDir.TrimEnd('\') -eq $root.TrimEnd('\')) {
        throw 'WorkDir must not be a drive root.'
    }
    New-Item -ItemType Directory -Force -Path $resolvedWorkDir | Out-Null
    $logs = Join-Path $resolvedWorkDir 'logs'
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    $log = Join-Path $logs ("export-rdr-apks-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Start-Transcript -LiteralPath $log -Force | Out-Null
    $script:TranscriptStarted = $true
    Write-Host "Log: $log"
    $script:Adb = Get-VerifiedPlatformTools -ResolvedWorkDir $resolvedWorkDir
    Select-SourceDevice
    $paths = @(Select-InstalledGame)
    Export-Game -RemotePaths $paths | Out-Null
}
catch {
    $failure = $_
    Write-Host ''
    Write-Host "FAILED: $($failure.Exception.Message)" -ForegroundColor Red
}
finally {
    if ($script:TranscriptStarted) { try { Stop-Transcript | Out-Null } catch { } }
}

if ($null -ne $failure) { exit 1 }
exit 0
