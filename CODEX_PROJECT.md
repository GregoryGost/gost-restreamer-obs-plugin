# Профиль проекта Codex

Профиль фиксирует стек, настройки, контракты и правила работы с проектом.
Задачи, ход реализации и результаты отдельных проверок ведутся в `TASKS.md`
и `TASKS_ARCHIVE.md`. Профиль обновляется при изменении проектных решений;
статусы задач и отчёты о сеансах в него не включаются.

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
- Название продукта: `GOST Restreamer`; модуль плагина: `gost-restreamer-obs-plugin`.
- Тип: нативный плагин OBS Studio, управляющий клиент отдельного сервера.
- Основной язык: C++20. Дополнительные форматы: CMake, JSON, INI, TOML; QML допускается для будущих явно запрошенных задач.
- Платформа: Windows 10/11 x64; целевая базовая версия OBS — 33.0.1.
- Источник требований: `gost-obs-plugin-architecture.md`. Архитектура описывает целевое поведение; наличие требования не подтверждает его реализацию или приёмку.

## Natural Language Profile

- Ответы пользователю: русский.
- Сообщения Git: русский; формат задаёт `git.md`.
- Собственная документация проекта: русский.
- Комментарии, пояснения рядом с кодом, документационные блоки, описания сценариев и проверок тестов: русский, грамматически цельные предложения.
- Идентификаторы кода и тестовых функций, имена API, полей, протоколов, библиотек, команд, путей и реальные контрактные значения сохранять на английском/в оригинале.
- Пользовательский интерфейс: русские подписи и локализация `en-US`, согласно архитектуре; продукт называется `GOST Restreamer`.
- Импортированные правила, навыки и инструкции ролей сохраняют язык upstream. Политика русского языка относится к собственной прозе проекта; переводить весь импортированный набор не требуется.
- `comment-language-audit` применять к затронутой прозе при соответствующих задачах; новые линтеры/тесты языка автоматически не добавлять.

## Validation Workspace Boundary

По указанию владельца все проверки выполняются только внутри
текущего workspace проекта.

- Для проверок не использовать системные и пользовательские AppData/ProgramData:
  ни чтение тестовых данных, ни запись, ни временное размещение плагина.
- Конфигурация OBS и плагина, тестовые профили, плагины, кеши, временные файлы,
  журналы и результаты проверок располагаются внутри workspace. `TEMP`/`TMP`
  тестовых дочерних процессов направляются в собственный каталог проекта.
- Настоящий OBS запускать только как portable-копию внутри workspace;
  конфигурация находится в её собственном `config/`, плагин — в `plugins/`.
- Не запускать `-Mode Normal` и помощники размещения/очистки ProgramData.
- Если проверку нельзя выполнить с такой изоляцией, не запускать её и отмечать
  соответствующий результат как непроверенный. Формат установочного каталога
  проверять на staging внутри проекта; это не подтверждает фактическую загрузку
  из системного пути.

Это ограничение применяется и к последующим задачам, и к проверкам выпуска.
Системные пути установки из архитектуры описывают поставку продукта и не
являются разрешением использовать их при проверках в этом workspace.

## Active Stack Profiles

- `cpp-core`
- `cpp-style`
- `cpp-cmake`
- `cpp-testing`
- `cpp-http-client`
- `qt-core`
- `qt-http-client`
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
- Качество комментариев: `code_comment_quality.md`.
- C++: `cpp_core.md`, `cpp_style.md`, `cpp_cmake.md`, `cpp_testing.md`, `cpp_http_client.md`.
- Qt 6: `qt_core.md`, `qt_http_client.md`, `qt_widgets.md`, `qt_testing.md`, `qt_quick_qml.md`.

Все дополнительные правила находятся в `.codex/rules/`; здесь использованы
точные имена файлов согласно activation contract upstream. Условные
bootstrap-правила проверки зависимостей и координации указаны выше отдельно.

## Active Skills

- `comment-language-audit`
- `code-comment-quality`
- `cpp-core`
- `cpp-style`
- `cpp-cmake`
- `cpp-testing`
- `cpp-http-client`
- `qt-core`
- `qt-http-client`
- `qt-widgets`
- `qt-testing`
- `qt-quick-qml`

Полные пакеты расположены в `.agents/skills/<skill-name>/`.

Пара `code_comment_quality.md` + `code-comment-quality` проверяет техническую
полноту и достоверность пояснений в затронутом коде: контракты, состояния,
владение ресурсами, порядок действий и причины неочевидных решений.
`comment-language-audit` отдельно проверяет русский язык пояснений.
Взаимная обязательная зависимость пары проверяется через
`.codex/rules/material_dependencies.md`.

## Agent Roles And Model Selection

