Set-StrictMode -Version Latest

function Assert-WorkspacePath {
    param([Parameter(Mandatory)][string] $Path)

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
