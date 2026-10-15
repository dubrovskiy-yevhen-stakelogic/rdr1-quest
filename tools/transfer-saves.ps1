[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$From,
    [switch]$FromHeadset,
    [string]$To,
    [switch]$IncludeBackups,
    [switch]$Status,
    [switch]$SkipExtras,
    [string]$Adb,
    [string]$AndroidSdk,
    [string]$WorkDir = 'C:\RDRVRBuild',
    [string]$Serial,
    [switch]$Yes,
    [switch]$NonInteractive
)

# Copies saved games between the PC version of Red Dead Redemption and the Quest.
# The game keeps its saves in private storage that ADB cannot read. At every start the mod
# copies them to rdrvr/save-export and imports the files staged in rdrvr/save-import, after
# backing up each save it replaces in rdrvr/save-backup. The game is never launched.

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$script:PackageName = 'com.rockstargames.rdr'
$script:RemoteRoot = "/sdcard/Android/data/$($script:PackageName)/files/rdrvr"
$script:RemoteList = "$($script:RemoteRoot)/save-files.txt"
$script:RemoteLog = "$($script:RemoteRoot)/save-transfer.log"
$script:RemoteExport = "$($script:RemoteRoot)/save-export"
$script:RemoteImport = "$($script:RemoteRoot)/save-import"
$script:RemoteBackup = "$($script:RemoteRoot)/save-backup"
$script:RemoteTemp = '/data/local/tmp'
$script:SaveNamePattern = '^(?:RDR2SAVE[0-9]|RDR2ZOMBIESAVE[0-9]|REDDEAD2HCOR[0-9]|RDR2EXTRAS)\.SAV$'
$script:MinSaveBytes = 1024
$script:MaxSaveBytes = 1048576
$script:SaveMarker = 'METAXTR'
$script:SaveMarkerOffset = 0x20
$script:StagedRemote = New-Object 'System.Collections.Generic.List[string]'
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

function Connect-Quest {
    param([Parameter(Mandatory = $true)][string]$ResolvedWorkDir)
    $script:AdbExe = Resolve-Adb -ResolvedWorkDir $ResolvedWorkDir
    Write-Host "ADB: $($script:AdbExe)"
    Select-Quest
    $package = Invoke-Adb -Arguments @('shell', 'pm', 'path', $script:PackageName) -AllowFailure
    if ($package.ExitCode -ne 0 -or $package.Text -notmatch '(?m)^package:') {
        throw "$($script:PackageName) is not installed on the selected Quest."
    }
}

function Test-RemoteFile {
    param([Parameter(Mandatory = $true)][string]$Path)
    $result = Invoke-Adb -Arguments @('shell', 'test', '-f', $Path) -AllowFailure
    return $result.ExitCode -eq 0
}

function Test-RemoteDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)
    $result = Invoke-Adb -Arguments @('shell', 'test', '-d', $Path) -AllowFailure
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

