# BUGSAUDIT-2026-09-19

## 1. Шапка

- Дата: 2026-09-19
- Ревизия до правок: `git rev-parse HEAD` → `39a246075f2ef70dbdc8e56f5459583125405e5e` (ветка `master`, дерево чистое)
- Область аудита: весь проект `C:/Research/keyboard/keysidekick-public` (KeySidekick 0.9.6, C++/Win32 + ванильный JS-дашборд).
  Рядом лежат `keyboard-router` (то же дерево, отставший HEAD `ec36c24` + чужие незакоммиченные правки — см. §6) и `keyboard-binding` (старые AHK-эксперименты, вне продукта: не собираются, ни на что не ссылаются).
- Точка отката: ветка `ultra-audit-2026-09-19` от `39a2460`; правки — три коммита поверх `master` (`9d87d79`, `87f94eb`, `742202f`).
- Чем проверял:
  - сборка: `cmd /c src\build.bat` (MinGW-w64 g++ 16.2.0) → `=== Build OK ===` (из `src/`, как написано в README; из корня репозитория build.bat падает — см. BA-43)
  - юнит-тесты: `run_all_tests.sh` → базово `14 passed, 0 failed`; после правок `15 passed, 0 failed` (дважды подряд)
  - HTTP-интеграция: `tests/http_integration_tests.sh` против живого `sidekick.exe` → базово и после правок `58 passed, 0 failed`
  - изолированный прогон `sidekick.exe` в `tmp/audit/run` с замером `HandleCount` через PowerShell (P0 ниже)
  - браузер (реальный Chromium через `browser.open`) против живого дашборда — для проверки XSS и клиентских правок
  - `git log -p -40` обоих репозиториев (28918 строк диффа в `tmp/audit/history-full.diff`), грепы по TODO/FIXME/strncpy/NDEBUG
  - восемь read-only скаутов (sidekick.cpp по частям, конфиг-слой, Win32-слой, фронтенд, тесты, сборка/CI/доки, HTTP, ввод/устройства) — их цитаты кода я использую как «подтверждено (scout …)» только там, где сам этот участок не открывал; всё, что правил, прочитал лично.

## 2. Покрытие

| Область | Просмотрено | Чем смотрел | Почему не смотрел |
| --- | --- | --- | --- |
| `src/sidekick.cpp` (6127 стр.) | да | личное чтение: HTTP-хендлер целиком (3495–5090), hot path (1060–1420), MsgWndProc (2222–2760), конфиг (5080–5560), ReadLoop/main (5580–6127), трей/JSON-хелперы/SSE (2930–3420) + скаут на остаток | — |
| `src/http_security.{h,cpp}` | да | личное чтение + скаут | — |
| `src/config_v3.*`, `config_domain_bridge.*`, `domain_model.*` | да | скаут ConfigModel (построчно), лично — только места правок и `IsSafeId`/`isValidStableId` | — |
| `src/runtime_storage.*`, `action_parser.*` | да | скаут ConfigModel + лично `UniqueTempPath` и тест-хелпер | — |
| `src/windows_targets.*`, `targeted_input.*`, `probe_device.cpp` | частично | скаут WinTargets/SidekickInput (полные чтения + аудит хендлов) | сам читал только потребляющие участки sidekick.cpp; правки в этих файлах не делал (кроме тестов) |
| `src/supervisor.*`, `startup_manager.*`, `runtime_state.*`, `command_queue.h` | частично | скаут SidekickCore + личная охота за флейком command_queue | модули не входят в сборку продукта; читал только то, что нужно для находок |
| `src/mingw_threading.h` | да | личное чтение `thread`/`mutex`/`lock_guard`/CV-части, минимальные пробники | — |
| `web/app.js` (1634 стр.), `index.html`, `styles.css`, `generate_dashboard.ps1` | да | скаут WebFrontend (полное чтение app.js) + личная проверка XSS в браузере | — |
| `tests/**` (15 файлов), `run_all_tests.sh` | да | скаут TestQuality (все файлы) + личные правки и прогоны | — |
| `.github/workflows/`, `dist/make_dist.ps1`, `src/build.bat`, README×3, CHANGELOG, docs/, presentation.html | да | скаут BuildCiDocs (полное чтение) + лично make_dist/README/build.bat | — |
| `keyboard-router/` (второй рабочий tree) | частично | сравнение файлов, git log, статус | это копия того же дерева; аудит вёл по keysidekick-public (§6) |
| `keyboard-binding/`, `tmp/`, `Interception.zip`, PDF/DEEP_RESEARCH_REQUEST | нет | — | не собирается, не линкуется, не упоминается продуктом: старые эксперименты |

Итоги: находок 67 · полностью подтверждено 65 · содержат непроверенную часть 2 (BA-26 — какие именно процессы не дают прочитать имя; BA-27 — переиспользование HWND ОС) · областей закрыто 8 из 9 (не закрыта `keyboard-binding/` как не-продукт).
Правками закрыто: P0 — 1/1, P1 — 30 из 36, P2 — 11 из 30; остальное перечислено в §6 поимённо.
Находки BA-60…BA-62 добавлены после разбора приоритетов владельца (детект подключения и переключение драйвера) — они найдены при живой проверке на реальном железе, а не чтением.
Ревью-проход после фиксов нашёл ещё два дефекта в уже исправленных местах (BA-59 и пропущенный README-фикс, §4) — они вошли в коммит `7fedf8f`.

## 3. Находки

Формат: ID · где (файл:строка — символ) · суть · путь достижимости · как заметить · последствие · severity · статус (кто подтвердил) · текущий код · исправленный код · объяснение · почему это баг, а не решение.

---

### [BA-01] Каждый успешный HTTP-запрос течёт сокетом: обработчик не закрывает принятое соединение

- **Где:** `src/sidekick.cpp:5019` — `HandleHttpConnection` (хвост), `src/sidekick.cpp:5024` — `HttpWorkerThread`
- **Severity:** P0 · **Статус:** подтверждено (проверено мной, замером)
- **Путь достижимости:** любой клиент → `HttpThread::accept` (3476) → `HttpWorkerThread` (5024) → `HandleHttpConnection` → цепочка роутов → выход из функции без `closesocket`
- **Как заметить:** открыть дашборд (он опрашивает `/api/status`, `/api/v1/state`, `/api/v1/activity` раз в 0.8–3 с) и смотреть `HandleCount` процесса: +1 на запрос.
- **Последствие:** неограниченный рост дескрипторов и сокетов (в CLOSE_WAIT), затем `accept` начинает падать (`WSAENOBUFS`/`WSAEMFILE`) — API умирает при живом процессе, лечится только перезапуском.
- **Текущий код:**
  ```cpp
      else {
          HttpSend(cli, "not found", "text/plain", 404);
      }
  }   // ← выхода с closesocket нет
  ```
  а на SSE-ветке стоял комментарий `return;  // skip the closesocket at the end`, ссылающийся на уже не существующий вызов.
- **Исправленный код:**
  ```cpp
      else {
          HttpSend(cli, "not found", "text/plain", 404);
      }

      // Every response above is written with "Connection: close", and this worker
      // owns the accepted socket for exactly one request: close it here.
      closesocket(cli);
  }
  ```
- **Объяснение:** сокет закрывался только на ранних `return`-ветках (401/403/405/413/431, `windows/foreground` без окна) и в SSE-ветке (передача владения потоку). Нормальный путь — HTML дашборда, все GET-ы, все успешные POST-ы, 404 — падал из функции без закрытия.
- **Почему это баг, а не решение:** ветка `HttpWorkerThread` (`if (!g_running) closesocket(cli)`) закрывает сокет только при остановке; комментарий на SSE-ветке прямо утверждает, что закрытие в конце функции есть. Значит, вызов был удалён как регрессия, а не спроектирован.
- **Проверка:** до правки (та же сборка из `git stash`): `handles 209 → 509 → 809` после 0/300/600 запросов `GET /api/status`. После: `201 → 201 → 202`, `CLOSE_WAIT` = 0.

### [BA-02] Action-клавиша забывалась в prev-состоянии: удержание + любая другая клавиша повторно стреляли действием

- **Где:** `src/sidekick.cpp:1083` — `BasicReinject` (до правки фильтр `exceptUsages` не попадал в `g_prevReport`)
- **Severity:** P1 · **Статус:** подтверждено (проверено мной; покрыто тестом `tests/report_diff_tests.cpp`)
- **Путь достижимости:** физическое нажатие → `ReadLoop` → `ProcessReport` (BASIC-ветка) → `BasicReinject` → `g_prevReport` без consumed-клавиши; следующее нажатие любой другой клавиши даёт отчёт, где action-клавиша снова «новая» → `FindMappingForUsage` → `ExecuteAction` второй раз.
- **Как заметить:** зажать клавишу с `!launch:...` и, не отпуская, нажать другую — приложение запускается повторно; то же с макросом `{Ctrl+C}` (отправится дважды) и с `!switch:`.
- **Последствие:** повторные запуски приложений, дубли макросов/переключений — то есть действия пользователя выполняются не один раз, а при каждом изменении состава отчёта.
- **Текущий код:**
  ```cpp
      int cur[6] = {0,0,0,0,0,0};
      int ncur = 0;
      for (int i = 2; i < 8 && i < (int)len; i++) {
          if (report[i] != 0 && ncur < 6) {
              int uid = report[i];
              bool skip = false;
              for (int e : exceptUsages) if (e == uid) { skip = true; break; }
              if (!skip) cur[ncur++] = uid;      // consumed-клавиша исчезает из состояния
          }
      }
      ...
      for (int k = 0; k < 6; k++) g_prevReport[k] = (k < ncur) ? cur[k] : 0;
  ```
- **Исправленный код:** логика вынесена в чистый модуль `src/report_diff.h` (`ComputeReportEdges`/`StoreHeldUsages`), который считает `held` по ВСЕМ usage отчёта, а `pressed` — только новые; инжекция пропускает consumed, состояние — нет:
  ```cpp
      const keysidekick::ReportEdges edges = keysidekick::ComputeReportEdges(g_prevReport, report, len);
      for (std::size_t i = 0; i < edges.pressed.size(); ++i) {
          const int uid = edges.pressed[i];
          if (keysidekick::ReportEdgeConsumed(uid, exceptUsages)) continue;
          ScanInfo si = UsageToSet1(uid);
          if (si.scan) SendOneScan(si.scan, si.extended, false);
      }
      keysidekick::StoreHeldUsages(edges.held, g_prevReport);
  ```
- **Почему это баг:** комментарий рядом («обновить prev (только неотфильтрованные usage)») описывал именно то поведение, которое ломает edge-детект: в targeted-ветке `g_prevReport` хранит неотфильтрованный набор, в basic — отфильтрованный. Две разные трактовки одного состояния.
- **Проверка:** тест `tests/report_diff_tests.cpp` (22 checks) — «consumed key stays held across later reports»; падал на моей первой версии теста, где я ошибочно ждал press-edge у удерживаемой клавиши (тест поймал ошибку в самом тесте, что подтверждает его чувствительность).

### [BA-03] Диф модификаторов пропускался, если в отчёте была action-клавиша → зажатый Ctrl/Shift/Alt/Win оставался в системе

- **Где:** `src/sidekick.cpp:1090` — `BasicReinject`, флаг `hasActionKey`
- **Severity:** P1 · **Статус:** подтверждено (проверено мной)
- **Путь достижимости:** зажать Ctrl выделенной клавиатуры, нажать клавишу с action-mapping, отпустить Ctrl — отчёт «Ctrl снят» приходит вместе с action-клавишей? Нет: достаточно отпустить модификатор в том же отчёте, где появляется action-клавиша, либо наоборот — `hasActionKey == true` отменяет весь блок диффа, включая KEYUP.
- **Как заметить:** после описанной последовательности Ctrl остаётся нажатым во всей системе до перезапуска/`ReleaseAllKeys`.
- **Последствие:** «залипшие» модификаторы: дальнейший ввод интерпретируется как Ctrl+… во всех приложениях.
- **Текущий код:**
  ```cpp
      bool hasActionKey = !exceptUsages.empty();
      if (!hasActionKey) {   // инжектим модификаторы только если нет action-клавиши
          BYTE modDelta = mod ^ g_prevModifiers;
          ...
      }
      g_prevModifiers = mod;   // всегда обновляем edge-detect
  ```
- **Исправленный код:** блок выполняется всегда; защита «не отпускать чужое» уже есть внутри `SendOneScan` (`owns()`), поэтому парность down/up сохраняется:
  ```cpp
      const BYTE modDelta = mod ^ g_prevModifiers;
      if (modDelta) { for (auto& mb : MOD_BITS) { ... SendOneScan(si.scan, si.extended, keyUp); } }
      g_prevModifiers = mod;
  ```
