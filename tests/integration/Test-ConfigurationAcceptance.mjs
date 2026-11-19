/** Проверяет конфигурацию через настоящую portable OBS и изолированный dev.8.
 * Prepare копирует только runtime без config; Freeze обновляет хэши после утверждения снимка; Run и Resume требуют неизменный freeze.json.
 * Секреты сравниваются в RAM, отчёт содержит только признаки и хэши.
 */
// fs читает собственные планы/результаты, path ограничивает пути workspace, crypto вычисляет контрольные суммы.
import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
// isDeepStrictEqual сравнивает документы без зависимости от порядка JSON-полей.
import { isDeepStrictEqual } from 'node:util';
// delay даёт короткую паузу между ограниченными проверками фактического удаления.
import { setTimeout as delay } from 'node:timers/promises';
// spawn запускает только установленный PowerShell с уже существующим native runner; fileURLToPath определяет корень.
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
// root — репозиторий; fixture — эксклюзивная среда; runner — общий нативный runner; phase задаёт Prepare, Freeze, Run либо Resume.
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const fixture = path.join(root, 'build/test-workspace/configuration-final-native');
const runner = path.join(root, 'tests/integration/Test-ConfigurationNative.ps1');
const phase = process.argv[2];
// inputs содержит замораживаемые входы; checks хранит только подтверждённые результаты; entities — точные собственные IDs.
const inputs = ['tests/integration/Test-ConfigurationAcceptance.mjs', 'tests/integration/Test-ConfigurationNative.ps1', 'tests/integration/Invoke-ConfigurationNativeUi.ps1', 'tests/integration/configuration-fault-proxy.mjs', 'tests/integration/Test-Dev8ProfileNative.ps1', 'tests/integration/Test-Dev8ProfileUi.ps1', 'tests/fixtures/WorkspacePaths.ps1', 'tests/fixtures/backend/profile-presets.json', 'build/test-workspace/configuration-final-native/obs-ru-RU/bin/64bit/obs64.exe', 'build/test-workspace/configuration-final-native/obs-ru-RU/bin/64bit/Qt6Core.dll', 'build/test-workspace/configuration-final-native/obs-ru-RU/bin/64bit/Qt6Gui.dll', 'build/test-workspace/configuration-final-native/obs-ru-RU/bin/64bit/Qt6Widgets.dll', 'build/test-workspace/configuration-final-native/obs-ru-RU/plugins/gost-restreamer-obs-plugin/data/locale/ru-RU.ini', 'build/install/gost-restreamer-obs-plugin/gost-restreamer-obs-plugin.dll'];
const checks = [];
const entities = [];
// visitedKinds отличает первую пустую форму страницы от последующих созданий из сохранённой непустой карточки.
const visitedKinds = new Set();
// cleanupFailures сохраняет независимые от функциональных проверок отказы освобождения ресурсов.
const cleanupFailures = [];
// sequence выдаёт уникальные имена планов; running фиксирует попытку Start для обязательного Stop.
let sequence = 0;
let running = false;
// managementOpen отражает только подтверждённые open/hide текущего процесса, исключая лишнее меню при видимом окне.
let managementOpen = false;
/** Возвращает SHA256 точных UTF-8 строк либо Buffer value, не сохраняя исходное значение. */
function hash(value) { return crypto.createHash('sha256').update(value).digest('hex'); }
/** Проверяет condition по независимому ожиданию message; при отказе прекращает прогон. */
function requireThat(condition, message) { if (!condition) throw new Error(message); }
/** Фиксирует только проверенный сценарий name и безопасные evidence в итоговой матрице. */
function passed(name, evidence = {}) { checks.push({ name, status: 'PASS', ...evidence }); }
/** Читает или изменяет только разрешённый loopback origin по route; method/body задают явную операцию.
 * expect ограничивает допустимые статусы; ошибочное тело никогда не включается в исключение.
 */
