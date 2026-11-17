#Requires -Version 7.0
<#
.SYNOPSIS
Проверяет редактор профиля и уведомления в собственной portable OBS с сервером в памяти.
.DESCRIPTION
ObsRoot уже подготовлен базовым Test-PortableObs.ps1 и содержит только тестовый профиль Isolated.
Сценарий заменяет только настройки плагина этой копии, запускает OBS видимо на Default при 100%,
а UIA — скрытым Windows PowerShell 5.1 MTA. HTTP слушает только случайный loopback-порт;
все изменения профиля отвергаются синтетической Problem, запросы остаются в RAM.
Результат считается успешным только по JSON помощника, журналу и штатному завершению OBS.
.PARAMETER ObsRoot
Подготовленная внутри workspace portable OBS 33.0.1 с собственным config и актуальным плагином.
.PARAMETER ExpectedBinary
Файл выбранной сборки внутри workspace; ExpectedBinarySha256 фиксирует его ожидаемый SHA256.
.PARAMETER SnapshotId
Внешняя идентичность стабильного снимка исходников и тестов, фиксируемая основным исполнителем.
.PARAMETER ResultRoot
Существующий пустой каталог результатов внутри workspace. TEMP/TMP дочерних процессов находятся здесь.
.PARAMETER Locale
Локаль собственной конфигурации, отдельно ru-RU и en-US.
.PARAMETER LogMode
Режим журнала собственной конфигурации: normal либо detailed.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $ObsRoot,
    [Parameter(Mandatory)][string] $ExpectedBinary,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string] $ExpectedBinarySha256,
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string] $SnapshotId,
    [Parameter(Mandatory)][string] $ResultRoot,
    [Parameter(Mandatory)][ValidateSet('ru-RU', 'en-US')][string] $Locale,
    [ValidateSet('normal', 'detailed')][string] $LogMode = 'normal'
)

# ErrorActionPreference запрещает продолжение после неудачной подготовки fixture.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../fixtures/WorkspacePaths.ps1')

# nativeSource определяет только наблюдение desktop/окон и адресное закрытие собственного HWND.
$nativeSource = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
// Наблюдает окна и desktop без переключения рабочего стола или чужих очередей ввода.
public static class ProfileFixtureWindows {
    // Visitor получает HWND и неприменяемый пользовательский параметр перечисления.
    private delegate bool Visitor(IntPtr window, IntPtr parameter);
    // Перечисляет верхние окна текущего desktop через visitor с parameter.
    [DllImport("user32.dll")] private static extern bool EnumWindows(Visitor visitor, IntPtr parameter);
    // Возвращает поток HWND window, записывая его PID в processId.
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    // Проверяет видимость HWND window.
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    // Читает заголовок window в text длиной count.
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetWindowTextW(IntPtr window, StringBuilder text, int count);
    // Возвращает desktop потока threadId.
    [DllImport("user32.dll")] public static extern IntPtr GetThreadDesktop(uint threadId);
    // Возвращает идентификатор вызывающего потока.
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    // Открывает активный desktop с flags/inherit/access для последующего закрытия.
    [DllImport("user32.dll")] private static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    // Закрывает только дескриптор desktop, открытый этим классом.
    [DllImport("user32.dll")] private static extern bool CloseDesktop(IntPtr desktop);
    // Читает имя объекта handle в value длиной length; needed получает требуемый размер.
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern bool GetUserObjectInformationW(IntPtr handle, int index, StringBuilder value, int length, out int needed);
    // Отправляет message с wParam/lParam только в явно проверенный window.
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    // Возвращает физический DPI окна window.
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
    // Читает имя desktop; ошибку Win32 считает непригодной средой.
    public static string DesktopName(IntPtr desktop) {
        // value получает имя desktop, needed — требуемый размер буфера в байтах.
        var value = new StringBuilder(512);
        int needed;
        if (desktop == IntPtr.Zero || !GetUserObjectInformationW(desktop, 2, value, 1024, out needed))
            throw new InvalidOperationException("Не удалось прочитать имя рабочего стола.");
        return value.ToString();
    }
    // Требует Default у вызывающего потока и активного desktop; ничего не переключает.
    public static void AssertDesktop() {
        // input освобождается даже при несовпадении активного рабочего стола.
        var input = OpenInputDesktop(0, false, 1);
        try {
            if (DesktopName(GetThreadDesktop(GetCurrentThreadId())) != "Default" || DesktopName(input) != "Default")
                throw new InvalidOperationException("Проверка требует интерактивный Default desktop.");
        } finally { if (input != IntPtr.Zero) CloseDesktop(input); }
    }
    // Возвращает единственный видимый главный HWND OBS указанного processId либо ноль.
    public static IntPtr MainWindow(int processId) {
        // matches хранит только окна OBS 33.0.1; callback проверяет принадлежность до чтения заголовка.
        var matches = new List<IntPtr>();
        EnumWindows((window, parameter) => {
            // owner получает PID окна, title — его ограниченный заголовок.
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == processId && IsWindowVisible(window)) {
                var title = new StringBuilder(1024);
                GetWindowTextW(window, title, title.Capacity);
                if (title.ToString().StartsWith("OBS 33.0.1 - ", StringComparison.Ordinal)) matches.Add(window);
            }
            return true;
        }, IntPtr.Zero);
        return matches.Count == 1 ? matches[0] : IntPtr.Zero;
    }
}
'@
Add-Type -TypeDefinition $nativeSource

