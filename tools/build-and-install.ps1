[CmdletBinding()]
param(
    [string]$GamePackage,
    [string]$WorkDir = 'C:\RDRVRBuild',
    [string]$AndroidSdk,
    [string]$JavaHome,
    [string]$Serial,
    [string]$LogPath,
    [string]$Keystore,
    [ValidateSet(-1, 0, 1)][int]$Trade = 0,
    [switch]$BuildOnly,
    [switch]$Rebuild,
    [switch]$NonInteractive
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$script:ModVersion = '0.1.1'
$script:PackageName = 'com.rockstargames.rdr'
$script:ExpectedVersionCode = '63226194'
$script:ExpectedVersionName = '1.58.63226194'
$script:ExpectedEngineSha256 = '3F80845EDEC32A663C42A8570E3131446813E695076C4BC96E2142C190BC22F3'
$script:ExpectedOpenXrLoader = '713E3BB8D955254C670ACC1C4899A65CB8C930E97DD9958BF37EA922D72B7A06'
$script:ThirdPartySigner = '358020B0ED9D910C3EB4E03E261EB340604833F8BDC75E236D965E80E63A56DE'
$script:VulkanHeadersRootName = 'Vulkan-Headers-vulkan-sdk-1.4.357.0'
$script:RemoteDataRoot = "/sdcard/Android/data/$($script:PackageName)"
$script:RemoteFilesRoot = "$($script:RemoteDataRoot)/files"
$script:RemoteObbRoot = "/sdcard/Android/obb/$($script:PackageName)"
$script:SelectedSerial = $null
$script:AdbExe = $null
$script:TranscriptStarted = $false

. (Join-Path $PSScriptRoot 'build-cache.ps1')

$script:PinnedFiles = @{
    Python = @{
        FileName = 'python-3.12.10-embed-amd64.zip'
        Uri = 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip'
        Sha256 = '4ACBED6DD1C744B0376E3B1CF57CE906F9DC9E95E68824584C8099A63025A3C3'
    }
    Java = @{
        FileName = 'OpenJDK21U-jdk_x64_windows_hotspot_21.0.11_10.zip'
        Uri = 'https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.11%2B10/OpenJDK21U-jdk_x64_windows_hotspot_21.0.11_10.zip'
        Sha256 = 'D3625E7CADF23787EA540229544B6E2AB494B3B54DA1801879E583E1DFEE0A64'
    }
    AndroidCommandLineTools = @{
        FileName = 'commandlinetools-win-15859902_latest.zip'
        Uri = 'https://dl.google.com/android/repository/commandlinetools-win-15859902_latest.zip'
        Sha256 = '90AE805D20434428BFFCB699C290860F19BB5F66A67E6B330067E3DE801FB04A'
    }
    PlatformTools = @{
        FileName = 'platform-tools_r37.0.1-win.zip'
        Uri = 'https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip'
        Sha256 = '45F4D63113E895EBDE0C90F194099A4676B6AC653BD28D54314A9E022BBC1A99'
    }
    OpenXr = @{
        FileName = 'openxr_loader_for_android-1.1.43.aar'
        Uri = 'https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.43/openxr_loader_for_android-1.1.43.aar'
        Sha256 = '7E1B36141F9A4F1FA4A7E061936344FD9FCD36BCE6C47EAE2AD09812736167B6'
    }
    VulkanHeaders = @{
        FileName = 'vulkan-sdk-1.4.357.0.zip'
        Uri = 'https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.357.0.zip'
        Sha256 = 'BC7925A17854A9BAC9433309E8DF88A701EF1D49D2020FFF6DB9407B637EA840'
    }
}

function Write-Step {
    param([Parameter(Mandatory = $true)][string]$Text)
    Write-Host "`n==> $Text" -ForegroundColor Cyan
}

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path.Trim().Trim('"').Trim("'"))
}

function Test-IsUnderPath {
    param(
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$Root
    )
    $candidateFull = Get-FullPath $Candidate
    $rootFull = (Get-FullPath $Root).TrimEnd('\') + '\'
    return $candidateFull.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-SafeWorkDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-FullPath $Path
    $root = [System.IO.Path]::GetPathRoot($full)
    if ([string]::IsNullOrWhiteSpace($root) -or $full.TrimEnd('\') -eq $root.TrimEnd('\')) {
        throw "WorkDir must be a dedicated folder, not a drive root: $full"
    }
    return $full
}

function Move-AsideIfPresent {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$AllowedRoot
    )
    if (-not (Test-Path -LiteralPath $Path)) { return }
    if (-not (Test-IsUnderPath -Candidate $Path -Root $AllowedRoot)) {
        throw "Refusing to replace a path outside the managed root: $Path"
    }
    $suffix = Get-Date -Format 'yyyyMMdd-HHmmss'
    $aside = "$Path.invalid-$suffix"
    Move-Item -LiteralPath $Path -Destination $aside
    Write-Warning "Preserved an invalid managed item at: $aside"
}

function Get-JsonProperty {
    param(
        $Object,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Write-TextFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text
    )
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Format-DownloadSize {
    param([Parameter(Mandatory = $true)][long]$Bytes)
    if ($Bytes -ge 1GB) { return ('{0:N2} GiB' -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ('{0:N1} MiB' -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ('{0:N1} KiB' -f ($Bytes / 1KB)) }
    return "$Bytes bytes"
}

function Invoke-DownloadWithProgress {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$OutFile,
        [Parameter(Mandatory = $true)][string]$DisplayName,
        [ValidateRange(15, 600)][int]$StallTimeoutSeconds = 90,
        [ValidateRange(1, 100)][int]$MaxAttempts = 30
    )

    # Slow or unstable connections drop long downloads ("unexpected EOF", resets, stalls). Every failure is
    # retried and the download continues where it stopped (HTTP Range) instead of starting over.
    Add-Type -AssemblyName System.Net.Http
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $true
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.Timeout = [System.Threading.Timeout]::InfiniteTimeSpan
    $client.DefaultRequestHeaders.UserAgent.ParseAdd("RDRVR-SourceKit/$($script:ModVersion)")
    $progressId = 1701
    [long]$written = 0
    $total = $null
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    [long]$sessionStart = 0
    $nextTextReport = 5.0
    $lastError = $null
    if (Test-Path -LiteralPath $OutFile) { Remove-Item -LiteralPath $OutFile -Force }
    try {
        Write-Host "Downloading $DisplayName"
        Write-Host "  Source: $Uri"
        Write-Host "  A dropped or stalled connection ($StallTimeoutSeconds s without data) is retried and resumed automatically."
        for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
            $response = $null; $source = $null; $target = $null
            $headerTimeout = [System.Threading.CancellationTokenSource]::new()
            $headerTimeout.CancelAfter([TimeSpan]::FromSeconds(60))
            try {
                $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Get, $Uri)
                if ($written -gt 0) {
                    $request.Headers.Range = [System.Net.Http.Headers.RangeHeaderValue]::new($written, $null)
                }
                try {
                    $response = $client.SendAsync(
                        $request,
                        [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead,
                        $headerTimeout.Token).GetAwaiter().GetResult()
                }
                catch [System.OperationCanceledException] {
                    throw "Timed out while connecting to $Uri"
                }
                $status = [int]$response.StatusCode
                if ($written -gt 0 -and $status -eq 200) {
                    Write-Host "  The server cannot resume; starting $DisplayName again."
                    $written = 0
                }
                elseif ($written -gt 0 -and $status -eq 416 -and $null -ne $total -and $written -eq $total) {
                    break
                }
                $response.EnsureSuccessStatusCode() | Out-Null
                if ($status -eq 200) {
                    $total = $response.Content.Headers.ContentLength
                }
                elseif ($status -eq 206 -and $null -ne $response.Content.Headers.ContentRange -and
                        $null -ne $response.Content.Headers.ContentRange.Length) {
                    $total = [long]$response.Content.Headers.ContentRange.Length
                }
                $source = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
                $mode = if ($written -gt 0) { [System.IO.FileMode]::Append } else { [System.IO.FileMode]::Create }
                $target = [System.IO.File]::Open($OutFile, $mode, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
                if ($attempt -gt 1) { Write-Host "  Resuming at $(Format-DownloadSize $written) (attempt $attempt of $MaxAttempts)" }
                $sessionStart = $written
                $timer.Restart()
                $buffer = New-Object byte[] (1MB)
                while ($true) {
                    $readTimeout = [System.Threading.CancellationTokenSource]::new()
                    $readTimeout.CancelAfter([TimeSpan]::FromSeconds($StallTimeoutSeconds))
                    try {
                        try {
                            $read = $source.ReadAsync($buffer, 0, $buffer.Length, $readTimeout.Token).GetAwaiter().GetResult()
                        }
                        catch [System.OperationCanceledException] {
                            throw "no data received for $StallTimeoutSeconds seconds"
                        }
                    }
                    finally { $readTimeout.Dispose() }
                    if ($read -eq 0) { break }
                    $target.Write($buffer, 0, $read)
                    $written += $read
                    $seconds = [Math]::Max($timer.Elapsed.TotalSeconds, 0.001)
                    $bytesPerSecond = ($written - $sessionStart) / $seconds
                    $rateText = "$(Format-DownloadSize ([long]$bytesPerSecond))/s"
                    $writtenText = Format-DownloadSize $written
                    if ($null -ne $total -and $total -gt 0) {
                        $percent = [Math]::Min(100, [int](100.0 * $written / $total))
                        $remainingSeconds = if ($bytesPerSecond -gt 0) { [Math]::Max(0, [int](($total - $written) / $bytesPerSecond)) } else { 0 }
                        $statusText = "$writtenText / $(Format-DownloadSize $total) - $percent% - $rateText - ETA $([TimeSpan]::FromSeconds($remainingSeconds).ToString('hh\:mm\:ss'))"
                        Write-Progress -Id $progressId -Activity "Downloading $DisplayName" -Status $statusText -PercentComplete $percent
                    }
                    else {
                        $statusText = "$writtenText received - $rateText"
                        Write-Progress -Id $progressId -Activity "Downloading $DisplayName" -Status $statusText
                    }
                    if ($timer.Elapsed.TotalSeconds -ge $nextTextReport) {
                        Write-Host "  $statusText"
                        $nextTextReport = $timer.Elapsed.TotalSeconds + 5.0
                    }
                }
                $target.Flush()
                if ($null -ne $total -and $total -ge 0 -and $written -lt $total) {
                    throw "the connection closed at $(Format-DownloadSize $written) of $(Format-DownloadSize $total)"
                }
                $lastError = $null
                break
            }
            catch {
                $lastError = $_.Exception.Message
                if ($attempt -ge $MaxAttempts) { break }
                $wait = [Math]::Min(30, 3 * $attempt)
                Write-Host "  Download interrupted ($lastError). Retrying in $wait s..." -ForegroundColor Yellow
                if ($null -ne $target) { $target.Dispose(); $target = $null }
                Start-Sleep -Seconds $wait
            }
            finally {
                if ($null -ne $target) { $target.Dispose() }
                if ($null -ne $source) { $source.Dispose() }
                if ($null -ne $response) { $response.Dispose() }
                $headerTimeout.Dispose()
            }
        }
        if ($null -ne $lastError) {
            throw "Downloading $DisplayName failed after $MaxAttempts attempts: $lastError. Check the internet connection and run BUILD_AND_INSTALL again."
        }
        if ($null -ne $total -and $total -ge 0 -and $written -ne $total) {
            throw "Incomplete download for $DisplayName. Expected $total bytes, received $written."
        }
        Write-Host "Downloaded ${DisplayName}: $(Format-DownloadSize $written)."
    }
    finally {
        Write-Progress -Id $progressId -Activity "Downloading $DisplayName" -Completed
        $client.Dispose()
        $handler.Dispose()
    }
}

function Get-VerifiedDownload {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Definition,
        [Parameter(Mandatory = $true)][string]$DownloadRoot
    )
    New-Item -ItemType Directory -Force -Path $DownloadRoot | Out-Null
    $destination = Join-Path $DownloadRoot $Definition.FileName
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $existingHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
        if ($existingHash -eq $Definition.Sha256) {
            Write-Host "Using verified cache: $destination"
            return $destination
        }
        Move-AsideIfPresent -Path $destination -AllowedRoot $DownloadRoot
    }

    $temporary = "$destination.download-$([Guid]::NewGuid().ToString('N'))"
    try {
        Invoke-DownloadWithProgress -Uri $Definition.Uri -OutFile $temporary `
            -DisplayName $Definition.FileName
        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash
        if ($actualHash -ne $Definition.Sha256) {
            throw "SHA256 mismatch for $($Definition.FileName). Expected $($Definition.Sha256), got $actualHash"
        }
        Move-Item -LiteralPath $temporary -Destination $destination
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
    return $destination
}

function Expand-ZipToFreshDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Archive,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$ManagedRoot
    )
    if (Test-Path -LiteralPath $Destination) {
        Move-AsideIfPresent -Path $Destination -AllowedRoot $ManagedRoot
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($Archive, $Destination)
}

function Invoke-NativeCapture {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure,
        [switch]$EchoOutput
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
    if ($EchoOutput.IsPresent -and -not [string]::IsNullOrWhiteSpace($text)) {
        Write-Host $text
    }
    if ($exitCode -ne 0 -and -not $AllowFailure.IsPresent) {
        throw "Command failed ($exitCode): $FilePath $($Arguments -join ' ')`n$text"
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Lines = $lines
        Text = $text
    }
}

function Get-NativeOutputLine {
    param([Parameter(Mandatory = $true)]$Item)
    $line = if ($Item -is [System.Management.Automation.ErrorRecord]) {
        $Item.Exception.Message
    } else {
        $Item.ToString()
    }
    if ($line -eq 'System.Management.Automation.RemoteException') { return '' }
    return $line
}

function Invoke-NativeLive {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$FailureMessage
    )
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $FilePath @Arguments 2>&1 | ForEach-Object {
            $line = Get-NativeOutputLine -Item $_
            if (-not [string]::IsNullOrWhiteSpace($line)) { Write-Host $line }
        }
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($exitCode -ne 0) {
        if (-not [string]::IsNullOrWhiteSpace($FailureMessage)) { throw "$FailureMessage (exit code $exitCode)" }
        throw "Command failed ($exitCode): $FilePath $($Arguments -join ' ')"
    }
}

