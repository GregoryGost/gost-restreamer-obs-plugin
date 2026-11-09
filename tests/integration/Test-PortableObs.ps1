#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $ObsRoot,
    [ValidateSet('Portable', 'Normal')][string] $Mode = 'Portable',
    [ValidateSet('ru-RU', 'en-US')][string] $Locale = 'ru-RU',
    [ValidateRange(5, 60)][int] $StartupTimeoutSeconds = 45,
    [ValidateRange(5, 30)][int] $ShutdownTimeoutSeconds = 20
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../fixtures/WorkspacePaths.ps1')
if (-not ('ObsFixtureWindowControl' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class ObsFixtureWindowControl {
    private delegate bool WindowVisitor(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowVisitor visitor, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)] private static extern int GetWindowTextW(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll", ExactSpelling=true)] private static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    public static IntPtr FindMainWindow(int processId) {
        var matches = new List<IntPtr>();
        EnumWindows((window, parameter) => {
            uint ownerProcess;
            GetWindowThreadProcessId(window, out ownerProcess);
            if (ownerProcess == processId && IsWindowVisible(window) && GetWindow(window, 4) == IntPtr.Zero) {
                var title = new StringBuilder(1024);
                GetWindowTextW(window, title, title.Capacity);
                if (title.ToString().StartsWith("OBS 33.0.1 - ", StringComparison.Ordinal))
                    matches.Add(window);
            }
            return true;
        }, IntPtr.Zero);
        return matches.Count == 1 ? matches[0] : IntPtr.Zero;
    }
    public static bool CloseMainWindow(int processId, IntPtr window) {
        uint ownerProcess;
        GetWindowThreadProcessId(window, out ownerProcess);
        return window != IntPtr.Zero && ownerProcess == processId && PostMessageW(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
    }
}
'@
}

function Request-OwnedObsClose {
    $identity = [Diagnostics.Process]::GetProcessById($process.Id)
    try {
        if ($identity.MainModule.FileName -ne $executable -or $identity.StartTime.ToUniversalTime().Ticks -ne $obsStartTicks) {
            throw 'Перед закрытием изменилась идентичность тестового процесса OBS.'
        }
        return [ObsFixtureWindowControl]::CloseMainWindow($process.Id, $obsMainWindowHandle)
    } finally {
        $identity.Dispose()
    }
}

function Assert-NormalPluginPath {
    param([Parameter(Mandatory)][string] $Path)
    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    if ($resolved -ne $normalPluginRoot -and
        -not $resolved.StartsWith($normalPluginRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Проверка обычного режима допускает только согласованный каталог плагина в ProgramData.'
    }
    $current = Get-Item -LiteralPath $resolved -Force
    while ($current) {
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Каталог плагина ProgramData не должен проходить через ссылки.'
        }
        $current = if ($current -is [IO.DirectoryInfo]) { $current.Parent } else { $current.Directory }
    }
    return $resolved
}

function Assert-NormalPluginArtifact {
    $null = Assert-NormalPluginPath $normalPluginRoot
    if ((Get-FileHash -LiteralPath $binary).Hash -ne $expectedBinaryHash) {
        throw 'Временная DLL ProgramData отличается от выбранной сборки.'
    }
    foreach ($language in @('ru-RU', 'en-US')) {
        $localePath = Assert-NormalPluginPath (Join-Path $normalPluginRoot "data/locale/$language.ini")
        $sourceLocale = Assert-WorkspacePath (Join-Path $PSScriptRoot "../../data/locale/$language.ini")
        if ((Get-FileHash -LiteralPath $localePath).Hash -ne (Get-FileHash -LiteralPath $sourceLocale).Hash) {
            throw 'Временная локализация ProgramData отличается от исходника.'
        }
    }
    $files = @(Get-ChildItem -LiteralPath $normalPluginRoot -Recurse -File -Force)
    if ($files.Count -ne 3 -or (Get-ChildItem -LiteralPath $normalPluginRoot -Recurse -Force | Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
    })) { throw 'Временный каталог ProgramData должен содержать только согласованные DLL и две локализации без ссылок.' }
}

