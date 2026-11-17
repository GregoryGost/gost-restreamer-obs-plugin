#Requires -Version 5.1
<#
.SYNOPSIS
Проверяет профили и единое окно отказа через UIA собственного OBS на Default desktop.
.DESCRIPTION
Вызывается только Test-Dev8ProfileNative.ps1 скрытым Windows PowerShell 5.1 MTA.
Параметры задают PID, точный EXE, start ticks, главный HWND и выбранную DLL.
Перед действиями повторно проверяются процесс, HWND, PID элемента, foreground и DPI 96.
Запросы /fixture/status обращаются только к переданному loopback-серверу; внешних сервисов нет.
.PARAMETER ObsProcessId
PID собственного OBS; ExpectedExecutable, ExpectedStartTicks и MainWindowHandle защищают от подмены.
.PARAMETER ExpectedPluginPath
Точный workspace-путь реально загруженной DLL с ExpectedPluginSha256.
.PARAMETER Locale
Локаль тестового OBS; обе локали проверяются независимыми запусками.
.PARAMETER FixtureOrigin
HTTP origin вида http://127.0.0.1:port, созданный запускающим runner.
.PARAMETER ResultRoot
Существующий workspace-каталог для ui-result.json и ограниченной диагностики.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int] $ObsProcessId,
    [Parameter(Mandatory)][string] $ExpectedExecutable,
    [Parameter(Mandatory)][long] $ExpectedStartTicks,
    [Parameter(Mandatory)][long] $MainWindowHandle,
    [Parameter(Mandatory)][string] $ExpectedPluginPath,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string] $ExpectedPluginSha256,
    [Parameter(Mandatory)][ValidateSet('ru-RU','en-US')][string] $Locale,
    [Parameter(Mandatory)][ValidatePattern('^http://127\.0\.0\.1:[0-9]+$')][string] $FixtureOrigin,
    [Parameter(Mandatory)][string] $ResultRoot
)

Set-StrictMode -Version Latest
# ErrorActionPreference завершает сценарий при первом неподтверждённом действии.
$ErrorActionPreference='Stop'
# helperDirectory восстанавливает путь исходника при запуске через UTF-8 scriptblock в EncodedCommand.
$helperDirectory=Split-Path -Parent $env:PROFILE_UI_SCRIPT
. (Join-Path $helperDirectory '../fixtures/WorkspacePaths.ps1')
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

# nativeSource содержит только адресные сообщения и проверки собственных окон/desktop.
$nativeSource=@'
using System;
using System.Text;
using System.Runtime.InteropServices;
// Проверяет UI-предусловия и отправляет клавиши только в проверенный HWND.
public static class ProfileUiWindows {
    // Возвращает PID и поток окна window.
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    // Читает имя класса window в text длиной count.
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr window, StringBuilder text, int count);
    // Возвращает корневое окно для window по flags.
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr window, uint flags);
    // Возвращает фактически активное окно.
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    // Запрашивает активацию window, которую вызывающий код проверяет отдельно.
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    // Возвращает физический DPI window.
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
    // Проверяет фактическую видимость window независимо от сохранённого объекта UIA.
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    // Передаёт message с параметрами wParam/lParam конкретному window.
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    // Возвращает desktop потока threadId.
    [DllImport("user32.dll")] public static extern IntPtr GetThreadDesktop(uint threadId);
    // Возвращает идентификатор текущего потока.
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    // Открывает активный desktop с flags/inherit/access.
    [DllImport("user32.dll")] private static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    // Освобождает открытый дескриптор desktop.
    [DllImport("user32.dll")] private static extern bool CloseDesktop(IntPtr desktop);
    // Читает имя объекта handle в text длиной length; needed получает требуемый размер.
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern bool GetUserObjectInformationW(IntPtr handle, int index, StringBuilder text, int length, out int needed);
    // Читает имя desktop и отклоняет недоступную среду.
    public static string DesktopName(IntPtr desktop) {
        // text хранит имя desktop, needed — требуемую длину Win32-буфера.
        var text=new StringBuilder(512);
        int needed;
        if (desktop==IntPtr.Zero || !GetUserObjectInformationW(desktop,2,text,1024,out needed))
            throw new InvalidOperationException("Не определён рабочий стол UIA.");
        return text.ToString();
    }
    // Требует Default у помощника и активного desktop; не переключает сеанс.
    public static void AssertDesktop() {
        // input содержит единственный открываемый этим методом дескриптор desktop.
        var input=OpenInputDesktop(0,false,1);
        try {
            if (DesktopName(GetThreadDesktop(GetCurrentThreadId()))!="Default" || DesktopName(input)!="Default")
                throw new InvalidOperationException("UIA требует активный Default desktop.");
        } finally { if(input!=IntPtr.Zero) CloseDesktop(input); }
    }
}
'@
Add-Type -TypeDefinition $nativeSource

# executable/plugin/results — существующие пути в workspace, используемые для идентичности и результатов.
$executable=Assert-WorkspacePath $ExpectedExecutable
$plugin=Assert-WorkspacePath $ExpectedPluginPath
$results=Assert-WorkspacePath $ResultRoot
# report сохраняет первый отказ и уже доказанные сценарии; checks заполняется только после их утверждений.
$report=[ordered]@{ Check='ProfileUi'; Passed=$false; StartedUtc=[DateTime]::UtcNow.ToString('o'); EndedUtc=$null; Error=$null; ProcessId=$ObsProcessId; Locale=$Locale; Dpi=0; Checks=@(); PopupHandle=0; Desktop=$null }
$checks=[Collections.Generic.List[string]]::new()
# processCondition ограничивает все корневые поиски собственным PID; allCondition используется в ограниченном дереве.
$processCondition=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ProcessIdProperty,$ObsProcessId)
$allCondition=[System.Windows.Automation.Condition]::TrueCondition
# main/management — заимствованные UIA-элементы; каждый повторно проверяется перед действием.
$main=$null
$management=$null