function Invoke-NativeTee {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $FilePath @Arguments 2>&1 | ForEach-Object {
            $line = Get-NativeOutputLine -Item $_
            $lines.Add($line)
            if (-not [string]::IsNullOrWhiteSpace($line)) { Write-Host $line }
        }
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Lines = $lines.ToArray()
        Text = ($lines.ToArray() -join [Environment]::NewLine)
    }
}

function Invoke-AndroidSdkLicenses {
    param(
        [Parameter(Mandatory = $true)][string]$JavaExe,
        [Parameter(Mandatory = $true)][string]$Classpath,
        [Parameter(Mandatory = $true)][string]$SdkRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot,
        [switch]$VerifyOnly
    )
    # Source-file launch needs the JDK 21 resolved by this builder. The runner
    # feeds the answers to sdkmanager inside the JVM instead of through stdin.
    $answer = if ($VerifyOnly.IsPresent) { 'n' } else { 'y' }
    $source = @"
import java.io.*;
import java.nio.charset.StandardCharsets;
public class RdrvrSdkLicenseRunner {
    public static void main(String[] args) throws Exception {
        System.setIn(new ByteArrayInputStream("$answer\n".repeat(256).getBytes(StandardCharsets.UTF_8)));
        PrintStream console = System.out;
        ByteArrayOutputStream recorded = new ByteArrayOutputStream();
        System.setOut(new PrintStream(new OutputStream() {
            public void write(int b) { console.write(b); recorded.write(b); }
            public void write(byte[] b, int off, int len) {
                console.write(b, off, len); recorded.write(b, off, len);
            }
            public void flush() { console.flush(); }
        }, true, StandardCharsets.UTF_8));
        Class.forName("com.android.sdklib.tool.sdkmanager.SdkManagerCli")
            .getMethod("main", String[].class).invoke(null, (Object)args);
        System.out.flush();
        if (!recorded.toString(StandardCharsets.UTF_8).contains("All SDK package licenses accepted")) {
            throw new IllegalStateException("SDK did not confirm acceptance of all licenses.");
        }
    }
}
"@
    New-Item -ItemType Directory -Force -Path $ToolsRoot | Out-Null
    $runner = Join-Path $ToolsRoot 'RdrvrSdkLicenseRunner.java'
    Write-TextFile -Path $runner -Text $source
    Invoke-NativeLive -FilePath $JavaExe -Arguments @(
        '-Dfile.encoding=UTF-8', '-cp', $Classpath, $runner, "--sdk_root=$SdkRoot", '--licenses'
    ) -FailureMessage 'The Android SDK licenses are not all accepted'
}

function Read-PathWithDialog {
    param(
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$Filter,
        [string]$StartFolder
    )
    $owner = $null
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $owner = New-Object System.Windows.Forms.Form
        $owner.TopMost = $true
        $choice = [System.Windows.Forms.MessageBox]::Show(
            $owner,
            "$Title`n`nYes: choose the export folder (base.apk and split_*.apk)`nNo: choose a single APK, or an .apks or .zip archive`nCancel: type the path in the console window",
            'Red Dead Redemption VR source kit',
            [System.Windows.Forms.MessageBoxButtons]::YesNoCancel,
            [System.Windows.Forms.MessageBoxIcon]::Information
        )
        $startExists = -not [string]::IsNullOrWhiteSpace($StartFolder) -and
            (Test-Path -LiteralPath $StartFolder -PathType Container)
        if ($choice -eq [System.Windows.Forms.DialogResult]::Yes) {
            $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
            $dialog.Description = $Title
            $dialog.ShowNewFolderButton = $false
            if ($startExists) { $dialog.SelectedPath = $StartFolder }
            if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {
                return $dialog.SelectedPath
            }
        }
        elseif ($choice -eq [System.Windows.Forms.DialogResult]::No) {
            $dialog = New-Object System.Windows.Forms.OpenFileDialog
            $dialog.Title = $Title
            $dialog.Filter = $Filter
            $dialog.CheckFileExists = $true
            if ($startExists) { $dialog.InitialDirectory = $StartFolder }
            if ($dialog.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {
                return $dialog.FileName
            }
        }
    }
    catch {
        Write-Verbose "Graphical path prompt is unavailable: $($_.Exception.Message)"
    }
    finally {
        if ($null -ne $owner) { $owner.Dispose() }
    }
    return $null
}

function Resolve-RequiredInputPath {
    param(
        [string]$Value,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Prompt,
        [Parameter(Mandatory = $true)][string]$Filter,
        [string]$StartFolder
    )
    $candidate = $Value
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        if ($NonInteractive.IsPresent) {
            throw "$Name is required with -NonInteractive."
        }
        $candidate = Read-PathWithDialog -Title $Prompt -Filter $Filter -StartFolder $StartFolder
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            $candidate = Read-Host "$Prompt (paste a folder or file path)"
        }
    }
    if ([string]::IsNullOrWhiteSpace($candidate)) { throw "$Name was not provided." }
    $candidate = Get-FullPath $candidate
    if (-not (Test-Path -LiteralPath $candidate)) { throw "$Name does not exist: $candidate" }
    return $candidate
}

function Confirm-ExactText {
    param(
        [Parameter(Mandatory = $true)][string]$Message,
        [Parameter(Mandatory = $true)][string]$RequiredText,
        [switch]$IgnoreCase
    )
    if ($NonInteractive.IsPresent) { return $false }
    Write-Warning $Message
    $answer = Read-Host "Type $RequiredText to continue"
    if ($null -eq $answer) { return $false }
    if ($IgnoreCase.IsPresent) {
        return $answer.Trim() -ieq $RequiredText
    }
    return $answer -ceq $RequiredText
}

function Resolve-JavaHome {
    param(
        [string]$Requested,
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot
    )
    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        $requestedFull = Get-FullPath $Requested
        $javac = Join-Path $requestedFull 'bin\javac.exe'
        if (-not (Test-Path -LiteralPath $javac -PathType Leaf)) {
            throw "JDK javac.exe is missing: $javac"
        }
        $version = Invoke-NativeCapture -FilePath $javac -Arguments @('-version') -AllowFailure
        if ($version.ExitCode -ne 0 -or $version.Text -notmatch 'javac\s+21(?:\.|\s|$)') {
            throw "-JavaHome must point to JDK 21. Reported: $($version.Text)"
        }
        return $requestedFull
    }

    $destination = Join-Path $ToolsRoot 'temurin-jdk-21.0.11+10'
    $knownJavac = Join-Path $destination 'bin\javac.exe'
    if (-not (Test-Path -LiteralPath $knownJavac -PathType Leaf)) {
        $archive = Get-VerifiedDownload -Definition $script:PinnedFiles.Java -DownloadRoot $DownloadRoot
        $temporary = Join-Path $ToolsRoot ('.extract-jdk-' + [Guid]::NewGuid().ToString('N'))
        Expand-ZipToFreshDirectory -Archive $archive -Destination $temporary -ManagedRoot $ToolsRoot
        try {
            $root = Get-ChildItem -LiteralPath $temporary -Directory |
                Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin\javac.exe') -PathType Leaf } |
                Select-Object -First 1
            if ($null -eq $root) { throw 'Pinned JDK archive did not contain bin\javac.exe.' }
            if (Test-Path -LiteralPath $destination) {
                Move-AsideIfPresent -Path $destination -AllowedRoot $ToolsRoot
            }
            Move-Item -LiteralPath $root.FullName -Destination $destination
        }
        finally {
            if (Test-Path -LiteralPath $temporary) {
                Remove-Item -LiteralPath $temporary -Recurse -Force
            }
        }
    }
    $version = Invoke-NativeCapture -FilePath $knownJavac -Arguments @('-version') -AllowFailure
    if ($version.ExitCode -ne 0 -or $version.Text -notmatch 'javac\s+21\.0\.11') {
        throw "Pinned JDK validation failed: $($version.Text)"
    }
    return $destination
}

function Resolve-Python {
    param(
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot
    )
    $destination = Join-Path $ToolsRoot 'python-3.12.10-embed-amd64'
    $python = Join-Path $destination 'python.exe'
    if (-not (Test-Path -LiteralPath $python -PathType Leaf)) {
        $archive = Get-VerifiedDownload -Definition $script:PinnedFiles.Python -DownloadRoot $DownloadRoot
        Expand-ZipToFreshDirectory -Archive $archive -Destination $destination -ManagedRoot $ToolsRoot
    }
    $version = Invoke-NativeCapture -FilePath $python -Arguments @('--version') -AllowFailure
    if ($version.ExitCode -ne 0 -or $version.Text -notmatch 'Python\s+3\.12\.10') {
        throw "Pinned Python validation failed: $($version.Text)"
    }
    return $python
}