- **Объяснение:** исходный комментарий боялся «KEYUP ненадёжен для scan-based событий» и решал это асимметрией (down не шлём, если есть action-клавиша) — но именно асимметрия и создаёт залипание: гейт оценивается на КАЖДЫЙ отчёт, а состояние модификатора глобально.
- **Почему это баг:** `ReleaseAllKeys` существует ровно как страховка от таких ситуаций — то есть автор знал о риске; гейт делает страховку штатным путём.

### [BA-04] Right Arrow отображался на scan-код End

- **Где:** `src/sidekick.cpp:625` — таблица `UsageToSet1`, строка `{0x4F,0xE04F,true}`
- **Severity:** P1 · **Статус:** подтверждено (проверено мной по таблице Set-1)
- **Путь достижимости:** basic-режим → `ProcessReport` → `BasicReinject` → `SendOneScan(usage 0x4F)` → `SendInput` с E0 4F = End.
- **Как заметить:** замапить стрелку Right в basic-профиле и нажать — курсор уходит в конец строки/документа (End), а не на символ вправо.
- **Последствие:** неверный ввод; кроме того ledger ключуется `(scan, extended)`, поэтому удерживаемый Right и настоящий End считаются одной клавишей — преждевременный release.
- **Текущий код:** `{0x4D,0xE04F,true}, // End` и `{0x4F,0xE04F,true}, // Right` — один и тот же scan.
- **Исправленный код:** `{0x4F,0xE04D,true}, // Right` (end — E0 4F, right — E0 4D).
- **Почему это баг:** соседние строки (Left E0 4B, Down E0 50, Up E0 48, Home E0 47, PageUp E0 49) верны; ошибка изолирована.

### [BA-05] Pause отображалась на scan-код Insert

- **Где:** `src/sidekick.cpp:616` — `UsageToSet1`, `{0x48,0xE052,true}`
- **Severity:** P1 · **Статус:** подтверждено (проверено мной)
- **Как заметить:** нажать Pause в basic-профиле → система получает Insert (E0 52).
- **Последствие:** неверная клавиша; коллизия с Insert в ledger.
- **Исправленный код:** `{0x48,0x45,false}, // Pause (Set-1 make 0x45; префикс E1 здесь невыразим)`.
- **Объяснение/ограничение:** Pause в Set-1 — `E1 1D 45`, а схема поддерживает только `E0`. Выбран честный make-код 0x45; коллизия с NumLock (тоже 0x45) остаётся неустранимой в этой схеме и задокументирована в отчёте (BA-35).

### [BA-06] Ledger'ы инъекций мутировались из HTTP-воркеров без синхронизации

- **Где:** `src/sidekick.cpp:1060` (`g_injectedKeys`), `src/sidekick.cpp:1062` (`g_targetedKeys`); мутаторы из HTTP — `GET /api/v1/devices/detect` (4565-4566 в старой нумерации) и `POST /api/v1/devices/capture`
- **Severity:** P1 · **Статус:** подтверждено (скаут SidekickInput + лично — вызовы внутри хендлера)
- **Путь достижимости:** дашборд/`curl` → `ReleaseAllKeys()`/`ReleaseAllTargetedKeys()` в HTTP-потоке одновременно с `SendOneScan`/`ProcessReport` в read-потоке.
- **Как заметить:** гонка на `std::vector` без лока — повреждение кучи или потеря записи владения; внешне — «залипшая» клавиша или падение.
- **Последствие:** порча памяти/залипание клавиш при обычном пользовательском сценарии (мастер «Identify keyboard» сам опрашивает detect).
- **Исправленный код:** введён `g_csLedger`; под ним — `owns`/`recordDown`/`recordUp`/`ownedKeysForRelease`/`releaseAll`/`dueRepeats` в `SendOneScan`, `ReleaseAllKeys`, `SendHoldableKeyToTarget`, `ReleaseTargetedUsage`, `ReleaseAllTargetedKeys`, `DispatchTargetedRepeats`. Лок никогда не удерживается во время `SendInput`/`PostMessage`.
- **Почему это баг:** `InputLedger`/`TargetedInputLedger` — обычные контейнеры без внутренней синхронизации, а вызывающие живут в разных потоках; в `main()` критические секции создавались для профилей/SSE, но не для ledger'ов.

### [BA-07] Окно identify глотает key-up → инжектированные клавиши остаются нажатыми

- **Где:** `src/sidekick.cpp:1349` — `ProcessReport` (ранний `return` в identify-окне), `src/sidekick.cpp:4748` — `POST /api/v1/input/identify`
- **Severity:** P1 · **Статус:** подтверждено (скаут SidekickInput, логика прочитана мной)
- **Путь достижимости:** зажать клавишу выделенной клавиатуры → в дашборде нажать «Test keyboard»/identify → окно 15 с глотает отчёты (`g_prevReport` синхронизируется, но ledger не очищается) → физически отпущенная клавиша остаётся нажатой в системе.
- **Последствие:** залипшие символы/модификаторы; лечится только перезапуском (или release-all через другой эндпоинт).
- **Исправленный код:** при взведении окна снимаем всё, что уже инжектировано:
  ```cpp
      ReleaseAllKeys();
      ReleaseAllTargetedKeys();
      g_identifyListening.store(true);
  ```
- **Почему это баг:** в самом обработчике комментарий признаёт, что окно глотает события; отсутствие реконсиляции — недосмотр, а не решение.

### [BA-08] `SendHoldableKeyToTarget` возвращал `true` на всех отказах → нажатие терялось молча

- **Где:** `src/sidekick.cpp:2087` — `SendHoldableKeyToTarget` (пути «нет окна» и «PostMessage не удался»)
- **Severity:** P1 · **Статус:** подтверждено (скаут SidekickInput; код прочитан мной)
- **Путь достижимости:** targeted-профиль → key-down → `FindProfileTarget` не находит окно (приложение закрыто, AutoStart выключен) → `return true` → вызывающий (`if (!SendHoldableKeyToTarget(...)) SendToTargetWindow(...)`) не делает fallback → клавиша не отправлена никуда.
- **Последствие:** «клавиатура не работает» без сообщения; при этом `HandleMissingProfileTarget` (в т.ч. запуск приложения) не вызывается.
- **Исправленный код:** `return false` для «нет окна» и «InitialDown не отправлен» (при `recordDown`-компенсации остаётся `true`, т.к. ключ снят).

### [BA-09] Провал записи config.ini докладывался как успех

- **Где:** `src/sidekick.cpp:2721` — epilogue `WM_DASH_OP`; `src/sidekick.cpp:4230` — `POST /api/v1/applications/create`
- **Severity:** P1 · **Статус:** подтверждено (скаут SidekickCore; проверено мной замером)
- **Путь достижимости:** файл занят/только для чтения/нет прав → любая мутация через дашборд → HTTP 200 `{"ok":true}` → правка живёт в памяти и исчезает при перезапуске.
- **Как заметить:** `attrib +R src/config.ini`, затем любая правка профиля в дашборде — «сохранено», а на диске ничего.
- **Последствие:** потеря пользовательских правок без единого признака; мёртвый код (`bool writeOk = true;` никто не читал) подтверждает, что результат намеренно писали — просто забыли проверить.
- **Исправленный код (epilogue):**
  ```cpp
      if (!WriteConfig()) {
          op->success = false;
          snprintf(op->error, sizeof(op->error),
                   "config.ini could not be written — change not saved");
      }
  ```
  и в `applications/create`: `if (!WriteConfig()) { HttpSendJson(cli, "{\"error\":\"config.ini could not be written\"}", 500); } else { BumpRevision(); ... }`
- **Проверка:** `attrib +R src/config.ini` → `POST /api/v1/applications/create` → **HTTP 500** `{"error":"config.ini could not be written"}`; `attrib -R` → **HTTP 200** `{"ok":true,"id":"app-AuditWritableProbe-2"}`.

### [BA-10] Импорт конфига: валидатор-заглушка и отсутствие лока при записи того же файла

- **Где:** `src/sidekick.cpp:4248` — `POST /api/v1/config/import`
- **Severity:** P1 · **Статус:** подтверждено (скаут SidekickCore; проверено мной)
- **Путь достижимости:** дашборд → Import → base64 любого мусора → `AtomicWriteUtf8` с валидатором `[](const std::string&, std::string*){ return true; }` → файл перезаписан, `DASH_RELOAD` читает битый конфиг (parse-ошибок нет, см. BA-30) → профили «теряются».
- **Как заметить:** импортировать текстовый файл (не INI) — приходит `{"ok":true}`.
- **Последствие:** рабочий конфиг заменяется нечитаемым; пользователь видит пустой дашборд без объяснения.
- **Исправленный код:** контент проверяется `keysidekick::config::Parse(cfg).ok()` до записи (400 при отказе) и повторно в валидаторе `AtomicWriteUtf8`; запись идёт под `g_csProfile` (тем же локом, что и `WriteConfig`), лок отпускается ДО `RunDashOp`, иначе дедлок с main-потоком.
- **Почему это баг:** `AtomicWriteUtf8` принимает валидатор именно для проверки содержимого — заглушка `return true` обнуляет контракт; лок нужен потому, что в `WM_DASH_OP` комментарий прямо фиксирует «только main thread пишет файл», а этот роут пишет из воркера.

### [BA-11] После неудачного LoadConfig остаётся старый `g_domain` → следующий WriteConfig воскрешает прежний набор профилей

- **Где:** `src/sidekick.cpp:5289` (read-failure) и `src/sidekick.cpp:5309` (legacy fallback) — `LoadConfig`
- **Severity:** P1 · **Статус:** подтверждено (скаут SidekickCore; проверено мной)
- **Путь достижимости:** config.ini удалён/заблокирован → `POST /api/reload` → `ReloadConfig` чистит `g_profiles`, но не `g_domain`; любая последующая правка дашборда вызывает `WriteConfig` → `SyncRuntimeToDomain` домешивает старые профили к «basic» и пишет их в файл.
- **Последствие:** воскрешение удалённых профилей; расхождение runtime и domain.
- **Исправленный код:** в обеих ветках отказа `g_domain = keysidekick::DomainModel();` перед `EnsureBuiltinBasic()`/`LoadConfigLegacy()`.

### [BA-12] Санитайз имён профилей мог создать дубликат domain-id → Serialize падает → тихий откат на legacy-писатель

- **Где:** `src/sidekick.cpp:5470` — `SyncRuntimeToDomain`
- **Severity:** P1 · **Статус:** подтверждено (скаут SidekickCore; проверено мной)
- **Путь достижимости:** два профиля с именами `My.Profile` и `My_Profile` → оба санитайзятся в `My_Profile` → `ValidateForSerialization` даёт ERROR → `WriteConfig` уходит в `BuildConfigContent` (другой формат: без `[Application.*]`, без `ApplicationId`).
- **Последствие:** молчаливая деградация схемы файла; следующий `Parse` теряет связи профиль↔приложение.
- **Исправленный код:** перед `push_back` проверка `findProfile(newId)` с добавлением `-2`, `-3`, …

### [BA-13] Рантайм-порт и порт из конфига разъезжались → 403 на каждый запрос

