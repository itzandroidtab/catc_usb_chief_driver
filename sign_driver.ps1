# Creates certificate and signs both .sys and .cat
# Run as Administrator. change path to your WinDDK installation
$WinDDKRoot = "D:\development\software\WinDDK\7600.16385.1"

# Check for administrator rights
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "ERROR: This script must be run as Administrator!" -ForegroundColor Red
    Write-Host ""
    Write-Host "Please:" -ForegroundColor Yellow
    Write-Host "  1. Close this PowerShell window" -ForegroundColor White
    Write-Host "  2. Right-click PowerShell and select 'Run as Administrator'" -ForegroundColor White
    Write-Host "  3. Run this script again" -ForegroundColor White
    Write-Host ""
    pause
    exit 1
}

$ErrorActionPreference = "Stop"
$DriverPackagePath = $PWD.Path
$MakeCertPath = "$WinDDKRoot\bin\amd64\makecert.exe"
$CertMgrPath = "$WinDDKRoot\bin\amd64\certmgr.exe"
$Inf2CatPath = "$WinDDKRoot\bin\selfsign\inf2cat.exe"
$SignToolPath = "$WinDDKRoot\bin\amd64\signtool.exe"
$Pvk2PfxPath = "$WinDDKRoot\bin\amd64\pvk2pfx.exe"

Write-Host "=== USB Chief Driver Complete Signing Process ===" -ForegroundColor Cyan
Write-Host "(Running as Administrator)" -ForegroundColor Green
Write-Host ""

# Step 1: Clean up old files and certificates
Write-Host "[1/7] Cleaning up old files and certificates..." -ForegroundColor Yellow
try {
    Get-ChildItem Cert:\LocalMachine\Root | Where-Object {$_.Subject -like "*USB Chief*"} | Remove-Item -Force -ErrorAction SilentlyContinue
    Get-ChildItem Cert:\LocalMachine\My | Where-Object {$_.Subject -like "*USB Chief*"} | Remove-Item -Force -ErrorAction SilentlyContinue
    Get-ChildItem Cert:\CurrentUser\My | Where-Object {$_.Subject -like "*USB Chief*"} | Remove-Item -Force -ErrorAction SilentlyContinue
} catch {
    Write-Host "  WARNING: Could not remove some certificates (may need admin rights)" -ForegroundColor Yellow
}

if (Test-Path "usbchieftestsign.cer") { Remove-Item "usbchieftestsign.cer" -Force }
if (Test-Path "usbchieftestsign.pvk") { Remove-Item "usbchieftestsign.pvk" -Force }
if (Test-Path "usbchieftestsign.pfx") { Remove-Item "usbchieftestsign.pfx" -Force }
if (Test-Path "usbchief.cat") { Remove-Item "usbchief.cat" -Force }

Write-Host "  Cleanup complete!" -ForegroundColor Green

