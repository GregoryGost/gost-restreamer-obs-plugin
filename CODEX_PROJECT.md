# Профиль проекта Codex

## Activation Semantics

Режим репозитория — target project, использующий материалы `agent-projects-tools`.
Порядок входа: `AGENTS.md` → `.codex/rules/request_routing.md` → этот профиль →
точки входа применимых активных материалов → проверка их hard dependencies.
Правила и навыки применяются по области текущей задачи, а не все одновременно.

- Обязательная маршрутизация: `.codex/rules/request_routing.md`, всегда активна.
- Проверка зависимостей: `.codex/rules/material_dependencies.md`, обязательна при наличии hard dependencies.
- Координация: `.codex/rules/subagent_coordination.md`, обязательна перед делегированием исполняемого кода/тестов.
- Отсутствующие и не перечисленные опциональные материалы неактивны. Наличие файла, установленного инструмента или упоминание технологии не активирует материал.
- Этот профиль определяет стек и ограничения, но не разрешает побочные эффекты. Их область задаёт текущий запрос через обязательную маршрутизацию.
- Перед применением читать точку входа материала и проверять полное замыкание обязательных зависимостей; отсутствующую зависимость нельзя активировать молча.

## Project Identity

- Project key / workspace: `gost-restreamer-obs-plugin`.
- Название продукта: `GOST Restreamer`; модуль плагина в архитектуре: `gost-obs-plugin`.
- Тип: нативный плагин OBS Studio, управляющий клиент отдельного сервера.
- Основной язык: C++20. Дополнительные форматы: CMake, JSON, INI, TOML; QML допускается для будущих явно запрошенных задач.
- Платформа: Windows 10/11 x64; целевая базовая версия OBS — 33.0.1.
- Источник требований: `gost-obs-plugin-architecture.md`, редакция 1.5 от 08.11.2026.
- Стадия: подготовка репозитория. Исходники, CMake, тесты, CI, упаковка и совместимый комплект SDK пока не созданы. Архитектура описывает целевое состояние, а не выполненные проверки.

## Natural Language Profile

- Ответы пользователю: русский.
- Сообщения Git: русский; формат задаёт `git.md`.
- Собственная документация проекта: русский.
- Комментарии, пояснения рядом с кодом, документационные блоки, описания сценариев и проверок тестов: русский, грамматически цельные предложения.
- Идентификаторы кода и тестовых функций, имена API, полей, протоколов, библиотек, команд, путей и реальные контрактные значения сохранять на английском/в оригинале.
- Пользовательский интерфейс: русские подписи и локализация `en-US`, согласно архитектуре; продукт называется `GOST Restreamer`.
- Импортированные правила, навыки и инструкции ролей сохраняют язык upstream. Политика русского языка относится к собственной прозе проекта; переводить весь импортированный набор не требуется.
- `comment-language-audit` применять к затронутой прозе при соответствующих задачах; новые линтеры/тесты языка автоматически не добавлять.

## Active Stack Profiles

- `cpp-core`
- `cpp-style`
- `cpp-cmake`
- `cpp-testing`
- `qt-core`
- `qt-widgets`
- `qt-testing`
- `qt-quick-qml`

QML активирован по выбору владельца для будущих задач. Текущая продуктовая
архитектура использует Widgets; активация не требует добавлять QML-файлы,
Qt Quick runtime, модули или третье окно. Материалы QML применяются только
к задаче, действительно затрагивающей QML/Quick.

## Active Rules

- Required routing rule: `.codex/rules/request_routing.md`.
- Общие: `source_code_hygiene.md`, `git.md`.
- Документация: `context7_documentation.md`.
- C++: `cpp_core.md`, `cpp_style.md`, `cpp_cmake.md`, `cpp_testing.md`.
- Qt 6: `qt_core.md`, `qt_widgets.md`, `qt_testing.md`, `qt_quick_qml.md`.

Все дополнительные правила находятся в `.codex/rules/`; здесь использованы
точные имена файлов согласно activation contract upstream. Условные
bootstrap-правила проверки зависимостей и координации указаны выше отдельно.

## Active Skills

- `comment-language-audit`
- `cpp-core`
- `cpp-style`
- `cpp-cmake`
- `cpp-testing`
- `qt-core`
- `qt-widgets`
- `qt-testing`
- `qt-quick-qml`