<# Возвращает SHA256 файла Path в шестнадцатеричном виде без загрузки PowerShell-модулей.
Поток и алгоритм освобождаются также при ошибке чтения; содержимое DLL не меняется. #>
function Get-FileSha256 {
    param([Parameter(Mandatory)][string] $Path)
    # hasher вычисляет SHA256 по потоку и освобождается после завершения чтения.
    $hasher=[Security.Cryptography.SHA256]::Create()
    try {
        # stream читает проверяемую DLL без загрузки всего файла в память.
        $stream=[IO.File]::OpenRead($Path)
        try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','') }
        finally { $stream.Dispose() }
    } finally { $hasher.Dispose() }
}

<# Проверяет текущий PID, EXE, start ticks и главный HWND без доверия к сохранённому Process. #>
function Assert-OwnedProcess {
    # owned временно открывает процесс, owner проверяет HWND, thread задаёт desktop OBS.
    $owned=[Diagnostics.Process]::GetProcessById($ObsProcessId)
    $owner=[uint32]0
    $thread=[ProfileUiWindows]::GetWindowThreadProcessId([IntPtr]::new($MainWindowHandle),[ref]$owner)
    try {
        if ($owned.HasExited -or $owned.MainModule.FileName -cne $executable -or $owned.StartTime.ToUniversalTime().Ticks -ne $ExpectedStartTicks -or $owner -ne $ObsProcessId) { throw 'Изменилась идентичность тестового OBS.' }
        if ([ProfileUiWindows]::DesktopName([ProfileUiWindows]::GetThreadDesktop($thread)) -cne 'Default') { throw 'OBS находится вне Default desktop.' }
        if ([ProfileUiWindows]::GetDpiForWindow([IntPtr]::new($MainWindowHandle)) -ne 96) { throw 'Проверка разрешена только при масштабе 100%.' }
    } finally { $owned.Dispose() }
}

<# Проверяет принадлежность Element текущему OBS; window задаёт проверяемый нативный HWND действия. #>
function Assert-OwnedElement {
    param([Parameter(Mandatory)] $Element)
    Assert-OwnedProcess
    if ($Element.Current.ProcessId -ne $ObsProcessId) { throw 'UIA-элемент принадлежит чужому процессу.' }
    # window/owner дополняют PID элемента проверкой его собственного HWND, если он существует.
    $window=[IntPtr]::new($Element.Current.NativeWindowHandle)
    $owner=[uint32]0
    if ($window -ne [IntPtr]::Zero) {
        $null=[ProfileUiWindows]::GetWindowThreadProcessId($window,[ref]$owner)
        if ($owner -ne $ObsProcessId) { throw 'Нативное окно UIA-элемента принадлежит чужому процессу.' }
    }
}