- **Где:** `src/sidekick.cpp:5269` — `ApplyGeneralSettings`; потребители порта — policy (3673), URL-ы трея (2301/2309/2882), диагностика (5009)
- **Severity:** P1 · **Статус:** подтверждено (скаут SidekickHttp; проверено мной)
- **Путь достижимости:** импорт/правка config.ini с другим `HTTPPort` → reload → `policy.port`/Host-проверка начинают требовать новый порт, а listener остаётся на старом → все запросы 403 `host_forbidden`, дашборд «недоступен».
- **Последствие:** приложение выглядит мёртвым, хотя слушает; лечится только перезапуском; при этом `HTTPPort` в следующих ответах/файле — уже новый.
- **Исправленный код:** введён `g_httpPortBound` (порт живого listener'а, ставится после `bind`), `LiveHttpPort()` используется policy/URL/диагностикой/логом; `ApplyGeneralSettings` применяет `HTTPPort/HTTPEnabled` только пока сервер не запущен; `SyncRuntimeToDomain` больше не перезаписывает эти поля из рантайма, поэтому значение из файла (в т.ч. импортированное) сохраняется до следующего старта.
- **Почему это баг:** SSOT — два источника правды для одной величины; ни одно место не сообщало пользователю «порт применится после перезапуска».

### [BA-14] `POST /api/profile` создавал профиль только в runtime-проекции (фантом) и позволял занять имя `basic`

- **Где:** `src/sidekick.cpp:2374` — case `DASH_SET_PROFILE`
- **Severity:** P1 · **Статус:** подтверждено (проверено мной)
- **Путь достижимости:** дашборд (форма профиля) → `POST /api/profile` с новым именем → `g_profiles[op->profile]` (`std::map::operator[]`) создаёт запись; любая последующая операция с профилями вызывает `ProjectDomainToRuntime()`, который делает `g_profiles.clear()` и перестраивает карту из `g_domain` → профиль исчезает. Отдельный эффект: имя `basic` попадало в ту же ветку и переопределяло встроенный профиль.
- **Как заметить:** создать профиль через форму и переключить активный — новый профиль пропадает из списка; при сохранении под именем `basic` ломается встроенный.
- **Последствие:** «профиль исчез» и порча встроенного режима.
- **Исправленный код:** неизвестное имя создаётся через `keysidekick::ProfileService::createProfile` (id санитайзится) с последующей проекцией; имя `basic` отклоняется с ошибкой `'basic' is the built-in profile and cannot be edited here`.
- **Почему это баг:** `SyncRuntimeToDomain` умеет до-создавать domain-профили из runtime (значит, «профиль только в runtime» — не задуманный режим), а `operator[]` вместо `find` — классическая причина записи мимо источника правды.

### [BA-15] `!toggle:` не поднимал state revision → дашборд не видел смену профиля

- **Где:** `src/sidekick.cpp:1198` — `ExecuteAction`
- **Severity:** P1 · **Статус:** подтверждено (проверено мной)
- **Как заметить:** профиль с `!toggle:` → нажать клавишу → в дашборде активный профиль прежний до следующего не связанного события.
- **Последствие:** устаревший UI: активный профиль/режим на экране не соответствует железу (`!switch:` в том же месте revision поднимает).
- **Исправленный код:** добавлен `BumpRevision();` после `UpdateTray()`.

### [BA-16] Auto-switch мог активировать targeted-профиль для процесса, чьё имя не прочиталось

- **Где:** `src/sidekick.cpp:2194` — `AutoSwitchOnForegroundChange`
- **Severity:** P1 · **Статус:** подтверждено (скаут WinTargets; проверено мной)
- **Путь достижимости:** фокус на защищённом/AppContainer-процессе → `QueryFullProcessImageNameW` не даёт имя → `procUtf8` пуст → сравнение `_stricmp(prof.targetExe.c_str(), "")` истинно для профиля без targetExe → переход в targeted-профиль «на всякий случай».
- **Последствие:** клавиатура молча уходит в чужой режим; ввод перестаёт печатать.
- **Исправленный код:** `if (procUtf8.empty()) return;` перед сравнением.

### [BA-17] `POST /api/v1/startup`: результат `CoInitializeEx` перетирался → дисбаланс ссылок COM

- **Где:** `src/sidekick.cpp:4483` — обработчик startup (ветка `enabled`)
- **Severity:** P1 · **Статус:** подтверждено (проверено мной)
- **Путь достижимости:** включение автозапуска из дашборда; `hr` переиспользуется для `CoCreateInstance`/`QueryInterface`/`Save`, а `CoUninitialize()` вызывается по последнему `hr`.
- **Последствие:** при `RPC_E_CHANGED_MODE` + успешных последующих вызовах — лишний `CoUninitialize()` (чужая квартира); при успешном `CoInitializeEx` и провале создания ярлыка — утечка инициализации COM на потоке-воркере.
- **Исправленный код:** отдельная переменная `comInit`/`comOwned`, `if (comOwned) CoUninitialize();`.

### [BA-18] XSS в дашборде: `esc()` вместо `jsStr()` внутри атрибутов с однокавычечными JS-строками

- **Где:** `web/app.js:204` (кнопка ✎ карточки маппинга → `editMapping`), `web/app.js:621` и `web/app.js:1014` (`applyDriverSwap` у баннера смены порта и в мастере)
- **Severity:** P1 (security) · **Статус:** подтверждено (проверено мной в браузере)
- **Путь достижимости:** поле «Raw action» → `addKey` → `POST /api/key` → `DASH_ADD_KEY` сохраняет строку как есть → `GET /api/profiles` → `renderEditor` подставляет её в inline-обработчик; второй путь — импорт чужого config.ini (штатный сценарий шаринга) и любой рендер списка профилей с `!switch:`-строками.
- **Как заметить:** действие `\');window.__XSS_FIRED=true;//` — карточка рендерится, клик по ✎ выполняет произвольный JS в origin дашборда, где лежит CSRF-токен (оттуда доступны driver swap/restore и все мутации).
- **Последствие:** выполнение произвольного кода в привилегированном локальном origin (и, при импортированном конфиге, атака на того, кто конфиг импортирует).
- **Текущий код:**
  ```js
  onclick="editMapping('+k.usage+','+k.mod+',\''+esc(k.action).replace(/'/g,"\\'")+'\')"
  ```
  `esc()` не экранирует обратный слэш, поэтому `\'` в данных закрывает строку.
- **Исправленный код:** `...jsStr(k.action)+...` (и то же для двух `applyDriverSwap`), где `jsStr` уже существовал и используется в двух функциях рядом.
- **Проверка (реальный браузер, живой дашборд):** старая формула → `firedWithOldEscaping: true` (payload выполнился); новая → `xssFired: false`, а билдер открылся ровно с исходным действием (`actionsMatch: true`).
- **Почему это баг, а не решение:** в файле есть готовый `jsStr()` с комментарием «Value for embedding in single-quoted JS strings inside double-quoted HTML attributes»; правило реализовано дважды, и в одном месте реализовано неверно.

### [BA-19] Мутирующий GET `/api/v1/devices/detect` без токена + два разошедшихся списка «мутирующих» путей

- **Где:** `src/http_security.cpp:196` — `IsMutationPath` (6 путей) и `src/sidekick.cpp:3537` — `IsPostOnlyPath` (25 путей); обработчик detect — `src/sidekick.cpp:4634`
- **Severity:** P1 · **Статус:** подтверждено (скауты HTTP/WinTargets; проверено мной замером)
- **Путь достижимости:** `curl http://127.0.0.1:8765/api/v1/devices/detect` без токена → pipeline считает `mutation = false` (метод GET, путь не в списке) → ветка выполняется: `ReleaseAllKeys()`, `ReleaseAllTargetedKeys()` и открытие интерфейсов на 500 мс. Любой локальный процесс (или браузер без `Sec-Fetch-Site`) сбрасывает удерживаемые клавиши.
- **Последствие:** неаутентифицированное изменение состояния; плюс SSOT-разрыв (6 против 25 путей), из-за которого решение «что мутирует» принимается в двух местах по-разному.
- **Исправленный код:** единственный список `keysidekick::IsMutationPath` (охватывает все мутирующие роуты), `IsPostOnlyPath` удалён; обработчик переведён на `POST /api/v1/devices/detect`, клиент обновлён в трёх местах.
- **Проверка (живой билд):** `GET → 405`, `POST без токена → 403`, `POST с токеном → 200 {"detected":[]}`; интеграционный набор `58/58`.
- **Почему это баг:** комментарий в коде (`канонический API... state changes require POST`) и CHANGELOG («enforces CSRF token on mutating routes») утверждают обратное; два списка — прямое нарушение SSOT.

### [BA-20] `POST /api/profile/activate` отвечал `ok:true` до и вместо переключения

- **Где:** `src/sidekick.cpp:3788` — обработчик activate
- **Severity:** P1 · **Статус:** подтверждено (скаут SidekickHttp; проверено мной)
- **Путь достижимости:** дашборд `activate()` → имя опечатано/устарело → `SwitchProfileByName` логирует «Profile not found» и ничего не делает, а клиент показывает «Active profile: X».
- **Последствие:** ложный успех; кроме того пустая ветка отдавала `text/plain` на POST, из-за чего `api()` бросал `Invalid response from /api/profile/activate`.
- **Исправленный код:** проверка существования имени под `g_csProfile` → 404 `{"error":"profile not found"}`; legacy-ветка без имени отвечает JSON-ом.
- **Проверка:** `POST {"name":"nope-not-here"}` → **404**.

### [BA-21] «Save changes» на переименованном профиле создавал второй профиль

- **Где:** `web/app.js:240` — `saveProfile` (в связке с `DASH_SET_PROFILE`)
- **Severity:** P1 · **Статус:** подтверждено (скаут WebFrontend; проверено мной)
- **Путь достигнустимости:** поле «Profile name» редактируемо; Save отправлял новое имя в `POST /api/profile` (создание), хотя рядом есть кнопка Rename, использующая правильный роут.
- **Последствие:** вместо переименования появлялся второй профиль без маппингов (и, после BA-14, уже персистентный), редактор «возвращался» к старому профилю.
- **Исправленный код:** при изменении имени сначала `POST /api/v1/profile/rename` (при ошибке — стоп с тостом), затем сохранение настроек под новым именем.

### [BA-22] Шесть проверок «временный файл не остался» не могли упасть

- **Где:** `tests/runtime_storage_tests.cpp:112` — `CountTemporaryFiles` (шаблон `*.tmp.*`), вызовы на 164/178/199/221/237/258
- **Severity:** P1 (тестовая проверка) · **Статус:** подтверждено (скаут TestQuality; шаблон имени проверен мной по `src/runtime_storage.cpp:36-41`)
- **Как заметить:** производственное имя — `<base>.tmp_<hex>_<hex>_<hex><ext>` (напр. `config.tmp_0000000000000001_00abcdef_1.ini`); подстрока `.tmp.` в нём не встречается, поэтому `FindFirstFileW` всегда возвращал 0.
- **Последствие:** утечка временных файлов при регрессии `AtomicWriteUtf8`/`MigrateLegacyConfigIfNeeded` не была бы замечена; тест `TestRejectedMigrationCleansTemporaryFile` проходил вхолостую.
- **Исправленный код:** шаблон `*.tmp_*` + положительный контроль `TestTemporaryFileDetectorSeesProductionNames`, который сам создаёт файл производственной формы и требует, чтобы helper его увидел.
- **Проверка:** `run_all_tests.sh` → 15/15 (в т.ч. новый тест).

### [BA-23] Тест политики безопасности собирал собственную политику, противоречащую продакшн

- **Где:** `tests/http_security_tests.cpp:9-23` — `Policy()` (`allow_ipv6_loopback = true`, дублированные лимиты)
- **Severity:** P1 (тестовая проверка) · **Статус:** подтверждено (скаут TestQuality; проверено мной)
- **Последствие:** смена лимитов/имени заголовка/IPv6-политики в `src/` не ломала ни одного теста; проверялся собственный набор условий, а не продакшн-контракт.
- **Исправленный код:** лимиты берутся из `keysidekick::kDefaultMaxHeaderBytes/kDefaultMaxBodyBytes`, `allow_ipv6_loopback = false` (как в `sidekick.cpp`), добавлены проверки новых записей списка мутаций.

### [BA-24] Релизный ZIP увозил приватный `src/config.ini` разработчика

- **Где:** `dist/make_dist.ps1:81-84`
- **Severity:** P1 (privacy) · **Статус:** подтверждено (скаут BuildCiDocs; проверено мной)
- **Путь достижимости:** `make_dist.ps1` (его запускает CI на теге) кладёт в архив `source\src\*.ini`, т.е. рабочий конфиг автора: VID/PID его клавиатуры, его профили и пути к приложениям.
- **Последствие:** публикация личных данных в каждом релизе; значение имеет не только приватность — по конфигу восстанавливается, что именно и как маппил автор.
- **Исправленный код:** `if ($f.Name -eq 'config.ini') { continue }` с комментарием; `config.example.ini` остаётся.

### [BA-25] Откат к поиску окна по классу игнорировал процесс → нажатия могли уйти чужому приложению

- **Где:** `src/sidekick.cpp:1001` — `FindProfileTarget`
- **Severity:** P1 · **Статус:** подтверждено (скаут WinTargets; проверено мной)
- **Путь достижимости:** `ReadProcessPath` не смогла прочитать процесс кандидата (BA-26) → `ResolveTarget` вернул пусто → `FindWindowByClass(prof.targetClass)` находит **любое** окно того же класса, без проверки процесса; при AutoStart далее мог запускаться дубликат приложения.
- **Последствие:** ввод уходит в окно другого приложения с тем же классом (типовая ситуация для Electron/Chromium-приложений).
- **Исправленный код:** class-only откат выполняется только если `targetExe` пуст:
  ```cpp
      if (!h && prof.targetExe.empty() && !prof.targetClass.empty())
          h = FindWindowByClass(prof.targetClass.c_str());
  ```
- **Почему это баг:** «не нашли целевое окно» (и один лог/один AutoStart) безопаснее, чем доставка клавиш не тому процессу.

---

### [BA-26] `ReadProcessPath` глушит отказ чтения процесса; флаг `processMetadataAvailable` никем не читается

- **Где:** `src/windows_targets.cpp:83-110` (`ReadProcessPath`), `:136-141` (запись флага), `:257-265` (скоринг)
- **Severity:** P1 · **Статус:** подтверждено (scout WinTargets; сам файл не открывал — цитата кода из отчёта скаута, достижимость проверена по вызывающему коду)
- **Путь достижимости:** любой профиль с processName/processPath → `FindTargetWindowScored` → `EnumerateWindows` → для окна с непрочитанным процессом кандидат получает пустые `processName`/`processPath` → `ScoreTargetCandidate` возвращает `kNoMatch` → окно «не найдено» (и, до правки BA-25, identity-blind откат).
- **Последствие:** тихий отказ целевого поиска: «клавиатура не работает» для приложения, которое реально открыто; в логах ничего. Флаг `processMetadataAvailable`, заведённый ровно для сигнала об этом, в мире не читается (в JSON `/api/v1/windows` не выводится).
- **Гипотеза:** в защищённых процессах (PPL/AppContainer), при гонке «окно закрылось между GetWindowThreadProcessId и OpenProcess».
- **Не исправлено** — см. §6; частично снято правкой BA-25 (хуже всего было молчаливое перенаправление ввода).
- **Почему это баг:** поле-сигнал есть, но у него нет потребителя — «ошибка без владельца» (класс 3).

### [BA-27] Удержанная targeted-клавиша хранит HWND без проверки личности окна

- **Где:** `src/targeted_input.h:16-24` (`TargetedKey.target`), `src/sidekick.cpp:2045` (`DispatchTargetedRepeats`), `:2074` (`ReleaseAllTargetedKeys`)
- **Severity:** P1 · **Статус:** подтверждено для отсутствия ревалидации (скаут WinTargets + моё чтение); «переиспользование HWND» — гипотеза (поведение ОС, не наблюдал)
- **Путь достижимости:** зажать клавишу в targeted-режиме → окно-цель закрывается → Windows переиспользует значение HWND → автоповтор и KEYUP уходят новому окну (`IsWindow` истинно).
- **Последствие:** ввод в постороннее окно, включая key-up без key-down у получателя.
- **Не исправлено:** корректный фикс требует хранить идентичность окна (класс+процесс+threadId) и сверять её перед повтором; правка не влезает в текущий объём без риска для hot path — см. §6.

### [BA-28] `AutoStart` не переносится мостом config↔domain: чекбокс в дашборде сбрасывается, а `AutoStart=1` из примера конфига уничтожается первой же записью

- **Где:** `src/config_domain_bridge.cpp:76-78` (`ConfigToDomain` теряет `auto_start`), `:202` (`DomainToConfig` пишет `auto_start=false`), `src/sidekick.cpp:268-269` (проекция: «domain не хранит; оставляем false»)
- **Severity:** P1 · **Статус:** подтверждено (scout ConfigModel; проверено мной) · **Исправлено в `6ff6cb5`**
- **Последствие (до фикса):** функция «запустить приложение, если окно не найдено» (`HandleMissingProfileTarget`) не могла сработать после перезапуска: `rp.autoStart` всегда false, а значение из файла затиралось при первом сохранении.
- **Исправление (по замыслу самого репозитория):** domain уже описывает это как `LaunchPolicy{Never, IfNotRunning, Always}`, а комментарий в `ProjectDomainToRuntime` обещал «Phase 3 добавит launchPolicy → autoStart mapping». Маппинг сделан в обе стороны: `AutoStart=1 ↔ IfNotRunning`, `0 ↔ Never` (bridge, проекция, обратная синхронизация).
- **Проверка:** живой `sidekick.exe` с `config.example.ini` → `/api/profiles` отдаёт `aimp autoStart=True`; после правки через API в файле остаётся `AutoStart=1`; `POST /api/profile` с `autoStart:false` пишет `AutoStart=0`; юнит-тест `TestLaunchPolicyRoundTrip` (bridge, 49 checks).

### [BA-29] Механизм `Extension` (сохранение неизвестных строк INI) теряется мостом при первой же записи

- **Где:** `src/config_domain_bridge.cpp` (`ConfigToDomain`/`DomainToConfig` не переносят `extensions`), `src/config_v3.*`
- **Severity:** P1 · **Статус:** подтверждено (scout ConfigModel) · **Исправлено в `6ff6cb5`**
- **Последствие (до фикса):** строки, которые парсер специально сохранял для совместимости, удалялись при первом сохранении из дашборда.
- **Исправление:** `sidekick.cpp` запоминает `extensions` из последнего успешного `Parse` и возвращает их в `Config` перед `Serialize` (на legacy-пути и при ошибке чтения список очищается).
- **Проверка:** секция `[Sample] Kept=yes` в тестовом конфиге после правки через API сохранена как `[Extension.0001] Section=Sample Key=Kept Value=yes`.

### [BA-30] `Parse` не выдаёт `DIAGNOSTIC_ERROR` никогда → предохранитель `LoadConfig` и весь legacy-парсер мертвы

- **Где:** `src/config_v3.cpp` (валидация только как WARNING), `src/sidekick.cpp:5301-5313` (`if (hasErrors) → LoadConfigLegacy`)
- **Severity:** P1 · **Статус:** подтверждено (scout ConfigModel) · **Исправлено в `65d6540`** (для класса «не распознано ничего»)
- **Последствие (до фикса):** файл, в котором не распознано ни одной секции, молча принимался как «пустой конфиг»: приложение стартовало на дефолтах и первым же сохранением затирало пользовательский файл; ветка `fallback на legacy-парсер` и ~170 строк `LoadConfigLegacy` были недостижимы. Приватная проверка на UTF-16 без BOM остаётся незакрытой деталью (см. §6).
- **Исправление:** в `Parse` добавлено узкое правило — ERROR только если в файле нет ни одной знакомой секции (`General`/`Application.*`/`Profile.*`/`Extension.*`/`AIMP`/`Keys`); достаточно одной, поэтому легитимные legacy-конфиги и фикстуры v2 не задеваются.
- **Проверка:** юнит-кейс «rejects files without recognized sections» (мусор → ERROR; пустой/комментарии → ok; `[General]` → ok) + все прежние фикстуры; на живом приложении с `[NotAConfig]` в лог попало `reported 1 error(s), falling back to legacy parser` и `ERROR: config contains no recognized sections`, приложение стартовало, файл на диске остался нетронутым.

### [BA-31] Защита «нельзя удалить профиль, на который ссылаются» не может сработать

- **Где:** `src/domain_model.cpp:133-149` (`actionReferencesProfile`), `:713-749` (`deleteProfile`)
- **Severity:** P1 · **Статус:** подтверждено (scout SidekickCore; согласуется с pass-through: `action.profileId` несёт сырую строку)
- **Последствие:** удаление профиля, на который ссылаются маппинги `!switch:`/`!toggle:`, оставляет висячие действия.

### [BA-32] Suite `startup_manager` проверяет стаб класса, который не вызывается продуктом

- **Где:** `tests/startup_manager_tests.cpp:170-184` против `src/startup_manager.cpp:694-702`; сам класс не линкуется в `sidekick.exe` (`src/build.bat`), живой автозапуск — ярлык в Startup у `sidekick.cpp:4394`
- **Severity:** P1 (тестовая проверка) · **Статус:** подтверждено (scout TestQuality)
- **Последствие:** CHANGELOG заявляет «startup-manager test suite was orphaned — wired into the runner», но проверяется поведение заглушки TestOnly; продакшн-путь автозапуска юнит-тестами не покрыт вовсе.

### [BA-33] `testConcurrentExactlyOnceCompletion` падал на `completionWins == 1` — причина в шиме потоков, не в очереди

- **Где:** `tests/command_queue_tests.cpp:212`; корневая причина — `src/mingw_threading.h:189-216` (`std::thread` шима)
- **Severity:** P2 (надёжность CI; шим не входит в поставляемый бинарник) · **Статус:** подтверждено (измерено мной) · **Исправлено**
- **Как заметить:** `for i in $(seq 1 60); do ./.ai-cache/command_queue.exe >/dev/null 2>&1 || echo fail; done` — часть прогонов завершается `exit=3` с `Assertion failed … completionWins.load() == 1`.
- **Замеры:** 2/40, 4/15 и 13/60 — на чистой одиночной сборке (без параллельных задач). Инструментированный повтор того же сценария печатал в момент падения `state_after_pop=1 status_before=0 wins=0 status_after=0` — все 16 вызовов `fulfillSuccess` вернули false, состояние осталось Pending; под инструментацией падение не воспроизводилось (0/60), т.е. классический heisenbug по времени.
- **Настоящая причина:** `State::completeSuccess` (`src/command_queue.h:48-61`) корректен (mutex + ранний выход), поэтому «wins=0» объяснялось не очередью. Проверка гипотезы «join() не ждёт» минимальным пробником (16 потоков инкрементируют один atomic, затем `join()`): счётчик равен `0, 8, 9, 2, 6, 5` в шести прогонах — `join()` возвращался до выполнения тел потоков. Причина: `std::thread` в шиме владеет сырым `HANDLE`, но не объявлял ни копирование, ни перемещение, поэтому компилятор генерировал копирующий конструктор; `std::vector<std::thread>::push_back` копирует, деструктор временного объекта закрывает хендл, а оставшаяся копия ждёт **закрытый** хендл. Тот же паттерн используют оба конкурентных набора (`command_queue`, `runtime_state`) — то есть все их проверки читали счётчики раньше работы.
- **Исправленный код:** `thread` стал move-only (удалены копирующие операции, добавлены перемещающие с переносом хендла и обнулением источника; лишний член `FuncBase*` убран — владельцем держателя является процедура потока), плюс новый набор `tests/mingw_threading_tests.cpp` (join ждёт потоки из вектора, перемещение переносит владение, перемещающее присваивание закрывает прежний хендл, рукопожатие через condition_variable).
- **Проверка:** пробник после фикса — `counter=16` в 6/6; `command_queue.exe` — **0/80** падений (было 13/60); юнит-набор `16 passed, 0 failed`; продукт (`build.bat`) собирается, интеграция `58/58`.
- **Почему это баг, а не «флейк среды»:** измеренная частота и детерминированный механизм (копирование владеющего хендла) объясняют все наблюдения; `std::thread` по стандарту move-only, шим это нарушал.

---

### [BA-34] `POST /api/reload` всегда отвечает успехом

- **Где:** `src/sidekick.cpp:2509` (case `DASH_RELOAD`: `op->success = true` безусловно), ветка 500 в обработчике мертва
- **Severity:** P2 · **Статус:** подтверждено (скаут SidekickHttp)
- **Последствие:** «Reload failed» недостижим; при нечитаемом конфиге пользователь получает «ok» и пустой список профилей (частично смягчено BA-11).

### [BA-35] Коллизия Pause/NumLock в Set-1 неразрешима текущей схемой

- **Где:** `src/sidekick.cpp:616` и `:637` (`0x45` у Pause и NumLock)
- **Severity:** P2 · **Статус:** подтверждено (проверено мной)
- **Последствие:** в ledger (ключ scan+extended) Pause и NumLock — одна клавиша; одновременное удержание даст преждевременный release. Документировано, не исправлено (нужен учёт E1-префикса).

### [BA-36] Результат `SendInput` не проверяется, а ledger всё равно фиксирует владение

- **Где:** `src/sidekick.cpp:1066` (`SendOneScan`), `:5674` (`ReleaseAllKeys`)
- **Severity:** P2 · **Статус:** подтверждено (скаут SidekickInput)
- **Последствие:** неудачная инъекция KEYUP стирает запись владения → клавиша остаётся зажатой, повторить снятие нечем.

### [BA-37] `probe_device`: неинициализированный `USB_INTERFACE_DESCRIPTOR` после проигнорированной ошибки

- **Где:** `src/probe_device.cpp:144-152`
- **Severity:** P1 · **Статус:** подтверждено (scout WinTargets; файл сам не открывал)
- **Последствие:** отказ `WinUsb_QueryInterfaceSettings` не сообщается, цикл по эндпойнтам идёт по мусорному `bNumEndpoints` — диагностический инструмент врёт именно в том случае, для которого создан.

### [BA-38] `probe_device`: код возврата не отражает провал, а сообщения уводят не туда

- **Где:** `src/probe_device.cpp:111`/`217-250` (exit code), `:123-137` («read-only» доступ 0 + обвинение драйвера)
- **Severity:** P2 · **Статус:** подтверждено (scout WinTargets)
- **Последствие:** скрипт, проверяющий exit code, видит «успех», когда не открылось ни одно устройство; пользователя отправляют переставлять драйвер, хотя причина в режиме доступа.

### [BA-39] `B64Decode`: переполнение знакового `int` (UB) на длинном вводе

- **Где:** `src/sidekick.cpp:3043-3056`
- **Severity:** P2 · **Статус:** подтверждено (проверено мной)
- **Исправленный код:** после выдачи байта аккумулятор маскируется: `buf &= (1 << bits) - 1;`.

### [BA-40] `strncpy` без терминации при усечении (3 места)

- **Где:** `src/sidekick.cpp:4692` и `:4702` (`shortPath` в `/api/v1/devices/detect`), `src/sidekick.cpp:5236-5237` (legacy-загрузчик: `g_deviceVidPid`, `g_defaultProfile`)
- **Severity:** P2 (чтение за границей буфера; вызов требует длинного пути/значения) · **Статус:** подтверждено (проверено мной)
- **Исправленный код:** явная терминация `[...] = 0` после копии (в detect — плюс инициализация `= {0}`).

### [BA-41] Мёртвый роут `GET /api/profile?name=X` с обращением к итератору после снятия лока

- **Где:** `src/sidekick.cpp:3801` (удалён)
- **Severity:** P2 · **Статус:** подтверждено (проверено мной)
- **Как заметить:** путь `/api/profile` отклоняется как mutating-GET (405) ещё в policy, поэтому ветка недостижима; внутри — `it != g_profiles.end()` после `LeaveCriticalSection` (гонка с `g_profiles.clear()` в reload).
- **Исправленный код:** ветка удалена (ни один клиент её не вызывает).

### [BA-42] Раннер тестов выбрасывал вывод упавших наборов

- **Где:** `run_all_tests.sh:33`
- **Severity:** P2 · **Статус:** подтверждено (скаут TestQuality; проверено мной)
- **Последствие:** красный билд без причины (для `assert`-наборов — без единой строки о том, что упало).
- **Исправленный код:** вывод пишется в `$OBJDIR/<name>.run.log`, при падении печатаются последние 20 строк (этой правкой и был найден BA-33).
- **Проверка:** сломанный assert в `command_queue` теперь виден в выводе раннера; на зелёном прогоне — 15/15.

### [BA-43] README против кода: порядок поиска тулчейна

- **Где:** `README.md:223` («looks for C:\MinGW64\bin\g++.exe first and falls back to g++ from your PATH») против `src/build.bat:7-11` (сначала `where g++`, затем `C:\MinGW64\bin`)
- **Severity:** P2 · **Статус:** подтверждено (проверено мной) · **Исправлено в `7fedf8f`** (в `742202f` правка была заявлена, но фактически не применилась — см. §4/§5)
- **Исправленный код:** «It uses `g++`/`windres` from your PATH first and falls back to `C:\MinGW64\bin` — either works as long as MinGW-w64 g++ is reachable.»
- **Осталось:** в `README.ru.md`/`README.zh.md` та же фраза в переводе — не правил, чтобы не ломать переводы без языковой вычитки (§6).
- **Дополнительно (не дефект, зафиксировано):** запуск `src\build.bat` из корня репозитория падает — `windres` получает `resources.rc` относительно CWD; это документировано (`cd src`), CI делает `Push-Location src`.

### [BA-44] Диагностика рендерилась без экранирования

- **Где:** `web/app.js:1309-1313` (`r.pipeId`, `r.vidpid`, `r.configFile` в `innerHTML`)
- **Severity:** P2 · **Статус:** подтверждено (скаут WebFrontend; проверено мной)
- **Исправленный код:** значения обёрнуты в `esc()` (кроме `r.device`, который приходит из фиксированных литералов сервера).

### [BA-45] JSON-хелперы — подстрочный сканер, а не парсер (5 дефектов)

- **Где:** `src/sidekick.cpp:3004` (`JsonGetStr`), `:3078` (`JsonGetInt`), `:3100` (`JsonGetBool`)
- **Severity:** P2 · **Статус:** подтверждено (скаут SidekickHttp)
- **Дефекты:** (1) ключ ищется первым вхождением подстроки — может совпасть внутри значения другого поля; (2) незакрытая строка читается до конца тела и считается успехом; (3) одиночный/битый суррогат кодируется в CESU-8, `JsonEscape` его не переэкранирует; (4) числа берутся `atoi`/`strtol` без границ; (5) `\u0000` становится настоящим NUL и обрезает значение при копировании в `snprintf`-буферы.
- **Последствие:** расхождение «что отправили / что сохранили» для крафт-запросов; инъекция в JSON невозможна (экранирование есть). Не исправлено — см. §6.

### [BA-46] `HttpSend` игнорирует результат `send()`

- **Где:** `src/sidekick.cpp:3140-3156`
- **Severity:** P2 · **Статус:** подтверждено (скаут SidekickHttp)
- **Последствие:** обрыв на 10-секундном `SO_SNDTIMEO` (большой HTML) не оставляет следа: клиент получает обрезанный ответ, сервер — ни строки в логе.

### [BA-47] SSE: клиент регистрируется после отправки revision, лимит 8 не атомарен

- **Где:** `src/sidekick.cpp:3351-3366` (порядок регистрации), `:3324-3331` (проверка лимита)
- **Severity:** P2 · **Статус:** подтверждено (скаут SidekickHttp)
- **Последствие:** редкая потеря первого revision-события (дашборд ждёт следующего) и мягкий лимит, который можно превысить двумя одновременными подключениями (каждый — поток + сокет).

### [BA-48] `204 No Content` отправляется с `Content-Length: 0`

- **Где:** `src/sidekick.cpp:3659` (`/favicon.ico` → `HttpSend(..., 204)`)
- **Severity:** P2 · **Статус:** подтверждено (скаут SidekickHttp) · RFC 7230 запрещает заголовок у 204; клиенты терпят.

### [BA-49] Лог растёт без границы и отдаётся через HTTP

- **Где:** `src/sidekick.cpp:114` (`Log`, append без ротации), `:4965-4986` (`/api/v1/diagnostics` читает файл ЦЕЛИКОМ и отдаёт последние 10 строк)
- **Severity:** P2 · **Статус:** подтверждено (скаут SidekickHttp)
- **Последствие:** рост файла и полное чтение его в память на каждый запрос диагностики.

### [BA-50] `WM_INPUT` аллоцирует буфер на каждое событие ввода

- **Где:** `src/sidekick.cpp:2222-2245` (`malloc(sz)`/`free` в обработчике raw input)
- **Severity:** P2 · **Статус:** подтверждено (проверено мной) · Буфер ≤4096 байт, но это горячий путь (каждое движение мыши).

### [BA-51] `probe_device`: непроверенные преобразования и разбор аргументов

- **Где:** `src/probe_device.cpp:115-118` (`MultiByteToWideChar` без проверки → `CreateFileW` по мусору), `:183-184` (guard `transferred >= 7` при чтении байтов 7-8), `:88-90`, `:217-231` (argv: только `argv[1]`, неизвестный флаг = фильтр, усечение фильтра/пути)
- **Severity:** P2 · **Статус:** подтверждено (scout WinTargets)

### [BA-52] Дашборд: 20+ мутирующих обработчиков без обработки ошибок; пустые catch превращают сбой в ложный диагноз

- **Где:** `web/app.js:158` (`api()` бросает на не-JSON), вызовы без `try/catch` — 240, 241, 242, 243, 244, 245, 246, 247-273, 382, 405, 425-426, 447-505, 507, 543, 609-612, 953, 1296-1300; конкретно «ложные диагнозы»: `:405-409` («No WinUSB keyboard found… switch the driver with Zadig» при недоступном API), `:425-438` («No keyboard activated»), `:1405-1416`, `:1490-1545` (Live навсегда в «Loading…» с продолжающимся опросом)
- **Severity:** P2 · **Статус:** подтверждено (скаут WebFrontend)
- **Последствие:** неудачный запрос выглядит как «ничего не произошло» или как неверная рекомендация переустановить драйвер.

### [BA-53] Опросы не останавливаются при переходе в Help; часть поллингов живёт после ухода с экрана

- **Где:** `web/app.js:804-809` (`stopAllPolling`) не вызывается из `showHelp` (`:1339`); продолжает работать `capture`-поллинг (`:286`), `identify` (`:764`), `zadig` (`:1121`)
- **Severity:** P2 · **Статус:** подтверждено (скаут WebFrontend)
- **Последствие:** до 10 запросов/с к `/api/capture/poll` после ухода с экрана (в связке с BA-01 это и был ускоритель утечки сокетов).

### [BA-54] Дашборд: мелкие функциональные дефекты (перечисление)

- **Где/что:**
  - `web/app.js:745` читает `e.mi` из событий identify, а сервер (`src/sidekick.cpp:4779-4785`) его не отдаёт — фид никогда не показывает `[MI_xx]`;
  - `web/app.js:485` и `:1472` не передают `profileId`, поэтому шаблон применяется только один раз (`profile already exists: agent-…`);
  - `src/sidekick.cpp:3672` инжектит revision строкой, SSE (`:3269`) — числом: `d.revision !== LAST_REVISION` всегда истинно, дедупликация мертва;
  - `src/sidekick.cpp:2565` (`DASH_DUPLICATE_KEY`) ищет источник только по `usage`, игнорируя `mod` (клиент присылает `mod`, но `web/app.js:245` его не отправляет) — дублируется не та строка Fn-слоя;
  - `web/app.js:509` один общий `#toast`: два тоста подряд гасят друг друга таймером;
  - `web/app.js:576-588` «Pick foreground app» = `setTimeout(2000)`, почти всегда ловит сам дашборд (готовый `POST /api/v1/windows/foreground/pick` не вызывается никем);
  - `web/app.js:640` «Restore original driver» меняет драйвер без подтверждения, хотя соседняя кнопка swap закрыта чек-листом.
- **Severity:** P2 · **Статус:** подтверждено (скаут WebFrontend) · Не исправлено — см. §6.

### [BA-55] Документация и цифры расходятся с кодом (перечисление)

- **Где/что:**
  - `presentation.html:252-256/445/638` учит `cd tests && run_all_tests.sh` — файла `tests/run_all_tests.sh` нет (раннер в корне и сам делает `cd "$(dirname "$0")"`); рядом три разных числа юнит-тестов (13 vs 14) и «53 integration» против 58 `assert` (CHANGELOG:42 говорит 58);
  - `docs/HID-USAGE-TABLE.md` — таблица numpad сдвинута/самопротиворечива в районе 0x5A;
  - `README.md:223` (см. BA-43), `docs/FAQ.md` (битый якорь `ZADIG_INSTRUCTIONS.md`, устаревшее «per-instance matching — future feature», отозванная инструкция по probe_device), `docs/PROBLEM-AND-SOLUTION.md` («One-time setup = Zadig swap» против self-swap без Zadig);
  - версия `0.9.6` продублирована в `src/sidekick.cpp:64`, `src/resources.rc`, `CHANGELOG.md`, `README×3` и не сверяется CI (`dist/make_dist.ps1` при неудаче парсинга молча берёт `0.9.0`).
- **Severity:** P2 · **Статус:** подтверждено (скаут BuildCiDocs) · Не исправлено — см. §6.

### [BA-56] Останов процесса: дренаж 3 с против законных 15 с у воркера

- **Где:** `src/sidekick.cpp:6045-6062` (`WaitForSingleObject(op->doneEvent, kDashOpTimeoutMs)` в `RunDashOp` vs `Sleep(100)×30` в main), затем `DeleteCriticalSection(&g_csProfile/&g_csRevision)`
- **Severity:** P2 (только выход) · **Статус:** подтверждено (скаут SidekickHttp; моя правка BA-06 добавила в тот же класс `g_csLedger` — он, как и `g_csLog`, намеренно не удаляется)
- **Последствие:** запоздавший воркер входит в удалённую секцию — UB на выходе.

### [BA-57] Тестовые баннеры печатают счёт, которого никто не измерял

- **Где:** `tests/app_instance_tests.cpp:157` (5/5), `config_v3_tests.cpp:359` (6/6), `input_ledger_tests.cpp:119`, `supervisor_tests.cpp:253`, `targeted_input_tests.cpp:163`, `windows_targets_tests.cpp:266`, `http_security_tests.cpp:205`
- **Severity:** P2 · **Статус:** подтверждено (скаут TestQuality) · Удаление/переименование кейса не меняет «5/5»; раннер эти строки раньше вообще не показывал (BA-42).

### [BA-58] `windows_targets::TestEnumerationSmoke` повторяет предикат фильтрации как собственное утверждение

- **Где:** `tests/windows_targets_tests.cpp:224-241` (`REQUIRE(IsWindowCandidateAllowed(*it, policy))` по списку, построенному тем же предикатом); там же живёт оживление мёртвого `LaunchApplication` (прод использует свой `LaunchApp` с CP_ACP и без квотирования пути)
- **Severity:** P2 · **Статус:** подтверждено (скаут WinTargets/TestQuality)

### [BA-59] `ScheduleTargetedRepeatTimer` читал очередь повторов без лока — гонка осталась на read-пути

- **Где:** `src/sidekick.cpp:2046` — `ScheduleTargetedRepeatTimer` (`g_targetedKeys.nextRepeatAt`)
- **Severity:** P1 · **Статус:** подтверждено (найдено на ревью собственного фикса BA-06) · **Исправлено в `7fedf8f`**
- **Путь достижимости:** `SendHoldableKeyToTarget`/`DispatchTargetedRepeats` → `ScheduleTargetedRepeatTimer` (main-поток) одновременно с `ReleaseAllTargetedKeys` (HTTP-воркер, `/api/v1/devices/detect` или `/devices/capture`) → `nextRepeatAt` обходит `std::vector`, который в этот момент очищается.
- **Последствие:** та же порча контейнера/падение, от которой защищал BA-06 — фикс был неполным: локались `dueRepeats`/`releaseAll`/`recordUp`/`owns`/`ownedKeysForRelease`, но не чтение дедлайна.
- **Текущий код:** `std::uint64_t deadline = 0; if (!g_targetedKeys.nextRepeatAt(&deadline)) return;`
- **Исправленный код:**
  ```cpp
      EnterCriticalSection(&g_csLedger);
      const bool hasRepeat = g_targetedKeys.nextRepeatAt(&deadline);
      LeaveCriticalSection(&g_csLedger);
      if (!hasRepeat) return;
  ```
- **Почему это баг, а не мелочь:** комментарий к фиксу BA-06 утверждает, что «весь доступ к ledger'ам сериализован»; это было неверно ровно в одном месте — на пути планирования таймера.

### [BA-60] `GET /api/v1/devices` отдавал обрезанный vidpid

- **Где:** `src/sidekick.cpp:4619` — обработчик `/api/v1/devices`
- **Severity:** P2 (неверный идентификатор в публичном API; поставленный UI использует `/api/v1/hid`) · **Статус:** подтверждено (найдено живой проверкой на реальном железе) · **Исправлено в `6ff6cb5`**
- **Как заметить:** на машине с устройством на WinUSB: `curl /api/v1/devices` → `"vidpid":"vid_0406&"` вместо `vid_0406&pid_2814`.
- **Последствие:** клиент, сопоставляющий устройства по vidpid (визард/список для multi-device), не может связать запись с данными `/api/v1/hid`.
- **Текущий код:**
  ```cpp
  const char* vid = strstr(dp, "vid_");
  if (vid) {
      const char* mi = strstr(dp, "&mi_");
      std::size_t vidEnd = mi ? (std::size_t)(mi - dp - (vid - dp)) : std::string::npos;
      if (vidEnd != std::string::npos) vidpid.assign(dp + (vid - dp), vidEnd - (vid - dp));
  }
  ```
- **Исправленный код:** `VidPidMiFromString(dp, vid, pid, mi); vidpid = "vid_" + vid + "&pid_" + pid;` — тем же разбором, что и в остальном файле.
- **Проверка:** живой сервер → `vidpid= vid_0406&pid_2814` (до правки — `vid_0406&`).

### [BA-61] `sidekick.exe --driver swap|restore|status` не находил ни одного устройства (две ошибки в одной функции)

- **Где:** `src/sidekick.cpp:5888` — `DrvFindNodes`
- **Severity:** P1 (сломана функция смены драйвера — центральный шаг настройки продукта; P0 не ставлю: падения/потери данных нет) · **Статус:** подтверждено (найдено и проверено на реальном железе) · **Исправлено в `6ff6cb5`**
- **Путь достижимости:** `sidekick.exe --driver status <vidpid>` (CLI) → `DrvFindNodes`; тот же код вызывает `/api/v1/driver/swap|restore` через элевированный self-spawn, поэтому не работали ни кнопка свапа в дашборде, ни восстановление после смены USB-порта.
- **Как заметить:** до правки на любой реальной машине:
  `sidekick.exe --driver status "vid_046d&pid_c548"` → `No present device nodes match vid_046d&pid_c548`, `exit=2`, хотя устройство присутствует (видно в `/api/v1/hid` как `needs-driver`).
- **Последствие:** пользователь подтверждает UAC, после чего инструмент сообщает «клавиатура не подключена?» и ничего не делает; функция выглядит рабочей в README/CHANGELOG.
- **Ошибка 1 (сбор needle):**
  ```cpp
  std::wstring hex;
  for (auto c : pat) if (iswxdigit(c)) hex += towlower(c);   // ловит 'd' из vid_/pid_!
  if (hex.size() < 8) return false;
  swprintf(vp, 64, L"USB\\VID_%c%c%c%c&PID_%c%c%c%c", hex[0]..hex[7]);
  std::wstring needle = vp;
  ```
  Для `vid_046d&pid_c548` набор — `d`+`046d`+`d`+`c548` = `d046ddc548`, needle = `USB\VID_D046&PID_DDC5` — совпадений не бывает.
- **Ошибка 2 (регистр):** needle содержал заглавные `USB\VID_`/`&PID_`, а сравнение шло с `DrvLowerW(hwid)` — даже с правильными цифрами совпадение было невозможно.
- **Исправленный код:** VID/PID разбирается структурно (`VidPidMiFromString`, тот же файл), needle целиком в нижнем регистре:
  ```cpp
  std::string vid, pid, mi;
  VidPidMiFromString(vidpid, vid, pid, mi);
  if (vid.size() != 4 || pid.size() != 4) return false;
  std::wstring needle = L"usb\\vid_";
  for (...) needle += towlower(vid[index]);
  needle += L"&pid_"; for (...) needle += towlower(pid[index]);
  ```
- **Проверка (реальное железо, живой билд):**
  `vid_046d&pid_c548` → 5 узлов (`usbccgp` / `HidUsb` ×3 / `WinUSB`), exit 0;
  `vid_0406&pid_2814` → 3 узла (в т.ч. `MI_00 service: WinUSB`), exit 0;
  `vid_aaaa&pid_bbbb` → «No present device nodes match», exit 2 (негативный контроль сохранился).
- **Почему это баг, а не решение:** `--driver status` — read-only диагностика, задуманная как способ проверить драйвер перед свапом; её сообщение «is the keyboard plugged in?» прямо противоречит наблюдаемому состоянию. Модуль не покрыт юнит-тестами, что и позволило обеим ошибкам дожить до релиза.

### [BA-62] Интеграционная проверка `swap without vidpid → 400` зависела от состояния машины (и могла запускать UAC)

- **Где:** `tests/http_integration_tests.sh:230` (до правки)
- **Severity:** P2 (тест) · **Статус:** подтверждено (воспроизведено на этой машине) · **Исправлено в `6ff6cb5`**
- **Как заметить:** на машине, где устройство из `DeviceVIDPID` присутствует как обычная клавиатура (после шага `activate` в самом скрипте), роут `/api/v1/driver/swap` с пустым телом корректно подставляет vidpid «сменившего порт» устройства и отвечает 200 → проверка `= 400` падает на рабочем продукте.
- **Дополнительный риск:** в этом состоянии роут запускает **элевированный** self-spawn (UAC) — автотест не должен этого делать.
- **Исправленный код:** проверка стала зависимой от состояния, а запрос при ожидающей смене порта не отправляется вовсе:
  ```bash
  if echo "$STATE_BODY" | grep -q '"portChangeDetected":true'; then
      SWAP_NOVID="skipped"
      echo "  - swap without vidpid: skipped (port change pending → documented fallback launches UAC)"
  else
      SWAP_NOVID=$(curl -s ... -d '{}')
  fi
  ```
- **Проверка:** прогон даёт `57 passed, 0 failed` (один check пропущен с явной пометкой); при отсутствии ожидающей смены порта — 58/58.

### [BA-63] `DrvPickForSwap`/`DrvPickForRestore` при неудаче возвращали `nodes[0]` — свап мог уйти на родительский узел устройства

- **Где:** `src/sidekick.cpp:5931` и `:5937` (до правки) — `DrvPickForSwap`, `DrvPickForRestore`
- **Severity:** P1 (смена драйвера — центральный шаг настройки; угадывание ломает клавиатуру) · **Статус:** подтверждено (данные с реального железа) · **Исправлено в `65d6540`**
- **Путь достижимости:** `/api/v1/driver/swap|restore` → элевированный `sidekick.exe --driver …` → `DrvFindNodes` → `DrvPickForSwap`; на этой машине список узлов для `vid_046d&pid_c548` — 5 штук, и `nodes[0]` = `USB\VID_046D&PID_C548&REV_0503` (service `usbccgp`, родитель составного устройства).
- **Последствие:** у устройства без узла `&MI_00` (или при иной раскладке интерфейсов) WinUSB привязывался бы ко всему устройству вместо интерфейса клавиатуры — клавиатура перестала бы печатать; `restore` так же «успешно» возвращал бы не тот узел.
- **Исправленный код:** приоритет `&MI_00` (swap) / service `WinUSB` (restore), затем — только если узел ровно один; иначе `NULL` и отказ:
  ```cpp
      DrvNode* n = restore ? DrvPickForRestore(nodes) : DrvPickForSwap(nodes);
      if (!n) {
          printf("Refusing to guess: %s matches several interfaces, none of them identifiable as the %s target.\n", ...);
          DrvPrintNodeList(nodes);
          return 3;
      }
  ```
- **Проверка (реальное железо):** `--driver status vid_046d&pid_c548` теперь печатает dry-run выбора — `swap would bind WinUSB to: …REV_0503&MI_00` (интерфейс клавиатуры, service HidUsb), `restore would return to HID: …REV_0503&MI_03` (интерфейс, который уже на WinUSB). Сама смена драйвера не выполнялась (требует UAC).
- **Почему это баг:** `nodes[0]` для составного USB-устройства — это узел композитного родителя; выбор «первый попавшийся» не имеет отношения к тому, какой интерфейс печатает.

### [BA-64] Сообщение «config_v3 reported N error(s)» считало все диагностики, включая warning'и

- **Где:** `src/sidekick.cpp:5350` (до правки)
- **Severity:** P2 (диагностируемость) · **Статус:** подтверждено (наблюдалось: «3 error(s)» при одной реальной ошибке) · **Исправлено в `65d6540`**
- **Последствие:** при разборе конфига по логу нельзя отличить ошибку от сохранённого неизвестного поля; теперь предохранитель стал рабочим (BA-30), и это сообщение — первое, что читает пользователь.
- **Проверка:** на мусорном конфиге лог показывает «reported 1 error(s)» вместо прежних «3 error(s)».

### [BA-65] Обратный слэш в конце комментария выключил строку таблицы scan-кодов — клавиша `;` перестала инжектиться

- **Где:** `src/sidekick.cpp:619` (до правки) — таблица `UsageToSet1`, строка комментария к клавише `\`
- **Severity:** P1 (клавиша молча не работает на обычном пути) · **Статус:** подтверждено (найдено предупреждением `-Wcomment`) · **Исправлено в `0b05ab2`**
- **Механика:** `// \` — обратный слэш последним символом строки склеивает её со следующей (фаза 2 трансляции), поэтому следующая физическая строка `{0x33,0x27,false}, // ;` целиком стала частью комментария. Запись для usage 0x33 исчезла из таблицы ещё на этапе компиляции.
- **Последствие:** `UsageToSet1(0x33)` возвращает scan = 0, `SendOneScan` молча пропускает клавишу — в basic-режиме `;` не печатается, в ledger не появляется. Ни компилятор без флагов, ни тесты этого не видели.
- **Исправленный код:** `{0x31,0x2B,false}, // backslash key` (без завершающего слэша).
- **Проверка:** `-Wcomment` исчез из вывода; строка `{0x33,0x27,false}` снова является кодом (видно в исходнике после правки).
- **Почему это баг, а не оформление:** это класс «код, выключенный текстом» — ровно то, что ищет линтер и что не ловит ни один тест.

### [BA-66] Power-resume с неудачным переоткрытием устройства: `WinUsb_ReadPipe(NULL, …)` вместо ожидания

- **Где:** `src/sidekick.cpp:5713` (до правки) — `ReadLoop`, ветка `g_powerResume`
- **Severity:** P1 (падение вместо деградации) · **Статус:** подтверждено (MSVC `/analyze` C6387 + чтение кода) · **Исправлено в `0b05ab2`**
- **Путь достижимости:** сон/пробуждение системы → `PBT_APMRESUMESUSPEND` → `WinUsb_AbortPipe` → в `ReadLoop` ветка `g_powerResume`: `CloseDevice()` (обнуляет `g_hWinUsb`), затем `if (!ReconnectDevice()) Sleep(2000);` — **без выхода из цикла** → следующая итерация вызывает `WinUsb_ReadPipe(g_hWinUsb = NULL, …)`.
- **Последствие:** падение процесса при resume, если клавиатура не переоткрылась (переткнута/занята); вместо этого приложение должно было уйти в событийное ожидание (оно есть в main-цикле).
- **Исправленный код:** при неудаче — `Log(...); BumpRevision(); break;` (как в ветке активации), плюс явная проверка инварианта в начале итерации и проверка `CreateEvent` на NULL.
- **Проверка:** MSVC `/analyze` больше не выдаёт C6387 ни для `ov.hEvent`, ни для `g_hWinUsb`; сборка OK, юнит 16/16, интеграция 58/58.

### [BA-67] Непроверенные хендлы и не терминированный `_snwprintf` (MSVC C6387/C6053)

- **Где:** `src/sidekick.cpp:5676` (`CreateEvent` без проверки), `:2914` (`_snwprintf` в трее)
- **Severity:** P2 (редкие условия: отказ создания события; усечение строки «About») · **Статус:** подтверждено (MSVC `/analyze`) · **Исправлено в `0b05ab2`**
- **Исправление:** выход из `ReadLoop` с логом, если событие не создано (иначе NULL попадает в массив хендлов `MsgWaitForMultipleObjectsEx`); явный terminator `about[255] = L'\0'` после `_snwprintf`.

### Инструментальная проверка (что вообще есть для этого языка)

- Ни `clang-tidy`, ни `cppcheck`, ни `eslint`, ни конфигов линтеров в репозитории нет; `build.bat` собирает без единого предупреждающего флага.
- Доступно без установки: `g++ -Wall -Wextra` (MinGW 16.2) и **MSVC `/analyze`** (Visual Studio 18 установлена на машине) — второй нашёл BA-66/BA-67, первый — BA-65.
- `-fanalyzer` (встроенный анализатор GCC) в этой сборке MinGW не скомпилирован: `unrecognized command line option`.
- JS проверен через `node --check web/app.js` → синтаксис в порядке (глубже — уже разобрано в BA-45/52/53).
- Итог прогона: 10 из 12 продакшн-единиц компилируются без предупреждений вообще; после правок `sidekick.cpp` даёт только `missing-field-initializers` (123×, шум `= {0}`), `probe_device.cpp` — 7 таких же.

## 4. Журнал решений

| Развилка | Что выбрал | Почему | Что отверг и чем оно хуже |
| --- | --- | --- | --- |
| В каком репозитории аудитить и чинить | `keysidekick-public` (HEAD `39a2460`, теги v0.9.1–v0.9.6, чистое дерево) | это канонический публикуемый репозиторий; рабочее дерево `keyboard-router` совпадает с ним по содержимому (побайтово — `sidekick.cpp`, `domain_model.cpp`, README, .rc, workflow), но его HEAD на 5 релизов позади, а 17 файлов — незакоммиченный copy-forward автора | чинить в `keyboard-router` — значит потерять историю релизов и трогать чужие незакоммиченные правки (запрещено скиллом) |
| Точка отката при чистом дереве | ветка `ultra-audit-2026-09-19` от `master`, правки тремя коммитами | позволяет вернуться и к состоянию до аудита, и к каждому шагу починки | stash/патч-файл — хуже: не даёт бисекции и теряет сообщения |
| Как проверять P0 (утечку сокетов) | снять фикс в `stash`, собрать «до», замерить `HandleCount` под нагрузкой, вернуть фикс, замерить снова | число показывает ровно +1 дескриптор на запрос — доказательство, а не рассуждение | верить чтению кода — оставляло риск, что закрытие есть в другом месте (его там нет) |
| Как чинить повторный запуск action-клавиши | вынести детект фронтов в чистый модуль `src/report_diff.h` и покрыть юнит-тестом, а не править выражения на месте | `BasicReinject`/`ProcessReport` в `sidekick.cpp` недостижимы для тестов (файл с `main` и WinUSB), а это ровно тот класс, где ошибка стоила денег; стиль репозитория — чистые модули + `tests/` + раннер | правка внутри `BasicReinject` без теста — «исправлено, но никем не проверено навсегда» |
| Что считать правдой про порт | живой порт listener'а (`g_httpPortBound`) для policy/URL/диагностики, значение файла — для следующего старта | listener не переезжает; policy обязана совпадать с сокетом | перезапуск сервера при импорте — меняет семантику «import» на «рестарт» и рвёт текущие соединения |
| Как чинить «мутирующий GET» | сделать `/api/v1/devices/detect` POST-ом и свести два списка в один | GET с побочными эффектами нельзя защитить токеном, не сломав семантику; список путей — одна сущность | оставить GET и «задокументировать» — сохраняет дыру; добавить путь в оба списка — оставляет дублирование, которое уже разъехалось |
| Что делать с фантомным профилем | создавать через `ProfileService` (domain), а не запретить создание | форма «новый профиль» в клиенте опирается на create-on-miss; правка сохраняет сценарий и убирает расхождение источников | вернуть 400 на неизвестное имя — ломает создание профиля из формы без правки трёх клиентских мест |
| Провал записи конфига | сделать ошибкой операции (`op->success=false`, 500 в роут-е), а не отдельным статусом | дашборд уже умеет показывать `error` из ответа; любая другая форма требует нового контракта | тихо писать в лог — ровно то, что было |
| Импорт конфига | валидировать `Parse(...).ok()` и писать под `g_csProfile`, лок отпускать до `RunDashOp` | иначе мусорный файл затирает рабочий, а удержание лока через `RunDashOp` даёт дедлок с main-потоком | писать без лока — уже проверено, что даёт гонку с `WriteConfig` |
| XSS | заменить `esc()` на существующий `jsStr()` в трёх местах | правило «одна строка в inline-обработчике» уже сформулировано в комментарии `jsStr` и верно применяется в двух соседних функциях | переписать рендер на DOM-API — крупный рефакторинг всей вьюхи, вне объёма фикса |
| Правка клиента «Save» при смене имени | сначала `profile/rename`, затем сохранение настроек | сохраняет видимое поведение («сохранил») и убирает дубликат | серверная эвристика «имя изменилось → переименовать» — делает `POST /api/profile` неидемпотентным и мешает create-on-miss |
| Флейк `command_queue` | оставить находкой, не «чинить» | модуль не входит в сборку продукта; под инструментацией не воспроизводится (0/60), а правка наугад в тесте — подгонка | убрать assert — запрещено правилами скилла (ослабление теста) |
| Что не трогать | `keyboard-binding/`, `tmp/`, вендорные zip, `keyboard-router` | не собирается/не линкуется, чужие правки | «заодно» чистить чужие деревья — за пределами мандата |
| Ревью-проход по своим фиксам (что проверял заново) | перечитал тела `ReleaseTargetedUsage`, `ScheduleTargetedRepeatTimer`, ветку `hasErrors`, README-дифф, оба сайта `shortPath`, `CoUninitialize` | правки вносились fuzzy-редактором, и он дважды применил не то, что задумано | доверять статусу «success» — уже приводило к записи «исправлено» там, где правки не было |
| Утверждение «`State::completed()` читает completionStatus без лока — гонка» | **отвергнуто** | `completed()` объявлен у `Outcome` (`src/command_queue.h:89-100`) — это самостоятельный value-объект, живущий в копии вызывающего; `State::completionStatus` пишется под `mtx` в `completeSuccess`, а читается под локом в `Request::status()` | принять как находку — значит записать в отчёт дефект, которого нет |
| «Флейк `command_queue` — гонка в очереди» | **отвергнуто в пользу шима** | проверка гипотезы пробником: `join()` возвращался до выполнения потоков (счётчик 0..9 из 16), потому что `std::thread` шима был копируемым и владел сырым `HANDLE` | «оставить как необъяснимый флейк» — оставляло бы 12 конкурентных тестов, ничего не проверяющих |
| Приоритеты владельца: «действуем как в репозитории сейчас»; нужен уверенный детект на старте подключения и лёгкое переключение драйвера | BA-28 закрыт через существующий `LaunchPolicy` (а не новой поле в схеме), BA-29 — возвратом extensions в `Config` перед Serialize; проверки велись на реальном железе (`--driver status`, `/api/v1/hid`, `/api/v1/devices`, активация устройства) | в репозитории уже есть и понятие (`LaunchPolicy`), и комментарий-обещание «Phase 3 добавит launchPolicy → autoStart», и парсер/писатель extensions; новая сущность добавила бы вторую правду | вариант «добавить в domain отдельный bool autoStart» — разошёлся бы с `LaunchPolicy`, который уже используют policy-поля профиля |
| BA-32 (`StartupManager`) и BA-30 (мёртвый legacy-парсер) | оставлены как есть, статус задокументирован | владелец: «действуем как в репозитории сейчас» — в поставке автозапуск делает ярлык в папке Startup, и этот путь работает; удаление 700-строчного модуля с тестами — отдельное решение | подключить Task Scheduler-путь к продукту — меняет поведение автозапуска без запроса; удалить молча — потерять готовую реализацию |
| Проверка `swap without vidpid` в интеграционном наборе | сделана зависимой от состояния с пропуском вместо запроса при ожидающей смене порта | роут корректно уходит в fallback и запускает элевированный процесс (UAC) — автотест не должен поднимать UAC на машине разработчика | оставить `= 400` — красно на любой машине с подходящей клавиатурой; ожидать 200 — тест поднимал бы UAC |
| Ошибка в моём же отчёте | зафиксирована явно: `742202f` был описан как содержащий правку README, которой в коммите нет (`git status` показывал только 7 файлов; в коммит попал `README.md` в списке файлов, но само изменение не применилось) | отчёт, расходящийся с деревом, хуже отсутствующего пункта | тихо дописать правку в следующий коммит — теряет след аудита |

## 5. Журнал правок

| Коммит | ID багов | Файлы | Проверка (команда и вывод) |
| --- | --- | --- | --- |
| `9d87d79` | BA-01 | `src/sidekick.cpp` | **падало до:** `handles 209 → 509 → 809` (0/300/600 ×`GET /api/status`), т.е. +1 дескриптор на запрос; **проходит после:** `handles 201 → 201 → 202`, `netstat … CLOSE_WAIT` = 0 |
| `87f94eb` | BA-02…BA-17, BA-39, BA-40, BA-41 | `src/sidekick.cpp`, `src/report_diff.h` (новый), `tests/report_diff_tests.cpp` (новый), `run_all_tests.sh` | **новый тест до фикса-логики:** моя первая версия теста падала на шаге «удерживаемая клавиша не новый фронт» — тест чувствителен; **после:** `report_diff: 22 checks, 0 failed`; полный набор `15 passed, 0 failed` (дважды); интеграция `58 passed, 0 failed`; **запись конфига:** read-only `config.ini` → `HTTP 500`, вернув права → `HTTP 200 {"ok":true,...}` |
| `742202f` | BA-18, BA-19, BA-20, BA-21, BA-22, BA-23, BA-24, BA-42, BA-44 | `src/sidekick.cpp`, `src/http_security.cpp`, `tests/http_security_tests.cpp`, `tests/runtime_storage_tests.cpp`, `web/app.js`, `run_all_tests.sh`, `dist/make_dist.ps1` | **XSS в браузере:** старая формула → `firedWithOldEscaping: true`; `jsStr` → `xssFired: false` (кнопка ✎ найдена по `onclick^=editMapping`), `actionsMatch: true`; **контракт detect:** `GET → 405`, `POST без токена → 403`, `POST с токеном → 200 {"detected":[]}`; **recursive-проверка:** `POST activate {"name":"nope-not-here"} → 404`; юнит-набор `15/15`; интеграция `58/58`. **Поправка:** BA-43 (README) в этом коммите заявлен, но не применён — вошёл в `7fedf8f`. |
| `f931b18` | — | `BUGSAUDIT-2026-09-19.md` | отчёт (документ), проверок не требует |
| `6ff6cb5` | BA-28, BA-29, BA-60, BA-61, BA-62 | `src/config_domain_bridge.cpp`, `src/sidekick.cpp`, `src/startup_manager.h`, `tests/config_domain_bridge_tests.cpp`, `tests/http_integration_tests.sh` | **AutoStart:** живой `/api/profiles` → `aimp autoStart=True`, после API-правки в файле `AutoStart=1`, `autoStart:false` → `AutoStart=0`; **extensions:** `[Sample] Kept=yes` → `[Extension.0001]`; **драйвер (CLI, реальное железо):** `vid_046d&pid_c548` → 5 узлов, exit 0; `vid_0406&pid_2814` → 3 узла (MI_00 WinUSB), exit 0; `vid_aaaa&pid_bbbb` → exit 2; до правки — «no nodes» на всех трёх; **vidpid:** `/api/v1/devices` → `vid_0406&pid_2814`; unit 16/16, интеграция 57 passed/0 failed |
| `a553a68`, `190a7cb` | BA-22 (класс), BA-57 | `tests/config_v3_tests.cpp` | **Проверка (по коду выхода самого бинарника):** A) реальный `src/config.example.ini` → `exit 0` (7/7); B) `KSK_EXAMPLE_CFG=src/__no_such_file__.ini` → `exit 1` с `FAILED: … exampleParsed`; C) `KSK_EXAMPLE_CFG=src` (каталог) → `exit 1`. Проверка сделана файловой, а не «что-то распарсилось»: `is_open()` + признаки примера (`[Application.*]`, непустой `DeviceVIDPID`), потому что `Parse("")` сам по себе даёт `ok()` и ≥1 профиль. **Отдельная находка о среде:** harness-овые файловые операции (`mv`/`ls`/`rm`) и нативные Windows-программы расходились в представлении ФС (файл, «удалённый» для harness, был виден нативному `dir`; одна правка исходника не дошла до компилятора) — поэтому от «спрятать файл» контроля пришлось отказаться в пользу переменной окружения, а итоговое состояние дерева перепроверено нативным `dir` + `git status`. |
| `0b05ab2` | BA-65, BA-66, BA-67 | `src/sidekick.cpp` | **BA-65:** `-Wcomment` исчез, строка `{0x33,0x27,false}` снова код (до правки компилятор склеивал её с комментарием); **BA-66/67:** MSVC `/analyze` больше не выдаёт C6387 (NULL `ov.hEvent`/`g_hWinUsb`) и C6053 (`_snwprintf`); сборка OK, юнит 16/16, интеграция 58 passed / 0 failed |
| `65d6540` | BA-30, BA-63, BA-64 | `src/config_v3.cpp`, `src/sidekick.cpp`, `tests/config_v3_tests.cpp` | **BA-63:** `--driver status vid_046d&pid_c548` → `swap would bind WinUSB to: …&MI_00` / `restore would return to HID: …&MI_03` (до правки при отсутствии MI_00 выбирался бы `nodes[0]` = `usbccgp`); **BA-30/64:** мусорный конфиг → лог `reported 1 error(s), falling back to legacy parser` + `ERROR: config contains no recognized sections`, приложение стартует, файл не перезаписан; юнит-кейс «rejects files without recognized sections» (7/7 в config_v3); unit 16/16, интеграция 58 passed/0 failed |
| `7fedf8f` | BA-33, BA-43, BA-59 | `src/mingw_threading.h`, `src/sidekick.cpp`, `tests/mingw_threading_tests.cpp` (новый), `run_all_tests.sh`, `README.md` | **BA-33 (корневая причина):** пробник 16 потоков → `counter=0,8,9,2,6,5` до фикса и `counter=16` в 6/6 после; `command_queue.exe` — 13/60 падений до, **0/80** после; новый набор `mingw_threading: 9 checks, 0 failed` (5 прогонов); **BA-59:** лок вокруг `nextRepeatAt`; юнит-набор `16 passed, 0 failed`; продукт собран (`Build OK`), интеграция `58/58` |