Полные пакеты расположены в `.agents/skills/<skill-name>/`.

## Agent Roles And Model Selection

- Subagents enabled: yes, для разрешённых изменений исполняемого кода приложения и тестов.
- Delegation scope: исполняемый код/тесты; задачи только по документации, комментариям, docstrings и инструкциям выполняет основной агент.
- Primary agent model/effort: `gpt-6.1-sol` / `xhigh`, согласно существующему локальному `.codex/config.toml`.
- Ограничение локальной конфигурации: 4 одновременно открытых потока субагентов, без основного потока.
- Формат: отдельные `.codex/agents/*.toml` с `name`, `description`, `developer_instructions`, моделью и effort. MCP и разрешения наследуются от родителя; инструкции read-only не создают отдельную техническую песочницу.
- Доступность моделей и загрузку ролей проверять в конкретной сессии Codex; TOML-валидация не доказывает доступность аккаунта. Недоступные роли/модели не заменять молча.
- Fresh thread policy: каждое назначение — новый `spawn_agent` без истории (`fork_turns: "none"` при подтверждённом MultiAgentV2). Не использовать `followup_task` и завершённый поток для нового назначения.
- Subagent handoff persistence: `in-session-only`; автоматическое создание файлов handoff, задач, заметок, worktree и Git commits не разрешено.

Selected role files:

| Роль | Файл | Модель | Effort |
| --- | --- | --- | --- |
| Архитектура: стабильность и совместимость | `.codex/agents/architect_stability.toml` | `gpt-6.1-sol` | `xhigh` |
| Архитектура: целостность и развитие | `.codex/agents/architect_evolution.toml` | `gpt-6.1-sol` | `xhigh` |
| Исследование кода | `.codex/agents/code_explorer.toml` | `gpt-6-luna` | `high` |
| Исследование документации/API | `.codex/agents/docs_researcher.toml` | `gpt-6-luna` | `high` |
| Сложная реализация | `.codex/agents/complex_implementer.toml` | `gpt-6.1-sol` | `high` |
| Небольшие исправления | `.codex/agents/simple_implementer.toml` | `gpt-6-luna` | `high` |
| Независимые тесты | `.codex/agents/test_engineer.toml` | `gpt-6.1-sol` | `high` |
| Независимое ревью | `.codex/agents/code_reviewer.toml` | `gpt-6.1-sol` | `high` |
| Сложная диагностика | `.codex/agents/deep_debugger.toml` | `gpt-6-astra` | `xhigh` |

`database_architect` не выбран: серверная SQLite и миграции принадлежат backend,
локальная база данных плагину не требуется. Их изменение не входит в область
проекта плагина. Новая такая область потребует отдельного решения владельца.

### Delegation And Ownership

Перед делегированием загрузить `subagent_coordination.md`; выбрать только нужные
роли, определить владельцев файлов, разрешённые команды и необходимость
независимых тестов/ревью. Передать исходные требования, проверенную базу,
активные материалы, ограничения среды и ожидаемый отчёт, без истории чата.
Параллельные записи допускаются только при подтверждённом отсутствии пересечений
файлов, fixtures, контрактов и ресурсов. Основной агент отвечает за интеграцию.

### Architecture Gate

- Для каждой задачи до зависимой работы явно выбрать `NOT_REQUIRED`, если архитекторы не назначены.
- При назначении архитекторов обязательны обе роли; сразу установить `PENDING`, в том числе при назначении посреди работы.
- `PENDING`, `REOPENED`, архитектурный `BLOCKED` запрещают затронутые планы реализации, тестовые сценарии, изменения кода/тестов, выполнение тестов и приёмку основному агенту и исполнителям. Разрешены ограниченное чтение, фактическая диагностика и архитектурные предложения/критика.
- Архитекторы независимо изучают одинаковые требования, обмениваются критикой и оба явно подтверждают одну неизменяемую каноническую финальную редакцию. Основной агент проверяет область и выпускает утверждённый Architecture Handoff с `task_scope_id`, `decision_version`, контрактами, ограничениями, владельцами и обязательной проверкой; только после этого — `APPROVED`.
- Новое поведение должно следовать запросу или подтверждённому контракту. Дополнительные меры требуют установленного отказа и рассмотрения меньшего достаточного решения; мнение архитектора само по себе не разрешает новую функциональность.
- При закрытии gate остановить зависимых исполнителей, дождаться подтверждения/окончания команд, проверить частичные изменения и отсутствие продолжающейся записи. Сообщение «остановись» не доказывает безопасную паузу.
- Нарушение корректного дизайна реализацией исправляет исполнитель. Только доказанный дефект самого дизайна открывает `REOPENED`; неясные причины сначала исследовать без записи. Статусы архитектуры, тестов и ревью независимы.
- После сжатия контекста восстановить bootstrap из файлов и текущее решение `NOT_REQUIRED` либо полный актуальный утверждённый handoff. Без подтверждения не возобновлять зависимую работу.