async function api(route, { port = 18080, method = 'GET', body, expect = 200 } = {}) {
  requireThat([18080, 18081, 18082].includes(port) && route.startsWith('/'), 'Запрещённый адрес API.');
  // response принадлежит одному ограниченному запросу; text используется только в RAM.
  const response = await fetch('http://127.0.0.1:' + port + route, { method, headers: body ? { 'content-type': 'application/json' } : {}, body: body ? JSON.stringify(body) : undefined, redirect: 'error', signal: AbortSignal.timeout(10000) });
  const text = await response.text();
  requireThat(response.status === expect, method + ' ' + route.split('?')[0] + ': HTTP ' + response.status);
  return text ? JSON.parse(text) : null;
}
/** Возвращает детальную запись kind/id прямо с backend, минуя faults. */
async function detail(kind, id) { return api('/api/v1/' + kind + '/' + id); }
/** Устанавливает rules только на принадлежащем прогону proxy18081, предварительно освобождая удержания. */
async function faults(rules = []) { return api('/__fixture/control', { port: 18081, method: 'POST', body: { clear: true, rules } }); }
/** Снимает хэши всех inputs; file обозначает один вход, result содержит только путь и SHA256. */
async function manifest() {
  const result = {};
  for (const file of inputs) result[file] = hash(await fs.readFile(path.join(root, file)));
  return result;
}
/** Запускает существующий runner в mode с необязательным plan; вывод сохраняется локально без публикации.
 * child принадлежит прогону и принудительно завершается при превышении 150 секунд; код выхода проверяется отдельно.
 */
async function native(mode, plan) {
  if (mode === 'Start' || mode === 'Stop') managementOpen = false;
  // command содержит только argv без shell; child — собственный короткоживущий runner; stdout/stderr накапливают его диагностику.
  const command = ['-NoProfile', '-File', runner, '-Mode', mode, '-FixtureRoot', fixture, ...(plan ? ['-PlanPath', plan] : [])];
  const child = spawn('C:/Program Files/PowerShell/7/pwsh.exe', command, { cwd: root, windowsHide: true, env: { ...process.env, TEMP: path.join(fixture, 'tmp'), TMP: path.join(fixture, 'tmp') }, stdio: ['ignore', 'pipe', 'pipe'] });
  let stdout = '';
  let stderr = '';
  // Обработчики получают chunk — очередную порцию соответствующего канала; канал может наследоваться запущенным OBS.
  // timer прерывает зависший runner без поиска процессов по имени.
  child.stdout.on('data', (chunk) => { stdout += chunk; });
  child.stderr.on('data', (chunk) => { stderr += chunk; });
  const timer = setTimeout(() => child.kill(), 150000);
  try {
    // code завершается по exit процесса runner: stdout может оставаться открытым у запущенного OBS; resolve/reject обслуживают только child.
    const code = await new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
    await fs.writeFile(path.join(fixture, 'commands', String(++sequence) + '-' + mode + '.json'), JSON.stringify({ argv: command, code, stdout, stderr }, null, 2));
    requireThat(code === 0, 'Отказ runner ' + mode + '; диагностика сохранена локально.');
  } finally { clearTimeout(timer); }
}
/** Выполняет steps как отдельный UI-план label и независимо проверяет отчёт; check в callback обозначает один завершённый шаг. */
async function ui(label, steps) {
  // requested сохраняет исходную последовательность; expectedOpen вычисляет видимость после успешного плана; step — одно действие.
  const requested = steps;
  let expectedOpen = managementOpen;
  steps = [];
  for (const step of requested) {
    if (step.action === 'open' && expectedOpen) continue;
    steps.push(step);
    if (step.action === 'open') expectedOpen = true;
    if (step.action === 'hide') expectedOpen = false;
  }
  // plan — собственный план; report — фактический отчёт helper с тем же именем.
  const plan = path.join(fixture, 'commands', String(++sequence) + '-' + label + '.json');
  await fs.writeFile(plan, JSON.stringify({ steps }, null, 2));
  await native('Ui', plan);
  const report = JSON.parse(await fs.readFile(path.join(fixture, 'results', path.basename(plan, '.json') + '.result.json'), 'utf8'));
  requireThat(report.Passed === true && report.Checks.length === steps.length && report.Checks.every((check) => check.status === 'PASS'), 'Неполный результат UI ' + label);
  managementOpen = expectedOpen;
  return report;
}
/** Возвращает шаг выбора страницы page и точной карточки name. */
function openCard(page, name) { return [{ action: 'open' }, { action: 'page', name: page }, { action: 'row', name }]; }
/** Возвращает проверку серверного подтверждения, обязательную после успешного Save. */
function saved() { return { action: 'assert', id: 'configurationMessage', type: 'Text', property: 'name', value: 'Сервер подтвердил сохранение конфигурации.' }; }
/** Возвращает утверждение свойства property контрола id; extra уточняет повторённые поля. */
function assertion(id, property, value, extra = {}) { return { action: 'assert', id, property, value, ...extra }; }
/** В Run создаёт запись kind через native страницу page с name и steps, затем независимо читает новый ID/revision.
 * В Resume перечитывает точный собственный ID из архивного Run и проверяет исходные name/revision, не повторяя создание.
 * item в фильтре — новая запись списка, old — одна исходная запись для исключения прежних IDs. */