function Get-RemoteNames {
    param([Parameter(Mandatory = $true)][string]$Directory)
    if (-not (Test-RemoteDirectory -Path $Directory)) { return @() }
    $result = Invoke-Adb -Arguments @('shell', 'ls', '-1', $Directory) -AllowFailure
    if ($result.ExitCode -ne 0) { throw "Could not list $Directory on the headset: $($result.Text)" }
    return @($result.Lines | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' } | Sort-Object)
}

function Test-GameRunning {
    $processResult = Invoke-Adb -Arguments @('shell', 'pidof', $script:PackageName) -AllowFailure
    return -not [string]::IsNullOrWhiteSpace($processResult.Text)
}

function Stop-Game {
    $stop = Invoke-Adb -Arguments @('shell', 'am', 'force-stop', $script:PackageName) -AllowFailure
    $processResult = Invoke-Adb -Arguments @('shell', 'pidof', $script:PackageName) -AllowFailure
    if (-not [string]::IsNullOrWhiteSpace($processResult.Text)) {
        throw "$($script:PackageName) is still running after force-stop. PID: $($processResult.Text.Trim())"
    }
    if ($stop.ExitCode -ne 0) { throw "Could not stop $($script:PackageName): $($stop.Text)" }
    Write-Host 'The game is closed.'
}

function Format-UnixTime {
    param([string]$Seconds, [string]$Format = 'yyyy-MM-dd HH:mm')
    [int64]$value = 0
    if (-not [int64]::TryParse([string]$Seconds, [ref]$value) -or $value -le 0) { return 'unknown time' }
    return [DateTimeOffset]::FromUnixTimeSeconds($value).LocalDateTime.ToString($Format)
}

function Get-SaveKind {
    param([Parameter(Mandatory = $true)][string]$Name)
    $upper = $Name.ToUpperInvariant()
    if ($upper.StartsWith('RDR2ZOMBIESAVE')) { return 'Undead Nightmare' }
    if ($upper.StartsWith('REDDEAD2HCOR')) { return 'hardcore' }
    if ($upper.StartsWith('RDR2EXTRAS')) { return 'extras' }
    if ($upper.StartsWith('RDR2SAVE')) { return 'story' }
    return 'other'
}

function Get-LocalSaveName {
    # save-export and save-backup name each file by its private path with '/' replaced by '__'.
    param([Parameter(Mandatory = $true)][string]$RemoteName)
    $index = $RemoteName.LastIndexOf('__')
    if ($index -ge 0) { return $RemoteName.Substring($index + 2) }
    return $RemoteName
}

function Get-SaveProblem {
    param([Parameter(Mandatory = $true)][System.IO.FileInfo]$File)
    if ($File.Length -le $script:MinSaveBytes) { return "too small ($($File.Length) bytes)" }
    if ($File.Length -ge $script:MaxSaveBytes) { return "too large ($($File.Length) bytes)" }
    $bytes = [System.IO.File]::ReadAllBytes($File.FullName)
    $marker = [System.Text.Encoding]::ASCII.GetString($bytes, $script:SaveMarkerOffset, $script:SaveMarker.Length)
    if ($marker -cne $script:SaveMarker) { return 'no Red Dead Redemption save header' }
    return $null
}

function Get-HeadsetSaveList {
    # save-files.txt is written at every game start: one line per save with its private path,
    # size and modification time (Unix seconds), separated by tabs, after a '#' header line.
    $list = [pscustomobject]@{ Exists = $false; ListedAt = $null; Entries = @() }
    if (-not (Test-RemoteFile -Path $script:RemoteList)) { return $list }
    $result = Invoke-Adb -Arguments @('shell', 'cat', $script:RemoteList) -AllowFailure
    if ($result.ExitCode -ne 0) { throw "Could not read $($script:RemoteList): $($result.Text)" }
    $list.Exists = $true
    $entries = @()
    foreach ($line in $result.Lines) {
        $text = $line.TrimEnd("`r")
        if ($text.StartsWith('#')) {
            if ($text -match '\s(\d{9,12})(\s|$)') { $list.ListedAt = $Matches[1] }
            continue
        }
        $fields = $text -split "`t"
        if ($fields.Count -lt 3 -or [string]::IsNullOrWhiteSpace($fields[0])) { continue }
        $entries += [pscustomobject]@{
            Rel = $fields[0]
            Name = ($fields[0] -split '/')[-1]
            Size = $fields[1]
            Modified = $fields[2]
        }
    }
    $list.Entries = $entries
    return $list
}

function Find-PcProfileFolders {
    # The PC game keeps one folder per profile in Documents\Rockstar Games\Red Dead Redemption\Profiles.
    $roots = @()
    $documentsFolders = @([Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments))
    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) { $documentsFolders += (Join-Path $env:USERPROFILE 'Documents') }
    foreach ($documents in $documentsFolders) {
        if ([string]::IsNullOrWhiteSpace([string]$documents)) { continue }
        $root = Join-Path $documents 'Rockstar Games\Red Dead Redemption\Profiles'
        if ((Test-Path -LiteralPath $root -PathType Container) -and $roots -notcontains $root) { $roots += $root }
    }
    $profiles = @()
    foreach ($root in $roots) {
        foreach ($folder in @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue)) {
            $saves = @(Get-ChildItem -LiteralPath $folder.FullName -File -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match $script:SaveNamePattern })
            if ($saves.Count -eq 0) { continue }
            $newest = ($saves | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1).LastWriteTime
            $profiles += [pscustomobject]@{ Path = $folder.FullName; Newest = $newest }
        }
    }
    return @($profiles | Sort-Object Newest -Descending)
}