<# Ожидает Condition не дольше Seconds, проверяя идентичность; Failure сообщает исходный неподтверждённый факт. #>
function Wait-Observed {
    param([Parameter(Mandatory)][scriptblock] $Condition,[Parameter(Mandatory)][string] $Failure,[int] $Seconds=7)
    # deadline ограничивает наблюдение; ожидание само по себе не считается успехом.
    $deadline=[DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        Assert-OwnedProcess
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    throw $Failure
}

<# Возвращает собственные UIA-корни текущего desktop без поиска содержимого посторонних окон. #>
function Get-OwnedRoots {
    Assert-OwnedProcess
    return @([System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children,$processCondition))
}

<# Ищет единственный контрол с AutomationId Id в Root и его потомках.
Допускает точное совпадение либо квалифицированный Qt ID с границей «.» или «::» перед Id.
Без Root поиск ограничен корнями собственного процесса. Visible отбрасывает скрытые страницы;
совпадения всех форм проверяются вместе, поэтому точное имя не скрывает неоднозначный результат.
Type при необходимости отличает именованный виджет от виртуальных элементов списка с тем же ID. #>
function Find-Control {
    param([Parameter(Mandatory)][string] $Id,$Root=$management,[switch] $Visible,[System.Windows.Automation.ControlType] $Type=$null)
    # roots содержит область поиска, matches — найденные элементы, seen исключает повтор RuntimeId.
    $roots=if ($Root) { @($Root) } else { @(Get-OwnedRoots) }
    $matches=[Collections.Generic.List[object]]::new()
    $seen=@{}
    # root/candidate перебирают только собственное дерево; identity различает повторно найденные элементы.
    foreach ($root in $roots) {
        Assert-OwnedElement $root
        # candidates включает сам root: отдельное окно может предоставлять квалифицированный AutomationId.
        $candidates=@($root)+@($root.FindAll([System.Windows.Automation.TreeScope]::Descendants,$processCondition))
        foreach ($candidate in $candidates) {
            # candidateId — фактический ID; processCondition уже ограничил потомков собственным PID.
            $candidateId=$candidate.Current.AutomationId
            if ($candidateId -ceq $Id -or $candidateId.EndsWith('.'+$Id,[StringComparison]::Ordinal) -or $candidateId.EndsWith('::'+$Id,[StringComparison]::Ordinal)) {
                if ($Type -and $candidate.Current.ControlType -ne $Type) { continue }
                Assert-OwnedElement $candidate
                if ($Visible -and $candidate.Current.IsOffscreen) { continue }
                $identity=$candidate.GetRuntimeId() -join ':'
                if (-not $seen.ContainsKey($identity)) { $seen[$identity]=$true; $matches.Add($candidate) }
            }
        }
    }
    if ($matches.Count -ne 1) {
        # details содержит только идентификаторы и типы первых совпадений, без содержимого полей.
        $details=@()
        # entry обозначает собственный элемент, чьи метаданные объясняют неоднозначный поиск.
        foreach ($entry in @($matches | Select-Object -First 16)) {
            Assert-OwnedElement $entry
            $details += [pscustomobject]@{ Id=$entry.Current.AutomationId; Type=$entry.Current.ControlType.ProgrammaticName; Class=$entry.Current.ClassName }
        }
        # preview показывает структуру последнего собственного дерева, не читая значения полей.
        $preview=@()
        # entry описывает первые элементы дерева, когда искомое имя вообще не обнаружено.
        foreach ($entry in @($candidates | Select-Object -First 100)) {
            $preview += [pscustomobject]@{ Id=$entry.Current.AutomationId; Type=$entry.Current.ControlType.ProgrammaticName; Class=$entry.Current.ClassName; Offscreen=$entry.Current.IsOffscreen }
        }
        $report['LookupFailure']=[ordered]@{ RequestedId=$Id; ExpectedType=if ($Type) { $Type.ProgrammaticName } else { $null }; MatchCount=$matches.Count; Matches=$details; Preview=$preview }
        throw ('Не найден единственный контрол: '+$Id+'; совпадений: '+$matches.Count)
    }
    return $matches[0]
}

<# Проверяет HWND и DPI Top, запрашивает фокус только собственного окна и наблюдает реальный foreground. #>
function Focus-OwnedWindow {
    param([Parameter(Mandatory)] $Top)
    Assert-OwnedElement $Top
    # handle — HWND верхнего собственного окна; owner подтверждает PID повторно перед активацией.
    $handle=[IntPtr]::new($Top.Current.NativeWindowHandle)
    $owner=[uint32]0
    $null=[ProfileUiWindows]::GetWindowThreadProcessId($handle,[ref]$owner)
    if ($handle -eq [IntPtr]::Zero -or $owner -ne $ObsProcessId -or [ProfileUiWindows]::GetDpiForWindow($handle) -ne 96) { throw 'Не подтверждены собственный HWND и DPI окна действия.' }
    $Top.SetFocus()
    $null=[ProfileUiWindows]::SetForegroundWindow($handle)
    Wait-Observed { [ProfileUiWindows]::GetForegroundWindow() -eq $handle } 'Не подтверждён фактический foreground собственного окна.'
}

<# Требует собственный foreground и Element перед InvokePattern; Top не может быть чужим окном. #>
function Invoke-Control {
    param([Parameter(Mandatory)] $Element,$Top=$management)
    Focus-OwnedWindow $Top
    Assert-OwnedElement $Element
    if (-not $Element.Current.IsEnabled) { throw 'Недоступная кнопка не может быть действием теста.' }
    # pattern выбирает поддержанный action: checkable QToolButton может предоставлять только TogglePattern.
    $pattern=$null
    if ($Element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,[ref]$pattern)) { $pattern.Invoke() }
    elseif ($Element.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern,[ref]$pattern)) { $pattern.Toggle() }
    else { throw 'Контрол не предоставляет адресное действие UIA.' }
}

<# Посылает одну пару WM_KEYDOWN/UP Key только в проверенный foreground Top.
FocusedElement отдельно подтверждает фокус нужного контрола; глобальная клавиатура не используется. #>
function Send-OwnedKey {
    param([Parameter(Mandatory)][int] $Key,[Parameter(Mandatory)] $FocusedElement,$Top=$management)
    Assert-OwnedElement $Top
    Assert-OwnedElement $FocusedElement
    # handle/focused — текущие HWND и UIA-фокус; owner дополнительно проверяет цель сообщения.
    $handle=[IntPtr]::new($Top.Current.NativeWindowHandle)
    $focused=[System.Windows.Automation.AutomationElement]::FocusedElement
    $owner=[uint32]0
    $null=[ProfileUiWindows]::GetWindowThreadProcessId($handle,[ref]$owner)
    Assert-OwnedElement $focused
    if ($owner -ne $ObsProcessId -or [ProfileUiWindows]::GetForegroundWindow() -ne $handle -or -not [System.Windows.Automation.Automation]::Compare($focused,$FocusedElement)) {
        $report['KeyFocusFailure']=[ordered]@{ ExpectedId=$FocusedElement.Current.AutomationId; ExpectedType=$FocusedElement.Current.ControlType.ProgrammaticName; ActualId=$focused.Current.AutomationId; ActualType=$focused.Current.ControlType.ProgrammaticName; ExpectedWindow=$handle.ToInt64(); ForegroundWindow=[ProfileUiWindows]::GetForegroundWindow().ToInt64(); WindowOwner=$owner }
        throw 'Не подтверждены адресный HWND, foreground и фокус клавиши.'
    }
    if (-not [ProfileUiWindows]::PostMessageW($handle,0x100,[IntPtr]::new($Key),[IntPtr]::new(1)) -or -not [ProfileUiWindows]::PostMessageW($handle,0x101,[IntPtr]::new($Key),[IntPtr]::new(0xC0000001L))) { throw 'Не отправлена адресная клавиша.' }
}