<# Возвращает текущий процесс только при совпадении PID, пути и startTicks.
RequireWindow дополнительно требует принадлежности главного HWND перед оконным действием.
Очистка процесса допускается до появления окна; возвращённый Process вызывающий код освобождает сам. #>
function Get-OwnedObs {
    param([switch] $RequireWindow)
    # current повторно разрешает PID и защищает очистку от его переиспользования.
    $current = [Diagnostics.Process]::GetProcessById($obs.Id)
    try {
        if ($current.HasExited -or $current.MainModule.FileName -cne $executable -or
            $current.StartTime.ToUniversalTime().Ticks -ne $startTicks) {
            throw 'Идентичность собственного OBS изменилась.'
        }
        if ($RequireWindow) {
            # owner сверяет PID HWND отдельно от идентичности процесса.
            $owner = [uint32]0
            $null = [ProfileFixtureWindows]::GetWindowThreadProcessId($window, [ref]$owner)
            if ($window -eq [IntPtr]::Zero -or $owner -ne $obs.Id) { throw 'Главный HWND собственного OBS изменился.' }
        }
        return $current
    } catch {
        $current.Dispose()
        throw
    }
}

<# Обслуживает готовые loopback-запросы без внешнего backend. Ограничивает заголовки/тело;
SSE остаётся открытым до cleanup. Разрешён только GET и отвергаемый POST профиля.
Список requests содержит только синтетические тела текущего процесса и не записывается на диск. #>
function Receive-FixtureRequests {
    if ([DateTime]::UtcNow -ge $script:nextHeartbeat) {
        # heartbeat удерживает открытую подписку; sseClient адресует только соединение собственной fixture.
        $heartbeat=[Text.Encoding]::ASCII.GetBytes(": keepalive`n`n")
        foreach ($sseClient in @($sseClients.ToArray())) {
            try { $sseClient.GetStream().Write($heartbeat,0,$heartbeat.Length) }
            catch { $sseClient.Dispose(); $null=$sseClients.Remove($sseClient) }
        }
        $script:nextHeartbeat=[DateTime]::UtcNow.AddSeconds(3)
    }
    while ($listener.Pending()) {
        # client представляет одно локальное соединение; stream читает байты заголовков и тела запроса.
        $client = $listener.AcceptTcpClient()
        $stream = $client.GetStream()
        $stream.ReadTimeout = 2000
        $stream.WriteTimeout = 2000
        # header собирается побайтово до CRLFCRLF, byte — очередной байт, marker — последние четыре байта.
        $header = [Collections.Generic.List[byte]]::new()
        $byte = 0
        $marker = ''
        try {
            while ($header.Count -lt 16384) {
                $byte = $stream.ReadByte()
                if ($byte -lt 0) { throw 'HTTP-фикстура получила незавершённый заголовок.' }
                $header.Add([byte]$byte)
                $marker = ($marker + [char]$byte)
                if ($marker.Length -gt 4) { $marker = $marker.Substring($marker.Length - 4) }
                if ($marker -ceq "`r`n`r`n") { break }
            }
            if ($marker -cne "`r`n`r`n") { throw 'Заголовки HTTP превысили лимит фикстуры.' }
            # text содержит завершённые заголовки, line — стартовую строку, length — число байт тела.
            $text = [Text.Encoding]::ASCII.GetString($header.ToArray())
            $line = $text.Split("`r`n")[0].Split(' ')
            $length = 0
            if ($text -match '(?im)^Content-Length:\s*(\d+)\s*$') { $length = [int]$Matches[1] }
            if ($length -gt 65536 -or $text -match '(?im)^Transfer-Encoding:') { throw 'Неожиданное тело HTTP-запроса.' }
            # body хранит ровно length байт, offset и read учитывают частичные чтения.
            $body = [byte[]]::new($length)
            $offset = 0
            while ($offset -lt $length) {
                $read = $stream.Read($body, $offset, $length - $offset)
                if ($read -le 0) { throw 'HTTP-фикстура получила неполное тело.' }
                $offset += $read
            }
            # method/path описывают только loopback-запрос; response/status/type задают ответ fixture.
            $method = $line[0]
            $path = $line[1].Split('?')[0]
            $response = '{"items":[],"next_cursor":""}'
            $status = '200 OK'
            $type = 'application/json'
            if ($method -ceq 'POST' -and $path -ceq '/api/v1/transcode-profiles') {
                $requests.Add([Text.Encoding]::UTF8.GetString($body))
                $status = '409 Conflict'
                $type = 'application/problem+json'
                $response = @{ type='about:blank'; title='Profile unavailable'; status=409; code='profile_locked'
                    detail="<b>literal rejection $($requests.Count)</b>"; action="<i>change profile $($requests.Count)</i>"
                    diagnostic="fixture-secret-$($requests.Count)" } | ConvertTo-Json -Compress
            } elseif ($method -cne 'GET') {
                throw 'Фикстура запрещает незапланированную операцию записи.'
            } elseif ($path -ceq '/fixture/status') {
                $response = @{ mutations=$requests.Count; lastBody=if ($requests.Count) { $requests[$requests.Count-1] | ConvertFrom-Json } else { $null } } | ConvertTo-Json -Depth 25 -Compress
            } elseif ($path -like '/api/v1/health/*') {
                $response = '{"status":"ready","reason":""}'
            } elseif ($path -ceq '/api/v1/system/capabilities') {
                $response = '{"ffmpeg":{"status":"unverified","reason":"","checked_at":null},"ffprobe":{"status":"unverified","reason":"","checked_at":null},"helper":{"status":"unverified","reason":"","checked_at":null},"nvidia":{"status":"unverified","reason":"","checked_at":null}}'
            } elseif ($path -ceq '/api/v1/events/snapshot') {
                $response = '{"cursor":"fixture-cursor","sessions":[],"gpu":null}'
            } elseif ($path -ceq '/api/v1/events') {
                # wire содержит открывающий SSE-заголовок и комментарий; собственный client удерживается до shutdown.
                $wire = [Text.Encoding]::UTF8.GetBytes("HTTP/1.1 200 OK`r`nContent-Type: text/event-stream`r`nConnection: keep-alive`r`n`r`n: ready`n`n")
                $stream.Write($wire, 0, $wire.Length)
                $sseClients.Add($client)
                $client = $null
                continue
            } elseif ($path -notin @('/api/v1/streams','/api/v1/transcode-profiles','/api/v1/transcode-groups','/api/v1/destinations','/api/v1/fallback-assets','/api/v1/sessions')) {
                throw 'Фикстура получила неизвестный маршрут чтения.'
            }
            # payload — UTF-8 тело, prefix — ASCII заголовки с точным количеством байт.
            $payload = [Text.Encoding]::UTF8.GetBytes($response)
            $prefix = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 $status`r`nContent-Type: $type`r`nContent-Length: $($payload.Length)`r`nConnection: close`r`n`r`n")
            $stream.Write($prefix, 0, $prefix.Length)
            $stream.Write($payload, 0, $payload.Length)
        } finally { if ($client) { $client.Dispose() } }
    }
}

