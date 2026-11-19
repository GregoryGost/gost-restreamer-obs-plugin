/** Управляемая локальная HTTP/SSE-оснастка. Тела штатного обмена не сериализуются заново. */
// http передаёт байты HTTP, fs проверяет каталог журналов, path разрешает пути, fileURLToPath определяет workspace.
import http from 'node:http';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

/** Проверяет origin value: разрешён только HTTP на числовом loopback с явным портом. */
function checkedOrigin(value) {
  // origin содержит разобранный адрес, но не используется для переадресации по входному Host.
  const origin = new URL(value);
  if (!/^http:\/\/127\.0\.0\.1:[0-9]+\/?$/.test(value) || origin.username || origin.password
      || !origin.port || origin.pathname !== '/' || origin.search || origin.hash) {
    throw new Error('Разрешён только http://127.0.0.1:<порт>.');
  }
  return origin;
}

/** Возвращает безопасные HTTP-заголовки headers без заголовков отдельного соединения. */
function forwardedHeaders(headers) {
  // result содержит копию заголовков; name перебирает запрещённые заголовки соединения.
  const result = { ...headers };
  for (const name of ['connection', 'keep-alive', 'proxy-authenticate', 'proxy-authorization', 'te', 'trailer', 'transfer-encoding', 'upgrade', ...(headers.connection || '').split(',')]) {
    delete result[name.trim().toLowerCase()];
  }
  return result;
}

/** Читает ограниченное тело stream; limit задаёт предел в байтах, превышение отклоняется. */
async function readBody(stream, limit = 8 * 1024 * 1024) {
  // chunks накапливает исходные байты, size считает их суммарный объём, chunk содержит очередную порцию.
  const chunks = [];
  let size = 0;
  for await (const chunk of stream) {
    size += chunk.length;
    if (size > limit) throw new Error('Превышен предел тела оснастки.');
    chunks.push(chunk);
  }
  return Buffer.concat(chunks);
}

/** Отправляет value как служебный JSON-ответ с HTTP-кодом status через response. */
function sendJson(response, status, value) {
  if (response.destroyed || response.writableEnded) return;
  response.writeHead(status, { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' });
  response.end(JSON.stringify(value));
}

/** Заменяет только существующий JSON-токен по pointer исходного text на raw; соседние числа сохраняются посимвольно. */
function replaceToken(text, pointer, raw) {
  // parts содержит декодированные сегменты JSON Pointer; offset отмечает текущий символ, found — найденный диапазон.
  const parts = pointer.slice(1).split('/').map(
    // part содержит один сегмент указателя; callback раскрывает стандартные escape-последовательности.
    (part) => part.replaceAll('~1', '/').replaceAll('~0', '~'));
  let offset = 0;
  let found;
  /** Пропускает пробелы в текущем исходном JSON. */
  function whitespace() { while (/\s/.test(text[offset] || '') && offset < text.length) offset++; }
  /** Читает строковый токен с escape-последовательностями и возвращает его строковое значение. */
  function stringToken() {
    // start сохраняет начало строки для разбора только её строкового значения.
    const start = offset++;
    while (offset < text.length) {
      if (text[offset] === '\\') offset += 2;
      else if (text[offset++] === '"') return JSON.parse(text.slice(start, offset));
    }
    throw new Error('Незавершённая строка JSON.');
  }
  /** Обходит значение по location, сохраняя диапазон совпавшего указателя без числового преобразования. */
  function value(location) {
    whitespace();
    // start отмечает начало значения, opener — вид контейнера, index — индекс элемента массива.
    const start = offset;
    const opener = text[offset];
    let index = 0;
    if (opener === '{' || opener === '[') {
      offset++;
      whitespace();
      while (text[offset] !== (opener === '{' ? '}' : ']')) {
        // key содержит строковый ключ объекта либо индекс массива для следующего уровня указателя.
        const key = opener === '{' ? stringToken() : String(index++);
        if (opener === '{') { whitespace(); if (text[offset++] !== ':') throw new Error('Ожидалось двоеточие JSON.'); }
        value([...location, key]);
        whitespace();
        if (text[offset] !== ',') break;
        offset++;
        whitespace();
      }
      if (text[offset++] !== (opener === '{' ? '}' : ']')) throw new Error('Незавершённый контейнер JSON.');
    } else if (opener === '"') stringToken();
    else {
      while (offset < text.length && !/[\s,}\]]/.test(text[offset])) offset++;
      if (offset === start) throw new Error('Пустое значение JSON.');
    }
    if (location.length === parts.length && location.every(
      // segment — сегмент текущего пути, position — его индекс; callback проверяет точное совпадение.
      (segment, position) => segment === parts[position])) found = [start, offset];
  }
  // Полный разбор только проверяет корректность. Сериализация его результата не выполняется.
  JSON.parse(text);
  value([]);
  if (!found) throw new Error('Указанное поле отсутствует в ответе.');
  return text.slice(0, found[0]) + raw + text.slice(found[1]);
}

