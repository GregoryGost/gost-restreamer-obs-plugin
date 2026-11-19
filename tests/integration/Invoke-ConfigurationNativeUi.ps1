#Requires -Version 5.1
<# Выполняет ограниченный список UIA-действий только в явно указанной portable OBS.
IdentityPath содержит PID/EXE/start-time/HWND и SHA256 DLL, PlanPath — действия,
ResultPath — отчёт внутри workspace. Вызывается скрытым Windows PowerShell 5.1 MTA.
Проверенные функции владения и адресного ввода импортируются из профильного UI runner;
его собственный сценарий и синтетический сервер не запускаются. #>
param([Parameter(Mandatory)][string] $IdentityPath,
      [Parameter(Mandatory)][string] $PlanPath,
      [Parameter(Mandatory)][string] $ResultPath)

# ErrorActionPreference останавливает действия после первого недоказанного условия.
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
# sourcePath адресует существующий профильный UIA runner; source и marker ограничивают импорт его определениями.
$sourcePath=Join-Path $env:CONFIGURATION_WORKSPACE 'tests/integration/Test-Dev8ProfileUi.ps1'
$source=[IO.File]::ReadAllText($sourcePath,[Text.Encoding]::UTF8)
$marker=$source.IndexOf("try {`r`n    [ProfileUiWindows]::AssertDesktop()",[StringComparison]::Ordinal)
if ($marker -lt 0) { $marker=$source.IndexOf("try {`n    [ProfileUiWindows]::AssertDesktop()",[StringComparison]::Ordinal) }
if ($marker -lt 0) { throw 'Не найдена граница определений UIA.' }
# identity задаёт единственную разрешённую цель; prefix не содержит действий старого сценария.
$identity=[IO.File]::ReadAllText($IdentityPath,[Text.Encoding]::UTF8) | ConvertFrom-Json
$prefix=$source.Substring(0,$marker)
# Быстрые смены профиля/origin подчиняются штатному пределу трёх подключений за10с;
# окно наблюдения15с покрывает эту документированную паузу, не заменяя её sleep-ом.
$prefix=$prefix.Replace('[int] $Seconds=7','[int] $Seconds=15')
$env:PROFILE_UI_SCRIPT=$sourcePath
$env:CONFIGURATION_PLAN=$PlanPath
$env:CONFIGURATION_RESULT=$ResultPath
# body выполняет только перечисленные действия и записывает первый отказ вместе с выполненными шагами.
$body=@'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
// Читает Unicode clipboard после явного Copy своего тестового окна, не требуя STA.
public static class ConfigurationClipboard {
    // Открывает clipboard для чтения без назначения нового владельца.
    [DllImport("user32.dll")] private static extern bool OpenClipboard(IntPtr owner);
    // Освобождает clipboard после завершения чтения.
    [DllImport("user32.dll")] private static extern bool CloseClipboard();
    // Удаляет прежнее содержимое для проверки фактического нового Copy.
    [DllImport("user32.dll")] private static extern bool EmptyClipboard();
    // Возвращает заимствованный дескриптор данных указанного format.
    [DllImport("user32.dll")] private static extern IntPtr GetClipboardData(uint format);
    // Отображает заимствованную память дескриптора memory.
    [DllImport("kernel32.dll")] private static extern IntPtr GlobalLock(IntPtr memory);
    // Освобождает отображение без удаления данных clipboard.
    [DllImport("kernel32.dll")] private static extern bool GlobalUnlock(IntPtr memory);
    // Возвращает строку CF_UNICODETEXT либо ошибку недоступного clipboard.
    public static string Read() {
        if (!OpenClipboard(IntPtr.Zero)) throw new InvalidOperationException("Clipboard занят.");
        try {
            // memory содержит заимствованные данные CF_UNICODETEXT; pointer ограничен парой Lock/Unlock.
            var memory=GetClipboardData(13);
            var pointer=GlobalLock(memory);
            if (pointer==IntPtr.Zero) throw new InvalidOperationException("Нет Unicode clipboard.");
            try { return Marshal.PtrToStringUni(pointer); }
            finally { GlobalUnlock(memory); }
        } finally { CloseClipboard(); }
    }
    // Удаляет прежнюю строку, чтобы старый clipboard не мог дать ложный успех следующему Copy.
    public static void Clear() {
        if (!OpenClipboard(IntPtr.Zero)) throw new InvalidOperationException("Clipboard занят.");
        try { if (!EmptyClipboard()) throw new InvalidOperationException("Clipboard не очищен."); }
        finally { CloseClipboard(); }
    }
}
// Переводит адрес выбранной строки в клиентские координаты только собственного HWND.
public static class ConfigurationPointer {
    // Point содержит координаты X (по горизонтали) и Y (по вертикали) одной проверенной точки.
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X; public int Y; }
    // Rectangle хранит клиентские края: Left/Top — начало, Right/Bottom — конец по соответствующим осям.
    [StructLayout(LayoutKind.Sequential)] public struct Rectangle { public int Left; public int Top; public int Right; public int Bottom; }
    // Переводит screen point в координаты клиента window.
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr window, ref Point point);
    // Возвращает физические клиентские границы собственного window.
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rectangle rectangle);
}
"@
# plan содержит доверенные действия текущего запуска; outcomes сохраняет фактические результаты шагов.
$plan=Get-Content -LiteralPath $env:CONFIGURATION_PLAN -Raw -Encoding UTF8 | ConvertFrom-Json
$outcomes=[Collections.Generic.List[object]]::new()
# MenuActions фиксирует закрытие собственного popup до единственного вызова QAction, без серверных тел.
$report['MenuActions']=@()

