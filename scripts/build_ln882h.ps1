[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_.-]*$')]
    [string]$Version = ("dev_local_" + (Get-Date -Format 'yyyyMMdd_HHmmss'))
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("obk-ln882h-" + [guid]::NewGuid().ToString('N'))
$snapshot = Join-Path $temp 'snapshot'
$out = Join-Path $repo ("output\" + $Version)
$image = 'obk_ln882h_local'

foreach ($command in @('git', 'docker', 'tar')) {
    Get-Command $command -ErrorAction Stop | Out-Null
}

try {
    New-Item -ItemType Directory -Path $snapshot -Force | Out-Null

    # Snapshot working files, not the index. Submodules are initialized in Linux.
    $files = & git -C $repo -c core.quotePath=false ls-files --cached --others --exclude-standard
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list local source files.' }
    foreach ($file in ($files | Sort-Object -Unique)) {
        $source = Join-Path $repo $file
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            $destination = Join-Path $snapshot $file
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath $source -Destination $destination
        }
    }

    & tar -cf (Join-Path $temp 'source.tar') -C $snapshot .
    if ($LASTEXITCODE -ne 0) { throw 'Cannot archive local source files.' }

    $build = @'
#!/bin/bash
set -euo pipefail
git config --global --add safe.directory /source
git config --global --add safe.directory /source/.git
git -c core.autocrlf=false clone --no-local --no-checkout /source /home/devuser/app
cd /home/devuser/app
git reset --mixed --quiet HEAD
tar -xf /input/source.tar
# Windows line endings are normalized only in the disposable Linux copy.
find . -path ./.git -prune -o -type f -name '*.sh' -exec sed -i 's/\r$//' {} +
make APP_VERSION="$APP_VERSION" APP_NAME=OpenLN882H OpenLN882H
cp "output/$APP_VERSION/OpenLN882H_$APP_VERSION.bin" /out/
cp "output/$APP_VERSION/OpenLN882H_${APP_VERSION}_OTA.bin" /out/
'@
    [System.IO.File]::WriteAllText(
        (Join-Path $temp 'build.sh'),
        ($build -replace "`r`n", "`n"),
        (New-Object System.Text.UTF8Encoding($false))
    )

    & docker build -t $image (Join-Path $repo 'docker')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot build the Docker image. Check Docker Desktop is running.' }

    New-Item -ItemType Directory -Path $out -Force | Out-Null
    Write-Host "Building local LN882H sources as $Version"
    & docker run --rm `
        --mount "type=bind,source=$repo,target=/source,readonly" `
        --mount "type=bind,source=$temp,target=/input,readonly" `
        --mount "type=bind,source=$out,target=/out" `
        -e "APP_VERSION=$Version" `
        --entrypoint /bin/bash $image /input/build.sh
    if ($LASTEXITCODE -ne 0) { throw 'LN882H build failed. See the Docker output above.' }

    foreach ($name in @("OpenLN882H_$Version.bin", "OpenLN882H_${Version}_OTA.bin")) {
        $artifact = Get-Item -LiteralPath (Join-Path $out $name)
        if ($artifact.Length -eq 0) { throw "Empty firmware artifact: $name" }
    }
    Write-Host "Firmware saved to $out"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
