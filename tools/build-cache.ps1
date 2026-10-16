# Local build history. All paths refer to this PC; this file never reads the headset.
function Get-KitCacheKey {
    param([string]$ProjectRoot)
    $paths = @()
    foreach ($name in @('native', 'layer', 'loader', 'assets', 'tools')) {
        $paths += @(Get-ChildItem -LiteralPath (Join-Path $ProjectRoot $name) -File -Recurse |
            Where-Object { $_.FullName -notmatch '[\\/]__pycache__[\\/]' })
    }
    $paths += @(Get-ChildItem -LiteralPath $ProjectRoot -File | Where-Object { $_.Extension -in @('.bat', '.sh') })
    $lines = foreach ($file in @($paths | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($ProjectRoot.TrimEnd('\', '/').Length + 1).Replace('\', '/')
        "$relative`t$((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)"
    }
    $hash = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))))).Replace('-', '').ToLowerInvariant() }
    finally { $hash.Dispose() }
}

function Get-GameInputCacheKey {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $item = Get-Item -LiteralPath $Path
    if (-not $item.PSIsContainer -and $item.Name -ine 'base.apk') {
        Write-Host 'Checking the saved game archive...'
        return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
    $root = if ($item.PSIsContainer) { $item.FullName } else { $item.DirectoryName }
    $files = @(Get-ChildItem -LiteralPath $root -File | Where-Object { $_.Name -eq 'base.apk' -or $_.Name -like 'split_*.apk' })
    $external = Join-Path $root 'external'
    if (Test-Path -LiteralPath $external -PathType Container) { $files += @(Get-ChildItem -LiteralPath $external -Recurse -File) }
    if ($files.Count -eq 0) { return $null }
    $lines = foreach ($file in @($files | Sort-Object FullName)) {
        Write-Host "Checking saved input $($file.Name)..."
        "$($file.FullName.Substring($root.Length))`t$((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)"
    }
    $hash = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))))).Replace('-', '') }
    finally { $hash.Dispose() }
}

function Read-BuildCache {
    param([string]$WorkRoot)
    $path = Join-Path $WorkRoot 'last-build.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    try {
        $state = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        if ($state.formatVersion -ne 1) { return $null }
        foreach ($key in @('gamePackage', 'keystore', 'keystoreSha256', 'kitKey', 'inputKey', 'trade', 'buildRoot')) {
            if ($null -eq $state.PSObject.Properties[$key]) { return $null }
        }
        $root = Get-FullPath ([string]$state.buildRoot)
        if (-not (Test-IsUnderPath -Candidate $root -Root (Join-Path $WorkRoot 'runs'))) { return $null }
        return $state
    }
    catch { Write-Warning 'The previous build record could not be read. A new build will be prepared.'; return $null }
}

function Find-PreviousGameInput {
    param([string]$WorkRoot, $Cache)
    if ($null -ne $Cache -and (Test-Path -LiteralPath ([string]$Cache.gamePackage))) { return [string]$Cache.gamePackage }
    # Older installers kept extracted, unmodified APKs under runs/<id>/input.
    # Only recover a complete build's archive input, never its patched out/base.apk.
    $runs = Join-Path $WorkRoot 'runs'
    if (-not (Test-Path -LiteralPath $runs -PathType Container)) { return $null }
    foreach ($run in @(Get-ChildItem -LiteralPath $runs -Directory | Sort-Object Name -Descending)) {
        $manifest = Join-Path $run.FullName 'build-manifest.json'
        $input = Join-Path $run.FullName 'input'
        if ((Test-Path -LiteralPath $manifest -PathType Leaf) -and (Test-Path -LiteralPath (Join-Path $input 'base.apk') -PathType Leaf)) {
            try {
                $m = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
                if ($m.package -cin @('com.rockstargames.rdr', 'com.netflix.NGP.Kamo') -and $m.versionCode -eq $script:ExpectedVersionCode -and
                    @($m.external.obb).Count -eq 0 -and @($m.external.assetpacks).Count -eq 0) { return $input }
            }
            catch { continue }
        }
    }
    return $null
}