<# Устанавливает фактический фокус Element внутри собственного Top.
После UIA SetFocus наблюдает результат; при необходимости использует адресный Tab только
между контролами того же окна. Недоступная цель или цикл обхода завершают проверку отказом. #>
function Focus-OwnedControl {
    param([Parameter(Mandatory)] $Element,$Top=$management)
    Focus-OwnedWindow $Top
    Assert-OwnedElement $Element
    $Element.SetFocus()
    # deadline ограничивает наблюдение отложенного UIA SetFocus; focused — текущий собственный фокус.
    $deadline=[DateTime]::UtcNow.AddMilliseconds(300)
    $focused=$null
    do {
        Assert-OwnedProcess
        $focused=[System.Windows.Automation.AutomationElement]::FocusedElement
        Assert-OwnedElement $focused
        if ([System.Windows.Automation.Automation]::Compare($focused,$Element)) { return }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $deadline)
    # seen обнаруживает полный цикл Tab; topId не позволяет перейти к чужому окну того же процесса.
    $seen=@{}
    $topId=$Top.Current.AutomationId
    # attempt ограничивает даже нестабильную цепочку фокуса, которая не повторяет RuntimeId.
    for ($attempt=0; $attempt -lt 160; $attempt++) {
        $focused=[System.Windows.Automation.AutomationElement]::FocusedElement
        Assert-OwnedElement $focused
        if ([System.Windows.Automation.Automation]::Compare($focused,$Element)) { return }
        if (-not $focused.Current.AutomationId.StartsWith($topId+'.',[StringComparison]::Ordinal)) { throw 'Фокус Tab вышел за пределы собственного окна.' }
        # identity отмечает посещённый контрол; previous удерживает точный фокус до отправки Tab.
        $identity=$focused.GetRuntimeId() -join ':'
        $previous=$focused
        if ($seen.ContainsKey($identity)) { throw 'Нужный контрол не достигнут в цикле Tab.' }
        $seen[$identity]=$true
        Send-OwnedKey 0x09 $previous $Top
        Wait-Observed { -not [System.Windows.Automation.Automation]::Compare([System.Windows.Automation.AutomationElement]::FocusedElement,$previous) } 'Адресный Tab не изменил фокус.'
    }
    throw 'Превышен предел переходов Tab к собственному контролу.'
}

<# Возвращает названия элементов выпадающего Combo; никаких значений в форму не добавляет.
Выпадающий список Qt допускается отдельным собственным корнем. Список закрывается через ExpandCollapsePattern.Collapse. #>
function Get-ComboOptions {
    param([Parameter(Mandatory)] $Combo)
    Focus-OwnedControl $Combo
    # comboId ограничивает поиск виртуальных пунктов логическим деревом этого ComboBox.
    $comboId=$Combo.Current.AutomationId
    # expand/listCondition/items задают открытый список этого Combo и его элементы.
    $expand=$Combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $listCondition=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::ListItem)
    $expand.Expand()
    Wait-Observed { $expand.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded } 'Список выбора не раскрылся.'
    # items сначала читаются из Combo; Qt может поместить popup отдельным корнем.
    $items=@($Combo.FindAll([System.Windows.Automation.TreeScope]::Descendants,$listCondition))
    if (-not $items.Count) {
        # root охватывает собственные окна: Qt может оставить popup внутри дерева окна управления.
        # seen исключает один и тот же виртуальный пункт, найденный через несколько собственных корней.
        $seen=@{}
        foreach ($root in @(Get-OwnedRoots)) {
            Assert-OwnedElement $root
            # candidateId/identity связывают пункт candidate с этим ComboBox и исключают повторы UIA.
            foreach ($candidate in @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants,$listCondition))) {
                $candidateId=$candidate.Current.AutomationId
                if ($candidateId -ceq $comboId -or $candidateId.StartsWith($comboId+'.',[StringComparison]::Ordinal) -or $candidateId.StartsWith($comboId+'::',[StringComparison]::Ordinal)) {
                    Assert-OwnedElement $candidate
                    $identity=$candidate.GetRuntimeId() -join ':'
                    if (-not $seen.ContainsKey($identity)) { $seen[$identity]=$true; $items+=@($candidate) }
                }
            }
        }
    }
    try {
        if (-not $items.Count) {
            # observed хранит только идентификаторы собственных пунктов для проверки размещения Qt popup.
            $observed=@()
            # root/candidate повторно перечисляют только пункты собственного процесса, без значений полей.
            foreach ($root in @(Get-OwnedRoots)) {
                Assert-OwnedElement $root
                foreach ($candidate in @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants,$listCondition) | Select-Object -First 32)) {
                    Assert-OwnedElement $candidate
                    $observed += $candidate.Current.AutomationId
                }
            }
            $report['ComboLookup']=[ordered]@{ ComboId=$comboId; ObservedItemIds=$observed }
            throw 'UIA не предоставляет элементы раскрытого списка.'
        }
        # item/nameList удерживают подписи и доступность каждого реального пункта до закрытия popup.
        $nameList=[Collections.Generic.List[object]]::new()
        foreach ($item in $items) { Assert-OwnedElement $item; $nameList.Add([pscustomobject]@{ Name=$item.Current.Name; Enabled=$item.Current.IsEnabled }) }
        return $nameList.ToArray()
    } finally {
        Assert-OwnedElement $Combo
        $expand.Collapse()
    }
}