async function create(kind, page, name, steps) {
  if (phase === 'Resume') {
    // previous ограничивает продолжение собственными IDs сохранённого прогона; entity — одна запись этого перечня.
    const previous = JSON.parse(await fs.readFile(path.join(fixture, 'attempt4-results/Run-summary.json'), 'utf8'));
    for (const entity of previous.entities) {
      if (entity.kind !== kind) continue;
      // record читается заново, поэтому продолжение не использует сохранённые тела или секреты.
      const record = await detail(kind, entity.id);
      if (record.name !== name) continue;
      requireThat(name.startsWith('Acceptance ') && record.revision === 1, 'Изменилась собственная начальная запись продолжения.');
      entities.push(entity);
      passed('Повторное чтение ' + kind, { id: record.id, revision: record.revision });
      return record;
    }
    throw new Error('Не найден точный собственный ID продолжения.');
  }
  // before содержит существующие IDs, after — полный список после подтверждения; matches находит только новое точное имя.
  const before = await api('/api/v1/' + kind);
  requireThat(!before.next_cursor, 'Начальный список неполон.');
  // creation подтверждает реальный переход непустой карточки к новой; первая активация страницы уже создаёт пустой draft.
  const creation = visitedKinds.has(kind) ? [{ action: 'invoke', name: 'Создать', type: 'Button' }] : [assertion('configurationName', 'value', '', { type: 'Edit' })];
  await ui('create-' + kind, [{ action: 'open' }, { action: 'page', name: page }, ...creation, { action: 'set', id: 'configurationName', value: name }, ...steps, { action: 'invoke', id: 'configurationSave', type: 'Button' }, saved()]);
  const after = await api('/api/v1/' + kind);
  const matches = after.items.filter((item) => item.name === name && !before.items.some((old) => old.id === item.id));
  requireThat(!after.next_cursor && matches.length === 1 && matches[0].revision === 1, 'Не подтверждено создание точного нового ID.');
  visitedKinds.add(kind);
  entities.push({ kind, page, id: matches[0].id });
  passed('Создание ' + kind, { id: matches[0].id, revision: 1 });
  return detail(kind, matches[0].id);
}
/** Переименовывает record из kind/page, проверяя рост ревизии и отсутствие секретных/profile полей PATCH.
 * entry в фильтре обозначает безопасные метаданные одного запроса proxy. */
