[CmdletBinding()]
param(
    [string]$Root,
    [string]$PythonExe,
    [string]$BashExe
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = Split-Path -Path $PSScriptRoot -Parent
}
$Root = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
$failures = New-Object 'System.Collections.Generic.List[string]'

function Add-Failure {
    param([Parameter(Mandatory = $true)][string]$Message)
    $failures.Add($Message)
}

function Get-RelativePath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return $Path.Substring($Root.Length).TrimStart('\', '/').Replace('\', '/')
}

function Invoke-NativeText {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = (& $FilePath @Arguments 2>&1 | ForEach-Object { $_.ToString() }) -join "`n"
        $exitCode = $LASTEXITCODE
    }
    catch {
        $output = $_.Exception.Message
        $exitCode = -1
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Output = $output }
}

function Read-KitText {
    param([Parameter(Mandatory = $true)][string]$Relative)
    $path = Join-Path $Root $Relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Add-Failure "Required file is missing: $Relative"
        return ''
    }
    return [System.IO.File]::ReadAllText($path)
}

$forbiddenDirectoryNames = @(
    'recon', 'deps', 'dist', 'out', '__pycache__', 'node_modules',
    '.agents', '.cache', '.claude', '.codex', '.gradle', '.idea', '.vscode'
)
$forbiddenExtensions = @(
    '.apk', '.apks', '.xapk', '.apkm', '.aab', '.aar', '.obb',
    '.so', '.dex', '.idsig', '.osw', '.idx',
    '.keystore', '.jks', '.p12', '.pfx', '.pem', '.key',
    '.zip', '.7z', '.rar', '.tar', '.gz', '.xz', '.bz2', '.tgz',
    '.log', '.csv', '.trace', '.perfetto-trace', '.tombstone',
    '.pyc', '.o', '.obj', '.a', '.lib', '.exe', '.dll', '.dylib',
    '.class', '.jar', '.wasm', '.bin', '.rpf', '.sav'
)
$forbiddenFileNames = @(
    '.env', 'local.properties', 'release-signing.properties', 'changelog.md', 'source_snapshot.md',
    'agents.md', 'claude.md', 'handoff.md', 'status.md', 'contract.md', 'memory.md'
)
$binaryExtensions = @('.png', '.jpg', '.jpeg', '.webp', '.ktx', '.ktx2', '.rgba', '.uxrh', '.spv', '.ttf', '.otf', '.wav', '.ogg')

