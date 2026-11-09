[CmdletBinding()]
param(
    [Parameter(Mandatory)][int] $ObsProcessId,
    [Parameter(Mandatory)][string] $ExpectedExecutable,
    [Parameter(Mandatory)][long] $ExpectedStartTicks,
    [Parameter(Mandatory)][long] $MainWindowHandle,
    [Parameter(Mandatory)][string] $WorkspaceRoot,
    [Parameter(Mandatory)][string] $ExpectedPluginPath,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{64}$')][string] $ExpectedPluginSHA256,
    [ValidateSet('ru-RU', 'en-US')][string] $Locale,
    [ValidateRange(1, 2)][int] $Launch = 1,
    [int] $ExpectedDockLeft = 0,
    [int] $ExpectedDockTop = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$nativeSource = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class ObsWindowInspection {
    [StructLayout(LayoutKind.Sequential)] public struct Rectangle { public int Left; public int Top; public int Right; public int Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct MonitorInformation { public int Size; public Rectangle Monitor; public Rectangle Work; public uint Flags; }
    public delegate bool WindowVisitor(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(WindowVisitor visitor, IntPtr parameter);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll", ExactSpelling=true)] public static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rectangle rectangle);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr window, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)] public static extern bool GetMonitorInfoW(IntPtr monitor, ref MonitorInformation information);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
}
'@
Add-Type -TypeDefinition $nativeSource

$workspace = (Resolve-Path -LiteralPath $WorkspaceRoot).Path.TrimEnd('\', '/')
$executable = (Resolve-Path -LiteralPath $ExpectedExecutable).Path
if (-not $executable.StartsWith($workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Исполняемый файл OBS должен находиться внутри рабочего каталога.'
}
$ancestor = Get-Item -LiteralPath $executable
while ($ancestor.FullName -ne $workspace) {
    if (($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Путь тестового OBS не должен проходить через ссылки.'
    }
    $ancestor = if ($ancestor -is [IO.DirectoryInfo]) { $ancestor.Parent } else { $ancestor.Directory }
}

function Assert-OwnedProcess {
    $owned = [Diagnostics.Process]::GetProcessById($ObsProcessId)
    try {
        if ($owned.HasExited -or $owned.MainModule.FileName -ne $executable -or
            $owned.StartTime.ToUniversalTime().Ticks -ne $ExpectedStartTicks) {
            throw 'Идентичность изолированного процесса OBS изменилась.'
        }
    } finally {
        $owned.Dispose()
    }
}

function Wait-Condition {
    param([Parameter(Mandatory)][scriptblock] $Condition, [Parameter(Mandatory)][string] $Failure)
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Assert-OwnedProcess
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    try {
        Write-UiDiagnostics $Failure
    } catch {
        [pscustomobject]@{ Check = 'ObsUiDiagnostics'; Failure = $Failure
            DiagnosticError = $_.Exception.Message } | ConvertTo-Json -Compress
    }
    throw $Failure
}

function Assert-OwnedElement {
    param([Parameter(Mandatory)] $Element)
    Assert-OwnedProcess
    $elementProcessId = $Element.Current.ProcessId
    if ($elementProcessId -ne $ObsProcessId) {
        [pscustomobject]@{ Check = 'ObsUiOwnership'; ActualPID = $elementProcessId; ExpectedPID = $ObsProcessId
            CallerScriptLineNumber = $MyInvocation.ScriptLineNumber } | ConvertTo-Json -Compress
        throw 'Элемент интерфейса не принадлежит тестовому процессу OBS.'
    }
}

$russian = $Locale -eq 'ru-RU'
$foregroundBeforeDock = $null
$dockMenuExpandPattern = $null
$dockMenuExpandState = $null
$gearName = if ($russian) { 'Открыть управление и настройки' } else { 'Open management and settings' }
$title = if ($russian) { 'GOST Restreamer — Управление и настройки' } else { 'GOST Restreamer — Management and settings' }
$pages = if ($russian) {
    @('Потоки', 'Выходы', 'Группы и профили', 'Заглушки', 'Диагностика', 'Настройки')
} else {
    @('Streams', 'Outputs', 'Groups and profiles', 'Fallback', 'Diagnostics', 'Settings')
}
$processCondition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ObsProcessId)
$gearCondition = [System.Windows.Automation.AndCondition]::new(
    [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $gearName),
    [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button))

Assert-OwnedProcess
$main = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]::new($MainWindowHandle))
Assert-OwnedElement $main
if ($main.Current.ClassName -cne 'OBSBasic') { throw 'Переданный дескриптор не относится к главному окну OBS.' }

function Get-Gears {
    $roots = @($main) + @([System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Children, $processCondition) | Where-Object { $_.Current.Name -ceq 'GOST Restreamer' })
    $seen = @{}
    foreach ($root in $roots) {
        Assert-OwnedElement $root
        foreach ($gear in $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $gearCondition)) {
            $identity = $gear.GetRuntimeId() -join ':'
            if (-not $gear.Current.IsOffscreen -and -not $seen.ContainsKey($identity)) {
                $seen[$identity] = $true
                Write-Output $gear
            }
        }
    }
}

function Get-ManagementWindows {
    $roots = @($main) + @([System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Children, $processCondition))
    $seen = @{}
    foreach ($root in $roots) {
        Assert-OwnedElement $root
        $candidates = @($root) + @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $processCondition))
        foreach ($candidate in $candidates) {
            Assert-OwnedElement $candidate
            if ($candidate.Current.Name.StartsWith('GOST Restreamer — ', [StringComparison]::Ordinal)) {
                $identity = $candidate.GetRuntimeId() -join ':'
                if (-not $seen.ContainsKey($identity)) {
                    $seen[$identity] = $true
                    Write-Output $candidate
                }
            }
        }
    }
}