<# Нажимает центр видимого Element только адресными сообщениями своего Top.
Не использует системный курсор; HWND/PID/foreground и клиентские границы проверяются перед сообщением.
MenuPopup допускает foreground главного владельца для неактивируемого собственного QMenu.
CheckIndicator нажимает левую область флажка строки при проверенном DPI96;
состояние и точный адресат после сохранения проверяются независимым чтением backend. #>
function Click-AcceptanceElement {
    param($Element,$Top=$management,[switch]$MenuPopup,[switch]$CheckIndicator)
    if (-not $MenuPopup) { Focus-OwnedWindow $Top }
    Assert-OwnedElement $Top
    Assert-OwnedElement $Element
    # bounds/point/window определяют клиентскую точку конкретного собственного элемента.
    $bounds=$Element.Current.BoundingRectangle
    $point=[ConfigurationPointer+Point]::new()
    if ($CheckIndicator -and $bounds.Width -lt 20) { throw 'Строка слишком узкая для адресного флажка.' }
    $point.X=[int]($bounds.Left+$(if ($CheckIndicator) { 10 } else { $bounds.Width/2 }))
    $point.Y=[int]($bounds.Top+$bounds.Height/2)
    $window=[IntPtr]::new([long]$Top.Current.NativeWindowHandle)
    # client ограничивает точку реальным клиентом, даже если Qt UIA не отметил обрезанный элемент offscreen.
    $client=[ConfigurationPointer+Rectangle]::new()
    if ($Element.Current.IsOffscreen -or $bounds.Width -le 0 -or $bounds.Height -le 0 -or -not [ConfigurationPointer]::ScreenToClient($window,[ref]$point) -or -not [ConfigurationPointer]::GetClientRect($window,[ref]$client) -or $point.X -lt $client.Left -or $point.Y -lt $client.Top -or $point.X -ge $client.Right -or $point.Y -ge $client.Bottom) { throw 'Не подтверждена клиентская точка контрола.' }
    # coordinates кодирует точку для пары сообщений только этого HWND.
    $coordinates=[IntPtr]::new(($point.Y -shl 16) -bor $point.X)
    Assert-OwnedElement $Element
    if ([ProfileUiWindows]::GetForegroundWindow() -ne $window -and (-not $MenuPopup -or [ProfileUiWindows]::GetForegroundWindow().ToInt64() -ne $MainWindowHandle)) {
        $report['PointerFailure']=[ordered]@{target=$Element.Current.AutomationId;window=$window.ToInt64();foreground=[ProfileUiWindows]::GetForegroundWindow().ToInt64();bounds=$bounds.ToString()}
        throw 'Потерян foreground адресного нажатия.'
    }
    $null=[ProfileUiWindows]::PostMessageW($window,0x201,[IntPtr]::new(1),$coordinates)
    $null=[ProfileUiWindows]::PostMessageW($window,0x202,[IntPtr]::Zero,$coordinates)
}

<# Раскрывает конкретный Combo и возвращает его реальные пункты вместе с собственным popup.
Клавиши Enter не посылаются закрытому Combo, чтобы не активировать постороннюю кнопку формы. #>
function Open-AcceptanceCombo {
    param($Combo,$Top=$management)
    Focus-OwnedWindow $Top
    Assert-OwnedElement $Combo
    # expand/items/condition/popup связывают открытый список с его собственным контролом.
    $expand=$Combo.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $condition=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::ListItem)
    $expand.Expand()
    Wait-Observed { $expand.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded } 'Не раскрылся конечный селектор.'
    $items=@($Combo.FindAll([System.Windows.Automation.TreeScope]::Descendants,$condition))
    $popup=@(Get-OwnedRoots | Where-Object { $_.Current.ClassName -ceq 'QComboBoxPrivateContainer' -and -not $_.Current.IsOffscreen })
    if (-not $items.Count) {
        # root/item перебирают только собственные popup, prefix ограничивает виртуальные элементы этим Combo.
        $prefix=$Combo.Current.AutomationId
        foreach ($root in @(Get-OwnedRoots)) {
            foreach ($item in @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants,$condition))) {
                if ($item.Current.AutomationId -ceq $prefix -or $item.Current.AutomationId.StartsWith($prefix+'.',[StringComparison]::Ordinal) -or $item.Current.AutomationId.StartsWith($prefix+'::',[StringComparison]::Ordinal)) { Assert-OwnedElement $item; $items+=@($item) }
            }
        }
    }
    if (-not $items.Count -or $popup.Count -ne 1) { $expand.Collapse(); throw 'Не подтверждён единственный popup селектора.' }
    return [pscustomobject]@{items=$items;popup=$popup[0];expand=$expand}
}