- Subagents enabled: yes, для разрешённых изменений исполняемого кода приложения и тестов.
- Delegation scope: исполняемый код/тесты; задачи только по документации, комментариям, docstrings и инструкциям выполняет основной агент.
- Primary agent model/effort: `gpt-6.1-sol` / `xhigh`, согласно существующему локальному `.codex/config.toml`.
- Ограничение локальной конфигурации: 4 одновременно открытых потока субагентов, без основного потока.
- Формат: отдельные `.codex/agents/*.toml` с `name`, `description`, `developer_instructions`, моделью и effort. MCP и разрешения наследуются от родителя; инструкции read-only не создают отдельную техническую песочницу.
- Доступность моделей и загрузку ролей проверять в конкретной сессии Codex; TOML-валидация не доказывает доступность аккаунта. Недоступные роли/модели не заменять молча.
- Fresh thread policy: каждое назначение — новый `spawn_agent` без истории (`fork_turns: "none"` при подтверждённом MultiAgentV2). Не использовать `followup_task` и завершённый поток для нового назначения.
- Subagent handoff persistence: `local-ephemeral`; основной агент сохраняет проверенные отчёты и `index.md` в `.agent-handoffs/<task_scope_id>/` по `.codex/rules/subagent_coordination.md`, только когда текущий запрос разрешает эти записи.
- Автоматическое создание задач, заметок, worktree и Git commits не разрешено.

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
- C++20 закреплён в CMake (`LanguageStandard=stdcpp20` для MSVC targets).
- Импортированный `plugin-support.c.in` собирается как C17; собственные исходники плагина используют C++20.
- Компилятор: MSVC из Visual Studio Build Tools 2022, Windows x64; v143 и Windows SDK 10.0.26100.0 закреплены presets. RelWithDebInfo и Release используют `RuntimeLibrary=MultiThreadedDLL` (`/MD`). Отдельный preset Debug с debug CRT не заявляется.
- Exceptions: `/EHsc` (`ExceptionHandling=Sync`) в обеих выбранных конфигурациях; RTTI отдельно не переопределяется. Исключения не пересекают C/plugin callback boundary; allocation/free, borrowed handles, export/calling convention и время жизни соблюдать по контракту OBS.
- Целевой diagnostic provider: VS Code `ms-vscode.cpptools` + контекст CMake Tools. Настройки находятся в `.vscode/settings.json`, рекомендуемые расширения — в `.vscode/extensions.json`. Фактический контекст IDE проверять отдельно.
- Прямой доступ агента к Problems/LSP и несохранённым буферам: unavailable. Сохранённые файлы и реальные сообщения компилятора являются доступной границей диагностики; чистоту IDE не заявлять.
- `clangd`, `clang-tidy`, Clazy не выбраны как обязательные gates. Не устанавливать и не переключать provider автоматически.
- Терминальный debugger — CDB из Windows SDK. Целевой путь отладки редактора — `cppvsdbg`/CMake Tools; подключение к работающему OBS требует отдельной разрешённой задачи.
- Active pair: `cpp_core.md` + `cpp-core`.

## C++ Style Profile

- `.clang-format` задаёт `Standard: c++20`; `.editorconfig` — UTF-8, LF и отступы. Форматирование ограничено собственными затронутыми файлами.
- Выбран существующий clang-format 19.1.5: `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang-format.exe`.
- Область форматирования — только затронутые собственные файлы/строки; generated/vendor не форматировать. Имена ABI, callbacks, Qt signals/properties и сериализованные поля сохранять.
- Active pair: `cpp_style.md` + `cpp-style`.

## C++ CMake Profile

- Проект требует CMake 3.28 или новее.
- Выбран генератор `Visual Studio 17 2022`, x64, v143 и SDK 10.0.26100.0. Ninja не используется: Windows bootstrap шаблона зависит от VS platform и дочернего configure с `-A`.
- Configure preset `windows-x64`, build/test presets `windows-x64` (RelWithDebInfo) и `windows-x64-release` (Release), test preset `windows-x64-offline`; binary directory `build_x64/`. CI preset — `windows-ci-x64`; его наличие не подтверждает приёмку CI. Готовый каталог плагина обязательно обновлять в `build/install/` по порядку «Итоговая сборка в build/install» ниже.
- Основа CMake — официальный `obsproject/obs-plugintemplate`, ревизия `3e7d7ac3b5342cd7d9b88890b9c70b472d1520fc`. Зависимости выбираются по проектным `buildspec.json` и presets; версии из upstream-шаблона не наследуются автоматически.
- Windows-размещение следует формату OBS 33: `gost-restreamer-obs-plugin/gost-restreamer-obs-plugin.dll` и соседний `data/`. Структуры staging, portable-проверки и пакета должны быть согласованы. Загрузку и локализации проверять только в portable OBS внутри workspace с учётом `Validation Workspace Boundary`.
- Используются frontend API, Qt и Network; `ENABLE_FRONTEND_API`/`ENABLE_QT` включены.
- Native Visual Studio generator не создаёт `compile_commands.json`. В `.vscode/settings.json` выбран CMake Tools как provider для cpptools; фактический контекст IDE и Problems напрямую не проверены.
- Configure/build не запускать по вымышленным preset. Каталоги сборки, загрузки зависимостей и install/deploy эффекты должны соответствовать реальной задаче и будущим закреплённым командам.
- Active pair: `cpp_cmake.md` + `cpp-cmake`.