function Invoke-NativeFolderPreflight {
    param([Parameter(Mandatory)][Diagnostics.ProcessStartInfo] $ObsStartInfo)
    $command = @'
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class ObsNativeFolders {
    [DllImport("shell32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
    public static extern int SHGetFolderPathW(IntPtr owner, int folder, IntPtr token, uint flags, StringBuilder path);
}
"@
@(@(26, 28, 35) | ForEach-Object {
    $path = [Text.StringBuilder]::new(32768)
    $result = [ObsNativeFolders]::SHGetFolderPathW([IntPtr]::Zero, $_, [IntPtr]::Zero, 0, $path)
    [pscustomobject]@{ Folder = $_; HRESULT = $result; Path = $path.ToString() }
}) | ConvertTo-Json -Compress
'@
    $probe = [Diagnostics.ProcessStartInfo]::new()
    $probe.FileName = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $probe.Arguments = '-NoLogo -NoProfile -NonInteractive -Mta -EncodedCommand ' +
        [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $probe.WorkingDirectory = $ObsStartInfo.WorkingDirectory
    $probe.Environment.Clear()
    foreach ($entry in $ObsStartInfo.Environment.GetEnumerator()) { $probe.Environment[$entry.Key] = $entry.Value }
    $probe.UseShellExecute = $false
    $probe.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $probe.CreateNoWindow = $true
    $probe.RedirectStandardOutput = $true
    $probe.RedirectStandardError = $true
    $probe.StandardOutputEncoding = [Text.Encoding]::UTF8
    $probe.StandardErrorEncoding = [Text.Encoding]::UTF8
    $probeProcess = [Diagnostics.Process]::Start($probe)
    try {
        $output = $probeProcess.StandardOutput.ReadToEndAsync()
        $errors = $probeProcess.StandardError.ReadToEndAsync()
        if (-not $probeProcess.WaitForExit(15000)) {
            $probeProcess.Kill($true)
            $null = $probeProcess.WaitForExit(3000)
            throw 'Нативная проверка каталогов превысила ограниченный бюджет.'
        }
        if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($output, $errors), 3000)) {
            throw 'Чтение результата нативной проверки каталогов превысило бюджет.'
        }
        $json = $output.GetAwaiter().GetResult()
        $errorText = $errors.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $resultsDirectory "launch-$launch-native-folders.json"), $json, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $resultsDirectory "launch-$launch-native-folders-error.txt"), $errorText, [Text.UTF8Encoding]::new($false))
        if ($probeProcess.ExitCode -ne 0) { throw ('Нативная проверка каталогов не выполнена: ' + $errorText) }
        Write-Output $json
        $actual = @($json | ConvertFrom-Json)
        $expected = @{ 26 = $roamingBase; 28 = $localBase; 35 = 'C:\ProgramData' }
        foreach ($folder in @(26, 28, 35)) {
            $entry = @($actual | Where-Object { $_.Folder -eq $folder })
            if ($entry.Count -ne 1 -or $entry[0].HRESULT -ne 0 -or
                $entry[0].Path.TrimEnd('\', '/') -ne $expected[$folder].TrimEnd('\', '/')) {
                throw 'Нативные системные каталоги не совпали с изолированной средой и согласованным ProgramData.'
            }
        }
        if ($actual.Count -ne 3) { throw 'Нативная проверка вернула неожиданный набор каталогов.' }
    } finally {
        $probeProcess.Dispose()
    }
}