### Validation Gate

Независимый тестировщик выводит сценарии из требований и контрактов после
`NOT_REQUIRED`/`APPROVED`, владеет назначенными тестами и не ослабляет корректные
ожидания ради успеха. Исполнитель изменяет только назначенный код/тесты.
Сложные изменения lifecycle, ABI, SSE, команд и потоков требуют независимых
тестировщика и reviewer; для доказанно малой безопасной правки основной агент
может обосновать отсутствие дополнительной роли, сохраняя обязательные проверки.

Перед финальной проверкой зафиксировать стабильный снимок исходников, тестов,
fixtures и существенной конфигурации, включая незакоммиченные/новые файлы;
не менять эти входы параллельно проверке. Материальная правка обнуляет
затронутые результаты. Отсутствующий обязательный инструмент/тест —
`NOT VERIFIED`/`BLOCKED`, не `PASS`. Основной агент принимает итог только по
актуальным результатам проверок и обязательного ревью.

## C++ Core Profile

- C++ enabled: yes; целевой стандарт C++20, расширения компилятора не использовать. Базовый C++17 в переносимых материалах не понижает стандарт проекта.
- Источник стандарта сейчас: раздел 3 архитектуры. Фактические target flags будут подтверждаться `CMakeLists.txt`, presets, compile/link output и CI после создания каркаса.
- C в собственных исходниках пока отсутствует; стандарт C не выбран. C API OBS не означает автоматического выбора стандарта C для C++ файлов.
- Целевой компилятор: MSVC, Windows x64. Точные поддерживаемые toolset/CRT/stdlib, Debug/Release ABI и комплект SDK должны быть закреплены вместе с OBS 33.0.1 до реализации/сборки.
- Найдено локально 08.11.2026: VS Build Tools 2022 17.3.32922.545, MSVC toolset 14.33.31629 (`cl.exe` 19.33.31630.0), Windows SDK headers 10.0.19041.0. Это инвентаризация, а не подтверждение совместимости с OBS/Qt.
- Exceptions/RTTI: build policy ещё не закреплена. Исключения не пересекают C/plugin callback boundary; allocation/free, borrowed handles, export/calling convention и время жизни соблюдать по контракту OBS.
- Целевой diagnostic provider: VS Code `ms-vscode.cpptools` + контекст CMake Tools. Установлены cpptools 1.35.6, CMake Tools 1.24.42 и Qt extensions; активный provider и его контекст не подтверждены.
- Прямой доступ агента к Problems/LSP и несохранённым буферам: unavailable. Сохранённые файлы и реальные compiler diagnostics являются доступной границей, когда появится сборка; чистоту IDE не заявлять.
- `clangd`, `clang-tidy`, Clazy на PATH не обнаружены; они не выбраны как обязательные gates. Не устанавливать и не переключать provider автоматически.
- Терминальный debugger: не настроен. Целевой путь отладки — `cppvsdbg`/CMake Tools; подключение к работающему OBS требует отдельной разрешённой задачи.
- Active pair: `cpp_core.md` + `cpp-core`.

## C++ Style Profile

- `.clang-format`, `.editorconfig` и существующего стиля C++ пока нет. При создании каркаса учитывать конфигурацию выбранного OBS Plugin Template и явно закрепить проектный стиль; не навязывать другой глобальный стиль заранее.
- Найден formatter LLVM 14.0.5 в `VC/Tools/Llvm/x64/bin/clang-format.exe` локальных Build Tools; formatter/version проекта пока не выбраны.
- Область форматирования — только затронутые собственные файлы/строки; generated/vendor не форматировать. Имена ABI, callbacks, Qt signals/properties и сериализованные поля сохранять.
- Active pair: `cpp_style.md` + `cpp-style`.