## C++ Testing Profile

- Граница: CTest и Qt Test 6.11.1. Targets `gost-restreamer-<name>-test`, где name: `lifecycle`, `backend-readonly`, `dto`, `sse`, `state`, `settings`, `network`, `connection`, `stats-baseline`; имена CTest — `gost-restreamer.<name>`.
- Тесты в `tests/unit/`, `tests/contract/`, `tests/integration/`, `tests/fixtures/`.
- Чистые FSM/DTO/SSE/счётчики и порядок команд проверять без настоящего OBS через интерфейсы/fixtures. GUI/loadable DLL и live-приёмка — отдельные проверки Windows + OBS 33.0.1 + изолированный backend.
- Реальный пользовательский OBS, эфир, сервер и настройки не использовать как неявную тестовую fixture.
- Файловая изоляция всех последующих проверок обязательна по `Validation Workspace Boundary`; AppData/ProgramData для них не используются.
- Команды: `ctest --preset windows-x64`, `ctest --preset windows-x64-release`, `ctest --preset windows-x64-offline`. Успех объявлять только после выполнения на актуальном снимке.
- Sanitizers, coverage и benchmarks: не выбраны, порог coverage отсутствует. Применять только поддерживаемую и явно настроенную конфигурацию, без переносимых универсальных процентов.
- Active pair: `cpp_testing.md` + `cpp-testing`.

## C++ HTTP Client Profile

- Транспорт исходящих REST/SSE-запросов — `Qt6::Network` 6.11.1 в `gost-restreamer-core`. Версию закрепляет `CMakeLists.txt`, источник SDK — `buildspec.json`.
- `APIClient` выполняет GET, `EventClient` подписывается на `/api/v1/events`. Закреплённый контракт backend и доверенная локальная сеть описаны ниже и в разделах 7, 8, 12 и 14 архитектуры. Клиент использует явно выбранный HTTP(S) origin, штатную проверку TLS и ручную политику редиректов.
- Для REST от отправки запроса действуют отдельные лимиты длительности (15 с) и простоя (10 с). Максимум 4 активных и 32 ожидающих запроса. Лимиты тела — 8 MiB для REST и 16 MiB для snapshot. Обход списка ограничен 100 страницами, 10 000 элементами и 64 MiB принятых страниц. При исчерпании общего бюджета сохраняются полученные элементы с признаком неполноты. Превышение предела отдельного ответа даёт ошибку.
- У `APIClient` нет общего механизма повторов. `ConnectionController` отдельно управляет восстановлением подключения, повторным snapshot и подпиской. Для команд сохранять контракт идемпотентности с тем же `request_id` при потере ответа.
- Правила раскрытия данных заданы в `Product Contracts` и разделах 14–15 архитектуры. Диагностика клиента не включает ключи, секретные endpoint и тела запросов с секретами.
- Active pair: `cpp_http_client.md` + `cpp-http-client`. Обязательная база — `cpp_core.md` + `cpp-core`.

## Qt 6 Core Profile