# Step 2: Create new test certificate with private key
Write-Host "[2/7] Creating test certificate..." -ForegroundColor Yellow
& $MakeCertPath -r -pe -ss PrivateCertStore -n "CN=USB Chief Driver Test Certificate" `
    -eku 1.3.6.1.5.5.7.3.3 `
    -sv usbchieftestsign.pvk usbchieftestsign.cer `
    -sky signature -len 2048 -b 01/01/2025 -e 01/01/2040

if ($LASTEXITCODE -ne 0) {
    Write-Host "  ERROR: Failed to create certificate" -ForegroundColor Red
    exit 1
}
Write-Host "  Certificate created!" -ForegroundColor Green

# Step 3: Create PFX file from certificate and private key
Write-Host "[3/7] Creating PFX file with private key..." -ForegroundColor Yellow
& $Pvk2PfxPath -pvk usbchieftestsign.pvk -spc usbchieftestsign.cer -pfx usbchieftestsign.pfx
if ($LASTEXITCODE -ne 0) {
    Write-Host "  ERROR: Failed to create PFX file" -ForegroundColor Red
    exit 1
}
Write-Host "  PFX file created!" -ForegroundColor Green

# Step 4: Install certificate to Trusted Root
Write-Host "[4/7] Installing certificate to Trusted Root..." -ForegroundColor Yellow
& $CertMgrPath /add usbchieftestsign.cer /s /r localMachine root
if ($LASTEXITCODE -ne 0) {
    Write-Host "  ERROR: Failed to install certificate" -ForegroundColor Red
    exit 1
}
Write-Host "  Certificate installed!" -ForegroundColor Green

# Copy INF to build directory
Copy-Item "build\usbchief.sys" -Destination "." -Force
Copy-Item "build\usbchief.pdb" -Destination "." -Force

# Step 5: Sign the .sys file
Write-Host "[5/7] Signing usbchief.sys..." -ForegroundColor Yellow
& $SignToolPath sign /v /f usbchieftestsign.pfx /t http://timestamp.digicert.com usbchief.sys
if ($LASTEXITCODE -ne 0) {
    Write-Host "  WARNING: Timestamping failed, trying without timestamp..." -ForegroundColor Yellow
    & $SignToolPath sign /v /f usbchieftestsign.pfx usbchief.sys
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  ERROR: Failed to sign .sys file" -ForegroundColor Red
        exit 1
    }
}
Write-Host "  .sys file signed!" -ForegroundColor Green

# Step 6: Create catalog file
Write-Host "[6/7] Creating and signing catalog file..." -ForegroundColor Yellow
& $Inf2CatPath /driver:$DriverPackagePath /os:7_X64
if ($LASTEXITCODE -ne 0) {
    Write-Host "  ERROR: Failed to create catalog" -ForegroundColor Red
    exit 1
}
Write-Host "  Catalog created!" -ForegroundColor Green

# Sign the catalog
Write-Host "  Signing catalog..." -ForegroundColor Yellow
& $SignToolPath sign /v /f usbchieftestsign.pfx /t http://timestamp.digicert.com usbchief.cat
if ($LASTEXITCODE -ne 0) {
    Write-Host "  WARNING: Timestamping failed, trying without timestamp..." -ForegroundColor Yellow
    & $SignToolPath sign /v /f usbchieftestsign.pfx usbchief.cat
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  ERROR: Failed to sign catalog" -ForegroundColor Red
        exit 1
    }
}
Write-Host "  Catalog signed!" -ForegroundColor Green

# Step 7: Verify signatures
Write-Host "[7/7] Verifying signatures..." -ForegroundColor Yellow
Write-Host ""
Write-Host "  usbchief.sys:" -ForegroundColor Cyan
$sysSig = Get-AuthenticodeSignature "usbchief.sys"
Write-Host "    Status: $($sysSig.Status)" -ForegroundColor $(if ($sysSig.Status -eq "Valid") {"Green"} else {"Red"})
Write-Host "    Signer: $($sysSig.SignerCertificate.Subject)" -ForegroundColor Gray

Write-Host ""
Write-Host "  usbchief.cat:" -ForegroundColor Cyan
$catSig = Get-AuthenticodeSignature "usbchief.cat"
Write-Host "    Status: $($catSig.Status)" -ForegroundColor $(if ($catSig.Status -eq "Valid") {"Green"} else {"Red"})
Write-Host "    Signer: $($catSig.SignerCertificate.Subject)" -ForegroundColor Gray

Write-Host ""
if ($sysSig.Status -eq "Valid" -and $catSig.Status -eq "Valid") {
    Write-Host "=== SUCCESS! Driver package is fully signed ===" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next steps:" -ForegroundColor Cyan
    Write-Host "  1. Ensure test signing is enabled:" -ForegroundColor White
    Write-Host "     bcdedit /set testsigning on" -ForegroundColor Gray
    Write-Host ""
    Write-Host "  2. Restart your computer if you just enabled test signing" -ForegroundColor White
    Write-Host ""
    Write-Host "  3. Install the driver:" -ForegroundColor White
    Write-Host "     pnputil /add-driver usbchief.inf /install" -ForegroundColor Gray
    Write-Host ""
    Write-Host "Files in package:" -ForegroundColor Cyan
    Write-Host "  - usbchief.sys (signed driver binary)" -ForegroundColor Gray
    Write-Host "  - usbchief.cat (signed catalog)" -ForegroundColor Gray
    Write-Host "  - usbchief.inf (installation file)" -ForegroundColor Gray
    Write-Host "  - usbchieftestsign.cer (public certificate)" -ForegroundColor Gray
    Write-Host "  - usbchieftestsign.pfx (certificate with private key - keep secure!)" -ForegroundColor Gray
    Write-Host ""
} else {
    Write-Host "=== ERROR: Signature verification failed ===" -ForegroundColor Red
    exit 1
}