## C++ CMake Profile

- CMake выбран архитектурой, build files пока отсутствуют. Минимальная версия проекта не закреплена.
- На PATH: CMake/CTest 3.22.0 (`C:/Program Files/CMake/bin`). В Build Tools также найден комплект CMake и Ninja; они не являются выбранным build context.
- Генератор ещё не закреплён. Архитектурный ориентир — MSVC/Ninja и CMake Presets; preset `windows-x64`, CI и `buildspec.json` из разделов 16–17 пока только запланированы.
- OBS SDK/dependencies: официальный `obsproject/obs-plugintemplate`, адаптированный и закреплённый под OBS 33.0.1 и совместимый Qt. Наследовать зависимости OBS 31.1.1 из описанного в архитектуре шаблона без проверки нельзя.
- При реализации потребуются frontend API, Qt и Network; включение `ENABLE_FRONTEND_API`/`ENABLE_QT`, C++ entrypoint и link targets проверять в фактически выбранной ревизии шаблона.
- Compilation database отсутствует. Ninja/Makefiles могут создавать `compile_commands.json`; native Visual Studio generator не считать источником этой базы. До появления согласованного target/preset диагностический контекст unavailable.
- Configure/build не запускать по вымышленным preset. Каталоги сборки, загрузки зависимостей и install/deploy эффекты должны соответствовать реальной задаче и будущим закреплённым командам.
- Active pair: `cpp_cmake.md` + `cpp-cmake`.

## C++ Testing Profile

- Планируемая граница: CTest и Qt Test; существующих test targets, runner и версий framework пока нет. Новый сторонний framework автоматически не добавлять.
- Планируемые пути по архитектуре: `tests/unit/`, `tests/contract/`, `tests/integration/`; сейчас отсутствуют.
- Чистые FSM/DTO/SSE/счётчики и порядок команд проверять без настоящего OBS через интерфейсы/fixtures. GUI/loadable DLL и live-приёмка — отдельные проверки Windows + OBS 33.0.1 + изолированный backend.
- Реальный пользовательский OBS, эфир, сервер и настройки не использовать как неявную тестовую fixture.
- Targeted и broad test commands: пока отсутствуют. Создать и закрепить вместе с реальными targets/presets; отсутствие тестов не выдавать за успешную проверку.
- Sanitizers, coverage и benchmarks: не выбраны, порог coverage отсутствует. Применять только поддерживаемую и явно настроенную конфигурацию, без переносимых универсальных процентов.
- Active pair: `cpp_testing.md` + `cpp-testing`.

## Qt 6 Core Profile

- Qt enabled: yes, целевая major 6. Точная minor/patch, путь SDK и runtime ABI не подтверждены; установить их по совместимому комплекту OBS 33 до реализации и проверки version-sensitive API.
- Компоненты текущей архитектуры: Core, Widgets, Network; JSON через Qt Core. Test — для будущих тестов; Quick/Qml/QuickControls2/QuickTest не являются текущими runtime dependencies.
- Qt SDK не обнаружен на PATH и в проверенных `C:/Qt`, `D:/Qt`, `E:/Qt`; это не доказательство отсутствия SDK во всех других каталогах.
- Qt/CRT и загружаемые DLL должны быть совместимы с целевым OBS. Не упаковывать произвольный второй Qt runtime без проверки загрузки/ABI.
- QObject ownership явный; parent-child ownership и RAII не дублируют владение. OBS handles освобождать по их API, callbacks и queued deliveries не переживают владельца.
- Все network replies, таймеры и SSE работают асинхронно в известном Qt event loop. UI/OBS callbacks не блокируются сетевыми ожиданиями; запоздалые ответы старого поколения подключения не меняют новый StateStore.
- `moc/uic/rcc` и autogen — только через будущие CMake targets, вручную generated files не создавать.
- Clazy: unavailable, checks и gate не выбраны.
- Документация: Context7 при надёжном совпадении версии; иначе официальная version-matched документация Qt и ссылки активных навыков. Не заменять неизвестную версию на latest.
- Active pair: `qt_core.md` + `qt-core`.

## Qt Widgets Profile

