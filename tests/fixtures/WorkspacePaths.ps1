Set-StrictMode -Version Latest

function Assert-WorkspacePath {
    <#
    .SYNOPSIS
    Возвращает разрешённый существующий путь внутри рабочего каталога проекта или завершает проверку ошибкой.
    .DESCRIPTION
    Корень определяется расположением фикстуры. Сам корень не принимается: разрешён только его
    потомок с границей каталога, без ReparsePoint на всём пути до корня. Проверка не создаёт путь
    и не разрешает файловые действия за пределами проверенного значения. При создании нового
    каталога вызывающий код сначала выбирает разрешённого родителя, затем проверяет созданный путь.
    .PARAMETER Path
    Существующий файл или каталог; разрешается Resolve-Path без интерпретации шаблонов пути.
    #>
    param([Parameter(Mandatory)][string] $Path)

    # Проверяется существующий путь: префикс ограничивает область, а проверка ссылок запрещает перенаправление наружу.
    $workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path.TrimEnd('\', '/')
    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path.TrimEnd('\', '/')
    if (-not $resolved.StartsWith($workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Тестовый путь должен находиться внутри рабочего каталога проекта.'
    }
    $current = Get-Item -LiteralPath $resolved -Force
    while ($current.FullName -ne $workspace) {
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Тестовый путь не должен проходить через ссылки или точки повторной обработки.'
        }
        $current = if ($current -is [IO.DirectoryInfo]) { $current.Parent } else { $current.Directory }
    }
    return $resolved
}