$root = Assert-WorkspacePath $ObsRoot
$executable = Assert-WorkspacePath (Join-Path $root 'bin/64bit/obs64.exe')
$version = [Diagnostics.FileVersionInfo]::GetVersionInfo($executable)
if ($version.ProductVersion -notmatch '^33\.0\.1(?:\D|$)') {
    throw 'Изолированный экземпляр должен использовать OBS Studio 33.0.1.'
}
$normalPluginRoot = 'C:\ProgramData\obs-studio\plugins\gost-restreamer-obs-plugin'
$workspaceBinary = Assert-WorkspacePath (Join-Path $PSScriptRoot '../../build_x64/RelWithDebInfo/gost-restreamer-obs-plugin.dll')
$expectedBinaryHash = (Get-FileHash -LiteralPath $workspaceBinary).Hash
if ($Mode -eq 'Normal') {
    $binary = Assert-NormalPluginPath (Join-Path $normalPluginRoot 'gost-restreamer-obs-plugin.dll')
    Assert-NormalPluginArtifact
} else {
    $binary = Assert-WorkspacePath (Join-Path $root 'plugins/gost-restreamer-obs-plugin/gost-restreamer-obs-plugin.dll')
    foreach ($language in @('ru-RU', 'en-US')) {
        $null = Assert-WorkspacePath (Join-Path $root "plugins/gost-restreamer-obs-plugin/data/locale/$language.ini")
    }
    if ((Get-FileHash -LiteralPath $binary).Hash -ne $expectedBinaryHash) { throw 'DLL portable-копии отличается от выбранной сборки.' }
}
$existingObs = @(Get-Process -Name obs64 -ErrorAction SilentlyContinue)
if (($Mode -eq 'Normal' -and $existingObs.Count -ne 0) -or ($existingObs | Where-Object { $_.Path -eq $executable })) {
    throw 'В этом изолированном каталоге OBS уже запущен.'
}
$portableConfig = Join-Path $root 'config'
if (Test-Path -LiteralPath $portableConfig) {
    throw 'Для запуска требуется свежая изолированная копия OBS без каталога config.'
}

function Assert-NormalFixtureLayout {
    foreach ($marker in @('portable_mode', 'obs_portable_mode', 'portable_mode.txt', 'obs_portable_mode.txt', 'plugins', 'obs-plugins')) {
        if (Test-Path -LiteralPath (Join-Path $root $marker)) { throw 'Обычная тестовая копия содержит portable-маркер или локальный каталог плагинов.' }
    }
    if (Test-Path -LiteralPath 'C:\ProgramData\obs-studio\obs-plugins') { throw 'Обнаружен посторонний прежний каталог плагинов ProgramData.' }
    $globalEntries = @(Get-ChildItem -LiteralPath 'C:\ProgramData\obs-studio\plugins' -Force)
    if ($globalEntries.Count -ne 1 -or $globalEntries[0].FullName -ne $normalPluginRoot) {
        throw 'В ProgramData обнаружены плагины за пределами согласованной временной fixture.'
    }
}

if (Get-ChildItem -LiteralPath $root -Recurse -Force | Where-Object {
    ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
}) {
    throw 'Изолированная копия OBS не должна содержать ссылки.'
}
if ($Mode -eq 'Normal') {
    Assert-NormalFixtureLayout
    $userRoot = Join-Path $root 'isolated-user'
    if (Test-Path -LiteralPath $userRoot) { throw 'Каталог изолированного пользователя уже существует.' }
    $roamingBase = Join-Path $userRoot 'AppData/Roaming'
    $localBase = Join-Path $userRoot 'AppData/Local'
    $temporaryDirectory = Join-Path $userRoot 'Temp'
    $null = New-Item -ItemType Directory -Path $roamingBase, $localBase, $temporaryDirectory
    $config = $roamingBase
} else {
    $config = $portableConfig
}

# Эти файлы относятся только к пустому тестовому профилю внутри рабочей копии OBS.
$configuration = Join-Path $config 'obs-studio'
$profileDirectory = Join-Path $configuration 'basic/profiles/Isolated'
$scenes = Join-Path $configuration 'basic/scenes'
$null = New-Item -ItemType Directory -Path $profileDirectory, $scenes -Force
$globalConfig = @"
[General]
LastVersion=553648129
EnableAutoUpdates=false
BrowserHWAccel=false
"@
if ($Mode -eq 'Normal') {
    $locationBase = $roamingBase.Replace('\', '/')
    $globalConfig += "`n[Locations]`nConfiguration=$locationBase`nSceneCollections=$locationBase`nProfiles=$locationBase`nPluginManagerSettings=$locationBase`n"
}
$userConfig = @"
[General]
FirstRun=true
ConfirmOnExit=false
Language=$Locale
[Basic]
Profile=Isolated
ProfileDir=Isolated
SceneCollection=Isolated
SceneCollectionFile=Isolated
ConfigOnNewProfile=false
[BasicWindow]
PreviewEnabled=false
SysTrayEnabled=false
"@
[IO.File]::WriteAllText((Join-Path $configuration 'global.ini'), $globalConfig, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $configuration 'user.ini'), $userConfig, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $profileDirectory 'basic.ini'), "[General]`nName=Isolated`n[Video]`nBaseCX=640`nBaseCY=360`nOutputCX=640`nOutputCY=360`nFPSType=0`nFPSCommon=30`n", [Text.UTF8Encoding]::new($false))
$scene = @{
    name = 'Isolated'; current_scene = 'Blank'; current_program_scene = 'Blank'
    scene_order = @(@{ name = 'Blank' }); groups = @(); transitions = @()
    sources = @(@{ name = 'Blank'; id = 'scene'; settings = @{ items = @() } })
} | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText((Join-Path $scenes 'Isolated.json'), $scene, [Text.UTF8Encoding]::new($false))
$resultsDirectory = Join-Path $configuration 'test-results'
$null = New-Item -ItemType Directory -Path $resultsDirectory
$null = Assert-WorkspacePath $resultsDirectory