# 1. Forbidden directories, extensions and file names.
$gitMetadataRoot = (Join-Path $Root '.git').TrimEnd('\') + '\'
$allItems = @(
    Get-ChildItem -LiteralPath $Root -Force -Recurse |
        Where-Object {
            -not $_.FullName.StartsWith($gitMetadataRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            $_.FullName -ne $gitMetadataRoot.TrimEnd('\')
        }
)
foreach ($item in $allItems | Where-Object { $_.PSIsContainer }) {
    if ($forbiddenDirectoryNames -contains $item.Name.ToLowerInvariant() -or $item.Name -like 'build*') {
        Add-Failure "Forbidden directory: $(Get-RelativePath $item.FullName)"
    }
}
$files = @($allItems | Where-Object { -not $_.PSIsContainer })
foreach ($file in $files) {
    if ($forbiddenExtensions -contains $file.Extension.ToLowerInvariant() -or
        $forbiddenFileNames -contains $file.Name.ToLowerInvariant()) {
        Add-Failure "Forbidden file: $(Get-RelativePath $file.FullName)"
    }
}

# 2. Version: tools/build.py, README.md, the installers and SOURCE_MANIFEST.json agree.
$buildPyText = Read-KitText 'tools\build.py'
$versionMatch = [regex]::Match($buildPyText, '(?m)^MOD_VERSION\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"')
$modVersion = $null
if (-not $versionMatch.Success) {
    Add-Failure 'Could not read MOD_VERSION from tools/build.py.'
}
else {
    $modVersion = $versionMatch.Groups[1].Value
    $readmeText = Read-KitText 'README.md'
    if ($readmeText -cnotmatch ('Version `' + [regex]::Escape($modVersion) + ' alpha`')) {
        Add-Failure "README.md does not state Version ``$modVersion alpha``."
    }
    $windowsInstallerText = Read-KitText 'tools\build-and-install.ps1'
    $posixInstallerText = Read-KitText 'tools\build-and-install.sh'
    if ($windowsInstallerText -cnotmatch ("\`$script:ModVersion = '" + [regex]::Escape($modVersion) + "'") -or
        $posixInstallerText -cnotmatch ('MOD_VERSION="' + [regex]::Escape($modVersion) + '"') -or
        $posixInstallerText -cnotmatch ('RDRVR-SourceKit/' + [regex]::Escape($modVersion))) {
        Add-Failure "An installer does not expect mod version $modVersion."
    }
    $manifestPath = Join-Path $Root 'SOURCE_MANIFEST.json'
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $sourceManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $manifestEntries = @($sourceManifest.files.PSObject.Properties)
        if ($sourceManifest.version -ne $modVersion) {
            Add-Failure "SOURCE_MANIFEST.json version $($sourceManifest.version) differs from $modVersion."
        }
        if ($manifestEntries.Count -ne $files.Count - 1) {
            Add-Failure "SOURCE_MANIFEST.json lists $($manifestEntries.Count) files; the kit has $($files.Count - 1)."
        }
        foreach ($entry in $manifestEntries) {
            if ($entry.Name -match '(^/|(^|/)\.\.(/|$)|:|\\)') {
                Add-Failure "Unsafe SOURCE_MANIFEST.json path: $($entry.Name)"
                continue
            }
            $sourceFile = Join-Path $Root $entry.Name
            if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf) -or
                (Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash -ne $entry.Value) {
                Add-Failure "SOURCE_MANIFEST.json hash mismatch: $($entry.Name)"
            }
        }
    }
}

# 3. Syntax of every PowerShell, shell and Python script.
foreach ($script in $files | Where-Object { $_.Extension -ieq '.ps1' }) {
    try { [void][scriptblock]::Create([System.IO.File]::ReadAllText($script.FullName)) }
    catch { Add-Failure "PowerShell parse error in $(Get-RelativePath $script.FullName): $($_.Exception.Message)" }
}

function Test-BashCandidate {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $result = Invoke-NativeText -FilePath $Path -Arguments @('--version')
    return $result.ExitCode -eq 0 -and $result.Output -match 'GNU bash'
}
if ([string]::IsNullOrWhiteSpace($BashExe)) {
    $bashCandidates = @()
    if ($env:OS -eq 'Windows_NT') {
        $bashCandidates += @(
            (Join-Path $env:ProgramFiles 'Git\bin\bash.exe'),
            (Join-Path $env:ProgramFiles 'Git\usr\bin\bash.exe'),
            (Join-Path $env:LOCALAPPDATA 'Programs\Git\bin\bash.exe')
        )
    }
    $bashCandidates += @(Get-Command bash -All -ErrorAction SilentlyContinue | ForEach-Object { $_.Source } |
        Where-Object { $_ -notmatch '(?i)\\System32\\' })
    $BashExe = $bashCandidates | Where-Object { Test-BashCandidate $_ } | Select-Object -First 1
}
$shellScripts = @($files | Where-Object { $_.Extension -ieq '.sh' })
if ($shellScripts.Count -gt 0) {
    if ([string]::IsNullOrWhiteSpace($BashExe) -or -not (Test-BashCandidate $BashExe)) {
        Add-Failure 'Bash was not found; pass -BashExe so every shell script can be syntax-checked.'
    }
    else {
        foreach ($script in $shellScripts) {
            $bashPath = $script.FullName.Replace('\', '/')
            $result = Invoke-NativeText -FilePath $BashExe -Arguments @('-n', $bashPath)
            if ($result.ExitCode -ne 0) { Add-Failure "Bash syntax error in $(Get-RelativePath $script.FullName): $($result.Output)" }
        }
    }
}

function Test-PythonCandidate {
    param([string]$Command)
    if ([string]::IsNullOrWhiteSpace($Command)) { return $false }
    $result = Invoke-NativeText -FilePath $Command -Arguments @('--version')
    return $result.ExitCode -eq 0 -and $result.Output -match 'Python 3\.(1[0-9]|[2-9][0-9])'
}
if ([string]::IsNullOrWhiteSpace($PythonExe)) {
    $PythonExe = @(Get-Command python, python3 -All -ErrorAction SilentlyContinue | ForEach-Object { $_.Source }) |
        Where-Object { Test-PythonCandidate $_ } | Select-Object -First 1
}
$pythonScripts = @($files | Where-Object { $_.Extension -ieq '.py' })
if ($pythonScripts.Count -gt 0) {
    if (-not (Test-PythonCandidate $PythonExe)) {
        Add-Failure 'Python 3.10 or newer was not found; pass -PythonExe so every Python script can be parsed.'
    }
    else {
        foreach ($script in $pythonScripts) {
            $result = Invoke-NativeText -FilePath $PythonExe -Arguments @(
                '-c', 'import ast, pathlib, sys; ast.parse(pathlib.Path(sys.argv[1]).read_bytes().decode(), filename=sys.argv[1])',
                $script.FullName)
            if ($result.ExitCode -ne 0) { Add-Failure "Python syntax error in $(Get-RelativePath $script.FullName): $($result.Output)" }
        }
    }
}

# 4. Shipped scripts never launch the game or change headset power, properties or app data.
#    The GPU boost helper may set only the GPU level and its own heartbeat property.
$shippedScripts = @($files | Where-Object { $_.Extension.ToLowerInvariant() -in @('.ps1', '.sh', '.bat') })
$boostHelperPath = Join-Path $Root 'tools\rdrvr_boost.sh'
foreach ($script in $shippedScripts) {
    $normalized = (([System.IO.File]::ReadAllText($script.FullName) -replace '[\x27\x22`,()@]', ' ') -replace '\s+', ' ')
    if ($normalized -match '(?i)\bam\s+start(?:service)?\b|\bmonkey\b') {
        Add-Failure "Application launch command in $(Get-RelativePath $script.FullName): $($Matches[0])"
    }
    if ($script.FullName -ieq $boostHelperPath) {
        $normalized = $normalized -creplace '\bsetprop debug\.(?:oculus\.gpuLevel|oculus\.cpuLevel|rdrvr\.boostd) ', ' '
    }
    if ($normalized -match '(?i)vrpower[m]anager|\bkeyevent\b|\bsetprop\b|\bpm\s+clear\b|\bsvc\s+power\b') {
        Add-Failure "Headset power, property or app-data command in $(Get-RelativePath $script.FullName): $($Matches[0])"
    }
}

# 5. Product invariants of the exporter and the installers.
$exporterText = Read-KitText 'tools\export-rdr-apks.ps1'
if (-not (Test-Path -LiteralPath (Join-Path $Root 'EXPORT_RDR_APKS.bat') -PathType Leaf)) {
    Add-Failure 'Required file is missing: EXPORT_RDR_APKS.bat'
}
if ($exporterText -notmatch [regex]::Escape('lib/arm64-v8a/librdr.so') -or $exporterText -notmatch [regex]::Escape('_android.rpf')) {
    Add-Failure 'The exporter no longer verifies the engine library and the game data files.'
}
$windowsInstallerText = Read-KitText 'tools\build-and-install.ps1'
$posixInstallerText = Read-KitText 'tools\build-and-install.sh'
if ($windowsInstallerText -notmatch '\$stageContainer\s*=\s*"/sdcard/\.rdrvr-stage-\$id"' -or
    $posixInstallerText -notmatch 'stage_container="/sdcard/\.rdrvr-stage-\$id"') {
    Add-Failure 'External data uploads must be staged outside Android/data and Android/obb.'
}
if ($windowsInstallerText -notmatch [regex]::Escape("@('shell', 'chmod', '-R', 'a+rwX', `$Tree.RemoteFinal)") -or
    $posixInstallerText -notmatch [regex]::Escape('adb_cmd shell chmod -R a+rwX "$target"')) {
    Add-Failure 'An installer no longer makes published external data accessible to the game.'
}
if ($windowsInstallerText -notmatch '--no-incremental' -or $posixInstallerText -notmatch '--no-incremental') {
    Add-Failure 'An installer no longer installs with --no-incremental.'
}

# 6. The reset tools remove exactly the settings files that the native code writes.
$resetFiles = @('RESET_VR_SETTINGS.bat', 'RESET_VR_SETTINGS.sh', 'tools\reset-vr-settings.ps1', 'tools\reset-vr-settings.sh')
$resetText = ($resetFiles | ForEach-Object { Read-KitText $_ }) -join "`n"
$nativeSourceRoot = Join-Path $Root 'native\src'
if (-not (Test-Path -LiteralPath $nativeSourceRoot -PathType Container)) {
    Add-Failure 'Native sources are missing: native/src'
}
else {
    $nativeText = (Get-ChildItem -LiteralPath $nativeSourceRoot -File -Recurse |
        Where-Object { $_.Extension.ToLowerInvariant() -in @('.cpp', '.h', '.hpp', '.inc', '.c', '.cc') } |
        ForEach-Object { [System.IO.File]::ReadAllText($_.FullName) }) -join "`n"
    $nativeSettings = @(
        [regex]::Matches($nativeText, '"/?([a-z][a-z0-9_-]*\.cfg)"') |
            ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
    )
    if ($nativeSettings.Count -eq 0) {
        Add-Failure 'No settings file names were found in the native sources.'
    }
    $expectedSettings = @($nativeSettings + @($nativeSettings | ForEach-Object { "$_.tmp" }) | Sort-Object -Unique)
    foreach ($relative in @('tools\reset-vr-settings.ps1', 'tools\reset-vr-settings.sh')) {
        $implementation = Read-KitText $relative
        $implementedSettings = @(
            [regex]::Matches($implementation, '\b[a-z][a-z0-9_-]*\.cfg(?:\.tmp)?\b') |
                ForEach-Object { $_.Value } | Sort-Object -Unique
        )
        if (($implementedSettings -join "`n") -ne ($expectedSettings -join "`n")) {
            Add-Failure "The settings list in $relative ($($implementedSettings -join ', ')) differs from the native settings files ($($expectedSettings -join ', '))."
        }
        if ($implementation -notmatch [regex]::Escape('teleport-spots.txt')) {
            Add-Failure "$relative no longer handles the optional teleport-spots.txt removal."
        }
    }
}
$normalizedResetText = (($resetText -replace '[\x27\x22`,()@]', ' ') -replace '\s+', ' ')
if ($normalizedResetText -match '(?i)\bpm\s+clear\b|\buninstall\b|\*\.cfg|\brm\s+-[a-z]*r') {
    Add-Failure "A reset tool contains a broad removal operation: $($Matches[0])"
}

# 7. The save transfer tools stage files only for the mod to import; they never reach into the
#    private game storage or remove folders.
$saveFiles = @('TRANSFER_SAVES.bat', 'TRANSFER_SAVES.sh', 'tools\transfer-saves.ps1', 'tools\transfer-saves.sh')
$saveText = ($saveFiles | ForEach-Object { Read-KitText $_ }) -join "`n"
$normalizedSaveText = (($saveText -replace '[\x27\x22`,()@]', ' ') -replace '\s+', ' ')
if ($normalizedSaveText -match '(?i)\brun-as\b|\bpm\s+clear\b|\buninstall\b|/data/(?:data|user)/|\brm\s+-[a-z]*r') {
    Add-Failure "A save transfer tool contains a forbidden operation: $($Matches[0])"
}

# 8. Text hygiene: encoding, line endings and private, session or secret markers.
$forbiddenTextPatterns = @(
    '(?i)\bC:[\\/]Dev[\\/]', '(?i)\b[A-Z]:\\Users\\[^\\\s]+\\',
    '(?i)\bH:\\', '(?i)/Users/[^/\s]+/',
    '(?i)/home/[^/\s]+/', '(?i)promptPreview|ChatGPT|Codex|Claude',
    '(?i)\bOpenAI\b|\bGemini\b|\bCopilot\b|generated[- ]by(?: an? )?(?:AI|LLM)',
    '(?i)another agent|concurrently-owned|see memory|qbuild',
    '[\u0400-\u052f]',
    '(?i)hdcars|hd_cars|HD cars|vehicle_mesh_opt|vehicle_texture_audit|prepare-vehicle-tools',
    '(?i)the user (?:asked|requested|reported|wanted)|previous release|between releases|earlier work|field failure|r[0-9]+ fails here',
    '(?m)^(<<<<<<<|=======|>>>>>>>)',
    '-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
    '\bAKIA[0-9A-Z]{16}\b', '\bgh[pousr]_[A-Za-z0-9_]{20,}\b',
    '\b[DH]:[\\/]dev[\\/]',
    'rdr1-(artifacts|vrhands|mv|merge|perf-agent)',
    'scratchpad',
    '(?i)\b(Claude|Anthropic|Codex|ChatGPT|Gemini|Copilot|ultracode)\b',
    '(?i)\bsub-?agents?\b',
    "(?i)\bthe user'?s?\b",
    '(?i)\buser (said|asked|wants?|wanted|saw|reported|requested|confirmed)\b',
    '(?i)handoff/',
    '\b(?:(?:25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(?:25[0-5]|2[0-4]\d|1?\d?\d)\b',
    ':5555'
)
$datePattern = '\b20\d\d-\d\d-\d\d\b'
$auditPath = Join-Path $Root 'tools\release-audit.ps1'
[int64]$totalBytes = 0
foreach ($file in $files) {
    $totalBytes += $file.Length
    $extension = $file.Extension.ToLowerInvariant()
    if ($binaryExtensions -contains $extension -or $forbiddenExtensions -contains $extension) { continue }
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    if ([Array]::IndexOf($bytes, [byte]0) -ge 0) { continue }
    $relative = Get-RelativePath $file.FullName
    if ($extension -ne '.bat' -and [Array]::IndexOf($bytes, [byte]13) -ge 0) { Add-Failure "CR line endings (LF required): $relative" }
    if ($extension -in @('.ps1', '.bat', '.sh') -and @($bytes | Where-Object { $_ -gt 127 }).Count -gt 0) {
        Add-Failure "Non-ASCII bytes in script: $relative"
    }
    if ($file.FullName -eq $auditPath) { continue }
    $content = [System.Text.Encoding]::UTF8.GetString($bytes)
    foreach ($pattern in $forbiddenTextPatterns) {
        if ($content -match $pattern) {
            Add-Failure "Forbidden private, session or secret marker in ${relative}: $($Matches[0])"
        }
    }
    $dateText = $content
    if ($file.Name -in @('LICENSE', 'NOTICE.md')) {
        $dateText = (($content -split "`n") | Where-Object { $_ -notmatch '(?i)copyright' }) -join "`n"
    }
    if ($dateText -match $datePattern) {
        Add-Failure "Date in ${relative}: $($Matches[0])"
    }
}

if ($failures.Count -gt 0) {
    Write-Host "Source-kit audit FAILED with $($failures.Count) problem(s):" -ForegroundColor Red
    foreach ($failure in $failures) { Write-Host "  - $failure" }
    exit 1
}
Write-Host "Source-kit audit passed: $($files.Count) files, $totalBytes bytes" -ForegroundColor Green
exit 0