function Get-ReusableBuild {
    param($Cache, [string]$KitKey, [string]$KeystorePath, [string]$InputKey, [int]$RequestedTrade)
    if ($null -eq $Cache) { return $null }
    try {
        if ($Cache.kitKey -ne $KitKey -or [int]$Cache.trade -ne $RequestedTrade -or
            $Cache.keystore -ine $KeystorePath -or -not (Test-Path -LiteralPath $KeystorePath -PathType Leaf)) { return $null }
        if ((Get-FileHash -LiteralPath $KeystorePath -Algorithm SHA256).Hash -ne $Cache.keystoreSha256) { return $null }
        if (-not [string]::IsNullOrWhiteSpace($InputKey) -and $InputKey -ne $Cache.inputKey) { return $null }
        return Assert-BuildArtifacts -BuildRoot ([string]$Cache.buildRoot)
    }
    catch { Write-Warning "The previous build cannot be reused: $($_.Exception.Message)"; return $null }
}

function Save-BuildCache {
    param([string]$WorkRoot, [string]$KitKey, [string]$GamePath, [string]$InputKey, [string]$BuildRoot,
          [string]$KeystorePath, [int]$RequestedTrade)
    $state = [ordered]@{
        formatVersion = 1; kitKey = $KitKey; gamePackage = $GamePath; inputKey = $InputKey; buildRoot = $BuildRoot
        keystore = $KeystorePath; keystoreSha256 = (Get-FileHash -LiteralPath $KeystorePath -Algorithm SHA256).Hash
        trade = $RequestedTrade
    }
    $path = Join-Path $WorkRoot 'last-build.json'
    $temp = "$path.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        Write-TextFile -Path $temp -Text ($state | ConvertTo-Json -Depth 4)
        Move-Item -LiteralPath $temp -Destination $path -Force
    }
    finally { if (Test-Path -LiteralPath $temp -PathType Leaf) { Remove-Item -LiteralPath $temp -Force } }
}

function Get-ApkUpdatePlan {
    param($BuildInfo)
    $full = [pscustomobject]@{ ApkPaths = @($BuildInfo.ApkPaths); Inherit = $false; Bytes = $BuildInfo.ApkBytes }
    try {
        $paths = Invoke-AdbCapture -Arguments @('shell', 'pm', 'path', $script:PackageName)
        $remote = @{}
        foreach ($line in $paths.Lines) {
            if ([string]::IsNullOrWhiteSpace($line)) { continue }
            if ($line -notmatch '^package:(/data/app/[A-Za-z0-9_.~=+/-]+\.apk)\s*$') { return $full }
            $path = $Matches[1]
            if ($path.Contains('..')) { return $full }
            $leaf = $path.Substring($path.LastIndexOf('/') + 1)
            if ($remote.ContainsKey($leaf)) { return $full }
            $remote[$leaf] = $path
        }
        $records = @($BuildInfo.Manifest.outputs)
        if ($remote.Count -ne $records.Count) { return $full }
        $changed = @(); [int64]$bytes = 0
        foreach ($record in $records) {
            $leaf = Get-ExpectedInstalledApkLeaf -Record $record
            if (-not $remote.ContainsKey($leaf)) { return $full }
            Write-Host "Checking whether installed $leaf changed..."
            if ((Get-RemoteSha256 -RemotePath $remote[$leaf]) -ne ([string]$record.sha256).ToUpperInvariant()) {
                $changed += Resolve-ManifestFile -Root $BuildInfo.OutputRoot -RelativePath ([string]$record.file)
                $bytes += [int64]$record.size
            }
        }
        return [pscustomobject]@{ ApkPaths = @($changed); Inherit = ($changed.Count -gt 0 -and $changed.Count -lt $records.Count); Bytes = $bytes }
    }
    catch { Write-Warning 'Could not verify the existing APK set; all APKs will be sent.'; return $full }
}