function Copy-HeadsetSaves {
    # Copies every .SAV file of a headset folder to a PC folder as NAME.SAV and verifies each copy.
    param(
        [Parameter(Mandatory = $true)][string]$RemoteDirectory,
        [Parameter(Mandatory = $true)][string]$LocalDirectory
    )
    $records = @()
    $names = @(Get-RemoteNames -Directory $RemoteDirectory | Where-Object { $_ -match '(?i)\.SAV$' })
    foreach ($name in $names) {
        New-Item -ItemType Directory -Force -Path $LocalDirectory | Out-Null
        $localName = Get-LocalSaveName -RemoteName $name
        if (Test-Path -LiteralPath (Join-Path $LocalDirectory $localName)) { $localName = $name }
        $remote = "$RemoteDirectory/$name"
        $local = Join-Path $LocalDirectory $localName
        $pull = Invoke-Adb -Arguments @('pull', $remote, $local) -AllowFailure
        if ($pull.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $local -PathType Leaf)) {
            throw "Could not copy ${remote}: $($pull.Text)"
        }
        $localHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $local).Hash
        if ((Get-RemoteSha256 -Path $remote) -ne $localHash) { throw "The copy of $remote does not match the headset file." }
        $records += [pscustomobject]@{
            Name = $localName
            Remote = $remote
            Path = $local
            Size = (Get-Item -LiteralPath $local).Length
            Sha256 = $localHash.ToLowerInvariant()
        }
    }
    return $records
}

function Write-TransferManifest {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][object[]]$Records
    )
    $manifest = [ordered]@{
        formatVersion = 1
        package = $script:PackageName
        kind = $Kind
        createdUtc = [DateTime]::UtcNow.ToString('o')
        files = @($Records | ForEach-Object { [ordered]@{ name = $_.Name; source = $_.Remote; size = $_.Size; sha256 = $_.Sha256 } })
    }
    [System.IO.File]::WriteAllText((Join-Path $Directory 'transfer-manifest.json'),
        ($manifest | ConvertTo-Json -Depth 4), (New-Object System.Text.UTF8Encoding($false)))
}