- Qt enabled: yes, целевая версия SDK и OBS runtime — 6.11.1. SDK берётся из официального пакета OBS `2026-10-23` в `.deps/obs-deps-qt6-2026-10-23-x64`; SHA256 закреплён в `buildspec.json`. Совместимость ABI проверять сборкой и загрузкой DLL.
- Компоненты: Core, Widgets, Network; JSON через Qt Core. Test используется только тестовыми targets; Quick/Qml/QuickControls2/QuickTest не являются текущими runtime dependencies.
- Qt SDK используется из `.deps`, без установки в систему. Официальный пакет OBS не содержит Qt Test. Для тестов используется официальный Qt Base 6.11.1 MSVC 2022 x64, SHA256 `7f97edc3937fec7383eb865e010ed5128155bf9c80a563abca450860f3e9bef5`; bootstrap закреплён в `cmake/windows/qttest.cmake`. Runtime этого комплекта применяется только тестовыми процессами через CTest environment; с плагином не устанавливается.
- Qt/CRT и загружаемые DLL должны быть совместимы с целевым OBS. Не упаковывать произвольный второй Qt runtime без проверки загрузки/ABI.
- QObject ownership явный; parent-child ownership и RAII не дублируют владение. OBS handles освобождать по их API, callbacks и queued deliveries не переживают владельца.
- Все network replies, таймеры и SSE работают асинхронно в известном Qt event loop. UI/OBS callbacks не блокируются сетевыми ожиданиями; запоздалые ответы старого поколения подключения не меняют новый StateStore.
- REST/SSE используют NoProxy только у собственных QNetworkAccessManager: соединение с явно выбранным локальным сервером не вызывает синхронный WinHTTP proxy lookup и не меняет proxy остальных частей OBS.
- `moc/uic/rcc` и autogen — через CMake targets, вручную generated files не создавать.
- Clazy: checks и gate не выбраны.
- Документация: Context7 при надёжном совпадении версии; иначе официальная version-matched документация Qt и ссылки активных навыков. Не заменять неизвестную версию на latest.
- Active pair: `qt_core.md` + `qt-core`.

## Qt HTTP Client Profile

- `PluginLifecycle::Runtime` владеет `APIClient`, `EventClient` и `ConnectionController`. Каждый сетевой клиент владеет своим `QNetworkAccessManager` с NoProxy и работает в GUI Qt event loop. Replies читаются по частям, отменяются при смене контекста или завершении и освобождаются через `deleteLater()` либо уничтожение владельца. Таймеры и callbacks учитывают поколение запроса и время жизни получателя.
- Используются `QNetworkAccessManager`, `QNetworkRequest` и `QNetworkReply`. Выбор протокола оставлен Qt. Обязательного HTTP/2 и отдельной проверки его согласования в проекте нет.
- SSE использует отдельные таймеры заголовков (15 с) и простоя (45 с) без общего срока жизни подписки. Лимиты чтения, кадра и очереди — 256 KiB, 1 MiB и 4 MiB / 256 событий. За оборот event loop обрабатывается до 32 событий и 256 KiB либо один крупный допустимый кадр.
- После установки snapshot подписка передаёт непрозрачный query `cursor`. В runtime-событии SSE `id` должен совпадать с `cursor` из JSON. Курсор подтверждается после применения события. При обрыве восстановление использует подтверждённый cursor, при `resync_required` или недостоверном продолжении нужен новый snapshot. Политика восстановления — `QTimer`, backoff 1–30 с с jitter и до трёх стартов за 10 с. См. разделы 8 и 12 архитектуры.
- Подключение подтверждается заголовками `200 text/event-stream`. При `400`/`409` на маршруте событий каждый повтор начинает новый snapshot. По умолчанию три повтора после первого отказа. Локальная настройка позволяет задать число или выбрать «Без ограничения» с теми же паузами и пределом частоты. После `204` и постоянного отказа автоматическое восстановление прекращается с причиной для UI.
- Изолированные проверки — `gost-restreamer.sse`, `gost-restreamer.network`, `gost-restreamer.connection`, `gost-restreamer.lifecycle` в preset `windows-x64-offline`. Они используют Qt Test и локальные fixtures. Приёмка в OBS проводится отдельно.
- Active pair: `qt_http_client.md` + `qt-http-client`. Обязательные базы — `cpp_http_client.md` + `cpp-http-client` и `qt_core.md` + `qt-core`.

## Qt Widgets Profile

- Widgets enabled: yes. UI находится в `src/ui/`; страницы размещаются внутри `ManagementWindow`.
- UI состоит из `RestreamDock` и одного повторно используемого `ManagementWindow`; страницы и редакторы находятся внутри расширенного окна. Dock не содержит вкладок и форм настроек.
- Dock следует текущей теме OBS и её варианту: фон, границы, текст и кнопки обновляются при смене оформления. Компоновка содержит отдельные блоки состояния, локальных OBS-метрик, серверного входа и выходов; оформление не фиксирует собственную тёмную тему.
- Виджеты получают данные из общего `StateStore`, не выполняют HTTP сами. UI и GUI-facing models изменяются в GUI thread; workers используют безопасную queued передачу с проверенным временем жизни.
- Закрытие расширенного окна не завершает эфир, не удаляет Dock и не уничтожает общее состояние. Shutdown OBS снимает callbacks, отменяет принадлежащие плагину запросы/таймеры и соблюдает допустимую фазу OBS lifecycle.
- Форм `.ui` и собственных model/view targets нет; сборка включает AUTOMOC. Generated outputs не редактировать.
- Active pair: `qt_widgets.md` + `qt-widgets`.

## Qt Testing Profile