- Widgets enabled: yes. Планируемые пути: `src/ui/`, `src/ui/pages/`; пока отсутствуют.
- UI состоит из `RestreamDock` и одного повторно используемого `ManagementWindow`; страницы и редакторы находятся внутри расширенного окна. Dock не содержит вкладок и форм настроек.
- Виджеты получают данные из общего `StateStore`, не выполняют HTTP сами. UI и GUI-facing models изменяются в GUI thread; workers используют безопасную queued передачу с проверенным временем жизни.
- Закрытие расширенного окна не завершает эфир, не удаляет Dock и не уничтожает общее состояние. Shutdown OBS снимает callbacks, отменяет принадлежащие плагину запросы/таймеры и соблюдает допустимую фазу OBS lifecycle.
- Формы `.ui`, autogen и model/view targets ещё не выбраны; generated outputs не редактировать.
- Active pair: `qt_widgets.md` + `qt-widgets`.

## Qt Testing Profile

- Qt Test/QTest, QSignalSpy, CTest: целевой выбор, модуль и runners ещё не установлены/не закреплены для проекта.
- Application fixture выбирать по границе: QCoreApplication для Core, QApplication для Widgets, QGuiApplication/Quick Test только при реальной QML-задаче.
- Проверять сигналы и аргументы, event-loop completion, ownership/teardown, model notifications и lifecycle окон согласно затронутым требованиям. Ожидания ограничены, без произвольных sleep как доказательства синхронизации.
- GUI display/offscreen policy и команды пока не настроены; offscreen не заменяет Windows DPI, загрузку DLL и проверку поведения внутри OBS.
- Active pair: `qt_testing.md` + `qt-testing`; обязательные базы: активные `cpp-testing` и `qt-core` вместе с их rules/skills.

## Qt Quick / QML Profile

- Материалы enabled: yes, явно выбраны владельцем для будущих задач.
- Реализация QML/Quick в текущей архитектуре отсутствует. SDK, runtime modules, URIs, source/import/resource paths и CMake registration пока не выбраны.
- Не создавать QML scaffolding и не переводить Widgets на Quick из-за активации профиля. При задаче с QML сначала закрепить совместимые версии, реальные imports/targets и границы C++/QML ownership, thread affinity, NOTIFY и lifecycle.
- `qmlls`, `qmllint`, `qmlformat`, `qmltestrunner` на PATH не обнаружены; проектные команды отсутствуют.
- QML Preview MCP, Qt Documentation MCP, Quick Test и profiler не настроены. Упоминание этих средств в references не разрешает установку/запуск.
- Active pair: `qt_quick_qml.md` + `qt-quick-qml`.

## Build And Validation Commands

Источник реальных команд — этот профиль и будущие CMake/preset/CI файлы.
Исполнение на Windows — PowerShell; для curl использовать `curl.exe`.
Не использовать CMD/POSIX синтаксис без подтверждённой отдельной среды.

| Проверка | Доступная команда / состояние |
| --- | --- |
| Версия CMake | `cmake --version` |
| Версия CTest | `ctest --version` |
| Версия Codex | `codex --version` |
| Git status | `git status --short` |
| Whitespace в отслеживаемом diff | `git diff --check` (не проверяет содержимое новых untracked files) |
| Install dependencies | none; SDK и runtime dependencies не устанавливаются этой интеграцией |
| Configure/build/packaging | none; CMake files и действующие presets отсутствуют |
| Format/lint/static analysis | none; проектные конфигурации и контекст ещё не выбраны |
| Unit/contract/GUI/integration/E2E | none; targets и изолированная среда ещё не созданы |
| Sanitizers/coverage/benchmarks | none; не настроены |
| QML tools/tests | none; нет модулей, targets и рабочего SDK |

При добавлении реального каркаса обновить этот профиль в том же изменении:
закрепить версии SDK/toolchain, пути и actual preset/target names, команды
узких и обязательных общих проверок, generated paths и разрешённые эффекты.
Не объявлять сборку/тесты пройденными до их фактического исполнения.

## Documentation And Integrations

