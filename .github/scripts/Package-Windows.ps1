<#
Создаёт ZIP OBS 33 из явно выбранного staging внутри workspace.
StageDirectory — install prefix с каталогом плагина; BuildDirectory — каталог
CMake той же сборки; Configuration выбирает DLL для сверки. OutputDirectory
принимает новый архив; существующий ZIP никогда не перезаписывается.
Сценарий не собирает код, не устанавливает плагин в OBS и не создаёт checksums.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $StageDirectory,
    [Parameter(Mandatory)]
    [ValidateSet('RelWithDebInfo', 'Release')]
    [string] $Configuration,
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [string] $BuildDirectory = 'build_x64',
    [string] $OutputDirectory = 'release'
)

# ErrorActionPreference запрещает продолжать упаковку после ошибки чтения или записи.
$ErrorActionPreference = 'Stop'
if ( ![Environment]::Is64BitOperatingSystem -or $PSVersionTable.PSVersion -lt '7.2.0' ) {
    throw 'Требуются Windows x64 и PowerShell 7.2 или новее.'
}

# ProjectRoot ограничивает все входы и результат текущим workspace.
$ProjectRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path

<#
Возвращает абсолютный Path внутри workspace; относительный путь отсчитывается
от корня проекта. Отклоняет reparse points в существующих компонентах пути,
чтобы ссылка или junction не направила чтение/запись за пределы workspace.
#>
function Resolve-WorkspacePath {
    param([string] $Path)
    # FullPath нормализует '..'; Boundary отличает workspace от каталога с похожим именем.
    $FullPath = [IO.Path]::GetFullPath($Path, $ProjectRoot)
    $Boundary = $ProjectRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if ( !$FullPath.StartsWith($Boundary, [StringComparison]::OrdinalIgnoreCase) ) {
        throw "Путь должен находиться внутри workspace: $FullPath"
    }
    # Component проверяет каждый существующий предок, включая конечный файл/каталог.
    $Component = $FullPath
    while ( $Component.Length -gt $ProjectRoot.Length ) {
        if ( Test-Path -LiteralPath $Component ) {
            if ( (Get-Item -LiteralPath $Component -Force).Attributes -band [IO.FileAttributes]::ReparsePoint ) {
                throw "Ссылки и junction не допускаются в пути упаковки: $Component"
            }
        }
        $Component = [IO.Path]::GetDirectoryName($Component)
    }
    return $FullPath
}

<#
Проверяет равенство обязательных файлов Left и Right по содержимому.
Хеши используются только для внутренней сверки и не попадают в поставку.
#>
function Assert-SameFile {
    param([string] $Left, [string] $Right)
    if ( !(Test-Path -LiteralPath $Left -PathType Leaf) -or !(Test-Path -LiteralPath $Right -PathType Leaf) ) {
        throw "Нет обязательного файла: $Left или $Right"
    }
    if ( (Get-FileHash -LiteralPath $Left).Hash -ne (Get-FileHash -LiteralPath $Right).Hash ) {
        throw "Staging не соответствует выбранным входам: $Left"
    }
}

# BuildSpec задаёт контрактные имя и числовую версию пакета.
$BuildSpec = Get-Content -LiteralPath "$ProjectRoot/buildspec.json" -Raw | ConvertFrom-Json
if ( $BuildSpec.name -notmatch '^[a-z0-9-]+$' -or $BuildSpec.version -notmatch '^\d+\.\d+\.\d+$' ) {
    throw 'Некорректные имя или версия в buildspec.json.'
}
# ProductName используется в OBS-каталоге и DLL; ProductVersion — в имени ZIP и сверке DLL.
$ProductName = $BuildSpec.name
$ProductVersion = $BuildSpec.version
$StageDirectory = Resolve-WorkspacePath $StageDirectory
$BuildDirectory = Resolve-WorkspacePath $BuildDirectory
$OutputDirectory = Resolve-WorkspacePath $OutputDirectory

# PluginDirectory содержит выбранный плагин; Binary — его DLL в формате OBS 33.
$PluginDirectory = Resolve-WorkspacePath "$StageDirectory/$ProductName"
$Binary = Resolve-WorkspacePath "$PluginDirectory/$ProductName.dll"
# ExpectedBinary закрепляет конфигурацию сборки; Instructions — инструкцию из исходников.
$ExpectedBinary = Resolve-WorkspacePath "$BuildDirectory/$Configuration/$ProductName.dll"
$Instructions = Resolve-WorkspacePath "$ProjectRoot/INSTALL-WINDOWS.txt"
Assert-SameFile $Binary $ExpectedBinary
if ( !(Test-Path -LiteralPath $Instructions -PathType Leaf) -or (Get-Item -LiteralPath $Instructions).Length -eq 0 ) {
    throw 'Отсутствует непустой INSTALL-WINDOWS.txt.'
}
if ( [Diagnostics.FileVersionInfo]::GetVersionInfo($Binary).ProductVersion -ne $ProductVersion ) {
    throw 'ProductVersion DLL не совпадает с buildspec.json.'
}