- Qt Test/QTest 6.11.1, QSignalSpy и CTest закреплены в CMake. Фактический интерфейс OBS проверяется отдельно от offscreen.
- Application fixture выбирать по границе: QCoreApplication для Core, QApplication для Widgets, QGuiApplication/Quick Test только при реальной QML-задаче.
- Проверять сигналы и аргументы, event-loop completion, ownership/teardown, model notifications и lifecycle окон согласно затронутым требованиям. Ожидания ограничены, без произвольных sleep как доказательства синхронизации.
- Lifecycle-тест использует `QApplication` и `QT_QPA_PLATFORM=offscreen`, бюджет 30 секунд; backend-тест имеет бюджет 15 секунд, остальные — 120 секунд. TEMP/TMP и тестовые временные каталоги находятся в `build/` workspace. Offscreen не заменяет Windows DPI, загрузку DLL и проверку поведения внутри OBS.
- Active pair: `qt_testing.md` + `qt-testing`; обязательные базы: активные `cpp-testing` и `qt-core` вместе с их rules/skills.

## Qt Quick / QML Profile

- Материалы enabled: yes, явно выбраны владельцем для будущих задач.
- Реализация QML/Quick в текущей архитектуре отсутствует. SDK, runtime modules, URIs, source/import/resource paths и CMake registration пока не выбраны.
- Не создавать QML scaffolding и не переводить Widgets на Quick из-за активации профиля. При задаче с QML сначала закрепить совместимые версии, реальные imports/targets и границы C++/QML ownership, thread affinity, NOTIFY и lifecycle.
- `qmlls`, `qmllint`, `qmlformat`, `qmltestrunner` на PATH не обнаружены; проектные команды отсутствуют.
- QML Preview MCP, Qt Documentation MCP, Quick Test и profiler не настроены. Упоминание этих средств в references не разрешает установку/запуск.
- Active pair: `qt_quick_qml.md` + `qt-quick-qml`.

## Build And Validation Commands

Источник реальных команд — этот профиль и проектные CMake/preset/CI файлы.
Исполнение на Windows — PowerShell; для curl использовать `curl.exe`.
Не использовать CMD/POSIX синтаксис без подтверждённой отдельной среды.

| Проверка | Команда / конфигурация |
| --- | --- |
| Версия CMake | `cmake --version` |
| Версия CTest | `ctest --version` |
| Версия Codex | `codex --version` |
| Git status | `git status --short` |
| Whitespace в отслеживаемом diff | `git diff --check` (не проверяет содержимое новых untracked files) |
| Project-local SDK bootstrap | `cmake --preset windows-x64`; bootstrap загружает и проверяет закреплённые архивы в `.deps`, собирает SDK в этой же области; глобальной установки нет |
| Configure без тестовых targets | `cmake --preset windows-x64 -DBUILD_TESTING=OFF` |
| Build с символами / Release | `cmake --build --preset windows-x64`; `cmake --build --preset windows-x64-release` |
| Установка в staging | `cmake --install build_x64 --config RelWithDebInfo`; preset направляет результат в `build/install/` |
| Формат C++ | clang-format 19.1.5 `--dry-run --Werror` только для собственных затронутых `src` и `tests` |
| Локальные UI, lifecycle, HTTP/SSE | `ctest --preset windows-x64-offline` |
| Локальные и backend GET | `ctest --preset windows-x64`; `ctest --preset windows-x64-release` |
| PE/экспорты/раскладка | `tests/integration/Test-PluginArtifact.ps1 -PluginRoot <workspace-stage/plugin> -ExpectedBinary <workspace-DLL> -Dumpbin <dumpbin.exe>` |
| Изолированный OBS portable | `tests/integration/Test-PortableObs.ps1 -ObsRoot <fresh-workspace-OBS33> -Locale ru-RU` (отдельная свежая среда для `en-US`) |
| OBS normal / системное размещение | последующие запуски `-Mode Normal` и помощников ProgramData запрещены указанием владельца; использовать только portable и staging внутри workspace |
| Sanitizers/coverage/benchmarks | none; не настроены |
| QML tools/tests | none; нет модулей, targets и рабочего SDK |

При изменении стека, структуры или правил работы обновлять этот профиль в том же изменении:
закрепить версии SDK/toolchain, пути и actual preset/target names, команды
узких и обязательных общих проверок, generated paths и разрешённые эффекты.
Не объявлять сборку/тесты пройденными до их фактического исполнения.

Нативные UI-проверки требуют интерактивного рабочего стола и отдельной portable OBS.
Перед действиями проверяется принадлежность PID/path/start-time/HWND тестовому
экземпляру. Пользовательские настройки OBS не заменяют изолированную среду.
Offscreen-проверки, загрузка DLL, нативное поведение интерфейса и live-приёмка
имеют отдельные границы подтверждения.

