# Offline manifest and target-isolation checks. Never calls ADB.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'build-and-install.ps1')
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('rdrvr-profiles-' + [Guid]::NewGuid().ToString('N'))
$out = Join-Path $testRoot 'out'
New-Item -ItemType Directory -Path $out | Out-Null
$apk = Join-Path $out 'base.apk'
Write-TextFile $apk 'offline fixture'
$manifest = [ordered]@{
    formatVersion=1; modVersion=$script:ModVersion; package='com.rockstargames.rdr'
    versionCode=$script:ExpectedVersionCode; versionName=$script:ExpectedVersionName
    engineSha256=''; engineVariant=''; trade=0; buildId=('a'*64)
    sourceSignerSha256=('b'*64); outputSignerSha256=('c'*64); outputSignerHashCode='12345678'
    outputs=@(@{file='base.apk';split=$null;size=(Get-Item $apk).Length;sha256=(Get-FileHash $apk).Hash})
}
$hashes = @('3f80845edec32a663c42a8570e3131446813e695076c4bc96e2142c190bc22f3', '0f288e2f9255d04277190bb5ffca742ec395d33a47fb305ddb22b9f0e904ef72')
foreach ($package in @('com.rockstargames.rdr', 'com.netflix.NGP.Kamo')) {
    for ($i=0; $i -lt 2; $i++) {
        $manifest.package=$package; $manifest.engineSha256=$hashes[$i]; $manifest.engineVariant='variant-'+($i+1)
        Write-TextFile (Join-Path $testRoot 'build-manifest.json') ($manifest | ConvertTo-Json -Depth 8)
        $null=Assert-BuildArtifacts $testRoot
        if ($script:PackageName -cne $package -or $script:RemoteFilesRoot -cne "/sdcard/Android/data/$package/files") { throw 'wrong installation target' }
        Assert-SafeGeneratedRemotePath "/sdcard/Android/data/$package/files/.rdrvr-backup-$('a'*32)"
        $other=if($package -eq 'com.netflix.NGP.Kamo'){'com.rockstargames.rdr'}else{'com.netflix.NGP.Kamo'}
        $blocked=$false
        try { Assert-SafeGeneratedRemotePath "/sdcard/Android/data/$other/files/.rdrvr-backup-$('a'*32)" } catch { $blocked=$true }
        if(-not $blocked){throw 'cross-package cleanup accepted'}
    }
}
$manifest.engineSha256='d'*64
Write-TextFile (Join-Path $testRoot 'build-manifest.json') ($manifest | ConvertTo-Json -Depth 8)
$blocked=$false
try { $null=Assert-BuildArtifacts $testRoot } catch { $blocked=$_.Exception.Message -match 'report.*author' }
if(-not $blocked){throw 'unknown engine accepted or diagnostic missing'}
Write-Host "PASS: both engine/package combinations, unknown engine refusal and package-isolated paths. Fixtures: $testRoot"