function Get-VisibleManagementWindows {
    @(Get-ManagementWindows | Where-Object { -not $_.Current.IsOffscreen })
}

function Write-UiDiagnostics {
    param([string] $Failure)
    Assert-OwnedProcess
    $nativeWindows = [Collections.Generic.List[object]]::new()
    $visitor = [ObsWindowInspection+WindowVisitor] {
        param([IntPtr] $window, [IntPtr] $parameter)
        $ownerProcess = [uint32]0
        $null = [ObsWindowInspection]::GetWindowThreadProcessId($window, [ref]$ownerProcess)
        if ($ownerProcess -eq $ObsProcessId) {
            $text = [Text.StringBuilder]::new(1024)
            $null = [ObsWindowInspection]::GetWindowText($window, $text, $text.Capacity)
            $nativeWindows.Add([pscustomobject]@{
                Handle = $window.ToInt64(); Title = $text.ToString()
                Visible = [ObsWindowInspection]::IsWindowVisible($window)
                Owner = [ObsWindowInspection]::GetWindow($window, 4).ToInt64()
            })
        }
        return $true
    }
    $null = [ObsWindowInspection]::EnumWindows($visitor, [IntPtr]::Zero)
    $roots = @([System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Children, $processCondition))
    $rootDetails = @($roots | ForEach-Object {
        Assert-OwnedElement $_
        [pscustomobject]@{ Name = $_.Current.Name; Offscreen = $_.Current.IsOffscreen
            Handle = $_.Current.NativeWindowHandle; Class = $_.Current.ClassName }
    })
    $descendants = @()
    foreach ($root in @($main) + @($roots | Where-Object { $_.Current.Name -ceq 'GOST Restreamer' })) {
        Assert-OwnedElement $root
        $descendants += @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
            $processCondition) | Where-Object {
                Assert-OwnedElement $_
                $_.Current.NativeWindowHandle -ne 0 -or $_.Current.Name.StartsWith('GOST Restreamer', [StringComparison]::Ordinal)
            } | Select-Object -First 50 | ForEach-Object {
                [pscustomobject]@{ Name = $_.Current.Name; Offscreen = $_.Current.IsOffscreen
                    Handle = $_.Current.NativeWindowHandle; Class = $_.Current.ClassName }
            })
    }
    $currentDockMenuState = $null
    if ($dockMenuExpandPattern) {
        try { $currentDockMenuState = $dockMenuExpandPattern.Current.ExpandCollapseState.ToString() } catch { $currentDockMenuState = 'Unavailable' }
    }
    [pscustomobject]@{ Check = 'ObsUiDiagnostics'; Failure = $Failure; ProcessId = $ObsProcessId
        MainHandle = $MainWindowHandle; NativeWindows = $nativeWindows.ToArray(); UiaRoots = $rootDetails
        UiaDescendants = $descendants; ForegroundBeforeDock = $foregroundBeforeDock
        ForegroundIsMain = ([ObsWindowInspection]::GetForegroundWindow().ToInt64() -eq $MainWindowHandle)
        DockMenuExpandState = $currentDockMenuState } | ConvertTo-Json -Depth 5 -Compress
}