### Итоговая сборка в build/install

После изменений кода плагина, его ресурсов или сборочной конфигурации перед
завершением задачи обязательно собрать актуальный плагин и обновить готовый
каталог `build/install/gost-restreamer-obs-plugin/`. Промежуточная DLL в
`build_x64/` сама по себе не завершает подготовку сборки для владельца.

По умолчанию использовать RelWithDebInfo. Из корня workspace в PowerShell
выполнить последовательно, переходя к следующему шагу только при успехе:

```powershell
cmake --build --preset windows-x64
cmake --install build_x64 --config RelWithDebInfo
```

Для новой среды сначала выполнить `cmake --preset windows-x64`.
При явно выбранном Release использовать `windows-x64-release` и устанавливать
с `--config Release`: сборка и установка должны относиться к одной конфигурации.
Проверить, что фактический install prefix остаётся внутри workspace в
`build/install/`; системное размещение плагина этим не разрешается.

После установки проверить совпадение SHA256 DLL с результатом выбранной
сборки и обеих локализаций с `data/locale/`, наличие доступного PDB и раскладку
OBS 33: DLL в корне каталога плагина, рядом `data/locale/`. Для проверки PE x64,
экспортов и точности файлов использовать `tests/integration/Test-PluginArtifact.ps1`
с `-PluginRoot build/install/gost-restreamer-obs-plugin`, `-ExpectedBinary`
выбранной сборки и явным путём `-Dumpbin`. В ответе владельцу указывать путь
к готовому каталогу. Если сборка, установка или проверка не прошла, явно сообщать
об отказе и не представлять прежнее содержимое `build/install/` как актуальное.

### Запуск OBS для нативных UI-проверок

Для следующих проверок использовать этот порядок запуска и подготовки фокуса.
Он дополняет `Validation Workspace Boundary` и применяется только в рамках
разрешённой пользователем проверки.

1. Подготовить отдельную portable OBS 33.0.1 внутри `build/test-workspace/`:
   собственные `config/`, `plugins/`, пустые профиль и коллекция `Isolated`,
   актуальная DLL и обе локализации. Для каждой локали — отдельная конфигурация.
   Базовый сценарий — `tests/integration/Test-PortableObs.ps1`; он требует
   свежую копию без `config/` и сам создаёт тестовые данные. Последующие проверки
   оформления могут использовать уже подготовленную собственную копию.
2. Проверить рабочий стол запускающего процесса, OBS и UIA-помощника.
   Для видимой проверки они должны работать на интерактивном `Default`.
   В этой среде обычный запуск команды из песочницы может попасть на отдельный
   `CodexSandboxDesktop-*`: окно там недоступно пользователю, ожидание ручного
   щелчка не решает проблему. В таком случае использовать предусмотренный средой
   разрешённый запуск вне песочницы (`sandbox_permissions: require_escalated`),
   сохраняя все файлы внутри workspace. Вызывать сценарий непосредственно
   оператором PowerShell `&` из этого запускающего процесса. Принадлежность
   desktop проверять через `GetThreadDesktop`/`GetUserObjectInformationW`,
   активный desktop — через `OpenInputDesktop`; не переключать desktop вручную.
3. Для видимой проверки запускать OBS через `ProcessStartInfo` с
   `UseShellExecute=false`, `WindowStyle=Normal`, рабочим каталогом `bin/64bit`
   и аргументами `--portable --multi --disable-updater
   --disable-missing-files-check --profile Isolated --collection Isolated`.
   `WindowStyle=Normal` задаёт видимость окна; режим проверки остаётся Portable.
   В текущем базовом runner задан `WindowStyle=Hidden`; видимый запуск был
   проверен отдельным помощником. Удалять из среды дочернего OBS переменные
   `OBS_PLUGINS_PATH`, `OBS_LEGACY_PLUGINS_PATH`, `OBS_LEGACY_PLUGINS_DATA_PATH`,
   `QT_PLUGIN_PATH`, `QT_QPA_PLATFORM`, `QT_QPA_PLATFORM_PLUGIN_PATH`.
   `TEMP`/`TMP` OBS и помощника направлять в собственный каталог workspace.
4. Готовность подтверждать новым журналом текущего запуска: `Portable mode:
   true`, загрузка GOST Restreamer, `Startup complete` и единственный собственный
   главный HWND. Зафиксировать PID, точный путь EXE, время старта и HWND.
   Перед каждым действием повторять проверки процесса/окна; для UIA-элементов
   проверять `ProcessId`. Сверять путь и SHA256 реально загруженной DLL,
   а также Qt Core/Gui/Widgets 6.11.1 из этой portable-копии.