function Resolve-PlatformTools {
    param(
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot
    )
    $destination = Join-Path $ToolsRoot 'platform-tools-37.0.1'
    $adb = Join-Path $destination 'platform-tools\adb.exe'
    if (-not (Test-Path -LiteralPath $adb -PathType Leaf)) {
        $archive = Get-VerifiedDownload -Definition $script:PinnedFiles.PlatformTools -DownloadRoot $DownloadRoot
        Expand-ZipToFreshDirectory -Archive $archive -Destination $destination -ManagedRoot $ToolsRoot
    }
    $version = Invoke-NativeCapture -FilePath $adb -Arguments @('version') -AllowFailure
    if ($version.ExitCode -ne 0 -or $version.Text -notmatch 'Version\s+37\.0\.1-') {
        throw "Pinned ADB validation failed: $($version.Text)"
    }
    return $adb
}

function Resolve-OpenXrLoader {
    param(
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot
    )
    $destinationRoot = Join-Path $ToolsRoot 'openxr-loader-1.1.43'
    $destination = Join-Path $destinationRoot 'libopenxr_loader.so'
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
        if ($hash -eq $script:ExpectedOpenXrLoader) { return $destination }
        Move-AsideIfPresent -Path $destinationRoot -AllowedRoot $ToolsRoot
    }

    $archivePath = Get-VerifiedDownload -Definition $script:PinnedFiles.OpenXr -DownloadRoot $DownloadRoot
    New-Item -ItemType Directory -Force -Path $destinationRoot | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $entryName = 'prefab/modules/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so'
        $entry = $archive.GetEntry($entryName)
        if ($null -eq $entry) { throw "Pinned OpenXR AAR is missing $entryName" }
        $entryStream = $entry.Open()
        try {
            $output = [System.IO.File]::Open($destination, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
            try { $entryStream.CopyTo($output) }
            finally { $output.Dispose() }
        }
        finally { $entryStream.Dispose() }
    }
    finally { $archive.Dispose() }
    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash
    if ($actualHash -ne $script:ExpectedOpenXrLoader) {
        throw "Extracted OpenXR loader hash mismatch. Expected $($script:ExpectedOpenXrLoader), got $actualHash"
    }
    return $destination
}

function Resolve-VulkanHeaders {
    param(
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot
    )
    $destination = Join-Path $ToolsRoot 'vulkan-headers-1.4.357.0'
    $headersRoot = Join-Path $destination $script:VulkanHeadersRootName
    $marker = Join-Path $headersRoot 'include\vulkan\vulkan_core.h'
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        $archive = Get-VerifiedDownload -Definition $script:PinnedFiles.VulkanHeaders -DownloadRoot $DownloadRoot
        $temporary = Join-Path $ToolsRoot ('.extract-vulkan-' + [Guid]::NewGuid().ToString('N'))
        Expand-ZipToFreshDirectory -Archive $archive -Destination $temporary -ManagedRoot $ToolsRoot
        try {
            $extractedMarker = Join-Path $temporary "$($script:VulkanHeadersRootName)\include\vulkan\vulkan_core.h"
            if (-not (Test-Path -LiteralPath $extractedMarker -PathType Leaf)) {
                throw 'Pinned Vulkan-Headers archive has an unexpected layout.'
            }
            if (Test-Path -LiteralPath $destination) {
                Move-AsideIfPresent -Path $destination -AllowedRoot $ToolsRoot
            }
            Move-Item -LiteralPath $temporary -Destination $destination
        }
        finally {
            if (Test-Path -LiteralPath $temporary) {
                Remove-Item -LiteralPath $temporary -Recurse -Force
            }
        }
    }
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        throw "Vulkan headers are incomplete: $marker"
    }
    return $headersRoot
}

function Install-AndroidCommandLineTools {
    param(
        [Parameter(Mandatory = $true)][string]$SdkRoot,
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot
    )
    $destination = Join-Path $SdkRoot 'cmdline-tools\15859902'
    $sdkManagerClasspath = Join-Path $destination 'lib\sdkmanager-classpath.jar'
    if (Test-Path -LiteralPath $sdkManagerClasspath -PathType Leaf) { return $sdkManagerClasspath }

    $archive = Get-VerifiedDownload -Definition $script:PinnedFiles.AndroidCommandLineTools -DownloadRoot $DownloadRoot
    $temporary = Join-Path $ToolsRoot ('.extract-android-cli-' + [Guid]::NewGuid().ToString('N'))
    Expand-ZipToFreshDirectory -Archive $archive -Destination $temporary -ManagedRoot $ToolsRoot
    try {
        $source = Join-Path $temporary 'cmdline-tools'
        if (-not (Test-Path -LiteralPath (Join-Path $source 'lib\sdkmanager-classpath.jar') -PathType Leaf)) {
            throw 'Pinned Android command-line tools archive is missing sdkmanager-classpath.jar.'
        }
        $destinationParent = Split-Path -Parent $destination
        New-Item -ItemType Directory -Force -Path $destinationParent | Out-Null
        if (Test-Path -LiteralPath $destination) {
            $aside = "$destination.invalid-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
            Move-Item -LiteralPath $destination -Destination $aside
            Write-Warning "Preserved an invalid Android CLI directory at: $aside"
        }
        Move-Item -LiteralPath $source -Destination $destination
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Recurse -Force
        }
    }
    return $sdkManagerClasspath
}