Отклонение от плана скилла: правки разбиты на четыре кодовых коммита (P0; ввод+состояние/персистентность; безопасность+SSOT+тесты; шим потоков+доки) плюс два коммита отчёта вместо «одна группа — один коммит» — внутри групп файлы пересекаются (`sidekick.cpp`), поэтому раздельные коммиты по каждому багу требовали бы искусственного дробления и давали бы нерабочие промежуточные состояния.

## 6. Не сделано

| ID | Почему не сделано |
| --- | --- |
| BA-26 | Нужна политика: сообщать ли пользователю о нечитаемом процессе окна-цели (баннер/лог) и как менять скоринг. Правил только худшую часть (BA-25). |
| BA-27 | Корректный фикс требует хранить идентичность окна (класс+pid+threadId) в `TargetedKey` и ревалидировать её на каждом повторе — правка схемы `targeted_input` + hot path; в текущий объём не влезла без риска. |
| BA-28 | **Сделано** в `6ff6cb5` через существующий `LaunchPolicy` (см. §3 и §4). |
| BA-29 | **Сделано** в `6ff6cb5`: extensions возвращаются в `Config` перед `Serialize` (носитель им не нужен — они принадлежат файлу, а не домену). |
| BA-30 (остаток) | Класс «не распознано ничего» закрыт в `65d6540`. Осталось: файл в UTF-16 без BOM/с иной кодировкой по-прежнему пройдёт через парсер как «нет знакомых секций» — это уже даёт ERROR и fallback, но корректного диагноза «это UTF-16» в логе нет. |
| BA-31 | Правильный фикс — разбирать pass-through строки действий (`!switch:`/`!app:`) в структурные ссылки; это редизайн модели действий, а не правка. |
| BA-32 | По решению владельца оставлен как есть; статус зафиксирован прямо в `src/startup_manager.h` (модуль не линкуется в `sidekick.exe`, автозапуск делается ярлыком в папке Startup). Подключение Task Scheduler-варианта или удаление — по-прежнему решение владельца. |
| Проверка «поставляемый пример валиден» (BA-22-класс) | **Сделано** в `a553a68`/`190a7cb`/`15628f6`-цикле: кейс разбирает `src/config.example.ini`, требует `is_open()` и признаков примера, а при отсутствии файла падает (`REQUIRE(exampleParsed)`); негативный контроль воспроизводим без правок кода: `KSK_EXAMPLE_CFG=src/__no_such_file__.ini ./.ai-cache/config_v3.exe` → exit 1. |
| BA-33 | **Сделано** в `7fedf8f`: корневая причина оказалась в шиме `src/mingw_threading.h` (копируемый `thread` с сырым `HANDLE` → `join()` ждал закрытый хендл), а не в очереди; шим исправлен на move-only, добавлен `tests/mingw_threading_tests.cpp`. |
| BA-34 | Осмысленный ответ требует отличать «конфиг не прочитан» от «прочитан, но пуст»; после BA-11 ложный успех уже не приводит к тихой подмене набора профилей. |
| BA-35 | Нужна поддержка E1-префикса в таблице scan-кодов — отдельная работа. |
| BA-36 | Пробрасывание результата `SendInput` до ledger'а требует решения «что считать владением» при отказе инъекции; оставлено как есть. |
| BA-37, BA-38, BA-51 | Файл `probe_device.cpp` — диагностическая утилита вне основного пути; правки в нём не проверяются без реального WinUSB-устройства (в этой среде его нет). Все три подтверждены по коду скаутом. |
| BA-45, BA-46, BA-47, BA-48, BA-49, BA-50, BA-52…BA-58 | P2-класс: перечислены поимённо, исправлены только те, что давали измеримый эффект (BA-39/40/41/44) или скрывали причину отказа (BA-42). Dоковая группа BA-55 требует языковой вычитки переводов (ru/zh) — не делал, чтобы не ломать переводы машинной правкой. |
| README.ru.md / README.zh.md (BA-43) | Английский README исправлен (`7fedf8f`); переводы оставлены с прежней формулировкой о порядке тулчейна: правка переводов без носителя языка — хуже, чем зафиксированный дефект. |
| `keyboard-router/` (второй tree, 17 изменённых файлов) | **Чужие незакоммиченные правки** (copy-forward из публичного репо). Не трогал, не коммитил, не откатывал. После аудита дерево осталось ровно таким, каким было. |
| `keyboard-binding/`, `tmp/`, `Interception.zip`, `DEEP_RESEARCH_REQUEST.md`, PDF | Вне продукта: не собирается, не линкуется, продукт на них не ссылается. В обход не включал (см. §2). |

