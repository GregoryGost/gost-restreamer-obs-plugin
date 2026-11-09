[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $PluginRoot,
    [Parameter(Mandatory)][string] $ExpectedBinary,
    [Parameter(Mandatory)][string] $Dumpbin
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../fixtures/WorkspacePaths.ps1')

$plugin = Assert-WorkspacePath $PluginRoot
$expected = Assert-WorkspacePath $ExpectedBinary
$binary = Assert-WorkspacePath (Join-Path $plugin 'gost-restreamer-obs-plugin.dll')
if ((Split-Path -Leaf $plugin) -ne 'gost-restreamer-obs-plugin') {
    throw 'Каталог плагина должен совпадать с базовым именем DLL.'
}
$contents = @(Get-ChildItem -LiteralPath $plugin -Recurse -Force)
foreach ($item in $contents) {
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Артефакт плагина не должен содержать ссылки.'
    }
}
$copies = @($contents | Where-Object { -not $_.PSIsContainer -and $_.Name -eq 'gost-restreamer-obs-plugin.dll' })
if ($copies.Count -ne 1 -or $copies[0].FullName -ne $binary) {
    throw 'DLL должна находиться только в корне нового каталога плагина OBS 33.'
}
$binaryHash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
if ($binaryHash -ne (Get-FileHash -LiteralPath $expected -Algorithm SHA256).Hash) {
    throw 'Проверяемая DLL отличается от результата выбранной сборки.'
}
$dumpbinPath = (Get-Item -LiteralPath $Dumpbin -ErrorAction Stop).FullName
$inspection = (& $dumpbinPath /headers /exports $binary 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $inspection -notmatch '(?i)8664 machine') {
    throw 'Не подтверждён формат Windows x64 PE выбранного артефакта.'
}
$requiredExports = @(
    'obs_module_load', 'obs_module_unload', 'obs_module_ver', 'obs_module_set_pointer',
    'obs_module_set_locale', 'obs_module_free_locale', 'obs_module_get_string',
    'obs_module_name', 'obs_module_description'
)
foreach ($symbol in $requiredExports) {
    if ($inspection -notmatch ('\b' + [regex]::Escape($symbol) + '\b')) {
        throw "Отсутствует обязательный экспорт модуля: $symbol."
    }
}
foreach ($locale in @('ru-RU', 'en-US')) {
    $stagedLocale = Assert-WorkspacePath (Join-Path $plugin "data/locale/$locale.ini")
    $sourceLocale = Assert-WorkspacePath (Join-Path $PSScriptRoot "../../data/locale/$locale.ini")
    if ((Get-FileHash -LiteralPath $stagedLocale).Hash -ne (Get-FileHash -LiteralPath $sourceLocale).Hash) {
        throw "Упакованная локализация $locale отличается от проверяемого исходника."
    }
}
[pscustomobject]@{ Check = 'PluginArtifact'; Architecture = 'x64'; RequiredExports = $requiredExports.Count; SHA256 = $binaryHash }