<# Выбирает Name из конечного Combo через адресные клавиши, вызывая реальное пользовательское activated.
Home и Down не требуют глобальных модификаторов; после Home, Down и Enter проверяется итоговое UIA-значение. #>
function Select-Combo {
    param([Parameter(Mandatory)][string] $Id,[Parameter(Mandatory)][string] $Name)
    # combo/options/index определяют существующий доступный пункт; candidate — очередной пункт каталога.
    $combo=Find-Control $Id -Type ([System.Windows.Automation.ControlType]::ComboBox)
    $options=@(Get-ComboOptions $combo)
    $index=-1
    for ($candidate=0; $candidate -lt $options.Count; $candidate++) { if ($options[$candidate].Name -ceq $Name -and $options[$candidate].Enabled) { $index=$candidate; break } }
    if ($index -lt 0) { throw ('Нет доступного пункта '+$Name+' в '+$Id) }
    Focus-OwnedControl $combo
    Send-OwnedKey 0x24 $combo
    # step считает только доступные пункты до выбранного; disabled исходные значения клавиши пропускают.
    for ($step=0; $step -lt $index; $step++) {
        if ($options[$step+1].Enabled) { Send-OwnedKey 0x28 $combo }
    }
    Send-OwnedKey 0x0D $combo
    Wait-Observed { (Get-ControlValue $combo) -ceq $Name } ('Не подтверждён выбор '+$Name)
}

<# Возвращает значение Edit/Combo через UIA ValuePattern либо собственный SelectionPattern. #>
function Get-ControlValue {
    param([Parameter(Mandatory)] $Element)
    Assert-OwnedElement $Element
    # pattern получает поддержанный интерфейс; selection содержит единственный выбранный пункт списка.
    $pattern=$null
    if ($Element.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern,[ref]$pattern)) { return $pattern.Current.Value }
    if ($Element.TryGetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern,[ref]$pattern)) {
        $selection=@($pattern.GetCurrentSelection())
        if ($selection.Count -eq 1) { Assert-OwnedElement $selection[0]; return $selection[0].Current.Name }
    }
    throw 'UIA не предоставляет наблюдаемое значение поля.'
}

<# Заменяет ввод Text через ValuePattern, затем адресно добавляет и удаляет пробел.
Qt может отражать ValuePattern через setText без textEdited; настоящие WM_CHAR/Backspace
подтверждают пользовательское намерение, а окончательное значение обязано точно совпасть с Text. #>
function Set-Edit {
    param([Parameter(Mandatory)][string] $Id,[Parameter(Mandatory)][string] $Text)
    # edit — единственный диапазонный контрол текущей карточки.
    $edit=Find-Control $Id -Visible -Type ([System.Windows.Automation.ControlType]::Edit)
    Focus-OwnedControl $edit
    $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Text)
    Wait-Observed { (Get-ControlValue $edit) -ceq $Text } ('Не отражён ввод '+$Id)
    Send-OwnedKey 0x23 $edit
    Assert-OwnedElement $management
    Assert-OwnedElement $edit
    # handle/focused/owner повторно подтверждают адресную цель и фокус непосредственно перед WM_CHAR.
    $handle=[IntPtr]::new($management.Current.NativeWindowHandle)
    $focused=[System.Windows.Automation.AutomationElement]::FocusedElement
    $owner=[uint32]0
    $null=[ProfileUiWindows]::GetWindowThreadProcessId($handle,[ref]$owner)
    Assert-OwnedElement $focused
    if ($owner -ne $ObsProcessId -or [ProfileUiWindows]::GetForegroundWindow() -ne $handle -or -not [System.Windows.Automation.Automation]::Compare($focused,$edit)) { throw 'Не подтверждён фокус адресного текстового ввода.' }
    if (-not [ProfileUiWindows]::PostMessageW($handle,0x102,[IntPtr]::new(32),[IntPtr]::new(1))) { throw 'Не отправлен адресный текстовый ввод.' }
    Wait-Observed { (Get-ControlValue $edit) -ceq ($Text+' ') } ('Не подтверждён пользовательский ввод '+$Id)
    Send-OwnedKey 0x08 $edit
    Wait-Observed { (Get-ControlValue $edit) -ceq $Text } ('Не восстановлено точное значение '+$Id)
}

<# Читает только статус локальной RAM-фикстуры, запрещая proxy и внешние адреса. #>
function Get-FixtureStatus {
    # request ограничен своим loopback origin, тремя секундами и запретом proxy/redirect.
    $request=[Net.HttpWebRequest]::Create($FixtureOrigin+'/fixture/status')
    $request.Proxy=$null
    $request.Timeout=3000
    $request.ReadWriteTimeout=3000
    $request.AllowAutoRedirect=$false
    # response/reader освобождают только ответ текущего синтетического GET.
    $response=$request.GetResponse()
    try {
        $reader=[IO.StreamReader]::new($response.GetResponseStream(),[Text.Encoding]::UTF8)
        try { return ($reader.ReadToEnd() | ConvertFrom-Json) } finally { $reader.Dispose() }
    } finally { $response.Dispose() }
}

