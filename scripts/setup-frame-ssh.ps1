[CmdletBinding()]
param(
    [string]$FrameHost = $env:FRAME_HOST
)

$ErrorActionPreference = "Stop"

if (-not $FrameHost) {
    $FrameHost = "steamos@frame"
}
if ($FrameHost -notmatch '^[A-Za-z0-9_.@:-]+$') {
    throw "FrameHost contains unsupported characters: $FrameHost"
}

foreach ($commandName in @("ssh.exe", "ssh-keygen.exe")) {
    if (-not (Get-Command $commandName -ErrorAction SilentlyContinue)) {
        throw "Required Windows command was not found: $commandName"
    }
}

$sshDirectory = Join-Path $env:USERPROFILE ".ssh"
$keyPath = Join-Path $sshDirectory "id_ed25519_frame_notify"
$publicKeyPath = "$keyPath.pub"

if (-not (Test-Path -LiteralPath $keyPath)) {
    New-Item -ItemType Directory -Path $sshDirectory -Force | Out-Null
    Write-Host "[SSH] Creating a dedicated Frame Notify key at $keyPath"
    Write-Host "[SSH] Press Enter twice when asked for a passphrase to allow unattended pushes."
    & ssh-keygen.exe -t ed25519 -f $keyPath -C "frame-notify-$env:USERNAME"
    if ($LASTEXITCODE -ne 0) {
        throw "ssh-keygen failed with exit code $LASTEXITCODE"
    }
}

if (-not (Test-Path -LiteralPath $publicKeyPath)) {
    throw "Public key was not found: $publicKeyPath"
}
$publicKey = (Get-Content -LiteralPath $publicKeyPath -Raw).Trim()
if ($publicKey -notmatch '^ssh-ed25519 [A-Za-z0-9+/=]+(?: [A-Za-z0-9@._-]+)?$') {
    throw "Public key has an unexpected format: $publicKeyPath"
}

Write-Host "[SSH] Installing the public key on $FrameHost (the Frame password is needed once)..."
$installCommand = "umask 077; mkdir -p ~/.ssh; chmod 700 ~/.ssh; " +
    "touch ~/.ssh/authorized_keys; " +
    "chmod 600 ~/.ssh/authorized_keys; " +
    "grep -qxF '$publicKey' ~/.ssh/authorized_keys || " +
    "printf '%s\n' '$publicKey' >> ~/.ssh/authorized_keys"
& ssh.exe $FrameHost $installCommand
if ($LASTEXITCODE -ne 0) {
    throw "Could not install the public key on $FrameHost"
}

& ssh.exe -i $keyPath -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=10 `
    $FrameHost "true"
if ($LASTEXITCODE -ne 0) {
    throw "Key login was not verified. If the key has a passphrase, load it into ssh-agent first."
}

Write-Host "[SSH] Key login verified. Future push-frame.ps1 runs will use this key."