# root/executable/binary/sourceBinary/helper/resultDirectory — проверенные workspace-пути входов и результатов.
$root = Assert-WorkspacePath $ObsRoot
$executable = Assert-WorkspacePath (Join-Path $root 'bin/64bit/obs64.exe')
$binary = Assert-WorkspacePath (Join-Path $root 'plugins/gost-restreamer-obs-plugin/gost-restreamer-obs-plugin.dll')
$sourceBinary = Assert-WorkspacePath $ExpectedBinary
$helper = Assert-WorkspacePath (Join-Path $PSScriptRoot 'Test-Dev8ProfileUi.ps1')
$resultDirectory = Assert-WorkspacePath $ResultRoot
if (@(Get-ChildItem -LiteralPath $resultDirectory -Force).Count) { throw 'Каталог результатов должен быть пустым.' }
# inputHashes фиксирует все исполняемые входы helper/runner/DLL для повторной проверки после завершения.
$inputHashes = @{}
# inputPath последовательно фиксирует каждый выбранный вход, не меняя его.
foreach ($inputPath in @($binary, $sourceBinary, $helper, $PSCommandPath)) { $inputHashes[$inputPath] = (Get-FileHash -LiteralPath $inputPath).Hash }
if ($inputHashes[$binary] -ne $ExpectedBinarySha256 -or $inputHashes[$sourceBinary] -ne $ExpectedBinarySha256) { throw 'Выбранная DLL не совпала с ожидаемым SHA256.' }
if ([Diagnostics.FileVersionInfo]::GetVersionInfo($executable).ProductVersion -notmatch '^33\.0\.1(?:\D|$)') { throw 'Требуется OBS 33.0.1.' }
if (Get-ChildItem -LiteralPath $root -Recurse -Force | Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 }) { throw 'Portable-копия не должна содержать ссылки.' }
if (Get-Process -Name obs64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -ceq $executable }) { throw 'Эта тестовая копия OBS уже запущена.' }
# config/profileIni/userIni ограничивают подготовку собственной конфигурацией после базового runner.
$config = Assert-WorkspacePath (Join-Path $root 'config/obs-studio')
$profileIni = Assert-WorkspacePath (Join-Path $config 'basic/profiles/Isolated/basic.ini')
$userIni = Assert-WorkspacePath (Join-Path $config 'user.ini')
if ((Get-Content -LiteralPath $profileIni -Raw) -notmatch '(?m)^Name=Isolated\s*$' -or
    (Get-Content -LiteralPath $userIni -Raw) -notmatch ('(?m)^Language=' + [regex]::Escape($Locale) + '\s*$')) { throw 'Ожидались собственные Isolated и указанная локаль.' }