try {
    [ProfileUiWindows]::AssertDesktop()
    if ($PSVersionTable.PSVersion.Major -ne 5 -or [Threading.Thread]::CurrentThread.ApartmentState -ne 'MTA') { throw 'Требуется Windows PowerShell 5.1 MTA.' }
    Assert-OwnedProcess
    $report.Desktop='Default'
    $report.Dpi=[ProfileUiWindows]::GetDpiForWindow([IntPtr]::new($MainWindowHandle))
    $main=[System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]::new($MainWindowHandle))
    Assert-OwnedElement $main
    if ($main.Current.ClassName -cne 'OBSBasic') { throw 'Главный HWND не является OBSBasic.' }
    # owned/modules/module/moduleName проверяют реально загруженную DLL и Qt, не только файлы рядом.
    $owned=[Diagnostics.Process]::GetProcessById($ObsProcessId)
    try {
        $modules=@($owned.Modules)
        $module=@($modules | Where-Object { $_.ModuleName -ieq 'gost-restreamer-obs-plugin.dll' })
        if ($module.Count -ne 1 -or $module[0].FileName -cne $plugin -or (Get-FileSha256 $plugin) -ine $ExpectedPluginSha256) { throw 'Не подтверждён загруженный модуль выбранной сборки.' }
        foreach ($moduleName in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll')) {
            $module=@($modules | Where-Object { $_.ModuleName -ieq $moduleName })
            if ($module.Count -ne 1 -or -not $module[0].FileName.StartsWith((Split-Path -Parent $executable)+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or $module[0].FileVersionInfo.ProductVersion -notmatch '^6\.11\.1(?:\D|$)') { throw 'Не подтверждён Qt 6.11.1 собственной portable-копии.' }
        }
    } finally { $owned.Dispose() }
    Focus-OwnedWindow $main
    # menuType/toolsNames/tools ограничивают поиск штатным меню Сервис в своём OBS.
    $menuType=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::MenuItem)
    $toolsNames=if ($Locale -ceq 'ru-RU') { @('Сервис (T)','Сервис') } else { @('Tools') }
    $tools=@($main.FindAll([System.Windows.Automation.TreeScope]::Descendants,$menuType) | Where-Object { $_.Current.Name -cin $toolsNames -and -not $_.Current.IsOffscreen })
    if ($tools.Count -ne 1) { throw 'Не найдено единственное меню Сервис.' }
    Assert-OwnedElement $tools[0]
    $tools[0].GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    # action адресует единственный пункт GOST Restreamer внутри открытого собственного меню.
    $action=@($tools[0].FindAll([System.Windows.Automation.TreeScope]::Descendants,$menuType) | Where-Object { $_.Current.Name -ceq 'GOST Restreamer' })
    if ($action.Count -ne 1) { throw 'Не найден пункт управления плагином.' }
    Assert-OwnedElement $action[0]
    $action[0].GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Wait-Observed { try { $null=Find-Control 'managementWindow' -Root $null -Visible; return $true } catch { return $false } } 'Не открыто единственное окно управления.'
    $management=Find-Control 'managementWindow' -Root $null -Visible
    Wait-Observed { [ProfileUiWindows]::GetForegroundWindow().ToInt64() -eq $management.Current.NativeWindowHandle } 'Окно управления не активировано самим плагином.'
    # navigation/pageType/page выбирают раздел Profiles только внутри собственного списка навигации.
    $navigation=Find-Control 'managementNavigation' -Type ([System.Windows.Automation.ControlType]::List)
    $pageType=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::ListItem)
    # pageItems сохраняет фактический порядок разделов, targetRow — положение единственного раздела профилей.
    $pageItems=@($navigation.FindAll([System.Windows.Automation.TreeScope]::Descendants,$pageType))
    $page=@($pageItems | Where-Object { $_.Current.Name -ceq $(if ($Locale -ceq 'ru-RU') { 'Профили' } else { 'Profiles' }) })
    if ($page.Count -ne 1) { throw 'Не найден раздел профилей.' }
    $targetRow=-1
    # index сопоставляет найденный раздел с реальным порядком элементов списка.
    for ($index=0; $index -lt $pageItems.Count; $index++) {
        Assert-OwnedElement $pageItems[$index]
        if ([System.Windows.Automation.Automation]::Compare($pageItems[$index],$page[0])) { $targetRow=$index }
    }
    if ($targetRow -lt 0) { throw 'Не установлено положение раздела профилей.' }
    Focus-OwnedWindow $management
    Assert-OwnedElement $navigation
    $navigation.SetFocus()
    # Qt UIA Select меняет выделение, но не обязан менять currentRow, управляющий страницей.
    # Адресные Home/Down проходят обычную клавиатурную навигацию; step обозначает ожидаемую строку.
    for ($step=0; $step -le $targetRow; $step++) {
        # focused может быть самим списком или виртуальным ListItem с ID этого же списка.
        $focused=[System.Windows.Automation.AutomationElement]::FocusedElement
        Assert-OwnedElement $focused
        if ($focused.Current.AutomationId -cne $navigation.Current.AutomationId -or $focused.Current.ControlType -notin @([System.Windows.Automation.ControlType]::List,[System.Windows.Automation.ControlType]::ListItem)) { throw 'Фокус не принадлежит собственному списку навигации.' }
        # key выбирает первую строку либо следующую, selection наблюдает результат до следующей клавиши.
        $key=if ($step -eq 0) { 0x24 } else { 0x28 }
        Send-OwnedKey $key $focused
        $selection=$pageItems[$step].GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
        Wait-Observed { $selection.Current.IsSelected } 'Клавиатурная навигация не выбрала ожидаемый раздел.'
    }
    # createName/buttonCondition/create находит видимую кнопку создания только текущей страницы.
    $createName=if ($Locale -ceq 'ru-RU') { 'Создать' } else { 'Create' }
    $buttonCondition=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::Button)
    $create=@($management.FindAll([System.Windows.Automation.TreeScope]::Descendants,$buttonCondition) | Where-Object { $_.Current.Name -ceq $createName -and -not $_.Current.IsOffscreen })
    if ($create.Count -ne 1) { throw 'Не найдена видимая кнопка создания профиля.' }
    Invoke-Control $create[0]
    Wait-Observed { try { $null=Find-Control 'profilePreset' -Type ([System.Windows.Automation.ControlType]::ComboBox); return $true } catch { return $false } } 'Не появилась форма нового профиля после создания.'
    # preset/options — реальный раскрытый селектор; нулевой пункт является ручным вводом, остальные строго три пресета.
    $preset=Find-Control 'profilePreset' -Type ([System.Windows.Automation.ControlType]::ComboBox)
    $options=@(Get-ComboOptions $preset)
    if ($options.Count -ne 4 -or ($options[1..3].Name -join '|') -cne 'H.264 1080p60|H.264 1440p60|AV1 1440p60') { throw 'Каталог содержит не ровно три утверждённых пресета.' }
    $checks.Add('ExactlyThreePresets')
    Select-Combo 'profilePreset' 'H.264 1080p60'
    if ((Get-ControlValue (Find-Control 'video.resolution' -Type ([System.Windows.Automation.ControlType]::ComboBox))) -cne '1920 × 1080' -or (Get-ControlValue (Find-Control 'video.target' -Type ([System.Windows.Automation.ControlType]::Edit))) -cne '6000') { throw 'Неверные параметры пресета 1080p60.' }
    Select-Combo 'profilePreset' 'AV1 1440p60'
    if ((Get-ControlValue (Find-Control 'video.target' -Type ([System.Windows.Automation.ControlType]::Edit))) -cne '10000') { throw 'Неверный битрейт пресета AV1.' }
    # audioOptions проверяет зависимый каталог copy-видео, где audio transcode запрещён.
    $audioOptions=@(Get-ComboOptions (Find-Control 'live_audio_mode' -Type ([System.Windows.Automation.ControlType]::ComboBox)) | Where-Object Enabled)
    if ($audioOptions.Count -ne 1 -or $audioOptions[0].Name -cne $(if ($Locale -ceq 'ru-RU') { 'Копировать звук' } else { 'Copy audio' })) { throw 'Copy-видео допускает недопустимый LIVE audio.' }
    Select-Combo 'profilePreset' 'H.264 1440p60'
    if ((Get-ControlValue (Find-Control 'video.target' -Type ([System.Windows.Automation.ControlType]::Edit))) -cne '12000') { throw 'Неверный битрейт пресета 1440p60.' }
    $audioOptions=@(Get-ComboOptions (Find-Control 'live_audio_mode' -Type ([System.Windows.Automation.ControlType]::ComboBox)) | Where-Object Enabled)
    if ($audioOptions.Count -ne 2) { throw 'Transcode-видео не обновило зависимые варианты LIVE audio.' }
    $checks.Add('PresetsAndDependentLiveAudio')
    # advanced — реальный checkable QToolButton; раскрытие делает дополнительные поля доступными UIA.
    $advanced=Find-Control 'profileAdvanced' -Type ([System.Windows.Automation.ControlType]::CheckBox)
    Invoke-Control $advanced
    Wait-Observed { $advanced.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On } 'Расширенные настройки не раскрылись.'
    # finiteIds перечисляет конечные контрактные поля, включая числовые селекторы и отдельные custom options.
    $finiteIds=@('mode','live_audio_mode','video.codec','video.codec_profile','video.level','video.tier','video.pixel_format','video.bit_depth','video.color.range','video.color.primaries','video.color.transfer','video.color.matrix','audio.codec','audio.codec_profile','audio.sample_rate','audio.channels','audio.channel_layout','video.b_frames','video.closed_gop','encoding.video_encoder','encoding.audio_encoder','encoding.rate_control','encoding.rescale','nvenc.preset','nvenc.tuning','nvenc.multipass','nvenc.lookahead','nvenc.spatial','nvenc.temporal','nvenc.strength','svt.preset','svt.tuning','svt.aq','svt.lookahead','custom.nonref_p','custom.strict_gop','custom.lp','custom.aac_coder')
    # id/control/editCondition проверяют закрытый тип и отсутствие дочернего свободного Edit даже в свёрнутом разделе.
    foreach ($id in $finiteIds) {
        $control=Find-Control $id -Type ([System.Windows.Automation.ControlType]::ComboBox)
        $editCondition=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::Edit)
        if ($control.Current.ControlType -ne [System.Windows.Automation.ControlType]::ComboBox -or $control.FindAll([System.Windows.Automation.TreeScope]::Descendants,$editCondition).Count -ne 0) { throw ('Конечное поле допускает свободный ввод: '+$id) }
    }
    $checks.Add('FiniteSelectorsAreNotEditable')
    Invoke-Control $advanced
    Wait-Observed { $advanced.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq [System.Windows.Automation.ToggleState]::Off } 'Расширенные настройки не свернулись.'
    Set-Edit 'configurationName' 'Native profile'
    Set-Edit 'video.target' 'unfinished'
    # save — кнопка текущей видимой карточки, mutations должен остаться нулём при локально неверном вводе.
    $save=Find-Control 'configurationSave' -Visible -Type ([System.Windows.Automation.ControlType]::Button)
    Wait-Observed { -not $save.Current.IsEnabled } 'Неверный диапазонный ввод не блокирует сохранение.'
    if ((Get-FixtureStatus).mutations -ne 0) { throw 'Неверный профиль ушёл на сервер.' }
    $checks.Add('InvalidDraftCannotBeSent')
    # vbrName использует публичную локализованную подпись из проверенного ресурса, не техническое itemData.
    $vbrName=if ($Locale -ceq 'ru-RU') { 'Переменный битрейт (VBR)' } else { 'Variable bitrate (VBR)' }
    # Смена rate control требует видимого расширенного раздела; проверка имени идёт по фактической локализации.
    Invoke-Control $advanced
    Wait-Observed { $advanced.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On } 'Не раскрыты настройки режима битрейта.'
    Select-Combo 'encoding.rate_control' $vbrName
    Set-Edit 'video.target' '11999.125'
    Invoke-Control $advanced
    Wait-Observed { $advanced.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -eq [System.Windows.Automation.ToggleState]::Off } 'Не восстановлен основной вид профиля.'
    $save=Find-Control 'configurationSave' -Visible -Type ([System.Windows.Automation.ControlType]::Button)
    Wait-Observed { $save.Current.IsEnabled } 'Исправленный полный профиль не разрешает сохранение.'
    # popupHandle сохраняет идентичность первого диалога; attempt обозначает новое явное нажатие Save.
    $popupHandle=0L
    foreach ($attempt in 1..2) {
        Invoke-Control $save
        Wait-Observed { (Get-FixtureStatus).mutations -eq $attempt } 'Не получен ровно один запрос явного сохранения.'
        Wait-Observed { try { $null=Find-Control 'operationErrorDialog' -Root $null -Visible; return $true } catch { return $false } } 'Не появилось окно явного отказа.'
        # popup/reason/actionText фиксируют именно PlainText содержимое и идентичность единственного окна.
        $popup=Find-Control 'operationErrorDialog' -Root $null -Visible
        $reason=Find-Control 'operationErrorReason' -Root $popup
        $actionText=Find-Control 'operationErrorAction' -Root $popup
        if ($reason.Current.Name -cne "<b>literal rejection $attempt</b>" -or $actionText.Current.Name -cne "<i>change profile $attempt</i>") { throw 'Причина или действие интерпретированы как HTML либо потеряны.' }
        if ($attempt -eq 1) { $popupHandle=$popup.Current.NativeWindowHandle }
        if ($popupHandle -eq 0 -or $popup.Current.NativeWindowHandle -ne $popupHandle) { throw 'Второй отказ создал новый диалог.' }
        # dock/rendered/entry исключают дублирование причины/action и raw диагностики в Dock и окне управления.
        $dock=Find-Control 'restreamDock' -Root $null
        $rendered=@($management.FindAll([System.Windows.Automation.TreeScope]::Descendants,$allCondition))
        $rendered+=@($dock.FindAll([System.Windows.Automation.TreeScope]::Descendants,$allCondition))
        foreach ($entry in $rendered) {
            Assert-OwnedElement $entry
            if ($entry.Current.Name -match 'literal rejection|change profile|fixture-secret-') { throw 'Серверный отказ продублирован в настройках или редакторе.' }
        }
        Wait-Observed { $save.Current.IsEnabled } 'Предметный 409 заблокировал новое явное сохранение как CAS-конфликт.'
    }
    $report.PopupHandle=$popupHandle
    # status содержит последний wire-запрос, независимо полученный HTTP-фикстурой.
    $status=Get-FixtureStatus
    # Тип проверяется до значения: PowerShell иначе принимает строку или true после неявного преобразования.
    if ($status.lastBody.profile.video_encoding.target_bitrate_bps -isnot [int] -and $status.lastBody.profile.video_encoding.target_bitrate_bps -isnot [long]) { throw 'Битрейт передан не целым числом JSON.' }
    if ($status.mutations -ne 2 -or $status.lastBody.name -cne 'Native profile' -or $status.lastBody.profile.video_encoding.target_bitrate_bps -ne 11999125 -or $status.lastBody.profile.encoding.rate_control -cne 'vbr' -or $status.lastBody.profile.live_audio_mode -cne 'copy' -or $status.lastBody.profile.encoding.PSObject.Properties.Name -contains 'version') { throw 'Явные правки не сохранились в полном числовом versionless запросе.' }
    $checks.Add('ExplicitNumericEditsReachWire')
    $checks.Add('NonCasRejectionReusesOnePlainTextDialog')
    # closeName/closeButtons закрывают только временный диалог; ManagementWindow продолжает отвечать.
    $closeName=if ($Locale -ceq 'ru-RU') { 'Закрыть' } else { 'Close' }
    $closeButtons=@($popup.FindAll([System.Windows.Automation.TreeScope]::Descendants,$buttonCondition) | Where-Object { $_.Current.Name -ceq $closeName })
    if ($closeButtons.Count -ne 1) { throw 'Не найдена кнопка закрытия временного диалога.' }
    Invoke-Control $closeButtons[0] $popup
    Wait-Observed { -not [ProfileUiWindows]::IsWindowVisible([IntPtr]::new($popupHandle)) } 'Временный диалог не закрывается асинхронно.'
    Focus-OwnedWindow $management
    $checks.Add('TransientDialogCloses')
    $report.Passed=$true
} catch {
    # $_ сохраняет первый диагностический отказ и место в сценарии без значений секретных полей UIA.
    $report.Error=$_.Exception.Message
    $report['ErrorLocation']=$_.ScriptStackTrace
} finally {
    $report.Checks=$checks.ToArray()
    $report.EndedUtc=[DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText((Join-Path $results 'ui-result.json'),($report | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
}
if (-not $report.Passed) { throw $report.Error }
$report | ConvertTo-Json -Depth 8 -Compress