$expectedDockLeft = 0
$expectedDockTop = 0
$dockPositionKnown = $false
foreach ($launch in @(1, 2)) {
    if ($launch -eq 2 -and -not $dockPositionKnown) { throw 'Не сохранено проверенное исходное положение Dock.' }
    if (Get-ChildItem -LiteralPath $root -Recurse -Force | Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
    }) {
        throw 'Перед повторным запуском в тестовой среде обнаружены ссылки.'
    }
    if ($Mode -eq 'Normal') {
        if (Get-Process -Name obs64 -ErrorAction SilentlyContinue) { throw 'Обычный тест запрещён при другом процессе OBS.' }
        Assert-NormalFixtureLayout
        Assert-NormalPluginArtifact
    }
    $process = $null
    $obsMainWindowHandle = [IntPtr]::Zero
    $obsStartTicks = 0L
    $success = $false
    try {
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $executable
    $startInfo.WorkingDirectory = Split-Path -Parent $executable
    $startInfo.Arguments = '--multi --disable-updater --disable-missing-files-check --profile Isolated --collection Isolated'
    if ($Mode -eq 'Portable') { $startInfo.Arguments = '--portable ' + $startInfo.Arguments }
    $startInfo.UseShellExecute = $false
    $startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    foreach ($name in @('OBS_PLUGINS_PATH', 'OBS_LEGACY_PLUGINS_PATH', 'OBS_LEGACY_PLUGINS_DATA_PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM', 'QT_QPA_PLATFORM_PLUGIN_PATH')) {
        $null = $startInfo.Environment.Remove($name)
    }
    if ($Mode -eq 'Normal') {
        $startInfo.Environment['USERPROFILE'] = $userRoot
        $startInfo.Environment['APPDATA'] = $roamingBase
        $startInfo.Environment['LOCALAPPDATA'] = $localBase
        $startInfo.Environment['TEMP'] = $temporaryDirectory
        $startInfo.Environment['TMP'] = $temporaryDirectory
        Invoke-NativeFolderPreflight $startInfo
        if (Get-Process -Name obs64 -ErrorAction SilentlyContinue) { throw 'Перед обычным запуском появился другой процесс OBS.' }
        Assert-NormalPluginArtifact
    }
    $launchStartedUtc = [DateTime]::UtcNow
    $process = [Diagnostics.Process]::Start($startInfo)
    $obsStartTicks = $process.StartTime.ToUniversalTime().Ticks
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    $ready = $false
    $log = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) { throw 'Изолированный OBS завершился до готовности.' }
        $obsMainWindowHandle = [ObsFixtureWindowControl]::FindMainWindow($process.Id)
        $log = Get-ChildItem -LiteralPath (Join-Path $configuration 'logs') -Filter '*.txt' -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTimeUtc -ge $launchStartedUtc } |
            Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        if ($log) {
            $logText = Get-Content -LiteralPath $log.FullName -Raw -Encoding utf8
            $modeLine = if ($Mode -eq 'Portable') { 'Portable mode: true' } else { 'Portable mode: false' }
            $ready = $logText -match [regex]::Escape($modeLine) -and
                $logText -match 'GOST Restreamer загружен' -and
                $logText -match '==== Startup complete' -and
                $logText -match 'Loaded scenes:' -and $obsMainWindowHandle -ne [IntPtr]::Zero
            if ($ready) { break }
        }
        Start-Sleep -Milliseconds 150
    }
    if (-not $ready) { throw 'Не подтверждена готовность плагина в выбранном изолированном режиме OBS.' }
    $uiHelper = Assert-WorkspacePath (Join-Path $PSScriptRoot 'Test-ObsUi.ps1')
    $uiCommand = @'
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$source = [IO.File]::ReadAllText($env:GOST_UIA_SCRIPT, [Text.Encoding]::UTF8)
& ([scriptblock]::Create($source)) -ObsProcessId ([int]$env:GOST_UIA_PID) -ExpectedExecutable $env:GOST_UIA_EXECUTABLE -ExpectedStartTicks ([long]$env:GOST_UIA_START) -MainWindowHandle ([long]$env:GOST_UIA_WINDOW) -WorkspaceRoot $env:GOST_UIA_ROOT -ExpectedPluginPath $env:GOST_UIA_PLUGIN -ExpectedPluginSHA256 $env:GOST_UIA_PLUGIN_SHA -Locale $env:GOST_UIA_LOCALE -Launch ([int]$env:GOST_UIA_LAUNCH) -ExpectedDockLeft ([int]$env:GOST_UIA_DOCK_LEFT) -ExpectedDockTop ([int]$env:GOST_UIA_DOCK_TOP)
'@
    $uiStart = [Diagnostics.ProcessStartInfo]::new()
    $uiStart.FileName = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($uiCommand))
    $uiStart.Arguments = '-NoLogo -NoProfile -NonInteractive -Mta -EncodedCommand ' + $encoded
    $uiStart.WorkingDirectory = $root
    $uiStart.UseShellExecute = $false
    $uiStart.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $uiStart.CreateNoWindow = $true
    $uiStart.RedirectStandardOutput = $true
    $uiStart.RedirectStandardError = $true
    $uiStart.StandardOutputEncoding = [Text.Encoding]::UTF8
    $uiStart.StandardErrorEncoding = [Text.Encoding]::UTF8
    $uiStart.Environment.Clear()
    foreach ($entry in $startInfo.Environment.GetEnumerator()) { $uiStart.Environment[$entry.Key] = $entry.Value }
    $uiStart.Environment['GOST_UIA_SCRIPT'] = $uiHelper
    $uiStart.Environment['GOST_UIA_PID'] = $process.Id.ToString()
    $uiStart.Environment['GOST_UIA_EXECUTABLE'] = $executable
    $uiStart.Environment['GOST_UIA_START'] = $obsStartTicks.ToString()
    $uiStart.Environment['GOST_UIA_WINDOW'] = $obsMainWindowHandle.ToInt64().ToString()
    $uiStart.Environment['GOST_UIA_ROOT'] = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
    $uiStart.Environment['GOST_UIA_LOCALE'] = $Locale
    $uiStart.Environment['GOST_UIA_LAUNCH'] = $launch.ToString()
    $uiStart.Environment['GOST_UIA_DOCK_LEFT'] = $expectedDockLeft.ToString()
    $uiStart.Environment['GOST_UIA_DOCK_TOP'] = $expectedDockTop.ToString()
    $uiStart.Environment['GOST_UIA_PLUGIN'] = $binary
    $uiStart.Environment['GOST_UIA_PLUGIN_SHA'] = $expectedBinaryHash
    $uiStart.Environment['TEMP'] = $root
    $uiStart.Environment['TMP'] = $root
    $uiProcess = [Diagnostics.Process]::Start($uiStart)
    try {
        $uiOutputTask = $uiProcess.StandardOutput.ReadToEndAsync()
        $uiErrorTask = $uiProcess.StandardError.ReadToEndAsync()
        $uiTimedOut = -not $uiProcess.WaitForExit(50000)
        if ($uiTimedOut) {
            $uiProcess.Kill($true)
            $null = $uiProcess.WaitForExit(3000)
        }
        if (-not [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($uiOutputTask, $uiErrorTask), 3000)) {
            throw 'Чтение вывода проверки интерфейса превысило ограниченный бюджет.'
        }
        $uiOutput = $uiOutputTask.GetAwaiter().GetResult()
        $uiError = $uiErrorTask.GetAwaiter().GetResult()
        $resultsDirectory = Join-Path $configuration 'test-results'
        $null = New-Item -ItemType Directory -Path $resultsDirectory -Force
        $null = Assert-WorkspacePath $resultsDirectory
        [IO.File]::WriteAllText((Join-Path $resultsDirectory "launch-$launch-ui-output.txt"), $uiOutput, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $resultsDirectory "launch-$launch-ui-error.txt"), $uiError, [Text.UTF8Encoding]::new($false))
        if ($uiTimedOut) {
            if ($uiOutput) { Write-Output $uiOutput }
            throw 'Проверка интерфейса OBS превысила жёсткий бюджет.'
        }
        if ($uiProcess.ExitCode -ne 0) {
            if ($uiOutput) { Write-Output $uiOutput }
            throw ('Проверка интерфейса OBS не пройдена: ' + $uiError)
        }
        $uiResult = $uiOutput | ConvertFrom-Json
        if ($uiResult.Check -ne 'ObsUi' -or $uiResult.Locale -ne $Locale -or $uiResult.ProcessId -ne $process.Id -or
            $uiResult.Launch -ne $launch -or ($launch -eq 2 -and
                (-not $uiResult.DockVisibilityRestored -or -not $uiResult.DockPositionVerified))) {
            throw 'Helper не вернул подтверждение проверки своего процесса OBS.'
        }
        if ($launch -eq 1) {
            $expectedDockLeft = [int]$uiResult.DockLeft
            $expectedDockTop = [int]$uiResult.DockTop
            $dockPositionKnown = $true
        }
        Write-Output $uiOutput
    } finally {
        $uiProcess.Dispose()
    }
    if (-not (Request-OwnedObsClose)) { throw 'Не удалось запросить штатное завершение своего экземпляра OBS.' }
    if (-not $process.WaitForExit($ShutdownTimeoutSeconds * 1000)) { throw 'Штатное завершение OBS превысило бюджет.' }
    if ($process.ExitCode -ne 0) { throw 'OBS завершился с ошибкой.' }
    $logText = Get-Content -LiteralPath $log.FullName -Raw -Encoding utf8
    if ($logText -notmatch '==== Shutting down') { throw 'Не найдено подтверждение штатного завершения OBS.' }
    if ($logText -match '==== (Streaming|Recording|Replay Buffer|Virtual Camera) Start' -or
        $logText -match '\[Loaded global audio device\]' -or $logText -match '\[(?:WASAPISource|DShow|window-capture|game-capture|monitor-capture)') {
        throw 'В пустой тестовой конфигурации обнаружена активность медиатракта.'
    }
    if ($logText -match '(?i)(failed to load|failed to open|not found|legacy)[^\r\n]*gost-restreamer-obs-plugin|gost-restreamer-obs-plugin[^\r\n]*(failed|legacy)') {
        throw 'OBS сообщил об ошибке загрузки или прежнем формате размещения плагина.'
    }
    $checkName = if ($Mode -eq 'Portable') { 'PortableObs' } else { 'NormalObs' }
    $obsResult = [pscustomobject]@{ Check = $checkName; Mode = $Mode; Launch = $launch; Version = $version.ProductVersion; Locale = $Locale
        ExitCode = $process.ExitCode; SHA256 = (Get-FileHash -LiteralPath $binary).Hash
        Ui = $uiResult; HelperSHA256 = (Get-FileHash -LiteralPath $uiHelper).Hash; RunnerSHA256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash }
    $resultJson = $obsResult | ConvertTo-Json -Depth 7
    [IO.File]::WriteAllText((Join-Path $resultsDirectory "launch-$launch-result.json"), $resultJson, [Text.UTF8Encoding]::new($false))
    Write-Output ($obsResult | ConvertTo-Json -Depth 7 -Compress)
    $success = $true
    } finally {
    if ($process) {
        if (-not $process.HasExited) {
            $null = Request-OwnedObsClose
            if (-not $process.WaitForExit(3000)) {
                $process.Kill($true)
                $null = $process.WaitForExit(3000)
                Write-Warning 'Свой тестовый процесс принудительно завершён после отказа; проверка не пройдена.'
            }
        }
        $process.Dispose()
    }
    if (-not $success) { Write-Warning 'Артефакты изолированного запуска сохранены для диагностики.' }
    }
}