/** Проверяет входное правило candidate и создаёт его рабочую копию без изменения состояния при ошибке. */
function checkedRule(candidate) {
  if (!candidate || typeof candidate !== 'object') throw new Error('Ожидался объект правила.');
  if (typeof candidate.id !== 'string' || !/^[a-zA-Z0-9_-]{1,64}$/.test(candidate.id)) throw new Error('Недопустимый id правила.');
  if (!['GET', 'POST', 'PATCH', 'PUT', 'DELETE'].includes(candidate.method)) throw new Error('Недопустимый method.');
  if (typeof candidate.path !== 'string' || !candidate.path.startsWith('/api/') || /[?#\r\n]/.test(candidate.path)
      || new URL(candidate.path, 'http://127.0.0.1').pathname !== candidate.path) throw new Error('Требуется точный pathname /api/.');
  if (!['hold_success', 'problem', 'patch_json'].includes(candidate.action)) throw new Error('Неизвестный action.');
  if (candidate.once !== undefined && typeof candidate.once !== 'boolean') throw new Error('once должен быть boolean.');
  if (candidate.action === 'hold_success' && (candidate.method === 'GET' || candidate.once === false)) throw new Error('hold_success применим однократно к записи.');
  if (candidate.action === 'hold_success' && candidate.hold_ms !== undefined
      && (!Number.isInteger(candidate.hold_ms) || candidate.hold_ms < 1 || candidate.hold_ms > 300000)) throw new Error('hold_ms вне 1..300000.');
  if (candidate.action === 'problem' && (!Number.isInteger(candidate.status) || candidate.status < 400 || candidate.status > 599
      || typeof candidate.code !== 'string' || !/^[a-zA-Z0-9_-]{1,100}$/.test(candidate.code))) throw new Error('problem требует status 400..599 и code.');
  if (candidate.action === 'patch_json') {
    if (candidate.method !== 'GET' || !Array.isArray(candidate.patches) || !candidate.patches.length || candidate.patches.length > 16) throw new Error('patch_json требует GET и 1..16 patches.');
    // patch — одна замена существующего поля; числовые значения остаются исходным JSON-текстом.
    for (const patch of candidate.patches) {
      if (!patch || typeof patch.pointer !== 'string' || !patch.pointer.startsWith('/') || patch.pointer.length > 512
          || typeof patch.raw !== 'string' || patch.raw.length > 65536) throw new Error('Замена требует pointer и raw JSON.');
      JSON.parse(patch.raw);
    }
  }
  // once задаёт однократность; hits считает срабатывания, remaining хранит остаток либо null для постоянного правила.
  return { ...candidate, once: candidate.once !== false, hits: 0, remaining: candidate.once === false ? null : 1 };
}

/** Создаёт исключительно локальный proxy: port — порт слушателя, upstream — разрешённый origin, resultRoot — существующий каталог workspace. */
export async function createFaultProxy({ port, upstream, resultRoot }) {
  if (!Number.isInteger(port) || port < 0 || port > 65535) throw new Error('Недопустимый порт слушателя.');
  // origin — проверенный адрес назначения; workspace — канонический корень репозитория, output — канонический каталог журналов.
  const origin = checkedOrigin(upstream);
  const workspace = await fs.realpath(fileURLToPath(new URL('../../', import.meta.url)));
  const output = await fs.realpath(resultRoot);
  if (output !== workspace && !output.startsWith(workspace + path.sep)) throw new Error('result-root должен находиться внутри workspace.');
  // rules хранит активные правила, history — безопасные последние запросы, counts — счётчики всех запросов по методу и пути.
  let rules = [];
  const history = [];
  const counts = new Map();
  // sockets, outgoing и timers принадлежат этому proxy; held содержит уже завершённые upstream-ответы, ожидающие разрыва.
  const sockets = new Set();
  const outgoing = new Set();
  const timers = new Set();
  const held = new Map();
  // sequence выдаёт номера запросов; closing не допускает повторного завершения; logChain сериализует безопасные записи на диск.
  let sequence = 0;
  let closing = false;
  let logChain = Promise.resolve();
  // journal получает имя после назначения фактического порта слушателя.
  let journal;

  /** Записывает безопасную копию entry; тела, query, Host и значения полей в журнал не попадают. */
  function log(entry) {
    logChain = logChain.then(
      // callback дописывает только метаданные завершённого запроса.
      () => fs.appendFile(journal, JSON.stringify(entry) + '\n')).catch(
      // callback отражает отказ журнала без вывода тела или системного пути.
      () => { process.stderr.write('Не удалось записать метаданные proxy.\n'); });
  }

  /** Формирует безопасное состояние, не раскрывая raw-замены или тела запросов. */
  function status() {
    // port — порт listener; upstream — фиксированный origin; closing — признак завершения; rules — безопасные описания правил;
    // counts содержит счётчики по маршрутам, history — последние метаданные, held — число удерживаемых ответов.
    return {
      port: server.address()?.port, upstream: origin.origin, closing,
      rules: rules.map(
        // rule содержит рабочее состояние; callback исключает значения подменяемых полей.
        (rule) => ({ id: rule.id, method: rule.method, path: rule.path, action: rule.action, hits: rule.hits, remaining: rule.remaining })),
      counts: Object.fromEntries(counts), history, held: held.size,
    };
  }

  /** Освобождает уже удерживаемые ответы. response, timer перебирают принадлежащие proxy соединения и сроки. */
  function releaseHeld() {
    for (const [response, timer] of held) { clearTimeout(timer); timers.delete(timer); response.destroy(); }
    held.clear();
  }

  /** Обслуживает request через response; управляющие запросы не отправляются backend и не включаются в историю продукта. */
  async function handle(request, response) {
    // pathname используется для точного выбора правила; адрес назначения никогда не берётся из запроса.
    const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
    if (!request.url.startsWith('/') || request.url.startsWith('//')) { sendJson(response, 400, { code: 'invalid_target' }); return; }
    if (pathname === '/__fixture/status' && request.method === 'GET') { sendJson(response, 200, status()); return; }
    if (pathname === '/__fixture/control' && request.method === 'POST') {
      try {
        // control содержит только команду управления; nextRules полностью проверяется до изменения текущих правил.
        const control = JSON.parse((await readBody(request, 1024 * 1024)).toString('utf8'));
        const nextRules = control.rules === undefined ? undefined : control.rules.map(checkedRule);
        if (nextRules && new Set(nextRules.map(
          // rule даёт id для проверки уникальности в одном наборе.
          (rule) => rule.id)).size !== nextRules.length) throw new Error('Повторяющиеся id правил.');
        if (control.clear) { rules = []; releaseHeld(); }
        if (control.reset_history) { history.length = 0; counts.clear(); }
        if (nextRules) rules = nextRules;
        sendJson(response, 200, status());
      } catch {
        sendJson(response, 400, { code: 'invalid_control', detail: 'Проверьте схему правил оснастки.' });
      }
      return;
    }
    if (pathname.startsWith('/__fixture/')) { sendJson(response, 404, { code: 'fixture_route_not_found' }); return; }
    // key объединяет только метод и pathname; entry содержит метаданные одного запроса без его значений.
    const key = request.method + ' ' + pathname;
    // Поля entry: id — номер; method/path — точная цель; count — число запросов этой цели; fields — имена полей тела;
    // status — код клиентского ответа; upstream_status — код backend; outcome — способ завершения, без значений payload.
    const entry = { id: ++sequence, method: request.method, path: pathname, count: (counts.get(key) || 0) + 1, fields: [], status: null, upstream_status: null, outcome: 'pending' };
    counts.set(key, entry.count);
    history.push(entry);
    if (history.length > 2000) history.shift();
    // finalized предотвращает повторную запись при finish и close одного ответа.
    let finalized = false;
    /** Завершает метаданные ровно один раз после доставки либо закрытия клиентского соединения. */
    function finalize() { if (!finalized) { finalized = true; if (entry.outcome === 'pending') entry.outcome = response.writableFinished ? 'delivered' : 'client_closed'; log({ ...entry }); } }
    response.once('finish', finalize);
    response.once('close', finalize);
    // body хранит исходные байты, parsed используется только для имён верхних полей, rule фиксирует выбранный сбой до отправки upstream.
    const body = await readBody(request);
    try {
      // parsed не сериализуется обратно в обмен и не сохраняется в журнале.
      const parsed = body.length ? JSON.parse(body.toString('utf8')) : null;
      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) entry.fields = Object.keys(parsed).sort();
    } catch { /* Не-JSON тело передаётся исходными байтами. */ }
    const rule = rules.find(
      // candidate сопоставляется только по методу и pathname, без приблизительных маршрутов.
      (candidate) => candidate.method === request.method && candidate.path === pathname && candidate.remaining !== 0);
    if (rule) { rule.hits++; if (rule.remaining !== null) rule.remaining--; }
    if (rule?.action === 'problem') {
      entry.status = rule.status;
      entry.outcome = 'injected_problem';
      response.writeHead(rule.status, { 'content-type': 'application/problem+json', 'cache-control': 'no-store' });
      response.end(JSON.stringify({ type: 'about:blank', title: 'Проверочный отказ операции', status: rule.status, code: rule.code, detail: 'Операция отклонена тестовой оснасткой.' }));
      return;
    }
    // headers исключает соединительные поля, но сохраняет content-type и исходную длину тела.
    const headers = forwardedHeaders(request.headers);
    headers.host = origin.host;
    headers['accept-encoding'] = 'identity';
    // upstreamRequest принадлежит proxy; callback получает upstreamResponse и либо передаёт поток, либо применяет выбранный сбой.
    const upstreamRequest = http.request({ hostname: '127.0.0.1', port: Number(origin.port), method: request.method, path: request.url, headers }, async (upstreamResponse) => {
      entry.upstream_status = upstreamResponse.statusCode;
      try {
        if (rule?.action === 'hold_success' && upstreamResponse.statusCode >= 200 && upstreamResponse.statusCode < 300) {
          upstreamResponse.resume();
          // callback начинает удержание лишь после полного получения успешного ответа backend.
          upstreamResponse.once('end', () => {
            if (response.destroyed) return;
            entry.outcome = 'committed_response_held';
            // timer разрывает потерянный ответ по ограниченному сроку; клиентский timeout обычно наступает раньше.
            const timer = setTimeout(
              // callback удаляет собственный таймер и закрывает только соответствующий клиентский ответ.
              () => { timers.delete(timer); held.delete(response); response.destroy(); }, rule.hold_ms ?? 30000);
            timers.add(timer);
            held.set(response, timer);
            // callback освобождает срок, если клиент самостоятельно завершил ожидание.
            response.once('close', () => { clearTimeout(timer); timers.delete(timer); held.delete(response); });
          });
          // callback фиксирует неполный upstream-ответ как транспортный отказ, а не подтверждённую запись.
          upstreamResponse.once('error', () => { entry.outcome = 'upstream_error'; response.destroy(); });
          return;
        }
        // responseHeaders передаёт штатные заголовки без привязки к соединению upstream.
        const responseHeaders = forwardedHeaders(upstreamResponse.headers);
        if (rule?.action === 'patch_json' && upstreamResponse.statusCode >= 200 && upstreamResponse.statusCode < 300) {
          // text содержит исходный UTF-8 JSON; patch последовательно заменяет только указанные поля.
          let text = (await readBody(upstreamResponse, 16 * 1024 * 1024)).toString('utf8');
          for (const patch of rule.patches) text = replaceToken(text, patch.pointer, patch.raw);
          delete responseHeaders['content-length'];
          delete responseHeaders.etag;
          delete responseHeaders['content-md5'];
          entry.status = upstreamResponse.statusCode;
          entry.outcome = 'patched_fields';
          response.writeHead(upstreamResponse.statusCode, responseHeaders);
          response.end(text);
          return;
        }
        entry.status = upstreamResponse.statusCode;
        response.writeHead(upstreamResponse.statusCode, responseHeaders);
        response.flushHeaders();
        upstreamResponse.pipe(response);
        // callback завершает клиентский поток, если upstream оборвался до конца тела.
        upstreamResponse.once('error', () => { entry.outcome = 'upstream_error'; response.destroy(); });
      } catch {
        entry.status = 502;
        entry.outcome = 'fixture_response_error';
        if (response.headersSent) response.destroy(); else sendJson(response, 502, { code: 'fixture_response_error' });
      }
    });
    outgoing.add(upstreamRequest);
    // callback удаляет завершённый исходящий запрос из набора ресурсов proxy.
    upstreamRequest.once('close', () => outgoing.delete(upstreamRequest));
    // callback отражает транспортный отказ без содержимого запроса или системной диагностики.
    upstreamRequest.once('error', () => { entry.status = 502; entry.outcome = 'upstream_error'; if (response.headersSent) response.destroy(); else sendJson(response, 502, { code: 'fixture_upstream_error' }); });
    // callback отменяет собственный upstream-поток при закрытии клиента, включая постоянный SSE.
    response.once('close', () => upstreamRequest.destroy());
    upstreamRequest.end(body);
  }

  // server принимает соединения только на 127.0.0.1; callback перехватывает ошибки асинхронного обработчика.
  const server = http.createServer((request, response) => {
    handle(request, response).catch(
      // callback завершает запрос при повреждённом или слишком большом теле без раскрытия его содержимого.
      () => { if (response.headersSent) response.destroy(); else sendJson(response, 400, { code: 'fixture_request_error' }); });
  });
  server.requestTimeout = 30000;
  // callback отслеживает socket; вложенный callback удаляет его после закрытия.
  server.on('connection', (socket) => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); });
  // listenPromise завершается после открытия порта либо ошибки; resolve/reject получают стандартные результаты события.
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(port, '127.0.0.1', resolve); });
  journal = path.join(output, 'configuration-proxy-' + server.address().port + '.jsonl');

  /** Закрывает только listener, таймеры и соединения этого экземпляра, затем дожидается записи метаданных. */
  async function close() {
    if (closing) return;
    closing = true;
    releaseHeld();
    // timer, request и socket перебирают исключительно ресурсы, созданные этим экземпляром.
    for (const timer of timers) clearTimeout(timer);
    for (const request of outgoing) request.destroy();
    // closedPromise ожидает подтверждение закрытия listener; resolve вызывается его штатным callback.
    const closedPromise = new Promise((resolve) => server.close(resolve));
    for (const socket of sockets) socket.destroy();
    await closedPromise;
    await logChain;
  }
  // Возвращаемые port, close и status позволяют владельцу адресовать listener, штатно завершить его и прочитать метаданные.
  return { port: server.address().port, close, status };
}