## 7. Подготовлено, но не выполнено

| Что требуется | Чем готово (файл / команда) | Чего ждёт |
| --- | --- | --- |
| Обновить второй рабочий tree (`keyboard-router`) до пропатченного состояния | все три коммита в `keysidekick-public` (`git -C keysidekick-public log --oneline -3`); при желании — `git -C keyboard-router fetch ../keysidekick-public` (локальный путь) | решения владельца: он один знает, зачем в этом дереве лежит незакоммиченный copy-forward (17 файлов) |
| Показать красный CI на флейке `command_queue` | **снято:** причина найдена и исправлена (`7fedf8f`) — копируемый `std::thread` в `src/mingw_threading.h`; воспроизведение для регрессии: `for i in $(seq 1 80); do ./.ai-cache/command_queue.exe >/dev/null 2>&1 || echo fail; done` (после фикса 0/80) | ничего |
| Навести порядок в показателях тестов в `presentation.html` (13/14/53/58) | точные числа: 15 наборов в `run_all_tests.sh`, 58 `assert` в `tests/http_integration_tests.sh` | языковой вычитки переводов; правка в трёх языковых версиях одного файла |
| Включить предупреждения как постоянный гейт | Команда готова (ничего ставить не нужно): `cd src && for f in *.cpp; do g++ -O2 -D_WIN32_WINNT=0x0600 -Wall -Wextra -Wno-missing-field-initializers -fsyntax-only $f; done` — сейчас чисто; для MSVC-анализатора готов `tmp/msvc_analyze.bat` (vcvars64 + `cl /c /analyze /std:c++14 /EHsc /D_WIN32_WINNT=0x0600 /DNOMINMAX`) | решения владельца: добавлять ли флаги в `build.bat`/CI (меняет вывод сборки и её время). `-Wno-missing-field-initializers` нужен именно как исключение: 123 места — это `= {0}`/`{ sizeof(x) }` по Win32-структурам, где массовая замена на `{}` обнулила бы, например, `SP_DEVINFO_DATA.cbSize` |
| Установить clang-tidy/cppcheck | Не установлено и не требуется для текущего уровня проверок | если нужно — это внешняя зависимость: пакет (MSYS2 `pacman -S mingw-w64-x86_64-clang-tools` / `cppcheck`), ставить не буду без явного «да» |
| Применить правки к опубликованному релизу | тег не ставился; `dist/make_dist.ps1` исправлен (приватный `config.ini` больше не попадает в zip) | явного решения о выпуске новой версии (тег + CI) — действие вне репозитория, не выполнял |