function Test-AndroidPackages {
    param([Parameter(Mandatory = $true)][string]$SdkRoot)
    $requiredFiles = @(
        (Join-Path $SdkRoot 'platforms\android-35\android.jar'),
        (Join-Path $SdkRoot 'build-tools\35.0.0\aapt2.exe'),
        (Join-Path $SdkRoot 'build-tools\35.0.0\zipalign.exe'),
        (Join-Path $SdkRoot 'build-tools\35.0.0\lib\apksigner.jar'),
        (Join-Path $SdkRoot 'build-tools\35.0.0\lib\d8.jar'),
        (Join-Path $SdkRoot 'ndk\27.2.12479018\build\cmake\android.toolchain.cmake'),
        (Join-Path $SdkRoot 'cmake\3.22.1\bin\cmake.exe'),
        (Join-Path $SdkRoot 'cmake\3.22.1\bin\ninja.exe')
    )
    return @($requiredFiles | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
}

function Resolve-AndroidSdk {
    param(
        [string]$Requested,
        [Parameter(Mandatory = $true)][string]$DefaultRoot,
        [Parameter(Mandatory = $true)][string]$DownloadRoot,
        [Parameter(Mandatory = $true)][string]$ToolsRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedJavaHome
    )
    $sdkRoot = if ([string]::IsNullOrWhiteSpace($Requested)) { $DefaultRoot } else { Get-FullPath $Requested }
    New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null
    $missing = @(Test-AndroidPackages -SdkRoot $sdkRoot)
    if ($missing.Count -eq 0) { return $sdkRoot }
    Write-Host 'Missing Android SDK components:'
    foreach ($path in $missing) { Write-Host "  $path" }

    $sdkManagerClasspath = Install-AndroidCommandLineTools -SdkRoot $sdkRoot -DownloadRoot $DownloadRoot -ToolsRoot $ToolsRoot
    $javaExe = Join-Path $ResolvedJavaHome 'bin\java.exe'
    $sdkManagerMain = 'com.android.sdklib.tool.sdkmanager.SdkManagerCli'
    $oldJavaHome = $env:JAVA_HOME
    $oldSdkRoot = $env:ANDROID_SDK_ROOT
    try {
        $env:JAVA_HOME = $ResolvedJavaHome
        $env:ANDROID_SDK_ROOT = $sdkRoot
        $licenseMarker = Join-Path $sdkRoot 'licenses\android-sdk-license'
        $accepted = $false
        if (Test-Path -LiteralPath $licenseMarker -PathType Leaf) {
            Write-Host 'Checking the recorded Android SDK license acceptance...'
            try {
                Invoke-AndroidSdkLicenses -JavaExe $javaExe -Classpath $sdkManagerClasspath -SdkRoot $sdkRoot -ToolsRoot $ToolsRoot -VerifyOnly
                $accepted = $true
            }
            catch {
                Write-Warning "Some Android SDK licenses are not accepted yet: $($_.Exception.Message)"
            }
        }
        if (-not $accepted) {
            if ($NonInteractive.IsPresent) {
                throw "Android SDK licenses have not been accepted in $sdkRoot. Run BUILD_AND_INSTALL once without -NonInteractive."
            }
            $confirmed = Confirm-ExactText `
                -Message 'The Android SDK platform, build tools, NDK and CMake are installed with Google sdkmanager, which asks to accept the Android SDK License Agreement (https://developer.android.com/studio/terms) and the other SDK package licenses it lists. Type ACCEPT only if you agree to all of them; the builder then answers each sdkmanager license prompt with yes.' `
                -RequiredText 'ACCEPT' `
                -IgnoreCase
            if (-not $confirmed) {
                throw 'Android SDK licenses were not accepted. No Android components were installed.'
            }
            Write-Host 'Recording Android SDK license acceptance...' -ForegroundColor Yellow
            Invoke-AndroidSdkLicenses -JavaExe $javaExe -Classpath $sdkManagerClasspath -SdkRoot $sdkRoot -ToolsRoot $ToolsRoot
            if (-not (Test-Path -LiteralPath $licenseMarker -PathType Leaf)) {
                throw "sdkmanager reported acceptance but did not record the Android SDK license in $sdkRoot."
            }
            Write-Host 'Android SDK licenses accepted.' -ForegroundColor Green
        }
        $packages = @(
            'platforms;android-35',
            'build-tools;35.0.0',
            'ndk;27.2.12479018',
            'cmake;3.22.1'
        )
        Write-Host 'Installing Android SDK components. The NDK download is large; this takes a while.'
        Invoke-NativeLive -FilePath $javaExe -Arguments (
            @('-cp', $sdkManagerClasspath, $sdkManagerMain, "--sdk_root=$sdkRoot") + $packages
        ) -FailureMessage 'sdkmanager could not install the Android SDK components'
    }
    finally {
        $env:JAVA_HOME = $oldJavaHome
        $env:ANDROID_SDK_ROOT = $oldSdkRoot
    }
    $missing = @(Test-AndroidPackages -SdkRoot $sdkRoot)
    if ($missing.Count -ne 0) {
        throw "Android SDK installation is incomplete:`n$($missing -join [Environment]::NewLine)"
    }
    return $sdkRoot
}

function Get-PersistentKeystorePath {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedWorkDir
    )
    $localAppData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
    if ([string]::IsNullOrWhiteSpace($localAppData)) { $localAppData = $env:LOCALAPPDATA }
    if ([string]::IsNullOrWhiteSpace($localAppData)) { throw 'LOCALAPPDATA is unavailable; cannot place the persistent signing key safely.' }
    $path = Join-Path $localAppData 'RDRVRBuilder\signing\rdrvr.keystore'
    if ((Test-IsUnderPath -Candidate $path -Root $ProjectRoot) -or
        (Test-IsUnderPath -Candidate $path -Root $ResolvedWorkDir)) {
        $roaming = [Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData)
        $path = Join-Path $roaming 'RDRVRBuilder\signing\rdrvr.keystore'
    }
    if ((Test-IsUnderPath -Candidate $path -Root $ProjectRoot) -or
        (Test-IsUnderPath -Candidate $path -Root $ResolvedWorkDir)) {
        throw 'Cannot choose a persistent keystore path outside the source kit and WorkDir.'
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    return $path
}

function Invoke-KitBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Python,
        [Parameter(Mandatory = $true)][string]$BuildScript,
        [Parameter(Mandatory = $true)][string]$GamePackagePath,
        [Parameter(Mandatory = $true)][string]$BuildRoot,
        [Parameter(Mandatory = $true)][string]$SdkRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedJavaHome,
        [Parameter(Mandatory = $true)][string]$OpenXrLoader,
        [Parameter(Mandatory = $true)][string]$VulkanHeaders,
        [Parameter(Mandatory = $true)][string]$KeystorePath,
        [string]$ReuseBuild
    )
    $arguments = @(
        '-u', $BuildScript,
        '--game-package', $GamePackagePath,
        '--build-dir', $BuildRoot,
        '--sdk', $SdkRoot,
        '--java-home', $ResolvedJavaHome,
        '--openxr-loader', $OpenXrLoader,
        '--vulkan-headers', $VulkanHeaders,
        '--keystore', $KeystorePath,
        '--trade', ([string]$Trade)
    )
    $oldJavaHome = $env:JAVA_HOME
    if (-not [string]::IsNullOrWhiteSpace($ReuseBuild)) { $arguments += @('--reuse-build', $ReuseBuild) }
    $oldSdkRoot = $env:ANDROID_SDK_ROOT
    $oldAndroidHome = $env:ANDROID_HOME
    try {
        $env:JAVA_HOME = $ResolvedJavaHome
        $env:ANDROID_SDK_ROOT = $SdkRoot
        $env:ANDROID_HOME = $SdkRoot
        Invoke-NativeLive -FilePath $Python -Arguments $arguments `
            -FailureMessage "The build failed. Read the ERROR line above; step logs are in $(Join-Path $BuildRoot 'logs')"
    }
    finally {
        $env:JAVA_HOME = $oldJavaHome
        $env:ANDROID_SDK_ROOT = $oldSdkRoot
        $env:ANDROID_HOME = $oldAndroidHome
    }
}

function Resolve-ManifestFile {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )
    $normalized = $RelativePath.Replace('/', '\')
    if ([System.IO.Path]::IsPathRooted($normalized)) { throw "Manifest path must be relative: $RelativePath" }
    $full = Get-FullPath (Join-Path $Root $normalized)
    if (-not (Test-IsUnderPath -Candidate $full -Root $Root)) {
        throw "Manifest path escapes its root: $RelativePath"
    }
    return $full
}

function Get-ExpectedInstalledApkLeaf {
    param([Parameter(Mandatory = $true)]$Record)
    $split = Get-JsonProperty $Record 'split'
    if ($null -eq $split) { return 'base.apk' }
    $safeSplit = ([string]$split) -replace '[^A-Za-z0-9_.-]+', '_'
    if ([string]::IsNullOrWhiteSpace($safeSplit)) { throw 'APK manifest contains an empty split id.' }
    return "split_$safeSplit.apk"
}

function ConvertTo-SignerHashValue {
    param([Parameter(Mandatory = $true)][string]$Text)
    $clean = $Text.Trim()
    if ($clean -notmatch '^[0-9a-fA-F]{1,8}$') { throw "Unexpected signer hash value: $Text" }
    return [Convert]::ToUInt32($clean, 16)
}

function Assert-BuildArtifacts {
    param([Parameter(Mandatory = $true)][string]$BuildRoot)
    $manifestPath = Join-Path $BuildRoot 'build-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Build manifest is missing: $manifestPath"
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $expected = [ordered]@{
        formatVersion = '1'
        modVersion = $script:ModVersion
        package = $script:PackageName
        versionCode = $script:ExpectedVersionCode
        versionName = $script:ExpectedVersionName
    }
    foreach ($key in $expected.Keys) {
        if ([string](Get-JsonProperty $manifest $key) -ne $expected[$key]) {
            throw "Build manifest field '$key' is unexpected: $(Get-JsonProperty $manifest $key)"
        }
    }
    if ([string](Get-JsonProperty $manifest 'engineSha256') -ne $script:ExpectedEngineSha256) {
        throw 'Build manifest engine hash is not the supported librdr.so.'
    }
    $manifestTrade = Get-JsonProperty $manifest 'trade'
    if ($null -eq $manifestTrade -or [int]$manifestTrade -ne $Trade) {
        throw "Build manifest trade value $manifestTrade does not match the requested -Trade $Trade."
    }
    if ([string](Get-JsonProperty $manifest 'buildId') -notmatch '^[0-9a-fA-F]{64}$') {
        throw 'Build manifest build id is missing.'
    }
    if ([string](Get-JsonProperty $manifest 'sourceSignerSha256') -eq $script:ThirdPartySigner) {
        throw 'The game package was modified by a third party. Export the game from the phone with EXPORT_RDR_APKS.bat.'
    }
    if ([string](Get-JsonProperty $manifest 'outputSignerSha256') -notmatch '^[0-9a-fA-F]{64}$') {
        throw 'Output signer is missing from the build manifest.'
    }
    $signerHash = ConvertTo-SignerHashValue -Text ([string](Get-JsonProperty $manifest 'outputSignerHashCode'))

    $outputRoot = Join-Path $BuildRoot 'out'
    $outputs = @(Get-JsonProperty $manifest 'outputs' | Where-Object { $null -ne $_ })
    if ($outputs.Count -eq 0) { throw 'Build manifest contains no APK outputs.' }
    $bases = @($outputs | Where-Object { $null -eq (Get-JsonProperty $_ 'split') })
    if ($bases.Count -ne 1) { throw "Build manifest must contain exactly one base APK, found $($bases.Count)." }
    $leaves = @{}
    [int64]$apkBytes = 0
    foreach ($record in $outputs) {
        $leaf = Get-ExpectedInstalledApkLeaf -Record $record
        if ($leaves.ContainsKey($leaf)) { throw "Build manifest contains duplicate APK split: $leaf" }
        $leaves[$leaf] = $true
        $path = Resolve-ManifestFile -Root $outputRoot -RelativePath ([string]$record.file)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Built APK is missing: $path" }
        $item = Get-Item -LiteralPath $path
        if ([int64]$item.Length -ne [int64]$record.size) { throw "Built APK size mismatch: $path" }
        Write-Host "Hashing $($item.Name) ($(Format-DownloadSize $item.Length))..."
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
        if ($hash -ne ([string]$record.sha256).ToUpperInvariant()) { throw "Built APK hash mismatch: $path" }
        $apkBytes += [int64]$item.Length
    }
    $ordered = @($bases[0]) + @($outputs | Where-Object { $null -ne (Get-JsonProperty $_ 'split') } | Sort-Object -Property file)
    $apkPaths = @($ordered | ForEach-Object { Resolve-ManifestFile -Root $outputRoot -RelativePath ([string]$_.file) })
    return [pscustomobject]@{
        Manifest = $manifest
        ManifestPath = $manifestPath
        OutputRoot = $outputRoot
        ApkPaths = $apkPaths
        ApkBytes = $apkBytes
        SignerHash = $signerHash
    }
}

function Resolve-ExternalDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$TreeName
    )
    $rootFull = Get-FullPath $Root
    $candidates = @($rootFull, (Join-Path $rootFull 'external'))
    $parent = Split-Path -Parent $rootFull
    if (-not [string]::IsNullOrWhiteSpace($parent)) { $candidates += $parent }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate $TreeName) -PathType Container) { return $candidate }
    }
    throw "The external $TreeName folder of the game export was not found near $rootFull"
}

function Get-ExternalPlan {
    param([Parameter(Mandatory = $true)]$Manifest)
    $plan = @()
    $external = Get-JsonProperty $Manifest 'external'
    if ($null -eq $external) { return $plan }
    $rootValue = [string](Get-JsonProperty $external 'root')
    $trees = @(
        [pscustomobject]@{
            Name = 'obb'; NestedLeaf = $script:PackageName
            RemoteParent = '/sdcard/Android/obb'; RemoteFinal = $script:RemoteObbRoot
        },
        [pscustomobject]@{
            Name = 'assetpacks'; NestedLeaf = 'assetpacks'
            RemoteParent = $script:RemoteFilesRoot; RemoteFinal = "$($script:RemoteFilesRoot)/assetpacks"
        }
    )
    foreach ($tree in $trees) {
        $listed = Get-JsonProperty $external $tree.Name
        $records = @($listed | Where-Object { $null -ne $_ })
        if ($records.Count -eq 0) { continue }
        if ([string]::IsNullOrWhiteSpace($rootValue)) {
            throw "The build manifest lists external $($tree.Name) files but no export folder."
        }
        $externalDirectory = Resolve-ExternalDirectory -Root $rootValue -TreeName $tree.Name
        $treeRoot = Join-Path $externalDirectory $tree.Name
        # The exporter pulls a device folder into external/<name>, which can add
        # one level named like the device folder.
        $contentRoot = $treeRoot
        $entries = @(Get-ChildItem -LiteralPath $treeRoot -Force)
        if ($entries.Count -eq 1 -and $entries[0].PSIsContainer -and $entries[0].Name -eq $tree.NestedLeaf) {
            $contentRoot = $entries[0].FullName
        }
        $localFiles = @(Get-ChildItem -LiteralPath $contentRoot -File -Recurse -Force)
        if ($localFiles.Count -ne $records.Count) {
            throw "The external $($tree.Name) folder changed after the build: $treeRoot"
        }
        $prefix = (Get-FullPath $contentRoot).TrimEnd('\') + '\'
        $used = @{}
        $items = @()
        [int64]$totalBytes = 0
        foreach ($file in $localFiles) {
            $relative = $file.FullName.Substring($prefix.Length).Replace('\', '/')
            foreach ($component in $relative.Split('/')) {
                if ($component -notmatch '^[A-Za-z0-9_.+=-]+$' -or $component -in @('.', '..')) {
                    throw "External file name is unsafe for ADB: $($file.FullName)"
                }
            }
            $exact = @($records | Where-Object { ([string]$_.path).Replace('\', '/').TrimStart('/') -eq $relative })
            $match = $exact
            if ($exact.Count -ne 1) {
                $match = @($records | Where-Object { ([string]$_.path).Replace('\', '/').EndsWith('/' + $relative) })
            }
            if ($match.Count -ne 1) { throw "External file is not listed in the build manifest: $($file.FullName)" }
            $key = [string]$match[0].path
            if ($used.ContainsKey($key)) { throw "External file is listed twice: $key" }
            $used[$key] = $true
            if ([int64]$file.Length -ne [int64]$match[0].size) { throw "External file changed after the build: $($file.FullName)" }
            Write-Host "Hashing external $($tree.Name) file $relative..."
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
            if ($hash -ne ([string]$match[0].sha256).ToUpperInvariant()) {
                throw "External file changed after the build: $($file.FullName)"
            }
            $items += [pscustomobject]@{ Relative = $relative; Size = [int64]$file.Length; Sha256 = $hash.ToLowerInvariant() }
            $totalBytes += [int64]$file.Length
        }
        $leaf = Split-Path -Leaf $contentRoot
        if ($leaf -notmatch '^[A-Za-z0-9_.-]+$') { throw "External folder name is unsafe for ADB: $contentRoot" }
        $plan += [pscustomobject]@{
            Name = $tree.Name
            ContentRoot = $contentRoot
            Leaf = $leaf
            RemoteParent = $tree.RemoteParent
            RemoteFinal = $tree.RemoteFinal
            Files = $items
            TotalBytes = $totalBytes
        }
    }
    return $plan
}

function Invoke-AdbCapture {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure,
        [switch]$EchoOutput,
        [switch]$WithoutSerial
    )
    if ([string]::IsNullOrWhiteSpace([string]$script:AdbExe)) { throw 'ADB has not been initialized.' }
    $allArguments = @()
    if (-not $WithoutSerial.IsPresent) {
        if ([string]::IsNullOrWhiteSpace([string]$script:SelectedSerial)) { throw 'No Quest serial has been selected.' }
        $allArguments += @('-s', $script:SelectedSerial)
    }
    $allArguments += $Arguments
    return Invoke-NativeCapture -FilePath $script:AdbExe -Arguments $allArguments -AllowFailure:$AllowFailure -EchoOutput:$EchoOutput
}

function Invoke-AdbTee {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    if ([string]::IsNullOrWhiteSpace([string]$script:SelectedSerial)) { throw 'No Quest serial has been selected.' }
    return Invoke-NativeTee -FilePath $script:AdbExe -Arguments (@('-s', $script:SelectedSerial) + $Arguments)
}

function Get-AdbDeviceList {
    $result = Invoke-AdbCapture -Arguments @('devices', '-l') -WithoutSerial -AllowFailure
    $devices = @()
    foreach ($line in $result.Lines) {
        if ($line -match '^(\S+)\s+(device|unauthorized|offline|no permissions|authorizing|connecting)\b\s*(.*)$') {
            $devices += [pscustomobject]@{ Serial = $Matches[1]; State = $Matches[2]; Detail = $Matches[3] }
        }
    }
    return $devices
}

function Write-QuestConnectionGuidance {
    Write-Host ''
    Write-Host 'WAITING FOR THE QUEST HEADSET...' -ForegroundColor Yellow
    Write-Host '  1. Developer Mode must be on: Meta Horizon phone app > Devices > (headset) > Headset settings > Developer mode. Restart the headset after switching it on.'
    Write-Host '  2. Connect the headset to this PC with a USB-C data cable. A charge-only cable is not detected.'
    Write-Host '  3. Put on the headset and accept Allow USB debugging. Tick Always allow from this computer.'
    Write-Host 'The installer continues automatically when the headset is ready (up to 2 minutes).' -ForegroundColor Cyan
}

function Get-QuestConnectionError {
    param(
        [AllowEmptyCollection()][object[]]$Devices,
        [string]$RequestedSerial
    )
    $pool = @($Devices)
    if (-not [string]::IsNullOrWhiteSpace($RequestedSerial)) {
        $pool = @($pool | Where-Object { $_.Serial -eq $RequestedSerial })
        if ($pool.Count -eq 0) { return "The requested device $RequestedSerial was not found by ADB." }
    }
    if (@($pool | Where-Object { $_.State -eq 'unauthorized' -or $_.State -eq 'authorizing' }).Count -gt 0) {
        return 'The headset is connected but USB debugging is not allowed yet. Put on the headset, accept Allow USB debugging (tick Always allow from this computer), then run the installer again.'
    }
    if (@($pool | Where-Object { $_.State -eq 'offline' -or $_.State -eq 'connecting' }).Count -gt 0) {
        return 'The headset connection is offline. Reconnect the USB cable or use another USB port, then run the installer again.'
    }
    if (@($pool | Where-Object { $_.State -eq 'no permissions' }).Count -gt 0) {
        return 'ADB has no permission to access the headset over USB. Reconnect the cable and accept the prompt in the headset.'
    }
    return 'No Quest headset was found over ADB. Check that Developer Mode is on, use a USB-C data cable, accept Allow USB debugging in the headset, then run the installer again. If the headset still does not appear, install the Meta Quest ADB driver for Windows.'
}

function Wait-QuestDevice {
    param(
        [string]$RequestedSerial,
        [int]$TimeoutSeconds = 120
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $guidanceShown = $false
    while ($true) {
        $devices = @(Get-AdbDeviceList)
        $ready = @($devices | Where-Object { $_.State -eq 'device' })
        if (-not [string]::IsNullOrWhiteSpace($RequestedSerial)) {
            $ready = @($ready | Where-Object { $_.Serial -eq $RequestedSerial })
        }
        if ($ready.Count -gt 0) {
            Write-Progress -Id 7 -Activity 'Waiting for the Quest headset' -Completed
            return $ready
        }
        if (-not $guidanceShown) {
            Write-QuestConnectionGuidance
            $guidanceShown = $true
        }
        $remaining = [Math]::Max(0, [int][Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds))
        if ($remaining -le 0) {
            Write-Progress -Id 7 -Activity 'Waiting for the Quest headset' -Completed
            throw (Get-QuestConnectionError -Devices $devices -RequestedSerial $RequestedSerial)
        }
        $stateText = if (@($devices | Where-Object { $_.State -eq 'unauthorized' }).Count -gt 0) {
            'Headset found; accept Allow USB debugging inside the headset'
        } else { 'No headset yet; connect it with a USB-C data cable' }
        Write-Progress -Id 7 -Activity 'Waiting for the Quest headset' `
            -Status "$stateText ($remaining seconds remaining)" `
            -PercentComplete ([Math]::Min(100, [Math]::Max(0, ($TimeoutSeconds - $remaining) * 100 / $TimeoutSeconds)))
        Start-Sleep -Seconds 2
    }
}

function Test-IsWifiSerial {
    param([Parameter(Mandatory = $true)][string]$DeviceSerial)
    return $DeviceSerial -match ':' -or $DeviceSerial -match '(?i)_adb-tls-connect|\._tcp'
}

function Select-QuestDevice {
    param([string]$RequestedSerial)
    Invoke-AdbCapture -Arguments @('start-server') -WithoutSerial -AllowFailure | Out-Null
    $ready = @(Wait-QuestDevice -RequestedSerial $RequestedSerial)
    if (-not [string]::IsNullOrWhiteSpace($RequestedSerial)) {
        $script:SelectedSerial = $RequestedSerial
    }
    elseif ($ready.Count -eq 1) {
        $script:SelectedSerial = $ready[0].Serial
    }
    elseif ($NonInteractive.IsPresent) {
        throw 'More than one ADB device is connected. Use -Serial with -NonInteractive.'
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

    $model = (Invoke-AdbCapture -Arguments @('shell', 'getprop', 'ro.product.model')).Text.Trim()
    $manufacturer = (Invoke-AdbCapture -Arguments @('shell', 'getprop', 'ro.product.manufacturer')).Text.Trim()
    Write-Host "Selected device: $($script:SelectedSerial) ($manufacturer $model)"
    $isMeta = $manufacturer -match '(?i)^(oculus|meta)'
    $isSupported = $isMeta -and $model -match '(?i)^quest\s*3s?$'
    if (-not $isSupported) {
        $message = if ($isMeta) {
            "This headset reports the model '$model'. The mod supports Quest 3 and Quest 3S only; other headsets are untested and may be too slow."
        } else {
            "The selected device does not identify itself as a Meta Quest 3 or Quest 3S: '$manufacturer $model'."
        }
        if ($NonInteractive.IsPresent) { throw "$message Run the installer without -NonInteractive to approve the installation." }
        if (-not (Confirm-ExactText -Message "$message Installation continues only after explicit approval." -RequiredText 'INSTALL')) {
            throw 'Installation was not approved for the selected device.'
        }
    }
    if (Test-IsWifiSerial -DeviceSerial $script:SelectedSerial) {
        Write-Warning 'The headset is connected over Wi-Fi. Installing about 3 GB over Wi-Fi often fails when the connection drops; a USB-C data cable is more reliable.'
    }
    Write-Host 'Keep the headset awake and connected until the installer finishes.'
}

function Get-InstalledPackageState {
    $paths = Invoke-AdbCapture -Arguments @('shell', 'pm', 'path', $script:PackageName) -AllowFailure
    $remotePaths = @($paths.Lines | ForEach-Object {
        if ($_ -match '^package:(.+\.apk)\s*$') { $Matches[1].Trim() }
    } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($remotePaths.Count -eq 0) {
        return [pscustomobject]@{
            Installed = $false; Paths = @(); VersionCode = $null; SignerHashes = @(); FirstInstallTime = $null
        }
    }
    $dump = Invoke-AdbCapture -Arguments @('shell', 'dumpsys', 'package', $script:PackageName) -AllowFailure
    $versionMatch = [regex]::Match($dump.Text, '(?m)^\s*versionCode=(\d+)')
    $firstInstallMatch = [regex]::Match($dump.Text, '(?m)^\s*firstInstallTime=(.+?)\s*$')
    $signerMatch = [regex]::Match($dump.Text, '(?<!past )signatures:\[([0-9a-fA-F, ]*)\]')
    $hashes = @()
    if ($signerMatch.Success) {
        foreach ($value in $signerMatch.Groups[1].Value.Split(',')) {
            if (-not [string]::IsNullOrWhiteSpace($value)) { $hashes += (ConvertTo-SignerHashValue -Text $value) }
        }
    }
    return [pscustomobject]@{
        Installed = $true
        Paths = $remotePaths
        VersionCode = if ($versionMatch.Success) { $versionMatch.Groups[1].Value } else { $null }
        SignerHashes = $hashes
        FirstInstallTime = if ($firstInstallMatch.Success) { $firstInstallMatch.Groups[1].Value } else { $null }
    }
}

function Stop-GameAndVerify {
    param([switch]$BestEffort)
    if ([string]::IsNullOrWhiteSpace([string]$script:SelectedSerial) -or
        [string]::IsNullOrWhiteSpace([string]$script:AdbExe)) { return }
    $stop = Invoke-AdbCapture -Arguments @('shell', 'am', 'force-stop', $script:PackageName) -AllowFailure
    $pidResult = Invoke-AdbCapture -Arguments @('shell', 'pidof', $script:PackageName) -AllowFailure
    if (-not [string]::IsNullOrWhiteSpace($pidResult.Text)) {
        if ($BestEffort.IsPresent) {
            Write-Warning "Could not confirm that $($script:PackageName) is stopped. PID: $($pidResult.Text.Trim())"
            return
        }
        throw "$($script:PackageName) is still running after force-stop. PID: $($pidResult.Text.Trim())"
    }
    if ($stop.ExitCode -ne 0 -and -not $BestEffort.IsPresent) {
        throw "Failed to force-stop $($script:PackageName): $($stop.Text)"
    }
}

function Get-QuestFreeBytes {
    $result = Invoke-AdbCapture -Arguments @('shell', 'toybox', 'df', '-k', '/sdcard')
    $lines = @($result.Lines | Where-Object { $_ -match '^/|/sdcard|/storage' })
    if ($lines.Count -eq 0) { throw "Could not parse Quest storage information:`n$($result.Text)" }
    $fields = @($lines[$lines.Count - 1].Trim() -split '\s+')
    if ($fields.Count -lt 4) { throw "Could not parse Quest free space line: $($lines[$lines.Count - 1])" }
    [int64]$kilobytes = 0
    if (-not [int64]::TryParse($fields[3], [ref]$kilobytes)) {
        throw "Could not parse Quest free kilobytes: $($fields[3])"
    }
    return $kilobytes * 1024
}

function Assert-QuestFreeSpace {
    param(
        [Parameter(Mandatory = $true)]$BuildInfo,
        [switch]$Update,
        [int64]$ExternalBytes = 0
    )
    [int64]$apkBytes = $BuildInfo.ApkBytes
    # Allow staging and installed copies, native extraction and Android's reserve,
    # including on a fresh install. APK download size alone is not sufficient.
    [int64]$required = (2 * $apkBytes) + 3GB
    $required += $ExternalBytes
    [int64]$available = Get-QuestFreeBytes
    Write-Host ('Quest free space: {0:N2} GiB; required: {1:N2} GiB' -f ($available / 1GB), ($required / 1GB))
    if ($available -lt $required) {
        throw ('The headset does not have enough free storage. Free at least {0:N1} GiB (uninstall unused apps or delete videos), then run the installer again.' -f (($required - $available) / 1GB))
    }
}

function Test-RemotePath {
    param([Parameter(Mandatory = $true)][string]$RemotePath)
    if ($RemotePath -match "['`r`n]") { throw "Unsafe remote path: $RemotePath" }
    $result = Invoke-AdbCapture -Arguments @('shell', 'test', '-e', $RemotePath) -AllowFailure
    return $result.ExitCode -eq 0
}

function Assert-SafeGeneratedRemotePath {
    param([Parameter(Mandatory = $true)][string]$RemotePath)
    if ($RemotePath -match "['`r`n]" -or
        $RemotePath -notmatch '^/sdcard/(\.rdrvr-stage-[a-f0-9]{32}(/[A-Za-z0-9_.-]+)?|Android/obb/\.rdrvr-backup-[a-f0-9]{32}|Android/data/com\.rockstargames\.rdr/files/\.rdrvr-backup-[a-f0-9]{32})$') {
        throw "Refusing unsafe generated remote path: $RemotePath"
    }
}

function Remove-GeneratedRemotePath {
    param([Parameter(Mandatory = $true)][string]$RemotePath)
    Assert-SafeGeneratedRemotePath -RemotePath $RemotePath
    Invoke-AdbCapture -Arguments @('shell', 'rm', '-rf', $RemotePath) | Out-Null
}

function Get-RemoteSha256 {
    param([Parameter(Mandatory = $true)][string]$RemotePath)
    if ($RemotePath -match "['`r`n]") { throw "Unsafe remote path: $RemotePath" }
    $result = Invoke-AdbCapture -Arguments @('shell', "toybox sha256sum '$RemotePath'") -AllowFailure
    if ($result.ExitCode -ne 0) { throw "Could not hash $RemotePath on the headset:`n$($result.Text)" }
    $hash = ($result.Text.Trim() -split '\s+')[0]
    if ($hash -notmatch '^[0-9a-fA-F]{64}$') { throw "Unexpected sha256sum output for ${RemotePath}: $($result.Text)" }
    return $hash.ToUpperInvariant()
}

function Assert-RemoteTreeHashes {
    param(
        [Parameter(Mandatory = $true)][string]$RemoteRoot,
        [Parameter(Mandatory = $true)][string]$SumsPath
    )
    if ($RemoteRoot -match "['`r`n]" -or $SumsPath -match "['`r`n]") { throw "Unsafe remote path: $RemoteRoot" }
    # `adb shell` already invokes the remote shell. Passing the command as one
    # argument avoids a nested `sh -c` whose argument boundary ADB can discard.
    $command = "cd '$RemoteRoot' && toybox sha256sum -c '$SumsPath' >/dev/null"
    $result = Invoke-AdbCapture -Arguments @('shell', $command) -AllowFailure
    if ($result.ExitCode -ne 0) {
        throw "Hash verification on the headset failed at $RemoteRoot`n$($result.Text)"
    }
}

function New-RemoteDirectoryTree {
    param(
        [Parameter(Mandatory = $true)][string]$LocalRoot,
        [Parameter(Mandatory = $true)][string]$RemoteRoot
    )
    $localFull = (Get-FullPath $LocalRoot).TrimEnd('\', '/')
    $localPrefix = $localFull + [System.IO.Path]::DirectorySeparatorChar
    foreach ($directory in Get-ChildItem -LiteralPath $localFull -Directory -Recurse) {
        if (-not $directory.FullName.StartsWith($localPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Directory escaped its local root: $($directory.FullName)"
        }
        $relative = $directory.FullName.Substring($localPrefix.Length).Replace('\', '/')
        if ($relative -notmatch '^[A-Za-z0-9_.+=-]+(?:/[A-Za-z0-9_.+=-]+)*$') {
            throw "Directory name is unsafe for ADB: $relative"
        }
        Invoke-AdbCapture -Arguments @('shell', 'mkdir', '-p', "$RemoteRoot/$relative") | Out-Null
    }
}

function New-RemoteParentDirectory {
    param([Parameter(Mandatory = $true)][string]$RemoteParent)
    if ($RemoteParent -notmatch '^/sdcard/Android/[A-Za-z0-9_./-]+$' -or $RemoteParent -match '\.\.') {
        throw "Refusing an unexpected remote parent: $RemoteParent"
    }
    $current = '/sdcard/Android'
    foreach ($component in $RemoteParent.Substring($current.Length + 1).Split('/')) {
        $current = "$current/$component"
        if (Test-RemotePath -RemotePath $current) { continue }
        Invoke-AdbCapture -Arguments @('shell', 'mkdir', '-p', $current) | Out-Null
        # Shell-created folders must stay writable for the game, which creates
        # its own folders inside them on the first start.
        $chmod = Invoke-AdbCapture -Arguments @('shell', 'chmod', 'a+rwx', $current) -AllowFailure
        if ($chmod.ExitCode -ne 0) { Write-Warning "Could not open permissions on ${current}: $($chmod.Text)" }
    }
}

function Publish-ExternalTree {
    param(
        [Parameter(Mandatory = $true)]$Tree,
        [Parameter(Mandatory = $true)][string]$RunRoot
    )
    $id = [Guid]::NewGuid().ToString('N')
    # Never use `adb push` below Android/data or Android/obb. Upload and verify
    # on ordinary shared storage, then move the verified tree with the device shell.
    $stageContainer = "/sdcard/.rdrvr-stage-$id"
    $stage = "$stageContainer/$($Tree.Leaf)"
    $sumsRemote = "$stageContainer/$($Tree.Name).sha256"
    $backup = "$($Tree.RemoteParent)/.rdrvr-backup-$id"
    foreach ($path in @($stageContainer, $stage, $sumsRemote, $backup)) { Assert-SafeGeneratedRemotePath -RemotePath $path }
    if (@($script:RemoteObbRoot, "$($script:RemoteFilesRoot)/assetpacks") -notcontains $Tree.RemoteFinal) {
        throw "Refusing unexpected final path: $($Tree.RemoteFinal)"
    }

    $sumsLocal = Join-Path $RunRoot "external-$($Tree.Name).sha256"
    $sumsText = ($Tree.Files | ForEach-Object { "$($_.Sha256)  $($_.Relative)`n" }) -join ''
    [System.IO.File]::WriteAllText($sumsLocal, $sumsText, (New-Object System.Text.ASCIIEncoding))

    New-RemoteParentDirectory -RemoteParent $Tree.RemoteParent
    if (Test-RemotePath -RemotePath $stageContainer) { Remove-GeneratedRemotePath -RemotePath $stageContainer }
    Invoke-AdbCapture -Arguments @('shell', 'mkdir', '-p', $stage) | Out-Null
    New-RemoteDirectoryTree -LocalRoot $Tree.ContentRoot -RemoteRoot $stage
    Write-Host "Uploading $($Tree.ContentRoot) ($(Format-DownloadSize $Tree.TotalBytes))"
    try {
        # Pushing a folder into an existing remote folder creates <remote>/<leaf>.
        Invoke-AdbCapture -Arguments @('push', $Tree.ContentRoot, $stageContainer) -EchoOutput | Out-Null
        Invoke-AdbCapture -Arguments @('push', $sumsLocal, $sumsRemote) | Out-Null
        if (-not (Test-RemotePath -RemotePath "$stage/$($Tree.Files[0].Relative)")) {
            throw "ADB push produced an unexpected remote layout under $stageContainer"
        }
        Assert-RemoteTreeHashes -RemoteRoot $stage -SumsPath $sumsRemote
    }
    catch {
        $stageError = $_
        if (Test-RemotePath -RemotePath $stageContainer) { Remove-GeneratedRemotePath -RemotePath $stageContainer }
        throw $stageError
    }

    $oldMoved = $false
    $newPublished = $false
    try {
        if (Test-RemotePath -RemotePath $Tree.RemoteFinal) {
            Invoke-AdbCapture -Arguments @('shell', 'mv', $Tree.RemoteFinal, $backup) | Out-Null
            $oldMoved = $true
        }
        Invoke-AdbCapture -Arguments @('shell', 'mv', $stage, $Tree.RemoteFinal) | Out-Null
        $newPublished = $true
        # Files written by the ADB shell keep shell ownership after the move;
        # the game needs read and write access to its own data.
        Invoke-AdbCapture -Arguments @('shell', 'chmod', '-R', 'a+rwX', $Tree.RemoteFinal) | Out-Null
        Assert-RemoteTreeHashes -RemoteRoot $Tree.RemoteFinal -SumsPath $sumsRemote
        try { Remove-GeneratedRemotePath -RemotePath $stageContainer }
        catch { Write-Warning "The data was published, but the staging folder remains: $stageContainer" }
        if ($oldMoved -and (Test-RemotePath -RemotePath $backup)) {
            Remove-GeneratedRemotePath -RemotePath $backup
        }
    }
    catch {
        $publishError = $_
        if ($newPublished -and (Test-RemotePath -RemotePath $Tree.RemoteFinal)) {
            Invoke-AdbCapture -Arguments @('shell', 'rm', '-rf', $Tree.RemoteFinal) -AllowFailure | Out-Null
        }
        if ($oldMoved -and (Test-RemotePath -RemotePath $backup)) {
            Invoke-AdbCapture -Arguments @('shell', 'mv', $backup, $Tree.RemoteFinal) -AllowFailure | Out-Null
        }
        if (Test-RemotePath -RemotePath $stageContainer) { Remove-GeneratedRemotePath -RemotePath $stageContainer }
        throw $publishError
    }
    Write-Host "Published and verified: $($Tree.RemoteFinal) ($($Tree.Files.Count) files)"
}

function Backup-QuestFiles {
    param(
        [Parameter(Mandatory = $true)][string]$BackupRoot,
        [switch]$SkipAssetPacks
    )
    $remoteRoot = $script:RemoteFilesRoot
    $filesRoot = Join-Path $BackupRoot 'files'
    New-Item -ItemType Directory -Force -Path $filesRoot | Out-Null
    $copiedNames = @()
    $excluded = @('.rdrvr-stage-*', '.rdrvr-backup-*')
    if ($SkipAssetPacks.IsPresent) { $excluded += 'assetpacks' }

    if (Test-RemotePath -RemotePath $remoteRoot) {
        $list = Invoke-AdbCapture -Arguments @('shell', 'ls', '-1A', $remoteRoot) -AllowFailure
        if ($list.ExitCode -ne 0) { throw "Could not list the game files on the headset: $($list.Text)" }
        foreach ($rawName in $list.Lines) {
            [string]$name = ([string]$rawName).Trim()
            if ([string]::IsNullOrEmpty($name)) { continue }
            if (@($excluded | Where-Object { $name -like $_ }).Count -gt 0) { continue }
            if ($name -notmatch '^[A-Za-z0-9_.+=-]+$' -or $name -in @('.', '..')) {
                throw "Unsafe file name in the game folder on the headset: $name"
            }
            Write-Host "Backing up $remoteRoot/$name"
            Invoke-AdbCapture -Arguments @('pull', "$remoteRoot/$name", $filesRoot) | Out-Null
            $copiedNames += $name
        }
    }

    $records = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $filesRoot -File -Recurse)) {
        $relative = $file.FullName.Substring($filesRoot.Length).TrimStart('\').Replace('\', '/')
        if ($relative -match "['`r`n]" -or (($relative -split '/') -contains '..')) {
            throw "Unsafe path in the backup: $relative"
        }
        $localHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
        $remoteHash = Get-RemoteSha256 -RemotePath "$remoteRoot/$relative"
        if ($remoteHash -ne $localHash) { throw "Backup hash mismatch: $relative" }
        $records += [pscustomobject]@{ path = $relative; size = [int64]$file.Length; sha256 = $localHash.ToLowerInvariant() }
    }
    $manifest = [ordered]@{
        formatVersion = 1
        package = $script:PackageName
        createdUtc = [DateTime]::UtcNow.ToString('o')
        remoteRoot = $remoteRoot
        excluded = $excluded
        topLevelItems = $copiedNames
        files = $records
    }
    $manifestPath = Join-Path $BackupRoot 'backup-manifest.json'
    Write-TextFile -Path $manifestPath -Text ($manifest | ConvertTo-Json -Depth 8)
    Write-Host "Backup verified: $BackupRoot ($($records.Count) files)"
    return $manifestPath
}

function Invoke-SignerReplacement {
    param(
        [Parameter(Mandatory = $true)][string]$BackupRoot,
        [switch]$SkipAssetPacks
    )
    if ($NonInteractive.IsPresent) {
        throw 'The installed game was signed with a different key. Replacing it requires uninstalling it, which needs typed confirmation. Run the installer without -NonInteractive. The installed game was left in place.'
    }
    Write-Host 'Backing up the game files that ADB can read (settings and reports)...'
    $backupManifest = Backup-QuestFiles -BackupRoot $BackupRoot -SkipAssetPacks:$SkipAssetPacks
    $warning = @"
The installed copy was signed with a different key than this personal build.
Android accepts this build only after the installed copy is uninstalled.
Uninstalling deletes the saved games, the Rockstar first-start acceptance and the game folders on the headset.
Saved games are stored in private app storage and cannot be copied over ADB, so they cannot be backed up here.
Backup of the ADB-readable files: $BackupRoot
"@
    if (-not (Confirm-ExactText -Message $warning -RequiredText 'UNINSTALL')) {
        throw "Uninstall was not approved. The installed game was left in place. Backup: $backupManifest"
    }
    Stop-GameAndVerify
    $uninstall = Invoke-AdbCapture -Arguments @('uninstall', $script:PackageName) -AllowFailure -EchoOutput
    if ($uninstall.ExitCode -ne 0 -or $uninstall.Text -notmatch '(?im)^Success\s*$') {
        throw "Approved uninstall failed:`n$($uninstall.Text)"
    }
    Write-Host 'The previous copy was uninstalled.'
    return $backupManifest
}

function Test-TransportFailure {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    return $Text -match "(?i)device offline|device '[^']*' not found|device not found|no devices/emulators found|broken pipe|connection reset|protocol fault|failed to (?:read|write)|\bclosed\b|transport"
}

function Invoke-ApkInstall {
    param([Parameter(Mandatory = $true)][string[]]$ApkPaths, [switch]$Inherit)
    $arguments = @('install-multiple', '-r', '--no-incremental') + $ApkPaths
    if ($Inherit.IsPresent) { $arguments = @('install-multiple', '-r', '-p', $script:PackageName, '--no-incremental') + $ApkPaths }
    elseif ($ApkPaths.Count -eq 1) { $arguments = @('install', '-r', '--no-incremental', $ApkPaths[0]) }
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        Write-Host 'Sending the APK set to the headset. This takes a few minutes over USB.'
        $result = Invoke-AdbTee -Arguments $arguments
        if ($result.ExitCode -eq 0 -and $result.Text -match '(?im)^Success\s*$') {
            return [pscustomobject]@{ Success = $true; SignatureMismatch = $false; Text = $result.Text }
        }
        if ($result.Text -match '(?i)INSTALL_FAILED_INSUFFICIENT_STORAGE') {
            throw 'Android could not allocate enough storage for the installation. Free more space on the headset (unused apps or videos), then retry. The APK build succeeded; keep the build outputs and personal signing key.'
        }
        if ($result.Text -match '(?i)INSTALL_FAILED_UPDATE_INCOMPATIBLE|signatures? do not match|inconsistent certificates|existing package.*signature') {
            return [pscustomobject]@{ Success = $false; SignatureMismatch = $true; Text = $result.Text }
        }
        if ($attempt -eq 1 -and (Test-TransportFailure -Text $result.Text)) {
            Write-Warning 'The connection to the headset was interrupted. Reconnect the cable if needed and keep the headset awake; the installer retries once.'
            Wait-QuestDevice -RequestedSerial $script:SelectedSerial | Out-Null
            continue
        }
        break
    }
    return [pscustomobject]@{ Success = $false; SignatureMismatch = $false; Text = $result.Text }
}

function Assert-InstalledApks {
    param([Parameter(Mandatory = $true)]$BuildInfo)
    $pathsResult = Invoke-AdbCapture -Arguments @('shell', 'pm', 'path', $script:PackageName)
    $remoteByLeaf = @{}
    foreach ($line in $pathsResult.Lines) {
        if ($line -notmatch '^package:(.+\.apk)\s*$') { continue }
        $remotePath = $Matches[1].Trim()
        if ($remotePath -notmatch '^/data/app/[A-Za-z0-9_.~=+/-]+\.apk$' -or $remotePath -match '\.\.') {
            throw "Unexpected installed APK path: $remotePath"
        }
        $leaf = $remotePath.Substring($remotePath.LastIndexOf('/') + 1)
        if ($remoteByLeaf.ContainsKey($leaf)) { throw "Installed package reports a duplicate APK: $leaf" }
        $remoteByLeaf[$leaf] = $remotePath
    }
    $expected = @{}
    foreach ($record in @($BuildInfo.Manifest.outputs)) {
        $expected[(Get-ExpectedInstalledApkLeaf -Record $record)] = $record
    }
    $missing = @($expected.Keys | Where-Object { -not $remoteByLeaf.ContainsKey($_) })
    $unexpected = @($remoteByLeaf.Keys | Where-Object { -not $expected.ContainsKey($_) })
    if ($missing.Count -ne 0 -or $unexpected.Count -ne 0) {
        throw "Installed APK set differs from the build. Missing: $($missing -join ', '). Unexpected: $($unexpected -join ', ')."
    }
    foreach ($leaf in @($expected.Keys | Sort-Object)) {
        Write-Host "Hashing the installed $leaf on the headset..."
        $remoteHash = Get-RemoteSha256 -RemotePath $remoteByLeaf[$leaf]
        if ($remoteHash -ne ([string]$expected[$leaf].sha256).ToUpperInvariant()) {
            throw "Installed APK hash mismatch: $leaf"
        }
    }
    $state = Get-InstalledPackageState
    if ($state.VersionCode -ne $script:ExpectedVersionCode) {
        throw "The installed versionCode is $($state.VersionCode), expected $($script:ExpectedVersionCode)."
    }
    if (@($state.SignerHashes).Count -eq 0) {
        Write-Warning 'Could not read the installed signer from dumpsys; the APK hashes above are verified.'
    }
    elseif (@($state.SignerHashes).Count -ne 1 -or $state.SignerHashes[0] -ne $BuildInfo.SignerHash) {
        throw 'The installed signer is not the personal signing key of this build.'
    }
    Write-Host "Installed APKs verified: $($expected.Count) file(s), versionCode $($state.VersionCode), personal signer." -ForegroundColor Green
    return $state
}

# Optional last step: starts the GPU boost helper. A failure only prints a note.
function Start-GpuBoostHelper {
    $helper = Join-Path $PSScriptRoot 'rdrvr_boost.sh'
    $remote = '/data/local/tmp/rdrvr_boost.sh'
    try {
        if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) { throw "Missing $helper" }
        Invoke-AdbCapture -Arguments @('push', $helper, $remote) | Out-Null
        Invoke-AdbCapture -Arguments @('shell', 'sh', $remote, 'start') | Out-Null
        for ($attempt = 1; $attempt -le 5; $attempt++) {
            Start-Sleep -Seconds 2
            $status = Invoke-AdbCapture -Arguments @('shell', 'sh', $remote, 'status') -AllowFailure
            if ($status.Text -match '(?m)^helper: running') {
                Write-Host 'GPU boost helper started. Run GPU_BOOST.bat again after every headset restart.' -ForegroundColor Green
                return
            }
        }
        throw 'The helper did not report that it is running.'
    }
    catch {
        Write-Warning "The GPU boost helper was not started: $($_.Exception.Message)"
        Write-Host 'The install is complete. Run GPU_BOOST.bat later to use GPU BOOST in the game.'
    }
}

function Write-FirstLaunchHints {
    param([Parameter(Mandatory = $true)][string]$KeystorePath)
    Write-Host ''
    Write-Host 'FIRST START' -ForegroundColor Green
    Write-Host '  - In the headset open the Library, set the filter to Unknown sources and start RDR.'
    Write-Host '  - Start the game with the headset on, in a lit room, with tracking active. If the headset asks to turn on tracking, turn it on; do not continue without tracking (6DoF is required).'
    Write-Host '  - The first start shows the Rockstar privacy policy and license screens once (thumbstick scrolls, A continues).'
    Write-Host '  - VR menu: hold the menu button on the left controller. Settings are saved automatically.'
    Write-Host "  - If the headset reports that RDR is not responding, choose Close app and start the game again."
    Write-Host ''
    Write-Host 'SIGNING KEY' -ForegroundColor Yellow
    Write-Host "  $KeystorePath"
    Write-Host '  Keep this file and a backup of it. Updates must be signed with the same key. Without it the game has to be uninstalled before the next install, and saved games are lost.'
}

function Invoke-Main {
    $projectRoot = Get-FullPath (Join-Path $PSScriptRoot '..')
    $buildScript = Join-Path $PSScriptRoot 'build.py'
    if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) {
        throw "The source kit is incomplete. Missing: $buildScript"
    }

    $resolvedWorkDir = Assert-SafeWorkDirectory -Path $WorkDir
    if ($resolvedWorkDir.TrimEnd('\') -ieq $projectRoot.TrimEnd('\') -or
        (Test-IsUnderPath -Candidate $resolvedWorkDir -Root $projectRoot) -or
        (Test-IsUnderPath -Candidate $projectRoot -Root $resolvedWorkDir)) {
        throw "WorkDir must be separate from the source kit folder: $projectRoot"
    }
    New-Item -ItemType Directory -Force -Path $resolvedWorkDir | Out-Null
    $downloads = Join-Path $resolvedWorkDir '.downloads'
    $toolsRoot = Join-Path $resolvedWorkDir '.tools'
    $defaultSdk = Join-Path $resolvedWorkDir '.android-sdk'
    $runsRoot = Join-Path $resolvedWorkDir 'runs'
    $runId = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), ([Guid]::NewGuid().ToString('N').Substring(0, 8))
    $buildRoot = Join-Path $runsRoot $runId
    $backupRoot = Join-Path (Join-Path $resolvedWorkDir 'device-backups') $runId
    New-Item -ItemType Directory -Force -Path $downloads, $toolsRoot, $runsRoot, $buildRoot | Out-Null

    $resolvedLogPath = $LogPath
    if ([string]::IsNullOrWhiteSpace($resolvedLogPath)) {
        $resolvedLogPath = Join-Path (Join-Path $resolvedWorkDir 'logs') ("build-and-install-{0}.log" -f $runId)
    }
    $resolvedLogPath = Get-FullPath $resolvedLogPath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedLogPath) | Out-Null
    Start-Transcript -LiteralPath $resolvedLogPath -Force | Out-Null
    $script:TranscriptStarted = $true

    Write-Host "Red Dead Redemption VR $($script:ModVersion) - personal Quest builder" -ForegroundColor Green
    Write-Host "This tool needs your own Red Dead Redemption $($script:ExpectedVersionName) for Android, exported from your phone with EXPORT_RDR_APKS.bat."
    Write-Host 'It does not download or redistribute game APKs or game data.'
    Write-Host "Work directory: $resolvedWorkDir"
    Write-Host "Log: $resolvedLogPath"

    $cache = Read-BuildCache -WorkRoot $resolvedWorkDir
    $explicitInput = -not [string]::IsNullOrWhiteSpace($GamePackage)
    $resolvedGamePackage = $GamePackage
    if (-not $explicitInput -and $null -ne $cache) { $resolvedGamePackage = [string]$cache.gamePackage }
    $kitKey = Get-KitCacheKey -ProjectRoot $projectRoot

    Write-Step 'Preparing pinned build tools'
    $resolvedJavaHome = Resolve-JavaHome -Requested $JavaHome -DownloadRoot $downloads -ToolsRoot $toolsRoot
    $python = Resolve-Python -DownloadRoot $downloads -ToolsRoot $toolsRoot
    $openXrLoader = Resolve-OpenXrLoader -DownloadRoot $downloads -ToolsRoot $toolsRoot
    $vulkanHeaders = Resolve-VulkanHeaders -DownloadRoot $downloads -ToolsRoot $toolsRoot
    $resolvedSdk = Resolve-AndroidSdk -Requested $AndroidSdk -DefaultRoot $defaultSdk -DownloadRoot $downloads `
        -ToolsRoot $toolsRoot -ResolvedJavaHome $resolvedJavaHome
    if ([string]::IsNullOrWhiteSpace($Keystore)) {
        if ($null -ne $cache -and (Test-Path -LiteralPath ([string]$cache.keystore) -PathType Leaf)) {
            $keystorePath = [string]$cache.keystore
        }
        else { $keystorePath = Get-PersistentKeystorePath -ProjectRoot $projectRoot -ResolvedWorkDir $resolvedWorkDir }
    }
    else {
        $keystorePath = Get-FullPath $Keystore
        if (-not (Test-Path -LiteralPath $keystorePath -PathType Leaf)) {
            throw "Requested signing key does not exist: $keystorePath"
        }
    }
    Write-Host "JDK: $resolvedJavaHome"
    Write-Host "Android SDK: $resolvedSdk"
    Write-Host "Personal signing key: $keystorePath (created by the first build; keep it for future updates)"

    $buildInfo = $null
    $inputKey = $null
    $fingerprintedGamePackage = $null
    if (-not $explicitInput -and -not $Rebuild.IsPresent -and $null -ne $cache -and $cache.kitKey -eq $kitKey) {
        if (-not [string]::IsNullOrWhiteSpace($resolvedGamePackage)) { $inputKey = Get-GameInputCacheKey -Path $resolvedGamePackage }
        $fingerprintedGamePackage = $resolvedGamePackage
        $buildInfo = Get-ReusableBuild -Cache $cache -KitKey $kitKey -KeystorePath $keystorePath -InputKey $inputKey -RequestedTrade $Trade
    }
    if ($null -ne $buildInfo) {
        Write-Host 'Reusing the verified APK set. No compilation or APK repackaging is needed.' -ForegroundColor Green
    }
    else {
        if (-not $explicitInput -and ([string]::IsNullOrWhiteSpace($resolvedGamePackage) -or -not (Test-Path -LiteralPath $resolvedGamePackage))) {
            $resolvedGamePackage = Find-PreviousGameInput -WorkRoot $resolvedWorkDir -Cache $cache
        }
        $resolvedGamePackage = Resolve-RequiredInputPath -Value $resolvedGamePackage -Name 'GamePackage' `
            -Prompt 'Select the game exported by EXPORT_RDR_APKS.bat: the RDR-export folder with base.apk and the split_*.apk files, or a single APK' `
            -Filter 'Game APK or archive (*.apk;*.apks;*.zip)|*.apk;*.apks;*.zip|All files (*.*)|*.*' `
            -StartFolder $resolvedWorkDir
        Write-Host "Game input: $resolvedGamePackage"
        if ([string]::IsNullOrWhiteSpace($inputKey) -or $fingerprintedGamePackage -ine $resolvedGamePackage) {
            $inputKey = Get-GameInputCacheKey -Path $resolvedGamePackage
        }
        Write-Step 'Building the personal APK set'
        Invoke-KitBuild -Python $python -BuildScript $buildScript -GamePackagePath $resolvedGamePackage `
            -BuildRoot $buildRoot -SdkRoot $resolvedSdk -ResolvedJavaHome $resolvedJavaHome `
            -OpenXrLoader $openXrLoader -VulkanHeaders $vulkanHeaders -KeystorePath $keystorePath `
            -ReuseBuild $(if ($null -ne $cache -and -not $Rebuild.IsPresent) { [string]$cache.buildRoot } else { '' })
        Write-Step 'Verifying the build outputs'
        $buildInfo = Assert-BuildArtifacts -BuildRoot $buildRoot
        # Persist before device selection: a storage/USB failure must not force another build.
        Save-BuildCache -WorkRoot $resolvedWorkDir -KitKey $kitKey -GamePath $resolvedGamePackage -InputKey $inputKey `
            -BuildRoot $buildRoot -KeystorePath $keystorePath -RequestedTrade $Trade
    }
    Write-Host "Verified build manifest: $($buildInfo.ManifestPath)" -ForegroundColor Green
    if ($BuildOnly.IsPresent) {
        Write-Host "BuildOnly complete. APKs: $($buildInfo.OutputRoot)" -ForegroundColor Green
        Write-Host 'No device was selected or modified.'
        return
    }
    $externalPlan = @(Get-ExternalPlan -Manifest $buildInfo.Manifest)
    [int64]$externalBytes = 0
    foreach ($tree in $externalPlan) { $externalBytes += $tree.TotalBytes }

    Write-Step 'Selecting the Quest headset'
    $script:AdbExe = Resolve-PlatformTools -DownloadRoot $downloads -ToolsRoot $toolsRoot
    Select-QuestDevice -RequestedSerial $Serial

    Write-Step 'Checking the installed game'
    $before = Get-InstalledPackageState
    $mode = 'Fresh'
    if ($before.Installed) {
        if (@($before.SignerHashes).Count -eq 0) {
            $mode = 'Unknown'
            Write-Warning 'Could not read the signing key of the installed copy; the install result decides.'
        }
        elseif (@($before.SignerHashes).Count -eq 1 -and $before.SignerHashes[0] -eq $buildInfo.SignerHash) {
            $mode = 'Update'
            Write-Host 'The installed copy uses this signing key. It is updated in place; saved games and settings are kept.'
        }
        else {
            $mode = 'Replace'
            Write-Warning 'The installed copy was signed with a different key.'
        }
        if ($null -ne $before.VersionCode -and $before.VersionCode -ne $script:ExpectedVersionCode) {
            Write-Warning "The installed copy reports versionCode $($before.VersionCode); this build is $($script:ExpectedVersionCode)."
        }
    }
    else {
        Write-Host 'Red Dead Redemption is not installed on this headset. A fresh install follows.'
    }
    Stop-GameAndVerify
    $installPlan = [pscustomobject]@{ ApkPaths = @($buildInfo.ApkPaths); Inherit = $false; Bytes = $buildInfo.ApkBytes }
    if ($mode -eq 'Update') { $installPlan = Get-ApkUpdatePlan -BuildInfo $buildInfo }
    if ($installPlan.ApkPaths.Count -gt 0 -or $externalBytes -gt 0) {
        Assert-QuestFreeSpace -BuildInfo ([pscustomobject]@{ ApkBytes = $installPlan.Bytes }) `
            -Update:($mode -eq 'Update' -or $mode -eq 'Unknown') -ExternalBytes $externalBytes
    }
    $skipAssetPacks = @($externalPlan | Where-Object { $_.Name -eq 'assetpacks' }).Count -gt 0
    $backupManifest = $null
    if ($mode -eq 'Replace') {
        $backupManifest = Invoke-SignerReplacement -BackupRoot $backupRoot -SkipAssetPacks:$skipAssetPacks
    }

    Write-Step 'Installing the personally signed APK set'
    if ($installPlan.ApkPaths.Count -eq 0) {
        Write-Host 'All installed APKs already match. No APK transfer is needed.' -ForegroundColor Green
        $install = [pscustomobject]@{ Success = $true; SignatureMismatch = $false; Text = 'Already installed' }
    }
    else {
        Write-Host "Sending $($installPlan.ApkPaths.Count) of $($buildInfo.ApkPaths.Count) APK(s)."
        $install = Invoke-ApkInstall -ApkPaths $installPlan.ApkPaths -Inherit:$installPlan.Inherit
    }
    if (-not $install.Success -and $install.SignatureMismatch -and $null -eq $backupManifest) {
        Write-Warning 'Android rejected the update because the installed copy has a different signing key.'
        $backupManifest = Invoke-SignerReplacement -BackupRoot $backupRoot -SkipAssetPacks:$skipAssetPacks
        $mode = 'Replace'
        $install = Invoke-ApkInstall -ApkPaths $buildInfo.ApkPaths
    }
    if (-not $install.Success) {
        $hint = if ($null -ne $backupManifest) { " Backup: $backupManifest" } else { '' }
        throw "APK installation failed. Read the ADB message above.$hint"
    }

    if ($externalPlan.Count -gt 0) {
        Write-Step 'Publishing the external game data'
        foreach ($tree in $externalPlan) { Publish-ExternalTree -Tree $tree -RunRoot $buildRoot }
    }

    Write-Step 'Verifying the installed game and leaving it stopped'
    $after = Assert-InstalledApks -BuildInfo $buildInfo
    if ($mode -eq 'Update' -and $null -ne $before.FirstInstallTime -and $after.FirstInstallTime -ne $before.FirstInstallTime) {
        Write-Warning 'The first-install time changed during the update; Android may have reset the app data.'
    }
    Stop-GameAndVerify

    $result = [ordered]@{
        formatVersion = 1
        modVersion = $script:ModVersion
        buildId = [string]$buildInfo.Manifest.buildId
        mode = $mode
        apks = @($buildInfo.Manifest.outputs | ForEach-Object { [ordered]@{ file = $_.file; sha256 = $_.sha256 } })
        external = @($externalPlan | ForEach-Object { [ordered]@{ name = $_.Name; files = $_.Files.Count; bytes = $_.TotalBytes } })
        backup = $backupManifest
        finishedUtc = [DateTime]::UtcNow.ToString('o')
    }
    Write-TextFile -Path (Join-Path $buildRoot 'install-result.json') -Text ($result | ConvertTo-Json -Depth 6)

    Write-Step 'Starting the GPU boost helper'
    Start-GpuBoostHelper

    Write-Host ''
    Write-Host "INSTALLED AND VERIFIED on $($script:SelectedSerial)." -ForegroundColor Green
    Write-Host 'The game is stopped. This installer never starts it; start it from the headset when ready.'
    Write-Host "Build manifest: $($buildInfo.ManifestPath)"
    if ($null -ne $backupManifest) { Write-Host "Backup of the previous game files: $backupManifest" }
    Write-FirstLaunchHints -KeystorePath $keystorePath
}

if ($MyInvocation.InvocationName -ne '.') {
    $failure = $null
    try {
        Invoke-Main
    }
    catch {
        $failure = $_
        if (-not [string]::IsNullOrWhiteSpace([string]$script:SelectedSerial)) {
            try { Stop-GameAndVerify -BestEffort }
            catch { Write-Warning "Final best-effort force-stop failed: $($_.Exception.Message)" }
        }
        Write-Host "`nFAILED: $($failure.Exception.Message)" -ForegroundColor Red
    }
    finally {
        if ($script:TranscriptStarted) {
            try { Stop-Transcript | Out-Null }
            catch { }
        }
    }
    if ($null -ne $failure) { exit 1 }
    exit 0
}