function Invoke-ToHeadset {
    param([Parameter(Mandatory = $true)][string]$ResolvedWorkDir)

    # 1. The saves on this PC.
    if (-not [string]::IsNullOrWhiteSpace($From)) {
        $sourcePath = [System.IO.Path]::GetFullPath($From.Trim().Trim('"').Trim("'"))
        if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
            $sourceFiles = @(Get-Item -LiteralPath $sourcePath)
        }
        elseif (Test-Path -LiteralPath $sourcePath -PathType Container) {
            $sourceFiles = @(Get-ChildItem -LiteralPath $sourcePath -File)
        }
        else {
            throw "-From was not found: $sourcePath"
        }
        Write-Host "Source: $sourcePath"
    }
    else {
        $profiles = @(Find-PcProfileFolders)
        if ($profiles.Count -eq 0) {
            throw 'No saves of the PC game were found in Documents\Rockstar Games\Red Dead Redemption\Profiles. Pass -From with the folder that holds the .SAV files.'
        }
        $sourcePath = $profiles[0].Path
        Write-Host "PC profile folder with the newest saves: $sourcePath"
        if ($profiles.Count -gt 1) {
            Write-Host 'Other profile folders (pass one with -From to use it instead):'
            foreach ($other in $profiles[1..($profiles.Count - 1)]) {
                Write-Host "  $($other.Path) (newest save $($other.Newest.ToString('yyyy-MM-dd HH:mm')))"
            }
        }
        $sourceFiles = @(Get-ChildItem -LiteralPath $sourcePath -File)
    }

    $selected = @()
    $ignored = @()
    foreach ($file in $sourceFiles) {
        if ($file.Name -notmatch $script:SaveNamePattern) {
            if ($file.Extension -ieq '.sav') { $ignored += "$($file.Name) (not a save slot name)" }
            continue
        }
        if ($SkipExtras.IsPresent -and $file.Name -ieq 'RDR2EXTRAS.SAV') {
            $ignored += "$($file.Name) (-SkipExtras)"
            continue
        }
        $selected += $file
    }
    if ($selected.Count -eq 0) {
        throw "No save files (RDR2SAVE0.SAV, RDR2ZOMBIESAVE0.SAV, REDDEAD2HCOR0.SAV, RDR2EXTRAS.SAV and so on) were found in $sourcePath."
    }
    $problems = @()
    foreach ($file in $selected) {
        $problem = Get-SaveProblem -File $file
        if ($problem) { $problems += "  $($file.Name): $problem" }
    }
    if ($problems.Count -gt 0) {
        throw ("These files are not Red Dead Redemption saves; nothing was sent to the headset:`n" + ($problems -join "`n"))
    }
    Write-Host ''
    Write-Host 'Saves to send:' -ForegroundColor Yellow
    foreach ($file in $selected) {
        Write-Host ('  {0,-20} {1,-16} {2,8} bytes  modified {3}' -f $file.Name.ToUpperInvariant(), (Get-SaveKind $file.Name),
            $file.Length, $file.LastWriteTime.ToString('yyyy-MM-dd HH:mm'))
    }
    foreach ($name in $ignored) { Write-Host "  ignored: $name" }
    foreach ($file in $selected) {
        if ($file.Name -match '[4-9]\.SAV$') {
            Write-Host "Note: the Android game has save slots 0 to 3; $($file.Name.ToUpperInvariant()) may not appear in Load Game." -ForegroundColor Yellow
        }
    }

    # 2. The saves on the headset, as listed by the mod at the last game start.
    Write-Host ''
    Connect-Quest -ResolvedWorkDir $ResolvedWorkDir
    $headset = Get-HeadsetSaveList
    if (-not $headset.Exists) {
        throw 'The mod has not listed the headset saves yet (rdrvr/save-files.txt is missing). Start the game once until its main menu, close it, then run TRANSFER_SAVES again. If the file is still missing, build and install the mod from this kit again.'
    }
    $entries = @($headset.Entries)
    if ($entries.Count -eq 0) {
        throw 'The game has no saves on the headset yet, so the folder for imported saves is unknown. Start a new game on the headset, save it once, close the game, then run TRANSFER_SAVES again.'
    }
    $relatives = [string[]]@($entries | ForEach-Object { $_.Rel })
    [Array]::Sort($relatives, [StringComparer]::Ordinal)
    $slash = $relatives[0].LastIndexOf('/')
    $newFolder = if ($slash -ge 0) { $relatives[0].Substring(0, $slash + 1) } else { '' }

    # A save that exists is staged under its private path so the mod replaces exactly that file;
    # a new one is staged under its plain name and created next to the existing saves.
    $plan = @()
    foreach ($file in $selected) {
        $name = $file.Name.ToUpperInvariant()
        $match = @($entries | Where-Object { $_.Name -ieq $name })
        if ($match.Count -gt 0) {
            $plan += [pscustomobject]@{ File = $file; Name = $name; Stage = $match[0].Rel.Replace('/', '__'); Replaces = $match[0] }
        }
        else {
            $plan += [pscustomobject]@{ File = $file; Name = $name; Stage = $name; Replaces = $null }
        }
    }
    $replacedRelatives = @($plan | Where-Object { $null -ne $_.Replaces } | ForEach-Object { $_.Replaces.Rel })
    $kept = @($entries | Where-Object { $replacedRelatives -notcontains $_.Rel })
    $stageNames = @($plan | ForEach-Object { $_.Stage })
    $pending = @(Get-RemoteNames -Directory $script:RemoteImport | Where-Object { $_ -match '(?i)\.SAV$' })
    $obsolete = @($pending | Where-Object { $stageNames -cnotcontains $_ })

    Write-Host ''
    Write-Host "Saves on the headset when the game last started ($(Format-UnixTime $headset.ListedAt)). Saves imported or made since then are not listed:" -ForegroundColor Yellow
    foreach ($entry in $entries) {
        Write-Host ('  {0,-20} {1,-16} {2,8} bytes  saved {3}' -f $entry.Name, (Get-SaveKind $entry.Name), $entry.Size, (Format-UnixTime $entry.Modified))
    }
    Write-Host ''
    Write-Host 'Changes at the next game start:' -ForegroundColor Yellow
    foreach ($item in $plan) {
        if ($null -ne $item.Replaces) {
            Write-Host "  REPLACE  $($item.Name) ($(Get-SaveKind $item.Name)); the headset save from $(Format-UnixTime $item.Replaces.Modified) is backed up first"
        }
        else {
            Write-Host "  NEW      $($item.Name) ($(Get-SaveKind $item.Name)); not listed at the last start, imported as $newFolder$($item.Name)"
        }
    }
    foreach ($entry in $kept) { Write-Host "  keep     $($entry.Name)" }
    foreach ($name in $obsolete) {
        Write-Host "  An earlier transfer staged $name and it was not imported yet; it is removed so that only this transfer is applied."
    }
    Write-Host ''
    Write-Host "Every headset save that is replaced is backed up first on the headset in $($script:RemoteBackup)."
    Write-Host 'A copy of the current headset saves is also kept on this PC.'

    if (-not $Yes.IsPresent) {
        Write-Host 'The game is closed first; progress that was not saved in a running game is lost.'
        $answer = Read-Host 'Type IMPORT to continue'
        if ($answer -cne 'IMPORT') { throw 'The transfer was cancelled; nothing was sent to the headset.' }
    }

    # 3. Close the game, keep a PC copy of the headset saves, stage the new ones.
    Stop-Game
    $backupRoot = Join-Path (Join-Path $ResolvedWorkDir 'save-backups') (Get-Date -Format 'yyyyMMdd-HHmmss')
    $records = @(Copy-HeadsetSaves -RemoteDirectory $script:RemoteExport -LocalDirectory $backupRoot)
    if ($records.Count -gt 0) {
        Write-TransferManifest -Directory $backupRoot -Kind 'save-export' -Records $records
        Write-Host "Copy of the headset saves on this PC (as of the last game start): $backupRoot"
    }
    $copiedNames = @($records | ForEach-Object { $_.Name })
    $notCopied = @($entries | Where-Object { $copiedNames -notcontains $_.Name } | ForEach-Object { $_.Name })
    if ($notCopied.Count -gt 0) {
        Write-Host "WARNING: the headset has no exported copy of $($notCopied -join ', '); only the backup on the headset will hold it." -ForegroundColor Yellow
    }

    $mkdir = Invoke-Adb -Arguments @('shell', 'mkdir', '-p', $script:RemoteImport) -AllowFailure
    if ($mkdir.ExitCode -ne 0) { throw "Could not create $($script:RemoteImport): $($mkdir.Text)" }
    $chmod = Invoke-Adb -Arguments @('shell', 'chmod', '777', $script:RemoteImport) -AllowFailure
    if ($chmod.ExitCode -ne 0) { Write-Host "WARNING: could not open $($script:RemoteImport) to the game: $($chmod.Text)" -ForegroundColor Yellow }
    foreach ($name in $obsolete) {
        $remove = Invoke-Adb -Arguments @('shell', 'rm', '-f', '--', "$($script:RemoteImport)/$name") -AllowFailure
        if ($remove.ExitCode -ne 0) { throw "Could not remove the earlier staged file ${name}: $($remove.Text)" }
    }
    foreach ($item in $plan) {
        $temporary = "$($script:RemoteTemp)/rdrvr-$($item.Stage)"
        $target = "$($script:RemoteImport)/$($item.Stage)"
        $script:StagedRemote.Add($temporary)
        $script:StagedRemote.Add($target)
        $push = Invoke-Adb -Arguments @('push', $item.File.FullName, $temporary) -AllowFailure
        if ($push.ExitCode -ne 0) { throw "Could not copy $($item.Name) to the headset: $($push.Text)" }
        $copy = Invoke-Adb -Arguments @('shell', 'cp', $temporary, $target) -AllowFailure
        if ($copy.ExitCode -ne 0) { throw "Could not stage ${target}: $($copy.Text)" }
        Invoke-Adb -Arguments @('shell', 'chmod', '666', $target) -AllowFailure | Out-Null
        Invoke-Adb -Arguments @('shell', 'rm', '-f', '--', $temporary) -AllowFailure | Out-Null
        $localHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $item.File.FullName).Hash
        if ((Get-RemoteSha256 -Path $target) -ne $localHash) { throw "The staged copy of $($item.Name) does not match the PC file." }
    }
    $stagedNow = @(Get-RemoteNames -Directory $script:RemoteImport)
    foreach ($item in $plan) {
        if ($stagedNow -cnotcontains $item.Stage) { throw "Staging verification failed; $($item.Stage) is missing in $($script:RemoteImport)." }
    }
    $script:StagedRemote.Clear()

    Write-Host ''
    Write-Host "$($plan.Count) save file(s) are staged and verified. The headset saves change only when the game starts." -ForegroundColor Green
    foreach ($item in $plan) {
        $action = if ($null -ne $item.Replaces) { 'replaces' } else { 'new' }
        Write-Host "  $($item.Name) ($action)"
    }
    Write-Host ''
    Write-Host 'Next steps' -ForegroundColor Green
    Write-Host '  1. Start the game on the headset once. The saves are imported while it starts; each replaced'
    Write-Host "     save is backed up first in $($script:RemoteBackup)/<start time>."
    Write-Host '  2. In the main menu use Load Game and pick the slot. Continue loads the save with the newest'
    Write-Host '     in-game save date, which may be a different slot.'
    Write-Host '  3. TRANSFER_SAVES.bat -Status shows the import log after that start.'
    if ($records.Count -gt 0) { Write-Host "PC copy of the previous headset saves: $backupRoot" }
    Write-Host 'The game was not launched.'
}