<# Выбирает существующий Name точным адресным нажатием в popup Id и проверяет activated по значению. #>
function Select-AcceptanceCombo {
    param([string] $Id,[string] $Name,$Top=$management)
    # combo/opened/target связывают желаемую строку с единственным реально открытым собственным списком.
    $combo=Find-Control $Id -Root $(if ($Id -eq 'streamSelector') { $null } else { $Top }) -Type ([System.Windows.Automation.ControlType]::ComboBox)
    Wait-Observed {
        try {
            # probe проверяет появление пункта после асинхронного чтения каталога и сразу закрывает свой popup.
            $probe=Open-AcceptanceCombo $combo $Top
            try { return @($probe.items | Where-Object { $_.Current.Name -ceq $Name -and $_.Current.IsEnabled }).Count -eq 1 }
            finally { $probe.expand.Collapse() }
        } catch { return $false }
    } ('Не появился допустимый пункт '+$Name+' в '+$Id)
    $opened=Open-AcceptanceCombo $combo $Top
    $target=@($opened.items | Where-Object { $_.Current.Name -ceq $Name -and $_.Current.IsEnabled })
    if ($target.Count -ne 1) { $opened.expand.Collapse(); throw ('Нет единственного допустимого пункта '+$Name+' в '+$Id) }
    Click-AcceptanceElement $target[0] $opened.popup
    Wait-Observed { (Get-ControlValue $combo) -ceq $Name -and $opened.expand.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Collapsed } 'Не подтверждён реальный выбор селектора.'
}

<# Выполняет Action штатного Menu после подтверждённого закрытия его единственного popup.
Qt UIA Invoke вызывает QAction напрямую и не закрывает QMenu. Порядок Collapse→Invoke
повторяет обычный выбор пункта: меню теряет захват до действия и последующей активации окна.
Элементы и паттерн захватываются до Collapse; при их утрате действие не повторяется. #>
function Invoke-AcceptanceMenuAction {
    param($Menu,$Action)
    Assert-OwnedElement $Menu
    Assert-OwnedElement $Action
    if (-not $Action.Current.IsEnabled) { throw 'Пункт собственного меню недоступен.' }
    # expansion закрывает именно родительское меню; invocation сохраняет однократное действие QAction.
    $expansion=$Menu.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $invocation=$Action.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    # popupCondition и popups ограничивают ожидание единственным видимым QMenu внутри своего родителя.
    $popupCondition=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ClassNameProperty,'QMenu')
    $popups=@($Menu.FindAll([System.Windows.Automation.TreeScope]::Descendants,$popupCondition) | Where-Object {
        Assert-OwnedElement $_
        $_.Current.NativeWindowHandle -ne 0 -and [ProfileUiWindows]::IsWindowVisible([IntPtr]::new([long]$_.Current.NativeWindowHandle))
    })
    if ($popups.Count -ne 1) { throw 'Не подтверждён единственный popup собственного меню.' }
    # popupHandle остаётся известным после скрытия, когда UIA уже может исключить QMenu из видимого дерева.
    $popupHandle=[IntPtr]::new([long]$popups[0].Current.NativeWindowHandle)
    $expansion.Collapse()
    Wait-Observed { $expansion.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Collapsed -and -not [ProfileUiWindows]::IsWindowVisible($popupHandle) } 'Собственное меню не закрылось перед действием.'
    Assert-OwnedElement $Menu
    Assert-OwnedElement $Action
    if (-not $Action.Current.IsEnabled) { throw 'Пункт меню изменился до действия.' }
    $report['MenuActions']+=@([pscustomobject]@{menu=$Menu.Current.Name;action=$Action.Current.Name;popup=$popupHandle.ToInt64();collapsed=$true;nativeVisible=$false})
    $invocation.Invoke()
}

<# Показывает Dock через штатное меню своего главного OBS без изменения сервера. #>
function Show-AcceptanceDock {
    try { return (Find-Control 'restreamDock' -Root $null -Visible) } catch {}
    Focus-OwnedWindow $main
    # menuType/menus/action ограничивают поиск штатным меню панелей текущей локали и собственного PID.
    $menuType=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::MenuItem)
    $menus=@($main.FindAll([System.Windows.Automation.TreeScope]::Descendants,$menuType) | Where-Object { $_.Current.Name -in @('Док-панели (D)','Док-панели','Docks') -and -not $_.Current.IsOffscreen })
    if ($menus.Count -ne 1) { throw 'Не найдено единственное меню Dock.' }
    Assert-OwnedElement $menus[0]
    $menus[0].GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    # action появляется асинхронно после раскрытия своего меню.
    $action=$null
    Wait-Observed { @($menus[0].FindAll([System.Windows.Automation.TreeScope]::Descendants,$menuType) | Where-Object { $_.Current.Name -ceq 'GOST Restreamer' }).Count -eq 1 } 'Не появился пункт Dock.'
    $action=@($menus[0].FindAll([System.Windows.Automation.TreeScope]::Descendants,$menuType) | Where-Object { $_.Current.Name -ceq 'GOST Restreamer' })[0]
    Assert-OwnedElement $action
    Invoke-AcceptanceMenuAction $menus[0] $action
    Wait-Observed { try { $null=Find-Control 'restreamDock' -Root $null -Visible; return $true } catch { return $false } } 'Dock не показан.'
    return (Find-Control 'restreamDock' -Root $null -Visible)
}

<# Проверяет фактический фокус конкретного Edit перед каждым адресным событием ввода.
Виртуальные UIA-обёртки могут различаться RuntimeId, поэтому проверяются полный ID и тип Edit. #>
function Assert-AcceptanceInputFocus {
    param($Element)
    Assert-OwnedElement $Element
    # focused и window подтверждают конкретный ввод и его foreground, включая перехват своим диалогом.
    $focused=[System.Windows.Automation.AutomationElement]::FocusedElement
    $window=[IntPtr]::new([long]$management.Current.NativeWindowHandle)
    Assert-OwnedElement $focused
    if ($focused.Current.AutomationId -cne $Element.Current.AutomationId -or $focused.Current.ControlType -ne [System.Windows.Automation.ControlType]::Edit -or [ProfileUiWindows]::GetForegroundWindow() -ne $window) { throw 'Не подтверждён фокус точного текстового поля.' }
}