[ProfileFixtureWindows]::AssertDesktop()
# result фиксирует начало до запуска дочерних процессов; errorText никогда не содержит тела fixture-запроса.
$result = [ordered]@{ Check='ProfileNative'; Passed=$false; StartedUtc=[DateTime]::UtcNow.ToString('o'); EndedUtc=$null; SnapshotId=$SnapshotId; Locale=$Locale; LogMode=$LogMode; Error=$null; Cleanup=$null; Ui=$null; InputHashes=$inputHashes }
$errorText = $null
# listener слушает только loopback на свободном порту; requests/sseClients живут только в этом процессе.
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$requests = [Collections.Generic.List[string]]::new()
$sseClients = [Collections.Generic.List[Net.Sockets.TcpClient]]::new()
# nextHeartbeat ограничивает частоту комментариев открытой SSE, не имитируя доменные события.
$script:nextHeartbeat=[DateTime]::UtcNow
# obs/ui — собственные дочерние процессы, window/startTicks — их проверяемая OBS-идентичность.
$obs = $null
$ui = $null
$window = [IntPtr]::Zero
$startTicks = 0L
# logPath указывает только на новый журнал текущего запуска; uiOutput/uiError читаются асинхронно.
$logPath = $null
$uiOutput = $null
$uiError = $null
try {
    $listener.Start()
    # origin публикует случайный loopback-порт только в тестовой конфигурации и среде helper.
    $origin = 'http://127.0.0.1:' + $listener.LocalEndpoint.Port
    # settingsDirectory — собственный каталог плагина; parentPath проверяется до создания вложенного пути.
    $parentPath = Assert-WorkspacePath $config
    $settingsDirectory = Join-Path $parentPath 'plugin_config/gost-restreamer-obs-plugin'
    $null = New-Item -ItemType Directory -Path $settingsDirectory -Force
    $settingsDirectory = Assert-WorkspacePath $settingsDirectory
    # preferences — синтетическая схема v3 без серверных секретов или заимствованных профилей.
    $preferences = @{ schema_version=3; server_origin=$origin; auto_connect=$true; reconnect=$true; sse_rejection_retries=0; http_timeout_seconds=15; log_level=$LogMode; profiles=@(); bindings=@() }
    [IO.File]::WriteAllText((Join-Path $settingsDirectory 'settings.json'), ($preferences | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
    # startInfo задаёт видимый собственный OBS и полностью workspace TEMP/TMP.
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $executable
    $startInfo.WorkingDirectory = Split-Path -Parent $executable
    $startInfo.Arguments = '--portable --multi --disable-updater --disable-missing-files-check --profile Isolated --collection Isolated'
    $startInfo.UseShellExecute = $false
    $startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Normal
    # name обозначает только переменные поиска посторонних Qt/OBS-плагинов.
    foreach ($name in @('OBS_PLUGINS_PATH','OBS_LEGACY_PLUGINS_PATH','OBS_LEGACY_PLUGINS_DATA_PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM','QT_QPA_PLATFORM_PLUGIN_PATH')) { $null = $startInfo.Environment.Remove($name) }
    $startInfo.Environment['TEMP'] = $resultDirectory
    $startInfo.Environment['TMP'] = $resultDirectory
    # startedUtc/deadline ограничивают готовность новым журналом, а не одной задержкой.
    $startedUtc = [DateTime]::UtcNow
    $obs = [Diagnostics.Process]::Start($startInfo)
    $startTicks = $obs.StartTime.ToUniversalTime().Ticks
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    do {
        Receive-FixtureRequests
        if ($obs.HasExited) { throw 'OBS завершился до готовности.' }
        $window = [ProfileFixtureWindows]::MainWindow($obs.Id)
        # log выбирается только среди файлов текущего запуска.
        $log = Get-ChildItem -LiteralPath (Join-Path $config 'logs') -Filter '*.txt' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTimeUtc -ge $startedUtc } | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        if ($log) {
            $logPath = Assert-WorkspacePath $log.FullName
            # logText проверяет portable, модуль и готовность именно текущего процесса.
            $logText = Get-Content -LiteralPath $logPath -Raw -Encoding utf8
            if ($window -ne [IntPtr]::Zero -and $logText -match 'Portable mode: true' -and $logText -match 'GOST Restreamer загружен' -and $logText -match '==== Startup complete') { break }
        }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($window -eq [IntPtr]::Zero -or -not $logPath -or $logText -notmatch '==== Startup complete') { throw 'Не подтверждена готовность нового запуска.' }
    # owned освобождает временный Process после сверки desktop и DPI главного окна.
    $owned = Get-OwnedObs -RequireWindow
    try {
        # obsOwner/obsThread повторно связывают desktop с текущим главным HWND.
        $obsOwner=[uint32]0
        $obsThread=[ProfileFixtureWindows]::GetWindowThreadProcessId($window,[ref]$obsOwner)
        if ($obsOwner -ne $obs.Id -or [ProfileFixtureWindows]::DesktopName([ProfileFixtureWindows]::GetThreadDesktop($obsThread)) -cne 'Default' -or [ProfileFixtureWindows]::GetDpiForWindow($window) -ne 96) { throw 'OBS требует Default desktop и масштаб 100%.' }
    } finally { $owned.Dispose() }
    # command передаёт helper только явные идентификаторы и workspace-пути через среду процесса.
    $command = @'
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
& ([scriptblock]::Create([IO.File]::ReadAllText($env:PROFILE_UI_SCRIPT,[Text.Encoding]::UTF8))) -ObsProcessId ([int]$env:PROFILE_UI_PID) -ExpectedExecutable $env:PROFILE_UI_EXE -ExpectedStartTicks ([long]$env:PROFILE_UI_START) -MainWindowHandle ([long]$env:PROFILE_UI_WINDOW) -ExpectedPluginPath $env:PROFILE_UI_PLUGIN -ExpectedPluginSha256 $env:PROFILE_UI_HASH -Locale $env:PROFILE_UI_LOCALE -FixtureOrigin $env:PROFILE_UI_ORIGIN -ResultRoot $env:PROFILE_UI_RESULTS
'@
    # uiStart запускает UIA в MTA Windows PowerShell 5.1 без видимого терминала.
    $uiStart = [Diagnostics.ProcessStartInfo]::new()
    $uiStart.FileName = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $uiStart.Arguments = '-NoLogo -NoProfile -NonInteractive -Mta -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $uiStart.UseShellExecute = $false
    $uiStart.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $uiStart.CreateNoWindow = $true
    $uiStart.RedirectStandardOutput = $true
    $uiStart.RedirectStandardError = $true
    $uiStart.StandardOutputEncoding = [Text.Encoding]::UTF8
    $uiStart.StandardErrorEncoding = [Text.Encoding]::UTF8
    $uiStart.WorkingDirectory = $root
    # entry переносит уже изолированную среду OBS в helper.
    foreach ($entry in $startInfo.Environment.GetEnumerator()) { $uiStart.Environment[$entry.Key] = $entry.Value }
    $uiStart.Environment['PROFILE_UI_SCRIPT']=$helper
    $uiStart.Environment['PROFILE_UI_PID']=$obs.Id.ToString()
    $uiStart.Environment['PROFILE_UI_EXE']=$executable
    $uiStart.Environment['PROFILE_UI_START']=$startTicks.ToString()
    $uiStart.Environment['PROFILE_UI_WINDOW']=$window.ToInt64().ToString()
    $uiStart.Environment['PROFILE_UI_PLUGIN']=$binary
    $uiStart.Environment['PROFILE_UI_HASH']=$ExpectedBinarySha256
    $uiStart.Environment['PROFILE_UI_LOCALE']=$Locale
    $uiStart.Environment['PROFILE_UI_ORIGIN']=$origin
    $uiStart.Environment['PROFILE_UI_RESULTS']=$resultDirectory
    $ui = [Diagnostics.Process]::Start($uiStart)
    $uiOutput = $ui.StandardOutput.ReadToEndAsync()
    $uiError = $ui.StandardError.ReadToEndAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds(150)
    while (-not $ui.HasExited -and [DateTime]::UtcNow -lt $deadline) { Receive-FixtureRequests; Start-Sleep -Milliseconds 20 }
    if (-not $ui.HasExited) { throw 'Превышен бюджет UIA-проверки.' }
    [IO.File]::WriteAllText((Join-Path $resultDirectory 'ui-output.txt'), $uiOutput.GetAwaiter().GetResult(), [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $resultDirectory 'ui-error.txt'), $uiError.GetAwaiter().GetResult(), [Text.UTF8Encoding]::new($false))
    # uiResult читается из отдельного JSON, поэтому сообщение в stdout и exit 0 не подменяют приёмку.
    $uiResult = Get-Content -LiteralPath (Join-Path $resultDirectory 'ui-result.json') -Raw -Encoding utf8 | ConvertFrom-Json
    $result.Ui = $uiResult
    if ($ui.ExitCode -ne 0 -or -not $uiResult.Passed -or $uiResult.Error -or $uiResult.ProcessId -ne $obs.Id -or $uiResult.Locale -cne $Locale -or $requests.Count -ne 2) { throw 'Помощник не подтвердил все нативные сценарии.' }
    $owned = Get-OwnedObs -RequireWindow
    try { if (-not [ProfileFixtureWindows]::PostMessageW($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)) { throw 'Не отправлено штатное закрытие OBS.' } } finally { $owned.Dispose() }
    if (-not $obs.WaitForExit(20000) -or $obs.ExitCode -ne 0) { throw 'OBS не завершился штатно.' }
    $logText = Get-Content -LiteralPath $logPath -Raw -Encoding utf8
    if ($logText -notmatch '==== Shutting down' -or $logText -match '==== (Streaming|Recording|Replay Buffer|Virtual Camera) Start') { throw 'Нарушена граница пустой portable-проверки.' }
    # index различает два явных отказа: каждый должен иметь одну запись события и собственную диагностику.
    foreach ($index in 1..2) {
        if ([regex]::Matches($logText, [regex]::Escape("literal rejection $index")).Count -ne 2 -or
            [regex]::Matches($logText, [regex]::Escape("fixture-secret-$index")).Count -ne 1) { throw 'Не подтверждена однократная полная диагностика отказа в журнале OBS.' }
    }
    if ([regex]::Matches($logText, 'Отказ операции: profile_locked').Count -ne 2) { throw 'Неверное число записей явного отказа.' }
    # inputPath повторно сверяет все исходные исполняемые входы после проверки.
    foreach ($inputPath in $inputHashes.Keys) { if ((Get-FileHash -LiteralPath $inputPath).Hash -cne $inputHashes[$inputPath]) { throw 'Вход проверки изменился во время запуска.' } }
    $result.Passed = $true
} catch {
    # errorText содержит текст первого отказа runner; $_ не включает синтетические тела запросов в отчёт.
    $errorText = $_.Exception.Message
    $result.Error = $errorText
} finally {
    if ($ui -and -not $ui.HasExited) { $ui.Kill($true); $null=$ui.WaitForExit(3000); $result.Passed=$false }
    if ($obs -and -not $obs.HasExited) {
        try {
            # owned подтверждает процесс независимо от того, успел ли OBS создать главное окно.
            $owned = Get-OwnedObs
            try {
                # cleanupWindow — текущее главное окно; cleanupOwner проверяет его PID перед WM_CLOSE.
                $cleanupWindow=[ProfileFixtureWindows]::MainWindow($obs.Id)
                $cleanupOwner=[uint32]0
                if ($cleanupWindow -ne [IntPtr]::Zero) {
                    $null=[ProfileFixtureWindows]::GetWindowThreadProcessId($cleanupWindow,[ref]$cleanupOwner)
                    if ($cleanupOwner -eq $obs.Id) { $null=[ProfileFixtureWindows]::PostMessageW($cleanupWindow,0x10,[IntPtr]::Zero,[IntPtr]::Zero) }
                }
            } finally { $owned.Dispose() }
            if (-not $obs.WaitForExit(5000)) {
                # Повторная проверка процесса перед Kill не требует отсутствующего или уже закрытого HWND.
                $owned=Get-OwnedObs
                try { $owned.Kill() } finally { $owned.Dispose() }
                $result.Cleanup='Forced'
                $result.Passed=$false
                if (-not $obs.WaitForExit(3000)) { throw 'Не подтверждено завершение принудительно очищенного OBS.' }
            }
        } catch { $result.Cleanup='OwnershipOrShutdownFailure'; $result.Passed=$false }
    }
    if (-not $result.Cleanup) { $result.Cleanup='Graceful' }
    # client освобождает только SSE-соединения этой loopback-фикстуры.
    foreach ($client in $sseClients) { $client.Dispose() }
    $listener.Stop()
    if ($ui) { $ui.Dispose() }
    if ($obs) { $obs.Dispose() }
    $result.EndedUtc=[DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText((Join-Path $resultDirectory 'native-result.json'), ($result | ConvertTo-Json -Depth 15), [Text.UTF8Encoding]::new($false))
}
if (-not $result.Passed) { throw ('Нативная проверка не пройдена: ' + $errorText) }
$result | ConvertTo-Json -Depth 15 -Compress