function Invoke-FromHeadset {
    param([Parameter(Mandatory = $true)][string]$ResolvedWorkDir)
    if (-not [string]::IsNullOrWhiteSpace($To)) {
        $destination = [System.IO.Path]::GetFullPath($To.Trim().Trim('"').Trim("'"))
    }
    else {
        $destination = Join-Path (Join-Path $ResolvedWorkDir 'saves-from-headset') (Get-Date -Format 'yyyyMMdd-HHmmss')
    }
    Connect-Quest -ResolvedWorkDir $ResolvedWorkDir
    $headset = Get-HeadsetSaveList
    $exportNames = @(Get-RemoteNames -Directory $script:RemoteExport | Where-Object { $_ -match '(?i)\.SAV$' })
    if ($exportNames.Count -eq 0) {
        throw 'The headset has no exported saves yet (rdrvr/save-export is empty). Start the game once until its main menu, close it, then run TRANSFER_SAVES -FromHeadset again.'
    }
    foreach ($name in $exportNames) {
        $existing = Join-Path $destination (Get-LocalSaveName -RemoteName $name)
        if (Test-Path -LiteralPath $existing) {
            throw "$existing already exists. Choose an empty folder with -To; existing saves are never overwritten."
        }
    }

    $records = @(Copy-HeadsetSaves -RemoteDirectory $script:RemoteExport -LocalDirectory $destination)
    Write-TransferManifest -Directory $destination -Kind 'save-export' -Records $records
    Write-Host ''
    Write-Host "Headset saves as of the last game start ($(Format-UnixTime $headset.ListedAt)):" -ForegroundColor Green
    foreach ($record in $records) {
        Write-Host ('  {0,-20} {1,-16} {2,8} bytes' -f $record.Name, (Get-SaveKind $record.Name), $record.Size)
        $problem = Get-SaveProblem -File (Get-Item -LiteralPath $record.Path)
        if ($problem) { Write-Host "    WARNING: $problem" -ForegroundColor Yellow }
    }
    Write-Host "Copied to: $destination"
    Write-Host 'The mod copies the saves out when the game starts, before it imports staged saves. Saves made or'
    Write-Host 'imported after that are copied at the next start: start the game once until its main menu, close'
    Write-Host 'it, and run this again to include them.'

    $backupFolders = @(Get-RemoteNames -Directory $script:RemoteBackup)
    if ($backupFolders.Count -gt 0) {
        Write-Host ''
        Write-Host "The headset also keeps $($backupFolders.Count) backup folder(s) of saves that imports replaced ($($script:RemoteBackup))."
        $copyBackups = $IncludeBackups.IsPresent
        if (-not $copyBackups -and -not $NonInteractive.IsPresent) {
            $answer = Read-Host 'Type Y to copy them too, or press Enter to skip'
            $copyBackups = $answer -match '^(?i)y(es)?$'
        }
        if ($copyBackups) {
            foreach ($folder in $backupFolders) {
                $label = $folder
                if ($folder -match '^\d+$') { $label = Format-UnixTime -Seconds $folder -Format 'yyyyMMdd-HHmmss' }
                $local = Join-Path (Join-Path $destination 'headset-backups') $label
                $copied = @(Copy-HeadsetSaves -RemoteDirectory "$($script:RemoteBackup)/$folder" -LocalDirectory $local)
                if ($copied.Count -gt 0) { Write-TransferManifest -Directory $local -Kind 'save-backup' -Records $copied }
                Write-Host "  backup ${label}: $($copied.Count) file(s)"
            }
            Write-Host "Backups copied to: $(Join-Path $destination 'headset-backups')"
        }
        elseif ($NonInteractive.IsPresent) {
            Write-Host 'Add -IncludeBackups to copy them too.'
        }
    }

    Write-Host ''
    Write-Host 'To play them on the PC: close the PC game, keep a copy of its profile folder, then copy the'
    Write-Host ".SAV files from $destination into the profile folder:"
    $profiles = @(Find-PcProfileFolders)
    if ($profiles.Count -gt 0) { Write-Host "  $($profiles[0].Path)" }
    else { Write-Host '  Documents\Rockstar Games\Red Dead Redemption\Profiles\<profile>' }
    Write-Host 'The game was not launched.'
}

