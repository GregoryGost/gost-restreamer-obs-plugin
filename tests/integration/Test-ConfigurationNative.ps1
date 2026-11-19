#Requires -Version 7.0
<# Запускает собственную portable OBS, выполняет заданный UIA-план либо штатно закрывает её.
Mode выбирает Start/Ui/Stop, FixtureRoot ограничивает все файлы workspace,
PlanPath содержит действия UIA. Origin допускает только выделенный loopback proxy.
После Start процесс намеренно остаётся для следующих фаз; Stop перепроверяет идентичность.
Сервер WSL и его сервисы принадлежат пользователю и этим сценарием не останавливаются. #>
param([Parameter(Mandatory)][ValidateSet('Start','Ui','Stop')][string] $Mode,
      [Parameter(Mandatory)][string] $FixtureRoot,
      [string] $PlanPath,
      [ValidatePattern('^http://127\.0\.0\.1:1808[12]$')][string] $Origin='http://127.0.0.1:18081',
      [ValidateSet('ru-RU','en-US')][string] $Locale='ru-RU')

# ErrorActionPreference исключает продолжение после неподтверждённой подготовки.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../fixtures/WorkspacePaths.ps1')
# fixture/workspace/obsRoot/results/temporary/identityPath задают изолированные пути текущей проверки.
$fixture=Assert-WorkspacePath $FixtureRoot
$workspace=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$obsRoot=Assert-WorkspacePath (Join-Path $fixture ('obs-'+$Locale))
$results=Assert-WorkspacePath (Join-Path $fixture 'results')
$temporary=Assert-WorkspacePath (Join-Path $fixture 'tmp')
$identityPath=Join-Path $fixture ('identity-'+$Locale+'.json')
$env:TEMP=$temporary
$env:TMP=$temporary

# nativeSource импортирует только уже проверенный Win32-класс, без запуска старого тестового сервера.
$nativeSource=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'Test-Dev8ProfileNative.ps1'),[Text.Encoding]::UTF8)
# nativeClass извлекает единственный C#-блок с типом ProfileFixtureWindows.
$nativeClass=[regex]::Match($nativeSource,"(?s)(using System;.*?`r?`n})`r?`n'@")
if (-not $nativeClass.Success) { throw 'Не найден проверенный Win32-класс.' }
Add-Type -TypeDefinition $nativeClass.Groups[1].Value
[ProfileFixtureWindows]::AssertDesktop()

<# Возвращает процесс только после сравнения PID, полного EXE, start-time и собственного HWND.
Identity содержит сохранённую этим runner идентичность; результат освобождает вызывающий код. #>
function Get-AcceptanceProcess {
    param($Identity)
    # owned и owner проверяют процесс и принадлежность HWND независимо друг от друга.
    $owned=[Diagnostics.Process]::GetProcessById([int]$Identity.pid)
    $owner=[uint32]0
    $null=[ProfileFixtureWindows]::GetWindowThreadProcessId([IntPtr]::new([long]$Identity.hwnd),[ref]$owner)
    if ($owned.HasExited -or $owned.MainModule.FileName -cne $Identity.executable -or $owned.StartTime.ToUniversalTime().Ticks -ne [long]$Identity.startTicks -or $owner -ne [int]$Identity.pid) { $owned.Dispose(); throw 'Изменилась идентичность собственного OBS.' }
    return $owned
}

<# Читает журнал текущего собственного OBS с разрешённой одновременной записью.
Path не публикует содержимое журнала в stdout, значение возвращается только для проверок готовности. #>
function Read-AcceptanceLog {
    param([string] $Path)
    # stream и reader освобождаются также при ошибке чтения.
    $stream=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose(); $stream.Dispose() }
}