function Invoke-Gear {
    $gears = @(Get-Gears)
    if ($gears.Count -ne 1) { throw 'Не найдена единственная локализованная шестерёнка Dock.' }
    Assert-OwnedElement $gears[0]
    $invoke = $gears[0].GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $invoke.Invoke()
}

function Assert-OwnedDock {
    param([Parameter(Mandatory)][IntPtr] $Window)
    Assert-OwnedProcess
    $ownerProcess = [uint32]0
    $null = [ObsWindowInspection]::GetWindowThreadProcessId($Window, [ref]$ownerProcess)
    $nativeTitle = [Text.StringBuilder]::new(1024)
    if ($ownerProcess -eq $ObsProcessId) {
        $null = [ObsWindowInspection]::GetWindowText($Window, $nativeTitle, $nativeTitle.Capacity)
    }
    if ($Window -eq [IntPtr]::Zero -or $ownerProcess -ne $ObsProcessId -or
        $nativeTitle.ToString() -cne 'GOST Restreamer' -or
        [ObsWindowInspection]::GetWindow($Window, 4).ToInt64() -ne $MainWindowHandle -or
        -not [ObsWindowInspection]::IsWindowVisible($Window)) {
        throw 'Не подтверждён собственный видимый плавающий Dock.'
    }
}

function Get-OwnedDock {
    Assert-OwnedProcess
    $handles = [Collections.Generic.List[long]]::new()
    $visitor = [ObsWindowInspection+WindowVisitor] {
        param([IntPtr] $window, [IntPtr] $parameter)
        $ownerProcess = [uint32]0
        $null = [ObsWindowInspection]::GetWindowThreadProcessId($window, [ref]$ownerProcess)
        if ($ownerProcess -eq $ObsProcessId -and [ObsWindowInspection]::IsWindowVisible($window)) {
            $nativeTitle = [Text.StringBuilder]::new(1024)
            $null = [ObsWindowInspection]::GetWindowText($window, $nativeTitle, $nativeTitle.Capacity)
            if ($nativeTitle.ToString() -ceq 'GOST Restreamer' -and
                [ObsWindowInspection]::GetWindow($window, 4).ToInt64() -eq $MainWindowHandle) {
                $handles.Add($window.ToInt64())
            }
        }
        return $true
    }
    $null = [ObsWindowInspection]::EnumWindows($visitor, [IntPtr]::Zero)
    if ($handles.Count -ne 1) { throw 'Не найден единственный собственный плавающий Dock.' }
    $window = [IntPtr]::new($handles[0])
    Assert-OwnedDock $window
    return $window
}