- `context7_documentation.md` активен. Context7 доступен через существующее подключение родительской сессии; resolve/query проверены при внедрении. Ключи не копировать в документацию или versioned config.
- Сначала проектные версии/контракты, затем Context7 для нужной версии и темы; при ненадёжном совпадении — первичные официальные источники. Context7 не определяет фактическую установленную версию.
- Backend: `https://github.com/GregoryGost/gost-restreamer`, ветка `develop`. Архитектурный срез — дерево `0e7ea267e9a2cc0523022043793b4c074e76d8b8`, не автоматически текущий HEAD.
- Перед реализацией DTO и контрактных тестов сверять исходники/API contracts и свежую `/api/v1/openapi.json` либо YAML той же фактической сборки backend. Старый `openapi.yaml` из архитектуры не является источником актуальных DTO.
- Ссылки на официальные OBS/Qt/CMake источники и backend contracts приведены в разделе 22 архитектуры и references выбранных навыков. Новые версии не менять молча.
- GitHub/network используются для разрешённого чтения исходников и документации; профиль сам по себе не разрешает live API-команды, изменение backend, Git push, публикацию, установку плагина или управление эфиром.
- Obsidian, Jira, сторонние taskbook/wiki и server deployment не подключены.

## Dependency Policy

- Текущий источник зависимостей — архитектура; CMake/buildspec/lockfile ещё отсутствуют.
- Запрошенный импорт `agent-projects-tools` разрешён этой задачей. Добавление остальных runtime/development dependencies требует явной авторизации владельца.
- Будущий каркас использует официальный OBS Plugin Template с закреплёнными OBS 33/Qt зависимостями, а не произвольные latest SDK. Установка/upgrade локальных средств, расширений и смена глобальных defaults автоматически не разрешены.
- Проектные build/runtime workflows не исполняют scripts непосредственно из `.agents/skills/`; нужные assets становятся отдельно управляемой проектной копией только в соответствующей задаче.

## Product Contracts

Полные требования и критерии приёмки — `gost-obs-plugin-architecture.md`.
Перед изменением компонента читать полный соответствующий раздел и зависимости.

- Ровно два основных окна: Dock и одно расширенное окно. Единственная шестерёнка активирует существующий экземпляр; Widgets остаётся текущим UI.
- OBS владеет единственным штатным streaming output, encoding, RTMP Server и Stream Key. Плагин не меняет streaming service, не создаёт второй output/encoder, не обрабатывает кадры и не реализует FFmpeg/серверную SQLite.
- Управление — REST/SSE через Qt Network и общий StateStore. HTTP `202` означает принятие; успех требует терминальной квитанции и отдельно наблюдаемого состояния. Сохранять `request_id` при повторе потерянного запроса.
- Start из Dock ждёт успешную квитанцию и Session `RUNNING` до OBS Start. Pause/Resume относится к серверу; штатный OBS Stop/потеря SSE не завершают Session автоматически.
- Snapshot → SSE, opaque cursor/epoch, bounded buffers/backoff, отмена и защита от устаревших ответов. Не заменять отсутствие наблюдения нулём или успехом.
- Output restart и массовые операции — контролируемая последовательность отдельных `stop`/`start` с отдельными квитанциями и частичным результатом; серверных restart-all/reset endpoints не выдумывать.
- Сброс статистики — только локальный baseline с идентичностью/эпохой; OBS render/encode/network drops, server queue/delivery и ACK показатели не смешивать.
- Decimal `uint64`/наносекунды, `null`, write-only secrets, `expected_revision`/`409`, полная замена профиля и `encoding` v2 сохраняют серверный контракт. Готовое поколение fallback не равно готовой Session.
- Доверенная локальная сеть владельца, REST/SSE без добавления авторизации. TLS проверяется штатно; ключи, секретные endpoint и request payload с секретами в клиентский журнал не попадают.
- Изменения публичного контракта, UI или среды требуют обновления архитектуры и соответствующих проверок. Нативная сборка не заменяет реальную Windows/OBS/live-приёмку.

## Imported Materials

- Upstream: `https://github.com/GregoryGost/agent-projects-tools`.
- Revision: `ff4393f50c7d2e47cc03197e6e45ed087c89c954` (`master`, снимок от 08.11.2026).
- Переносимые rules/skills/roles подключены выборочно; `AGENTS.md` и общие правила адаптированы к целевому проекту.
- Локальный `.codex/config.toml` остаётся вне Git; `.codex/config.example.toml` содержит воспроизводимый пример без секретов.