function Show-SaveStatus {
    param([Parameter(Mandatory = $true)][string]$ResolvedWorkDir)
    Connect-Quest -ResolvedWorkDir $ResolvedWorkDir
    $headset = Get-HeadsetSaveList
    Write-Host ''
    if (-not $headset.Exists) {
        Write-Host 'The save list (rdrvr/save-files.txt) is not present yet. The mod writes it when the game starts.' -ForegroundColor Yellow
    }
    else {
        Write-Host "Saves on the headset when the game last started ($(Format-UnixTime $headset.ListedAt)). Saves imported or made since then are not listed:" -ForegroundColor Green
        if (@($headset.Entries).Count -eq 0) { Write-Host '  none' }
        foreach ($entry in @($headset.Entries)) {
            Write-Host ('  {0,-20} {1,-16} {2,8} bytes  saved {3}' -f $entry.Name, (Get-SaveKind $entry.Name), $entry.Size, (Format-UnixTime $entry.Modified))
        }
    }

    $importNames = @(Get-RemoteNames -Directory $script:RemoteImport)
    $pending = @($importNames | Where-Object { $_ -match '(?i)\.SAV$' })
    $done = @($importNames | Where-Object { $_ -match '(?i)\.SAV\.done$' })
    Write-Host ''
    if ($pending.Count -gt 0) {
        Write-Host 'Waiting for import at the next game start:' -ForegroundColor Yellow
        foreach ($name in $pending) { Write-Host "  $name" }
    }
    else {
        Write-Host 'No saves are waiting for import.'
    }
    if ($done.Count -gt 0) { Write-Host "Imported earlier: $($done -join ', ')" }

    $exportNames = @(Get-RemoteNames -Directory $script:RemoteExport | Where-Object { $_ -match '(?i)\.SAV$' })
    Write-Host "Exported copies in rdrvr/save-export: $($exportNames.Count)"
    $backupFolders = @(Get-RemoteNames -Directory $script:RemoteBackup)
    if ($backupFolders.Count -gt 0) {
        Write-Host 'Backups of replaced saves in rdrvr/save-backup:'
        foreach ($folder in $backupFolders) {
            $files = @(Get-RemoteNames -Directory "$($script:RemoteBackup)/$folder")
            $label = if ($folder -match '^\d+$') { Format-UnixTime $folder } else { $folder }
            Write-Host "  $folder ($label): $($files -join ', ')"
        }
    }
    else {
        Write-Host 'No backups of replaced saves yet.'
    }

    Write-Host ''
    if (Test-RemoteFile -Path $script:RemoteLog) {
        $tail = Invoke-Adb -Arguments @('shell', 'tail', '-n', '20', $script:RemoteLog) -AllowFailure
        Write-Host 'Last lines of rdrvr/save-transfer.log:' -ForegroundColor Green
        foreach ($line in $tail.Lines) { Write-Host "  $($line.TrimEnd())" }
    }
    else {
        Write-Host 'No save transfer log yet.'
    }
    if (Test-GameRunning) {
        Write-Host ''
        Write-Host 'The game is running. Saves staged now are imported at the next start after it is closed.' -ForegroundColor Yellow
    }
    Write-Host 'The game was not launched.'
}