async function rename(kind, page, record, name) {
  await ui('rename-' + kind, [...openCard(page, record.name), { action: 'set', id: 'configurationName', value: name }, { action: 'invoke', id: 'configurationSave', type: 'Button' }, saved()]);
  // next — независимый readback; status — безопасная история proxy, mutation — последняя запись точного ID.
  const next = await detail(kind, record.id);
  const status = await api('/__fixture/status', { port: 18081 });
  const mutation = status.history.filter((entry) => entry.method === 'PATCH' && entry.path === '/api/v1/' + kind + '/' + record.id).at(-1);
  requireThat(next.name === name && next.id === record.id && next.revision === record.revision + 1 && JSON.stringify(mutation?.fields) === JSON.stringify(['expected_revision', 'name']), 'Переименование нарушило ревизию или частичный PATCH.');
  passed('Переименование ' + kind, { id: next.id, revision: next.revision });
  return next;
}
/** Создаёт пустую изолированную fixture, копируя только разрешённые каталоги runtime; существующая конфигурация запрещена. */
async function prepare() {
  await fs.mkdir(fixture, { recursive: true });
  // directory обозначает только собственные каталоги результатов и временных файлов.
  for (const directory of ['results', 'tmp', 'commands', 'obs-ru-RU']) await fs.mkdir(path.join(fixture, directory), { recursive: true });
  // callback catch возвращает null при отсутствии конфигурации; имеющаяся конфигурация всегда запрещает Prepare.
  requireThat(!(await fs.stat(path.join(fixture, 'obs-ru-RU/config')).catch(() => null)), 'Prepare не перезаписывает существующий config.');
  // directory выбирает только runtime, исключая прежние серверные адреса и привязки.
  for (const directory of ['bin', 'core', 'data', 'plugins']) await fs.cp(path.join(root, 'build/test-workspace/obs-readonly-52b863c8/obs-ru-RU', directory), path.join(fixture, 'obs-ru-RU', directory), { recursive: true, force: false, errorOnExist: true });
  await fs.writeFile(path.join(fixture, 'freeze.json'), JSON.stringify(await manifest(), null, 2));
}
/** Проверяет CRUD, ключи, каталог до Session, реальный CAS и потерянное подтверждение на замороженном снимке. */
async function run() {
  // frozen фиксирует утверждённые хэши; presets предоставляет независимые документы ожидаемых профилей.
  const frozen = JSON.parse(await fs.readFile(path.join(fixture, 'freeze.json'), 'utf8'));
  const presets = JSON.parse(await fs.readFile(path.join(root, 'tests/fixtures/backend/profile-presets.json'), 'utf8'));
  requireThat(JSON.stringify(await manifest()) === JSON.stringify(frozen), 'Снимок изменился после freeze.');
  await faults();
  running = true;
  try {
    await native('Start');
    // stream хранит собственный Stream, profileRecords/groups/destinations содержат только собственные связанные записи.
    let stream = await create('streams', 'Потоки', 'Acceptance Main', [{ action: 'invoke', id: 'generatePublicationKey', type: 'Button' }]);
    requireThat(typeof stream.publication_key === 'string' && stream.publication_key.length > 20, 'Не прочитан сгенерированный publication_key.');
    const profileRecords = [];
    const groups = [];
    const destinations = [];
    // preset — один из трёх независимых эталонов владельца; record — созданный полный профиль.
    for (const preset of presets.items) {
      const record = await create('transcode-profiles', 'Профили', 'Acceptance ' + preset.name, [{ action: 'combo', id: 'profilePreset', value: preset.name }]);
      requireThat(isDeepStrictEqual(record.profile, preset.profile), 'Профиль не совпал с независимым эталоном.');
      profileRecords.push(record);
    }
    // index задаёт соответствующие Group/Profile/Destination; platforms содержит точные публичные подписи сервисов.
    const platforms = ['YouTube', 'Twitch', 'VK Видео Live'];
    for (let index = 0; index < 3; index++) {
      // group содержит созданную серверную группу и её проверяемые ссылки на Stream/Profile.
      const group = await create('transcode-groups', 'Группы кодирования', 'Acceptance Group ' + index, [{ action: 'combo', id: 'configurationStream', value: stream.name }, { action: 'combo', id: 'configurationProfile', value: profileRecords[index].name }]);
      requireThat(group.stream_id === stream.id && group.profile_id === profileRecords[index].id, 'Неверная связь Group.');
      groups.push(group);
      // destination содержит новое выключенное назначение с синтетическим endpoint/key и ссылкой на group.
      const destination = await create('destinations', 'Выходы', 'Acceptance Destination ' + index, [{ action: 'combo', id: 'configurationStream', value: stream.name }, { action: 'combo', id: 'configurationGroup', value: group.name }, { action: 'combo', id: 'configurationPlatform', value: platforms[index] }, { action: 'type', id: 'secretReceived', type: 'Edit', count: 2, index: 0, value: 'rtmp://127.0.0.1:19350/live/' + index }, { action: 'type', id: 'secretReceived', type: 'Edit', count: 2, index: 1, value: 'acceptance-key-' + index }]);
      requireThat(destination.stream_id === stream.id && destination.group_id === group.id && destination.enabled === false && destination.endpoint === 'rtmp://127.0.0.1:19350/live/' + index && destination.stream_key === 'acceptance-key-' + index, 'Неверная связь/секрет/disabled Destination.');
      destinations.push(destination);
    }
    // record в map обозначает одно собственное назначение, чьё имя должно присутствовать в каталоге.
    if (phase !== 'Resume') await ui('catalog', [{ action: 'dock' }, { action: 'combo', id: 'streamSelector', value: stream.name, scope: 'main' }, { action: 'catalog', value: destinations.map((record) => record.name) }]);
    if (phase !== 'Resume') passed('Каталог выключенных назначений до Session');
    await ui('stream-secret', [...openCard('Потоки', stream.name), assertion('secretReceived', 'password', true, { type: 'Edit' }), { action: 'copy', id: 'secretCopy', type: 'Button', sha256: hash(stream.publication_key) }, { action: 'invoke', id: 'secretEye', type: 'Button' }, assertion('secretReceived', 'password', false, { type: 'Edit' }), { action: 'hide' }, { action: 'open' }, assertion('secretReceived', 'password', true, { type: 'Edit' })]);
    stream = await rename('streams', 'Потоки', stream, 'Acceptance Main Renamed');
    // destination/baseKey/baseEndpoint фиксируют исходные секреты только в RAM для проверки частичного PATCH.
    let destination = destinations[0];
    const baseKey = destination.stream_key;
    const baseEndpoint = destination.endpoint;
    await ui('destination-secret', [...openCard('Выходы', destination.name), assertion('secretReceived', 'password', true, { type: 'Edit', count: 2, index: 0 }), assertion('secretReceived', 'password', true, { type: 'Edit', count: 2, index: 1 }), { action: 'copy', id: 'secretCopy', type: 'Button', count: 2, index: 0, sha256: hash(baseEndpoint) }, { action: 'copy', id: 'secretCopy', type: 'Button', count: 2, index: 1, sha256: hash(baseKey) }]);
    destination = await rename('destinations', 'Выходы', destination, 'Acceptance Destination Renamed');
    requireThat(destination.stream_key === baseKey && destination.endpoint === baseEndpoint, 'Rename изменил секреты.');
    await native('Stop'); running = false;
    running = true; await native('Start');
    await ui('restart-secrets', [...openCard('Потоки', stream.name), assertion('secretReceived', 'password', true, { type: 'Edit' }), { action: 'copy', id: 'secretCopy', type: 'Button', sha256: hash(stream.publication_key) }, ...openCard('Выходы', destination.name), assertion('secretReceived', 'password', true, { type: 'Edit', count: 2, index: 1 }), { action: 'copy', id: 'secretCopy', type: 'Button', count: 2, index: 1, sha256: hash(baseKey) }]);
    passed('Маска, точное копирование, переоткрытие и restart секретов');
    await ui('clear-key', [{ action: 'type', id: 'secretReceived', type: 'Edit', count: 2, index: 1, clearLength: baseKey.length, value: '' }, { action: 'invoke', id: 'configurationSave', type: 'Button' }, saved()]);
    destination = await detail('destinations', destination.id);
    requireThat(destination.stream_key === '' && destination.endpoint === baseEndpoint, 'Явная пустота не очистила только ключ.');
    passed('Явная очистка ключа');
    // baseName/draftName/serverName различают три версии реального конфликта; server — результат независимого PATCH.
    const baseName = destination.name;
    const draftName = 'Acceptance CAS Draft';
    const serverName = 'Acceptance CAS Server';
    await ui('cas-draft', [...openCard('Выходы', baseName), { action: 'set', id: 'configurationName', value: draftName }]);
    const server = await api('/api/v1/destinations/' + destination.id, { method: 'PATCH', body: { expected_revision: destination.revision, name: serverName } });
    requireThat(server.revision === destination.revision + 1, 'Независимый CAS не поднял ревизию.');
    // conflictReport содержит видимое сравнение, требуемое контрактом трёх версий.
    const conflictReport = await ui('cas-conflict', [{ action: 'invoke', id: 'configurationSave', type: 'Button' }, { action: 'assert', name: 'Подтвердить изменения на новой ревизии', type: 'Button', property: 'enabled', value: true }, { action: 'tree' }]);
    // name перебирает ожидаемые версии, node — видимые элементы таблицы сравнения.
    requireThat([baseName, draftName, serverName].every((name) => conflictReport.Checks.at(-1).observation.some((node) => node.name === name)), 'Не показаны три различные версии конфликта.');
    requireThat((await detail('destinations', destination.id)).name === serverName, 'Конфликт перезаписал сервер без подтверждения.');
    // conflictHistory доказывает настоящий HTTP409, не локальную имитацию UI.
    const conflictHistory = await api('/__fixture/status', { port: 18081 });
    // entry — безопасная запись истории, подтверждающая адресный конфликт backend.
    requireThat(conflictHistory.history.some((entry) => entry.method === 'PATCH' && entry.path.endsWith('/' + destination.id) && entry.upstream_status === 409), 'Не подтверждён настоящий revision_conflict.');
    await ui('cas-confirm', [{ action: 'invoke', name: 'Подтвердить изменения на новой ревизии', type: 'Button', dialogAppear: 'QMessageBox' }, { action: 'invoke', name: 'Да', type: 'Button', dialog: 'QMessageBox' }, saved()]);
    destination = await detail('destinations', destination.id);
    requireThat(destination.name === draftName && destination.revision === server.revision + 1, 'Подтверждённый CAS не сохранил черновик на новой ревизии.');
    passed('Реальный CAS409 и явное подтверждение');
    // route адресует единственную потерянную запись; lostName позволяет доказать коммит прямым readback.
    const route = '/api/v1/destinations/' + destination.id;
    const lostName = 'Acceptance Lost Confirmation';
    await ui('lost-draft', [...openCard('Выходы', destination.name), { action: 'set', id: 'configurationName', value: lostName }]);
    await faults([{ id: 'hold-write', method: 'PATCH', path: route, action: 'hold_success', hold_ms: 30000 }, { id: 'fail-reconcile', method: 'GET', path: route, action: 'problem', status: 503, code: 'fixture_unavailable', once: false }]);
    // beforeLost фиксирует счётчики до Save; patchCountBefore исключает незамеченный автоматический повтор во время timeout.
    const beforeLost = await api('/__fixture/status', { port: 18081 });
    const patchCountBefore = beforeLost.counts['PATCH ' + route] || 0;
    await ui('lost-save', [{ action: 'invoke', id: 'configurationSave', type: 'Button' }, { action: 'dialog', id: 'operationErrorDialog' }, assertion('operationErrorReason', 'name', 'Сервер не подтвердил сохранение до истечения времени ожидания.', { type: 'Text', dialog: 'operationErrorDialog' }), { action: 'invoke', name: 'Закрыть', type: 'Button', dialog: 'operationErrorDialog' }, assertion('configurationSave', 'enabled', false, { type: 'Button' })]); 
    // lostStatus фиксирует число попыток; committed доказывает реальный коммит при потерянном ответе.
    const lostStatus = await api('/__fixture/status', { port: 18081 });
    requireThat(lostStatus.counts['PATCH ' + route] === patchCountBefore + 1, 'Save либо timeout повторил PATCH.');
    const committed = await detail('destinations', destination.id);
    // entry в проверке истории подтверждает завершённый upstream200 с удержанным ответом.
    requireThat(committed.name === lostName && committed.revision === destination.revision + 1 && lostStatus.history.some((entry) => entry.path === route && entry.outcome === 'committed_response_held' && entry.upstream_status === 200), 'Не подтверждён коммит потерянного ответа.');
    // beforeRefresh задаёт новую границу наблюдения; refreshDeadline ограничивает ожидание завершённого адресного GET503.
    const beforeRefresh = await api('/__fixture/status', { port: 18081 });
    const refreshDeadline = Date.now() + 5000;
    // failedRefresh подтверждается только новым завершённым запросом; refreshStatus хранит безопасную историю proxy.
    let failedRefresh = false;
    let refreshStatus;
    await ui('failed-refresh', [{ action: 'invoke', id: 'configurationRefresh', type: 'Button' }]);
    do {
      refreshStatus = await api('/__fixture/status', { port: 18081 });
      // entry проверяет точный маршрут и новый id относительно снимка перед явным Refresh.
      failedRefresh = refreshStatus.history.some((entry) => entry.id > (beforeRefresh.history.at(-1)?.id || 0) && entry.method === 'GET' && entry.path === route && entry.status === 503 && entry.outcome === 'injected_problem');
      if (failedRefresh) break;
      await delay(100);
    } while (Date.now() < refreshDeadline);
    requireThat(failedRefresh && refreshStatus.counts['GET ' + route] > (beforeRefresh.counts['GET ' + route] || 0), 'Не подтверждён новый завершённый GET503 явного Refresh.');
    await ui('barrier-after-failed-refresh', [assertion('configurationMessage', 'name', 'Не удалось прочитать конфигурацию. Сохранённый ввод остаётся доступен.', { type: 'Text' }), assertion('configurationRefresh', 'enabled', true, { type: 'Button' }), assertion('configurationSave', 'enabled', false, { type: 'Button' }), { action: 'hide' }, { action: 'open' }, assertion('configurationSave', 'enabled', false, { type: 'Button' })]);
    await faults();
    await ui('reconcile', [{ action: 'invoke', id: 'configurationRefresh', type: 'Button' }, assertion('configurationName', 'value', lostName, { type: 'Edit' }), { action: 'set', id: 'configurationName', value: 'Acceptance Reconciled' }, assertion('configurationSave', 'enabled', true, { type: 'Button' })]);
    requireThat((await api('/__fixture/status', { port: 18081 })).counts['PATCH ' + route] === patchCountBefore + 1, 'Сверка автоматически повторила запись.');
    // Перезапуск завершает проверку RAM-барьера; новый Save на старой base может законно потребовать CAS.
    await native('Stop'); running = false;
    running = true; await native('Start');
    passed('Потеря подтверждения, failed refresh, reopen и снятие барьера только после сверки');
    // record обозначает ранее созданную Group/Profile; переименование проверяет UPDATE всех типов CRUD.
    await rename('transcode-groups', 'Группы кодирования', groups[0], 'Acceptance Group Renamed');
    await rename('transcode-profiles', 'Профили', profileRecords[0], 'Acceptance Profile Renamed');
    // entity проходит обратный порядок зависимостей; record — свежая серверная ревизия для native удаления.
    for (const entity of [...entities].reverse()) {
      const record = await detail(entity.kind, entity.id);
      await ui('delete-' + entity.kind, [...openCard(entity.page, record.name), { action: 'invoke', name: 'Удалить', type: 'Button', dialogAppear: 'QMessageBox' }, { action: 'invoke', name: 'Да', type: 'Button', dialog: 'QMessageBox' }]);
      // deadline ограничивает ожидание асинхронного DELETE, response содержит только статус повторного GET.
      const deadline = Date.now() + 5000;
      let response;
      do {
        response = await fetch('http://127.0.0.1:18080/api/v1/' + entity.kind + '/' + entity.id, { redirect: 'error', signal: AbortSignal.timeout(3000) });
        await response.arrayBuffer();
        if (response.status === 404) break;
        requireThat(response.status === 200, 'Неожиданный статус сверки удаления.');
        await delay(100);
      } while (Date.now() < deadline);
      requireThat(response.status === 404, 'Не подтверждено удаление точного ID.');
      passed('Удаление ' + entity.kind, { id: entity.id });
      entities.pop();
    }
    requireThat(JSON.stringify(await manifest()) === JSON.stringify(frozen), 'Входы изменились во время проверки.');
  } finally {
    // error в каждом catch описывает отдельный отказ очистки; отказ proxy не пропускает остановку OBS.
    try { await faults(); } catch (error) { cleanupFailures.push('Снятие faults: ' + error.message); }
    try { if (running) { await native('Stop'); running = false; } } catch (error) { cleanupFailures.push('Остановка собственного OBS: ' + error.message); }
    if (cleanupFailures.length) process.exitCode = 1;
  }
}
// error сохраняется только как безопасное сообщение; summary не содержит успешных тел/секретов.
try {
  if (phase === 'Prepare') await prepare();
  else if (phase === 'Freeze') await fs.writeFile(path.join(fixture, 'freeze.json'), JSON.stringify(await manifest(), null, 2));
  else if (phase === 'Run' || phase === 'Resume') await run();
  else throw new Error('Требуется Prepare, Freeze, Run либо Resume.');
} catch (error) {
  checks.push({ name: 'Остановка прогона', status: 'FAIL', error: error.message });
  process.exitCode = 1;
} finally {
  // summary фиксирует только выполненную матрицу; оставшиеся собственные backend IDs сохраняются для адресной очистки.
  const summary = { phase, architectureGate: 'NOT_REQUIRED', decisionVersion: 'NOT_REQUIRED', testingStatus: process.exitCode || phase === 'Resume' ? 'NOT_ACCEPTED' : phase === 'Run' ? 'ACCEPTED' : 'NOT_STARTED', priorEvidence: phase === 'Resume' ? 'attempt4-results/Run-summary.json: исходный отказ навигации остаётся неклассифицированным' : null, checks, entities, cleanupFailures, ownObsRunning: running, untested: ['Неизвестный профиль и полная замена после repair', 'Числовые черновики и точные CRF/QP', 'Привязки OBS Profile/origin и настройки требуют отдельной проверки', 'Live/hardware матрица не запускалась'] };
  await fs.writeFile(path.join(fixture, 'results', phase + '-summary.json'), JSON.stringify(summary, null, 2));
  process.stdout.write(JSON.stringify({ phase, status: summary.testingStatus, checks: checks.length, ownObsRunning: running }) + '\n');
}