if ($Mode -eq 'Start') {
    if (Test-Path -LiteralPath $identityPath) {
        # previous запрещает второй параллельный запуск той же копии; старый PID проверяется только на существование.
        $previous=Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json
        # previousProcess может ещё перечисляться Windows после завершения, пока кто-то держит его дескриптор.
        $previousProcess=Get-Process -Id ([int]$previous.pid) -ErrorAction SilentlyContinue
        if ($previousProcess) { try { if (-not $previousProcess.HasExited) { throw 'Предыдущий процесс fixture ещё работает.' } } finally { $previousProcess.Dispose() } }
    }
    # configuration/profile/scenes/pluginConfig принадлежат только этой portable-копии.
    $configuration=Join-Path $obsRoot 'config/obs-studio'
    $profile=Join-Path $configuration 'basic/profiles/Isolated'
    $scenes=Join-Path $configuration 'basic/scenes'
    $pluginConfig=Join-Path $configuration 'plugin_config/gost-restreamer-obs-plugin'
    if (-not (Test-Path -LiteralPath $configuration)) {
        $null=New-Item -ItemType Directory -Path $profile,$scenes,$pluginConfig -Force
        [IO.File]::WriteAllText((Join-Path $configuration 'global.ini'),"[General]`nLastVersion=553648129`nEnableAutoUpdates=false`nBrowserHWAccel=false`n",[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $configuration 'user.ini'),"[General]`nFirstRun=true`nConfirmOnExit=false`nLanguage=$Locale`n[Basic]`nProfile=Isolated`nProfileDir=Isolated`nSceneCollection=Isolated`nSceneCollectionFile=Isolated`nConfigOnNewProfile=false`n[BasicWindow]`nPreviewEnabled=false`nSysTrayEnabled=false`n",[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $profile 'basic.ini'),"[General]`nName=Isolated`n[Video]`nBaseCX=640`nBaseCY=360`nOutputCX=640`nOutputCY=360`nFPSType=0`nFPSCommon=30`n",[Text.UTF8Encoding]::new($false))
        # scene содержит единственную пустую сцену без пользовательских источников.
        $scene=@{name='Isolated';current_scene='Blank';current_program_scene='Blank';scene_order=@(@{name='Blank'});groups=@();transitions=@();sources=@(@{name='Blank';id='scene';settings=@{items=@()}})}
        [IO.File]::WriteAllText((Join-Path $scenes 'Isolated.json'),($scene|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
        # preferences направляет первый запуск только в локальный стенд, без прежних origins/привязок.
        $preferences=@{schema_version=3;server_origin=$Origin;auto_connect=$true;reconnect=$true;sse_rejection_retries=3;http_timeout_seconds=5;log_level='detailed';profiles=@();bindings=@()}
        [IO.File]::WriteAllText((Join-Path $pluginConfig 'settings.json'),($preferences|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
    }
    # executable/pluginFile/expectedHash фиксируют единственную разрешённую копию и выбранную сборку.
    $executable=Assert-WorkspacePath (Join-Path $obsRoot 'bin/64bit/obs64.exe')
    $pluginFile=Assert-WorkspacePath (Join-Path $obsRoot 'plugins/gost-restreamer-obs-plugin/gost-restreamer-obs-plugin.dll')
    $expectedHash=(Get-FileHash -LiteralPath (Join-Path $workspace 'build/install/gost-restreamer-obs-plugin/gost-restreamer-obs-plugin.dll') -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $pluginFile -Algorithm SHA256).Hash -cne $expectedHash) { throw 'Копия плагина не совпала с готовой сборкой.' }
    # startInfo задаёт видимый Default desktop, собственный TEMP и отсутствие чужих Qt/OBS overrides.
    $startInfo=[Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName=$executable
    $startInfo.WorkingDirectory=Split-Path -Parent $executable
    # effectiveProfile сохраняет фактическое имя профиля после его штатного переименования в OBS.
    $effectiveProfile=([regex]::Match([IO.File]::ReadAllText((Join-Path $configuration 'user.ini')),'(?m)^Profile=(.+)\r?$')).Groups[1].Value.TrimEnd("`r")
    if ([string]::IsNullOrWhiteSpace($effectiveProfile)) { throw 'Не определён фактический профиль portable OBS.' }
    # argument передаётся как отдельный argv без shell quoting и интерпретации имени профиля.
    foreach ($argument in @('--portable','--multi','--disable-updater','--disable-missing-files-check','--profile',$effectiveProfile,'--collection','Isolated')) { $startInfo.ArgumentList.Add($argument) }
    $startInfo.UseShellExecute=$false
    $startInfo.WindowStyle=[Diagnostics.ProcessWindowStyle]::Normal
    # variable обозначает исключаемое переопределение среды только собственного дочернего процесса.
    foreach ($variable in @('OBS_PLUGINS_PATH','OBS_LEGACY_PLUGINS_PATH','OBS_LEGACY_PLUGINS_DATA_PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM','QT_QPA_PLATFORM_PLUGIN_PATH')) { $null=$startInfo.Environment.Remove($variable) }
    $startInfo.Environment['TEMP']=$temporary
    $startInfo.Environment['TMP']=$temporary
    # startedAt/deadline/obs/identity/log задают текущую попытку и её наблюдаемую готовность.
    $startedAt=[DateTime]::UtcNow
    $deadline=$startedAt.AddSeconds(30)
    # effectivePreferences подтверждает действующий адрес из собственной конфигурации вместо CLI-значения по умолчанию.
    $effectivePreferences=Get-Content -LiteralPath (Join-Path $pluginConfig 'settings.json') -Raw | ConvertFrom-Json
    if ($effectivePreferences.server_origin -notmatch '^http://127\.0\.0\.1:1808[12]$') { throw 'Повторный запуск запретил посторонний origin.' }
    $obs=$null
    $identity=$null
    $log=$null
    try {
        $obs=[Diagnostics.Process]::Start($startInfo)
        $identity=[ordered]@{pid=$obs.Id;executable=$executable;startTicks=$obs.StartTime.ToUniversalTime().Ticks.ToString();hwnd=0;plugin=$pluginFile;pluginSha256=$expectedHash;origin=$effectivePreferences.server_origin;profileName=$effectiveProfile;locale=$Locale;results=$results;log=$null;owner='configuration-acceptance';cleanup='IntentionallyRunning'}
        [IO.File]::WriteAllText($identityPath,($identity|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
        do {
            $obs.Refresh()
            if ($obs.HasExited) { throw 'OBS завершился до готовности.' }
            $identity.hwnd=[ProfileFixtureWindows]::MainWindow($obs.Id).ToInt64()
            $log=Get-ChildItem -LiteralPath (Join-Path $configuration 'logs') -Filter '*.txt' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTimeUtc -ge $startedAt } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
            if ($log -and $identity.hwnd -ne 0 -and (Read-AcceptanceLog $log.FullName).Contains('==== Startup complete')) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        if (-not $log -or $identity.hwnd -eq 0) { throw 'Не подтверждены HWND и свежий журнал.' }
        # readyLog подтверждает portable и загрузку плагина, а не просто существование процесса.
        $readyLog=Read-AcceptanceLog $log.FullName
        if (-not $readyLog.Contains('Portable mode: true') -or -not $readyLog.Contains('GOST Restreamer загружен') -or -not $readyLog.Contains('==== Startup complete') -or [ProfileFixtureWindows]::GetDpiForWindow([IntPtr]::new($identity.hwnd)) -ne 96) { throw 'Не подтверждены загрузка portable/DLL и DPI100%.' }
        $identity.log=$log.FullName
        [IO.File]::WriteAllText($identityPath,($identity|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
        $identity|ConvertTo-Json -Compress
    } catch {
        if ($obs -and -not $obs.HasExited -and $obs.MainModule.FileName -ceq $executable) { $null=[ProfileFixtureWindows]::PostMessageW([ProfileFixtureWindows]::MainWindow($obs.Id),0x10,[IntPtr]::Zero,[IntPtr]::Zero); if (-not $obs.WaitForExit(10000)) { $obs.Kill($true); $null=$obs.WaitForExit(3000) } }
        throw
    } finally { if ($obs) { $obs.Dispose() } }
} elseif ($Mode -eq 'Ui') {
    # identity/owned повторно проверяют процесс до запуска ограниченного помощника.
    $identity=Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json
    # currentPreferences обновляет изменяемый origin сессии после сохранения адреса через настоящий UI.
    $currentPreferences=Get-Content -LiteralPath (Join-Path $obsRoot 'config/obs-studio/plugin_config/gost-restreamer-obs-plugin/settings.json') -Raw | ConvertFrom-Json
    if ($currentPreferences.server_origin -notmatch '^http://127\.0\.0\.1:1808[12]$') { throw 'UIA запрещён для постороннего origin.' }
    $identity.origin=$currentPreferences.server_origin
    [IO.File]::WriteAllText($identityPath,($identity|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    $owned=Get-AcceptanceProcess $identity
    $owned.Dispose()
    $PlanPath=Assert-WorkspacePath $PlanPath
    # resultPath/uiStart/loader задают отдельный отчёт и короткую команду без ограничения 32K.
    $resultPath=Join-Path $results (([IO.Path]::GetFileNameWithoutExtension($PlanPath))+'.result.json')
    $uiStart=[Diagnostics.ProcessStartInfo]::new()
    $uiStart.FileName=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $loader='& ([scriptblock]::Create([IO.File]::ReadAllText($env:CONFIGURATION_HELPER,[Text.Encoding]::UTF8))) -IdentityPath $env:CONFIGURATION_IDENTITY -PlanPath $env:CONFIGURATION_PLAN -ResultPath $env:CONFIGURATION_RESULT'
    $uiStart.Arguments='-NoLogo -NoProfile -NonInteractive -Mta -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($loader))
    $uiStart.WorkingDirectory=$workspace
    $uiStart.UseShellExecute=$false
    $uiStart.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    $uiStart.CreateNoWindow=$true
    $uiStart.RedirectStandardOutput=$true
    $uiStart.RedirectStandardError=$true
    $uiStart.Environment['CONFIGURATION_WORKSPACE']=$workspace
    $uiStart.Environment['CONFIGURATION_HELPER']=Join-Path $PSScriptRoot 'Invoke-ConfigurationNativeUi.ps1'
    $uiStart.Environment['CONFIGURATION_IDENTITY']=$identityPath
    $uiStart.Environment['CONFIGURATION_PLAN']=$PlanPath
    $uiStart.Environment['CONFIGURATION_RESULT']=$resultPath
    $uiStart.Environment['TEMP']=$temporary
    $uiStart.Environment['TMP']=$temporary
    # helper/stdout/stderr удерживают только собственный процесс и оба его канала без взаимной блокировки.
    $helper=[Diagnostics.Process]::Start($uiStart)
    $stdout=$helper.StandardOutput.ReadToEndAsync()
    $stderr=$helper.StandardError.ReadToEndAsync()
    try {
        if (-not $helper.WaitForExit(120000)) { $helper.Kill($true); $null=$helper.WaitForExit(3000); throw 'Собственный UIA помощник превысил120с.' }
        [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdout,$stderr),3000) | Out-Null
        [IO.File]::WriteAllText(($resultPath+'.stdout.txt'),$stdout.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText(($resultPath+'.stderr.txt'),$stderr.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
        if (-not (Test-Path -LiteralPath $resultPath)) { throw 'UIA не записал отчёт; см. stderr.' }
        # outcome проверяется независимо от exit code дочернего runner.
        $outcome=Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
        [pscustomobject]@{plan=$PlanPath;result=$resultPath;passed=$outcome.Passed;error=$outcome.Error;steps=@($outcome.Checks).Count}|ConvertTo-Json -Compress
        if ($helper.ExitCode -ne 0 -or -not $outcome.Passed -or $outcome.ProcessId -ne [int]$identity.pid) { throw 'Нативный план не принят; исходный отказ сохранён.' }
    } finally { $helper.Dispose() }
} else {
    # identity/owned фиксируют единственный процесс перед адресным штатным WM_CLOSE.
    $identity=Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json
    $owned=Get-AcceptanceProcess $identity
    try {
        # handle удерживает собственный дескриптор до WM_CLOSE: повторно найденный Process иначе может потерять ExitCode.
        $handle=$owned.Handle
        $null=[ProfileFixtureWindows]::PostMessageW([IntPtr]::new([long]$identity.hwnd),0x10,[IntPtr]::Zero,[IntPtr]::Zero)
        if (-not $owned.WaitForExit(20000)) { throw 'Собственный OBS не завершился штатно; требуется диагностика.' }
        $owned.Refresh()
        # exitCode фиксируется до освобождения handle и сохраняется даже при отрицательной приёмке cleanup.
        $exitCode=$owned.ExitCode
        $identity|Add-Member -NotePropertyName exitCode -NotePropertyValue $exitCode -Force
        [IO.File]::WriteAllText($identityPath,($identity|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
        if ($null -eq $exitCode -or $exitCode -ne 0 -or -not (Read-AcceptanceLog $identity.log).Contains('==== Shutting down')) { throw 'Не подтверждено штатное завершение.' }
        $identity.cleanup='Graceful'
        [IO.File]::WriteAllText($identityPath,($identity|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
        [pscustomobject]@{cleanup='Graceful';pid=$identity.pid;exitCode=$exitCode;remaining=(-not $owned.HasExited)} | ConvertTo-Json -Compress
    } finally { $owned.Dispose() }
}