try {
    if ($FromHeadset.IsPresent -and $Status.IsPresent) { throw 'Use either -FromHeadset or -Status, not both.' }
    $toHeadset = -not ($FromHeadset.IsPresent -or $Status.IsPresent)
    if (-not $toHeadset -and -not [string]::IsNullOrWhiteSpace($From)) { throw '-From is used only when sending saves to the headset.' }
    if (-not $toHeadset -and $SkipExtras.IsPresent) { throw '-SkipExtras is used only when sending saves to the headset.' }
    if (-not $FromHeadset.IsPresent -and -not [string]::IsNullOrWhiteSpace($To)) { throw '-To is used only with -FromHeadset.' }
    if (-not $FromHeadset.IsPresent -and $IncludeBackups.IsPresent) { throw '-IncludeBackups is used only with -FromHeadset.' }
    if ($toHeadset -and $NonInteractive.IsPresent -and -not $Yes.IsPresent) {
        throw '-NonInteractive requires -Yes before saves can be sent to the headset.'
    }
    $resolvedWorkDir = [System.IO.Path]::GetFullPath($WorkDir.Trim().Trim('"').Trim("'"))
    $driveRoot = [System.IO.Path]::GetPathRoot($resolvedWorkDir)
    if ($resolvedWorkDir.TrimEnd('\') -eq $driveRoot.TrimEnd('\')) { throw 'WorkDir must be a dedicated folder, not a drive root.' }

    if ($Status.IsPresent) { Show-SaveStatus -ResolvedWorkDir $resolvedWorkDir }
    elseif ($FromHeadset.IsPresent) { Invoke-FromHeadset -ResolvedWorkDir $resolvedWorkDir }
    else { Invoke-ToHeadset -ResolvedWorkDir $resolvedWorkDir }
    exit 0
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    if ($script:StagedRemote.Count -gt 0) {
        foreach ($path in $script:StagedRemote) {
            try { Invoke-Adb -Arguments @('shell', 'rm', '-f', '--', $path) -AllowFailure | Out-Null } catch { }
        }
        Write-Host 'The files staged by this run were removed from the headset; its saves are unchanged.' -ForegroundColor Yellow
    }
    exit 1
}