function Read-DockPosition {
    param([Parameter(Mandatory)][IntPtr] $Window)
    Assert-OwnedDock $Window
    $previousDpi = [ObsWindowInspection]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
    if ($previousDpi -eq [IntPtr]::Zero) { throw 'Не удалось установить контекст физических координат тестового потока.' }
    try {
        $rectangle = [ObsWindowInspection+Rectangle]::new()
        if (-not [ObsWindowInspection]::GetWindowRect($Window, [ref]$rectangle)) { throw 'Не удалось прочитать положение Dock.' }
        return [pscustomobject]@{ Left = $rectangle.Left; Top = $rectangle.Top
            Width = $rectangle.Right - $rectangle.Left; Height = $rectangle.Bottom - $rectangle.Top }
    } finally {
        $null = [ObsWindowInspection]::SetThreadDpiAwarenessContext($previousDpi)
    }
}

function Move-OwnedDock {
    param([Parameter(Mandatory)][IntPtr] $Window)
    Assert-OwnedDock $Window
    $previousDpi = [ObsWindowInspection]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
    if ($previousDpi -eq [IntPtr]::Zero) { throw 'Не удалось установить контекст физических координат тестового потока.' }
    try {
        $rectangle = [ObsWindowInspection+Rectangle]::new()
        if (-not [ObsWindowInspection]::GetWindowRect($Window, [ref]$rectangle)) { throw 'Не удалось прочитать исходную геометрию Dock.' }
        $information = [ObsWindowInspection+MonitorInformation]::new()
        $information.Size = [Runtime.InteropServices.Marshal]::SizeOf([type][ObsWindowInspection+MonitorInformation])
        $monitor = [ObsWindowInspection]::MonitorFromWindow($Window, 2)
        if ($monitor -eq [IntPtr]::Zero -or -not [ObsWindowInspection]::GetMonitorInfoW($monitor, [ref]$information)) {
            throw 'Не удалось определить рабочую область монитора Dock.'
        }
        $width = $rectangle.Right - $rectangle.Left
        $height = $rectangle.Bottom - $rectangle.Top
        if ($information.Work.Right - $information.Work.Left -lt $width + 128 -or
            $information.Work.Bottom - $information.Work.Top -lt $height + 128) {
            throw 'Рабочая область недостаточна для безопасной проверки перемещения Dock.'
        }
        $targetLeft = $information.Work.Left + 64
        $targetTop = $information.Work.Top + 64
        if ([Math]::Abs($targetLeft - $rectangle.Left) -lt 32 -and [Math]::Abs($targetTop - $rectangle.Top) -lt 32) {
            $targetLeft = $information.Work.Right - $width - 64
            $targetTop = $information.Work.Bottom - $height - 64
        }
        if ([Math]::Abs($targetLeft - $rectangle.Left) -lt 32 -and [Math]::Abs($targetTop - $rectangle.Top) -lt 32) {
            throw 'Не найдено отличимое безопасное положение Dock.'
        }
        Assert-OwnedDock $Window
        if (-not [ObsWindowInspection]::SetWindowPos($Window, [IntPtr]::Zero, $targetLeft, $targetTop, 0, 0, 0x15)) {
            throw 'Не удалось переместить собственный Dock.'
        }
        Wait-Condition {
            $current = Read-DockPosition $Window
            [Math]::Abs($current.Left - $targetLeft) -le 2 -and [Math]::Abs($current.Top - $targetTop) -le 2
        } 'Собственный Dock не достиг проверяемого положения.'
        return Read-DockPosition $Window
    } finally {
        $null = [ObsWindowInspection]::SetThreadDpiAwarenessContext($previousDpi)
    }
}

function Assert-DockObservations {
    Assert-OwnedDock $dockWindow
    $dockElement = [System.Windows.Automation.AutomationElement]::FromHandle($dockWindow)
    Assert-OwnedElement $dockElement
    $labelsCondition = [System.Windows.Automation.AndCondition]::new($processCondition,
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ClassNameProperty, 'QLabel'))
    $expectedIdle = if ($russian) { 'Остановлена' } else { 'Stopped' }
    Wait-Condition {
        $names = @($dockElement.FindAll([System.Windows.Automation.TreeScope]::Descendants, $labelsCondition) |
            ForEach-Object { Assert-OwnedElement $_; $_.Current.Name })
        @($names | Where-Object { $_ -ceq 'Isolated' }).Count -eq 1 -and
            @($names | Where-Object { $_ -ceq $expectedIdle }).Count -eq 1
    } 'Dock не показывает наблюдаемые тестовый профиль и остановленную трансляцию OBS.'
}