## 8. Закрытие областей: где находок нет

| Область | Проверено, находок нет — потому что |
| --- | --- |
| Пути/файлы из HTTP | ни один роут не принимает путь: читается/пишется только `CONFIG_FILE`/`LOG_FILE` (константы, резолвятся один раз в `ResolveDataPaths`), дашборд — компайл-тайм массив; траверсала невозможна. |
| Шелл/SQL-инъекции | SQL нет вовсе; `ShellExecuteW runas` получает `vidpid` только после посимвольного allow-list (`isalnum`, `&`, `_`, `-`), exe — из `GetModuleFileNameW`; проверено тестом (`swap with bad vidpid format → 400`). |
| Секреты в коде/логах/URL | CSRF-токен — 32 байта системного RNG, hex; логируется только длина, в URL не попадает, сравнение константное (`ConstantTimeTokenEquals`). |
| Аудио/реестр/иконки: утечки хендлов | сквозной аудит acquire/release по `windows_targets.cpp`, `probe_device.cpp`, `http_security.cpp` — 11 ресурсов, все освобождаются на всех путях (таблица в отчёте скаута WinTargets); мои правки добавили только `g_csLedger`, который, как `g_csLog`, не удаляется намеренно. |
| Утечки памяти в горячем пути | буферы фиксированы, контейнеры ограничены (`g_activityEvents` ≤ 24, `g_identifyEvents` ≤ 16, SSE ≤ 8 + лимит воркеров 8); единственные неограниченные сущности — лог (BA-49) и дескрипторы (BA-01, исправлено). |
| Состояние в localStorage vs сервер | `ks_onboarded` — чисто UI-флаг, `ks_prep_state` — локальный гейт с явным «продолжить всё равно»; кэшированных серверных значений нет. |
| Утечки слушателей/DOM в дашборде | один общий `_modalKeyHandler` снимается в `closeModal`, тикер и фид идентификации рендерятся срезами `slice(-10)`/`slice(-12)`. |
| Кодировки/время/локаль в конфиге | парсер и писатель симметрично экранируют (`\n`, `\r`, `=`, ведущие пробелы), строки читаются/пишутся в UTF-8, время нигде не сериализуется; тайминги — монотонный `GetTickCount64`. |

