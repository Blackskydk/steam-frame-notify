[CmdletBinding()]
param(
    [string]$FrameHost = $env:FRAME_HOST,
    [string]$RemoteDirectory = "frame-notify",
    [switch]$Run,
    [switch]$NativeNotification,
    [switch]$PackageOnly
)

$ErrorActionPreference = "Stop"

if (-not $FrameHost) {
    $FrameHost = "steamos@frame"
}
if ($FrameHost -notmatch '^[A-Za-z0-9_.@:-]+$') {
    throw "FrameHost contains unsupported characters: $FrameHost"
}
if ($RemoteDirectory -notmatch '^[A-Za-z0-9._-]+$') {
    throw "RemoteDirectory must be a simple directory name."
}

foreach ($commandName in @("tar.exe", "scp.exe", "ssh.exe")) {
    if (-not (Get-Command $commandName -ErrorAction SilentlyContinue)) {
        throw "Required Windows command was not found: $commandName"
    }
}

$projectDirectory = Split-Path -Parent $PSScriptRoot
$archiveName = "frame-notify-$PID-$(Get-Random).tar.gz"
$archivePath = Join-Path $env:TEMP $archiveName
$remoteArchive = "frame-notify-upload.tar.gz"
$sshKeyPath = Join-Path $env:USERPROFILE ".ssh\id_ed25519_frame_notify"
$sshOptions = @()
if (-not $PackageOnly -and (Test-Path -LiteralPath $sshKeyPath)) {
    $sshOptions = @("-i", $sshKeyPath, "-o", "IdentitiesOnly=yes")
}

try {
    Write-Host "[Push] Packaging source files..."
    Push-Location $projectDirectory
    try {
        & tar.exe -czf $archivePath CMakeLists.txt README.md LICENSE .gitignore src scripts tests docs
        if ($LASTEXITCODE -ne 0) {
            throw "tar failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        Pop-Location
    }

    if ($PackageOnly) {
        Write-Host "[Push] Package validation succeeded."
        return
    }

    Write-Host "[Push] Uploading to $FrameHost..."
    & scp.exe @sshOptions $archivePath "${FrameHost}:$remoteArchive"
    if ($LASTEXITCODE -ne 0) {
        throw "scp failed with exit code $LASTEXITCODE"
    }

    Write-Host "[Push] Extracting, building, and running tests on Steam Frame..."
    $buildCommand = "mkdir -p ~/$RemoteDirectory && " +
        "tar -xzf ~/$remoteArchive -C ~/$RemoteDirectory && " +
        "cd ~/$RemoteDirectory && bash ./scripts/build-frame.sh"
    if ($Run -or $NativeNotification) {
        $runOption = if ($NativeNotification) { " --native-notification" } else { "" }
        $buildCommand += " && printf '\n[Run] Starting Frame Notify. Press Ctrl+C to stop.\n'" +
            " && ./build-frame/frame-notify-test$runOption"
        & ssh.exe @sshOptions -t $FrameHost $buildCommand
    }
    else {
        & ssh.exe @sshOptions $FrameHost $buildCommand
    }
    if ($LASTEXITCODE -ne 0) {
        throw "remote build or application failed with exit code $LASTEXITCODE"
    }

    Write-Host "[Push] Steam Frame build succeeded."
    if (-not ($Run -or $NativeNotification)) {
        Write-Host "Run it with:"
        Write-Host ".\scripts\push-frame.ps1 -Run"
    }
}
finally {
    if (Test-Path -LiteralPath $archivePath) {
        Remove-Item -LiteralPath $archivePath -Force
    }
}