function Observe-ManagementActivation {
    $deadline = [DateTime]::UtcNow.AddSeconds(1)
    do {
        Assert-OwnedElement $management
        if ([ObsWindowInspection]::GetForegroundWindow().ToInt64() -eq $management.Current.NativeWindowHandle) { return $true }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

function Prepare-OwnedMainForeground {
    param([ValidateRange(1, 20)][int] $ObservationSeconds = 1)
    Assert-OwnedElement $main
    $window = [IntPtr]::new($MainWindowHandle)
    $ownerProcess = [uint32]0
    $null = [ObsWindowInspection]::GetWindowThreadProcessId($window, [ref]$ownerProcess)
    $nativeTitle = [Text.StringBuilder]::new(1024)
    if ($ownerProcess -eq $ObsProcessId) { $null = [ObsWindowInspection]::GetWindowText($window, $nativeTitle, $nativeTitle.Capacity) }
    if ($ownerProcess -ne $ObsProcessId -or $main.Current.ClassName -cne 'OBSBasic' -or
        $main.Current.NativeWindowHandle -ne $MainWindowHandle -or
        -not $nativeTitle.ToString().StartsWith('OBS 33.0.1 - ', [StringComparison]::Ordinal)) {
        throw 'Не подтверждено собственное главное окно для подготовки проверки активации.'
    }
    Assert-OwnedProcess
    $requested = [ObsWindowInspection]::SetForegroundWindow($window)
    $deadline = [DateTime]::UtcNow.AddSeconds($ObservationSeconds)
    do {
        Assert-OwnedProcess
        if ([ObsWindowInspection]::GetForegroundWindow() -eq $window) {
            return [pscustomobject]@{ Requested = $requested; Verified = $true }
        }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    return [pscustomobject]@{ Requested = $requested; Verified = $false }
}

$foregroundBeforeDock = Prepare-OwnedMainForeground -ObservationSeconds 20
if (-not $foregroundBeforeDock.Verified) {
    try { Write-UiDiagnostics 'Не подтверждено foreground-предусловие собственного меню Dock.' } catch { }
    throw 'Не подтверждено foreground-предусловие fixture; действия меню не выполнялись.'
}
if ($Launch -eq 1) {
    $menuNames = if ($russian) { @('Док-панели (D)', 'Док-панели') } else { @('Docks') }
    $menuCondition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuItem)
    $docksMenus = @($main.FindAll([System.Windows.Automation.TreeScope]::Descendants, $menuCondition) |
        Where-Object { $_.Current.Name -cin $menuNames })
    if ($docksMenus.Count -ne 1) {
        try { Write-UiDiagnostics 'Не найдено единственное штатное меню Dock.' } catch { }
        throw 'Не найдено единственное штатное меню Dock.'
    }
    Assert-OwnedElement $docksMenus[0]
    $expand = $docksMenus[0].GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $dockMenuExpandPattern = $expand
    $expand.Expand()
    $dockMenuExpandState = $expand.Current.ExpandCollapseState.ToString()
    $dockActionCondition = [System.Windows.Automation.AndCondition]::new($menuCondition,
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, 'GOST Restreamer'))
    Wait-Condition {
        @($docksMenus[0].FindAll([System.Windows.Automation.TreeScope]::Descendants, $dockActionCondition)).Count -eq 1
    } 'В меню Dock не появился пункт GOST Restreamer.'
    $dockAction = @($docksMenus[0].FindAll([System.Windows.Automation.TreeScope]::Descendants, $dockActionCondition))[0]
    Assert-OwnedElement $dockAction
    $dockAction.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
Wait-Condition { @(Get-Gears).Count -eq 1 } 'Видимая шестерёнка Dock не появилась или не восстановилась.'
$dockWindow = Get-OwnedDock
if ($Launch -eq 1) {
    $dockPosition = Move-OwnedDock $dockWindow
} else {
    $dockPosition = Read-DockPosition $dockWindow
    if ([Math]::Abs($dockPosition.Left - $ExpectedDockLeft) -gt 2 -or
        [Math]::Abs($dockPosition.Top - $ExpectedDockTop) -gt 2) {
        throw 'Повторный запуск не восстановил сохранённое положение Dock.'
    }
}
Assert-DockObservations
if (@(Get-ManagementWindows).Count -ne 0) { throw 'Расширенное окно создано до нажатия шестерёнки.' }
Invoke-Gear
Wait-Condition { @(Get-VisibleManagementWindows).Count -eq 1 } 'Расширенное окно не открылось.'
$management = @(Get-VisibleManagementWindows)[0]
Assert-OwnedElement $management
if ($management.Current.Name -cne $title) { throw 'Заголовок расширенного окна не соответствует локализации.' }
$managementHandle = $management.Current.NativeWindowHandle
$runtimeId = $management.GetRuntimeId() -join ':'
if ($managementHandle -eq 0) { throw 'Не подтверждён собственный дескриптор расширенного окна.' }

function Assert-SameManagementWindow {
    param([Parameter(Mandatory)][string] $Stage)
    $windows = @(Get-VisibleManagementWindows)
    if ($windows.Count -ne 1 -or $windows[0].Current.NativeWindowHandle -ne $managementHandle -or
        ($windows[0].GetRuntimeId() -join ':') -ne $runtimeId) {
        $actual = @($windows | ForEach-Object {
            Assert-OwnedElement $_
            [pscustomobject]@{ Handle = $_.Current.NativeWindowHandle; RuntimeId = ($_.GetRuntimeId() -join ':')
                Title = $_.Current.Name; Offscreen = $_.Current.IsOffscreen }
        })
        [pscustomobject]@{ Check = 'ObsUiIdentity'; Stage = $Stage; ExpectedHandle = $managementHandle
            ExpectedRuntimeId = $runtimeId; ActualCount = $windows.Count; Actual = $actual } |
            ConvertTo-Json -Depth 4 -Compress
        try { Write-UiDiagnostics ('Идентичность окна: ' + $Stage) } catch { }
        throw ('Повторное открытие не сохранило единственный экземпляр расширенного окна: ' + $Stage)
    }
    Assert-OwnedElement $windows[0]
}

function Close-OwnedManagementWindow {
    Assert-OwnedElement $management
    $window = [IntPtr]::new($management.Current.NativeWindowHandle)
    $ownerProcess = [uint32]0
    $null = [ObsWindowInspection]::GetWindowThreadProcessId($window, [ref]$ownerProcess)
    $nativeTitle = [Text.StringBuilder]::new(1024)
    $null = [ObsWindowInspection]::GetWindowText($window, $nativeTitle, $nativeTitle.Capacity)
    if ($window -eq [IntPtr]::Zero -or $ownerProcess -ne $ObsProcessId -or
        $management.Current.Name -cne $title -or $nativeTitle.ToString() -cne $title) {
        throw 'Не подтверждена идентичность закрываемого расширенного окна.'
    }
    Assert-OwnedProcess
    if (-not [ObsWindowInspection]::PostMessageW($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw 'Не удалось запросить штатное закрытие расширенного окна.'
    }
}

function Get-VisibleNativeManagementHandles {
    Assert-OwnedProcess
    $handles = [Collections.Generic.List[long]]::new()
    $visitor = [ObsWindowInspection+WindowVisitor] {
        param([IntPtr] $window, [IntPtr] $parameter)
        $ownerProcess = [uint32]0
        $null = [ObsWindowInspection]::GetWindowThreadProcessId($window, [ref]$ownerProcess)
        if ($ownerProcess -eq $ObsProcessId -and [ObsWindowInspection]::IsWindowVisible($window)) {
            $nativeTitle = [Text.StringBuilder]::new(1024)
            $null = [ObsWindowInspection]::GetWindowText($window, $nativeTitle, $nativeTitle.Capacity)
            if ($nativeTitle.ToString() -ceq $title) { $handles.Add($window.ToInt64()) }
        }
        return $true
    }
    $null = [ObsWindowInspection]::EnumWindows($visitor, [IntPtr]::Zero)
    $handles.ToArray()
}

$navigationCondition = [System.Windows.Automation.PropertyCondition]::new(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
$navigation = @($management.FindAll([System.Windows.Automation.TreeScope]::Descendants, $navigationCondition))
$names = @($navigation | ForEach-Object { $_.Current.Name })
if ($names.Count -ne 6 -or ($names -join '|') -cne ($pages -join '|')) {
    throw 'Не подтверждены шесть локализованных разделов расширенного окна.'
}
$foregroundBeforeGear = Prepare-OwnedMainForeground
Invoke-Gear
Assert-SameManagementWindow 'secondGear'
$activationAfterGear = Observe-ManagementActivation
$windowPattern = $management.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
$minimizeVerified = $false
$activationAfterRestore = $null
$foregroundBeforeRestore = $null
if ($windowPattern.Current.CanMinimize) {
    Assert-OwnedElement $management
    $windowPattern.SetWindowVisualState([System.Windows.Automation.WindowVisualState]::Minimized)
    Wait-Condition { $windowPattern.Current.WindowVisualState -eq [System.Windows.Automation.WindowVisualState]::Minimized } 'Окно не свернулось.'
    $foregroundBeforeRestore = Prepare-OwnedMainForeground
    Invoke-Gear
    Wait-Condition { $windowPattern.Current.WindowVisualState -eq [System.Windows.Automation.WindowVisualState]::Normal } 'Шестерёнка не восстановила свёрнутое окно.'
    Assert-SameManagementWindow 'minimizeRestore'
    $activationAfterRestore = Observe-ManagementActivation
    $minimizeVerified = $true
}
Assert-OwnedElement $management
$selectedSection = $navigation[$navigation.Count - 1]
Assert-OwnedElement $selectedSection
$selection = $selectedSection.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
$selection.Select()
Wait-Condition { $selection.Current.IsSelected } 'Не удалось выбрать последний раздел перед закрытием.'
Close-OwnedManagementWindow
Wait-Condition { @(Get-VisibleManagementWindows).Count -eq 0 } 'Закрытие не скрыло расширенное окно.'
Assert-OwnedElement $main
if (@(Get-Gears).Count -ne 1) { throw 'Закрытие расширенного окна удалило шестерёнку Dock.' }
Invoke-Gear
Wait-Condition { @(Get-VisibleManagementWindows).Count -eq 1 } 'Повторное открытие скрытого окна не завершилось.'
$reopened = @(Get-VisibleManagementWindows)[0]
Assert-OwnedElement $reopened
if ($reopened.Current.Name -cne $title) { throw 'Повторно открытое окно имеет неверный локализованный заголовок.' }
$reopenedNavigation = @($reopened.FindAll([System.Windows.Automation.TreeScope]::Descendants, $navigationCondition))
$reopenedNames = @($reopenedNavigation | ForEach-Object { $_.Current.Name })
if ($reopenedNames.Count -ne 6 -or ($reopenedNames -join '|') -cne ($pages -join '|')) {
    throw 'Повторно открытое окно потеряло локализованные разделы.'
}
$reopenedSection = $reopenedNavigation[$reopenedNavigation.Count - 1]
Assert-OwnedElement $reopenedSection
$reopenedSelection = $reopenedSection.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
Wait-Condition { $reopenedSelection.Current.IsSelected } 'Закрытие и повторное открытие сбросили выбранный раздел.'
$reopenedHandle = $reopened.Current.NativeWindowHandle
if ($reopenedHandle -eq 0) { throw 'Не подтверждён дескриптор повторно открытого окна.' }
$nativeManagement = @(Get-VisibleNativeManagementHandles)
if (@(Get-VisibleManagementWindows).Count -ne 1 -or $nativeManagement.Count -ne 1 -or
    $nativeManagement[0] -ne $reopenedHandle) {
    throw 'После повторного открытия не подтверждено единственное видимое нативное окно управления.'
}
Assert-DockObservations

$owned = [Diagnostics.Process]::GetProcessById($ObsProcessId)
try {
    Assert-OwnedProcess
    $runtimeDirectory = (Split-Path -Parent $executable).TrimEnd('\', '/')
    $runtime = @($owned.Modules | Where-Object { $_.ModuleName -in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll') })
    if ($runtime.Count -ne 3) { throw 'Не подтверждены три загруженных модуля Qt.' }
    foreach ($module in $runtime) {
        if ((Split-Path -Parent $module.FileName) -ne $runtimeDirectory -or
            $module.FileVersionInfo.ProductVersion -notmatch '^6\.11\.1(?:\D|$)') {
            throw 'Загруженный Qt не соответствует изолированному runtime 6.11.1.'
        }
    }
    $pluginModules = @($owned.Modules | Where-Object { $_.ModuleName -ieq 'gost-restreamer-obs-plugin.dll' })
    if ($pluginModules.Count -ne 1 -or $pluginModules[0].FileName -ine $ExpectedPluginPath) {
        throw 'Загружена не единственная DLL из согласованного каталога плагина.'
    }
    $pluginHasher = [Security.Cryptography.SHA256]::Create()
    try {
        $pluginStream = [IO.File]::OpenRead($pluginModules[0].FileName)
        try {
            $actualPluginHash = [BitConverter]::ToString($pluginHasher.ComputeHash($pluginStream)).Replace('-', '')
        } finally {
            $pluginStream.Dispose()
        }
    } finally {
        $pluginHasher.Dispose()
    }
    if ($actualPluginHash -ine $ExpectedPluginSHA256) { throw 'Загруженная DLL отличается от проверяемой сборки.' }
    [pscustomobject]@{
        Check = 'ObsUi'; Locale = $Locale; ProcessId = $ObsProcessId; ManagementHandle = $managementHandle
        Launch = $Launch; DockVisibilityRestored = ($Launch -eq 2); DockPositionVerified = ($Launch -eq 2)
        DockLeft = $dockPosition.Left; DockTop = $dockPosition.Top; DockProfileAndIdleVerified = $true
        Pages = 6; GearReuse = $true; CloseHideReopen = $true; MinimizeRestore = $minimizeVerified
        ReopenedHandle = $reopenedHandle; NativeHandleRecreated = ($reopenedHandle -ne $managementHandle)
        SelectedSectionPreserved = $true
        ActivationAfterGear = $activationAfterGear; ActivationAfterRestore = $activationAfterRestore
        ForegroundBeforeGear = $foregroundBeforeGear; ForegroundBeforeRestore = $foregroundBeforeRestore
        ForegroundBeforeDock = $foregroundBeforeDock; DockMenuExpandState = $dockMenuExpandState
        ActivationSetupVerified = ($foregroundBeforeGear.Verified -and ($null -eq $foregroundBeforeRestore -or $foregroundBeforeRestore.Verified))
        ActivationVerified = ($foregroundBeforeGear.Verified -and $activationAfterGear -and
            ($null -eq $foregroundBeforeRestore -or ($foregroundBeforeRestore.Verified -and $activationAfterRestore)))
        PluginPath = $pluginModules[0].FileName; PluginSHA256 = $actualPluginHash
        QtRuntime = @($runtime | ForEach-Object { @{ Path = $_.FileName; Version = $_.FileVersionInfo.ProductVersion } })
    } | ConvertTo-Json -Depth 4 -Compress
} finally {
    $owned.Dispose()
}
