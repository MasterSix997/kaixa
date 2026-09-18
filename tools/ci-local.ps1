[CmdletBinding()]
param(
    [ValidateSet("All", "Quality", "Linux", "Windows")]
    [string] $Job = "All"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$workspace = Split-Path -Parent $PSScriptRoot
$containerImage = "kaixa-ci-local:ubuntu-24.04"

function Invoke-Native {
    param(
        [Parameter(Mandatory)]
        [scriptblock] $Command
    )

    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "command failed with exit code $LASTEXITCODE"
    }
}

function Invoke-ContainerJob {
    param(
        [Parameter(Mandatory)]
        [ValidateSet("all", "quality", "linux")]
        [string] $ContainerJob
    )

    Invoke-Native { docker info --format "{{.ServerVersion}}" }
    Invoke-Native {
        docker build --file .github/ci/ubuntu.Dockerfile --tag $containerImage .
    }
    Invoke-Native {
        docker run --rm --volume "${workspace}:/workspace" $containerImage bash tools/ci-local.sh $ContainerJob
    }
}

function Invoke-WindowsJob {
    $build = Join-Path $workspace "build-ci-windows"
    $driveRoot = [System.IO.Path]::GetPathRoot($workspace)
    $fixtureId = [guid]::NewGuid().ToString("N").Substring(0, 8)
    $fixtureActual = Join-Path $driveRoot "kcia-$fixtureId"
    $fixtureAlias = Join-Path $driveRoot "kcil-$fixtureId"
    $previousTestTemp = [Environment]::GetEnvironmentVariable("KAIXA_TEST_TEMP", "Process")

    Invoke-Native {
        cmake --fresh -S . -B $build -G "Visual Studio 17 2022" -A x64
    }
    Invoke-Native {
        cmake --build $build --config Debug --parallel --target kaixa kaixa_tests
    }

    try {
        New-Item -ItemType Directory -Path $fixtureActual -Force | Out-Null
        New-Item -ItemType Junction -Path $fixtureAlias -Target $fixtureActual | Out-Null
        $env:KAIXA_TEST_TEMP = $fixtureAlias
        Invoke-Native {
            ctest --test-dir $build -C Debug --output-on-failure --label-regex "^kaixa[.]purpose:test$"
        }
    } finally {
        if ($null -eq $previousTestTemp) {
            Remove-Item Env:KAIXA_TEST_TEMP -ErrorAction SilentlyContinue
        } else {
            $env:KAIXA_TEST_TEMP = $previousTestTemp
        }

        foreach ($fixture in @($fixtureAlias, $fixtureActual)) {
            $resolved = [System.IO.Path]::GetFullPath($fixture)
            if ((Split-Path -Parent $resolved).TrimEnd('\') -ne $driveRoot.TrimEnd('\')) {
                throw "refusing to remove unexpected fixture path: $resolved"
            }
            if ((Split-Path -Leaf $resolved) -notmatch '^kci[al]-[0-9a-f]{8}$') {
                throw "refusing to remove unexpected fixture path: $resolved"
            }
        }

        if (Test-Path -LiteralPath $fixtureAlias) {
            [System.IO.Directory]::Delete($fixtureAlias)
        }
        if (Test-Path -LiteralPath $fixtureActual) {
            Remove-Item -LiteralPath $fixtureActual -Recurse -Force
        }
    }
}

Push-Location $workspace
try {
    switch ($Job) {
        "Quality" { Invoke-ContainerJob "quality" }
        "Linux" { Invoke-ContainerJob "linux" }
        "Windows" { Invoke-WindowsJob }
        "All" {
            Invoke-ContainerJob "all"
            Invoke-WindowsJob
        }
    }
} finally {
    Pop-Location
}
