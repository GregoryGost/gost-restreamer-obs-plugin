<#
Собирает Windows, выполняет offline-тесты и устанавливает плагин только после
их успеха. Target задаёт архитектуру, Configuration — общую конфигурацию
сборки, тестов и установки в release/<Configuration> внутри workspace.
#>
[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('RelWithDebInfo', 'Release')]
    [string] $Configuration = 'RelWithDebInfo'
)

# ErrorActionPreference прекращает цепочку при первой ошибке PowerShell.
$ErrorActionPreference = 'Stop'
if ( $env:CI -eq $null ) {
    throw 'Сценарий сборки предназначен для CI.'
}
if ( ![Environment]::Is64BitOperatingSystem -or $PSVersionTable.PSVersion -lt '7.2.0' ) {
    throw 'Требуются Windows x64 и PowerShell 7.2 или новее.'
}

# ProjectRoot закрепляет рабочий каталог независимо от места вызова сценария.
$ProjectRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
. "$PSScriptRoot/utils.pwsh/Invoke-External.ps1"

Push-Location -LiteralPath $ProjectRoot
try {
    # BuildDirectory содержит сборку; TestLogDirectory — отчёты текущего запуска CI.
    $BuildDirectory = Join-Path $ProjectRoot "build_${Target}"
    $TestLogDirectory = Join-Path $BuildDirectory 'test-results'
    # TEMP/TMP configure и тестовых дочерних процессов остаются внутри workspace.
    $env:TEMP = Join-Path $ProjectRoot 'build/tmp'
    $env:TMP = $env:TEMP
    New-Item -ItemType Directory -Path $env:TEMP -Force | Out-Null

    Invoke-External cmake --preset "windows-ci-${Target}"
    Invoke-External cmake --build --preset "windows-ci-${Target}" --config $Configuration --parallel -- /consoleLoggerParameters:Summary /noLogo

    New-Item -ItemType Directory -Path $TestLogDirectory -Force | Out-Null
    # Offline preset исключает backend; -C выбирает также Release вместо значения preset.
    # Пустой URL дополнительно исключает наследование адреса настоящего сервера.
    $env:GOST_TEST_BACKEND_URL = ''
    Invoke-External ctest --preset windows-x64-offline -C $Configuration --output-on-failure --no-tests=error --output-junit "$TestLogDirectory/offline.xml" --output-log "$TestLogDirectory/offline.log"

    Invoke-External cmake --install $BuildDirectory --prefix "$ProjectRoot/release/$Configuration" --config $Configuration
}
finally {
    Pop-Location
}