<# Передаёт одну клавишу Key только в foreground своего поля Element. #>
function Send-AcceptanceInputKey {
    param($Element,[int]$Key)
    Assert-AcceptanceInputFocus $Element
    # window содержит собственный верхний HWND, который Qt направляет уже проверенному полю фокуса.
    $window=[IntPtr]::new([long]$management.Current.NativeWindowHandle)
    $null=[ProfileUiWindows]::PostMessageW($window,0x100,[IntPtr]::new($Key),[IntPtr]::new(1))
    $null=[ProfileUiWindows]::PostMessageW($window,0x101,[IntPtr]::new($Key),[IntPtr]::new(0xC0000001L))
}

<# Читает безопасную историю текущего loopback proxy без редиректа и системного proxy. #>
function Get-AcceptanceProxyStatus {
    # request/response/reader ограничивают служебный GET тремя секундами и освобождаются после чтения.
    $request=[Net.HttpWebRequest]::Create($FixtureOrigin+'/__fixture/status')
    $request.Proxy=$null
    $request.AllowAutoRedirect=$false
    $request.Timeout=3000
    $response=$request.GetResponse()
    try {
        $reader=[IO.StreamReader]::new($response.GetResponseStream(),[Text.Encoding]::UTF8)
        try { return ($reader.ReadToEnd() | ConvertFrom-Json) } finally { $reader.Dispose() }
    } finally { $response.Dispose() }
}

<# Возвращает число реально принятых proxy записей, отличая отложенный Invoke от уже отправленного HTTP. #>
function Get-AcceptanceWriteCount {
    # status/property суммируют только счётчики мутаций, без чтения тел или секретов.
    $status=Get-AcceptanceProxyStatus
    # total содержит число отправленных запросов; property — один счётчик method/path.
    $total=0
    foreach ($property in $status.counts.PSObject.Properties) { if ($property.Name -match '^(POST|PATCH|PUT|DELETE) ') { $total += [int]$property.Value } }
    return $total
}

<# Находит один видимый контрол по Id либо Name в своём Root.
Index/Count явно разрешают повторённые поля endpoint/key, не принимая случайное первое совпадение. #>
function Find-AcceptanceControl {
    param($Spec,$Root=$management)
    # candidates ограничены собственным деревом, matches собирает подходящие видимые элементы.
    $candidates=if ($Root) { @($Root)+@($Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,$processCondition)) } else { @(Get-OwnedRoots) }
    $matches=@($candidates | Where-Object {
        Assert-OwnedElement $_
        (-not $_.Current.IsOffscreen) -and
        ((-not $Spec.id) -or $_.Current.AutomationId -ceq $Spec.id -or $_.Current.AutomationId.EndsWith('.'+$Spec.id,[StringComparison]::Ordinal) -or $_.Current.AutomationId.EndsWith('::'+$Spec.id,[StringComparison]::Ordinal)) -and
        ((-not $Spec.name) -or $_.Current.Name -ceq $Spec.name) -and
        ((-not $Spec.type) -or $_.Current.ControlType.ProgrammaticName -ceq ('ControlType.'+$Spec.type))
    })
    # count и index задают ожидаемую кратность и конкретную позицию в форме.
    $count=if ($null -ne $Spec.count) { [int]$Spec.count } else { 1 }
    $index=if ($null -ne $Spec.index) { [int]$Spec.index } else { 0 }
    if ($matches.Count -ne $count -or $index -lt 0 -or $index -ge $matches.Count) { throw ('Неоднозначный контрол '+$Spec.id+' / '+$Spec.name+': '+$matches.Count) }
    return $matches[$index]
}

<# Выбирает существующую строку List по Name адресным нажатием её реальной точки.
SelectionItem.Select отдельно недостаточен: Qt должен изменить currentRow и активировать страницу. #>
function Select-AcceptanceRow {
    param($List,[string] $Name,$Top=$management)
    # itemCondition/items/index задают полный порядок строк только этого списка.
    $itemCondition=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::ListItem)
    $items=@($List.FindAll([System.Windows.Automation.TreeScope]::Descendants,$itemCondition))
    $index=-1
    # position обозначает проверяемый индекс строки.
    for ($position=0; $position -lt $items.Count; $position++) { Assert-OwnedElement $items[$position]; if ($items[$position].Current.Name -ceq $Name) { $index=$position } }
    if ($index -lt 0) { throw ('В списке отсутствует '+$Name) }
    Assert-OwnedElement $List
    Click-AcceptanceElement $items[$index] $Top
    # selected подтверждает обработанное событие выбора конкретной строки.
    $selected=$items[$index].GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
    Wait-Observed { $selected.Current.IsSelected } 'Не подтверждён выбор строки.'
}

