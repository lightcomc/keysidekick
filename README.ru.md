[English](README.md) · [Русский](README.ru.md) · [简体中文](README.zh.md)

# KeySidekick

> Превратите любую запасную клавиатуру в выделенного фонового ассистента — управляйте медиаплеерами, DAW, OBS и не только, не теряя фокус на том, чем заняты.

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows%2010%2B-blue.svg)](#system-requirements)
[![Language: C++](https://img.shields.io/badge/Language-C%2B%2B14-orange.svg)](#building-from-source)

<p align="center"><img src="images/preview.webp" alt="«KeySidekick — запасная клавиатура становится пультом управления»" width="100%"></p>

Вторая клавиатура (или нумпад, или макропад) становится верным **сайдкиком** — выделенным пультом для одного или нескольких приложений, который работает в фоне и никогда не крадёт фокус. Основная клавиатура продолжает работать как обычно. Переключайте, чем управляет сайдкик, на лету: через системный трей, выделенные клавиши или встроенный веб-дашборд.

---

## Проблема, которую решает проект

Вы хотите, чтобы одна конкретная клавиатура управляла одним конкретным приложением **без** попадания её клавиш в активное окно — и чтобы можно было на лету переключать, каким приложением она управляет.

Звучит просто, но Windows сопротивляется на каждом шагу:

| Подход | Различает клавиатуры | Блокирует клавиши | Задержка |
|---|---|---|---|
| `WH_KEYBOARD_LL` hook | ❌ нет ID устройства | ✅ да | ❌ блокирует весь системный поток ввода |
| Raw Input (`WM_INPUT`) | ⚠️ только если `hDevice != NULL` | ❌ read-only | ✅ нет |
| Interception (kernel filter) | ✅ | ✅ | ✅ но kernel-драйвер, **риск зависания при hibernate** |

И есть неприятная ловушка: **дешёвые / композитные клавиатуры (клавиатура + тачпад) в Raw Input отдают `hDevice == NULL`**, так что даже Raw Input не может отличить их от основной — обе выглядят анонимно. Гибрид «Raw Input для идентификации + LL hook для блокировки» тогда лагает всю систему или даёт ложные срабатывания.

## Решение

**WinUSB Driver-Replacement Bypass.** Интерфейс клавиатуры отвязывается от системного клавиатурного стека (заменяется встроенным `WinUSB.sys` от Microsoft через [Zadig](https://zadig.akeo.ie/)). Её клавиши вообще перестают попадать в очередь ввода Windows — они не могут дойти ни до какого окна. KeySidekick читает сырые HID-отчёты напрямую через WinUSB и диспетчеризует их по активному **профилю**.

Это обходит все ограничения выше:
- ✅ Нет LL-хука → нет лага системы
- ✅ Работает даже при `hDevice == NULL` (мы читаем устройство напрямую, а не через Raw Input)
- ✅ Нет kernel-фильтра — только WHQL-подписанный WinUSB от Microsoft
- ✅ Переживает hibernate/sleep (WinUSB нативно работает с ACPI)

Подробное техническое описание: [`docs/PROBLEM-AND-SOLUTION.md`](docs/PROBLEM-AND-SOLUTION.md).

---

## Возможности

- **Профили** — несколько конфигураций, переключение на лету
- **Два режима на профиль:**
  - `basic` — выделенная клавиатура **печатает как обычно** в сфокусированное окно (через реинжект `SendInput`). Полезно, когда нужно временно использовать её как обычную клавиатуру.
  - `targeted` — клавиши направляются в окно выбранного приложения через `PostMessage`, никогда не попадая на передний план; одиночные маппинги сохраняют key-down, автоповтор и key-up
- **Multi-app маршрутизация** — внутри одного профиля разные клавиши могут целиться в *разные* приложения (`!app:Spotify:{Media_Play_Pause}` отправляет в Spotify, остальное идёт по профилю)
- **Комбинации клавиш** — `Ctrl+Shift+1`, `Alt+Q` и т.п. как триггеры (например `USAGE_1E+Ctrl+Shift=!switch:aimp`)
- **Web Dashboard** — встроенная панель управления на `http://127.0.0.1:8765/`:
  - Создание, переименование, дублирование, удаление профилей (без правки INI)
  - Визуальный выбор приложения — выбор цели из запущенных окон
  - Конструктор действий с чипами (keys, media, switch, launch, multi-app)
  - Страница диагностики (состояние устройства/драйвера/конфига, свежий лог)
  - Страница Help/Setup (онбординг, решение проблем, откат драйвера)
  - Живые обновления через SSE (без ручного обновления страницы)
- **Способы переключения профилей на лету:**
  - **Клавиши** на выделенной клавиатуре (токены действий `!switch:`/`!toggle:`)
  - **Иконка в системном трее** — левый клик → дашборд; правый клик → меню с профилями и индикаторами режимов
  - **Локальный HTTP API** на `127.0.0.1` — CSRF-защищённый, с SSE
- **Надёжность:**
  - Постоянный цикл устройства — приложение живёт, даже если клавиатура отключена; в простое событийная модель (без поллинга, ноль дисковых/CPU-нагрузок, пока ничего не происходит)
  - Singleton-mutex — никаких дублирующихся экземпляров
  - Журнал владения инжекцией — отпускает только те клавиши, которые сам и инжектил, никогда не трогает модификаторы основной клавиатуры
  - Атомарная запись конфига — temp → валидация → замена с бэкапом
  - Безопасно для hibernate/sleep — автопереподключение после пробуждения
  - Автозапуск целевых приложений, если их окно не найдено
- **Автозапуск с Windows** — настраивается через дашборд или API

## Известные ограничения (прочитайте перед использованием)

- **Basic-режим не работает в играх / с античитом.** `SendInput` помечает события флагом `LLKHF_INJECTED`; игры вроде CS:GO, Valorant, защищённые EAC тайтлы обнаруживают и игнорируют (или банят) инжектированный ввод. Basic-режим отлично подходит для браузеров, редакторов, офисных приложений, терминалов. Если нужна совместимость с играми, клавиатура должна остаться на родном HID-драйвере (не используйте basic-режим этого инструмента — используйте `targeted`-профиль или верните драйвер через Zadig).
- **Удержание в targeted работает только для приложений на сообщениях.** Фоновое удержание/повтор доставляется как `WM_KEYDOWN`/`WM_KEYUP`. Приложения, опрашивающие `GetAsyncKeyState`, DirectInput или Raw Input, могут его игнорировать.
- **Только Windows 10 (1809+).** Используются WinUSB и современный SetupAPI.
- **Одноразовая настройка Zadig обязательна** — заменяет драйвер интерфейса клавиатуры (обратимо; см. [ZADIG_INSTRUCTIONS.md](ZADIG_INSTRUCTIONS.md)).

---

## Скачивание / установка

1. Скачайте ZIP последнего релиза (файл `KeySidekick-<version>.zip`) со страницы Releases проекта на GitHub.
2. **Проверьте SHA256-контрольную сумму** архива по значению, опубликованному вместе с релизом.
3. Распакуйте архив в любое место — установка не требуется — и запустите `run.bat` (или `sidekick.exe` прямо из распакованной папки).
4. Откройте http://127.0.0.1:8765/ в браузере.
5. Используйте **+ Setup keyboard** для одноразовой замены драйвера через Zadig, затем **+ Pad template** для создания первой управляющей панели.

Полное пошаговое руководство — в разделе [Быстрый старт](#quickstart).

---

## Быстрый старт

### 1. Получите бинарник

Скачайте последний релизный архив со страницы [Releases](#download--install), **или** соберите из исходников (ниже).

### 2. Найдите свою клавиатуру и замените драйвер (один раз)

Откройте дашборд (`http://127.0.0.1:8765/`) → **+ Setup keyboard**. До Zadig ваша клавиатура — **обычная**, и мастер стартует отсюда:

1. Перечисляет **все** устройства ввода — клавиатуры, мыши, смарт-устройства с клавиатурой — независимо от драйвера, с их состоянием (`Normal keyboard (HidUsb)` vs `WinUSB — ready`).
2. **Нажмите клавишу для определения**, какой VID/PID у вашей клавиатуры (Raw Input, работает и на обычных клавиатурах). Для композитных устройств Windows может не дать идентичность по нажатиям — тогда выбирайте по имени / VID / PID в списке.
3. **Подготовка перед заменой**: проверьте, что клавиатура печатает, включите автозапуск KeySidekick (**после замены клавиатура печатает, только пока запущен sidekick.exe**), держите запасной способ ввода и убедитесь, что MI_00 — это клавиатура (MI_01 не трогайте — это мышь/тачпад).
4. Пошаговый **гид по Zadig** для вашего точного VID/PID — мастер следит, когда устройство переключится на WinUSB, и предупреждает о ловушке с phantom-копией (`Reinstall Driver`, если оно всё ещё печатает как обычно).
5. **Проверьте**, что KeySidekick ловит клавиши, затем сделайте клавиатуру активной (пишет `DeviceVIDPID` в конфиг).

Эквивалент вручную:

1. Скачайте **Zadig** с <https://zadig.akeo.ie/> и запустите **от имени администратора**.
2. **Options → List All Devices** ✓ (отметьте).
3. Найдите интерфейс `MI_00` (клавиатура) вашей клавиатуры — например `USB Input Device (VID xxxx PID yyyy) [MI 00]`. **Не** выбирайте `MI_01` (это часто тачпад/мышь).
4. Целевой драйвер: **WinUSB (Microsoft)** (используйте стрелки).
5. Нажмите **Replace Driver** → дождитесь «SUCCESS».

Полные шаги + как не сломать не тот интерфейс + откат: [`ZADIG_INSTRUCTIONS.md`](ZADIG_INSTRUCTIONS.md).

### 3. Настройка

Скопируйте `src/config.example.ini` в `src/config.ini` и отредактируйте. Минимум (текущая **схема v3** — `SchemaVersion=3`, профили в секциях `[Profile.<name>]`):

```ini
[General]
SchemaVersion=3
DeviceVIDPID=vid_xxxx&pid_yyyy     ; your device (see Zadig / Device Manager)
DefaultProfileId=basic
HTTPPort=8765
HTTPEnabled=1
TrayEnabled=1
EnableLog=1

[Application.aimp]
Name=AIMP
TargetClass=TAIMPMainForm           ; window class of the target app
TargetExe=AIMP.exe
TargetPath=C:\\Program Files (x86)\\AIMP\\AIMP.exe
AutoStart=1

[Profile.aimp]
Mode=targeted
ApplicationId=aimp                  ; link to [Application.aimp] above

[Profile.aimp.Mappings]
USAGE_14={F1}                       ; physical Q → F1
USAGE_1E=1                          ; physical 1 → 1
USAGE_29=!switch:basic              ; Esc → switch to basic (types normally)
USAGE_1E+Ctrl+Shift=!switch:aimp    ; Ctrl+Shift+1 → switch to aimp
```

Найдите VID/PID своего устройства в Zadig или через `Get-PnpDevice -Class Keyboard`. Класс окна целевого приложения — с помощью [Spy++](https://learn.microsoft.com/en-us/visualstudio/debugger/introducing-spy-increment) или PowerShell (см. [FAQ](docs/FAQ.md)).

### 4. Запуск

```
cd src
run.bat
```

Или напрямую: `sidekick.exe` (из каталога, где лежит `config.ini`).

### 5. Управление

- **Web Dashboard** — откройте `http://127.0.0.1:8765/` в браузере. Левый клик по иконке в трее тоже открывает его.
  - Создание, переименование, дублирование, удаление профилей (без правки INI)
  - **+ Agent pad** — пресет в один клик: превращает запасную клавиатуру в панель управления AI-кодингом (Codex / Claude / Cursor / Devin / ChatGPT): accept, cancel, branch, sidebar, voice, prompt-history, media — как Stream Deck / Codex Micro, но на вашей существующей клавиатуре.
  - **Pad templates (use-case)** — готовые профили F1–F12 для медиа, OBS, созвонов, PowerPoint, **REAPER**, **DaVinci Resolve**, **Ableton Live**, **Adobe Premiere**, **Lightroom Classic** (хоткеи DAW/видео сверены с официальными мануалами).
  - **Live** — сетка клавиш активного профиля + лента только что сработавших действий. Кликните любую ячейку или любой хит ленты, чтобы **выполнить действие мгновенно** (`POST /api/v1/action/fire`) — физическая клавиша не нужна.
  - **Typed Action Builder** — модальный редактор действий (заменяет браузерный `prompt()`): чипы key/media/macro/switch/launch/send-to-app, живое превью, выбор запущенных окон.
  - **First-run onboarding** — пустой дашборд предлагает три пути (Pad template / Create profile / Keyboard setup); «Start here» возвращает к выбору в любой момент.
  - Вкладка **Macros** в конструкторе действий — готовые примеры комбинаций/последовательностей (`{Ctrl+B}`, `{Ctrl+M}`, `{/}{Enter}`, …). Комбинации и последовательности из нескольких клавиш поддерживаются как действия: `{Ctrl+Shift+F}`, `{Up}{Enter}`.
  - Визуальный выбор приложения из запущенных окон
  - Страницы Diagnostics и Help/Setup
  - Живые обновления через SSE (без ручного обновления)
- **Иконка в трее** — левый клик → открыть дашборд; правый клик → контекстное меню с профилями и индикаторами режимов.
- **HTTP API** (только loopback, CSRF-защищённый):
  ```bash
  curl http://127.0.0.1:8765/api/profiles        # list profiles
  curl http://127.0.0.1:8765/api/v1/state        # unified state snapshot
  curl http://127.0.0.1:8765/api/v1/diagnostics  # health check
  curl http://127.0.0.1:8765/api/v1/hid          # all input devices + driver state (ready / needs-driver / ordinary)
  curl http://127.0.0.1:8765/api/v1/input/identify   # last keypress source (Raw Input; POST resets first)
  curl http://127.0.0.1:8765/api/v1/presets     # AI-agent pad catalog
  curl http://127.0.0.1:8765/api/v1/activity    # recent fired actions (Live screen)
  curl -X POST http://127.0.0.1:8765/api/v1/action/fire \
    -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" \
    -d '{"action":"{Media_Play_Pause}","usage":42,"profile":""}'  # fire an action like a pressed key
  ```
  Переключение профилей / активация устройства / применение пресета агента (требуется CSRF-токен с дашборда):
  ```bash
  TOKEN=$(curl -s http://127.0.0.1:8765/ | grep -o 'CSRF_TOKEN="[^"]*"' | grep -o '"[^"]*"' | tr -d '"')
  curl -X POST http://127.0.0.1:8765/api/profile/activate -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"name":"aimp"}'
  curl -X POST http://127.0.0.1:8765/api/v1/devices/activate -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"vidpid":"vid_xxxx&pid_yyyy"}'
  curl -X POST http://127.0.0.1:8765/api/v1/preset/apply -H "Content-Type: application/json" -H "X-KeySidekick-Token: $TOKEN" -d '{"agentId":"codex","name":"Codex pad"}'
  ```
  > **Для разработчиков:** `tests/http_integration_tests.sh` запускается против живого инстанса; он делает снапшот `src/config.ini` и автоматически восстанавливает его при выходе — ручной бэкап не нужен.
- **Клавиши** — всё, что вы назначили на `!switch:`/`!toggle:` в активном профиле.

---

## Сборка из исходников

Нужен g++ из [MinGW-w64](https://www.mingw-w64.org/) (например из [MSYS2](https://www.msys2.org/) или standalone-сборки). Пути Windows SDK не нужны — в MinGW есть свои `winusb.h`/`setupapi.h`.

```
cd src
build.bat
```

`src\build.bat` выполняет полную сборку: перегенерирует встроенный веб-дашборд из `web/` через `web/generate_dashboard.ps1`, компилирует иконки `windres`'ом (`resources.rc` → `resources.o`), затем линкует `sidekick.exe` (все 11 C++-исходников + `resources.o`, библиотеки WinUSB/SetupAPI/user32/ws2_32/…) и `probe_device.exe`. Сначала ищет `C:\MinGW64\bin\g++.exe`, затем откатывается на `g++` из PATH — подойдёт любой, лишь бы MinGW-w64 g++ был доступен.

`probe_device.exe` — диагностическая утилита: запустите после замены драйвера Zadig, чтобы выгрузить GUID интерфейса устройства, эндпоинты и HID report descriptor (подтверждает стандартный 8-байтовый keyboard report).

## Системные требования

- Windows 10 версии 1809 или новее (x64)
- Одна свободная USB-клавиатура, чей интерфейс `MI_00` вы готовы перевести на WinUSB-драйвер
- [Zadig](https://zadig.akeo.ie/) для одноразовой замены драйвера
- MinGW-w64 g++ (только если собираете из исходников)

## Документация

- [`presentation.html`](presentation.html) — отдельная лендинг-страница (EN/РУС/中文): проблема, решение, возможности, pad-шаблоны, API
- [`ZADIG_INSTRUCTIONS.md`](ZADIG_INSTRUCTIONS.md) — одноразовая замена драйвера по шагам (включая откат)
- [`docs/PROBLEM-AND-SOLUTION.md`](docs/PROBLEM-AND-SOLUTION.md) — техническая история: архитектура ввода Windows, почему `hDevice == NULL`, почему LL-hook лагает, почему Interception рискован, почему выигрывает WinUSB
- [`docs/FAQ.md`](docs/FAQ.md) — частые проблемы и их решения
- [`docs/HID-USAGE-TABLE.md`](docs/HID-USAGE-TABLE.md) — таблица HID Usage ID (Keyboard/Keypad) для ваших INI-маппингов

## Как это работает (в одном абзаце)

HID-интерфейс клавиатуры переключается с `hidusb.sys → kbdhid.sys → kbdclass.sys` на `WinUSB.sys` от Microsoft. Без привязанного keyboard-class драйвера её клавиши вообще не попадают в очередь ввода Windows — они невидимы для переднего плана. KeySidekick (`sidekick.exe`) открывает устройство через WinUSB API, асинхронно читает 8-байтовые HID-отчёты клавиатуры с interrupt-IN эндпоинта, диффает key-down/key-up состояние, ищет активный профиль и диспетчеризует каждую клавишу либо как одноразовое действие (`!switch`, `!app:`, ...), либо как жизненный цикл удерживаемой клавиши, отправляемый в целевое окно через `PostMessage` (режим `targeted`), либо реинжектит её в системный поток ввода через `SendInput` (режим `basic`). Для одиночных маппингов в targeted используются задержка/скорость повтора Windows …

## Благодарности

- **[Zadig](https://zadig.akeo.ie/)** от [Pete Batard / Akeo](https://github.com/pbatard/libwdi) — инструмент замены драйвера, делающий настройку WinUSB делом одного клика (LGPL).
- **Microsoft WinUSB** (`winusb.sys`) — WHQL-подписанный пользовательский USB-драйвер, на котором держится весь подход.
- **[USB HID Usage Tables](https://usb.org/sites/default/files/hut1_22.pdf)** (USB-IF) — канонический источник Usage ID клавиатуры.

## Участие в разработке

Приветствуются issues и pull requests. Для крупных изменений сначала откройте issue для обсуждения. Собирайте через `src/build.bat` и сверяйтесь с [чек-листом](#building-from-source) перед отправкой PR.

## Лицензия

Copyright © 2026. Лицензия **[GPL-3.0](LICENSE)**.

Проект использует, но не включает [Zadig](https://zadig.akeo.ie/) (LGPL) — пользователи скачивают его отдельно. Роутер линкуется с системными библиотеками Microsoft Windows (`winusb`, `setupapi`, `ws2_32` и др.), на которые лицензия не распространяется.