# Files задаёт точный состав архива; посторонние файлы из staging не попадают в поставку.
$Files = [ordered]@{
    "$ProductName/$ProductName.dll" = $Binary
    'INSTALL-WINDOWS.txt' = $Instructions
}
# Locale обозначает обязательный язык: сверка не допускает старую копию в staging.
foreach ( $Locale in @('ru-RU', 'en-US') ) {
    # StagedLocale и SourceLocale связывают файл поставки с актуальным исходным переводом.
    $StagedLocale = Resolve-WorkspacePath "$PluginDirectory/data/locale/$Locale.ini"
    $SourceLocale = Resolve-WorkspacePath "$ProjectRoot/data/locale/$Locale.ini"
    Assert-SameFile $StagedLocale $SourceLocale
    $Files["$ProductName/data/locale/$Locale.ini"] = $StagedLocale
}
# Symbols включаются только при наличии и точном соответствии PDB выбранной конфигурации.
$Symbols = Resolve-WorkspacePath "$PluginDirectory/$ProductName.pdb"
if ( Test-Path -LiteralPath $Symbols -PathType Leaf ) {
    Assert-SameFile $Symbols (Resolve-WorkspacePath "$BuildDirectory/$Configuration/$ProductName.pdb")
    $Files["$ProductName/$ProductName.pdb"] = $Symbols
}

# ArchivePath — готовый ZIP; PartialPath сохраняет незавершённый результат при отказе.
$ArchivePath = Resolve-WorkspacePath "$OutputDirectory/$ProductName-$ProductVersion-windows-$Target.zip"
$PartialPath = Resolve-WorkspacePath "$ArchivePath.partial"
if ( (Test-Path -LiteralPath $ArchivePath) -or (Test-Path -LiteralPath $PartialPath) ) {
    throw "Результат уже существует; выберите другой OutputDirectory: $ArchivePath"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
# ArchiveStream принадлежит этому запуску; CreateNew защищает от перезаписи при гонке.
$ArchiveStream = [IO.File]::Open($PartialPath, [IO.FileMode]::CreateNew)
try {
    # Archive завершает ZIP при Dispose, сохраняя внешний поток открытым до finally.
    $Archive = [IO.Compression.ZipArchive]::new($ArchiveStream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        # File сопоставляет относительный путь ZIP и проверенный абсолютный вход.
        foreach ( $File in $Files.GetEnumerator() ) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($Archive, $File.Value, $File.Key, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
    finally {
        $Archive.Dispose()
    }
}
finally {
    $ArchiveStream.Dispose()
}

# CompletedArchive читает закрытый ZIP и проверяет фактические записи и размеры.
$CompletedArchive = [IO.Compression.ZipFile]::OpenRead($PartialPath)
try {
    if ( $CompletedArchive.Entries.Count -ne $Files.Count ) {
        throw 'Неожиданный состав готового ZIP.'
    }
    # File повторно обозначает исходный файл для каждой обязательной записи ZIP.
    foreach ( $File in $Files.GetEnumerator() ) {
        # Entry — запись готового ZIP, а не предполагаемый путь staging.
        $Entry = $CompletedArchive.GetEntry($File.Key)
        if ( $null -eq $Entry -or $Entry.Length -eq 0 -or $Entry.Length -ne (Get-Item -LiteralPath $File.Value).Length ) {
            throw "Неверная или пустая запись ZIP: $($File.Key)"
        }
        # EntryStream читает распакованные байты записи для сверки с фактическим входом.
        $EntryStream = $Entry.Open()
        try {
            if ( (Get-FileHash -InputStream $EntryStream).Hash -ne (Get-FileHash -LiteralPath $File.Value).Hash ) {
                throw "Содержимое ZIP не совпадает с исходным файлом: $($File.Key)"
            }
        }
        finally {
            $EntryStream.Dispose()
        }
    }
}
finally {
    $CompletedArchive.Dispose()
}
# Повторная проверка обоих абсолютных путей предшествует перемещению единственного своего файла.
[IO.File]::Move((Resolve-WorkspacePath $PartialPath), (Resolve-WorkspacePath $ArchivePath))
Write-Output "Готовый ZIP: $ArchivePath"