5. UI Automation выполнять отдельным скрытым Windows PowerShell 5.1
   (`powershell.exe -NoLogo -NoProfile -NonInteractive -Mta -EncodedCommand`),
   передавая идентичность OBS явно. Основные проверки принадлежности и фокуса
   находятся в `tests/integration/Test-ObsUi.ps1`. Перед действиями меню
   проверять `Assert-OwnedElement $main`, вызывать `$main.SetFocus()`, затем
   `Prepare-OwnedMainForeground` и требовать `Verified=true`, подтверждённый
   через `GetForegroundWindow`. Одного результата `SetForegroundWindow`
   недостаточно. При отказе остановить UI-действия и сохранить диагностику.
   Подготовку фокуса главного OBS отделять от проверки активации окна управления:
   после шестерёнки его активация должна наблюдаться без принудительного фокуса.
6. Настройки OBS искать среди потомков собственного главного окна; использовать
   полный `AutomationId` и доступные UIA patterns. Если `SetFocus()` элемента
   не изменил фактический фокус, перепроверить его и foreground. В проверенном
   сценарии навигация и выбор варианта оформления выполнялись адресными
   `WM_KEYDOWN`/`WM_KEYUP` собственного Settings HWND, включая Tab до списка
   вариантов; перед клавишами подтверждались процесс, окно и текущий фокус.
   Глобальные клавиши и клики, присоединение к чужой очереди ввода не применять.
   Исходный вариант оформления восстановить; после Cancel дождаться закрытия
   настроек перед завершением OBS.
7. Снимать только собственный Dock через `PrintWindow`, сохранять PNG/SHA256
   и фактический `GetDpiForWindow`. В результатах отдельно проверять успех
   помощника и сценария, foreground, штатное завершение OBS и отсутствие
   оставшихся собственных процессов. Один exit code запускающего сценария
   не доказывает успех: вспомогательный runner может записать `Error` в JSON
   и завершиться с кодом 0. Закрывать только повторно подтверждённый собственный
   HWND через `WM_CLOSE`; принудительная очистка после отказа не считается PASS.

## Documentation And Integrations

- `context7_documentation.md` активен. Context7 используется через настроенное MCP-подключение; доступность проверять в текущей сессии. Ключи не копировать в документацию или versioned config.
- Сначала проектные версии/контракты, затем Context7 для нужной версии и темы; при ненадёжном совпадении — первичные официальные источники. Context7 не определяет фактическую установленную версию.
- Backend: `https://github.com/GregoryGost/gost-restreamer`, ветка `develop`. Клиентский контракт API закреплён за commit `103d839b11591fbb786fd027aefa143a2ff79036`. Версию установленного сервера устанавливать отдельно; `info.version=1.0.0` обозначает версию API, а не commit сборки.
- Тестовый backend владельца: `http://192.168.88.8:8080`. В рамках разрешённой проверки допускается автоматическое чтение трёх маршрутов `/api/v1/health/live`, `/api/v1/health/ready`, `/api/v1/system/capabilities`, без отдельной команды/подтверждения. Presets задают URL только тестовым процессам; продуктовый адрес вводится на странице настроек. Запуски эфиров, изменения backend и серверное развёртывание этим не разрешены.
- Перед реализацией DTO и контрактных тестов сверять исходники/API contracts и актуальную `/api/v1/openapi.json` либо YAML той же фактической сборки backend.
- Контракт API возвращает строковые `publication_key`, `stream_key`, полный `endpoint` в конфигурационных POST/GET/PATCH/списках и исторических снимках Session/Output. Отсутствующее значение несовместимого ответа явно недоступно, а null/неверный тип — ошибка контракта. Снимок OpenAPI: `tests/fixtures/backend/openapi.json`, SHA256 `9F6C45C1A8C1EEF0B3CD9BD1457AAE31EE0366AE096740F404640A5746EAA50C`; происхождение — соседний `provenance.json`. Backend в этом репозитории не изменяется.
- Ссылки на официальные OBS/Qt/CMake источники и backend contracts приведены в разделе 22 архитектуры и references выбранных навыков. Новые версии не менять молча.
- GitHub/network используются для разрешённого чтения исходников и документации; профиль сам по себе не разрешает live API-команды, изменение backend, Git push, публикацию, установку плагина или управление эфиром.
- Obsidian, Jira, сторонние taskbook/wiki и server deployment не подключены.

## Dependency Policy