<# Открывает или активирует единственное окно управления через собственное меню OBS. #>
function Open-AcceptanceManagement {
    Focus-OwnedWindow $main
    # menuType/toolsNames/tools выбирают только штатное меню своего главного окна.
    $menuType=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::MenuItem)
    $toolsNames=if ($Locale -ceq 'ru-RU') { @('Сервис (T)','Сервис') } else { @('Tools') }
    $tools=@($main.FindAll([System.Windows.Automation.TreeScope]::Descendants,$menuType) | Where-Object { $_.Current.Name -cin $toolsNames -and -not $_.Current.IsOffscreen })
    if ($tools.Count -ne 1) { throw 'Не найдено собственное меню Сервис.' }
    Assert-OwnedElement $tools[0]
    $tools[0].GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    # action ищется только внутри открытого собственного меню.
    $action=@($tools[0].FindAll([System.Windows.Automation.TreeScope]::Descendants,$menuType) | Where-Object { $_.Current.Name -ceq 'GOST Restreamer' })
    if ($action.Count -ne 1) { throw 'Не найден единственный пункт плагина.' }
    Assert-OwnedElement $action[0]
    Invoke-AcceptanceMenuAction $tools[0] $action[0]
    Wait-Observed { try { $null=Find-Control 'managementWindow' -Root $null -Visible; return $true } catch { return $false } } 'Не открылось окно управления.'
    Wait-Observed { [ProfileUiWindows]::GetForegroundWindow().ToInt64() -eq (Find-Control 'managementWindow' -Root $null -Visible).Current.NativeWindowHandle } 'Меню не активировало окно управления.'
    return (Find-Control 'managementWindow' -Root $null -Visible)
}

<# Возвращает описание собственного дерева без значений Edit и без содержимого clipboard.
Для элементов с HWND nativeVisible отражает Win32-видимость независимо от Qt UIA IsOffscreen. #>
function Get-AcceptanceTree {
    # nodes собирает только структурные сведения; root/node перебирают окна и их собственных потомков.
    $nodes=[Collections.Generic.List[object]]::new()
    foreach ($root in @(Get-OwnedRoots)) {
        foreach ($node in @($root)+@($root.FindAll([System.Windows.Automation.TreeScope]::Descendants,$processCondition))) {
            Assert-OwnedElement $node
            if ($node.Current.IsOffscreen) { continue }
            # nativeHandle связывает Win32-видимость с этим собственным элементом, а не с текущим foreground.
            $nativeHandle=$node.Current.NativeWindowHandle
            $nodes.Add([pscustomobject]@{ id=$node.Current.AutomationId; name=if ($node.Current.ControlType -eq [System.Windows.Automation.ControlType]::Edit) { $null } else { $node.Current.Name }; type=$node.Current.ControlType.ProgrammaticName; class=$node.Current.ClassName; enabled=$node.Current.IsEnabled; password=$node.Current.IsPassword; hwnd=$nativeHandle; nativeVisible=if ($nativeHandle) { [ProfileUiWindows]::IsWindowVisible([IntPtr]::new([long]$nativeHandle)) } else { $null }; focused=$node.Current.HasKeyboardFocus; bounds=$node.Current.BoundingRectangle.ToString() })
        }
    }
    return $nodes.ToArray()
}