/** Читает CLI args с обязательными port, upstream и result-root; неизвестные и повторные ключи отклоняет. */
function parseArguments(args) {
  // values хранит проверенные пары опций; index отмечает очередной ключ, name — его имя.
  const values = {};
  for (let index = 0; index < args.length; index += 2) {
    const name = args[index];
    if (!['--port', '--upstream', '--result-root'].includes(name) || values[name] !== undefined || args[index + 1] === undefined) throw new Error('Требуются --port, --upstream и --result-root.');
    values[name] = args[index + 1];
  }
  if (!/^\d+$/.test(values['--port'] || '') || !values['--upstream'] || !values['--result-root']) throw new Error('Не заданы обязательные параметры.');
  if (Number(values['--port']) === 0) throw new Error('CLI требует явно выбранный ненулевой порт.');
  return { port: Number(values['--port']), upstream: values['--upstream'], resultRoot: values['--result-root'] };
}

// Проверка главного модуля сохраняет возможность изолированного smoke без автоматического listener при import.
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    // proxy содержит единственный CLI listener; stop закрывает его при SIGINT/SIGTERM, не затрагивая backend.
    const proxy = await createFaultProxy(parseArguments(process.argv.slice(2)));
    /** Завершает CLI после освобождения принадлежащих ему соединений и журналов. */
    const stop = async () => { await proxy.close(); };
    process.once('SIGINT', stop);
    process.once('SIGTERM', stop);
    process.stdout.write(JSON.stringify({ ready: true, port: proxy.port }) + '\n');
  } catch {
    process.stderr.write('Не удалось запустить loopback proxy; проверьте параметры и каталог workspace.\n');
    process.exitCode = 1;
  }
}