- Источники зависимостей — `buildspec.json`, `cmake/windows/qttest.cmake` и архитектура. Официальные архивы закреплены по версии и SHA256.
- Добавление runtime/development dependencies требует явной авторизации владельца.
- Каркас использует официальный OBS Plugin Template с закреплёнными OBS 33/Qt зависимостями. Установка/upgrade локальных средств, расширений и смена глобальных defaults автоматически не разрешены.
- Проектные build/runtime workflows не исполняют scripts непосредственно из `.agents/skills/`; нужные assets становятся отдельно управляемой проектной копией только в соответствующей задаче.

## Product Contracts

Полные требования и критерии приёмки — `gost-obs-plugin-architecture.md`.
Перед изменением компонента читать полный соответствующий раздел и зависимости.

- Ровно два основных окна: Dock и одно расширенное окно. Единственная шестерёнка активирует существующий экземпляр; Widgets остаётся текущим UI.
- Выбранный Stream хранится отдельно для каждой пары «сервер + OBS Profile». Переименование профиля меняет только отображаемое имя и сохраняет все его привязки, включая восстановление после перезапуска OBS; одного отображаемого имени недостаточно для идентичности профиля.
- OBS владеет единственным штатным streaming output, encoding, RTMP Server и Stream Key. Плагин не меняет streaming service, не создаёт второй output/encoder, не обрабатывает кадры и не реализует FFmpeg/серверную SQLite. Карточка «Потоки» должна предоставлять серверный `publication_key` для ручной вставки в OBS; карточка «Выходы» — `stream_key` и полный `endpoint` Destination.
- Все три значения изначально маскированы; глаз показывает/скрывает, копирование берёт точное значение без предварительного раскрытия. Смена сущности и повторное открытие карточки возвращают маску. Доступ сохраняется для прежних записей после перезапуска OBS, источник истины — сервер. Если чтение недоступно, показывать это явно, без фиктивного значения или локальной постоянной копии.
- Управление — REST/SSE через Qt Network и общий StateStore. HTTP `202` означает принятие; успех требует терминальной квитанции и отдельно наблюдаемого состояния. Сохранять `request_id` при повторе потерянного запроса.
- Start из Dock ждёт успешную квитанцию и Session `RUNNING` до OBS Start. Pause/Resume относится к серверу; штатный OBS Stop/потеря SSE не завершают Session автоматически.
- Snapshot → SSE, opaque cursor/epoch, bounded buffers/backoff, отмена и защита от устаревших ответов. Не заменять отсутствие наблюдения нулём или успехом.
- Output restart и массовые операции — контролируемая последовательность отдельных `stop`/`start` с отдельными квитанциями и частичным результатом; серверных restart-all/reset endpoints не выдумывать.
- Сброс статистики — только локальный baseline с идентичностью/эпохой; OBS render/encode/network drops, server queue/delivery и ACK показатели не смешивать.
- Decimal `uint64`/наносекунды, отсутствующее поле/`null`, `expected_revision`/`409`, полная замена профиля и `encoding` v2 сохраняют серверный контракт. Отсутствие секретного поля в текущем GET не заменяет значение флагом `*_configured`; PATCH других полей сохраняет ключ/адрес без их явной замены.
- `profile.live_audio_mode` задаёт LIVE AAC независимо от видео `mode`, вне `encoding` v2. Новый профиль по умолчанию использует AAC `copy`; видео `copy` допускает только AAC `copy`. Отсутствующее поле старого профиля сохраняет наследуемую семантику, не превращается автоматически в новое значение по умолчанию; явный режим передаётся при каждой последующей полной замене. Изменения действуют на новое исполнение, не на закреплённый Output.
- Fallback всегда содержит цифровую AAC-тишину. `audio_encoding`/`audio_encoder`/`audio_custom` относятся к заглушке и к LIVE только при AAC `transcode`; при `copy` фактический AAC OBS должен быть совместим, автоматического transcode нет. Старый `READY` до генератора `fallback-fmp4-silent-v2` требует rebuild до эфира; ошибка rebuild не делает его пригодным. Публичный DTO не содержит версии рецепта. Готовое поколение не равно готовой Session.
- Доверенная локальная сеть владельца, REST/SSE без добавления авторизации. TLS проверяется штатно; ключи, секретные endpoint и request payload с секретами в клиентский журнал не попадают.
- Изменения публичного контракта, UI или среды требуют обновления архитектуры и соответствующих проверок. Нативная сборка не заменяет реальную Windows/OBS/live-приёмку.

## Imported Materials

- Upstream: `https://github.com/GregoryGost/agent-projects-tools`.
- Revision: `6a6245a97603c87a7c0f96653fb5bf963fcd5fe7` (`v1.19.0`).
- Переносимые rules/skills/roles подключены выборочно; `AGENTS.md` и общие правила адаптированы к целевому проекту.
- Локальный `.codex/config.toml` остаётся вне Git; `.codex/config.example.toml` содержит воспроизводимый пример без секретов.