try {
    [ProfileUiWindows]::AssertDesktop()
    if ($PSVersionTable.PSVersion.Major -ne 5 -or [Threading.Thread]::CurrentThread.ApartmentState -ne 'MTA') { throw 'Требуется Windows PowerShell 5.1 MTA.' }
    Assert-OwnedProcess
    $main=[System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]::new($MainWindowHandle))
    Assert-OwnedElement $main
    if ($main.Current.ClassName -cne 'OBSBasic') { throw 'Неверный класс главного HWND.' }
    # loaded/modules/runtimeName сверяют реально загруженные DLL с выбранной сборкой.
    $loaded=[Diagnostics.Process]::GetProcessById($ObsProcessId)
    try {
        $modules=@($loaded.Modules | Where-Object { $_.ModuleName -ieq 'gost-restreamer-obs-plugin.dll' })
        if ($modules.Count -ne 1 -or $modules[0].FileName -cne $plugin -or (Get-FileSha256 $plugin) -ine $ExpectedPluginSha256) { throw 'Не подтверждена загруженная DLL.' }
        foreach ($runtimeName in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll')) {
            $modules=@($loaded.Modules | Where-Object { $_.ModuleName -ieq $runtimeName })
            if ($modules.Count -ne 1 -or (Split-Path -Parent $modules[0].FileName) -cne (Split-Path -Parent $executable) -or $modules[0].FileVersionInfo.ProductVersion -notmatch '^6\.11\.1(?:\D|$)') { throw 'Не подтверждён собственный Qt 6.11.1.' }
        }
    } finally { $loaded.Dispose() }
    try { $management=Find-Control 'managementWindow' -Root $null -Visible } catch { $management=$null }
    # step — следующее доверенное действие плана; observation хранит только доказанное значение шага.
    foreach ($step in $plan.steps) {
        # optionalName объявляет отсутствующие необязательные параметры как null для StrictMode.
        foreach ($optionalName in @('id','name','type','count','index','value','dialog','property','label','clearLength','sha256','scope','dialogAppear','parent')) {
            if (-not $step.PSObject.Properties[$optionalName]) { $step | Add-Member -NotePropertyName $optionalName -NotePropertyValue $null }
        }
        Assert-OwnedProcess
        $observation=$null
        switch ($step.action) {
            'open' { $management=Open-AcceptanceManagement; $observation=$management.Current.NativeWindowHandle }
            'dock' { $observation=(Show-AcceptanceDock).Current.AutomationId }
            'catalog' {
                # dock/list/items считывают все имена каталога, включая выключенные назначения до Session.
                $dock=Find-Control 'restreamDock' -Root $null -Visible
                $list=Find-Control 'outputList' -Root $dock -Type ([System.Windows.Automation.ControlType]::List)
                $items=@($list.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::ListItem)))
                if ($items.Count -ne @($step.value).Count) { throw 'Каталог содержит неверное число строк.' }
                # expected обозначает имя одной ожидаемой строки; match доказывает состояние и отсутствие измерений.
                foreach ($expected in $step.value) {
                    $match=@($items | Where-Object { $_.Current.Name.StartsWith($expected+' · ',[StringComparison]::Ordinal) })
                    # expectedPrefix утверждает именно состояние и битрейт, а не произвольное отсутствие другой метрики.
                    $expectedPrefix=if ($Locale -eq 'ru-RU') { $expected+" · Нет данных`nБитрейт: Нет данных · " } else { $expected+" · No data`nBitrate: No data · " }
                    if ($match.Count -ne 1 -or -not $match[0].Current.Name.StartsWith($expectedPrefix,[StringComparison]::Ordinal)) { throw 'Неверное имя или выдуманное состояние/измерение каталога.' }
                }
                $observation=@($items|ForEach-Object { Assert-OwnedElement $_; $_.Current.Name })
            }
            'dialog' {
                Wait-Observed { try { $null=Find-Control $step.id -Root $null -Visible; return $true } catch { return $false } } 'Не появилось ожидаемое окно.'
                $observation=(Find-Control $step.id -Root $null -Visible).Current.NativeWindowHandle
            }
            { $_ -in @('menu','menuOptions') } {
                Focus-OwnedWindow $main
                # menuType/parentMenus/items адресуют ровно одно меню собственного OBS и его реальные действия.
                $menuType=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::MenuItem)
                $parentMenus=@($main.FindAll([System.Windows.Automation.TreeScope]::Descendants,$menuType)|Where-Object {$_.Current.Name -ceq $step.parent -and -not $_.Current.IsOffscreen})
                if ($parentMenus.Count -ne 1) { throw 'Не найдено единственное главное меню.' }
                Assert-OwnedElement $parentMenus[0]
                $parentMenus[0].GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
                Wait-Observed { @($parentMenus[0].FindAll([System.Windows.Automation.TreeScope]::Descendants,$menuType)).Count -gt 0 } 'Не раскрылись действия главного меню.'
                $items=@($parentMenus[0].FindAll([System.Windows.Automation.TreeScope]::Descendants,$menuType))
                if ($step.action -eq 'menuOptions') {
                    $observation=@($items|ForEach-Object { Assert-OwnedElement $_; [pscustomobject]@{name=$_.Current.Name;enabled=$_.Current.IsEnabled} })
                    $parentMenus[0].GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse()
                } else {
                    # item фиксирует точное действие; адресное нажатие выполняется после проверки PID и доступности.
                    $item=@($items|Where-Object {$_.Current.Name -ceq $step.name})
                    if ($item.Count -ne 1 -or -not $item[0].Current.IsEnabled) { throw 'Не найдено единственное доступное действие меню.' }
                    Assert-OwnedElement $item[0]
                    # menuPopup задаёт нативный QMenu: адресное нажатие не держит UIA RPC внутри модального QAction.
                    $menuPopup=@($parentMenus[0].FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ClassNameProperty,'QMenu')) | Where-Object { $_.Current.NativeWindowHandle -ne 0 -and [ProfileUiWindows]::IsWindowVisible([IntPtr]::new([long]$_.Current.NativeWindowHandle)) })
                    if ($menuPopup.Count -ne 1) { throw 'Не подтверждён единственный нативный popup меню.' }
                    Click-AcceptanceElement $item[0] $menuPopup[0] -MenuPopup
                    if ($step.dialogAppear) { Wait-Observed { try { $null=Find-Control $step.dialogAppear -Root $null -Visible; return $true } catch { return $false } } 'Не появился собственный диалог OBS.' }
                }
            }
            'prompt' {
                # prompt/edit/confirm/promptWindow ограничивают замену имени единственным своим диалогом ввода.
                $prompt=Find-Control $step.dialog -Root $null -Visible
                $edit=Find-AcceptanceControl ([pscustomobject]@{id=$null;name=$null;type='Edit';count=$null;index=$null}) $prompt
                Focus-OwnedWindow $prompt
                Assert-OwnedElement $edit
                $edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue([string]$step.value)
                Wait-Observed { (Get-ControlValue $edit) -ceq [string]$step.value } 'Не отражено новое имя в диалоге OBS.'
                $confirm=Find-AcceptanceControl ([pscustomobject]@{id=$null;name=$step.name;type='Button';count=$null;index=$null}) $prompt
                $promptWindow=[IntPtr]::new([long]$prompt.Current.NativeWindowHandle)
                Invoke-Control $confirm $prompt
                Wait-Observed { -not [ProfileUiWindows]::IsWindowVisible($promptWindow) } 'Диалог имени не завершён.'
            }
            'read' {
                if ($step.id -notmatch '^((video|audio|encoding|nvenc|svt|custom)\.|mode$|live_audio_mode$|profile|streamSelector$)') { throw 'Чтение значения разрешено только несекретным полям профиля и рабочему выбору.' }
                $observation=Get-ControlValue (Find-AcceptanceControl $step $(if ($step.scope -eq 'main') { $main } else { $management }))
            }
            'page' {
                Select-AcceptanceRow (Find-Control 'managementNavigation' -Visible -Type ([System.Windows.Automation.ControlType]::List)) $step.name
                Wait-Observed { try { $null=Find-AcceptanceControl ([pscustomobject]@{id=$null;name=$step.name;type='Text';count=$null;index=$null}); return $true } catch { return $false } } 'Не показан заголовок выбранной страницы.'
                $observation=$step.name
            }
            'row' {
                Select-AcceptanceRow (Find-Control 'configurationList' -Visible -Type ([System.Windows.Automation.ControlType]::List)) $step.name
                Wait-Observed { try { return (Get-ControlValue (Find-Control 'configurationName' -Visible -Type ([System.Windows.Automation.ControlType]::Edit))) -ceq $step.name } catch { return $false } } 'Выбор строки не открыл точную карточку.'
                $observation=$step.name
            }
            'invoke' {
                # target и top задают контрол и его собственное окно; dialog разрешает временный диалог того же PID.
                $top=if ($step.dialog) { Find-Control $step.dialog -Root $null -Visible } else { $management }
                $target=Find-AcceptanceControl $step $top
                # dialogHandle сохраняет HWND до Invoke, пока Qt ещё не уничтожил временный QMessageBox.
                $dialogHandle=[IntPtr]::Zero
                if ($step.dialog) {
                    # rawDialogHandle читается только до действия и обязан быть ненулевым HWND своего диалога.
                    $rawDialogHandle=$top.Current.NativeWindowHandle
                    if ($null -eq $rawDialogHandle -or [long]$rawDialogHandle -eq 0) { throw 'Диалог не предоставляет живой HWND.' }
                    $dialogHandle=[IntPtr]::new([long]$rawDialogHandle)
                }
                # writesBefore связывает Save с наблюдаемой отправкой, а не с завершением UIA Invoke.
                $writesBefore=if ($step.id -eq 'configurationSave') { Get-AcceptanceWriteCount } else { 0 }
                Invoke-Control $target $top
                if ($step.dialogAppear) { Wait-Observed { try { $null=Find-Control $step.dialogAppear -Root $null -Visible; return $true } catch { return $false } } 'Не появилось ожидаемое подтверждение.' }
                if ($step.dialog -eq 'QMessageBox') { Wait-Observed { -not [ProfileUiWindows]::IsWindowVisible($dialogHandle) } 'Подтверждение не закрыто после ответа.' }
                if ($step.dialog -eq 'operationErrorDialog' -and $step.name -in @('Закрыть','Close')) { Wait-Observed { -not [ProfileUiWindows]::IsWindowVisible($dialogHandle) } 'Окно отказа не закрылось.' }
                if ($step.name -in @('Создать','Create') -and -not $step.dialogAppear) { Wait-Observed { (Get-ControlValue (Find-Control 'configurationName' -Visible -Type ([System.Windows.Automation.ControlType]::Edit))) -ceq '' } 'Create не открыл пустую новую карточку.' }
                if ($step.id -eq 'configurationSave') { Wait-Observed { (Get-AcceptanceWriteCount) -eq ($writesBefore+1) } 'Save не отправил ровно одну запись.'; $observation=@{writesBefore=$writesBefore;writesAfter=(Get-AcceptanceWriteCount)} }
                if ($step.id -eq 'settingsSaveButton') { Wait-Observed { -not $target.Current.IsEnabled } 'Явное сохранение адреса ещё не обработано.' }
            }
            'set' { Set-Edit $step.id ([string]$step.value) }
            'toggle' {
                # target/pattern/desired задают требуемое конечное состояние собственной переключаемой кнопки.
                $target=Find-AcceptanceControl $step
                $pattern=$target.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
                $desired=if ([bool]$step.value) { [System.Windows.Automation.ToggleState]::On } else { [System.Windows.Automation.ToggleState]::Off }
                if ($pattern.Current.ToggleState -ne $desired) { Invoke-Control $target }
                Wait-Observed { $pattern.Current.ToggleState -eq $desired } 'Не подтверждено состояние переключателя.'
            }
            'membership' {
                # list ограничивает адресный флажок назначениями открытого Stream;
                # target является единственной строкой с точным именем внутри этого списка.
                $list=Find-Control 'streamDestinations' -Visible -Type ([System.Windows.Automation.ControlType]::List)
                $target=Find-AcceptanceControl $step $list
                Click-AcceptanceElement $target -CheckIndicator
                $observation='Нажатие флажка; требуется серверная сверка'
            }
            'spin' {
                # target/range задают доступный числовой контрол настроек и его диапазон UIA.
                $target=Find-AcceptanceControl $step
                Focus-OwnedWindow $management
                Assert-OwnedElement $target
                $range=$target.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
                $range.SetValue([double]$step.value)
                Wait-Observed { $range.Current.Value -eq [double]$step.value } 'Не отражено значение числовой настройки.'
            }
            'type' {
                # target получает текст в настоящий пользовательский ввод, включая password Edit.
                $target=Find-AcceptanceControl $step
                Click-AcceptanceElement $target
                Wait-Observed { try { Assert-AcceptanceInputFocus $target; return $true } catch { return $false } } 'Адресное нажатие не сфокусировало поле.'
                if ($null -ne $step.clearLength) {
                    Send-AcceptanceInputKey $target 0x23
                    # erased считает удаления точно известного синтетического исходного значения.
                    for ($erased=0; $erased -lt [int]$step.clearLength; $erased++) { Send-AcceptanceInputKey $target 0x08 }
                }
                # character — очередной UTF-16 символ; адресат остаётся собственным foreground окном.
                foreach ($character in ([string]$step.value).ToCharArray()) {
                    Assert-AcceptanceInputFocus $target
                    $null=[ProfileUiWindows]::PostMessageW([IntPtr]::new([long]$management.Current.NativeWindowHandle),0x102,[IntPtr]::new([int]$character),[IntPtr]::new(1))
                }
            }
            'combo' { Select-AcceptanceCombo $step.id $step.value $(if ($step.scope -eq 'main') { $main } else { $management }); $observation=$step.value }
            'assert' {
                # target проверяется по ожидаемому свойству без раскрытия значения в отчёте.
                Wait-Observed { try { $null=Find-AcceptanceControl $step $(if ($step.dialog) { Find-Control $step.dialog -Root $null -Visible } elseif ($step.scope -eq 'main') { $main } else { $management }); return $true } catch { return $false } } 'Не появился контрол утверждения.'
                $target=Find-AcceptanceControl $step $(if ($step.dialog) { Find-Control $step.dialog -Root $null -Visible } elseif ($step.scope -eq 'main') { $main } else { $management })
                switch ($step.property) {
                    'enabled' { Wait-Observed { $target.Current.IsEnabled -eq [bool]$step.value } ('Неверная доступность '+$step.id); $observation=$target.Current.IsEnabled }
                    'password' { Wait-Observed { $target.Current.IsPassword -eq [bool]$step.value } 'Неверный режим маскирования.'; $observation=$target.Current.IsPassword }
                    'value' { Wait-Observed { (Get-ControlValue $target) -ceq [string]$step.value } ('Неверное значение '+$step.id); $observation='exact-match' }
                    'name' { Wait-Observed { $target.Current.Name -ceq [string]$step.value } ('Неверная подпись '+$step.id); $observation='exact-match' }
                    'range' { $range=$target.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern); Wait-Observed { $range.Current.Value -eq [double]$step.value } 'Неверное числовое значение настройки.'; $observation=$range.Current.Value }
                    'options' {
                        # opened сохраняет собственный popup на время чтения конечных пунктов без клавиши Enter.
                        $opened=Open-AcceptanceCombo $target
                        try { $observation=@($opened.items|ForEach-Object { Assert-OwnedElement $_; [pscustomobject]@{Name=$_.Current.Name;Enabled=$_.Current.IsEnabled} }) } finally { $opened.expand.Collapse() }
                        if ($step.value -and (($observation.Name -join '|') -cne ($step.value -join '|'))) { throw 'Неверные пункты селектора.' }
                    }
                    default { throw 'Неизвестное утверждение UIA.' }
                }
            }
            'copy' {
                # target нажимает Copy; clipboardValue сравнивается в RAM и не выгружается в результат.
                $target=Find-AcceptanceControl $step
                [ConfigurationClipboard]::Clear()
                Invoke-Control $target
                Wait-Observed {
                    try {
                        $clipboardValue=[ConfigurationClipboard]::Read()
                        if ($step.sha256) {
                            # hasher/digest сверяют точные UTF-8 байты, не сохраняя ключ в плане или результате.
                            $hasher=[Security.Cryptography.SHA256]::Create()
                            try { $digest=[BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($clipboardValue))).Replace('-','') } finally { $hasher.Dispose() }
                            return $digest -ieq $step.sha256
                        }
                        return $clipboardValue -ceq [string]$step.value
                    } catch { return $false }
                } 'Clipboard не совпал с точным ожидаемым значением.'
                $observation='exact-match'
            }
            'hide' {
                Focus-OwnedWindow $management
                Assert-OwnedElement $management
                # hiddenWindow сохраняет HWND до скрытия: Qt UIA IsOffscreen может остаться устаревшим.
                $hiddenWindow=[IntPtr]::new([long]$management.Current.NativeWindowHandle)
                $null=[ProfileUiWindows]::PostMessageW($hiddenWindow,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
                Wait-Observed { -not [ProfileUiWindows]::IsWindowVisible($hiddenWindow) } 'Окно управления не скрыто.'
            }
            'tree' { $observation=Get-AcceptanceTree }
            default { throw ('Неизвестное действие '+$step.action) }
        }
        $outcomes.Add([pscustomobject]@{ label=$step.label; action=$step.action; status='PASS'; observation=$observation })
    }
    $report.Passed=$true
} catch {
    $report.Error=$_.Exception.Message
    $report['FailedLine']=$_.InvocationInfo.ScriptLineNumber
    $report['Stack']=$_.ScriptStackTrace
    $report['ForegroundWindow']=[ProfileUiWindows]::GetForegroundWindow().ToInt64()
    $report['Tree']=Get-AcceptanceTree
} finally {
    $report.Check='ConfigurationNativeUi'
    $report.Checks=$outcomes.ToArray()
    $report.EndedUtc=[DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText($env:CONFIGURATION_RESULT,($report | ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
}
if (-not $report.Passed) { throw $report.Error }
'@
# Передаёт импорту только проверенную идентичность текущей portable-копии.
& ([scriptblock]::Create($prefix+$body)) -ObsProcessId ([int]$identity.pid) -ExpectedExecutable $identity.executable -ExpectedStartTicks ([long]$identity.startTicks) -MainWindowHandle ([long]$identity.hwnd) -ExpectedPluginPath $identity.plugin -ExpectedPluginSha256 $identity.pluginSha256 -Locale $identity.locale -FixtureOrigin $identity.origin -ResultRoot $identity.results
