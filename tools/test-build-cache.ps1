# Offline tests: no ADB, game, or installed application is touched.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'build-and-install.ps1')
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('rdrvr-cache-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
$runs = Join-Path $testRoot 'runs'
$build = Join-Path $runs 'one'
New-Item -ItemType Directory -Path $build | Out-Null
$key = Join-Path $testRoot 'test.key'
$game = Join-Path $testRoot 'game.apks'
Write-TextFile $key 'key'
Write-TextFile $game 'game'
$inputKey = Get-GameInputCacheKey $game
function Assert-BuildArtifacts { param([string]$BuildRoot) if ($script:badArtifact) { throw 'bad hash' }; return [pscustomobject]@{ Root = $BuildRoot } }
$script:badArtifact = $false
Save-BuildCache $testRoot 'kit1' $game $inputKey $build $key 0
$cache = Read-BuildCache $testRoot
Assert ($null -ne $cache) 'state round trip'
Assert ((Find-PreviousGameInput $testRoot $cache) -eq $game) 'remember source'
Assert ($null -ne (Get-ReusableBuild $cache 'kit1' $key $inputKey 0)) 'same build must be reusable'
Assert ($null -ne (Get-ReusableBuild $cache 'kit1' $key $null 0)) 'missing original can reuse verified build'
Assert ($null -eq (Get-ReusableBuild $cache 'kit2' $key $inputKey 0)) 'changed source invalidates'
Assert ($null -eq (Get-ReusableBuild $cache 'kit1' $key $inputKey 1)) 'changed trade invalidates'
Write-TextFile $game 'different'
Assert ($null -eq (Get-ReusableBuild $cache 'kit1' $key (Get-GameInputCacheKey $game) 0)) 'changed game invalidates'
Write-TextFile $key 'different'
Assert ($null -eq (Get-ReusableBuild $cache 'kit1' $key $inputKey 0)) 'changed key invalidates'
Write-TextFile $key 'key'
$script:badArtifact = $true
Assert ($null -eq (Get-ReusableBuild $cache 'kit1' $key $inputKey 0)) 'bad output invalidates'
$script:badArtifact = $false
$cache.buildRoot = $testRoot
Write-TextFile (Join-Path $testRoot 'last-build.json') ($cache | ConvertTo-Json)
Assert ($null -eq (Read-BuildCache $testRoot)) 'build outside runs rejected'
Write-TextFile (Join-Path $testRoot 'last-build.json') '{}'
Assert ($null -eq (Read-BuildCache $testRoot)) 'invalid state rejected'

$kit = Join-Path $testRoot 'kit'
foreach ($name in @('native','layer','loader','assets','tools')) { New-Item -ItemType Directory -Path (Join-Path $kit $name) | Out-Null }
$source = Join-Path $kit 'native\source.cpp'
Write-TextFile $source 'one'
$kitKey = Get-KitCacheKey $kit
Assert ((Get-KitCacheKey $kit) -eq $kitKey) 'stable kit key'
Write-TextFile $source 'two'
Assert ((Get-KitCacheKey $kit) -ne $kitKey) 'source bytes invalidate kit key'
$kitKey = Get-KitCacheKey $kit
Write-TextFile (Join-Path $kit 'tools\generator.py') '# new generator'
Assert ((Get-KitCacheKey $kit) -ne $kitKey) 'new builder file invalidates kit key'

$old = Join-Path $runs 'old'
New-Item -ItemType Directory -Path (Join-Path $old 'input') | Out-Null
Write-TextFile (Join-Path $old 'input\base.apk') 'original fixture'
Write-TextFile (Join-Path $old 'build-manifest.json') '{"package":"com.rockstargames.rdr","versionCode":"63226194","external":{"obb":[],"assetpacks":[]}}'
Assert ((Find-PreviousGameInput $testRoot $null) -eq (Join-Path $old 'input')) 'recover old extracted original'

# Matching data APKs can stay on the headset; malformed or missing sets need a full install.
$script:remoteLines = @('package:/data/app/test/base.apk', 'package:/data/app/test/split_data0.apk', '')
$script:hashes = @{ '/data/app/test/base.apk' = 'OLD'; '/data/app/test/split_data0.apk' = 'DATA' }
function Invoke-AdbCapture { param($Arguments) return [pscustomobject]@{ Lines = $script:remoteLines } }
function Get-RemoteSha256 { param([string]$RemotePath) return $script:hashes[$RemotePath] }
$info = [pscustomobject]@{ OutputRoot = $build; ApkPaths = @('base.apk','split_data0.apk'); ApkBytes = 30
    Manifest = [pscustomobject]@{ outputs = @(
        [pscustomobject]@{ file = 'base.apk'; split = $null; sha256 = 'BASE'; size = 10 },
        [pscustomobject]@{ file = 'split_data0.apk'; split = 'data0'; sha256 = 'DATA'; size = 20 }) } }
$plan = Get-ApkUpdatePlan $info
Assert ($plan.Inherit -and $plan.ApkPaths.Count -eq 1 -and $plan.Bytes -eq 10) 'only base changed'
$script:hashes['/data/app/test/base.apk'] = 'BASE'
$plan = Get-ApkUpdatePlan $info
Assert ($plan.ApkPaths.Count -eq 0 -and -not $plan.Inherit) 'identical set skips transfer'
$script:remoteLines = @('package:/data/app/test/base.apk')
Assert ((Get-ApkUpdatePlan $info).ApkPaths.Count -eq 2) 'missing split requires full set'
$script:remoteLines = @('package:/data/app/test/../base.apk', 'package:/data/app/test/split_data0.apk')
Assert (-not (Get-ApkUpdatePlan $info).Inherit) 'unsafe path disables inheritance'

# A one-file inherited update must use install-multiple -p <package>, never standalone install.
function Invoke-AdbTee { param($Arguments) $script:installArgs = $Arguments; return [pscustomobject]@{ ExitCode=0; Text='Success' } }
$null = Invoke-ApkInstall -ApkPaths @('base.apk') -Inherit
Assert (($script:installArgs -join ' ') -eq 'install-multiple -r -p com.rockstargames.rdr --no-incremental base.apk') 'inherit command'
$null = Invoke-ApkInstall -ApkPaths @('base.apk')
Assert ($script:installArgs[0] -eq 'install') 'standalone command'
Write-Host "PASS: build cache, input/key invalidation, partial install planning and ADB arguments. Fixtures: $testRoot"
