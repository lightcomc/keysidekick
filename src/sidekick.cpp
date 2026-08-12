// sidekick.cpp  — KeySidekick
// Turn any spare keyboard into a dedicated background assistant.
// Multi-profile, multi-app, basic-mode, hotkey/tray/HTTP-controllable keyboard router.
//
// Изолированная через WinUSB клавиатура → маршрутизация нажатий по профилям.
//   • Режим basic:   SendInput re-inject (клавиатура печатает как обычная).
//   • Режим targeted: PostMessage WM_KEYDOWN/UP в окно target-приложения.
//   • Multi-app:     токен !app:<name>:<keys> override таргета для отдельной клавиши.
//   • Сочетания:     USAGE_xx+Ctrl+Shift=!switch:... (модификатор + клавиша).
//   • Управление:    клавишами (!switch:!toggle:!launch), tray-иконка + меню,
//                    локальный HTTP API (127.0.0.1:port).
//
// Предусловие: драйвер MI_00 заменён на WinUSB через Zadig (см. ZADIG_INSTRUCTIONS.md).
// Сборка: g++ -O2 -o sidekick.exe sidekick.cpp -lsetupapi -lwinusb -luser32 -lws2_32 -lshell32 -lgdi32 -static
// Лицензия: GPL-3.0 (см. LICENSE в корне репозитория).

#include <winsock2.h>
#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <newdev.h>
#include <shlobj.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <initguid.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include <map>
#include <atomic>
#include <algorithm>
#include <hidsdi.h>
#include <hidpi.h>
#include <devpkey.h>

#include "dashboard_embedded.generated.h"
#include "app_instance.h"
#include "input_ledger.h"
#include "targeted_input.h"
#include "runtime_storage.h"
#include "config_v3.h"
#include "domain_model.h"
#include "config_domain_bridge.h"
#include "windows_targets.h"
#include "http_security.h"
#include "action_parser.h"

// ---- WinUSB interface GUIDs ----
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB,
    0xDEE824E7, 0x7296, 0x4E41, 0x8C, 0x39, 0x0A, 0x9C, 0x1B, 0xDA, 0x4A, 0x2F);
DEFINE_GUID(GUID_DEVINTERFACE_LIBUSB0,
    0xF9F3FF14, 0xAEBE, 0x4Dde, 0xAE, 0x40, 0x72, 0x75, 0xF5, 0x34, 0x4A, 0x4A);
// GUID, назначенный Zadig нашему устройству (из реестра):
DEFINE_GUID(GUID_DEVINTERFACE_TARGET_WINUSB,
    0x901A2603, 0xA95E, 0x4CA8, 0x86, 0xBF, 0xFB, 0x05, 0x47, 0xC0, 0x6B, 0x64);

// =====================================================================
//  Конфигурация по умолчанию
// =====================================================================
static char  g_deviceVidPid[128] = "";  // set from config DeviceVIDPID (no author-hardware default)
static int   g_httpPort          = 8765;
static bool  g_httpEnabled       = true;
static bool  g_trayEnabled       = true;
static bool  g_logEnabled        = true;
static char  g_defaultProfile[64]= "basic";

static const char* CONFIG_FILE = "config.ini";
static const char* LOG_FILE    = "sidekick.log";

// Product version — single source of truth (mirrored in resources.rc VERSIONINFO).
static const char* APP_VERSION = "0.9.5";

// ---- Data path resolution ----
// config.ini and sidekick.log resolve with fallback order:
//   (1) the directory containing sidekick.exe (if the file exists there),
//   (2) the current working directory (if the file exists there),
//   (3) the exe directory (defaults) — so a double-clicked exe behaves the
//       same regardless of the launching CWD, and writes land next to the exe.
static char g_exeDir[MAX_PATH] = {0};
static char g_configPathBuf[MAX_PATH] = {0};
static char g_logPathBuf[MAX_PATH] = {0};

static bool DataFileExists(const char* path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void ResolveDataPaths() {
    wchar_t exeW[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(NULL, exeW, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        wchar_t* slash = wcsrchr(exeW, L'\\');
        if (slash) *slash = L'\0';
        WideCharToMultiByte(CP_ACP, 0, exeW, -1, g_exeDir, MAX_PATH, NULL, NULL);
    }
    if (g_exeDir[0]) {
        _snprintf(g_configPathBuf, sizeof(g_configPathBuf), "%s\\config.ini", g_exeDir);
        _snprintf(g_logPathBuf, sizeof(g_logPathBuf), "%s\\sidekick.log", g_exeDir);
        if (DataFileExists(g_configPathBuf)) CONFIG_FILE = g_configPathBuf;
        else if (DataFileExists("config.ini")) CONFIG_FILE = "config.ini";
        else CONFIG_FILE = g_configPathBuf;
        if (DataFileExists(g_logPathBuf)) LOG_FILE = g_logPathBuf;
        else if (DataFileExists("sidekick.log")) LOG_FILE = "sidekick.log";
        else LOG_FILE = g_logPathBuf;
    }
}

// =====================================================================
//  Логирование
// =====================================================================
static CRITICAL_SECTION g_csLog;
static bool g_csLogReady = false;
void Log(const char* fmt, ...) {
    if (!g_logEnabled) return;
    va_list ap; va_start(ap, fmt);
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    // H3: несколько потоков (HTTP-воркеры/SSE/main) пишут в один лог —
    // сериализуем запись, чтобы строки не перемешивались.
    if (g_csLogReady) EnterCriticalSection(&g_csLog);
    FILE* f = fopen(LOG_FILE, "a");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "%04d%02d%02d%02d%02d%02d - %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
    if (g_csLogReady) LeaveCriticalSection(&g_csLog);
}

static void Trim(char* s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1]==' '||s[len-1]=='\r'||s[len-1]=='\n'||s[len-1]=='\t'))
        s[--len] = 0;
    char* p = s; while (*p==' '||*p=='\t') p++;
    if (p != s) memmove(s, p, strlen(p)+1);
    // убрать комментарий в конце строки (если строка не начинается с # или ;)
    // оставляем точку-с-запятой внутри значений в покое — секция комментариев только для целых строк
}

// Lowercase a std::string (ASCII-only, for VID/PID matching)
static std::string ToLower(const char* s) {
    std::string r(s);
    for (std::size_t i = 0; i < r.size(); ++i) r[i] = (char)tolower((unsigned char)r[i]);
    return r;
}

// =====================================================================
//  Модель данных: Profile
// =====================================================================
enum ProfileMode { MODE_BASIC=0, MODE_TARGETED=1 };

struct KeyMapping {
    int usageId;
    unsigned char modMask = 0;   // требуемые модификаторы (битовая маска как в report[0]); 0 = без модификаторов
    std::string action;          // AHK-строка ИЛИ токен !switch:.. / !app:.. / !launch:..
};

struct Profile {
    std::string name;
    ProfileMode mode = MODE_BASIC;
    std::string targetClass;
    std::string targetExe;
    std::string targetPath;
    bool autoStart = false;
    std::vector<KeyMapping> keys;
    bool isBuiltinBasic = false;
    // Fn-слой: маппинги с этой модификатор-маской — «вторая страница» клавиатуры
    std::string layerModName;             // "Alt"/"Ctrl"/"Shift"/"Win"/""
    unsigned char layerModMask = 0;       // 8-bit bits (считается из layerModName)
};

// Имя Fn-модификатора → 8-бит маска (определение ниже, рядом с ModsMatch).
static unsigned char ModifierNameToMask(const std::string& name);

static std::map<std::string, Profile> g_profiles;
static std::string g_activeProfile = "basic";
static CRITICAL_SECTION g_csProfile;   // защита g_activeProfile + g_profiles при HTTP-переключениях

// ---- Phase 2: Canonical domain model (source of truth for multi-app) ----
// g_domain защищается тем же g_csProfile. g_profiles — это projection для hot path.
// Проекция перестраивается из g_domain через ProjectDomainToRuntime().
static keysidekick::DomainModel g_domain;
// Сохраняем general settings между LoadConfig и WriteConfig (config_v3 GeneralSettings)
static keysidekick::config::GeneralSettings g_configGeneral;

// ---- Phase 4: Security + live state ----
// CSRF token: генерируется при старте, embedded в dashboard HTML, проверяется на mutating POST.
static std::string g_csrfToken;
// State revision: инкрементируется при каждом изменении state (profile switch, config save, device state).
// SSE clients сравнивают lastRevision чтобы знать когда перечитать state.
static unsigned long long g_stateRevision = 0;
static CRITICAL_SECTION g_csRevision;   // защита g_stateRevision + g_sseClients

// Helper: bump revision (вызывать под g_csRevision или после mutation)
static void BumpRevision() {
    EnterCriticalSection(&g_csRevision);
    g_stateRevision++;
    unsigned long long rev = g_stateRevision;
    LeaveCriticalSection(&g_csRevision);
    // Notify SSE clients (see SSE handler)
    extern void NotifySseClients(unsigned long long revision);
    NotifySseClients(rev);
}

// Копия активного профиля по значению (безопасно: берём под локом, отдаём копию).
// Возврат указателя на node map после release lock — use-after-free при reload/edit.
// profFound=true если активный профиль найден и скопирован в out.
static bool ActiveProfileCopy(Profile& out) {
    EnterCriticalSection(&g_csProfile);
    auto it = g_profiles.find(g_activeProfile);
    bool found = (it != g_profiles.end());
    if (found) out = it->second;   // копирование под локом
    LeaveCriticalSection(&g_csProfile);
    return found;
}

// =====================================================================
//  Встроенный basic-профиль
// =====================================================================
static void EnsureBuiltinBasic() {
    if (g_profiles.find("basic") == g_profiles.end()) {
        Profile p;
        p.name = "basic";
        p.mode = MODE_BASIC;
        p.isBuiltinBasic = true;
        g_profiles["basic"] = p;
    }
}

// =====================================================================
//  Phase 2: Projection DomainModel → runtime g_profiles
//  Перестраивает g_profiles из g_domain для совместимости с hot path.
//  ВЫЗЫВАТЬ ПОД g_csProfile.
// =====================================================================
static void ProjectDomainToRuntime() {
    g_profiles.clear();

    for (std::size_t i = 0; i < g_domain.profiles.size(); ++i) {
        const keysidekick::Profile& dp = g_domain.profiles[i];

        Profile rp;
        // Domain Normal profile → runtime "basic" (canonical built-in name)
        if (dp.id() == keysidekick::Profile::normalId()) {
            rp.name = "basic";
        } else {
            rp.name = dp.name;
        }
        rp.mode = (dp.mode == keysidekick::ProfileMode::Targeted) ? MODE_TARGETED : MODE_BASIC;
        rp.isBuiltinBasic = dp.isBuiltIn();
        rp.layerModName = dp.layerModifier;
        rp.layerModMask = ModifierNameToMask(dp.layerModifier);

        // Target fields: берём default (или первый linked) ApplicationTarget
        std::string appId = dp.defaultApplicationId;
        if (appId.empty() && !dp.linkedApplicationIds.empty())
            appId = dp.linkedApplicationIds.front();
        if (!appId.empty()) {
            const keysidekick::ApplicationTarget* app = g_domain.findApplication(appId);
            if (app) {
                rp.targetClass = app->windowClass;
                rp.targetExe   = app->processName;
                rp.targetPath  = app->exePath;
            }
        }
        // autoStart: domain не хранит; config_v3 тоже теряет это поле в domain.
        // Оставляем false (Phase 3 добавит launchPolicy → autoStart mapping).

        // Mappings: pass-through action strings
        for (std::size_t m = 0; m < dp.mappings.size(); ++m) {
            const keysidekick::Mapping& dm = dp.mappings[m];
            KeyMapping km;
            km.usageId = (int)(dm.trigger.usageId & 0xFF);
            km.modMask = keysidekick::bridge::ExpandDomainModifierToConfig(dm.trigger.modifierMask);
            km.action  = dm.action.profileId;  // pass-through carrier
            rp.keys.push_back(km);
        }

        g_profiles[rp.name] = rp;
    }

    // Active profile: маппим domain activeProfileId → runtime name.
    // Domain Normal (id="profile.normal") → runtime "basic".
    // Остальные → по имени профиля (domain name == runtime key).
    if (g_domain.activeProfileId == keysidekick::Profile::normalId()) {
        g_activeProfile = "basic";
    } else {
        const keysidekick::Profile* ap = g_domain.findProfile(g_domain.activeProfileId);
        g_activeProfile = ap ? ap->name : "basic";
    }
    // Safety: если активный профиль не найден в g_profiles, fallback на basic
    if (g_profiles.find(g_activeProfile) == g_profiles.end()) {
        g_activeProfile = "basic";
        EnsureBuiltinBasic();
    }
}

// --- Capture state (для «ловить следующую клавишу» в дашборде) ---
// Определены здесь (раньше HTTP-секции), т.к. ProcessReport и MsgWndProc
// используют их до определения HTTP API.
static std::atomic<bool> g_captureArmed{false};
static std::atomic<int>  g_capturedUsage{0};

// --- Live activity: какие клавиши-actions недавно сработали (для Live-экрана) ---
struct ActivityEvent {
    std::uint64_t seq;
    std::uint32_t tick;      // GetTickCount()
    int usage;
    std::string action;
    std::string mode;        // "basic" / "targeted"
};
static std::vector<ActivityEvent> g_activityEvents;
static std::uint64_t g_activitySeq = 0;
static CRITICAL_SECTION g_csActivity;
// Live-экран: пуш по SSE (определена позже в SSE-секции).
static void NotifyActivitySse(const std::string& json);

static void RecordActivity(int usage, const std::string& action, const std::string& mode) {
    EnterCriticalSection(&g_csActivity);
    ActivityEvent e;
    e.seq = ++g_activitySeq;
    e.tick = GetTickCount();
    e.usage = usage;
    e.action = action;
    e.mode = mode;
    g_activityEvents.push_back(e);
    if (g_activityEvents.size() > 24) g_activityEvents.erase(g_activityEvents.begin());
    LeaveCriticalSection(&g_csActivity);
    // Live-экран: мгновенный пуш по SSE (NotifyActivitySse определена позже).
    auto sseEscape = [](const std::string& s)->std::string {
        std::string o;
        for (char c : s) {
            if (c == '"') o += "\\\"";
            else if (c == '\\') o += "\\\\";
            else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c); o += b; }
            else o += c;
        }
        return o;
    };
    std::string js = "{\"seq\":" + std::to_string(e.seq)
        + ",\"usage\":" + std::to_string(e.usage)
        + ",\"action\":\"" + sseEscape(e.action) + "\""
        + ",\"mode\":\"" + sseEscape(e.mode) + "\"}";
    NotifyActivitySse(js);
}

// ---- AI-agent пасеты (перенос Codex Micro / Stream Deck на spare-клавиатуру) ----
// Физ.клавиша → макро. Комбо (#4) разбираются в ParseKeyEvents; каталог отдаётся в UI.
static const struct AgentKeyDef { int usage; const char* action; const char* label; } kAgentKeys[] = {
    {0x3A, "{Enter}",            "Accept / send (Enter)"},
    {0x3B, "{Escape}",          "Cancel (Esc)"},
    {0x3C, "{Ctrl+Shift+F}",   "Branch session"},
    {0x3D, "{Ctrl+B}",         "Toggle sidebar"},
    {0x3E, "{Ctrl+M}",         "Voice dictation / push-to-talk"},
    {0x3F, "{Up}",             "Prompt history ↑"},
    {0x40, "{Down}",           "Prompt history ↓"},
    {0x41, "{/}{Enter}",       "Command mode ( / + Enter )"},
    {0x42, "{F5}",             "Retry / Run"},
    {0x43, "{Ctrl+Enter}",     "Send (Ctrl+Enter)"},
    {0x44, "{Media_Play_Pause}", "Play / pause media"},
    {0x45, "{Volume_Mute}",     "Mute audio"}
};

// Сценарий: медиа-панель (работает глобально через media-клавиши в basic-режиме)
static const AgentKeyDef kMediaKeys[] = {
    {0x3A, "{Media_Play_Pause}", "Play / pause"},
    {0x3B, "{Media_Next_Track}", "Next track"},
    {0x3C, "{Media_Prev_Track}", "Prev track"},
    {0x3D, "{Media_Stop}",       "Stop"},
    {0x3E, "{Volume_Up}",        "Volume +"},
    {0x3F, "{Volume_Down}",      "Volume -"},
    {0x40, "{Volume_Mute}",      "Mute"},
    {0x41, "{Space}",            "Play sup / Space"}
};
// Сценарий: OBS / стрим (укажите те же хоткеи в OBS → Настройки → Горячие клавиши)
static const AgentKeyDef kObsKeys[] = {
    {0x3A, "{Ctrl+Alt+1}", "Start / stop recording"},
    {0x3B, "{Ctrl+Alt+2}", "Start / stop streaming"},
    {0x3C, "{Ctrl+Alt+3}", "Pause recording"},
    {0x3D, "{Ctrl+Alt+4}", "Mute / unmute mic"},
    {0x3E, "{Ctrl+Alt+5}", "Mute / unmute desktop audio"},
    {0x3F, "{Alt+F4}",     "Close OBS"}
};
// Сценарий: созвоны (Meet/Zoom) — стандартные хоткеи, перенастройте в приложении при надобности
static const AgentKeyDef kMeetKeys[] = {
    {0x3A, "{Ctrl+D}",      "Mic off / on"},
    {0x3B, "{Ctrl+E}",      "Camera off / on"},
    {0x3C, "{Ctrl+Alt+K}",  "Raise hand"},
    {0x3D, "{Ctrl+Alt+C}",  "Open chat"},
    {0x3E, "{Ctrl+Alt+H}",  "Participants"},
    {0x3F, "{Ctrl+Alt+W}",  "Leave call"},
    {0x40, "{Ctrl+D}",      "Mic (alt)"}
};
// Сценарий: презентация PowerPoint
static const AgentKeyDef kPptKeys[] = {
    {0x3A, "{F5}",        "Start slideshow"},
    {0x3B, "{Shift+F5}",  "Start from current"},
    {0x3C, "{Right}",     "Next slide"},
    {0x3D, "{Left}",      "Previous slide"},
    {0x3E, "{Space}",     "Advance"},
    {0x3F, "{B}",         "Black screen"},
    {0x40, "{Esc}",       "End show"},
    {0x41, "{Home}",      "First slide"}
};
// Сценарий: REAPER — стандартные хоткеи (User Guide v7.78). {M} маркер — сверьте в Shift+F1.
static const AgentKeyDef kReaperKeys[] = {
    {0x3A, "{Space}",  "Play / stop"},
    {0x3B, "{Enter}",  "Pause (in place)"},
    {0x3C, "{Ctrl+R}", "Record (start/stop)"},
    {0x3D, "{S}",      "Split item"},
    {0x3E, "{Alt+S}",  "Snap on/off"},
    {0x3F, "{W}",      "Rewind / start of track"},
    {0x40, "{R}",      "Toggle repeat (loop)"},
    {0x41, "{F2}",     "Item properties"},
    {0x42, "{Ctrl+J}", "Jump to time / marker"},
    {0x43, "{M}",      "Marker (verify via Shift+F1)"},
    {0x44, "{Alt+R}",  "Routing matrix"},
    {0x45, "{Ctrl+T}", "New track"}
};
// Сценарий: DaVinci Resolve 19 — дефолтные хоткеи. Media browser = Shift+2, не Shift+5.
static const AgentKeyDef kResolveKeys[] = {
    {0x3A, "{Space}",    "Play / pause"},
    {0x3B, "{J}",        "Rewind (reverse)"},
    {0x3C, "{L}",        "Forward"},
    {0x3D, "{I}",        "Mark in"},
    {0x3E, "{O}",        "Mark out"},
    {0x3F, "{M}",        "Add marker"},
    {0x40, "{B}",        "Blade tool"},
    {0x41, "{Ctrl+B}",   "Blade edit mode"},
    {0x42, "{T}",        "Trim edit mode"},
    {0x43, "{Shift+2}",  "Media browser"},
    {0x44, "{Shift+3}",  "Cut page"},
    {0x45, "{Shift+F}",  "Fullscreen viewer"}
};
// Сценарий: Ableton Live 12 — офиц. хоткеи (Live Keyboard Shortcuts).
static const AgentKeyDef kAbletonKeys[] = {
    {0x3A, "{F9}",            "Record (global)"},
    {0x3B, "{Space}",         "Play / stop"},
    {0x3C, "{O}",             "Metronome on/off"},
    {0x3D, "{Ctrl+L}",        "Loop / brace on/off"},
    {0x3E, "{C}",             "Arm selected tracks"},
    {0x3F, "{Tab}",           "Session ↔ Arrangement"},
    {0x40, "{F11}",           "Full screen"},
    {0x41, "{F12}",           "Device / Clip view"},
    {0x42, "{Shift+Tab}",     "Device ↔ Clip view"},
    {0x43, "{Ctrl+Shift+F9}", "Record in Session View"},
    {0x44, "{Ctrl+M}",        "MIDI map mode"},
    {0x45, "{Ctrl+K}",        "Key map mode"}
};
// Сценарий: Adobe Premiere Pro — офиц. дефолты (Default Keyboard Shortcuts).
static const AgentKeyDef kPremiereKeys[] = {
    {0x3A, "{Space}",       "Play / stop"},
    {0x3B, "{J}",           "Scrub backward"},
    {0x3C, "{K}",           "Scrub stop"},
    {0x3D, "{L}",           "Scrub forward"},
    {0x3E, "{I}",           "Mark in"},
    {0x3F, "{O}",           "Mark out"},
    {0x40, "{Ctrl+K}",      "Razor: add edit"},
    {0x41, "{S}",           "Snap on/off"},
    {0x42, "{Shift+Delete}","Ripple delete"},
    {0x43, "{F}",           "Match frame"},
    {0x44, "{M}",           "Add marker"},
    {0x45, "{Ctrl+S}",      "Save project"}
};
// Сценарий: Lightroom Classic — офиц. хоткеи (модули, флаги, звёзды).
static const AgentKeyDef kLightroomKeys[] = {
    {0x3A, "{G}",            "Grid (Library)"},
    {0x3B, "{E}",            "Loupe (Library)"},
    {0x3C, "{D}",            "Develop"},
    {0x3D, "{R}",            "Crop (Develop)"},
    {0x3E, "{P}",            "Flag: pick"},
    {0x3F, "{X}",            "Flag: reject"},
    {0x40, "{U}",            "Flag: unflag"},
    {0x41, "{1}",            "Stars: 1"},
    {0x42, "{5}",            "Stars: 5"},
    {0x43, "{Ctrl+Shift+I}", "Import from disk"},
    {0x44, "{Left}",         "Prev photo"},
    {0x45, "{Right}",        "Next photo"}
};
static const struct AgentPreset {
    const char* agentId;
    const char* name;
    const char* desc;
    const char* kind;          // "agent" / "use-case"
    const char* profileMode;   // "targeted" / "basic"
    const char* defaultWindowClass;
    const char* defaultProcessName;
    const AgentKeyDef* keys;
    int keyCount;
} kAgentPresets[] = {
    { "codex",     "OpenAI Codex",   "Agent pad for Codex / ChatGPT",       "agent",  "targeted", "", "codex",  kAgentKeys, (int)(sizeof(kAgentKeys)/sizeof(kAgentKeys[0])) },
    { "chatgpt",   "ChatGPT",        "Agent pad for ChatGPT",               "agent",  "targeted", "", "chatgpt",kAgentKeys, (int)(sizeof(kAgentKeys)/sizeof(kAgentKeys[0])) },
    { "claude",    "Claude Code",    "Agent pad for Claude Code (terminal)","agent",  "targeted", "", "claude", kAgentKeys, (int)(sizeof(kAgentKeys)/sizeof(kAgentKeys[0])) },
    { "cursor",    "Cursor",         "Agent pad for Cursor",                "agent",  "targeted", "", "cursor", kAgentKeys, (int)(sizeof(kAgentKeys)/sizeof(kAgentKeys[0])) },
    { "devin",     "Devin",           "Agent pad for Devin",                "agent",  "targeted", "", "devin", kAgentKeys, (int)(sizeof(kAgentKeys)/sizeof(kAgentKeys[0])) },
    { "copilot",   "GitHub Copilot", "Agent pad for Copilot / VS Code",     "agent",  "targeted", "", "copilot", kAgentKeys, (int)(sizeof(kAgentKeys)/sizeof(kAgentKeys[0])) },
    { "media",     "Media player",   "Play/pause, track, volume — global", "use-case","basic", "", "",  kMediaKeys, (int)(sizeof(kMediaKeys)/sizeof(kMediaKeys[0])) },
    { "obs",       "OBS / Streaming", "Stream/record/mute — set the hotkeys in OBS", "use-case","targeted", "", "", kObsKeys,   (int)(sizeof(kObsKeys)/sizeof(kObsKeys[0])) },
    { "meet",      "Meets / Zoom",     "Mic/camera/hand/chat on calls", "use-case","targeted", "", "", kMeetKeys,  (int)(sizeof(kMeetKeys)/sizeof(kMeetKeys[0])) },
    { "office",    "PowerPoint presentation", "Slides forward/back, start, black screen", "use-case","targeted", "", "POWERPNT.EXE", kPptKeys, (int)(sizeof(kPptKeys)/sizeof(kPptKeys[0])) },
    { "reaper",    "REAPER DAW",        "Play/split/snap/loop — standard REAPER hotkeys", "use-case","targeted", "", "reaper.exe", kReaperKeys, (int)(sizeof(kReaperKeys)/sizeof(kReaperKeys[0])) },
    { "davinci",   "DaVinci Resolve",   "Transport, markers, blade — Resolve 19 hotkeys",      "use-case","targeted", "", "resolve.exe", kResolveKeys, (int)(sizeof(kResolveKeys)/sizeof(kResolveKeys[0])) },
    { "ableton",   "Ableton Live",      "Record, clips, metronome — official hotkeys",    "use-case","targeted", "", "live.exe", kAbletonKeys, (int)(sizeof(kAbletonKeys)/sizeof(kAbletonKeys[0])) },
    { "premiere",  "Adobe Premiere",    "Scrub, razor, markers — Premiere Pro defaults",     "use-case","targeted", "", "adobe premiere pro.exe", kPremiereKeys, (int)(sizeof(kPremiereKeys)/sizeof(kPremiereKeys[0])) },
    { "lightroom", "Lightroom Classic", "Modules, flags, stars — official LrC hotkeys",    "use-case","targeted", "", "lightroom.exe", kLightroomKeys, (int)(sizeof(kLightroomKeys)/sizeof(kLightroomKeys[0])) }
};
static const AgentPreset* FindAgentPreset(const char* id) {
    if (!id) return NULL;
    for (std::size_t i = 0; i < sizeof(kAgentPresets)/sizeof(kAgentPresets[0]); ++i)
        if (_stricmp(kAgentPresets[i].agentId, id) == 0) return &kAgentPresets[i];
    return NULL;
}

// --- Marshaling write-операций из HTTP-потока в main thread ---
#define WM_DASH_OP (WM_APP+3)
enum DashOpType {
    DASH_ADD_KEY=1, DASH_REMOVE_KEY=2, DASH_SET_PROFILE=3, DASH_RELOAD=4,
    // Phase 2 CRUD operations (use ProfileService on g_domain)
    DASH_CREATE_PROFILE=10, DASH_DELETE_PROFILE=11, DASH_RENAME_PROFILE=12,
    DASH_DUPLICATE_PROFILE=13, DASH_LINK_APP=14, DASH_UNLINK_APP=15,
    DASH_SET_DEFAULT_APP=16,
    // Mapping management (in-place edit, reorder, duplicate)
    DASH_UPDATE_KEY=17, DASH_MOVE_KEY=18, DASH_DUPLICATE_KEY=19,
    // HID-first onboarding: make a device active (append VID/PID to config + reconnect)
    DASH_ACTIVATE_DEVICE=20,
    // AI-agent preset: create a targeted profile with default mappings (Codex etc.)
    DASH_APPLY_PRESET=21,
    // Live click-to-fire: выполнить action «как будто нажата клавиша» (без persist)
    DASH_FIRE_ACTION=22
};
struct DashOp {
    DashOpType op;
    char profile[64];
    int usage; unsigned char mod;
    char action[256];
    char targetClass[256], targetExe[256], targetPath[512];
    char layerMod[16];          // DASH_SET_PROFILE: имя Fn-модификатора ("Alt"/"Ctrl"/...)
    int mode;     // 0=basic 1=targeted
    int autoStart;
    // Phase 2 CRUD fields
    char strArg1[128];      // newName (rename), newProfileId (duplicate), appId (link/unlink/setDefault)
    char strArg2[128];      // newName (duplicate)
    HANDLE doneEvent;
    bool success;
    char error[256];
    // UAF safety: HTTP caller sets true on timeout, main thread deletes if abandoned
    std::atomic<bool> timedOut;
    bool needsReconnect;   // DASH_ACTIVATE_DEVICE: переподключить устройство после WriteConfig
    bool noPersist;        // DASH_FIRE_ACTION: runtime-выполнение, config не писать
};

// M2: сколько HTTP-вызывающий ждёт завершения DashOp на main thread.
// Раньше было 5 с, а активация (WriteConfig + переподключение устройства)
// могла занимать до ~10.5 с → ложные «timeout» при успешной записи.
// Реконнект теперь асинхронный (C2), но WriteConfig/enumeration всё равно
// могут быть медленными — даём 15 с.
static const DWORD kDashOpTimeoutMs = 15000;

// =====================================================================
//  Таблица HID Usage ID (page 0x07) → Set-1 scan code для SendInput.
//  Значение 0 = нет отображения / не стандартное.
//  + флаг расширенной клавиши (KEYEVENTF_EXTENDEDKEY) для стрелок/nav/правых модификаторов.
// =====================================================================
struct ScanInfo { unsigned short scan; bool extended; };   // scan: 0x00-0xFF или 0xE0XX для extended

// Получить scan + extended-флаг по HID Usage ID.
static ScanInfo UsageToSet1(int usage) {
    // Карта построена по "USB HID to PS/2 (Set 1) Translation Table" (Microsoft)
    // и Chromium keycode_converter_data. Set1 make-коды. Extended-клавиши = 0xE0XX.
    static const struct { int u; unsigned short s; bool e; } T[] = {
        // буквы a-z (usage 0x04-0x1D) → scan 0x1E-0x35 (не полностью монотонно)
        {0x04,0x1E,false},{0x05,0x30,false},{0x06,0x2E,false},{0x07,0x20,false},
        {0x08,0x12,false},{0x09,0x21,false},{0x0A,0x22,false},{0x0B,0x23,false},
        {0x0C,0x17,false},{0x0D,0x24,false},{0x0E,0x25,false},{0x0F,0x26,false},
        {0x10,0x32,false},{0x11,0x31,false},{0x12,0x18,false},{0x13,0x19,false},
        {0x14,0x10,false},{0x15,0x13,false},{0x16,0x1F,false},{0x17,0x14,false},
        {0x18,0x16,false},{0x19,0x2F,false},{0x1A,0x11,false},{0x1B,0x2D,false},
        {0x1C,0x15,false},{0x1D,0x2C,false},
        // цифры 1-9,0 (usage 0x1E-0x27) → scan 0x02-0x0B
        {0x1E,0x02,false},{0x1F,0x03,false},{0x20,0x04,false},{0x21,0x05,false},
        {0x22,0x06,false},{0x23,0x07,false},{0x24,0x08,false},{0x25,0x09,false},
        {0x26,0x0A,false},{0x27,0x0B,false},
        // спецсимволы
        {0x28,0x1C,false}, // Enter
        {0x29,0x01,false}, // Esc
        {0x2A,0x0E,false}, // Backspace
        {0x2B,0x0F,false}, // Tab
        {0x2C,0x39,false}, // Space
        {0x2D,0x0C,false}, // -
        {0x2E,0x0D,false}, // =
        {0x2F,0x1A,false}, // [
        {0x30,0x1B,false}, // ]
        {0x31,0x2B,false}, // \
        {0x33,0x27,false}, // ;
        {0x34,0x28,false}, // '
        {0x35,0x29,false}, // `
        {0x36,0x33,false}, // ,
        {0x37,0x34,false}, // .
        {0x38,0x35,false}, // /
        {0x39,0x3A,false}, // CapsLock
        // F1-F12
        {0x3A,0x3B,false},{0x3B,0x3C,false},{0x3C,0x3D,false},{0x3D,0x3E,false},
        {0x3E,0x3F,false},{0x3F,0x40,false},{0x40,0x41,false},{0x41,0x42,false},
        {0x42,0x43,false},{0x43,0x44,false},{0x44,0x57,false},{0x45,0x58,false},
        {0x46,0xE037,true},  // PrintScreen (extended)
        {0x47,0x46,false},   // ScrollLock
        {0x48,0xE052,true},  // Pause (extended; обычно)
        // nav cluster (extended)
        {0x49,0xE052,true}, // Insert
        {0x4A,0xE047,true}, // Home
        {0x4B,0xE049,true}, // PageUp
        {0x4C,0xE053,true}, // Delete
        {0x4D,0xE04F,true}, // End
        {0x4E,0xE051,true}, // PageDown
        // arrows (extended)
        {0x4F,0xE04F,true}, // Right
        {0x50,0xE04B,true}, // Left
        {0x51,0xE050,true}, // Down
        {0x52,0xE048,true}, // Up
        // NumLock / numpad
        {0x53,0x45,false},  // NumLock (не extended)
        {0x54,0xE035,true}, // Numpad /
        {0x55,0x37,false},  // Numpad *
        {0x56,0x4A,false},  // Numpad -
        {0x57,0x4E,false},  // Numpad +
        {0x58,0xE01C,true}, // Numpad Enter (extended)
        {0x59,0x4F,false},  // Numpad 1
        {0x5A,0x50,false},  // Numpad 2
        {0x5B,0x51,false},  // Numpad 3
        {0x5C,0x4B,false},  // Numpad 4
        {0x5D,0x4C,false},  // Numpad 5
        {0x5E,0x4D,false},  // Numpad 6
        {0x5F,0x47,false},  // Numpad 7
        {0x60,0x48,false},  // Numpad 8
        {0x61,0x49,false},  // Numpad 9
        {0x62,0x52,false},  // Numpad 0
        {0x63,0x53,false},  // Numpad .
        // модификаторы (трактуются отдельно, но на всякий случай)
        {0xE0,0x1D,false}, // LCtrl
        {0xE1,0x2A,false}, // LShift
        {0xE2,0x38,false}, // LAlt
        {0xE3,0xE05B,true},// LGUI (extended)
        {0xE4,0xE01D,true},// RCtrl (extended)
        {0xE5,0x36,false}, // RShift
        {0xE6,0xE038,true},// RAlt (extended)
        {0xE7,0xE05C,true},// RGUI (extended)
    };
    for (auto& t : T) if (t.u == usage) return {t.s, t.e};
    return {0, false};
}

// Modifiers в byte[0] → scan (extended-флаг встроен)
struct ModBit { int mask; int usage; };
static const ModBit MOD_BITS[8] = {
    {0x01,0xE0},{0x02,0xE1},{0x04,0xE2},{0x08,0xE3},
    {0x10,0xE4},{0x20,0xE5},{0x40,0xE6},{0x80,0xE7},
};

// Разобрать суффиксы модификаторов в INI-ключе вида "USAGE_1E+Ctrl+Shift".
// Возвращает маску (как в report[0]) или 0 если модификаторов нет.
// Имена: Ctrl(=LCtrl|RCtrl) Shift(=LShift|RShift) Alt(=LAlt|RAlt) Win(=LWin|RWin)
//        либо конкретные LCtrl RCtrl LShift RShift LAlt RAlt LWin RWin.
static unsigned char ParseModSuffix(const std::string& s) {
    unsigned char mask = 0;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t plus = s.find('+', pos);
        std::string tok = (plus == std::string::npos) ? s.substr(pos) : s.substr(pos, plus - pos);
        if (_stricmp(tok.c_str(), "Ctrl")==0)       mask |= 0x01|0x10;
        else if (_stricmp(tok.c_str(), "Shift")==0) mask |= 0x02|0x20;
        else if (_stricmp(tok.c_str(), "Alt")==0)   mask |= 0x04|0x40;
        else if (_stricmp(tok.c_str(), "Win")==0)   mask |= 0x08|0x80;
        else if (_stricmp(tok.c_str(), "LCtrl")==0) mask |= 0x01;
        else if (_stricmp(tok.c_str(), "LShift")==0)mask |= 0x02;
        else if (_stricmp(tok.c_str(), "LAlt")==0)  mask |= 0x04;
        else if (_stricmp(tok.c_str(), "LWin")==0)  mask |= 0x08;
        else if (_stricmp(tok.c_str(), "RCtrl")==0) mask |= 0x10;
        else if (_stricmp(tok.c_str(), "RShift")==0)mask |= 0x20;
        else if (_stricmp(tok.c_str(), "RAlt")==0)  mask |= 0x40;
        else if (_stricmp(tok.c_str(), "RWin")==0)  mask |= 0x80;
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    return mask;
}

// =====================================================================
//  Таблица имён клавиш → VK (для targeted-режима, отправка WM_KEYDOWN)
// =====================================================================
static UINT VkKeyName(const char* name) {
    struct { const char* n; UINT vk; } tbl[] = {
        {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F4",VK_F4},
        {"F5",VK_F5},{"F6",VK_F6},{"F7",VK_F7},{"F8",VK_F8},
        {"F9",VK_F9},{"F10",VK_F10},{"F11",VK_F11},{"F12",VK_F12},
        {"Space",VK_SPACE},{"Enter",VK_RETURN},{"Esc",VK_ESCAPE},
        {"Tab",VK_TAB},{"Backspace",VK_BACK},
        {"Left",VK_LEFT},{"Right",VK_RIGHT},{"Up",VK_UP},{"Down",VK_DOWN},
        {"Home",VK_HOME},{"End",VK_END},{"PageUp",VK_PRIOR},{"PageDown",VK_NEXT},
        {"Insert",VK_INSERT},{"Delete",VK_DELETE},
        {"Media_Play_Pause",VK_MEDIA_PLAY_PAUSE},
        {"Media_Next_Track",VK_MEDIA_NEXT_TRACK},
        {"Media_Prev_Track",VK_MEDIA_PREV_TRACK},
        {"Media_Stop",VK_MEDIA_STOP},
        {"Volume_Mute",VK_VOLUME_MUTE},
        {"Volume_Up",VK_VOLUME_UP},
        {"Volume_Down",VK_VOLUME_DOWN},
        {NULL,0}
    };
    for (int i=0; tbl[i].n; i++) if (_stricmp(name, tbl[i].n)==0) return tbl[i].vk;
    return 0;
}

struct WindowKey {
    UINT virtualKey;
    UINT scanCode;
    bool extended;
};

static bool IsExtendedVirtualKey(UINT virtualKey) {
    switch (virtualKey) {
        case VK_RMENU:
        case VK_RCONTROL:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_CANCEL:
        case VK_DIVIDE:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
            return true;
        default:
            return false;
    }
}

struct KeyEvent {
    UINT vk = 0;
    UINT scan = 0;
    bool extended = false;
    bool down = true;
};

static KeyEvent MakeKeyEvent(UINT vk, bool down) {
    KeyEvent e;
    e.vk = vk;
    e.scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) & 0xFF;
    e.extended = IsExtendedVirtualKey(vk);
    e.down = down;
    return e;
}

static UINT SingleCharToVk(char c) {
    short sv = VkKeyScanA(c);
    return (sv != -1) ? (UINT)(sv & 0xFF) : 0;
}

// Модификатор как отдельный VK (Ctrl/Shift/Alt/Win + один символ как обычная клавиша).
static UINT ModNameToVk(const std::string& n) {
    if (n.size()==1) return SingleCharToVk(n[0]);
    if (_stricmp(n.c_str(),"Ctrl")==0 || _stricmp(n.c_str(),"LCtrl")==0) return VK_LCONTROL;
    if (_stricmp(n.c_str(),"RCtrl")==0) return VK_RCONTROL;
    if (_stricmp(n.c_str(),"Shift")==0 || _stricmp(n.c_str(),"LShift")==0) return VK_LSHIFT;
    if (_stricmp(n.c_str(),"RShift")==0) return VK_RSHIFT;
    if (_stricmp(n.c_str(),"Alt")==0 || _stricmp(n.c_str(),"LAlt")==0) return VK_LMENU;
    if (_stricmp(n.c_str(),"RAlt")==0) return VK_RMENU;
    if (_stricmp(n.c_str(),"Win")==0 || _stricmp(n.c_str(),"LWin")==0) return VK_LWIN;
    if (_stricmp(n.c_str(),"RWin")==0) return VK_RWIN;
    return 0;
}

// Разбор action-строки в упорядоченный список событий (down/up).
// Поддерживает: {F1}, {Ctrl+B} (комбо: hold моды → key → release), {A}{B} (последовательность),
// и одиночные буквы. Возвращает пустотой список, если ничего распознано.
static std::vector<KeyEvent> ParseKeyEvents(const std::string& keys) {
    std::vector<KeyEvent> out;
    const char* p = keys.c_str();
    while (*p) {
        if (*p == '{') {
            const char* end = strchr(p, '}');
            if (!end) break;
            std::string name; name.assign(p + 1, (size_t)(end - (p + 1)));
            UINT key = 0;
            if (name.find('+') == std::string::npos) {
                key = VkKeyName(name.c_str());
                if (!key && name.size() == 1) key = SingleCharToVk(name[0]);
                if (key) { out.push_back(MakeKeyEvent(key, true)); out.push_back(MakeKeyEvent(key, false)); }
            } else {
                // комбо: моды+клавиша
                std::vector<std::string> parts;
                std::string r = name;
                std::size_t pos = 0;
                while ((pos = r.find('+')) != std::string::npos) { parts.push_back(r.substr(0, pos)); r = r.substr(pos + 1); }
                parts.push_back(r);
                std::vector<UINT> mods;
                for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
                    UINT mv = ModNameToVk(parts[i]);
                    if (mv) mods.push_back(mv);
                }
                key = ModNameToVk(parts.back());
                if (!key) key = VkKeyName(parts.back().c_str());
                if (key) {
                    for (std::size_t i = 0; i < mods.size(); ++i) out.push_back(MakeKeyEvent(mods[i], true));
                    out.push_back(MakeKeyEvent(key, true));
                    out.push_back(MakeKeyEvent(key, false));
                    for (std::size_t i = mods.size(); i > 0; --i) out.push_back(MakeKeyEvent(mods[i - 1], false));
                }
            }
            p = end + 1;
        } else {
            if (*p != ' ') {
                UINT k = SingleCharToVk(*p);
                if (k) { out.push_back(MakeKeyEvent(k, true)); out.push_back(MakeKeyEvent(k, false)); }
            }
            p++;
        }
    }
    return out;
}

static LPARAM BuildWindowKeyLParam(
    const WindowKey& key,
    keysidekick::TargetedMessageState state) {
    std::uint32_t bits = 1u | ((key.scanCode & 0xFFu) << 16);
    if (key.extended) bits |= 1u << 24;
    bits |= keysidekick::TargetedMessageStateBits(state);
    return static_cast<LPARAM>(bits);
}

static bool PostWindowKey(HWND target,
                          const WindowKey& key,
                          keysidekick::TargetedMessageState state) {
    if (!target || !IsWindow(target)) return false;
    const UINT message = state == keysidekick::TargetedMessageState::KeyUp
        ? WM_KEYUP : WM_KEYDOWN;
    return PostMessageW(target, message, key.virtualKey,
                        BuildWindowKeyLParam(key, state)) != FALSE;
}

// Для hold/repeat: action должен быть ровно одной одиночной клавишей {Name}
// (без '+' и без последовательности). Возвращает тождество в out.
static bool ParseSingleKey(const std::string& action, WindowKey& out) {
    if (action.size() < 2 || action[0] != '{' || action[action.size()-1] != '}') return false;
    const std::string name = action.substr(1, action.size() - 2);
    if (name.find('+') != std::string::npos || name.find('{') != std::string::npos) return false;
    if (name.size() == 0) return false;
    UINT v = VkKeyName(name.c_str());
    if (!v && name.size() == 1) v = SingleCharToVk(name[0]);
    if (!v) return false;
    out.virtualKey = v;
    out.scanCode = MapVirtualKeyW(v, MAPVK_VK_TO_VSC) & 0xFF;
    out.extended = IsExtendedVirtualKey(v);
    return true;
}

static void SendKeyStringToWindow(HWND hwnd, const std::string& keys) {
    const std::vector<KeyEvent> ev = ParseKeyEvents(keys);
    for (std::size_t i = 0; i < ev.size(); ++i) {
        if (!ev[i].vk) continue;
        WindowKey wk;
        wk.virtualKey = ev[i].vk;
        wk.scanCode = ev[i].scan;
        wk.extended = ev[i].extended;
        PostWindowKey(hwnd, wk, ev[i].down
            ? keysidekick::TargetedMessageState::InitialDown
            : keysidekick::TargetedMessageState::KeyUp);
    }
}

// Глобальная эмуляция макро/комбо (basic-режим): SendInput по VK,
// моды → key → release. Каждое событие — отдельный INPUT.
static void SendMacroGlobal(const std::string& keys) {
    const std::vector<KeyEvent> ev = ParseKeyEvents(keys);
    for (std::size_t i = 0; i < ev.size(); ++i) {
        if (!ev[i].vk) continue;
        INPUT in = {0};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = ev[i].vk;
        in.ki.wScan = ev[i].scan;
        in.ki.dwFlags = ev[i].extended ? KEYEVENTF_EXTENDEDKEY : 0;
        if (!ev[i].down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT));
    }
}

// Найти окно target-приложения (по классу, фолбэк — по exe через Process→Window)
struct FindWinData { const char* cls; HWND found; };
static BOOL CALLBACK FindWndEnumProc(HWND h, LPARAM lp) {
    FindWinData* d = (FindWinData*)lp;
    wchar_t wcls[256];
    if (GetClassNameW(h, wcls, 256)) {
        char acls[256]; WideCharToMultiByte(CP_ACP, 0, wcls, -1, acls, 256, NULL, NULL);
        if (_stricmp(acls, d->cls) == 0) {
            if (IsWindowVisible(h)) { d->found = h; return FALSE; }
        }
    }
    return TRUE;
}
static HWND FindWindowByClass(const char* cls) {
    if (!cls || !*cls) return NULL;
    wchar_t wcls[256];
    int wc = MultiByteToWideChar(CP_ACP, 0, cls, -1, wcls, 256);
    if (wc <= 0) return NULL;
    HWND h = FindWindowW(wcls, NULL);
    if (h) return h;
    // фолбэк: перечислить, найти видимое
    FindWinData d{cls, NULL};
    EnumWindows(FindWndEnumProc, (LPARAM)&d);
    return d.found;
}

// Phase 3: Scored target resolver using windows_targets.
// Находит лучшее окно по windowClass + processName + processPath.
// Возвращает HWND или NULL. Не блокирует (нет Sleep).
static HWND FindTargetWindowScored(const std::string& windowClass,
                                   const std::string& processName,
                                   const std::string& processPath) {
    if (windowClass.empty() && processName.empty() && processPath.empty()) return NULL;

    auto toWstring = [](const std::string& s) -> std::wstring {
        if (s.empty()) return std::wstring();
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
        std::wstring ws(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], len);
        return ws;
    };

    keysidekick::windows_targets::WindowFilterPolicy policy;
    policy.requireVisible = true;
    policy.excludeEmptyTitles = false;
    policy.excludeToolWindows = false;
    policy.excludeShellWindows = false;

    std::vector<keysidekick::windows_targets::WindowCandidate> windows =
        keysidekick::windows_targets::EnumerateWindows(policy);

    keysidekick::windows_targets::TargetQuery query;
    query.windowClass = toWstring(windowClass);
    query.processName = toWstring(processName);
    query.processPath = toWstring(processPath);

    keysidekick::windows_targets::WindowCandidate resolved;
    if (keysidekick::windows_targets::ResolveTarget(windows, query, &resolved, policy)) {
        return (HWND)resolved.handle;
    }
    return NULL;
}

// Запуск приложения (для AutoStart)
static void LaunchApp(const std::string& path) {
    if (path.empty()) return;
    wchar_t wpath[1024];
    int wc = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wpath, 1024);
    if (wc <= 0 || wc >= 1024) {
        Log("LaunchApp: path too long or invalid");
        return;
    }
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    std::wstring cmd = wpath;
    if (CreateProcessW(NULL, (LPWSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        Log("Launched: %s", path.c_str());
    } else {
        Log("Launch failed err=%lu: %s", GetLastError(), path.c_str());
    }
}

// Отправить клавиши в окно профиля.
// Phase 3: использует scored resolver (class + processName + processPath).
// AutoStart: запускает приложение асинхронно (без blocking Sleep), ключи
// отправятся при следующем нажатии когда окно появится.
static HWND FindProfileTarget(const Profile& prof) {
    HWND h = FindTargetWindowScored(prof.targetClass, prof.targetExe, prof.targetPath);
    if (!h) h = FindWindowByClass(prof.targetClass.c_str());
    return h;
}

static void HandleMissingProfileTarget(const Profile& prof) {
    if (prof.autoStart && !prof.targetPath.empty()) {
        Log("Target not found, launching async: %s", prof.name.c_str());
        LaunchApp(prof.targetPath);
    } else {
        Log("Target not found, skipped: %s", prof.name.c_str());
    }
}

static void SendToTargetWindow(const Profile& prof, const std::string& keys) {
    HWND h = FindProfileTarget(prof);
    if (!h) {
        HandleMissingProfileTarget(prof);
        return;
    }
    SendKeyStringToWindow(h, keys);
    Log("Sent '%s' to %s", keys.c_str(), prof.name.c_str());
}

// Override-таргет для токена !app:<name>:<keys>.
// Phase 3: сначала ищет по appName как windowClass через scored resolver,
// потом fallback на профиль с таким именем.
static void SendToNamedApp(const std::string& appName, const std::string& keys) {
    // Scored resolver: appName как windowClass
    HWND h = FindTargetWindowScored(appName, "", "");
    if (!h) h = FindWindowByClass(appName.c_str());
    if (!h) {
        // Fallback: appName может быть именем профиля/ApplicationTarget
        EnterCriticalSection(&g_csProfile);
        auto it = g_profiles.find(appName);
        std::string cls2 = (it != g_profiles.end()) ? it->second.targetClass : "";
        std::string exe2 = (it != g_profiles.end()) ? it->second.targetExe : "";
        std::string path2 = (it != g_profiles.end()) ? it->second.targetPath : "";
        bool as2 = (it != g_profiles.end()) ? it->second.autoStart : false;
        LeaveCriticalSection(&g_csProfile);
        if (!cls2.empty()) {
            h = FindTargetWindowScored(cls2, exe2, path2);
            if (!h) h = FindWindowByClass(cls2.c_str());
            if (!h && as2 && !path2.empty()) {
                Log("Named app not found, launching async: %s", appName.c_str());
                LaunchApp(path2);  // async, без blocking Sleep
            }
        }
    }
    if (h) {
        SendKeyStringToWindow(h, keys);
        Log("Sent '%s' to named app %s", keys.c_str(), appName.c_str());
    } else {
        Log("Named app window not found: %s", appName.c_str());
    }
}

// =====================================================================
//  Basic mode: SendInput re-inject
// =====================================================================
static BYTE g_prevModifiers = 0;
static int  g_prevReport[6] = {0,0,0,0,0,0};

// Ownership ledger: tracks only keys we injected so ReleaseAllKeys
// releases only those, not main-keyboard modifiers.
static keysidekick::InputLedger g_injectedKeys;
static keysidekick::TargetedInputLedger g_targetedKeys;
static std::uint32_t g_targetedRepeatDelayMs = 500;
static std::uint32_t g_targetedRepeatIntervalMs = 100;

static void SendOneScan(unsigned short scan, bool extended, bool keyUp) {
    keysidekick::ScanKey key(scan & 0xFF, extended);
    if (keyUp && !g_injectedKeys.owns(key)) return;

    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = scan & 0xFF;          // для extended в scan хранится 0xE0XX, берём младший байт
    in.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (extended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (keyUp)    in.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));

    // Track ownership: record down/up so ReleaseAllKeys can release only
    // keys we actually injected (not main-keyboard modifiers).
    if (!keyUp) g_injectedKeys.recordDown(key);
    else        g_injectedKeys.recordUp(key);
}

// Basic re-inject всего отчёта, исключая usageIds из exceptSet (которые обрабатываются как action).
// ВАЖНО: если в отчёте есть action-key, модификаторы НЕ инжектятся в систему —
// иначе KEYDOWN-модификатор потом невозможно снять (SendInput KEYUP ненадёжен
// для scan-based событий), и он залипает. Модификаторы обновляются только в
// g_prevModifiers для корректного edge-detection, но в систему не уходят.
static void BasicReinject(const BYTE* report, DWORD len, const std::vector<int>& exceptUsages) {
    if (len < 2) return;

    BYTE mod = report[0];
    bool hasActionKey = !exceptUsages.empty();

    // --- Modifiers diff (byte 0) ---
    if (!hasActionKey) {   // инжектим модификаторы только если нет action-клавиши
        BYTE modDelta = mod ^ g_prevModifiers;
        if (modDelta) {
            for (auto& mb : MOD_BITS) {
                if (modDelta & mb.mask) {
                    ScanInfo si = UsageToSet1(mb.usage);
                    bool keyUp = !(mod & mb.mask);   // бит снят → keyup
                    SendOneScan(si.scan, si.extended, keyUp);
                }
            }
        }
    }
    g_prevModifiers = mod;   // всегда обновляем edge-detect (даже если не инжектили)

    // --- Keys diff (bytes 2-7) ---
    int cur[6] = {0,0,0,0,0,0};
    int ncur = 0;
    for (int i = 2; i < 8 && i < (int)len; i++) {
        if (report[i] != 0 && ncur < 6) {
            int uid = report[i];
            // пропустить usage из exceptUsages (они обработаны как action)
            bool skip = false;
            for (int e : exceptUsages) if (e == uid) { skip = true; break; }
            if (!skip) cur[ncur++] = uid;
        }
    }

    // KEYUP: usage, которые были в prev, но нет в cur
    for (int k = 0; k < 6; k++) {
        int uid = g_prevReport[k];
        if (uid == 0) continue;
        bool stillHeld = false;
        for (int j = 0; j < ncur; j++) if (cur[j] == uid) { stillHeld = true; break; }
        if (!stillHeld) {
            ScanInfo si = UsageToSet1(uid);
            if (si.scan) SendOneScan(si.scan, si.extended, true);
        }
    }
    // KEYDOWN: usage, которые есть в cur, но не было в prev
    for (int j = 0; j < ncur; j++) {
        int uid = cur[j];
        bool wasHeld = false;
        for (int k = 0; k < 6; k++) if (g_prevReport[k] == uid) { wasHeld = true; break; }
        if (!wasHeld) {
            ScanInfo si = UsageToSet1(uid);
            if (si.scan) SendOneScan(si.scan, si.extended, false);
        }
    }
    // обновить prev (только неотфильтрованные usage)
    for (int k = 0; k < 6; k++) g_prevReport[k] = (k < ncur) ? cur[k] : 0;
}

// =====================================================================
//  Грамматика действий: разбор value из keyMap
//  Возвращает true если это action-токен (обработан здесь), false если обычные клавиши.
// =====================================================================
static std::string g_lastProfileBeforeToggle;   // для !toggle

// Forward declarations (определены ниже в секции tray/windowing)
static void UpdateTray();
static void ShowTrayMenu(HWND h, int x, int y);
static void SwitchProfileByName(const std::string& name);
static void ReleaseAllKeys();   // определена ниже (в Entry)
static void ReleaseAllTargetedKeys();
static void ReleaseTargetedUsage(int usageId);
static bool SendHoldableKeyToTarget(const Profile& prof,
                                    int usageId,
                                    const std::string& action);

// Вызывается ПОСЛЕ смены активного профиоля.
// (1) Отпускает targeted-клавиши, отправленные старому окну.
// (2) Если мы покинули basic-режим, отпускает все клавиши, которые
//     basic-режим удерживал "нажатыми" через SendInput — иначе они залипают
//     в системе (т.к. KEYUP для них уже никогда не отправится).
static void OnProfileSwitched(const std::string& oldName, const std::string& newName) {
    ProfileMode oldMode = MODE_BASIC, newMode = MODE_BASIC;
    EnterCriticalSection(&g_csProfile);
    auto itOld = g_profiles.find(oldName);
    if (itOld != g_profiles.end()) oldMode = itOld->second.mode;
    auto itNew = g_profiles.find(newName);
    if (itNew != g_profiles.end()) newMode = itNew->second.mode;
    LeaveCriticalSection(&g_csProfile);

    ReleaseAllTargetedKeys();

    if (oldMode == MODE_BASIC && newMode != MODE_BASIC) {
        ReleaseAllKeys();
        Log("Profile %s→%s: released injected keys", oldName.c_str(), newName.c_str());
    }
}

static bool ExecuteAction(const std::string& action, int usageId) {
    if (action.empty()) return false;
    if (action[0] != '!') return false;   // не action

    if (action.rfind("!switch:", 0) == 0) {
        std::string target = action.substr(8);
        EnterCriticalSection(&g_csProfile);
        if (g_profiles.find(target) != g_profiles.end()) {
            std::string oldName = g_activeProfile;
            g_activeProfile = target;
            LeaveCriticalSection(&g_csProfile);
            Log("Switch → %s (usage 0x%02X)", target.c_str(), usageId);
            OnProfileSwitched(oldName, target);
            UpdateTray();
            BumpRevision();
            return true;
        }
        LeaveCriticalSection(&g_csProfile);
        Log("Switch target not found: %s", target.c_str());
        return true;
    }
    if (action.rfind("!toggle:", 0) == 0) {
        std::string target = action.substr(8);
        EnterCriticalSection(&g_csProfile);
        std::string oldName = g_activeProfile;
        std::string newName;
        if (g_activeProfile == target) {
            // вернуться к предыдущему
            if (!g_lastProfileBeforeToggle.empty() && g_profiles.find(g_lastProfileBeforeToggle) != g_profiles.end())
                newName = g_lastProfileBeforeToggle;
            else
                newName = g_defaultProfile;
        } else {
            g_lastProfileBeforeToggle = g_activeProfile;
            if (g_profiles.find(target) != g_profiles.end())
                newName = target;
            else
                newName = g_activeProfile;
        }
        g_activeProfile = newName;
        LeaveCriticalSection(&g_csProfile);
        Log("Toggle → %s (usage 0x%02X)", newName.c_str(), usageId);
        if (newName != oldName) OnProfileSwitched(oldName, newName);
        UpdateTray();
        return true;
    }
    if (action.rfind("!launch:", 0) == 0) {
        LaunchApp(action.substr(8));
        return true;
    }
    if (action.rfind("!app:", 0) == 0) {
        // формат: !app:<name>:<keys>
        std::string rest = action.substr(5);
        size_t colon = rest.find(':');
        if (colon != std::string::npos) {
            std::string appName = rest.substr(0, colon);
            std::string keys    = rest.substr(colon+1);
            SendToNamedApp(appName, keys);
        }
        return true;
    }
    Log("Unknown action token: %s", action.c_str());
    return true;
}

// Найти mapping для usage ID с учётом модификаторов.
// Precedence: сначала точное совпадение modMask == curMods; если нет — fallback на modMask==0.
// Возвращает найденный mapping или nullptr. matchIsActionOnlyReinjectExclude — для basic mode.
// Проверить, что текущие модификаторы (curMods) удовлетворяют требуемой маске (reqMask).
// reqMask задаётся парами битов (L|R) на группу: 0x01|0x10=Ctrl, 0x02|0x20=Shift, и т.д.
// Логика: для каждой группы, требуемой в reqMask, хотя бы одна сторона (L или R) нажата в curMods.
// Группа не требуемая (нет её битов в reqMask) → в curMods для неё можно что угодно.
// Имя модификатора ("Ctrl"/"Shift"/"Alt"/"Win" + левые/правые) → 8-бит маска report[0].
static unsigned char ModifierNameToMask(const std::string& name) {
    if (_stricmp(name.c_str(), "Ctrl")==0) return 0x01|0x10;
    if (_stricmp(name.c_str(), "Shift")==0) return 0x02|0x20;
    if (_stricmp(name.c_str(), "Alt")==0) return 0x04|0x40;
    if (_stricmp(name.c_str(), "Win")==0) return 0x08|0x80;
    if (_stricmp(name.c_str(), "LCtrl")==0) return 0x01;
    if (_stricmp(name.c_str(), "RCtrl")==0) return 0x10;
    if (_stricmp(name.c_str(), "LShift")==0) return 0x02;
    if (_stricmp(name.c_str(), "RShift")==0) return 0x20;
    if (_stricmp(name.c_str(), "LAlt")==0) return 0x04;
    if (_stricmp(name.c_str(), "RAlt")==0) return 0x40;
    if (_stricmp(name.c_str(), "LWin")==0) return 0x08;
    if (_stricmp(name.c_str(), "RWin")==0) return 0x80;
    return 0;
}

static bool ModsMatch(unsigned char reqMask, unsigned char curMods) {
    if (reqMask == 0) return true;   // без модификаторов — всегда (но precedence ниже чем у конкретных)
    static const unsigned char pairs[4][2] = {
        {0x01, 0x10},  // LCtrl, RCtrl
        {0x02, 0x20},  // LShift, RShift
        {0x04, 0x40},  // LAlt, RAlt
        {0x08, 0x80},  // LWin, RWin
    };
    for (int i = 0; i < 4; i++) {
        bool reqThis = (reqMask & (pairs[i][0] | pairs[i][1])) != 0;
        if (!reqThis) continue;
        bool curL = (curMods & pairs[i][0]) != 0;
        bool curR = (curMods & pairs[i][1]) != 0;
        if (!curL && !curR) return false;   // группа требуется, но ни одна сторона не нажата
    }
    return true;
}

static const KeyMapping* FindMappingForUsage(const std::vector<KeyMapping>& keys, int uid, unsigned char curMods) {
    const KeyMapping* best = nullptr;     // наиболее специфичный матч
    const KeyMapping* bare = nullptr;     // modMask==0 fallback
    int bestScore = -1;
    for (auto& m : keys) {
        if (m.usageId != uid) continue;
        if (m.modMask == 0) { if (!bare) bare = &m; continue; }
        if (!ModsMatch(m.modMask, curMods)) continue;
        // специфичность = число требуемых групп модификаторов (более специфичный приоритетнее)
        int score = 0;
        unsigned char mm = m.modMask;
        if (mm & (0x01|0x10)) score++;
        if (mm & (0x02|0x20)) score++;
        if (mm & (0x04|0x40)) score++;
        if (mm & (0x08|0x80)) score++;
        if (score > bestScore) { bestScore = score; best = &m; }
    }
    return best ? best : bare;
}

// =====================================================================
//  ProcessReport — единая точка диспетчеризации
// =====================================================================
static void ProcessReport(const BYTE* report, DWORD len) {
    if (len < 3) return;

    Profile prof;   // копия по значению (безопасно при reload/edit)
    if (!ActiveProfileCopy(prof)) return;

    unsigned char curMods = report[0];   // текущие модификаторы (для precedence)

    // Собрать текущие usage IDs
    int cur[6] = {0,0,0,0,0,0};
    int ncur = 0;
    for (int i = 2; i < 8 && i < (int)len; i++) {
        if (report[i] != 0 && ncur < 6) cur[ncur++] = report[i];
    }

    // --- Capture: если дашборд запросил «ловить следующую клавишу» ---
    if (g_captureArmed.load()) {
        for (int j = 0; j < ncur; j++) {
            int uid = cur[j];
            bool wasHeld = false;
            for (int k = 0; k < 6; k++) if (g_prevReport[k] == uid) { wasHeld = true; break; }
            if (!wasHeld) {   // keydown-edge → поймали
                g_capturedUsage = uid;
                g_captureArmed = false;
                break;
            }
        }
    }

    // --- BASIC режим ---
    if (prof.mode == MODE_BASIC) {
        // Сначала собрать usage, у которых есть action-mapping в этом профиле → обработать их, исключить из re-inject
        std::vector<int> exceptUsages;
        for (int j = 0; j < ncur; j++) {
            int uid = cur[j];
            bool wasHeld = false;
            for (int k = 0; k < 6; k++) if (g_prevReport[k] == uid) { wasHeld = true; break; }
            if (wasHeld) continue;   // только keydown-фронты
            const KeyMapping* m = FindMappingForUsage(prof.keys, uid, curMods);
            if (m && !m->action.empty()) {
                exceptUsages.push_back(uid);
                RecordActivity(uid, m->action, "basic");
                if (!ExecuteAction(m->action, uid)) SendMacroGlobal(m->action);
            }
        }
        BasicReinject(report, len, exceptUsages);
        return;
    }

    // --- TARGETED режим ---
    for (int k = 0; k < 6; k++) {
        int uid = g_prevReport[k];
        if (uid == 0) continue;
        bool stillHeld = false;
        for (int j = 0; j < ncur; j++) {
            if (cur[j] == uid) { stillHeld = true; break; }
        }
        if (!stillHeld) ReleaseTargetedUsage(uid);
    }

    for (int j = 0; j < ncur; j++) {
        int uid = cur[j];
        bool wasHeld = false;
        for (int k = 0; k < 6; k++) if (g_prevReport[k] == uid) { wasHeld = true; break; }
        if (wasHeld) continue;   // edge: только keydown
        const KeyMapping* m = FindMappingForUsage(prof.keys, uid, curMods);
        if (!m || m->action.empty()) continue;
        RecordActivity(uid, m->action, "targeted");
        if (!ExecuteAction(m->action, uid)) {
            if (!SendHoldableKeyToTarget(prof, uid, m->action)) {
                SendToTargetWindow(prof, m->action);
            }
        }
    }
    g_prevModifiers = curMods;
    for (int k = 0; k < 6; k++) g_prevReport[k] = (k < ncur) ? cur[k] : 0;
}

// =====================================================================
//  WinUSB device discovery (без изменений по сравнению с v1)
// =====================================================================
static HANDLE g_hDevice = NULL;
static WINUSB_INTERFACE_HANDLE g_hWinUsb = NULL;
static BYTE g_interruptInPipe = 0xFF;
// H3: HTTP-воркеры читают device-глобалы, пока main-поток их переподключает.
// g_csDevInfo сериализует snapshot'ы против OpenDevice/CloseDevice.
static CRITICAL_SECTION g_csDevInfo;
static std::string g_activeDevicePath;   // интерфейс, открытый ReadLoop (под g_csDevInfo)
static bool DevInfoConnected() {
    EnterCriticalSection(&g_csDevInfo);
    bool c = (g_hWinUsb != NULL);
    LeaveCriticalSection(&g_csDevInfo);
    return c;
}
static bool IsActiveDevicePath(const std::string& path) {
    if (path.empty()) return false;
    bool same = false;
    EnterCriticalSection(&g_csDevInfo);
    if (!g_activeDevicePath.empty() && _stricmp(path.c_str(), g_activeDevicePath.c_str()) == 0) same = true;
    LeaveCriticalSection(&g_csDevInfo);
    return same;
}
static void DevInfoSnapshot(bool& connected, unsigned char& pipeId) {
    EnterCriticalSection(&g_csDevInfo);
    connected = (g_hWinUsb != NULL);
    pipeId = g_interruptInPipe;
    LeaveCriticalSection(&g_csDevInfo);
}
static std::atomic<bool> g_running{true};
static bool g_powerResume = false;
// C2: activation ("Make active") must never tear down WinUSB from the
// message-dispatch path — an overlapped read may be outstanding there, and
// WinUsb_Free with pending I/O is undefined behavior per MSDN. The DashOp
// only sets this flag + event; ReadLoop (the thread that owns the read)
// performs AbortPipe → close → reopen. All access is from the main thread.
static bool g_pendingReconnect = false;
static HANDLE g_reconnectRequestEvent = NULL;

// Найти устройство по списку VID/PID (разделённых запятой).
// Поддерживает multi-device: g_deviceVidPid может быть "vid_1234&pid_abcd,vid_5678&pid_ef01"
bool FindDevicePath(std::string& outPath) {
    const GUID* guids[] = {
        &GUID_DEVINTERFACE_TARGET_WINUSB,
        &GUID_DEVINTERFACE_WINUSB,
        &GUID_DEVINTERFACE_LIBUSB0, NULL
    };
    // Split g_deviceVidPid by commas into search patterns
    std::vector<std::string> patterns;
    {
        std::string vidpid(g_deviceVidPid);
        std::size_t pos = 0;
        while (pos <= vidpid.size()) {
            std::size_t comma = vidpid.find(',', pos);
            std::string part = (comma != std::string::npos) ? vidpid.substr(pos, comma - pos) : vidpid.substr(pos);
            // Trim whitespace
            std::size_t start = part.find_first_not_of(" \t");
            std::size_t end = part.find_last_not_of(" \t");
            if (start != std::string::npos) part = part.substr(start, end - start + 1);
            if (!part.empty()) patterns.push_back(ToLower(part.c_str()));
            pos = (comma != std::string::npos) ? comma + 1 : vidpid.size() + 1;
        }
    }
    if (patterns.empty()) return false;

    for (int g = 0; guids[g]; g++) {
        HDEVINFO hDevInfo = SetupDiGetClassDevs(guids[g], NULL, NULL,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hDevInfo == INVALID_HANDLE_VALUE) continue;
        SP_DEVICE_INTERFACE_DATA ifData = { sizeof(ifData) };
        for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, guids[g], idx, &ifData); idx++) {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailA(hDevInfo, &ifData, NULL, 0, &needed, NULL);
            if (!needed) continue;
            std::vector<BYTE> buf(needed);
            PSP_DEVICE_INTERFACE_DETAIL_DATA_A pDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)buf.data();
            pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            if (!SetupDiGetDeviceInterfaceDetailA(hDevInfo, &ifData, pDetail, needed, NULL, NULL)) continue;
            char lower[1024];
            strncpy(lower, pDetail->DevicePath, sizeof(lower)-1);
            lower[sizeof(lower)-1] = 0;
            for (char* p = lower; *p; p++) *p = tolower(*p);
            bool match = false;
            for (std::size_t pi = 0; pi < patterns.size(); ++pi) {
                if (strstr(lower, patterns[pi].c_str()) != NULL) { match = true; break; }
            }
            if (match) {
                outPath = pDetail->DevicePath;
                SetupDiDestroyDeviceInfoList(hDevInfo);
                return true;
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }
    return false;
}

// M3: resolve the interrupt-IN pipe id from the interface descriptor.
// 0x81 is the common default but NOT guaranteed — devices whose keyboard
// interface uses a different pipe would never read/verify otherwise.
// Returns 0xFF when no interrupt-IN endpoint is present.
static BYTE QueryInterruptInPipe(WINUSB_INTERFACE_HANDLE wusb) {
    USB_INTERFACE_DESCRIPTOR ifDesc;
    if (!WinUsb_QueryInterfaceSettings(wusb, 0, &ifDesc)) return 0xFF;
    for (BYTE i = 0; i < ifDesc.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pi;
        if (WinUsb_QueryPipe(wusb, 0, i, &pi) && pi.PipeType == UsbdPipeTypeInterrupt &&
            (pi.PipeId & 0x80)) {
            return pi.PipeId;
        }
    }
    return 0xFF;
}

static bool OpenDevice(const std::string& path) {
    EnterCriticalSection(&g_csDevInfo);
    g_activeDevicePath = path;
    int wlen = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, NULL, 0);
    std::vector<wchar_t> wpath(wlen);
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wpath.data(), wlen);
    g_hDevice = CreateFileW(wpath.data(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (g_hDevice == INVALID_HANDLE_VALUE) { g_hDevice = NULL; LeaveCriticalSection(&g_csDevInfo); Log("CreateFile err=%lu", GetLastError()); return false; }
    if (!WinUsb_Initialize(g_hDevice, &g_hWinUsb)) {
        Log("WinUsb_Initialize err=%lu", GetLastError());
        CloseHandle(g_hDevice); g_hDevice = NULL; LeaveCriticalSection(&g_csDevInfo); return false;
    }
    g_interruptInPipe = QueryInterruptInPipe(g_hWinUsb);
    if (g_interruptInPipe == 0xFF) {
        Log("No interrupt IN endpoint found");
        WinUsb_Free(g_hWinUsb); g_hWinUsb = NULL;
        CloseHandle(g_hDevice); g_hDevice = NULL;
        LeaveCriticalSection(&g_csDevInfo);
        return false;
    }
    // Pipe policies. timeout=0 means infinite wait (no pipe-level timeout).
    // ReadLoop uses MsgWaitForMultipleObjectsEx with a bounded wait so it can
    // pump window messages and detect device loss without blocking forever.
    UCHAR rawIo = 0;
    WinUsb_SetPipePolicy(g_hWinUsb, g_interruptInPipe, RAW_IO, sizeof(rawIo), &rawIo);
    ULONG timeoutMs = 0;
    WinUsb_SetPipePolicy(g_hWinUsb, g_interruptInPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
    Log("Opened device, pipe=0x%02X", g_interruptInPipe);
    LeaveCriticalSection(&g_csDevInfo);
    return true;
}

// Инвариант: CloseDevice вызывается ТОЛЬКО когда overlapped read завершён
// (GetOverlappedResult или AbortPipe+wait). WinUsb_Free с outstanding IRP —
// UB по MSDN. Каждый вызов обязан сначала погасить pending read.
static void CloseDevice() {
    EnterCriticalSection(&g_csDevInfo);
    ReleaseAllTargetedKeys();
    ReleaseAllKeys();
    g_prevModifiers = 0;
    for (int index = 0; index < 6; ++index) g_prevReport[index] = 0;
    if (g_hWinUsb) { WinUsb_Free(g_hWinUsb); g_hWinUsb = NULL; }
    if (g_hDevice) { CloseHandle(g_hDevice); g_hDevice = NULL; }
    g_activeDevicePath.clear();
    LeaveCriticalSection(&g_csDevInfo);
}

static bool ReconnectDevice() {
    CloseDevice();
    Sleep(500);
    std::string path;
    int tries = 0;
    while (g_running && tries < 10) {
        if (FindDevicePath(path) && OpenDevice(path)) { Log("Reconnected"); return true; }
        Sleep(1000); tries++;
    }
    return false;
}

// =====================================================================
//  Phase "HID-first": перечисление ВСЕХ устройств ввода (до и после Zadig)
//  HID-класс даёт обычные клавиатуры/мыши (hidusb.sys); WinUSB/libusb —
//  уже конвертированные. Merge по USB instance id, чтобы показать
//  «до Zadig клавиатура обычная» и статус драйвера каждого интерфейса.
// =====================================================================

static std::string WideToUtf8(const wchar_t* ws) {
    if (!ws || !*ws) return std::string();
    int wlen = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
    if (wlen <= 1) return std::string();
    std::string s;
    s.resize(wlen - 1);
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], wlen - 1, NULL, NULL);
    return s;
}

// Instance id девайса, экспонировавшего интерфейс (devInfo из 6-го аргумента
// SetupDiGetDeviceInterfaceDetailA).
static std::string DevInstIdOf(HDEVINFO hDev, const SP_DEVINFO_DATA& devInfo) {
    char buf[512];
    DWORD sz = sizeof(buf);
    if (SetupDiGetDeviceInstanceIdA(hDev, (PSP_DEVINFO_DATA)&devInfo, buf, sizeof(buf), &sz))
        return std::string(buf);
    return std::string();
}

// Registry-строка девнода (SPDRP_SERVICE / SPDRP_FRIENDLYNAME / SPDRP_DEVICEDESC ...).
static std::string DevRegStr(HDEVINFO hDev, const SP_DEVINFO_DATA& devInfo, DWORD property) {
    char buf[512] = {0};
    DWORD type = 0, sz = 0;
    if (SetupDiGetDeviceRegistryPropertyA(hDev, (PSP_DEVINFO_DATA)&devInfo,
                                          property, &type, (PBYTE)buf, sizeof(buf), &sz))
        return std::string(buf);
    return std::string();
}

// DEVPKEY_Device_Parent — этот MinGW поставляет пустой devpkey.h, определяем сами
// (документированный ключ: fmtid {4340A6C5-93FA-4716-B340-53B27999E541}, pid 8).
static const DEVPROPKEY kDevpropParent = {
    {0x4340A6C5, 0x93FA, 0x4716, {0xB3, 0x40, 0x53, 0xB2, 0x79, 0x99, 0xE5, 0x41}}, 8
};

// cfgmgr32.dll для CM_Get_Parent: SetupDiGetDevicePropertyW возвращает
// ERROR_NOT_FOUND (1168) для DEVPKEY_Device_Parent на HID-узлах, полученных
// из device-interface (PowerShell берёт parent именно через CfgMgr API).
typedef DWORD (WINAPI *FnCM_Get_Parent)(DWORD*, DWORD, ULONG);
typedef DWORD (WINAPI *FnCM_Get_Device_IDW)(DWORD, PWSTR, ULONG, ULONG);
static FnCM_Get_Parent g_pCM_Get_Parent = NULL;
static FnCM_Get_Device_IDW g_pCM_Get_Device_IDW = NULL;

static bool CfgmgrLoaded() {
    static bool tried = false;
    static bool loaded = false;
    if (tried) return loaded;
    tried = true;
    HMODULE m = LoadLibraryW(L"cfgmgr32.dll");
    if (!m) return false;
    g_pCM_Get_Parent = (FnCM_Get_Parent)GetProcAddress(m, "CM_Get_Parent");
    g_pCM_Get_Device_IDW = (FnCM_Get_Device_IDW)GetProcAddress(m, "CM_Get_Device_IDW");
    loaded = (g_pCM_Get_Parent && g_pCM_Get_Device_IDW);
    return loaded;
}

// Parent-девнод (USB\...\&MI_xx\... узел) для HID-коллекции — это merge-ключ
// с Pass A (WinUSB-узлы).
static std::string ParentInstIdOf(HDEVINFO hDev, const SP_DEVINFO_DATA& devInfo) {
    DEVPROPTYPE propType = 0;
    WCHAR parentBuf[512] = {0};
    if (SetupDiGetDevicePropertyW(hDev, (PSP_DEVINFO_DATA)&devInfo,
                                  &kDevpropParent, &propType,
                                  (PBYTE)parentBuf, sizeof(parentBuf), NULL, 0))
        return WideToUtf8(parentBuf);
    // Фолбэк: CM_Get_Parent по devInfo.DevInst.
    if (CfgmgrLoaded()) {
        DWORD parent = 0;
        if (g_pCM_Get_Parent(&parent, devInfo.DevInst, 0) == 0 /*CR_SUCCESS*/ && parent != 0) {
            WCHAR idBuf[512] = {0};
            if (g_pCM_Get_Device_IDW(parent, idBuf, 512, 0) == 0)
                return WideToUtf8(idBuf);
        }
    }
    return std::string();
}

// SPDRP_SERVICE девнода, найденного по instance id (SetupDiOpenDeviceInfoA).
static std::string ServiceOfInstanceId(const std::string& instId) {
    if (instId.empty()) return std::string();
    HDEVINFO hDev = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (hDev == INVALID_HANDLE_VALUE) return std::string();
    SP_DEVINFO_DATA di = { sizeof(di) };
    std::string svc;
    if (SetupDiOpenDeviceInfoA(hDev, instId.c_str(), NULL, 0, &di))
        svc = DevRegStr(hDev, di, SPDRP_SERVICE);
    SetupDiDestroyDeviceInfoList(hDev);
    return svc;
}

// Вытащить vid/pid/mi из instance id или device path ("VID_1234&PID_ABCD&MI_00").
static void VidPidMiFromString(const std::string& s, std::string& vid, std::string& pid, std::string& mi) {
    vid.clear(); pid.clear(); mi.clear();
    std::string low = ToLower(s.c_str());
    std::size_t vp = low.find("vid_");
    if (vp == std::string::npos) return;
    std::size_t i = vp + 4;
    while (i < low.size() && vid.size() < 4 && isxdigit((unsigned char)low[i])) { vid += low[i]; ++i; }
    std::size_t pp = low.find("pid_", vp);
    if (pp == std::string::npos) return;
    i = pp + 4;
    while (i < low.size() && pid.size() < 4 && isxdigit((unsigned char)low[i])) { pid += low[i]; ++i; }
    std::size_t mp = low.find("&mi_", vp);
    if (mp != std::string::npos) {
        i = mp + 4;
        while (i < low.size() && mi.size() < 2 && isxdigit((unsigned char)low[i])) { mi += low[i]; ++i; }
    }
}

static std::string MiFromString(const std::string& s) {
    std::string vid, pid, mi;
    VidPidMiFromString(s, vid, pid, mi);
    return mi;
}

struct InputDeviceRow {
    std::string usbId;       // USB\VID_xxxx&PID_yyyy[&MI_xx]\... (merge key)
    std::string name;
    std::string vid, pid, mi;
    std::string service;     // HidUsb / WinUSB / libusbK / ...
    std::string kinds;       // comma-joined: keyboard/mouse/consumer/other/winusb
    bool hasHid = false;
    bool winusb = false;
    std::string hidPath;
    std::string winusbPath;
};

static void AddKind(InputDeviceRow& r, const std::string& k) {
    if (r.kinds.empty()) r.kinds = k;
    else if (r.kinds.find(k) == std::string::npos) { r.kinds += ","; r.kinds += k; }
}

static InputDeviceRow& RowFor(std::vector<InputDeviceRow>& rows,
                              std::map<std::string, std::size_t>& byUsbId,
                              const std::string& key) {
    std::map<std::string, std::size_t>::iterator it = byUsbId.find(key);
    if (it == byUsbId.end()) {
        byUsbId[key] = rows.size();
        rows.push_back(InputDeviceRow());
        rows.back().usbId = key;
        return rows.back();
    }
    return rows[it->second];
}

// hid.dll через динамический импорт: libhid.a в этом MinGW сломан (import-lib
// без head-объекта — _head_lib64_libhid_a не определён, символы не линкуются).
// Типы (HIDD_ATTRIBUTES, PHIDP_PREPARSED_DATA, HIDP_CAPS) берём из hidsdi.h/hidpi.h.
typedef BOOL (WINAPI *FnHidD_GetHidGuid)(GUID*);
typedef BOOL (WINAPI *FnHidD_GetAttributes)(HANDLE, PHIDD_ATTRIBUTES);
typedef BOOL (WINAPI *FnHidD_GetPreparsedData)(HANDLE, PHIDP_PREPARSED_DATA*);
typedef BOOL (WINAPI *FnHidD_FreePreparsedData)(PHIDP_PREPARSED_DATA);
typedef BOOL (WINAPI *FnHidD_GetProductString)(HANDLE, PVOID, ULONG);
typedef BOOL (WINAPI *FnHidD_GetManufacturerString)(HANDLE, PVOID, ULONG);
typedef NTSTATUS (WINAPI *FnHidP_GetCaps)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);

static FnHidD_GetHidGuid          g_pHidD_GetHidGuid = NULL;
static FnHidD_GetAttributes       g_pHidD_GetAttributes = NULL;
static FnHidD_GetPreparsedData    g_pHidD_GetPreparsedData = NULL;
static FnHidD_FreePreparsedData   g_pHidD_FreePreparsedData = NULL;
static FnHidD_GetProductString    g_pHidD_GetProductString = NULL;
static FnHidD_GetManufacturerString g_pHidD_GetManufacturerString = NULL;
static FnHidP_GetCaps             g_pHidP_GetCaps = NULL;

static bool HidDllLoaded() {
    static bool tried = false;
    static bool loaded = false;
    if (tried) return loaded;
    tried = true;
    HMODULE m = LoadLibraryW(L"hid.dll");
    if (!m) { Log("LoadLibrary(hid.dll) failed err=%lu", GetLastError()); return false; }
    g_pHidD_GetHidGuid          = (FnHidD_GetHidGuid)GetProcAddress(m, "HidD_GetHidGuid");
    g_pHidD_GetAttributes       = (FnHidD_GetAttributes)GetProcAddress(m, "HidD_GetAttributes");
    g_pHidD_GetPreparsedData    = (FnHidD_GetPreparsedData)GetProcAddress(m, "HidD_GetPreparsedData");
    g_pHidD_FreePreparsedData   = (FnHidD_FreePreparsedData)GetProcAddress(m, "HidD_FreePreparsedData");
    g_pHidD_GetProductString    = (FnHidD_GetProductString)GetProcAddress(m, "HidD_GetProductString");
    g_pHidD_GetManufacturerString = (FnHidD_GetManufacturerString)GetProcAddress(m, "HidD_GetManufacturerString");
    g_pHidP_GetCaps             = (FnHidP_GetCaps)GetProcAddress(m, "HidP_GetCaps");
    loaded = g_pHidD_GetHidGuid && g_pHidD_GetAttributes && g_pHidD_GetPreparsedData &&
             g_pHidD_FreePreparsedData && g_pHidD_GetProductString && g_pHidD_GetManufacturerString &&
             g_pHidP_GetCaps;
    if (!loaded) Log("hid.dll: missing exports — HID enumeration disabled");
    return loaded;
}

static void EnumerateInputDevices(std::vector<InputDeviceRow>& rows) {
    std::map<std::string, std::size_t> byUsbId;

    // --- Pass A: WinUSB / libusb интерфейсы (уже конвертированные, до HID не видны) ---
    const GUID* guids[] = { &GUID_DEVINTERFACE_WINUSB, &GUID_DEVINTERFACE_TARGET_WINUSB, &GUID_DEVINTERFACE_LIBUSB0, NULL };
    for (int g = 0; guids[g]; g++) {
        HDEVINFO hDev = SetupDiGetClassDevs(guids[g], NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hDev == INVALID_HANDLE_VALUE) continue;
        SP_DEVICE_INTERFACE_DATA ifData = { sizeof(ifData) };
        for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hDev, NULL, guids[g], idx, &ifData); idx++) {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailA(hDev, &ifData, NULL, 0, &needed, NULL);
            if (!needed) continue;
            std::vector<BYTE> buf(needed);
            PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)buf.data();
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            SP_DEVINFO_DATA devInfo = { sizeof(devInfo) };
            if (!SetupDiGetDeviceInterfaceDetailA(hDev, &ifData, det, needed, NULL, &devInfo)) continue;

            std::string key = DevInstIdOf(hDev, devInfo);
            if (key.empty()) continue;
            InputDeviceRow& r = RowFor(rows, byUsbId, key);
            r.winusb = true;
            if (r.winusbPath.empty()) r.winusbPath = det->DevicePath;
            if (r.service.empty()) {
                r.service = DevRegStr(hDev, devInfo, SPDRP_SERVICE);
                if (r.service.empty()) r.service = ServiceOfInstanceId(key);
            }
            if (r.name.empty()) {
                std::string fn = DevRegStr(hDev, devInfo, SPDRP_FRIENDLYNAME);
                if (fn.empty()) fn = DevRegStr(hDev, devInfo, SPDRP_DEVICEDESC);
                r.name = fn;
            }
            if (r.vid.empty()) VidPidMiFromString(key, r.vid, r.pid, r.mi);
            AddKind(r, "winusb");
        }
        SetupDiDestroyDeviceInfoList(hDev);
    }

    // --- Pass B: HID-класс (обычные клавиатуры/мыши, до Zadig) ---
    if (HidDllLoaded()) {
    GUID hidGuid;
    g_pHidD_GetHidGuid(&hidGuid);
    HDEVINFO hHid = SetupDiGetClassDevs(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hHid != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifData = { sizeof(ifData) };
        for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hHid, NULL, &hidGuid, idx, &ifData); idx++) {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailA(hHid, &ifData, NULL, 0, &needed, NULL);
            if (!needed) continue;
            std::vector<BYTE> buf(needed);
            PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)buf.data();
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            SP_DEVINFO_DATA devInfo = { sizeof(devInfo) };
            if (!SetupDiGetDeviceInterfaceDetailA(hHid, &ifData, det, needed, NULL, &devInfo)) continue;

            // Открыть (fallback read-only) для аттрибутов/имени/usage.
            HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE)
                h = CreateFileA(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);

            std::string vid, pid, product, manufacturer, kind = "other";
            PHIDP_PREPARSED_DATA ppd = NULL;
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attr = { sizeof(attr) };
                if (g_pHidD_GetAttributes(h, &attr)) {
                    char tmp[16];
                    snprintf(tmp, sizeof(tmp), "%04X", (unsigned)attr.VendorID); vid = tmp;
                    snprintf(tmp, sizeof(tmp), "%04X", (unsigned)attr.ProductID); pid = tmp;
                }
                if (g_pHidD_GetPreparsedData(h, &ppd)) {
                    HIDP_CAPS caps;
                    if (g_pHidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                        if (caps.UsagePage == 0x01 && caps.Usage == 0x06) kind = "keyboard";
                        else if (caps.UsagePage == 0x01 && caps.Usage == 0x02) kind = "mouse";
                        else if (caps.UsagePage == 0x0C) kind = "consumer";
                        else if (caps.UsagePage == 0x01) kind = "generic";
                    }
                }
                wchar_t wbuf[256] = {0};
                if (g_pHidD_GetProductString(h, wbuf, sizeof(wbuf))) product = WideToUtf8(wbuf);
                memset(wbuf, 0, sizeof(wbuf));
                if (g_pHidD_GetManufacturerString(h, wbuf, sizeof(wbuf))) manufacturer = WideToUtf8(wbuf);
            }

            // Merge-ключ: parent (USB-интерфейс) — тот же, что у WinUSB-узла в Pass A.
            std::string instId = DevInstIdOf(hHid, devInfo);
            std::string parentId = ParentInstIdOf(hHid, devInfo);
            std::string key = parentId.empty() ? instId : parentId;
            if (!key.empty()) {
                InputDeviceRow& r = RowFor(rows, byUsbId, key);
                r.hasHid = true;
                if (r.hidPath.empty()) r.hidPath = det->DevicePath;
                if (r.vid.empty() && !vid.empty()) { r.vid = vid; r.pid = pid; }
                if (r.mi.empty()) r.mi = MiFromString(key);
                if (r.service.empty()) r.service = ServiceOfInstanceId(key);
                if (r.name.empty()) {
                    r.name = product;
                    if (r.name.empty()) r.name = manufacturer;
                    if (r.name.empty()) {
                        std::string fn = DevRegStr(hHid, devInfo, SPDRP_FRIENDLYNAME);
                        if (fn.empty()) fn = DevRegStr(hHid, devInfo, SPDRP_DEVICEDESC);
                        r.name = fn;
                    }
                }
                AddKind(r, kind);
            }
            if (ppd) g_pHidD_FreePreparsedData(ppd);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        SetupDiDestroyDeviceInfoList(hHid);
    }
    } else {
        Log("HID enumeration skipped: hid.dll unavailable");
    }

    // Фолбэк имени для строк без product/friendly name.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        InputDeviceRow& r = rows[i];
        if (r.name.empty()) {
            std::string vp = r.vid.empty() ? "device" : ("VID_" + r.vid + " PID_" + r.pid);
            r.name = std::string("HID input ") + vp;
        }
    }
}

// ---- Raw Input identification ("press a key to identify") ----
// Windows сливает все клавиатуры в один поток; RAWINPUTHEADER.hDevice даёт
// пер-устройственную идентичность для большинства устройств, но NULL для
// композитных (клавиатура + тачпад/consumer). Это та же техника, что AHK.
struct IdentifyEvent {
    unsigned long seq = 0;
    unsigned int vk = 0;          // virtual key
    unsigned int makeCode = 0;    // scan code (Set-1)
    bool identifiable = false;    // hDevice присутствовал (не композитный-NULL)
    std::string vid, pid, mi, name, path;
};
struct IdentifiedInput {
    bool has = false;             // было ли нажатие с последнего сброса
    bool identifiable = false;
    unsigned long seq = 0;
    unsigned int lastVk = 0;
    unsigned int lastMakeCode = 0;
    std::string vid, pid, mi, name, path;
};
static IdentifiedInput g_identified;
// Live-поток последних нажатий (для фида «какая клавиша → с какого устройства»).
static std::vector<IdentifyEvent> g_identifyEvents;
static unsigned long g_identifySeq = 0;
static CRITICAL_SECTION g_csIdentified;

// IDENTIFY: record one press event (keyboard key-down OR mouse button-down)
// into the identify ring buffer with per-device attribution (vid/pid/mi/name
// resolved from the RAWINPUT hDevice). Shared by the RIM_TYPEKEYBOARD and
// RIM_TYPEMOUSE branches of WM_INPUT.
static void RecordIdentifyEvent(HANDLE hDevice, unsigned int vk, unsigned int makeCode) {
    EnterCriticalSection(&g_csIdentified);
    g_identified.has = true;
    g_identified.identifiable = (hDevice != NULL);
    g_identified.seq = ++g_identifySeq;
    g_identified.lastVk = vk;
    g_identified.lastMakeCode = makeCode;
    if (hDevice) {
        UINT len = 0;
        GetRawInputDeviceInfoA(hDevice, RIDI_DEVICENAME, NULL, &len);
        if (len > 0 && len < 4096) {
            std::string path(len, '\0');
            UINT l2 = len;
            if (GetRawInputDeviceInfoA(hDevice, RIDI_DEVICENAME, &path[0], &l2) != (UINT)-1) {
                path.resize(l2);
                std::size_t nul = path.find('\0');   // имя может иметь padding
                if (nul != std::string::npos) path.resize(nul);
                g_identified.path = path;
                VidPidMiFromString(path, g_identified.vid, g_identified.pid, g_identified.mi);
            }
        }
        if (g_identified.vid.empty()) {
            RID_DEVICE_INFO info; memset(&info, 0, sizeof(info)); info.cbSize = sizeof(info);
            UINT isz = sizeof(info);
            if (GetRawInputDeviceInfoA(hDevice, RIDI_DEVICEINFO, &info, &isz) != (UINT)-1 && info.dwType == RIM_TYPEHID) {
                char tmp[16];
                snprintf(tmp, sizeof(tmp), "%04X", (unsigned)info.hid.dwVendorId);  g_identified.vid = tmp;
                snprintf(tmp, sizeof(tmp), "%04X", (unsigned)info.hid.dwProductId); g_identified.pid = tmp;
            }
        }
        g_identified.name = g_identified.vid.empty() ? "unknown" : (g_identified.vid + "&" + g_identified.pid);
    } else {
        g_identified.vid.clear(); g_identified.pid.clear();
        g_identified.name.clear(); g_identified.path.clear();
    }
    // Занести в live-поток.
    IdentifyEvent ev;
    ev.seq = g_identified.seq;
    ev.vk = vk; ev.makeCode = makeCode;
    ev.identifiable = g_identified.identifiable;
    ev.vid = g_identified.vid; ev.pid = g_identified.pid; ev.mi = g_identified.mi;
    ev.name = g_identified.name; ev.path = g_identified.path;
    g_identifyEvents.push_back(ev);
    if (g_identifyEvents.size() > 16) g_identifyEvents.erase(g_identifyEvents.begin());
    LeaveCriticalSection(&g_csIdentified);
}


// =====================================================================
//  Окно сообщений: WM_POWERBROADCAST + tray callback
// =====================================================================
static HWND g_hMsgWindow = NULL;
static UINT g_openDashboardMsg = 0;  // от второго инстанса (app_instance)
static std::atomic<bool> g_deviceChangeNotified{false};  // WM_DEVICECHANGE: plug/unplug
// Port-change: клавиатура с настроенным VID/PID найдена как ОБЫЧНОЕ HID-
// устройство (переткнули в другой порт без серийника → новый узел без WinUSB).
static bool g_portChangeDetected = false;    // под g_csProfile
static std::string g_portChangeVidPid;       // под g_csProfile
static std::string g_portChangeHwid;         // под g_csProfile
static const wchar_t* MSG_WND_CLASS = L"WinUsbRouterMsgSink";
static const UINT_PTR TARGETED_REPEAT_TIMER_ID = 2;
#define WM_TRAYICON (WM_APP+1)
#define WM_HTTP_SWITCH (WM_APP+2)
#define WM_USER_SHUTDOWN (WM_APP+5)
#define WM_AUTO_SWITCH (WM_APP+4)  // auto-switch timer message
static UINT g_trayTaskbarCreated = 0;   // для пересоздания иконки при рестарте explorer
#define TRAY_UID 1
static HMENU g_trayMenu = NULL;
static std::string g_lastAutoSwitchCheck;   // last checked foreground process
static HWND g_lastAutoSwitchHwnd = NULL;    // idle-guard: то же окно — ничего не делаем
static bool g_autoSwitchEnabled = true;    // auto-switch on foreground change (enabled by default)
static HICON g_trayIcon = NULL;

static void UpdateTray();   // forward

// forward-declarations (определены позже в LoadConfig-секции, но нужны MsgWndProc'у)
static void ReloadConfig();
static bool WriteConfig();

static void RefreshTargetedRepeatSettings() {
    UINT delaySetting = 0;
    UINT speedSetting = 0;
    if (SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delaySetting, 0)) {
        g_targetedRepeatDelayMs = keysidekick::KeyboardRepeatDelayMs(delaySetting);
    } else {
        g_targetedRepeatDelayMs = 500;
    }
    if (SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speedSetting, 0)) {
        g_targetedRepeatIntervalMs = keysidekick::KeyboardRepeatIntervalMs(speedSetting);
    } else {
        g_targetedRepeatIntervalMs = 100;
    }
}

static WindowKey WindowKeyFromHeld(const keysidekick::TargetedKey& held) {
    WindowKey key;
    key.virtualKey = held.virtualKey;
    key.scanCode = held.scanCode;
    key.extended = held.extended;
    return key;
}

static void ScheduleTargetedRepeatTimer() {
    if (!g_hMsgWindow) return;
    KillTimer(g_hMsgWindow, TARGETED_REPEAT_TIMER_ID);

    std::uint64_t deadline = 0;
    if (!g_targetedKeys.nextRepeatAt(&deadline)) return;

    const std::uint64_t now = GetTickCount64();
    const std::uint64_t remaining = deadline > now ? deadline - now : 1;
    const UINT waitMs = remaining > 0x7FFFFFFFu
        ? 0x7FFFFFFFu : static_cast<UINT>(remaining);
    if (!SetTimer(g_hMsgWindow, TARGETED_REPEAT_TIMER_ID, waitMs, NULL)) {
        Log("Targeted repeat timer failed err=%lu", GetLastError());
    }
}

static void DispatchTargetedRepeats() {
    const std::uint64_t now = GetTickCount64();
    const std::vector<keysidekick::TargetedKey> due =
        g_targetedKeys.dueRepeats(now);
    for (std::vector<keysidekick::TargetedKey>::const_iterator held = due.begin();
         held != due.end(); ++held) {
        HWND target = reinterpret_cast<HWND>(held->target);
        if (PostWindowKey(target, WindowKeyFromHeld(*held),
                          keysidekick::TargetedMessageState::RepeatDown)) {
            continue;
        }
        g_targetedKeys.recordUp(held->usageId, NULL);
        Log("Targeted repeat stopped: target unavailable for usage 0x%02X",
            held->usageId);
    }
    ScheduleTargetedRepeatTimer();
}

static void ReleaseTargetedUsage(int usageId) {
    keysidekick::TargetedKey held = {0, 0, 0, 0, false, 0, 0};
    if (!g_targetedKeys.recordUp(usageId, &held)) return;

    HWND target = reinterpret_cast<HWND>(held.target);
    if (!PostWindowKey(target, WindowKeyFromHeld(held),
                       keysidekick::TargetedMessageState::KeyUp)) {
        Log("Targeted key-up target unavailable for usage 0x%02X", usageId);
    }
    ScheduleTargetedRepeatTimer();
}

static void ReleaseAllTargetedKeys() {
    if (g_hMsgWindow) KillTimer(g_hMsgWindow, TARGETED_REPEAT_TIMER_ID);
    const std::vector<keysidekick::TargetedKey> held = g_targetedKeys.releaseAll();
    for (std::vector<keysidekick::TargetedKey>::const_iterator key = held.begin();
         key != held.end(); ++key) {
        HWND target = reinterpret_cast<HWND>(key->target);
        PostWindowKey(target, WindowKeyFromHeld(*key),
                      keysidekick::TargetedMessageState::KeyUp);
    }
    if (!held.empty()) Log("Released %zu targeted keys", held.size());
}

static bool SendHoldableKeyToTarget(const Profile& prof,
                                    int usageId,
                                    const std::string& action) {
    WindowKey single;
    if (!ParseSingleKey(action, single)) return false;   // не одиночная клавиша → fall through к SendToTargetWindow (комбо/последовательность)
    if (g_targetedKeys.owns(usageId)) return true;

    HWND target = FindProfileTarget(prof);
    if (!target) {
        HandleMissingProfileTarget(prof);
        return true;
    }
    if (!PostWindowKey(target, single,
                       keysidekick::TargetedMessageState::InitialDown)) {
        Log("Targeted key-down failed for %s usage 0x%02X",
            prof.name.c_str(), usageId);
        return true;
    }

    keysidekick::TargetedKey held = {
        usageId,
        reinterpret_cast<std::uintptr_t>(target),
        static_cast<std::uint16_t>(single.virtualKey),
        static_cast<std::uint16_t>(single.scanCode),
        single.extended,
        GetTickCount64() + g_targetedRepeatDelayMs,
        g_targetedRepeatIntervalMs
    };
    if (!g_targetedKeys.recordDown(held)) {
        PostWindowKey(target, single, keysidekick::TargetedMessageState::KeyUp);
        return true;
    }

    ScheduleTargetedRepeatTimer();
    Log("Targeted key-down '%s' to %s", action.c_str(), prof.name.c_str());
    return true;
}

// Phase 2: Resolve profileId-or-name to canonical domain profile id.
// Domain profiles have migration ids like "profile-aimp-<hash>", but users/API
// refer to them by name ("aimp"). Also handles "basic" → Normal profile id.
// Returns empty string if not found.
static std::string ResolveDomainProfileId(const std::string& idOrName) {
    if (idOrName.empty()) return std::string();
    // Direct id match first
    const keysidekick::Profile* dp = g_domain.findProfile(idOrName);
    if (dp) return dp->id();
    // "basic" → Normal
    if (idOrName == "basic") {
        dp = g_domain.findProfile(keysidekick::Profile::normalId());
        if (dp) return dp->id();
    }
    // Match by name (case-insensitive, domain findProfile already does this)
    for (std::size_t i = 0; i < g_domain.profiles.size(); ++i) {
        if (keysidekick::equalsCaseInsensitive(g_domain.profiles[i].name, idOrName)) {
            return g_domain.profiles[i].id();
        }
    }
    return std::string();
}

// Case-insensitive substring search (UTF-8 safe for ASCII-only content)
static bool UTF8ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    std::string h = haystack, n = needle;
    for (auto& c : h) c = std::tolower((unsigned char)c);
    for (auto& c : n) c = std::tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

static void AutoSwitchOnForegroundChange() {
    if (!g_autoSwitchEnabled) return;
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    // Idle-guard: пока фокус на том же окне, никаких OpenProcess/Query —
    // таймер 1с в простое стоит одного GetForegroundWindow.
    if (fg == g_lastAutoSwitchHwnd) return;
    g_lastAutoSwitchHwnd = fg;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return;
    std::wstring procName;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t pp[MAX_PATH] = {0};
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, pp, &sz)) {
            procName = pp;
            std::size_t sl = procName.find_last_of(L'\\');
            if (sl != std::wstring::npos) procName = procName.substr(sl + 1);
        }
        CloseHandle(hProc);
    }
    std::string procUtf8;
    int wlen = WideCharToMultiByte(CP_UTF8, 0, procName.c_str(), (int)procName.size(), NULL, 0, NULL, NULL);
    if (wlen > 0) {
        procUtf8.resize(wlen);
        WideCharToMultiByte(CP_UTF8, 0, procName.c_str(), (int)procName.size(), &procUtf8[0], wlen, NULL, NULL);
    }
    if (procUtf8 == g_lastAutoSwitchCheck) return;
    g_lastAutoSwitchCheck = procUtf8;
    // Find a targeted profile with target_exe matching this process
    EnterCriticalSection(&g_csProfile);
    for (auto& kv : g_profiles) {
        const Profile& prof = kv.second;
        if (prof.mode != MODE_TARGETED) continue;
        // Check if targetExe matches the foreground process
        if (_stricmp(prof.targetExe.c_str(), procUtf8.c_str()) == 0 ||
            (prof.targetExe.empty() ? false : UTF8ContainsCaseInsensitive(procUtf8, prof.targetExe))) {
            // Don't switch to current profile
            if (_stricmp(prof.name.c_str(), g_activeProfile.c_str()) == 0) continue;
            std::string oldName = g_activeProfile;
            g_activeProfile = prof.name;
            LeaveCriticalSection(&g_csProfile);
            Log("Auto-switch → %s (foreground: %s)", prof.name.c_str(), procUtf8.c_str());
            OnProfileSwitched(oldName, prof.name);
            UpdateTray();
            BumpRevision();
            return;
        }
    }
    LeaveCriticalSection(&g_csProfile);
}

// Auto-switch timer: 1s interval
static void StartAutoSwitchTimer() {
    if (g_hMsgWindow && g_autoSwitchEnabled) {
        SetTimer(g_hMsgWindow, 9999, 1000, NULL);
    }
}
// Case-insensitive substring search (UTF-8 safe for ASCII-only content)
static LRESULT CALLBACK MsgWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        // Raw Input identification: записать источник последнего нажатия
        // (hDevice → VID/PID), чтобы мастер мог определить клавиатуру по нажатию.
        UINT sz = 0;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            sz > 0 && sz <= 4096) {
            BYTE* buf = (BYTE*)malloc(sz);
            if (buf) {
                UINT gotSz = sz;
                if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &gotSz, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                    RAWINPUT* ri = (RAWINPUT*)buf;
                    if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                        // Считаем только key-down (не key-up), чтобы фид был «нажатые клавиши».
                        if (!(ri->data.keyboard.Flags & RI_KEY_BREAK)) {
                            RecordIdentifyEvent(ri->header.hDevice,
                                ri->data.keyboard.VKey, ri->data.keyboard.MakeCode);
                        }
                    } else if (ri->header.dwType == RIM_TYPEMOUSE) {
                        // IDENTIFY: мыши регистрируются (0x01/0x02, RIDEV_INPUTSINK),
                        // но кнопки никогда не идентифицировались. Кнопка-вниз — это
                        // полноценное нажатие: vk = VK_LBUTTON/RBUTTON/MBUTTON/XBUTTON*,
                        // makeCode = 0 (у мыши нет Set-1 scan кода).
                        const unsigned int f = ri->data.mouse.usButtonFlags;
                        if (f & RI_MOUSE_LEFT_BUTTON_DOWN)   RecordIdentifyEvent(ri->header.hDevice, VK_LBUTTON, 0);
                        if (f & RI_MOUSE_RIGHT_BUTTON_DOWN)  RecordIdentifyEvent(ri->header.hDevice, VK_RBUTTON, 0);
                        if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) RecordIdentifyEvent(ri->header.hDevice, VK_MBUTTON, 0);
                        if (f & RI_MOUSE_BUTTON_4_DOWN)      RecordIdentifyEvent(ri->header.hDevice, VK_XBUTTON1, 0);
                        if (f & RI_MOUSE_BUTTON_5_DOWN)      RecordIdentifyEvent(ri->header.hDevice, VK_XBUTTON2, 0);
                    }
                }
                free(buf);
            }
        }
        return 0;
    }
    if (msg == WM_TIMER && wp == 9999) {
        AutoSwitchOnForegroundChange();
        return 0;
    }
    if (msg == WM_TIMER && wp == TARGETED_REPEAT_TIMER_ID) {
        KillTimer(h, TARGETED_REPEAT_TIMER_ID);
        DispatchTargetedRepeats();
        return 0;
    }
    if (msg == WM_SETTINGCHANGE &&
        (wp == SPI_SETKEYBOARDDELAY || wp == SPI_SETKEYBOARDSPEED)) {
        RefreshTargetedRepeatSettings();
        return 0;
    }
    if (msg == WM_POWERBROADCAST) {
        if (wp == PBT_APMSUSPEND) {
            ReleaseAllTargetedKeys();
            ReleaseAllKeys();
            return TRUE;
        }
        if (wp == PBT_APMRESUMESUSPEND || wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMECRITICAL) {
            Log("Power resume → reconnect");
            g_powerResume = true;
            if (g_hWinUsb && g_interruptInPipe != 0xFF) WinUsb_AbortPipe(g_hWinUsb, g_interruptInPipe);
        }
        return TRUE;
    }
    if (msg == g_trayTaskbarCreated && g_trayEnabled) {
        UpdateTray();   // explorer перезапустился — пересоздать иконку
        return 0;
    }
    if (msg == g_openDashboardMsg && g_openDashboardMsg != 0) {
        // Второй инстанс попросил открыть дашборд (app_instance → FindWindowW
        // по классу окна → точечное сообщение вместо HWND_BROADCAST).
        char url[64];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", g_httpPort);
        ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }
    if (msg == WM_TRAYICON) {
        // Phase 6: left-click → open dashboard, right-click → context menu
        if (lp == WM_LBUTTONUP) {
            char url[64];
            snprintf(url, sizeof(url), "http://127.0.0.1:%d/", g_httpPort);
            ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
        } else if (lp == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            ShowTrayMenu(h, pt.x, pt.y);
        }
        return 0;
    }
    if (msg == WM_HTTP_SWITCH) {
        // из HTTP-потока: wp = указатель на строку (выделена malloc)
        const char* profName = (const char*)wp;
        SwitchProfileByName(profName);
        free((void*)profName);
        return 0;
    }
    if (msg == WM_DEVICECHANGE) {
        // USB plug/unplug: ждём в событийном цикле (device-absent path) —
        // рескан энумерации запустится сразу, без периодического поллинга.
        g_deviceChangeNotified = true;
        return TRUE;
    }
    if (msg == WM_USER_SHUTDOWN) {
        // Запрос завершения из ConsoleHandler: abort pending read ЗДЕСЬ,
        // на main-потоке (владельце overlapped read), затем циклы видят
        // g_running == false и выходят сами.
        if (g_hWinUsb && g_interruptInPipe != 0xFF) WinUsb_AbortPipe(g_hWinUsb, g_interruptInPipe);
        return 0;
    }
    if (msg == WM_DASH_OP) {
        // из HTTP-потока: wp = DashOp* (heap). Применяем на main thread под локом.
        DashOp* op = (DashOp*)wp;
        op->success = false;
        op->error[0] = 0;
        bool leaveLock = true;   // epilogue отпускает g_csProfile (кроме DASH_RELOAD)
        EnterCriticalSection(&g_csProfile);
        switch (op->op) {
            case DASH_ADD_KEY: {
                auto it = g_profiles.find(op->profile);
                if (it == g_profiles.end()) { snprintf(op->error, sizeof(op->error), "profile '%s' not found", op->profile); }
                else {
                    // удалить существующее с тем же (usage, mod) и добавить новое
                    auto& keys = it->second.keys;
                    for (auto k = keys.begin(); k != keys.end(); ) {
                        if (k->usageId == op->usage && k->modMask == op->mod) k = keys.erase(k);
                        else ++k;
                    }
                    KeyMapping km; km.usageId = op->usage; km.modMask = op->mod; km.action = op->action;
                    keys.push_back(km);
                    op->success = true;
                }
                break;
            }
            case DASH_REMOVE_KEY: {
                auto it = g_profiles.find(op->profile);
                if (it == g_profiles.end()) { snprintf(op->error, sizeof(op->error), "profile not found"); }
                else {
                    auto& keys = it->second.keys;
                    for (auto k = keys.begin(); k != keys.end(); ) {
                        if (k->usageId == op->usage && k->modMask == op->mod) k = keys.erase(k);
                        else ++k;
                    }
                    op->success = true;   // удаление всегда «успешно» (даже если не было)
                }
                break;
            }
            case DASH_SET_PROFILE: {
                // не позволяем ломать built-in basic
                Profile& p = g_profiles[op->profile];
                p.name = op->profile;
                if (op->profile != std::string("basic")) {
                    p.mode = (op->mode == 0) ? MODE_BASIC : MODE_TARGETED;
                    p.targetClass = op->targetClass;
                    p.targetExe = op->targetExe;
                    p.targetPath = op->targetPath;
                    p.autoStart = op->autoStart != 0;
                    p.layerModName = op->layerMod;
                    p.layerModMask = ModifierNameToMask(p.layerModName);
                }
                op->success = true;
                break;
            }
            case DASH_ACTIVATE_DEVICE: {
                // op->action = нормализованное "vid_xxxx&pid_yyyy" — добавить в
                // g_deviceVidPid (comma-separated, без дублей) и переподключиться.
                std::string pat(op->action);
                if (pat.empty()) {
                    snprintf(op->error, sizeof(op->error), "missing vidpid");
                } else {
                    std::string cur(g_deviceVidPid);
                    // cur: split по запятой, trim, lowercase-сравнение
                    bool present = false;
                    std::string lcur = ToLower(cur.c_str());
                    std::size_t pos = 0;
                    while (pos <= lcur.size()) {
                        std::size_t comma = lcur.find(',', pos);
                        std::string part = (comma != std::string::npos) ? lcur.substr(pos, comma - pos) : lcur.substr(pos);
                        std::size_t st = part.find_first_not_of(" \t");
                        std::size_t en = part.find_last_not_of(" \t");
                        if (st != std::string::npos) part = part.substr(st, en - st + 1);
                        if (part == pat) { present = true; break; }
                        pos = (comma != std::string::npos) ? comma + 1 : lcur.size() + 1;
                    }
                    if (!present) {
                        if (!cur.empty()) cur += ",";
                        cur += pat;
                        strncpy(g_deviceVidPid, cur.c_str(), sizeof(g_deviceVidPid)-1);
                        g_deviceVidPid[sizeof(g_deviceVidPid)-1] = 0;
                        op->needsReconnect = true;
                        Log("Activated device: %s", pat.c_str());
                    }
                    op->success = true;
                }
                break;
            }
            case DASH_APPLY_PRESET: {
                // op: profile=profileId, strArg1=name, strArg2=agentId,
                //     targetClass/targetExe/targetPath = выбранное окно (или defaults)
                try {
                    const AgentPreset* ap = FindAgentPreset(op->strArg2);
                    if (!ap) {
                        snprintf(op->error, sizeof(op->error), "unknown agent '%s'", op->strArg2);
                        break;
                    }
                    std::string profId = op->profile[0] ? std::string(op->profile) : (std::string("agent-") + ap->agentId);
                    if (g_domain.findProfile(profId)) {
                        snprintf(op->error, sizeof(op->error), "profile already exists: %s", profId.c_str());
                        break;
                    }
                    std::string profName = op->strArg1[0] ? std::string(op->strArg1) : (std::string(ap->name) + " pad");

                    keysidekick::ProfileMode pm = (_stricmp(ap->profileMode, "basic") == 0)
                        ? keysidekick::ProfileMode::Normal : keysidekick::ProfileMode::Targeted;
                    keysidekick::ProfileService svc(g_domain);
                    svc.createProfile(profId, profName, pm);
                    keysidekick::Profile* prov = g_domain.findProfile(profId);
                    if (!prov) { snprintf(op->error, sizeof(op->error), "profile not created"); break; }

                    if (pm == keysidekick::ProfileMode::Targeted) {
                        // ApplicationTarget (для app-targeted маршрутизации и авто-свитча)
                        std::string appId = "app-" + profId;
                        g_domain.applications.push_back(keysidekick::ApplicationTarget(appId, profName));
                        keysidekick::ApplicationTarget& app = g_domain.applications.back();
                        app.windowClass = op->targetClass[0] ? op->targetClass : (ap->defaultWindowClass ? ap->defaultWindowClass : "");
                        app.processName = op->targetExe[0]   ? op->targetExe   : (ap->defaultProcessName ? ap->defaultProcessName : "");
                        app.exePath     = op->targetPath[0]   ? op->targetPath : "";
                        prov->linkedApplicationIds.push_back(appId);
                        prov->defaultApplicationId = appId;
                    }

                    // Маппинги: физическая клавиша → макро (action строка в Action::profileId)
                    int order = 0;
                    for (int i = 0; i < ap->keyCount; ++i) {
                        const AgentKeyDef& d = ap->keys[i];
                        char mid[64];
                        snprintf(mid, sizeof(mid), "%s-%d", profId.c_str(), order);
                        keysidekick::Action a = keysidekick::Action::sendKey((unsigned)d.usage & 0xFF);
                        a.profileId = d.action;   // pass-through carrier (raw action string)
                        keysidekick::Trigger tr((unsigned)d.usage & 0xFF, 0);
                        keysidekick::Mapping mp(mid, order++, tr, a,
                                                keysidekick::Destination::defaultApplication());
                        prov->mappings.push_back(mp);
                    }

                    ProjectDomainToRuntime();
                    op->success = true;
                    Log("Applied pad template: %s (%s mode=%s)", profId.c_str(), ap->agentId, ap->profileMode);
                } catch (const std::exception& e) {
                    snprintf(op->error, sizeof(op->error), "preset failed: %s", e.what());
                }
                break;
            }
            case DASH_FIRE_ACTION: {
                // Live click-to-fire: выполнить action «как будто нажата клавиша»
                // на активном (или указанном) профиле. Без persist (noPersist=true).
                try {
                    std::string profName = op->profile[0] ? std::string(op->profile) : g_activeProfile;
                    std::string action(op->action);
                    if (action.empty()) {
                        snprintf(op->error, sizeof(op->error), "empty action");
                        break;
                    }
                    Profile prof;
                    bool found = false;
                    {
                        EnterCriticalSection(&g_csProfile);
                        auto it = g_profiles.find(profName);
                        found = (it != g_profiles.end());
                        if (found) prof = it->second;   // копия по значению
                        LeaveCriticalSection(&g_csProfile);
                    }
                    if (!found) {
                        snprintf(op->error, sizeof(op->error), "profile '%s' not found", profName.c_str());
                        break;
                    }
                    int uid = op->usage;
                    std::string mode = (prof.mode == MODE_BASIC) ? "basic" : "targeted";
                    RecordActivity(uid, action, mode);
                    if (!ExecuteAction(action, uid)) {
                        if (prof.mode == MODE_BASIC) {
                            SendMacroGlobal(action);
                        } else {
                            if (!SendHoldableKeyToTarget(prof, uid, action)) {
                                SendToTargetWindow(prof, action);
                            }
                        }
                    }
                    op->success = true;
                } catch (const std::exception& e) {
                    snprintf(op->error, sizeof(op->error), "fire failed: %s", e.what());
                }
                break;
            }
            case DASH_RELOAD:
                leaveLock = false;                 // ReloadConfig берёт лок сам
                LeaveCriticalSection(&g_csProfile);
                ReloadConfig();
                op->success = true;
                break;                             // M4: общий epilogue — cleanup при timeout
            // ---- Mapping management (in-place edit, reorder, duplicate) ----
            case DASH_UPDATE_KEY: {
                // Update existing mapping (usageId, mod, action→newAction)
                auto it = g_profiles.find(op->profile);
                if (it == g_profiles.end()) { snprintf(op->error, sizeof(op->error), "profile not found"); }
                else {
                    bool found = false;
                    for (auto& k : it->second.keys) {
                        if (k.usageId == op->usage && k.modMask == op->mod) {
                            k.action = op->action;  // new action from action field
                            found = true;
                            break;
                        }
                    }
                    if (found) op->success = true;
                    else snprintf(op->error, sizeof(op->error), "key not found");
                }
                break;
            }
            case DASH_MOVE_KEY: {
                // Reorder: move key up/down by swapping positions
                // op->strArg1 = direction ("up" or "down"), op->profile = profile name
                auto it = g_profiles.find(op->profile);
                if (it == g_profiles.end()) { snprintf(op->error, sizeof(op->error), "profile not found"); }
                else {
                    auto& keys = it->second.keys;
                    for (std::size_t i = 0; i < keys.size(); ++i) {
                        if (keys[i].usageId == op->usage && keys[i].modMask == op->mod) {
                            std::string dir = op->strArg1;
                            if (dir == "up" && i > 0) {
                                std::swap(keys[i], keys[i-1]);
                                op->success = true;
                            } else if (dir == "down" && i + 1 < keys.size()) {
                                std::swap(keys[i], keys[i+1]);
                                op->success = true;
                            } else {
                                op->success = true; // already at boundary — no-op
                            }
                            break;
                        }
                    }
                }
                break;
            }
            case DASH_DUPLICATE_KEY: {
                // Copy a mapping to a different key on same profile
                auto it = g_profiles.find(op->profile);
                if (it == g_profiles.end()) { snprintf(op->error, sizeof(op->error), "profile not found"); }
                else {
                    // Find source mapping and duplicate it
                    std::string sourceAction;
                    unsigned char sourceMod = 0;
                    bool found = false;
                    for (const auto& k : it->second.keys) {
                        if (k.usageId == op->usage) {
                            sourceAction = k.action;
                            sourceMod = k.modMask;
                            found = true;
                            break;
                        }
                    }
                    if (found && op->strArg1[0]) {
                        int newUsage = atoi(op->strArg1);
                        // Check if new key already exists
                        bool exists = false;
                        for (const auto& k : it->second.keys) {
                            if (k.usageId == newUsage && k.modMask == sourceMod) { exists = true; break; }
                        }
                        if (!exists) {
                            KeyMapping km;
                            km.usageId = newUsage;
                            km.modMask = sourceMod;
                            km.action = sourceAction;
                            it->second.keys.push_back(km);
                            op->success = true;
                        } else {
                            snprintf(op->error, sizeof(op->error), "key already mapped");
                        }
                    } else {
                        snprintf(op->error, sizeof(op->error), "source key not found or invalid target");
                    }
                }
                break;
            }
            // ---- Phase 2 CRUD operations (ProfileService on g_domain) ----
            case DASH_CREATE_PROFILE: {
                try {
                    keysidekick::ProfileService svc(g_domain);
                    std::string profId = op->profile;
                    std::string profName = op->strArg1[0] ? std::string(op->strArg1) : profId;
                    keysidekick::ProfileMode pm = (op->mode == 1)
                        ? keysidekick::ProfileMode::Targeted
                        : keysidekick::ProfileMode::Normal;
                    svc.createProfile(profId, profName, pm);
                    ProjectDomainToRuntime();
                    op->success = true;
                } catch (const std::exception& e) {
                    snprintf(op->error, sizeof(op->error), "%s", e.what());
                }
                break;
            }
            case DASH_DELETE_PROFILE: {
                try {
                    std::string profId = ResolveDomainProfileId(op->profile);
                    if (profId.empty()) throw keysidekick::NotFoundError("profile not found: " + std::string(op->profile));
                    keysidekick::ProfileService svc(g_domain);
                    svc.deleteProfile(profId);
                    // Cleanup: удалить orphan-приложения (напр. созданные пресетами),
                    // на которые больше не ссылается ни один профиль.
                    for (auto it = g_domain.applications.begin(); it != g_domain.applications.end(); ) {
                        bool referenced = false;
                        for (std::size_t pi = 0; pi < g_domain.profiles.size() && !referenced; ++pi) {
                            const keysidekick::Profile& p = g_domain.profiles[pi];
                            if (p.defaultApplicationId == it->id()) { referenced = true; break; }
                            for (std::size_t li = 0; li < p.linkedApplicationIds.size(); ++li)
                                if (p.linkedApplicationIds[li] == it->id()) { referenced = true; break; }
                        }
                        if (!referenced) it = g_domain.applications.erase(it);
                        else ++it;
                    }
                    ProjectDomainToRuntime();
                    op->success = true;
                } catch (const std::exception& e) {
                    snprintf(op->error, sizeof(op->error), "%s", e.what());
                }
                break;
            }
            case DASH_RENAME_PROFILE: {
                try {
                    std::string profId = ResolveDomainProfileId(op->profile);
                    if (profId.empty()) throw keysidekick::NotFoundError("profile not found: " + std::string(op->profile));
                    keysidekick::ProfileService svc(g_domain);
                    svc.renameProfile(profId, op->strArg1);
                    ProjectDomainToRuntime();
                    op->success = true;
                } catch (const std::exception& e) {
                    snprintf(op->error, sizeof(op->error), "%s", e.what());
                }
                break;
            }
            case DASH_DUPLICATE_PROFILE: {
                try {
                    std::string profId = ResolveDomainProfileId(op->profile);
                    if (profId.empty()) throw keysidekick::NotFoundError("profile not found: " + std::string(op->profile));
                    keysidekick::ProfileService svc(g_domain);
                    svc.duplicateProfile(profId, op->strArg1, op->strArg2);
                    ProjectDomainToRuntime();
                    op->success = true;
                } catch (const std::exception& e) {
                    snprintf(op->error, sizeof(op->error), "%s", e.what());
                }
                break;
            }
            case DASH_LINK_APP: {
                try {
                    std::string profId = ResolveDomainProfileId(op->profile);
                    if (profId.empty()) throw keysidekick::NotFoundError("profile not found: " + std::string(op->profile));
                    keysidekick::ProfileService svc(g_domain);
                    svc.linkApplication(profId, op->strArg1);
                    ProjectDomainToRuntime();
                    op->success = true;
                } catch (const std::exception& e) {
                    snprintf(op->error, sizeof(op->error), "%s", e.what());
                }
                break;
            }
            case DASH_UNLINK_APP: {
                try {
                    std::string profId = ResolveDomainProfileId(op->profile);
                    if (profId.empty()) throw keysidekick::NotFoundError("profile not found: " + std::string(op->profile));
                    keysidekick::ProfileService svc(g_domain);
                    svc.unlinkApplication(profId, op->strArg1);
                    ProjectDomainToRuntime();
                    op->success = true;
                } catch (const std::exception& e) {
                    snprintf(op->error, sizeof(op->error), "%s", e.what());
                }
                break;
            }
            case DASH_SET_DEFAULT_APP: {
                try {
                    std::string profId = ResolveDomainProfileId(op->profile);
                    if (profId.empty()) throw keysidekick::NotFoundError("profile not found: " + std::string(op->profile));
                    keysidekick::ProfileService svc(g_domain);
                    svc.setDefaultApplication(profId, op->strArg1);
                    ProjectDomainToRuntime();
                    op->success = true;
                } catch (const std::exception& e) {
                    snprintf(op->error, sizeof(op->error), "%s", e.what());
                }
                break;
            }
            default:
                snprintf(op->error, sizeof(op->error), "unknown op");
        }
        bool writeOk = true;
        if (op->success && op->op != DASH_RELOAD && !op->noPersist) {
            // WriteConfig вне локa (только main thread пишет файл)
            LeaveCriticalSection(&g_csProfile);
            WriteConfig();
            if (op->needsReconnect) {
                // C2: НЕ трогаем WinUSB с message-dispatch пути — там может
                // висеть overlapped read (WinUsb_Free с pending I/O = UB).
                // Только флагуем реконнект; ReadLoop (владелец read) сделает
                // AbortPipe → close → reopen. Оп завершается асинхронно,
                // dashboard видит результат через polling /api/v1/state.
                g_pendingReconnect = true;
                if (g_reconnectRequestEvent) SetEvent(g_reconnectRequestEvent);
            }
            BumpRevision();   // Phase 4: notify SSE clients of state change
        } else if (leaveLock) {
            LeaveCriticalSection(&g_csProfile);
        }
        // M4: читаем timedOut ДО SetEvent — после SetEvent вызывающий может
        // освободить op (окно UAF). Если таймаут уже был — мы владеем op.
        const bool timedOut = op->timedOut.load();
        SetEvent(op->doneEvent);
        if (timedOut) {
            CloseHandle(op->doneEvent);
            delete op;
        }
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

// (forward-объявления выше указаны для ShowTrayMenu/SwitchProfileByName/UpdateTray —
//  определим их ниже.)

static void CreateMsgWindow() {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = MSG_WND_CLASS;
    RegisterClassW(&wc);
    // Hidden top-level window (NOT HWND_MESSAGE): message-only windows
    // do not receive WM_POWERBROADCAST or TaskbarCreated broadcasts.
    // A 0x0 0x0 WS_POPUP window is invisible but receives all broadcasts.
    g_hMsgWindow = CreateWindowExW(0, MSG_WND_CLASS, L"KeySidekick",
        WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    g_trayTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // Событийное пробуждение при plug/unplug: device-absent цикл ждёт
    // сообщение WM_DEVICECHANGE вместо периодического рескана SetupAPI.
    {
        DEV_BROADCAST_DEVICEINTERFACE_W filter = {0};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        // dbcc_classguid = GUID_NULL: уведомления обо всех interface-классах.
        RegisterDeviceNotificationW(g_hMsgWindow, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    RefreshTargetedRepeatSettings();

    // Raw Input identification: принимать клавиатуру+мышь в фоне (RIDEV_INPUTSINK),
    // чтобы мастер мог определить источник нажатия (VID/PID). Та же техника, что AHK.
    RAWINPUTDEVICE rids[2] = {{0}, {0}};
    rids[0].usUsagePage = 0x01; rids[0].usUsage = 0x06;   // keyboards
    rids[0].dwFlags = RIDEV_INPUTSINK; rids[0].hwndTarget = g_hMsgWindow;
    rids[1].usUsagePage = 0x01; rids[1].usUsage = 0x02;   // mice
    rids[1].dwFlags = RIDEV_INPUTSINK; rids[1].hwndTarget = g_hMsgWindow;
    if (!RegisterRawInputDevices(rids, 2, sizeof(RAWINPUTDEVICE)))
        Log("RegisterRawInputDevices err=%lu", GetLastError());
}

// Подкачка сообщений окна (DashOp, tray, таймер автосвитча). Используется в основном
// цикле ожидания, когда ReadLoop не активен (устройство отсутствует) — иначе HTTP-мутации
// (DashOp через WM_DASH_OP) висят до таймаута.
static void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// =====================================================================
//  Tray icon + menu
// =====================================================================
static NOTIFYICONDATAW g_nid = {0};

static void BuildTrayMenu() {
    if (g_trayMenu) DestroyMenu(g_trayMenu);
    g_trayMenu = CreatePopupMenu();

    // Open Dashboard (Phase 6: left-click opens dashboard, but also in menu)
    AppendMenuA(g_trayMenu, MF_STRING, 2, "Open Dashboard");
    // About
    AppendMenuA(g_trayMenu, MF_STRING, 3, "About");

    // Device status line
    std::string devStatus = g_hWinUsb ? "Device: connected" : "Device: disconnected";
    AppendMenuA(g_trayMenu, MF_STRING | MF_DISABLED, 0, devStatus.c_str());

    AppendMenuA(g_trayMenu, MF_SEPARATOR, 0, "");

    // Profiles with mode indicator
    EnterCriticalSection(&g_csProfile);
    for (auto& kv : g_profiles) {
        const std::string& name = kv.first;
        const Profile& prof = kv.second;
        UINT flags = MF_STRING;
        if (_stricmp(name.c_str(), g_activeProfile.c_str()) == 0) flags |= MF_CHECKED;
        // Mode indicator: [B] = basic/Normal typing, [T] = targeted/app control
        std::string label = name;
        if (prof.mode == MODE_BASIC) label += "  [typing]";
        else label += "  [app control]";
        AppendMenuA(g_trayMenu, flags, 1000 + (int)std::distance(g_profiles.begin(), g_profiles.find(name)),
            label.c_str());
    }
    LeaveCriticalSection(&g_csProfile);

    AppendMenuA(g_trayMenu, MF_SEPARATOR, 0, "");
    AppendMenuA(g_trayMenu, MF_STRING, 1, "Exit");
}

static void ShowTrayMenu(HWND h, int x, int y) {
    BuildTrayMenu();
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(g_trayMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, x, y, 0, h, NULL);
    PostMessageW(h, WM_NULL, 0, 0);
    if (cmd == 1) { g_running = false;
        if (g_hWinUsb && g_interruptInPipe != 0xFF) WinUsb_AbortPipe(g_hWinUsb, g_interruptInPipe);
    } else if (cmd == 2) {
        // Open dashboard in default browser
        char url[64];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", g_httpPort);
        ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    } else if (cmd == 3) {
        // About box
        wchar_t about[256];
        _snwprintf(about, sizeof(about) / sizeof(about[0]),
                   L"KeySidekick %hs\nGPL-3.0\nDedicated keyboard controller \x2014 see README/docs for details.",
                   APP_VERSION);
        MessageBoxW(h, about, L"About KeySidekick", MB_OK | MB_ICONINFORMATION);
    } else if (cmd >= 1000) {
        int idx = cmd - 1000;
        EnterCriticalSection(&g_csProfile);
        auto it = g_profiles.begin();
        std::advance(it, idx);
        std::string newName = (it != g_profiles.end()) ? it->first : std::string();
        LeaveCriticalSection(&g_csProfile);
        if (!newName.empty()) SwitchProfileByName(newName);  // does BumpRevision + OnProfileSwitched + UpdateTray
    }
}

static void UpdateTray() {
    if (!g_trayEnabled || !g_hMsgWindow) return;
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hMsgWindow;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // Mode-specific tray icon (loaded from embedded resources)
    static HICON g_iconNormal = 0;
    static HICON g_iconTargeted = 0;
    static HICON g_iconDisconnected = 0;
    if (!g_iconNormal) {
        g_iconNormal     = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(101));
        g_iconTargeted   = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(102));
        g_iconDisconnected = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(103));
    }
    // Get active profile info FIRST (needed for both icon and tooltip)
    Profile activeProf;
    bool hasProf = ActiveProfileCopy(activeProf);
    HICON trayIcon = g_iconDisconnected; // default = disconnected
    if (g_hWinUsb) { // device connected
        trayIcon = g_iconNormal;
    }
    if (hasProf && activeProf.mode == MODE_TARGETED) {
        trayIcon = g_iconTargeted;
    }
    if (!trayIcon) trayIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    g_nid.hIcon = trayIcon;
    // Phase 6: Tooltip = "KeySidekick — <profile> — <mode> — <device>"
    std::string modeStr = "unknown";
    if (hasProf) modeStr = (activeProf.mode == MODE_BASIC) ? "typing" : "app control";
    std::string devStr = g_hWinUsb ? "connected" : "disconnected";
    // Read active profile name under lock for consistency
    std::string activeName;
    EnterCriticalSection(&g_csProfile); activeName = g_activeProfile; LeaveCriticalSection(&g_csProfile);
    std::string tip = std::string("KeySidekick — ") + activeName + " — " + modeStr + " — " + devStr;
    MultiByteToWideChar(CP_ACP, 0, tip.c_str(), -1, g_nid.szTip, sizeof(g_nid.szTip)/sizeof(wchar_t));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void RemoveTray() {
    if (!g_hMsgWindow) return;
    NOTIFYICONDATAW rm = {0};
    rm.cbSize = sizeof(rm); rm.hWnd = g_hMsgWindow; rm.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &rm);
}

// SwitchProfileByName — общий переключатель (для HTTP/tray)
static void SwitchProfileByName(const std::string& name) {
    EnterCriticalSection(&g_csProfile);
    if (g_profiles.find(name) != g_profiles.end()) {
        std::string oldName = g_activeProfile;
        g_activeProfile = name;
        LeaveCriticalSection(&g_csProfile);
        Log("Switch → %s", name.c_str());
        OnProfileSwitched(oldName, name);
        UpdateTray();
        BumpRevision();   // Phase 4: notify SSE clients
    } else {
        LeaveCriticalSection(&g_csProfile);
        Log("Profile not found: %s", name.c_str());
    }
}

// =====================================================================
//  HTTP API (raw WinSock, отдельный поток) + Dashboard
// =====================================================================
// (Capture state, WM_DASH_OP, DashOp — определены выше, рядом с g_csProfile,
//  т.к. ProcessReport/MsgWndProc используют их раньше.)

// --- JSON helpers (минимальные, для наших форм) ---
static std::string JsonEscape(const std::string& s) {
    std::string out; out.reserve(s.size()+8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            // defense-in-depth: никогда не выпускаем сырые < > & в ответы
            // (защита от HTML-контекстных инъекций; JSON.parse декодирует обратно).
            case '<': out += "\\u003c"; break;
            case '>': out += "\\u003e"; break;
            case '&': out += "\\u0026"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; snprintf(b,sizeof(b),"\\u%04x",(unsigned char)c); out += b; }
                else out += c;
        }
    }
    return out;
}

// Base64 для экспорта/импорта конфига (безопасно для UTF-8 и спецсимволов).
static const char kB64Tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string B64Encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i+1] << 8 | (unsigned char)in[i+2];
        out += kB64Tab[(v >> 18) & 63]; out += kB64Tab[(v >> 12) & 63];
        out += kB64Tab[(v >> 6) & 63];  out += kB64Tab[v & 63];
        i += 3;
    }
    std::size_t rest = in.size() - i;
    if (rest == 1) {
        unsigned v = (unsigned char)in[i] << 16;
        out += kB64Tab[(v >> 18) & 63]; out += kB64Tab[(v >> 12) & 63]; out += "==";
    } else if (rest == 2) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i+1] << 8;
        out += kB64Tab[(v >> 18) & 63]; out += kB64Tab[(v >> 12) & 63];
        out += kB64Tab[(v >> 6) & 63];  out += '=';
    }
    return out;
}
static int B64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static std::string B64Decode(const std::string& in) {
    std::string out;
    int buf = 0, bits = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '=') break;
        int v = B64Val(in[i]);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return out;
}

// Простой extrakt JSON-поля: ищет "key" : "value" или "key" : number.
// Возвращает true если найдено. Для строк value в out без кавычек; для числа в outInt.
static bool JsonGetStr(const std::string& body, const char* key, std::string& out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return false;
    k = body.find(':', k + pat.size());
    if (k == std::string::npos) return false;
    k++;
    while (k < body.size() && (body[k]==' '||body[k]=='\t')) k++;
    if (k >= body.size() || body[k] != '"') return false;
    k++;
    std::string v;
    auto appendUtf8 = [&v](unsigned cp) {
        if (cp < 0x80) v += (char)cp;
        else if (cp < 0x800) { v += (char)(0xC0 | (cp >> 6)); v += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { v += (char)(0xE0 | (cp >> 12)); v += (char)(0x80 | ((cp >> 6) & 0x3F)); v += (char)(0x80 | (cp & 0x3F)); }
        else { v += (char)(0xF0 | (cp >> 18)); v += (char)(0x80 | ((cp >> 12) & 0x3F)); v += (char)(0x80 | ((cp >> 6) & 0x3F)); v += (char)(0x80 | (cp & 0x3F)); }
    };
    auto hexVal = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    };
    while (k < body.size() && body[k] != '"') {
        if (body[k] == '\\' && k+1 < body.size()) {
            char e = body[k+1];
            switch (e) {
                case '"': v += '"'; k += 2; break;
                case '\\': v += '\\'; k += 2; break;
                case '/': v += '/'; k += 2; break;
                case 'b': v += '\b'; k += 2; break;
                case 'f': v += '\f'; k += 2; break;
                case 'n': v += '\n'; k += 2; break;
                case 'r': v += '\r'; k += 2; break;
                case 't': v += '\t'; k += 2; break;
                case 'u': {
                    if (k + 5 < body.size()) {
                        int cp = 0; bool ok = true;
                        for (int i = 0; i < 4; ++i) {
                            int hv = hexVal(body[k+2+i]);
                            if (hv < 0) { ok = false; break; }
                            cp = (cp << 4) | hv;
                        }
                        if (ok) {
                            if (cp >= 0xD800 && cp <= 0xDBFF && k + 11 < body.size() &&
                                body[k+6] == '\\' && body[k+7] == 'u') {
                                int lo = 0; bool ok2 = true;
                                for (int i = 0; i < 4; ++i) {
                                    int hv = hexVal(body[k+8+i]);
                                    if (hv < 0) { ok2 = false; break; }
                                    lo = (lo << 4) | hv;
                                }
                                if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    appendUtf8(cp);
                                    k += 12;
                                    break;
                                }
                            }
                            appendUtf8(cp);
                            k += 6;
                            break;
                        }
                    }
                    v += body[k]; k++;
                    break;
                }
                default: v += body[k]; k++; break;
            }
        } else { v += body[k]; k++; }
    }
    out = v;
    return true;
}
static bool JsonGetInt(const std::string& body, const char* key, int& out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return false;
    k = body.find(':', k + pat.size());
    if (k == std::string::npos) return false;
    k++;
    while (k < body.size() && (body[k]==' '||body[k]=='\t')) k++;
    // boolean-литералы для полей вроде autoStart (true/false)
    if (k < body.size() && (body[k]=='t' || body[k]=='T')) { out = 1; return true; }
    if (k < body.size() && (body[k]=='f' || body[k]=='F')) { out = 0; return true; }
    // допускаем 0x-формат
    if (k+1 < body.size() && body[k]=='0' && (body[k+1]=='x'||body[k+1]=='X')) {
        out = (int)strtol(body.c_str()+k, NULL, 16);
    } else {
        out = atoi(body.c_str()+k);
    }
    return true;
}
// JsonGetBool — разбор boolean-поля ("enabled"): кавычечные "true"/"1"/"yes"
// (lowercase-независимо), bare-литералы true/false и числа (не ноль = true).
// Возвращает false если ключ отсутствует или значение не распарсилось.
static bool JsonGetBool(const std::string& body, const char* key, bool& out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return false;
    k = body.find(':', k + pat.size());
    if (k == std::string::npos) return false;
    k++;
    while (k < body.size() && (body[k]==' '||body[k]=='\t')) k++;
    if (k >= body.size()) return false;
    // Кавычечная строка: "true"/"1"/"yes" (и отрицания).
    if (body[k] == '"') {
        k++;
        std::string v;
        while (k < body.size() && body[k] != '"') {
            if (body[k] == '\\' && k+1 < body.size()) { v += body[k+1]; k += 2; }
            else { v += body[k]; k++; }
        }
        std::string low = ToLower(v.c_str());
        if (low == "true" || low == "1" || low == "yes") { out = true; return true; }
        if (low == "false" || low == "0" || low == "no") { out = false; return true; }
        return false;
    }
    // Bare-литералы true/false.
    if (body[k]=='t' || body[k]=='T') { out = true; return true; }
    if (body[k]=='f' || body[k]=='F') { out = false; return true; }
    // Число: не ноль = true.
    int n = 0;
    if (k+1 < body.size() && body[k]=='0' && (body[k+1]=='x'||body[k+1]=='X')) {
        n = (int)strtol(body.c_str()+k, NULL, 16);
    } else {
        n = atoi(body.c_str()+k);
    }
    out = (n != 0);
    return true;
}
static std::atomic<bool> g_httpRunning{false};
static HANDLE g_httpWorkerSemaphore = NULL;
static std::atomic<int> g_httpWorkersActive{0};
static HANDLE g_httpThreadHandle = NULL;

static void HttpSend(SOCKET s, const char* body, const char* contentType, int status = 200) {
    char hdr[512];
    const char* statusText = (status == 200) ? "OK" : (status == 204) ? "No Content" :
                             (status == 400) ? "Bad Request" :
                             (status == 403) ? "Forbidden" : (status == 404) ? "Not Found" :
                             (status == 405) ? "Method Not Allowed" :
                             (status == 413) ? "Payload Too Large" :
                             (status == 415) ? "Unsupported Media Type" :
                             (status == 431) ? "Request Header Fields Too Large" :
                             (status == 500) ? "Internal Server Error" :
                             (status == 503) ? "Service Unavailable" : "Error";
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        status, statusText, contentType, strlen(body));
    send(s, hdr, n, 0);
    send(s, body, (int)strlen(body), 0);
}
static void HttpSendJson(SOCKET s, const std::string& body, int status = 200) {
    HttpSend(s, body.c_str(), "application/json", status);
}

// Сериализовать один профиль в JSON.
// Добавляет additive multi-app fields (linkedApplications, defaultApplication)
// из canonical DomainModel, если профиль найден.
static std::string ProfileToJson(const Profile& p) {
    std::string j = "{";
    j += "\"name\":\"" + JsonEscape(p.name) + "\"";
    j += ",\"mode\":" + std::string(p.mode == MODE_BASIC ? "\"basic\"" : "\"targeted\"");
    j += ",\"targetClass\":\"" + JsonEscape(p.targetClass) + "\"";
    j += ",\"targetExe\":\"" + JsonEscape(p.targetExe) + "\"";
    j += ",\"targetPath\":\"" + JsonEscape(p.targetPath) + "\"";
    j += ",\"autoStart\":" + std::string(p.autoStart ? "true" : "false");
    j += ",\"layerMod\":\"" + JsonEscape(p.layerModName) + "\"";
    j += ",\"isBuiltin\":" + std::string(p.isBuiltinBasic ? "true" : "false");

    // Additive multi-app fields from DomainModel
    const keysidekick::Profile* dp = 0;
    if (p.isBuiltinBasic) {
        dp = g_domain.findProfile(keysidekick::Profile::normalId());
    } else {
        for (std::size_t i = 0; i < g_domain.profiles.size(); ++i) {
            if (g_domain.profiles[i].name == p.name) { dp = &g_domain.profiles[i]; break; }
        }
    }
    if (dp) {
        j += ",\"linkedApplications\":[";
        for (std::size_t i = 0; i < dp->linkedApplicationIds.size(); ++i) {
            if (i) j += ",";
            j += "\"" + JsonEscape(dp->linkedApplicationIds[i]) + "\"";
        }
        j += "]";
        j += ",\"defaultApplication\":\"" + JsonEscape(dp->defaultApplicationId) + "\"";
    } else {
        j += ",\"linkedApplications\":[]";
        j += ",\"defaultApplication\":\"\"";
    }

    j += ",\"keys\":[";
    for (size_t i = 0; i < p.keys.size(); i++) {
        if (i) j += ",";
        char kb[64];
        snprintf(kb, sizeof(kb), "{\"usage\":%d,\"mod\":%d,\"action\":\"", p.keys[i].usageId, (int)p.keys[i].modMask);
        j += kb;
        j += JsonEscape(p.keys[i].action);
        j += "\",\"description\":\"";
        j += JsonEscape(keysidekick::action_parser::DescribeAction(p.keys[i].action));
        j += "\",\"category\":\"";
        j += keysidekick::action_parser::ClassifyAction(p.keys[i].action);
        j += "\"}";
    }
    j += "]}";
    return j;
}

// Dashboard HTML is generated from web/ and embedded at build time.

// Результат RunDashOp: success + ownership flag.
// callerOwnsOp=false means HTTP caller must NOT delete op (main thread owns it).
struct DashOpResult {
    bool ok;
    bool callerOwnsOp;   // true = caller should delete op; false = abandoned (main deletes)
    std::string error;
};

// Выполнить write-операцию через marshaling в main thread (синхронный ответ).
static DashOpResult RunDashOp(DashOp* op) {
    DashOpResult result = {false, true, ""};
    op->doneEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    op->timedOut.store(false);
    if (!op->doneEvent) { result.error = "CreateEvent failed"; return result; }
    if (!PostMessageW(g_hMsgWindow, WM_DASH_OP, (WPARAM)op, 0)) {
        CloseHandle(op->doneEvent);
        result.error = "PostMessage failed";
        return result;
    }
    DWORD wr = WaitForSingleObject(op->doneEvent, kDashOpTimeoutMs);
    if (wr == WAIT_OBJECT_0) {
        result.ok = op->success;
        result.error = op->error;
        CloseHandle(op->doneEvent);
        return result;
    }
    // Timeout: mark as abandoned. MsgWndProc will delete it after processing.
    // We must NOT delete op here — main thread may still use it.
    op->timedOut.store(true);
    result.callerOwnsOp = false;
    result.error = "timeout";
    return result;
}

// =====================================================================
//  Phase 4: SSE (Server-Sent Events) for live dashboard updates
// =====================================================================
// H2: сокеты клиентов переводятся в non-blocking (FIONBIO) после рукопожатия.
// Push-путь (hot path ProcessReport) шлёт send() без блокировки; при сбое
// (SOCKET_ERROR / short send / WSAEWOULDBLOCK) pusher закрывает сокет и стирает
// запись ПОД g_csRevision. Поток-владелец (SseClientThread) следит за наличием
// своей записи и, если pusher уже стёр её, не закрывает сокет повторно.
static const int kMaxSseClients = 8;
struct SseClient {
    SOCKET socket;
};
static std::vector<SseClient> g_sseClients;
static int g_sseClientCount = 0;

// NotifySseClients — отправить revision update всем подключённым SSE клиентам.
// Вызывается из BumpRevision() при каждом state change.
void NotifySseClients(unsigned long long revision) {
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "event: revision\ndata: {\"revision\":%llu}\n\n", revision);
    if (len <= 0) return;

    // H2: send() на hot path выполняется на non-blocking сокете и возвращается
    // сразу. Любой сбой (SOCKET_ERROR, короткий send, WSAEWOULDBLOCK) = клиент
    // не успевает читать → закрываем сокет и стираем запись ПОД локом.
    EnterCriticalSection(&g_csRevision);
    for (std::size_t i = 0; i < g_sseClients.size(); ) {
        SseClient& c = g_sseClients[i];
        int sr = send(c.socket, msg, len, 0);
        if (sr == SOCKET_ERROR || sr != len) {
            closesocket(c.socket);
            g_sseClients.erase(g_sseClients.begin() + i);
            g_sseClientCount--;
            Log("SSE client dropped (revision send failed)");
        } else {
            ++i;
        }
    }
    LeaveCriticalSection(&g_csRevision);
}

// Live-экран: пуш "event: activity" всем SSE-клиентам (json уже готов).
static void NotifyActivitySse(const std::string& json) {
    std::string msg = "event: activity\ndata: " + json + "\n\n";
    // H2: non-blocking send под локом; сбой → закрыть сокет и стереть запись.
    EnterCriticalSection(&g_csRevision);
    for (std::size_t i = 0; i < g_sseClients.size(); ) {
        SseClient& c = g_sseClients[i];
        int sr = send(c.socket, msg.c_str(), (int)msg.size(), 0);
        if (sr == SOCKET_ERROR || sr != (int)msg.size()) {
            closesocket(c.socket);
            g_sseClients.erase(g_sseClients.begin() + i);
            g_sseClientCount--;
            Log("SSE client dropped (activity send failed)");
        } else {
            ++i;
        }
    }
    LeaveCriticalSection(&g_csRevision);
}

// Параметры для SseClientThread (heap — поток сам освобождает).
struct SseClientStart {
    SOCKET socket;
    std::string origin;
};

static DWORD WINAPI SseClientThread(LPVOID parameter) {
    SseClientStart* start = (SseClientStart*)parameter;
    SOCKET cli = start->socket;
    std::string origin = start->origin;
    delete start;

    // H2: cap — при переполнении отклоняем нового клиента.
    EnterCriticalSection(&g_csRevision);
    bool full = (g_sseClientCount >= kMaxSseClients);
    LeaveCriticalSection(&g_csRevision);
    if (full) {
        HttpSend(cli, "too many SSE clients", "text/plain", 503);
        closesocket(cli);
        return 0;
    }

    // H2: bounded send на push-сокете (HTTP-поток ставит 10s для
    // request/response, но для hot path это слишком долго).
    DWORD sndTimeoutMs = 200;
    setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeoutMs, sizeof(sndTimeoutMs));

    // M6: Origin эхо-заголовок выводим только если Origin разрешён
    // (keysidekick::IsAllowedOrigin) — никогда не шлём "*".
    std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n";
    if (!origin.empty()) {
        headers += "Access-Control-Allow-Origin: " + origin + "\r\n";
    }
    headers += "\r\n";
    send(cli, headers.c_str(), (int)headers.size(), 0);

    unsigned long long initRev;
    EnterCriticalSection(&g_csRevision); initRev = g_stateRevision; LeaveCriticalSection(&g_csRevision);
    char initMsg[128];
    int initLen = snprintf(initMsg, sizeof(initMsg),
        "event: revision\ndata: {\"revision\":%llu}\n\n", initRev);
    send(cli, initMsg, initLen, 0);

    // H2: после рукопожатия переводим сокет в non-blocking, затем регистрируем.
    u_long nb = 1;
    ioctlsocket(cli, FIONBIO, &nb);

    EnterCriticalSection(&g_csRevision);
    g_sseClients.push_back(SseClient());
    g_sseClients.back().socket = cli;
    g_sseClientCount++;
    LeaveCriticalSection(&g_csRevision);
    Log("SSE client connected (%d total)", g_sseClientCount);

    DWORD lastPing = GetTickCount();
    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(cli, &fds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int sel = select(0, &fds, NULL, NULL, &tv);
        if (sel == SOCKET_ERROR) break;
        if (sel == 0 && GetTickCount() - lastPing >= 15000) {
            // keepalive-фрейм: не даём промежуточным прокси/таймаутам убить стрим.
            if (send(cli, ": ping\n\n", 8, 0) == SOCKET_ERROR) break;
            lastPing = GetTickCount();
        }
        if (sel > 0) {
            char buf[64];
            int r = recv(cli, buf, sizeof(buf), 0);
            if (r <= 0) break;
        }
        // H2: если pusher уже закрыл сокет и стёр нашу запись (send failure) —
        // выходим без повторного closesocket.
        EnterCriticalSection(&g_csRevision);
        bool registered = false;
        for (std::size_t i = 0; i < g_sseClients.size(); ++i) {
            if (g_sseClients[i].socket == cli) { registered = true; break; }
        }
        LeaveCriticalSection(&g_csRevision);
        if (!registered) break;
    }

    // H2: закрываем сокет только если запись всё ещё наша. Если pusher уже
    // стёр её (send failure) — сокет уже закрыт, повторно не закрываем.
    bool owned = false;
    EnterCriticalSection(&g_csRevision);
    for (std::size_t i = 0; i < g_sseClients.size(); ++i) {
        if (g_sseClients[i].socket == cli) {
            g_sseClients.erase(g_sseClients.begin() + i);
            g_sseClientCount--;
            owned = true;
            break;
        }
    }
    LeaveCriticalSection(&g_csRevision);
    if (owned) {
        Log("SSE client disconnected (%d total)", g_sseClientCount);
        closesocket(cli);
    }
    return 0;
}

static void HandleHttpConnection(SOCKET cli);
static DWORD WINAPI HttpWorkerThread(LPVOID parameter);

static DWORD WINAPI HttpThread(LPVOID) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { Log("WSAStartup failed"); return 1; }
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { Log("socket failed"); return 1; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)g_httpPort);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Log("HTTP bind failed err=%d", WSAGetLastError()); closesocket(srv); return 1;
    }
    if (listen(srv, 5) == SOCKET_ERROR) { Log("HTTP listen failed"); closesocket(srv); return 1; }
    Log("HTTP listening on 127.0.0.1:%d", g_httpPort);
    while (g_httpRunning) {

        // H3: select-timeout accept — быстрый выход при остановке (join на exit).
        fd_set afds;
        FD_ZERO(&afds);
        FD_SET(srv, &afds);
        struct timeval atv;
        atv.tv_sec = 0;
        atv.tv_usec = 500000;
        int asel = select(0, &afds, NULL, NULL, &atv);
        if (asel == SOCKET_ERROR) break;
        if (asel == 0) continue;
        SOCKET cli = accept(srv, NULL, NULL);
        if (cli == INVALID_SOCKET) break;
        // H3: ограниченный пул воркеров (8): slowloris больше не блокирует
        // дашборд; переполнение пула — мгновенный отказ соединения.
        if (WaitForSingleObject(g_httpWorkerSemaphore, 0) != WAIT_OBJECT_0) {
            closesocket(cli);
            continue;
        }
        g_httpWorkersActive.fetch_add(1);
        HANDLE worker = CreateThread(NULL, 0, HttpWorkerThread,
                                     (LPVOID)(ULONG_PTR)cli, 0, NULL);
        if (worker == NULL) {
            g_httpWorkersActive.fetch_sub(1);
            ReleaseSemaphore(g_httpWorkerSemaphore, 1, NULL);
            closesocket(cli);
            continue;
        }
        CloseHandle(worker);
    }
    closesocket(srv);
    WSACleanup();
    return 0;
}

// H3: обработка одного HTTP-соединения на воркере пула. Мутации сериализуются
// через RunDashOp (main thread) и существующие критические секции; каждый
// воркер работает со своим сокетом и локальными данными.
static bool IsPostOnlyPath(const std::string& p) {
    static const char* kPostOnly[] = {
        "/api/profile/activate", "/api/profile", "/api/key", "/api/key/delete",
        "/api/key/update", "/api/key/move", "/api/key/duplicate", "/api/reload",
        "/api/capture/start",
        "/api/v1/profile/create", "/api/v1/profile/delete", "/api/v1/profile/rename",
        "/api/v1/profile/duplicate", "/api/v1/profile/link-app", "/api/v1/profile/unlink-app",
        "/api/v1/profile/set-default-app", "/api/v1/applications/create",
        "/api/v1/applications/test-resolve", "/api/v1/config/import",
        "/api/v1/preset/apply", "/api/v1/devices/activate", "/api/v1/action/fire",
        "/api/v1/windows/foreground/pick", "/api/v1/devices/capture",
        "/api/v1/driver/swap", "/api/v1/driver/restore"
    };
    for (const char* s : kPostOnly) if (p == s) return true;
    return false;
}

static void HandleHttpConnection(SOCKET cli) {
    // H1/H2: per-connection timeouts. SO_RCVTIMEO (5s) keeps one idle or
    // slow local client from stalling the only HTTP thread forever;
    // SO_SNDTIMEO (10s) bounds the response send (the embedded dashboard
    // HTML is ~600 KB — a client that connects and never reads must not
    // block the thread indefinitely either). SSE resets SO_SNDTIMEO to
    // ~200ms in its own thread (H2).
    DWORD rcvTimeoutMs = 5000;
    setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTimeoutMs, sizeof(rcvTimeoutMs));
    DWORD sndTimeoutMs = 10000;
    setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeoutMs, sizeof(sndTimeoutMs));

    // Накопить запрос: читать до \r\n\r\n (конец заголовков), потом body по Content-Length.
    // H3 design decision: пул воркеров (8). Каждое соединение обрабатывается
    // отдельным потоком; slowloris больше не блокирует дашборд. Каждый recv bounded
    // by SO_RCVTIMEO and the WHOLE request by a 15s deadline, so a slow
    // drip cannot monopolize the thread. Requests larger than 64 KiB are
    // rejected (431 for headers / 413 for body) BEFORE any parsing, and a
    // truncated read (timed-out OR clean-close mid-body) is never processed —
    // partial JSON with defaulted fields is a real bug class (see M5).
    std::string req;
    char buf[2048];
    size_t headerEnd = std::string::npos;
    int contentLength = 0;
    std::string hostHeader;
    std::string csrfHeader;
    std::string originHeader;
    std::string contentTypeHeader;
    std::string secFetchSiteHeader;
    const size_t kMaxRequestBytes = 64 * 1024;
    bool requestTooLarge = false;
    bool truncatedRead = false;
    const ULONGLONG requestDeadline = GetTickCount64() + 15000;
    while (g_httpRunning) {
        if (GetTickCount64() > requestDeadline) { truncatedRead = true; break; }
        int r = recv(cli, buf, sizeof(buf), 0);
        if (r == SOCKET_ERROR) {
            // SO_RCVTIMEO expired mid-request (or transport error) —
            // partial input is unsafe to parse.
            truncatedRead = true;
            break;
        }
        if (r == 0) {
            // Чистое закрытие (FIN): если заголовки получены, но body
            // недокачан по Content-Length — это тоже обрыв (M5).
            if (headerEnd != std::string::npos &&
                (int)(req.size() - (headerEnd + 4)) < contentLength) {
                truncatedRead = true;
            }
            break;
        }
        req.append(buf, r);
        if (req.size() > kMaxRequestBytes) { requestTooLarge = true; break; }
        if (headerEnd == std::string::npos) {
            headerEnd = req.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                // парсить заголовки (Host, Content-Length, token, Origin, Content-Type, Sec-Fetch-Site)
                std::string headers = req.substr(0, headerEnd);
                size_t pos = 0;
                while (pos < headers.size()) {
                    size_t eol = headers.find("\r\n", pos);
                    if (eol == std::string::npos) eol = headers.size();
                    std::string line = headers.substr(pos, eol - pos);
                    if (_strnicmp(line.c_str(), "Host:", 5) == 0) {
                        hostHeader = line.substr(5);
                        while (!hostHeader.empty() && (hostHeader.front()==' '||hostHeader.front()=='\t')) hostHeader.erase(0,1);
                    } else if (_strnicmp(line.c_str(), "Content-Length:", 15) == 0) {
                        contentLength = atoi(line.c_str()+15);
                    } else if (_strnicmp(line.c_str(), "X-KeySidekick-Token:", 20) == 0) {
                        csrfHeader = line.substr(20);
                        while (!csrfHeader.empty() && (csrfHeader.front()==' '||csrfHeader.front()=='\t')) csrfHeader.erase(0,1);
                    } else if (_strnicmp(line.c_str(), "Origin:", 7) == 0) {
                        originHeader = line.substr(7);
                        while (!originHeader.empty() && (originHeader.front()==' '||originHeader.front()=='\t')) originHeader.erase(0,1);
                    } else if (_strnicmp(line.c_str(), "Content-Type:", 13) == 0) {
                        contentTypeHeader = line.substr(13);
                        while (!contentTypeHeader.empty() && (contentTypeHeader.front()==' '||contentTypeHeader.front()=='\t')) contentTypeHeader.erase(0,1);
                    } else if (_strnicmp(line.c_str(), "Sec-Fetch-Site:", 15) == 0) {
                        secFetchSiteHeader = line.substr(15);
                        while (!secFetchSiteHeader.empty() && (secFetchSiteHeader.front()==' '||secFetchSiteHeader.front()=='\t')) secFetchSiteHeader.erase(0,1);
                    }
                    pos = eol + 2;
                }
            }
        }
        if (headerEnd != std::string::npos) {
            size_t bodyHave = req.size() - (headerEnd + 4);
            if ((int)bodyHave >= contentLength) break;   // всё тело прочитано
        }
    }
    if (req.empty() || truncatedRead) { closesocket(cli); return; }
    if (requestTooLarge) {
        // 431 если переполнились заголовки, 413 если тело.
        HttpSend(cli, "request too large", "text/plain",
                 headerEnd == std::string::npos ? 431 : 413);
        closesocket(cli); return;
    }

    // Парсим request line
    std::string method, path, query;
    size_t sp1 = req.find(' ');
    if (sp1 == std::string::npos) { HttpSend(cli, "bad request", "text/plain", 400); closesocket(cli); return; }
    method = req.substr(0, sp1);
    size_t sp2 = req.find(' ', sp1+1);
    std::string url = (sp2 != std::string::npos) ? req.substr(sp1+1, sp2-sp1-1) : req.substr(sp1+1);
    size_t qp = url.find('?');
    if (qp != std::string::npos) { path = url.substr(0, qp); query = url.substr(qp+1); }
    else path = url;

    std::string body = (headerEnd != std::string::npos) ? req.substr(headerEnd+4) : "";

    // C1: роутим КАЖДЫЙ запрос через http_security pipeline.
    // ValidateDashboardRequest (http_security.cpp, покрыт unit-тестами)
    // enforce: loopback Host/Origin, Sec-Fetch-Site, header/body caps,
    // mutating-GET → 405, и для mutating методов — CSRF token (403) +
    // Content-Type: application/json (415). Это заменяет старый ad-hoc
    // "POST ⇒ token" gate, который GET /switch и GET /profile обходили.
    {
        keysidekick::RequestMetadata meta;
        meta.method = method;
        meta.path = path;
        meta.host = hostHeader;
        // Голый Host (без порта) legacy-сервер терпел; нормализуем, чтобы
        // строгий loopback-check в pipeline его тоже принял.
        if (meta.host.find(':') == std::string::npos)
            meta.host += ":" + std::to_string(g_httpPort);
        meta.origin = originHeader;
        meta.sec_fetch_site = secFetchSiteHeader;
        meta.content_type = contentTypeHeader;
        meta.token_header_name = "X-KeySidekick-Token";
        meta.token_header = csrfHeader;
        meta.header_bytes = (headerEnd != std::string::npos) ? headerEnd + 4 : req.size();
        meta.body_bytes = body.size();

        keysidekick::SecurityPolicy policy;
        policy.port = (unsigned short)g_httpPort;
        policy.allow_ipv6_loopback = false;
        policy.token_header_name = "X-KeySidekick-Token";
        policy.token = g_csrfToken;
        policy.max_header_bytes = keysidekick::kDefaultMaxHeaderBytes;
        policy.max_body_bytes = keysidekick::kDefaultMaxBodyBytes;

        keysidekick::SecurityDecision decision =
            keysidekick::ValidateDashboardRequest(meta, policy);
        if (!decision.allowed) {
            HttpSendJson(cli,
                std::string("{\"error\":\"") + JsonEscape(decision.message) +
                "\",\"code\":\"" + decision.code + "\"}",
                decision.http_status);
            closesocket(cli); return;
        }
    }

    // C1: legacy mutating GETs (/switch, /profile) отклоняются намертво —
    // канонический API переключения: POST /api/profile/activate.
    if ((method == "GET" || method == "HEAD") &&
        (path == "/switch" || path == "/profile" || IsPostOnlyPath(path))) {
        HttpSendJson(cli,
            "{\"error\":\"state changes require POST\",\"code\":\"mutating_get_forbidden\"}", 405);
        closesocket(cli); return;
    }

    // --- Routing ---
    // favicon.ico — return empty 204 to avoid 404 noise in browser console
    if (method == "GET" && path == "/favicon.ico") {
        HttpSend(cli, "", "image/x-icon", 204);
    }
    else if (method == "GET" && path == "/") {
        // Phase 4: inject CSRF token into dashboard HTML.
        // kDashboardHtml содержит маркер /*{{CSRF_TOKEN}}*/ который заменяется на реальный token.
        std::string html(kDashboardHtml, kDashboardHtmlSize);
        const std::string marker = "/*{{CSRF_TOKEN}}*/\"\"";
        size_t pos = html.find(marker);
        if (pos != std::string::npos) {
            html.replace(pos, marker.size(), "\"" + g_csrfToken + "\"");
        }
        // Also expose state revision for SSE bootstrap
        const std::string revMarker = "/*{{STATE_REVISION}}*/\"0\"";
        size_t rpos = html.find(revMarker);
        if (rpos != std::string::npos) {
            unsigned long long curRev;
            EnterCriticalSection(&g_csRevision); curRev = g_stateRevision; LeaveCriticalSection(&g_csRevision);
            char revBuf[32]; snprintf(revBuf, sizeof(revBuf), "%llu", curRev);
            html.replace(rpos, revMarker.size(), std::string("\"") + revBuf + "\"");
        }
        HttpSend(cli, html.c_str(), "text/html; charset=utf-8");
    }
    // GET /api/status
    else if (method == "GET" && path == "/api/status") {
        bool dev = DevInfoConnected();
        std::string active;
        EnterCriticalSection(&g_csProfile); active = g_activeProfile; LeaveCriticalSection(&g_csProfile);
        bool pcDet = false;
        EnterCriticalSection(&g_csProfile); pcDet = g_portChangeDetected; LeaveCriticalSection(&g_csProfile);
        std::string j = "{\"device\":\"" + std::string(dev ? "connected" : "disconnected")
            + "\",\"active\":\"" + JsonEscape(active) + "\",\"http\":"
            + (g_httpEnabled ? "true" : "false") + ",\"tray\":"
            + (g_trayEnabled ? "true" : "false")
            + ",\"portChangeDetected\":" + (pcDet ? "true" : "false") + "}";
        HttpSendJson(cli, j);
    }
    // GET /api/v1/state — unified snapshot with revision (Phase 4)
    // SSE clients fetch this after receiving event:revision to get full state
    else if (method == "GET" && path == "/api/v1/state") {
        bool dev = DevInfoConnected();
        std::string active;
        unsigned long long rev;
        size_t profileCount, appCount;
        EnterCriticalSection(&g_csProfile);
        active = g_activeProfile;
        profileCount = g_profiles.size();
        appCount = g_domain.applications.size();
        LeaveCriticalSection(&g_csProfile);
        EnterCriticalSection(&g_csRevision); rev = g_stateRevision; LeaveCriticalSection(&g_csRevision);
        bool pcDet = false; std::string pcVp;
        EnterCriticalSection(&g_csProfile);
        pcDet = g_portChangeDetected;
        pcVp = g_portChangeVidPid;
        LeaveCriticalSection(&g_csProfile);
        std::string j = "{\"version\":\"" + std::string(APP_VERSION) + "\",\"revision\":"
            + std::to_string(rev) + ",\"device\":\"" + std::string(dev ? "connected" : "disconnected")
            + "\",\"active\":\"" + JsonEscape(active) + "\",\"http\":"
            + (g_httpEnabled ? "true" : "false") + ",\"tray\":"
            + (g_trayEnabled ? "true" : "false") + ",\"profileCount\":"
            + std::to_string(profileCount) + ",\"appCount\":" + std::to_string(appCount)
            + ",\"portChangeDetected\":" + (pcDet ? "true" : "false")
            + ",\"portChangeVidPid\":\"" + JsonEscape(pcVp) + "\"}";
        HttpSendJson(cli, j);
    }
    // GET /api/profiles (полный JSON всех профилей)
    else if (method == "GET" && path == "/api/profiles") {
        std::string j = "[";
        EnterCriticalSection(&g_csProfile);
        bool first = true;
        for (auto& kv : g_profiles) {
            if (!first) j += ",";
            j += ProfileToJson(kv.second);
            first = false;
        }
        std::string active = g_activeProfile;
        LeaveCriticalSection(&g_csProfile);
        j += "]";
        // обернуть с active для удобства фронта
        std::string wrap = "{\"active\":\"" + JsonEscape(active) + "\",\"profiles\":" + j + "}";
        HttpSendJson(cli, wrap);
    }
    // GET /api/profile?name=X
    else if (method == "GET" && path == "/api/profile") {
        std::string name;
        if (const char* p = strstr(query.c_str(), "name=")) {
            name = p+5; size_t a = name.find('&'); if (a != std::string::npos) name = name.substr(0,a);
        }
        if (name.empty()) { HttpSendJson(cli, "{\"error\":\"missing name\"}", 400); }
        else {
            EnterCriticalSection(&g_csProfile);
            auto it = g_profiles.find(name);
            std::string j = (it != g_profiles.end()) ? ProfileToJson(it->second) : "{\"error\":\"not found\"}";
            LeaveCriticalSection(&g_csProfile);
            HttpSendJson(cli, j, (it != g_profiles.end()) ? 200 : 404);
        }
    }
    // POST /api/profile/activate  body {"name":"X"}
    // (C1: legacy GET /switch и GET /profile удалены — они мутировали state
    //  без token/origin и теперь возвращают 405 выше по коду.)
    else if (method == "POST" && path == "/api/profile/activate") {
        std::string name;
        JsonGetStr(body, "name", name);
        if (name.empty()) {
            // вернуть текущий (backwards-compat)
            EnterCriticalSection(&g_csProfile); std::string cur = g_activeProfile; LeaveCriticalSection(&g_csProfile);
            HttpSend(cli, cur.c_str(), "text/plain");
        } else {
            char* dup = _strdup(name.c_str());
            PostMessageW(g_hMsgWindow, WM_HTTP_SWITCH, (WPARAM)dup, 0);
            HttpSendJson(cli, "{\"ok\":true,\"switching\":\"" + JsonEscape(name) + "\"}");
        }
    }
    // POST /api/key  body {profile, usage, mod, action} → ADD_KEY
    else if (method == "POST" && path == "/api/key") {
        std::string profile, action; int usage=0, mod=0;
        JsonGetStr(body, "profile", profile);
        JsonGetStr(body, "action", action);
        JsonGetInt(body, "usage", usage);
        JsonGetInt(body, "mod", mod);
        if (profile.empty() || usage <= 0) { HttpSendJson(cli, "{\"error\":\"missing profile/usage\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_ADD_KEY;
            snprintf(op->profile, sizeof(op->profile), "%s", profile.c_str());
            op->usage = usage; op->mod = (unsigned char)mod;
            snprintf(op->action, sizeof(op->action), "%s", action.c_str());
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;            }
    }
    // POST /api/key/delete  body {profile, usage, mod} → REMOVE_KEY
    else if (method == "POST" && path == "/api/key/delete") {
        std::string profile; int usage=0, mod=0;
        JsonGetStr(body, "profile", profile);
        JsonGetInt(body, "usage", usage);
        JsonGetInt(body, "mod", mod);
        if (profile.empty() || usage <= 0) { HttpSendJson(cli, "{\"error\":\"missing profile/usage\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_REMOVE_KEY;
            snprintf(op->profile, sizeof(op->profile), "%s", profile.c_str());
            op->usage = usage; op->mod = (unsigned char)mod;
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;            }
    }
    // ---- Mapping management (in-place edit, reorder) ----
    // POST /api/key/update — body {profile, usage, mod, newAction} → DASH_UPDATE_KEY
    else if (method == "POST" && path == "/api/key/update") {
        std::string profile, newAction; int usage=0, mod=0;
        JsonGetStr(body, "profile", profile);
        JsonGetStr(body, "newAction", newAction);
        JsonGetInt(body, "usage", usage);
        JsonGetInt(body, "mod", mod);
        if (profile.empty() || usage <= 0 || newAction.empty()) { HttpSendJson(cli, "{\"error\":\"missing profile/usage/newAction\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_UPDATE_KEY;
            snprintf(op->profile, sizeof(op->profile), "%s", profile.c_str());
            op->usage = usage; op->mod = (unsigned char)mod;
            snprintf(op->action, sizeof(op->action), "%s", newAction.c_str());
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;
        }
    }
    // POST /api/key/move — body {profile, usage, mod, direction} → DASH_MOVE_KEY (up/down swap)
    else if (method == "POST" && path == "/api/key/move") {
        std::string profile, dir; int usage=0, mod=0;
        JsonGetStr(body, "profile", profile);
        JsonGetStr(body, "direction", dir);
        JsonGetInt(body, "usage", usage);
        JsonGetInt(body, "mod", mod);
        if (profile.empty() || usage <= 0) { HttpSendJson(cli, "{\"error\":\"missing profile/usage\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_MOVE_KEY;
            snprintf(op->profile, sizeof(op->profile), "%s", profile.c_str());
            op->usage = usage; op->mod = (unsigned char)mod;
            snprintf(op->strArg1, sizeof(op->strArg1), "%s", dir.c_str());  // "up" or "down"
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;
        }
    }
    // POST /api/key/duplicate — body {profile, usage, newUsage} → DASH_DUPLICATE_KEY
    else if (method == "POST" && path == "/api/key/duplicate") {
        std::string profile; int usage=0, mod=0, newUsage=0;
        JsonGetStr(body, "profile", profile);
        JsonGetInt(body, "usage", usage);
        JsonGetInt(body, "mod", mod);
        JsonGetInt(body, "newUsage", newUsage);
        if (profile.empty() || usage <= 0 || newUsage <= 0) { HttpSendJson(cli, "{\"error\":\"missing profile/usage/newUsage\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_DUPLICATE_KEY;
            snprintf(op->profile, sizeof(op->profile), "%s", profile.c_str());
            op->usage = usage;
            op->mod = (unsigned char)mod;
            snprintf(op->strArg1, sizeof(op->strArg1), "%d", newUsage);
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;
        }
    }
    // POST /api/profile  body {name, mode, targetClass, targetExe, targetPath, autoStart, layerMod} → SET_PROFILE
    else if (method == "POST" && path == "/api/profile") {
        std::string name, tCls, tExe, tPath, modeStr, layerMod; int autoStart=0;
        JsonGetStr(body, "name", name);
        JsonGetStr(body, "mode", modeStr);
        JsonGetStr(body, "targetClass", tCls);
        JsonGetStr(body, "targetExe", tExe);
        JsonGetStr(body, "targetPath", tPath);
        JsonGetStr(body, "layerMod", layerMod);
        JsonGetInt(body, "autoStart", autoStart);
        if (name.empty()) { HttpSendJson(cli, "{\"error\":\"missing name\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_SET_PROFILE;
            snprintf(op->profile, sizeof(op->profile), "%s", name.c_str());
            op->mode = (modeStr == "basic") ? 0 : 1;
            snprintf(op->targetClass, sizeof(op->targetClass), "%s", tCls.c_str());
            snprintf(op->targetExe, sizeof(op->targetExe), "%s", tExe.c_str());
            snprintf(op->targetPath, sizeof(op->targetPath), "%s", tPath.c_str());
            snprintf(op->layerMod, sizeof(op->layerMod), "%s", layerMod.c_str());
            op->autoStart = autoStart ? 1 : 0;
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;            }
    }
    // POST /api/reload → ReloadConfig
    else if (method == "POST" && path == "/api/reload") {
        DashOp* op = new DashOp();
        op->op = DASH_RELOAD;
        DashOpResult rr = RunDashOp(op);
        if (rr.callerOwnsOp) delete op;
        HttpSendJson(cli, rr.ok ? "{\"ok\":true}" : "{\"error\":\"reload failed\"}", rr.ok ? 200 : 500);
    }
    // ---- Phase 2: multi-app CRUD endpoints (additive, no changes to existing) ----
    // GET /api/v1/applications — список ApplicationTargets
    else if (method == "GET" && path == "/api/v1/applications") {
        std::string j = "{\"applications\":[";
        EnterCriticalSection(&g_csProfile);
        bool first = true;
        for (std::size_t i = 0; i < g_domain.applications.size(); ++i) {
            const keysidekick::ApplicationTarget& app = g_domain.applications[i];
            if (!first) j += ",";
            first = false;
            j += "{\"id\":\"" + JsonEscape(app.id()) + "\"";
            j += ",\"name\":\"" + JsonEscape(app.name) + "\"";
            j += ",\"windowClass\":\"" + JsonEscape(app.windowClass) + "\"";
            j += ",\"exePath\":\"" + JsonEscape(app.exePath) + "\"";
            j += ",\"processName\":\"" + JsonEscape(app.processName) + "\"";
            j += "}";
        }
        LeaveCriticalSection(&g_csProfile);
        j += "]}";
        HttpSendJson(cli, j);
    }
    // POST /api/v1/profile/create — {id, name, mode}
    else if (method == "POST" && path == "/api/v1/profile/create") {
        std::string id, name, modeStr;
        JsonGetStr(body, "id", id);
        JsonGetStr(body, "name", name);
        JsonGetStr(body, "mode", modeStr);
        if (id.empty()) { HttpSendJson(cli, "{\"error\":\"missing id\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_CREATE_PROFILE;
            snprintf(op->profile, sizeof(op->profile), "%s", id.c_str());
            snprintf(op->strArg1, sizeof(op->strArg1), "%s", name.c_str());
            op->mode = (modeStr == "targeted") ? 1 : 0;
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;            }
    }
    // POST /api/v1/profile/delete — {id}
    else if (method == "POST" && path == "/api/v1/profile/delete") {
        std::string id;
        JsonGetStr(body, "id", id);
        if (id.empty()) { HttpSendJson(cli, "{\"error\":\"missing id\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_DELETE_PROFILE;
            snprintf(op->profile, sizeof(op->profile), "%s", id.c_str());
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;            }
    }
    // POST /api/v1/profile/rename — {id, newName}
    else if (method == "POST" && path == "/api/v1/profile/rename") {
        std::string id, newName;
        JsonGetStr(body, "id", id);
        JsonGetStr(body, "newName", newName);
        if (id.empty() || newName.empty()) { HttpSendJson(cli, "{\"error\":\"missing id/newName\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_RENAME_PROFILE;
            snprintf(op->profile, sizeof(op->profile), "%s", id.c_str());
            snprintf(op->strArg1, sizeof(op->strArg1), "%s", newName.c_str());
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;            }
    }
    // POST /api/v1/profile/duplicate — {sourceId, newId, newName}
    else if (method == "POST" && path == "/api/v1/profile/duplicate") {
        std::string sourceId, newId, newName;
        JsonGetStr(body, "sourceId", sourceId);
        JsonGetStr(body, "newId", newId);
        JsonGetStr(body, "newName", newName);
        if (sourceId.empty() || newId.empty()) { HttpSendJson(cli, "{\"error\":\"missing sourceId/newId\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_DUPLICATE_PROFILE;
            snprintf(op->profile, sizeof(op->profile), "%s", sourceId.c_str());
            snprintf(op->strArg1, sizeof(op->strArg1), "%s", newId.c_str());
            snprintf(op->strArg2, sizeof(op->strArg2), "%s", newName.c_str());
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;            }
    }
    // POST /api/v1/profile/link-app — {profileId, appId}
    else if (method == "POST" && path == "/api/v1/profile/link-app") {
        std::string profileId, appId;
        JsonGetStr(body, "profileId", profileId);
        JsonGetStr(body, "appId", appId);
        if (profileId.empty() || appId.empty()) { HttpSendJson(cli, "{\"error\":\"missing profileId/appId\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_LINK_APP;
            snprintf(op->profile, sizeof(op->profile), "%s", profileId.c_str());
            snprintf(op->strArg1, sizeof(op->strArg1), "%s", appId.c_str());
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;            }
    }
    // POST /api/v1/profile/unlink-app — {profileId, appId}
    else if (method == "POST" && path == "/api/v1/profile/unlink-app") {
        std::string profileId, appId;
        JsonGetStr(body, "profileId", profileId);
        JsonGetStr(body, "appId", appId);
        if (profileId.empty() || appId.empty()) { HttpSendJson(cli, "{\"error\":\"missing profileId/appId\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_UNLINK_APP;
            snprintf(op->profile, sizeof(op->profile), "%s", profileId.c_str());
            snprintf(op->strArg1, sizeof(op->strArg1), "%s", appId.c_str());
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;            }
    }
    // POST /api/v1/profile/set-default-app — {profileId, appId}
    else if (method == "POST" && path == "/api/v1/profile/set-default-app") {
        std::string profileId, appId;
        JsonGetStr(body, "profileId", profileId);
        JsonGetStr(body, "appId", appId);
        if (profileId.empty()) { HttpSendJson(cli, "{\"error\":\"missing profileId\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_SET_DEFAULT_APP;
            snprintf(op->profile, sizeof(op->profile), "%s", profileId.c_str());
            snprintf(op->strArg1, sizeof(op->strArg1), "%s", appId.c_str());
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}" : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if(rr.callerOwnsOp) delete op;            }
    }
    // ---- Phase 3: window discovery + application resolver ----
    // GET /api/v1/windows — список запущенных top-level окон для visual picker
    else if (method == "GET" && path == "/api/v1/windows") {
        keysidekick::windows_targets::WindowFilterPolicy policy;
        policy.requireVisible = true;
        policy.excludeEmptyTitles = true;
        policy.excludeToolWindows = true;
        policy.excludeShellWindows = true;
        std::vector<keysidekick::windows_targets::WindowCandidate> windows =
            keysidekick::windows_targets::EnumerateWindows(policy);

        std::string j = "{\"windows\":[";
        bool first = true;
        for (std::size_t i = 0; i < windows.size(); ++i) {
            const auto& w = windows[i];
            // Convert wstrings to UTF-8 for JSON
            auto toUtf8 = [](const std::wstring& ws) -> std::string {
                if (ws.empty()) return std::string();
                int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
                std::string s(len, 0);
                WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], len, NULL, NULL);
                return s;
            };
            if (!first) j += ",";
            first = false;
            char header[128];
            snprintf(header, sizeof(header), "{\"hwnd\":%lu,\"pid\":%lu,\"title\":\"",
                (unsigned long)w.handle, (unsigned long)w.processId);
            j += header;
            j += JsonEscape(toUtf8(w.title));
            j += "\",\"windowClass\":\"";
            j += JsonEscape(toUtf8(w.windowClass));
            j += "\",\"processName\":\"";
            j += JsonEscape(toUtf8(w.processName));
            j += "\",\"processPath\":\"";
            j += JsonEscape(toUtf8(w.processPath));
            j += "\"}";
        }
        j += "]}";
        HttpSendJson(cli, j);
    }
    // GET /api/v1/windows/foreground — current foreground window info (TinyWall-style pick)
    else if (method == "GET" && path == "/api/v1/windows/foreground") {
        HWND fg = GetForegroundWindow();
        if (!fg) {
            HttpSendJson(cli, "{\"found\":false,\"reason\":\"no foreground window\"}");
            closesocket(cli);
            return;
        }
        wchar_t title[512] = {0};
        wchar_t cls[256] = {0};
        GetWindowTextW(fg, title, 512);
        GetClassNameW(fg, cls, 256);
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        // Get process name + full image path
        std::wstring procName;
        std::wstring procPathFull;
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            wchar_t procPath[MAX_PATH] = {0};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, procPath, &sz)) {
                procPathFull = procPath;
                procName = procPath;
                // Extract just the filename
                std::size_t slash = procName.find_last_of(L'\\');
                if (slash != std::wstring::npos) procName = procName.substr(slash + 1);
            }
            CloseHandle(hProc);
        }
        // Convert to UTF-8
        auto toUtf8 = [](const std::wstring& ws) -> std::string {
            if (ws.empty()) return std::string();
            int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
            std::string s(len, 0);
            WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], len, NULL, NULL);
            return s;
        };
        std::string j = "{\"found\":true,\"hwnd\":"
            + std::to_string((unsigned long)(ULONG_PTR)fg) + ",\"pid\":"
            + std::to_string((unsigned long)pid) + ",\"title\":\""
            + JsonEscape(toUtf8(title)) + "\",\"windowClass\":\""
            + JsonEscape(toUtf8(cls)) + "\",\"processName\":\""
            + JsonEscape(toUtf8(procName)) + "\",\"processPath\":\""
            + JsonEscape(toUtf8(procPathFull)) + "\"}";
        HttpSendJson(cli, j);
    }
    // POST /api/v1/windows/foreground/pick — pick the foreground window as target for a profile
    // (Equivalent to writing the foreground info into the profile editor)
    else if (method == "POST" && path == "/api/v1/windows/foreground/pick") {
        HWND fg = GetForegroundWindow();
        if (!fg) {
            HttpSendJson(cli, "{\"found\":false}", 400);
            closesocket(cli);
            return;
        }
        wchar_t title[512] = {0};
        wchar_t cls[256] = {0};
        GetWindowTextW(fg, title, 512);
        GetClassNameW(fg, cls, 256);
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        std::wstring procName;
        std::wstring exePath;
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            wchar_t pp[MAX_PATH] = {0};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, pp, &sz)) {
                exePath = pp;
                std::size_t sl = exePath.find_last_of(L'\\');
                if (sl != std::wstring::npos) procName = exePath.substr(sl + 1);
            }
            CloseHandle(hProc);
        }
        // Return the fields the UI needs to fill the editor
        auto toUtf8 = [](const std::wstring& ws) -> std::string {
            if (ws.empty()) return std::string();
            int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
            std::string s(len, 0);
            WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], len, NULL, NULL);
            return s;
        };
        std::string rTitle, rClass, rProc, rPath;
        rTitle = toUtf8(title);
        rClass = toUtf8(cls);
        rProc = toUtf8(procName);
        rPath = toUtf8(exePath);
        std::string j = "{\"found\":true,\"title\":\"" + JsonEscape(rTitle)
            + "\",\"windowClass\":\"" + JsonEscape(rClass)
            + "\",\"processName\":\"" + JsonEscape(rProc)
            + "\",\"processPath\":\"" + JsonEscape(rPath) + "\"}";
        HttpSendJson(cli, j);
    }
    // POST /api/v1/applications/create — {name, windowClass, exePath, processName}
    else if (method == "POST" && path == "/api/v1/applications/create") {
        std::string name, windowClass, exePath, processName;
        JsonGetStr(body, "name", name);
        JsonGetStr(body, "windowClass", windowClass);
        JsonGetStr(body, "exePath", exePath);
        JsonGetStr(body, "processName", processName);
        if (name.empty()) { HttpSendJson(cli, "{\"error\":\"missing name\"}", 400); }
        else {
            EnterCriticalSection(&g_csProfile);
            // Generate safe application id from name
            std::string appId = std::string("app-") + name;
            for (char& c : appId) {
                if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) c = '_';
            }
            // Ensure uniqueness
            std::string baseId = appId;
            int suffix = 2;
            while (g_domain.findApplication(appId)) {
                char buf[16]; snprintf(buf, sizeof(buf), "%d", suffix++);
                appId = baseId + "-" + buf;
            }
            g_domain.applications.push_back(keysidekick::ApplicationTarget(appId, name));
            keysidekick::ApplicationTarget& app = g_domain.applications.back();
            app.windowClass = windowClass;
            app.exePath = exePath;
            app.processName = processName;
            LeaveCriticalSection(&g_csProfile);

            WriteConfig();
            BumpRevision();

            std::string resp = "{\"ok\":true,\"id\":\"" + JsonEscape(appId) + "\"}";
            HttpSendJson(cli, resp);
        }
    }
    // GET /api/v1/presets — каталог AI-agent панелей
    else if (method == "GET" && path == "/api/v1/presets") {
        int n = (int)(sizeof(kAgentPresets)/sizeof(kAgentPresets[0]));
        std::string j = "{\"presets\":[";
        for (int i = 0; i < n; ++i) {
            const AgentPreset& p = kAgentPresets[i];
            if (i) j += ",";
            j += "{\"agentId\":\"" + std::string(p.agentId) + "\"";
            j += ",\"name\":\"" + JsonEscape(p.name) + "\"";
            j += ",\"description\":\"" + JsonEscape(p.desc) + "\"";
            j += ",\"kind\":\"" + std::string(p.kind) + "\"";
            j += ",\"mode\":\"" + std::string(p.profileMode) + "\"";
            j += ",\"keys\":[";
            for (int k = 0; k < p.keyCount; ++k) {
                if (k) j += ",";
                j += "{\"usage\":" + std::to_string(p.keys[k].usage);
                j += ",\"label\":\"" + JsonEscape(p.keys[k].label) + "\"";
                j += ",\"action\":\"" + JsonEscape(p.keys[k].action) + "\"}";
            }
            j += "]}";
        }
        j += "]}";
        HttpSendJson(cli, j);
    }
    // GET /api/v1/config/export — текущий config.ini (base64) для бэкапа/шеринга
    else if (method == "GET" && path == "/api/v1/config/export") {
        std::string content;
        FILE* f = fopen(CONFIG_FILE, "rb");
        if (f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
            fclose(f);
        }
        std::string j = "{\"config\":\"" + B64Encode(content) + "\"}";
        HttpSendJson(cli, j);
    }
    // POST /api/v1/config/import — {config: "<base64>"} → записать config.ini и перезагрузить
    else if (method == "POST" && path == "/api/v1/config/import") {
        std::string b64;
        JsonGetStr(body, "config", b64);
        std::string cfg = B64Decode(b64);
        if (cfg.empty()) { HttpSendJson(cli, "{\"error\":\"missing or empty config\"}", 400); }
        else {
            std::wstring wpath;
            int wlen = MultiByteToWideChar(CP_ACP, 0, CONFIG_FILE, -1, NULL, 0);
            wpath.resize(wlen);
            MultiByteToWideChar(CP_ACP, 0, CONFIG_FILE, -1, &wpath[0], wlen);
            if (!wpath.empty() && wpath.back() == 0) wpath.pop_back();
            keysidekick::StorageResult r = keysidekick::AtomicWriteUtf8(
                wpath, cfg, [](const std::string&, std::string*) { return true; });
            if (!r.ok()) {
                HttpSendJson(cli, "{\"error\":\"write failed\"}", 500);
            } else {
                DashOp* op = new DashOp();
                op->op = DASH_RELOAD;
                DashOpResult rr = RunDashOp(op);
                if (rr.callerOwnsOp) delete op;
                HttpSendJson(cli, rr.ok ? "{\"ok\":true}" : "{\"error\":\"reload failed\"}", rr.ok ? 200 : 500);
            }
        }
    }
    // POST /api/v1/preset/apply — создать targeted-profile с маппингами агента
    else if (method == "POST" && path == "/api/v1/preset/apply") {
        std::string agentId, profileId, name, windowClass, processName, exePath;
        JsonGetStr(body, "agentId", agentId);
        JsonGetStr(body, "profileId", profileId);
        JsonGetStr(body, "name", name);
        JsonGetStr(body, "windowClass", windowClass);
        JsonGetStr(body, "processName", processName);
        JsonGetStr(body, "exePath", exePath);
        if (agentId.empty()) { HttpSendJson(cli, "{\"error\":\"missing agentId\"}", 400); }
        else {
            DashOp* op = new DashOp();
            op->op = DASH_APPLY_PRESET;
            snprintf(op->strArg2, sizeof(op->strArg2), "%s", agentId.c_str());
            snprintf(op->profile, sizeof(op->profile), "%s", profileId.c_str());
            snprintf(op->strArg1, sizeof(op->strArg1), "%s", name.c_str());
            snprintf(op->targetClass, sizeof(op->targetClass), "%s", windowClass.c_str());
            snprintf(op->targetExe, sizeof(op->targetExe), "%s", processName.c_str());
            snprintf(op->targetPath, sizeof(op->targetPath), "%s", exePath.c_str());
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok ? "{\"ok\":true}"
                : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if (rr.callerOwnsOp) delete op;
        }
    }
    // POST /api/v1/applications/test-resolve — {windowClass, processName, processPath}
    // Проверить, находится ли окно для данного target
    else if (method == "POST" && path == "/api/v1/applications/test-resolve") {
        std::string windowClass, processName, processPath;
        JsonGetStr(body, "windowClass", windowClass);
        JsonGetStr(body, "processName", processName);
        JsonGetStr(body, "processPath", processPath);

        auto toWstring = [](const std::string& s) -> std::wstring {
            if (s.empty()) return std::wstring();
            int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
            std::wstring ws(len, 0);
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], len);
            return ws;
        };

        keysidekick::windows_targets::WindowFilterPolicy policy;
        policy.requireVisible = true;
        policy.excludeEmptyTitles = false;
        policy.excludeToolWindows = false;
        policy.excludeShellWindows = false;
        std::vector<keysidekick::windows_targets::WindowCandidate> windows =
            keysidekick::windows_targets::EnumerateWindows(policy);

        keysidekick::windows_targets::TargetQuery query;
        query.windowClass = toWstring(windowClass);
        query.processName = toWstring(processName);
        query.processPath = toWstring(processPath);

        keysidekick::windows_targets::WindowCandidate resolved;
        bool found = keysidekick::windows_targets::ResolveTarget(windows, query, &resolved, policy);

        if (found) {
            auto toUtf8 = [](const std::wstring& ws) -> std::string {
                if (ws.empty()) return std::string();
                int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
                std::string s(len, 0);
                WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], len, NULL, NULL);
                return s;
            };
            char header[128];
            snprintf(header, sizeof(header), "{\"found\":true,\"hwnd\":%lu,\"pid\":%lu,\"title\":\"",
                (unsigned long)resolved.handle, (unsigned long)resolved.processId);
            std::string resp = header;
            resp += JsonEscape(toUtf8(resolved.title));
            resp += "\"}";
            HttpSendJson(cli, resp);
        } else {
            HttpSendJson(cli, "{\"found\":false}");
        }
    }
    // ---- Phase 4: SSE live updates ----
    // GET /api/v1/events — Server-Sent Events stream
    else if (method == "GET" && path == "/api/v1/events") {
        // M6: Origin эхо-заголовок выводим только когда Origin разрешён
        // (keysidekick::IsAllowedOrigin); иначе sseOrigin пуст и заголовок
        // Access-Control-Allow-Origin не выводится вовсе (никогда не "*").
        std::string sseOrigin;
        if (!originHeader.empty() &&
            keysidekick::IsAllowedOrigin(originHeader, (unsigned short)g_httpPort, false)) {
            sseOrigin = originHeader;
        }
        SseClientStart* start = new SseClientStart();
        start->socket = cli;
        start->origin = sseOrigin;
        HANDLE worker = CreateThread(NULL, 0, SseClientThread, start, 0, NULL);
        if (worker) {
            CloseHandle(worker);
        } else {
            delete start;
            closesocket(cli);
        }
        return;  // skip the closesocket at the end
    }
    // ---- Phase 6: Startup management ----
    // GET /api/v1/startup — check if auto-start is configured
    else if (method == "GET" && path == "/api/v1/startup") {
        // Check Startup folder for KeySidekick.lnk
        wchar_t startupDir[MAX_PATH] = {0};
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, startupDir))) {
            std::wstring shortcutPath = std::wstring(startupDir) + L"\\KeySidekick.lnk";
            DWORD attr = GetFileAttributesW(shortcutPath.c_str());
            bool exists = (attr != INVALID_FILE_ATTRIBUTES);
            std::string resp = "{\"installed\":";
            resp += exists ? "true" : "false";
            resp += "}";
            HttpSendJson(cli, resp);
        } else {
            HttpSendJson(cli, "{\"installed\":false,\"error\":\"cannot query startup folder\"}", 500);
        }
    }
    // POST /api/v1/startup — {enabled: true/false} to create/remove startup shortcut
    else if (method == "POST" && path == "/api/v1/startup") {
        bool enabled = false;
        JsonGetBool(body, "enabled", enabled);

        wchar_t startupDir[MAX_PATH] = {0};
        if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, startupDir))) {
            HttpSendJson(cli, "{\"error\":\"cannot find startup folder\"}", 500);
            closesocket(cli); return;
        }
        std::wstring shortcutPath = std::wstring(startupDir) + L"\\KeySidekick.lnk";

        if (enabled) {
            // Create shortcut via IShellLinkW COM (safe, no shell injection, no console flash)
            wchar_t exePath[MAX_PATH] = {0};
            DWORD exeLen = GetModuleFileNameW(NULL, exePath, MAX_PATH);
            bool created = false;
            if (exeLen > 0 && exeLen < MAX_PATH) {
                HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
                if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
                    IShellLinkW* pShellLink = NULL;
                    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (LPVOID*)&pShellLink);
                    if (SUCCEEDED(hr) && pShellLink) {
                        pShellLink->SetPath(exePath);
                        // Working directory = exe folder
                        std::wstring exeDir = std::wstring(exePath);
                        std::size_t lastSlash = exeDir.find_last_of(L'\\');
                        if (lastSlash != std::wstring::npos) {
                            exeDir = exeDir.substr(0, lastSlash);
                            pShellLink->SetWorkingDirectory(exeDir.c_str());
                        }
                        pShellLink->SetShowCmd(SW_SHOWMINNOACTIVE);  // minimized
                        IPersistFile* pPersistFile = NULL;
                        hr = pShellLink->QueryInterface(IID_IPersistFile, (LPVOID*)&pPersistFile);
                        if (SUCCEEDED(hr) && pPersistFile) {
                            hr = pPersistFile->Save(shortcutPath.c_str(), TRUE);
                            if (SUCCEEDED(hr)) created = true;
                            pPersistFile->Release();
                        }
                        pShellLink->Release();
                    }
                    // Only uninitialize if we initialized (not RPC_E_CHANGED_MODE)
                    if (SUCCEEDED(hr)) CoUninitialize();
                }
            }
            if (created) {
                Log("Startup shortcut created: %ls", shortcutPath.c_str());
                HttpSendJson(cli, "{\"ok\":true,\"installed\":true}");
            } else {
                Log("Startup shortcut creation FAILED");
                HttpSendJson(cli, "{\"error\":\"shortcut creation failed\"}", 500);
            }
        } else {
            if (DeleteFileW(shortcutPath.c_str())) {
                Log("Startup shortcut removed");
                HttpSendJson(cli, "{\"ok\":true,\"installed\":false}");
            } else {
                // Not an error if file didn't exist
                DWORD err = GetLastError();
                if (err == ERROR_FILE_NOT_FOUND) {
                    HttpSendJson(cli, "{\"ok\":true,\"installed\":false}");
                } else {
                    Log("Startup shortcut removal failed err=%lu", err);
                    HttpSendJson(cli, "{\"error\":\"removal failed\"}", 500);
                }
            }
        }
    }
    // POST /api/v1/driver/swap — переприменить WinUSB через ks_driver.exe
    // (порт сменился / первый раз). Требует UAC-согласия на элевацию.
    else if (method == "POST" && path == "/api/v1/driver/swap") {
        std::string vidpid;
        JsonGetStr(body, "vidpid", vidpid);
        if (vidpid.empty()) {
            EnterCriticalSection(&g_csProfile);
            vidpid = g_portChangeVidPid;
            LeaveCriticalSection(&g_csProfile);
        }
        if (vidpid.empty()) { HttpSendJson(cli, "{\"error\":\"missing vidpid\"}", 400); }
        else {
            // Формат: vid_xxxx&pid_yyyy — только hex/буквы & _ (защита от инъекции аргументов)
            bool formatOk = true;
            for (char c : vidpid) {
                if (!(isalnum((unsigned char)c) || c == '&' || c == '_' || c == '-')) { formatOk = false; break; }
            }
            if (!formatOk) { HttpSendJson(cli, "{\"error\":\"invalid vidpid format\"}", 400); }
            else {
                char cmdline[512];
                snprintf(cmdline, sizeof(cmdline), "--driver swap %s", vidpid.c_str());
                wchar_t wcmd[512], wexe[MAX_PATH];
                MultiByteToWideChar(CP_ACP, 0, cmdline, -1, wcmd, 512);
                GetModuleFileNameW(NULL, wexe, MAX_PATH);
                HINSTANCE h = ShellExecuteW(NULL, L"runas", wexe, wcmd, NULL, SW_SHOWNORMAL);
                if ((INT_PTR)h <= 32) {
                    Log("driver/swap: ShellExecute runas failed err=%d", (int)(INT_PTR)h);
                    HttpSendJson(cli, "{\"error\":\"failed to start elevated driver swap (UAC cancelled?)\"}", 500);
                } else {
                    Log("driver/swap: launched elevated self (--driver swap %s)", vidpid.c_str());
                    HttpSendJson(cli, "{\"ok\":true,\"started\":true,\"note\":\"Confirm the UAC prompt; the keyboard reappears automatically\"}");
                }
            }
        }
    }
    // POST /api/v1/driver/restore — вернуть встроенный HID-драйвер (input.inf)
    else if (method == "POST" && path == "/api/v1/driver/restore") {
        std::string vidpid;
        JsonGetStr(body, "vidpid", vidpid);
        if (vidpid.empty()) { HttpSendJson(cli, "{\"error\":\"missing vidpid\"}", 400); }
        else {
            bool formatOk = true;
            for (char c : vidpid) {
                if (!(isalnum((unsigned char)c) || c == '&' || c == '_' || c == '-')) { formatOk = false; break; }
            }
            if (!formatOk) { HttpSendJson(cli, "{\"error\":\"invalid vidpid format\"}", 400); }
            else {
                char cmdline[512];
                snprintf(cmdline, sizeof(cmdline), "--driver restore %s", vidpid.c_str());
                wchar_t wcmd[512], wexe[MAX_PATH];
                MultiByteToWideChar(CP_ACP, 0, cmdline, -1, wcmd, 512);
                GetModuleFileNameW(NULL, wexe, MAX_PATH);
                HINSTANCE h = ShellExecuteW(NULL, L"runas", wexe, wcmd, NULL, SW_SHOWNORMAL);
                if ((INT_PTR)h <= 32) {
                    Log("driver/restore: ShellExecute runas failed err=%d", (int)(INT_PTR)h);
                    HttpSendJson(cli, "{\"error\":\"failed to start elevated driver restore (UAC cancelled?)\"}", 500);
                } else {
                    HttpSendJson(cli, "{\"ok\":true,\"started\":true,\"note\":\"Confirm the UAC prompt\"}");
                }
            }
        }
    }
    // GET /api/v1/devices — enumerate all WinUSB/HID keyboards for multi-device setup
    else if (method == "GET" && path == "/api/v1/devices") {
        // Enumerate all WinUSB-compatible devices (not filtering by VID/PID)
        std::string j = "{\"devices\":[";
        bool first = true;

        const GUID* guids[] = { &GUID_DEVINTERFACE_WINUSB, &GUID_DEVINTERFACE_TARGET_WINUSB, &GUID_DEVINTERFACE_LIBUSB0, NULL };
        for (int g = 0; guids[g]; g++) {
            HDEVINFO hDev = SetupDiGetClassDevs(guids[g], NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (hDev == INVALID_HANDLE_VALUE) continue;
            SP_DEVICE_INTERFACE_DATA ifData = { sizeof(ifData) };
            for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hDev, NULL, guids[g], idx, &ifData); idx++) {
                DWORD needed = 0;
                SetupDiGetDeviceInterfaceDetailA(hDev, &ifData, NULL, 0, &needed, NULL);
                if (!needed) continue;
                std::vector<BYTE> buf(needed);
                PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)buf.data();
                det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
                if (!SetupDiGetDeviceInterfaceDetailA(hDev, &ifData, det, needed, NULL, NULL)) continue;

                const char* dp = det->DevicePath;
                // Extract VID/PID from path
                std::string vidpid;
                const char* vid = strstr(dp, "vid_");
                if (vid) {
                    const char* mi = strstr(dp, "&mi_");
                    std::size_t vidEnd = mi ? (std::size_t)(mi - dp - (vid - dp)) : std::string::npos;
                    if (vidEnd != std::string::npos) {
                        vidpid.assign(dp + (vid - dp), vidEnd - (vid - dp));
                    }
                }
                std::string displayName = vidpid.empty() ? "WinUSB Device" : vidpid;
                if (!first) j += ",";
                first = false;
                j += "{\"path\":\"" + JsonEscape(dp) + "\",\"vidpid\":\"" + JsonEscape(vidpid) + "\",\"name\":\"" + JsonEscape(displayName) + "\"}";
            }
            SetupDiDestroyDeviceInfoList(hDev);
        }
        j += "]}";
        HttpSendJson(cli, j);
    }
    // GET /api/v1/devices/detect — probe each WinUSB keyboard: short read with 500ms timeout,
    // and tell the caller which device (if any) produced data. Used for "identify my keyboard".
    else if (method == "GET" && path == "/api/v1/devices/detect") {
        // Страховка: сбрасываем все инжектированные клавиши ПЕРЕД пробированием —
        // даже одна украденная/потерянная key-up не оставит «залипший» Ctrl/Shift.
        ReleaseAllKeys();
        ReleaseAllTargetedKeys();
        std::string j = "{\"detected\":[";
        bool firstDet = true;

        const GUID* guids[] = { &GUID_DEVINTERFACE_WINUSB, &GUID_DEVINTERFACE_TARGET_WINUSB, &GUID_DEVINTERFACE_LIBUSB0, NULL };
        for (int g = 0; guids[g]; g++) {
            HDEVINFO hDev = SetupDiGetClassDevs(guids[g], NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (hDev == INVALID_HANDLE_VALUE) continue;
            SP_DEVICE_INTERFACE_DATA ifData = { sizeof(ifData) };
            for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hDev, NULL, guids[g], idx, &ifData); idx++) {
                DWORD needed = 0;
                SetupDiGetDeviceInterfaceDetailA(hDev, &ifData, NULL, 0, &needed, NULL);
                if (!needed) continue;
                std::vector<BYTE> buf(needed);
                PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)buf.data();
                det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
                if (!SetupDiGetDeviceInterfaceDetailA(hDev, &ifData, det, needed, NULL, NULL)) continue;

                // M5: НЕ открываем интерфейс, который уже читает ReadLoop —
                // второй ридер крадёт interrupt-IN репорты (теряются key-up →
                // «залипшие» инжектированные модификаторы до отключения).
                if (IsActiveDevicePath(det->DevicePath)) continue;
                // Try opening + reading from this device for 500ms
                HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
                if (h == INVALID_HANDLE_VALUE) continue;
                WINUSB_INTERFACE_HANDLE wusb = NULL;
                if (!WinUsb_Initialize(h, &wusb)) { CloseHandle(h); continue; }
                // M3: не хардкодим 0x81 — читаем фактический interrupt-IN pipe
                // из дескриптора интерфейса (у некоторых клавиатур он другой).
                BYTE pipeId = QueryInterruptInPipe(wusb);
                if (pipeId == 0xFF) { WinUsb_Free(wusb); CloseHandle(h); continue; }
                // Set short timeout for detection
                ULONG timeoutMs = 500;
                WinUsb_SetPipePolicy(wusb, pipeId, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
                BYTE rbuf[8] = {0};
                OVERLAPPED ov = {0}; ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
                if (ov.hEvent) {
                    ULONG got = 0;
                    if (WinUsb_ReadPipe(wusb, pipeId, rbuf, sizeof(rbuf), &got, &ov)) {
                        // Immediate data — device is actively sending
                        if (!firstDet) j += ",";
                        firstDet = false;
                        char shortPath[256];
                        strncpy(shortPath, det->DevicePath, sizeof(shortPath)-1);
                        j += "{\"path\":\"" + JsonEscape(shortPath) + "\",\"detected\":true}";
                    } else if (GetLastError() == ERROR_IO_PENDING) {
                        DWORD wr = WaitForSingleObject(ov.hEvent, 500);
                        if (wr == WAIT_OBJECT_0) {
                            if (WinUsb_GetOverlappedResult(wusb, &ov, &got, FALSE) && got > 0) {
                                if (!firstDet) j += ",";
                                firstDet = false;
                                char shortPath[256];
                                strncpy(shortPath, det->DevicePath, sizeof(shortPath)-1);
                                j += "{\"path\":\"" + JsonEscape(shortPath) + "\",\"detected\":true}";
                            }
                        } else {
                            WinUsb_AbortPipe(wusb, pipeId);
                            WaitForSingleObject(ov.hEvent, 100);
                        }
                    }
                    CloseHandle(ov.hEvent);
                }
                WinUsb_Free(wusb);
                CloseHandle(h);
            }
            SetupDiDestroyDeviceInfoList(hDev);
        }
        j += "]}";
        HttpSendJson(cli, j);
    }
    // GET /api/v1/devices/capture — interactive keyboard test
    // Opens each available device briefly and captures which one produced data.
    // Used by onboarding wizard: "press any key on your keyboard".
    else if (method == "POST" && path == "/api/v1/devices/capture") {
        ReleaseAllKeys();
        ReleaseAllTargetedKeys();
        std::string j = "{\"found\":null,\"counts\":{}";
        DWORD start = GetTickCount();
        BYTE sampleBuf[8] = {0};
        int bestGot = 0;
        std::string bestPath;

        const GUID* guids[] = { &GUID_DEVINTERFACE_WINUSB, &GUID_DEVINTERFACE_TARGET_WINUSB, &GUID_DEVINTERFACE_LIBUSB0, NULL };
        for (int g = 0; guids[g]; g++) {
            HDEVINFO hDev = SetupDiGetClassDevs(guids[g], NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (hDev == INVALID_HANDLE_VALUE) continue;
            SP_DEVICE_INTERFACE_DATA ifData = { sizeof(ifData) };
            for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hDev, NULL, guids[g], idx, &ifData); idx++) {
                DWORD needed = 0;
                SetupDiGetDeviceInterfaceDetailA(hDev, &ifData, NULL, 0, &needed, NULL);
                if (!needed) continue;
                std::vector<BYTE> buf(needed);
                PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)buf.data();
                det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
                if (!SetupDiGetDeviceInterfaceDetailA(hDev, &ifData, det, needed, NULL, NULL)) continue;

                // M5: не трогаем активный интерфейс ReadLoop (кража репортов).
                if (IsActiveDevicePath(det->DevicePath)) continue;
                // Try opening and reading for 300ms
                HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
                if (h == INVALID_HANDLE_VALUE) continue;
                WINUSB_INTERFACE_HANDLE wusb = NULL;
                if (!WinUsb_Initialize(h, &wusb)) { CloseHandle(h); continue; }
                // M3: фактический interrupt-IN pipe из дескриптора, не 0x81.
                BYTE pipeId = QueryInterruptInPipe(wusb);
                if (pipeId == 0xFF) { WinUsb_Free(wusb); CloseHandle(h); continue; }
                ULONG timeoutMs = 300;
                WinUsb_SetPipePolicy(wusb, pipeId, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);

                OVERLAPPED ov = {0};
                ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
                if (ov.hEvent) {
                    ULONG got = 0;
                    BOOL ok = WinUsb_ReadPipe(wusb, pipeId, sampleBuf, sizeof(sampleBuf), &got, &ov);
                    if (ok && got > 0) {
                        if (got > bestGot) { bestGot = (int)got; bestPath = det->DevicePath; }
                    } else if (GetLastError() == ERROR_IO_PENDING) {
                        DWORD wr = WaitForSingleObject(ov.hEvent, 300);
                        if (wr == WAIT_OBJECT_0 && WinUsb_GetOverlappedResult(wusb, &ov, &got, FALSE) && got > 0) {
                            if (got > bestGot) { bestGot = (int)got; bestPath = det->DevicePath; }
                        }
                        WinUsb_AbortPipe(wusb, pipeId);
                        WaitForSingleObject(ov.hEvent, 50);
                    }
                    CloseHandle(ov.hEvent);
                }
                WinUsb_Free(wusb);
                CloseHandle(h);
            }
            SetupDiDestroyDeviceInfoList(hDev);
        }

        if (!bestPath.empty()) {
            j += ",\"found\":{\"path\":\"" + JsonEscape(bestPath) + "\",\"bytes\":" + std::to_string(bestGot) + "}";
        }
        j += "}";
        HttpSendJson(cli, j);
    }
    // ---- Phase "HID-first": все устройства ввода (обычные + WinUSB) ----
    // GET /api/v1/hid — клавиатуры, мыши, смарт-устройства с клавиатурой
    // независимо от драйвера: status ready (WinUSB) | needs-driver | ordinary.
    else if (method == "GET" && path == "/api/v1/hid") {
        std::vector<InputDeviceRow> rows;
        EnumerateInputDevices(rows);
        std::string j = "{\"devices\":[";
        bool first = true;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const InputDeviceRow& r = rows[i];
            std::string status = r.winusb ? "ready"
                : (r.kinds.find("keyboard") != std::string::npos ? "needs-driver" : "ordinary");
            if (!first) j += ",";
            first = false;
            j += "{\"usbId\":\"" + JsonEscape(r.usbId) + "\"";
            j += ",\"name\":\"" + JsonEscape(r.name) + "\"";
            j += ",\"vid\":\"" + r.vid + "\"";
            j += ",\"pid\":\"" + r.pid + "\"";
            j += ",\"mi\":\"" + r.mi + "\"";
            j += ",\"service\":\"" + JsonEscape(r.service) + "\"";
            j += ",\"kinds\":\"" + r.kinds + "\"";
            j += ",\"status\":\"" + status + "\"";
            j += ",\"hidPath\":\"" + JsonEscape(r.hidPath) + "\"";
            j += ",\"winusbPath\":\"" + JsonEscape(r.winusbPath) + "\"";
            j += "}";
        }
        j += "]}";
        HttpSendJson(cli, j);
    }
    // POST /api/v1/input/identify — сброс и старт окна прослушивания:
    // пользователь жмёт клавишу, GET отдаёт источник (VID/PID).
    else if (method == "POST" && path == "/api/v1/input/identify") {
        EnterCriticalSection(&g_csIdentified);
        g_identified.has = false;
        g_identified.identifiable = false;
        g_identified.seq = 0;
        g_identified.lastVk = 0;
        g_identified.lastMakeCode = 0;
        g_identified.vid.clear(); g_identified.pid.clear(); g_identified.mi.clear();
        g_identified.name.clear(); g_identified.path.clear();
        g_identifyEvents.clear();
        g_identifySeq = 0;
        LeaveCriticalSection(&g_csIdentified);
        HttpSendJson(cli, "{\"ok\":true,\"listening\":true}");
    }
    // GET /api/v1/input/identify — poll: живой фид нажатий + источник (VID/PID)
    else if (method == "GET" && path == "/api/v1/input/identify") {
        std::string j = "{";
        EnterCriticalSection(&g_csIdentified);
        j += std::string("\"listening\":") + (g_identified.has ? "true" : "false");
        j += ",\"count\":" + std::to_string(g_identifySeq);
        if (g_identified.has) {
            j += ",\"identifiable\":" + std::string(g_identified.identifiable ? "true" : "false");
            j += ",\"lastVk\":" + std::to_string(g_identified.lastVk);
            j += ",\"lastMakeCode\":" + std::to_string(g_identified.lastMakeCode);
            if (g_identified.identifiable) {
                j += ",\"device\":{";
                j += "\"vid\":\"" + g_identified.vid + "\"";
                j += ",\"pid\":\"" + g_identified.pid + "\"";
                j += ",\"mi\":\"" + g_identified.mi + "\"";
                j += ",\"name\":\"" + JsonEscape(g_identified.name) + "\"";
                j += ",\"path\":\"" + JsonEscape(g_identified.path) + "\"";
                j += "}";
            }
        }
        // live-поток последних нажатий
        j += ",\"events\":[";
        for (std::size_t i = 0; i < g_identifyEvents.size(); ++i) {
            const IdentifyEvent& e = g_identifyEvents[i];
            if (i) j += ",";
            j += "{\"seq\":" + std::to_string(e.seq);
            j += ",\"vk\":" + std::to_string(e.vk);
            j += ",\"makeCode\":" + std::to_string(e.makeCode);
            j += ",\"identifiable\":" + std::string(e.identifiable ? "true" : "false");
            j += ",\"vid\":\"" + e.vid + "\"";
            j += ",\"pid\":\"" + e.pid + "\"";
            j += ",\"name\":\"" + JsonEscape(e.name) + "\"";
            j += "}";
        }
        j += "]";
        LeaveCriticalSection(&g_csIdentified);
        j += "}";
        HttpSendJson(cli, j);
    }
    // GET /api/v1/activity — недавно сработавшие actions (Live-экран)
    else if (method == "GET" && path == "/api/v1/activity") {
        std::vector<ActivityEvent> copy;
        EnterCriticalSection(&g_csActivity);
        copy = g_activityEvents;
        LeaveCriticalSection(&g_csActivity);
        std::string j = "{\"events\":[";
        for (std::size_t i = 0; i < copy.size(); ++i) {
            const ActivityEvent& e = copy[i];
            if (i) j += ",";
            j += "{\"seq\":" + std::to_string(e.seq);
            j += ",\"t\":" + std::to_string(e.tick);
            j += ",\"usage\":" + std::to_string(e.usage);
            j += ",\"action\":\"" + JsonEscape(e.action) + "\"";
            j += ",\"mode\":\"" + JsonEscape(e.mode) + "\"";
            j += "}";
        }
        j += "]}";
        HttpSendJson(cli, j);
    }
    // POST /api/v1/devices/activate — {vidpid}: сделать устройство активным:
    // добавить VID/PID в config (DeviceVIDPID) и переподключить hot path.
    else if (method == "POST" && path == "/api/v1/devices/activate") {
        std::string vidpid, devPath;
        JsonGetStr(body, "vidpid", vidpid);
        JsonGetStr(body, "path", devPath);
        std::string norm;
        bool fmtOk = false;
        if (!devPath.empty()) {
            // Per-instance: полный путь устройства как паттерн (lowercase).
            // FindDevicePath матчит substring — путь содержит серийник, так что
            // попадает только этот конкретный экземпляр (две одинаковые клавиатуры).
            for (std::size_t ci = 0; ci < devPath.size(); ++ci) {
                char c = devPath[ci];
                if (c != ' ' && c != '\t') norm += (char)tolower((unsigned char)c);
            }
            fmtOk = !norm.empty() && norm.size() <= 255;
        } else {
            // Формат: "vid_xxxx&pid_yyyy" (4 hex каждая).
            for (std::size_t ci = 0; ci < vidpid.size(); ++ci) {
                char c = vidpid[ci];
                if (c != ' ' && c != '\t') norm += (char)tolower((unsigned char)c);
            }
            fmtOk = (norm.size() >= 17 && norm.compare(0, 4, "vid_") == 0);
            if (fmtOk) {
                std::size_t amp = norm.find("&pid_", 4);
                if (amp == std::string::npos || amp + 9 > norm.size()) fmtOk = false;
                else {
                    for (std::size_t ci = 4; ci < amp && fmtOk; ++ci)
                        if (!isxdigit((unsigned char)norm[ci])) fmtOk = false;
                    for (std::size_t ci = amp + 5; ci < norm.size() && fmtOk; ++ci)
                        if (!isxdigit((unsigned char)norm[ci])) fmtOk = false;
                }
            }
        }
        if (!fmtOk) {
            HttpSendJson(cli, "{\"error\":\"invalid vidpid or device path\"}", 400);
        } else {
            DashOp* op = new DashOp();
            op->op = DASH_ACTIVATE_DEVICE;
            snprintf(op->action, sizeof(op->action), "%s", norm.c_str());
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok
                ? "{\"ok\":true,\"pattern\":\"" + JsonEscape(norm) + "\"}"
                : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            if (rr.ok) {
                // ACTIVATE WARNING: если устройство видно в /api/v1/hid, но НЕ
                // WinUSB-ready (status != ready), реконнект его не откроет —
                // предупреждаем пользователя (остаёмся lenient: ok:true; если
                // устройства в списке нет — паттерн может быть частичным).
                std::vector<InputDeviceRow> rows;
                EnumerateInputDevices(rows);
                bool exactFound = false;
                bool ready = false;
                for (std::size_t ri = 0; ri < rows.size(); ++ri) {
                    const InputDeviceRow& r = rows[ri];
                    bool match = false;
                    if (!devPath.empty()) {
                        std::string hay = ToLower(r.hidPath.c_str()) + ToLower(r.winusbPath.c_str());
                        match = !hay.empty() && hay.find(norm) != std::string::npos;
                    } else {
                        std::string rowPat = "vid_" + ToLower(r.vid.c_str()) + "&pid_" + ToLower(r.pid.c_str());
                        match = (rowPat == norm);
                    }
                    if (match) { exactFound = true; if (r.winusb) ready = true; }
                }
                if (exactFound && !ready) {
                    resp = "{\"ok\":true,\"pattern\":\"" + JsonEscape(norm)
                        + "\",\"warning\":\"device is not on the WinUSB driver — run the driver swap first\"}";
                }
            }
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if (rr.callerOwnsOp) delete op;
        }
    }
    // POST /api/v1/action/fire — {action, usage?, profile?} → выполнить действие
    // «как будто нажата физическая клавиша» на активном (или указанном) профиле.
    // Runtime-only: config не пишется. Используется Live click-to-fire.
    else if (method == "POST" && path == "/api/v1/action/fire") {
        std::string action, profile;
        int usage = 0;
        JsonGetStr(body, "action", action);
        JsonGetStr(body, "profile", profile);
        JsonGetInt(body, "usage", usage);
        if (action.empty()) {
            HttpSendJson(cli, "{\"error\":\"missing action\"}", 400);
        } else {
            DashOp* op = new DashOp();
            op->op = DASH_FIRE_ACTION;
            op->noPersist = true;
            snprintf(op->action, sizeof(op->action), "%s", action.c_str());
            snprintf(op->profile, sizeof(op->profile), "%s", profile.c_str());
            op->usage = usage;
            DashOpResult rr = RunDashOp(op);
            std::string resp = rr.ok
                ? "{\"ok\":true,\"fired\":true}"
                : (std::string("{\"error\":\"") + JsonEscape(rr.error) + "\"}");
            HttpSendJson(cli, resp, rr.ok ? 200 : 400);
            if (rr.callerOwnsOp) delete op;
        }
    }
    // ---- Phase 7: Diagnostics ----
    // GET /api/v1/diagnostics — device/driver/config health snapshot
    else if (method == "GET" && path == "/api/v1/diagnostics") {
        std::string j = "{";
        // Device state
        bool devConnected = false; unsigned char devPipe = 0xFF;
        DevInfoSnapshot(devConnected, devPipe);
        std::string vidpid;
        EnterCriticalSection(&g_csProfile); vidpid = g_deviceVidPid; LeaveCriticalSection(&g_csProfile);
        j += "\"device\":\"" + std::string(devConnected ? "connected" : "disconnected") + "\"";
        j += ",\"winusbHandle\":" + std::string(devConnected ? "true" : "false");
        char pipeHex[8]; snprintf(pipeHex, sizeof(pipeHex), "0x%02X", devPipe);
        j += ",\"pipeId\":\"" + std::string(pipeHex) + "\"";
        j += ",\"vidpid\":\"" + JsonEscape(vidpid) + "\"";

        // Device path (from SetupAPI)
        std::string devPath;
        if (FindDevicePath(devPath)) {
            j += ",\"devicePath\":\"" + JsonEscape(devPath) + "\"";
            j += ",\"deviceEnumerated\":true";
        } else {
            j += ",\"deviceEnumerated\":false";
        }

        // Config health
        j += ",\"configFile\":\"" + std::string(CONFIG_FILE) + "\"";
        DWORD attr = GetFileAttributesA(CONFIG_FILE);
        j += ",\"configExists\":" + std::string(attr != INVALID_FILE_ATTRIBUTES ? "true" : "false");
        size_t diagProfileCount, diagAppCount;
        std::string diagActive;
        EnterCriticalSection(&g_csProfile);
        diagProfileCount = g_profiles.size();
        diagAppCount = g_domain.applications.size();
        diagActive = g_activeProfile;
        LeaveCriticalSection(&g_csProfile);
        j += ",\"profileCount\":" + std::to_string(diagProfileCount);
        j += ",\"appCount\":" + std::to_string(diagAppCount);
        j += ",\"activeProfile\":\"" + JsonEscape(diagActive) + "\"";

        // Driver check: is MI_00 using WinUSB?
        // Check registry for the device interface GUID
        j += ",\"driverInfo\":{";
        j += "\"note\":\"Check Device Manager > KeySidekick device > Driver. Should be WinUSB.sys\"";
        j += ",\"winusbGuid\":\"{901A2603-A95E-4CA8-86BF-FB0547C06B64}\"";
        j += "}";

        // HTTP/API health
        j += ",\"httpPort\":" + std::to_string(g_httpPort);
        j += ",\"httpEnabled\":" + std::string(g_httpEnabled ? "true" : "false");
        j += ",\"trayEnabled\":" + std::string(g_trayEnabled ? "true" : "false");

        // Recent log lines (last 10)
        j += ",\"recentLog\":[";
        // Read last 10 lines from log file
        {
            FILE* lf = fopen(LOG_FILE, "r");
            if (lf) {
                // Read all lines, keep last 10
                std::vector<std::string> lines;
                char line[512];
                while (fgets(line, sizeof(line), lf)) {
                    std::string s(line);
                    while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
                    lines.push_back(s);
                }
                fclose(lf);
                std::size_t start = lines.size() > 10 ? lines.size() - 10 : 0;
                bool firstLog = true;
                for (std::size_t i = start; i < lines.size(); ++i) {
                    if (!firstLog) j += ",";
                    firstLog = false;
                    j += "\"" + JsonEscape(lines[i]) + "\"";
                }
            }
        }
        j += "]";

        j += "}";
        HttpSendJson(cli, j);
    }
    // POST /api/capture/start
    else if (method == "POST" && path == "/api/capture/start") {
        g_capturedUsage = 0;
        g_captureArmed = true;
        HttpSendJson(cli, "{\"ok\":true,\"armed\":true}");
    }
    // GET /api/capture/poll
    else if (method == "GET" && path == "/api/capture/poll") {
        int u = g_capturedUsage.exchange(0);
        if (u != 0) {
            char j[128];
            snprintf(j, sizeof(j), "{\"ready\":true,\"usage\":%d}", u);
            HttpSendJson(cli, j);
        } else {
            HttpSendJson(cli, "{\"ready\":false}");
        }
    }
    // Backwards-compat: GET /profiles (плоский текстовый список)
    else if (method == "GET" && path == "/profiles") {
        std::string list;
        EnterCriticalSection(&g_csProfile);
        for (auto& kv : g_profiles) { list += kv.first; list += "\n"; }
        LeaveCriticalSection(&g_csProfile);
        HttpSend(cli, list.c_str(), "text/plain");
    }
    else {
        HttpSend(cli, "not found", "text/plain", 404);
    }
}

static DWORD WINAPI HttpWorkerThread(LPVOID parameter) {
    SOCKET cli = (SOCKET)(ULONG_PTR)parameter;
    if (!g_running) {
        closesocket(cli);
    } else {
        HandleHttpConnection(cli);
    }
    g_httpWorkersActive.fetch_sub(1);
    ReleaseSemaphore(g_httpWorkerSemaphore, 1, NULL);
    return 0;
}


static void StartHttp() {
    if (!g_httpEnabled) return;
    g_httpRunning = true;
    g_httpWorkerSemaphore = CreateSemaphoreW(NULL, 8, 8, NULL);
    if (!g_httpWorkerSemaphore) {
        Log("HTTP worker semaphore create failed");
        g_httpRunning = false;
        return;
    }
    g_httpThreadHandle = CreateThread(NULL, 0, HttpThread, NULL, 0, NULL);
    if (!g_httpThreadHandle) {
        CloseHandle(g_httpWorkerSemaphore);
        g_httpWorkerSemaphore = NULL;
        g_httpRunning = false;
        Log("HTTP thread create failed");
    }
}

// Разбор INI-ключа секции Keys → (usageId, modMask).
// Форматы: "USAGE_1E", "USAGE_1E+Ctrl+Shift", "30" (десятичный), "30+Alt".
// Возвращает true если usageId > 0.
static bool ParseKeySpec(const char* spec, int& outUid, unsigned char& outMod) {
    outMod = 0;
    std::string s = spec;
    // отделить модификаторы после первого '+', но USAGE_xx+Mod — '+', а hex без '+'
    size_t plusPos = s.find('+');
    std::string usagePart = (plusPos == std::string::npos) ? s : s.substr(0, plusPos);
    int uid = -1;
    if (_strnicmp(usagePart.c_str(), "USAGE_", 6) == 0) uid = (int)strtol(usagePart.c_str()+6, NULL, 16);
    else uid = atoi(usagePart.c_str());
    if (uid <= 0) return false;
    outUid = uid;
    if (plusPos != std::string::npos) {
        outMod = ParseModSuffix(s.substr(plusPos + 1));
        // в ParseModSuffix строка начинается сразу после '+'; но у нас "A+B+C" → substr(+1) = "B+C"
    }
    // корректировка: ParseModSuffix ожидает строку токенов через '+', без ведущего
    // (substr(plusPos+1) уже даёт "Ctrl+Shift" — корректно)
    return true;
}

// =====================================================================
//  LoadConfig v2 (multi-profile + обратная совместимость)
//  Legacy fallback parser — используется если config_v3+bridge падает.
// =====================================================================
static void LoadConfigLegacy() {
    FILE* f = fopen(CONFIG_FILE, "r");
    if (!f) { Log("Config not found: %s (defaults)", CONFIG_FILE); return; }

    char line[512];
    std::string curSection;        // текущая секция (то, что в [...])
    std::string curProfileName;    // активный профиль при парсинге (для [Profile.X] / [Profile.X.Keys])

    // обратная совместимость: флаг что встретили старый формат
    bool legacyMode = false;
    Profile* legacyProf = nullptr;

    while (fgets(line, sizeof(line), f)) {
        // trim
        int ll = (int)strlen(line);
        while (ll>0 && (line[ll-1]=='\r'||line[ll-1]=='\n')) line[--ll]=0;
        char* p = line; while (*p==' '||*p=='\t') p++;
        if (*p==0 || *p==';' || *p=='#') continue;

        if (*p == '[') {
            char* end = strchr(p, ']');
            if (end) *end = 0;
            curSection = p+1;
            // определить, профиль ли это
            if (curSection.rfind("Profile.", 0) == 0) {
                std::string rest = curSection.substr(8);
                size_t dot = rest.find('.');
                if (dot == std::string::npos) {
                    // [Profile.NAME] — заголовок профиля
                    curProfileName = rest;
                    g_profiles[curProfileName].name = curProfileName;
                    g_profiles[curProfileName].mode = MODE_TARGETED;  // default для явных профилей
                } else {
                    // [Profile.NAME.Keys]
                    curProfileName = rest.substr(0, dot);
                }
            } else {
                curProfileName.clear();
            }
            // старый формат?
            if (curSection == "Keys" || curSection == "AIMP") {
                legacyMode = true;
                if (g_profiles.find("default") == g_profiles.end()) {
                    g_profiles["default"].name = "default";
                    g_profiles["default"].mode = MODE_TARGETED;
                }
                legacyProf = &g_profiles["default"];
            }
            continue;
        }

        // key=value
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = p; char* val = eq+1;
        Trim(key); Trim(val);

        if (curSection == "General") {
            if (_stricmp(key,"DeviceVIDPID")==0) { strncpy(g_deviceVidPid, val, sizeof(g_deviceVidPid)-1); }
            else if (_stricmp(key,"DefaultProfile")==0) { strncpy(g_defaultProfile, val, sizeof(g_defaultProfile)-1); }
            else if (_stricmp(key,"HTTPPort")==0) { g_httpPort = atoi(val); }
            else if (_stricmp(key,"HTTPEnabled")==0) { g_httpEnabled = atoi(val)!=0; }
            else if (_stricmp(key,"TrayEnabled")==0) { g_trayEnabled = atoi(val)!=0; }
            else if (_stricmp(key,"EnableLog")==0) { g_logEnabled = atoi(val)!=0; }
            continue;
        }

        if (legacyMode && (curSection == "AIMP" || curSection == "Keys")) {
            if (curSection == "AIMP") {
                if      (_stricmp(key,"TargetExe")==0)   legacyProf->targetExe = val;
                else if (_stricmp(key,"TargetClass")==0) legacyProf->targetClass = val;
                else if (_stricmp(key,"TargetPath")==0)  legacyProf->targetPath = val;
                else if (_stricmp(key,"AutoStart")==0)   legacyProf->autoStart = atoi(val)!=0;
                else if (_stricmp(key,"EnableLog")==0)   g_logEnabled = atoi(val)!=0;
            } else { // Keys
                int uid; unsigned char mod;
                if (ParseKeySpec(key, uid, mod) && val[0]) {
                    KeyMapping km; km.usageId = uid; km.modMask = mod; km.action = val;
                    legacyProf->keys.push_back(km);
                }
            }
            continue;
        }

        // Новый формат
        if (!curProfileName.empty()) {
            Profile* prof = &g_profiles[curProfileName];
            bool isKeysSection = (curSection.find(".Keys") != std::string::npos);
            if (isKeysSection) {
                int uid; unsigned char mod;
                if (ParseKeySpec(key, uid, mod) && val[0]) {
                    KeyMapping km; km.usageId = uid; km.modMask = mod; km.action = val;
                    prof->keys.push_back(km);
                }
            } else {
                // [Profile.NAME]
                if      (_stricmp(key,"Mode")==0)        prof->mode = (_stricmp(val,"basic")==0) ? MODE_BASIC : MODE_TARGETED;
                else if (_stricmp(key,"TargetClass")==0) prof->targetClass = val;
                else if (_stricmp(key,"TargetExe")==0)   prof->targetExe = val;
                else if (_stricmp(key,"TargetPath")==0)  prof->targetPath = val;
                else if (_stricmp(key,"AutoStart")==0)   prof->autoStart = atoi(val)!=0;
            }
        }
    }
    fclose(f);

    // если legacy — переопределить default-профиль как активный, если DefaultProfile не задан явно
    if (legacyMode && _stricmp(g_defaultProfile, "basic") == 0) {
        strncpy(g_defaultProfile, "default", sizeof(g_defaultProfile)-1);
 g_defaultProfile[sizeof(g_defaultProfile)-1] = 0;
    }
    g_activeProfile = g_defaultProfile;
    Log("Config loaded (legacy): %zu profiles, default=%s, http=%d tray=%d",
        g_profiles.size(), g_defaultProfile, (int)g_httpEnabled, (int)g_trayEnabled);
}

// =====================================================================
//  ApplyGeneralSettings — скопировать config_v3 GeneralSettings в globals
//  (g_deviceVidPid, g_httpPort и т.д.), которые использует остальной код.
// =====================================================================
static void ApplyGeneralSettings(const keysidekick::config::GeneralSettings& gs) {
    strncpy(g_deviceVidPid, gs.device_vid_pid.c_str(), sizeof(g_deviceVidPid)-1);
    g_deviceVidPid[sizeof(g_deviceVidPid)-1] = 0;
    // DefaultProfile: config хранит id, runtime использует name. Bridge маппит
    // profile.normal → "basic". Оставляем как есть (g_activeProfile выставит projection).
    strncpy(g_defaultProfile, gs.default_profile_id.c_str(), sizeof(g_defaultProfile)-1);
 g_defaultProfile[sizeof(g_defaultProfile)-1] = 0;
    g_defaultProfile[sizeof(g_defaultProfile)-1] = 0;
    if (gs.default_profile_id == keysidekick::Profile::normalId()) {
        strncpy(g_defaultProfile, "basic", sizeof(g_defaultProfile)-1);
 g_defaultProfile[sizeof(g_defaultProfile)-1] = 0;
    }
    g_httpPort = gs.http_port;
    g_httpEnabled = gs.http_enabled;
    g_trayEnabled = gs.tray_enabled;
    g_logEnabled = gs.enable_log;
}

// =====================================================================
//  LoadConfig (Phase 2) — config_v3 Parse → bridge → g_domain → projection.
//  Safety gate: если config_v3 вернул ERROR diagnostic → fallback на legacy.
// =====================================================================
static void LoadConfig() {
    // 1. Прочитать файл через runtime_storage
    std::wstring wpath;
    int wlen = MultiByteToWideChar(CP_ACP, 0, CONFIG_FILE, -1, NULL, 0);
    wpath.resize(wlen);
    MultiByteToWideChar(CP_ACP, 0, CONFIG_FILE, -1, &wpath[0], wlen);
    if (!wpath.empty() && wpath.back() == 0) wpath.pop_back();

    std::string fileContent;
    keysidekick::StorageResult readResult = keysidekick::ReadUtf8File(wpath, &fileContent);
    if (!readResult.ok()) {
        Log("LoadConfig: cannot read %s (%s) — using defaults", CONFIG_FILE, readResult.message.c_str());
        EnsureBuiltinBasic();
        return;
    }

    // 2. Парсить через config_v3
    keysidekick::config::ParseResult pr = keysidekick::config::Parse(fileContent);
    bool hasErrors = !pr.ok();
    if (hasErrors) {
        Log("LoadConfig: config_v3 reported %zu error(s), falling back to legacy parser",
            pr.diagnostics.size());
        for (std::size_t i = 0; i < pr.diagnostics.size() && i < 5; ++i) {
            if (pr.diagnostics[i].severity == keysidekick::config::DIAGNOSTIC_ERROR) {
                Log("  ERROR: %s", pr.diagnostics[i].message.c_str());
            }
        }
        EnsureBuiltinBasic();
        LoadConfigLegacy();
        return;
    }

    // Логировать warnings (миграция и т.д.)
    for (std::size_t i = 0; i < pr.diagnostics.size(); ++i) {
        if (pr.diagnostics[i].severity == keysidekick::config::DIAGNOSTIC_WARNING) {
            Log("  config warn: %s", pr.diagnostics[i].message.c_str());
        }
    }

    // 3. Сохранить general settings + конвертировать через bridge
    g_configGeneral = pr.config.general;
    g_domain = keysidekick::bridge::ConfigToDomain(pr.config);

    // 4. Применить general settings к globals
    ApplyGeneralSettings(g_configGeneral);

    // 5. Спроецировать domain → runtime g_profiles
    ProjectDomainToRuntime();

    Log("Config loaded (v3+bridge): %zu profiles, %zu apps, active=%s, http=%d tray=%d",
        g_domain.profiles.size(), g_domain.applications.size(),
        g_activeProfile.c_str(), (int)g_httpEnabled, (int)g_trayEnabled);
}

// =====================================================================
//  ReloadConfig — перечитать config.ini с диска, сохранив активный профиль.
//  Безопасно вызывать с main thread (под локом мутации не делаются — LoadConfig
//  пишет в g_profiles, поэтому берём g_csProfile на время всего reload).
//  В отличие от LoadConfig, НЕ сбрасывает g_activeProfile безусловно.
// =====================================================================
static void ReloadConfig() {
    ReleaseAllTargetedKeys();
    ReleaseAllKeys();
    EnterCriticalSection(&g_csProfile);
    std::string prevActive = g_activeProfile;   // сохранить

    g_profiles.clear();
    EnsureBuiltinBasic();
    LoadConfig();   // перепарсит INI (сбросит g_activeProfile = g_defaultProfile)

    // восстановить active, если профиль ещё существует; иначе оставить как LoadConfig поставил
    if (g_profiles.find(prevActive) != g_profiles.end()) {
        g_activeProfile = prevActive;
    }
    Log("Reloaded config: %zu profiles, active=%s", g_profiles.size(), g_activeProfile.c_str());
    LeaveCriticalSection(&g_csProfile);
}

// =====================================================================
//  WriteConfig — сериализовать g_profiles обратно в config.ini.
//  Комментарии НЕ сохраняются (переписывается полностью — принято решение).
//  Вызывать с main thread. g_csProfile брать вызывающий (для консистентности
//  с тем, что пишем — берём лок снаружи в handler'е).
// =====================================================================
// Суффикс модификаторов для ключа (обратный ParseModSuffix).
static std::string ModSuffix(unsigned char mask) {
    std::string s;
    if ((mask & 0x33) == 0x33) s += "+Ctrl";        // оба Ctrl = общий Ctrl
    else if (mask & 0x01) s += "+LCtrl";
    else if (mask & 0x10) s += "+RCtrl";
    if ((mask & 0x22) == 0x22) s += "+Shift";
    else if (mask & 0x02) s += "+LShift";
    else if (mask & 0x20) s += "+RShift";
    if ((mask & 0x44) == 0x44) s += "+Alt";
    else if (mask & 0x04) s += "+LAlt";
    else if (mask & 0x40) s += "+RAlt";
    if ((mask & 0x88) == 0x88) s += "+Win";
    else if (mask & 0x08) s += "+LWin";
    else if (mask & 0x80) s += "+RWin";
    return s;
}

// Build config content into a string buffer (separated from writing
// so the same code works with atomic file write).
static std::string BuildConfigContent() {
    std::string s;
    s.reserve(2048);

    // [General]
    s += "[General]\n";
    char line[512];
    snprintf(line, sizeof(line), "DeviceVIDPID=%s\n", g_deviceVidPid); s += line;
    snprintf(line, sizeof(line), "DefaultProfile=%s\n", g_defaultProfile); s += line;
    snprintf(line, sizeof(line), "HTTPPort=%d\n", g_httpPort); s += line;
    snprintf(line, sizeof(line), "HTTPEnabled=%d\n", g_httpEnabled ? 1 : 0); s += line;
    snprintf(line, sizeof(line), "TrayEnabled=%d\n", g_trayEnabled ? 1 : 0); s += line;
    snprintf(line, sizeof(line), "EnableLog=%d\n\n", g_logEnabled ? 1 : 0); s += line;

    // Профили
    for (auto& kv : g_profiles) {
        const Profile& p = kv.second;
        if (p.isBuiltinBasic && p.keys.empty()) continue;

        snprintf(line, sizeof(line), "[Profile.%s]\n", p.name.c_str()); s += line;
        snprintf(line, sizeof(line), "Mode=%s\n", (p.mode == MODE_BASIC) ? "basic" : "targeted"); s += line;
        if (!p.isBuiltinBasic) {
            snprintf(line, sizeof(line), "TargetClass=%s\n", p.targetClass.c_str()); s += line;
            snprintf(line, sizeof(line), "TargetExe=%s\n", p.targetExe.c_str()); s += line;
            snprintf(line, sizeof(line), "TargetPath=%s\n", p.targetPath.c_str()); s += line;
            snprintf(line, sizeof(line), "AutoStart=%d\n", p.autoStart ? 1 : 0); s += line;
        }
        s += "\n";

        if (!p.keys.empty()) {
            snprintf(line, sizeof(line), "[Profile.%s.Keys]\n", p.name.c_str()); s += line;
            for (auto& m : p.keys) {
                char keyBuf[64];
                snprintf(keyBuf, sizeof(keyBuf), "USAGE_%02X", m.usageId);
                std::string keyStr = keyBuf + ModSuffix(m.modMask);
                s += keyStr;
                s += "=";
                s += m.action;
                s += "\n";
            }
            s += "\n";
        }
    }
    return s;
}

// =====================================================================
//  SyncRuntimeToDomain — перенести мутации из g_profiles обратно в g_domain.
//  Вызывать ПОД g_csProfile перед WriteConfig.
//  Сохраняет multi-app links и applications из g_domain (они не меняются через DashOp).
//  Обновляет: mappings, target fields (из default app), mode, isBuiltIn.
// =====================================================================
static void SyncRuntimeToDomain() {
    // Обновить g_configGeneral из globals (HTTP/tray/etc могли измениться)
    g_configGeneral.device_vid_pid = g_deviceVidPid;
    g_configGeneral.http_port = g_httpPort;
    g_configGeneral.http_enabled = g_httpEnabled;
    g_configGeneral.tray_enabled = g_trayEnabled;
    g_configGeneral.enable_log = g_logEnabled;
    g_configGeneral.default_profile_id = g_activeProfile;

    for (auto& kv : g_profiles) {
        const std::string& runtimeName = kv.first;
        const Profile& rp = kv.second;

        // Найти соответствующий domain profile по имени.
        // Runtime "basic" маппится на canonical Normal profile (name="Normal").
        keysidekick::Profile* dp = 0;
        for (std::size_t i = 0; i < g_domain.profiles.size(); ++i) {
            if (g_domain.profiles[i].name == runtimeName) {
                dp = &g_domain.profiles[i];
                break;
            }
            // "basic" в runtime → Normal в domain (canonical built-in)
            if (runtimeName == "basic" &&
                g_domain.profiles[i].id() == keysidekick::Profile::normalId()) {
                dp = &g_domain.profiles[i];
                break;
            }
        }
        if (!dp) {
            // Новый профиль, созданный через dashboard — добавить в domain
            std::string newId = runtimeName;
            // Убедиться что id безопасен для config_v3 (alnum/-/_)
            for (char& c : newId) {
                if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) c = '_';
            }
            g_domain.profiles.push_back(
                keysidekick::Profile(newId, runtimeName,
                    (rp.mode == MODE_TARGETED) ? keysidekick::ProfileMode::Targeted
                                               : keysidekick::ProfileMode::Normal));
            dp = &g_domain.profiles.back();
        }

        dp->mode = (rp.mode == MODE_TARGETED) ? keysidekick::ProfileMode::Targeted
                                               : keysidekick::ProfileMode::Normal;
        dp->layerModifier = rp.layerModName;

        // Target fields → обновить default ApplicationTarget
        if (!rp.targetClass.empty() || !rp.targetExe.empty() || !rp.targetPath.empty()) {
            std::string appId = dp->defaultApplicationId;
            if (appId.empty() && !dp->linkedApplicationIds.empty())
                appId = dp->linkedApplicationIds.front();

            keysidekick::ApplicationTarget* app = appId.empty() ? 0 : g_domain.findApplication(appId);
            if (!app) {
                // Создать новый application для этого target
                appId = std::string("app-") + runtimeName;
                //sanitize id
                std::string safeAppId = appId;
                for (char& c : safeAppId) {
                    if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) c = '_';
                }
                g_domain.applications.push_back(keysidekick::ApplicationTarget(safeAppId, runtimeName));
                app = &g_domain.applications.back();
                dp->linkedApplicationIds.push_back(app->id());
                dp->defaultApplicationId = app->id();
            }
            app->windowClass = rp.targetClass;
            app->processName = rp.targetExe;
            app->exePath = rp.targetPath;
        }

        // Mappings: полностью перестроить из g_profiles
        dp->mappings.clear();
        for (std::size_t m = 0; m < rp.keys.size(); ++m) {
            const KeyMapping& km = rp.keys[m];
            std::string mapId = runtimeName + "." + std::to_string(km.usageId) + "." + std::to_string(km.modMask);
            unsigned int domainMod = keysidekick::bridge::ExpandConfigModifierToDomain(km.modMask);
            keysidekick::Trigger trigger((unsigned int)(km.usageId & 0xFF), domainMod);
            keysidekick::Action action = keysidekick::Action::sendKey((unsigned int)(km.usageId & 0xFF));
            action.profileId = km.action;  // pass-through carrier
            dp->mappings.push_back(keysidekick::Mapping(mapId, (int)m, trigger,
                                                        action, keysidekick::Destination::defaultApplication()));
        }
    }

    // Обновить active profile в domain
    g_domain.activeProfileId = g_activeProfile;
    // Если active = "basic", маппим на Normal
    if (g_activeProfile == "basic") {
        const keysidekick::Profile* normal = g_domain.findProfile(keysidekick::Profile::normalId());
        if (normal) g_domain.activeProfileId = normal->id();
    }
}

static bool WriteConfig() {
    // H3: весь проход под g_csProfile (reentrant) — с пулом HTTP-воркеров
    // WriteConfig может вызываться конкурентно, а g_domain читается без лока.
    EnterCriticalSection(&g_csProfile);
    // Sync runtime mutations → domain, then serialize through config_v3
    SyncRuntimeToDomain();
    keysidekick::config::Config config = keysidekick::bridge::DomainToConfig(g_domain, g_configGeneral);

    keysidekick::config::SerializeResult sr = keysidekick::config::Serialize(config);
    if (!sr.ok()) {
        Log("WriteConfig: Serialize failed (%zu diagnostics) — falling back to legacy writer",
            sr.diagnostics.size());
        for (std::size_t i = 0; i < sr.diagnostics.size() && i < 5; ++i) {
            Log("  SER err: %s", sr.diagnostics[i].message.c_str());
        }
        // Fallback: legacy writer (BuildConfigContent)
        std::string legacyContent = BuildConfigContent();
        std::wstring wpath;
        int wlen = MultiByteToWideChar(CP_ACP, 0, CONFIG_FILE, -1, NULL, 0);
        wpath.resize(wlen);
        MultiByteToWideChar(CP_ACP, 0, CONFIG_FILE, -1, &wpath[0], wlen);
        if (!wpath.empty() && wpath.back() == 0) wpath.pop_back();
        keysidekick::StorageResult result = keysidekick::AtomicWriteUtf8(wpath, legacyContent, nullptr);
        if (!result.ok()) {
            Log("WriteConfig legacy fallback FAILED: stage=%d err=%lu", (int)result.stage, result.win32Error);
            LeaveCriticalSection(&g_csProfile);
            return false;
        }
        Log("Config written (legacy fallback) to %s", CONFIG_FILE);
        LeaveCriticalSection(&g_csProfile);
        return true;
    }

    std::string content = sr.text;

    // Atomic write via runtime_storage: temp → write → flush → validate → replace + backup
    std::wstring wpath;
    int wlen = MultiByteToWideChar(CP_ACP, 0, CONFIG_FILE, -1, NULL, 0);
    wpath.resize(wlen);
    MultiByteToWideChar(CP_ACP, 0, CONFIG_FILE, -1, &wpath[0], wlen);
    // trim trailing null that MultiByteToWideChar adds
    if (!wpath.empty() && wpath.back() == 0) wpath.pop_back();

    keysidekick::StorageResult result = keysidekick::AtomicWriteUtf8(
        wpath, content,
        [](const std::string& c, std::string* reason) -> bool {
            if (c.empty()) { if (reason) *reason = "config is empty"; return false; }
            return true;
        });

    if (!result.ok()) {
        Log("WriteConfig FAILED: stage=%d err=%lu msg=%s",
            (int)result.stage, result.win32Error, result.message.c_str());
        LeaveCriticalSection(&g_csProfile);
        return false;
    }
    Log("Config written atomically (v3) to %s", CONFIG_FILE);
    LeaveCriticalSection(&g_csProfile);
    return true;
}

// =====================================================================
//  Read loop (overlapped + pump сообщений для power/tray/http)
// =====================================================================
static void ReadLoop() {
    BYTE buf[8] = {0};
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    bool readPending = false;

    while (g_running) {
        if (!readPending) {
            ResetEvent(ov.hEvent);
            ULONG got = 0;
            BOOL ok = WinUsb_ReadPipe(g_hWinUsb, g_interruptInPipe, buf, sizeof(buf), &got, &ov);
            if (ok) { if (got>0) ProcessReport(buf, got); continue; }
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                Log("ReadPipe err=%lu — reconnect", err);
                // Если реконнект не удался (устройство физически пропало) —
                // выходим в main-цикл: там событийное ожидание WM_DEVICECHANGE,
                // никакого спина с поллингом SetupAPI.
                if (!ReconnectDevice()) break;
                continue;
            }
            readPending = true;
        }
        HANDLE waitHandles[2] = {
            ov.hEvent,
            g_reconnectRequestEvent ? g_reconnectRequestEvent : ov.hEvent
        };
        DWORD wr = MsgWaitForMultipleObjectsEx(2, waitHandles, INFINITE,
            QS_ALLINPUT | QS_ALLPOSTMESSAGE, MWMO_INPUTAVAILABLE);
        if (wr == WAIT_OBJECT_0) {
            ULONG got = 0;
            if (WinUsb_GetOverlappedResult(g_hWinUsb, &ov, &got, FALSE)) {
                if (got>0) ProcessReport(buf, got);
            } else {
                DWORD e2 = GetLastError();
                if (e2 != ERROR_OPERATION_ABORTED) Log("GetOverlappedResult err=%lu", e2);
                if (g_powerResume) {
                    g_powerResume = false;
                    Log("Reconnect after resume");
                    CloseDevice();
                    if (!ReconnectDevice()) Sleep(2000);
                }
            }
            readPending = false;
        } else if (wr == WAIT_OBJECT_0 + 1) {
            // C2: активация запросила реконнект устройства (DASH_ACTIVATE_DEVICE,
            // wizard "Make active"). DashOp лишь выставил флаг+событие; здесь, в
            // потоке-владельце overlapped read, делаем безопасный teardown:
            // AbortPipe СНАЧАЛА (MSDN: отменить outstanding I/O до WinUsb_Free),
            // дождаться завершения read, потом close → reopen по новому VID/PID.
            // Оп уже завершён асинхронно; dashboard видит результат через polling.
            g_pendingReconnect = false;
            if (readPending) {
                if (g_hWinUsb && g_interruptInPipe != 0xFF)
                    WinUsb_AbortPipe(g_hWinUsb, g_interruptInPipe);
                WaitForSingleObject(ov.hEvent, 1000);
                readPending = false;
            }
            if (g_hWinUsb) {
                Log("Reconnect requested (activation)");
                CloseDevice();
            }
            if (!ReconnectDevice()) {
                Log("Reconnect (activation) failed — device gone, switching to event-driven wait");
                BumpRevision();   // SSE clients: device state изменился
                break;
            }
            BumpRevision();   // SSE clients: device state изменился
        } else if (wr == WAIT_OBJECT_0 + 2) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    if (readPending) { WinUsb_AbortPipe(g_hWinUsb, g_interruptInPipe); WaitForSingleObject(ov.hEvent, 1000); }
    CloseHandle(ov.hEvent);
}

// =====================================================================
//  Entry
// =====================================================================

// Release only keys that KeySidekick actually injected (ownership ledger).
// This prevents releasing Ctrl/Shift held by the main keyboard.
// For each owned key, send KEYUP with both vk and scan (reliable release).
static void ReleaseAllKeys() {
    // Map scan+extended → vk for reliable release
    static const struct { unsigned short scan; bool extended; unsigned short vk; } SCAN_TO_VK[] = {
        {0x2A, false, VK_LSHIFT},
        {0x36, false, VK_RSHIFT},
        {0x1D, false, VK_LCONTROL},
        {0x1D, true,  VK_RCONTROL},
        {0x38, false, VK_LMENU},
        {0x38, true,  VK_RMENU},
        {0x5B, true,  VK_LWIN},
        {0x5C, true,  VK_RWIN},
    };

    std::vector<keysidekick::ScanKey> owned = g_injectedKeys.ownedKeysForRelease();
    int released = 0;
    for (const auto& k : owned) {
        unsigned short vk = 0;
        for (const auto& m : SCAN_TO_VK) {
            if (m.scan == k.scan && m.extended == k.extended) { vk = m.vk; break; }
        }
        INPUT in = {0};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = k.scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        if (k.extended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        SendInput(1, &in, sizeof(INPUT));
        g_injectedKeys.recordUp(k);
        released++;
    }
    if (released > 0) Log("Released %d injected keys (ownership-based)", released);
}

static BOOL WINAPI ConsoleHandler(DWORD signal) {
    // Console-control поток НЕ трогает WinUSB: только флаг + сообщение.
    // AbortPipe выполнит main-поток в обработчике WM_USER_SHUTDOWN (владелец
    // overlapped read) — иначе гонка с CloseDevice/WinUsb_Free (M3).
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running = false;
        if (g_hMsgWindow) PostMessageW(g_hMsgWindow, WM_USER_SHUTDOWN, 0, 0);
    }
    return TRUE;
}

// Port-change: настроенный VID/PID найден как обычная (не-WinUSB) клавиатура?
static bool FindPortChangedKeyboard(std::string& vidpidOut, std::string& usbIdOut) {
    if (!g_deviceVidPid[0]) return false;
    // паттерны из g_deviceVidPid: "vid_xxxx&pid_yyyy[, ...]"
    std::vector<std::string> patterns;
    {
        std::string s(g_deviceVidPid);
        std::size_t pos = 0;
        while (pos <= s.size()) {
            std::size_t comma = s.find(',', pos);
            std::string p = (comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
            while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(0, 1);
            while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
            if (!p.empty()) patterns.push_back(ToLower(p.c_str()));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    if (patterns.empty()) return false;
    std::vector<InputDeviceRow> rows;
    EnumerateInputDevices(rows);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const InputDeviceRow& r = rows[i];
        if (r.winusb) continue;
        if (r.kinds.find("keyboard") == std::string::npos) continue;
        if (r.vid.empty() || r.pid.empty()) continue;
        std::string vp = ToLower(("vid_" + r.vid + "&pid_" + r.pid).c_str());
        for (std::size_t k = 0; k < patterns.size(); ++k) {
            if (vp == patterns[k]) {
                vidpidOut = vp;
                usbIdOut = r.usbId;
                return true;
            }
        }
    }
    return false;
}

// =====================================================================
//  Driver helper mode: sidekick.exe --driver swap|restore|status <vidpid>
//  Выполняется ДО singleton/сети: это отдельный CLI-режим того же exe.
//  Админ-права нужны только для swap/restore — самоэлевация (UAC) ровно
//  в этот момент; само приложение всегда работает без админки.
//  Свап: inbox winusb.inf (подписан Microsoft, Win10 1809+), без Zadig.
//  Restore: inbox input.inf — клавиатура снова обычная.
// =====================================================================
struct DrvNode {
    std::wstring instanceId;
    std::wstring hardwareId;   // USB\VID_xxxx&PID_yyyy&MI_xx
    std::wstring service;
};

static std::wstring DrvGetProp(HDEVINFO hDev, PSP_DEVINFO_DATA d, DWORD prop) {
    wchar_t buf[512] = {0};
    DWORD type = 0, needed = 0;
    if (SetupDiGetDeviceRegistryPropertyW(hDev, d, prop, &type, (PBYTE)buf, sizeof(buf), &needed)) {
        return std::wstring(buf);
    }
    return std::wstring();
}

static std::wstring DrvLowerW(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

// Все present-узлы, чей hardware ID начинается с USB\VID_xxxx&PID_yyyy.
static bool DrvFindNodes(const std::string& vidpid, std::vector<DrvNode>& nodes) {
    if (vidpid.empty()) return false;
    std::wstring pat;
    {
        int len = MultiByteToWideChar(CP_UTF8, 0, vidpid.c_str(), (int)vidpid.size(), NULL, 0);
        if (len <= 0) return false;
        pat.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, vidpid.c_str(), (int)vidpid.size(), &pat[0], len);
    }
    std::wstring hex;
    for (auto c : pat) if (iswxdigit(c)) hex += towlower(c);
    if (hex.size() < 8) return false;
    wchar_t vp[64] = {0};
    swprintf(vp, 64, L"USB\\VID_%c%c%c%c&PID_%c%c%c%c",
             hex[0], hex[1], hex[2], hex[3], hex[4], hex[5], hex[6], hex[7]);
    std::wstring needle = vp;

    HDEVINFO hDev = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDev == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA dd = { sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDev, i, &dd); ++i) {
        std::wstring hwid = DrvGetProp(hDev, &dd, SPDRP_HARDWAREID);
        if (DrvLowerW(hwid).find(needle) == std::wstring::npos) continue;
        DrvNode n;
        wchar_t iid[256] = {0};
        SetupDiGetDeviceInstanceIdW(hDev, &dd, iid, 256, NULL);
        n.instanceId = iid;
        n.service = DrvGetProp(hDev, &dd, SPDRP_SERVICE);
        n.hardwareId = hwid;
        nodes.push_back(n);
    }
    SetupDiDestroyDeviceInfoList(hDev);
    return !nodes.empty();
}

static DrvNode* DrvPickForSwap(std::vector<DrvNode>& nodes) {
    for (auto& n : nodes)
        if (DrvLowerW(n.hardwareId).find(L"&mi_00") != std::wstring::npos) return &n;
    return nodes.empty() ? NULL : &nodes[0];
}

static DrvNode* DrvPickForRestore(std::vector<DrvNode>& nodes) {
    for (auto& n : nodes)
        if (DrvLowerW(n.service) == L"winusb") return &n;
    return nodes.empty() ? NULL : &nodes[0];
}

static int DrvDoBind(const std::string& vidpid, bool restore) {
    std::vector<DrvNode> nodes;
    if (!DrvFindNodes(vidpid, nodes)) {
        printf("No device nodes match %s — is the keyboard plugged in?\n", vidpid.c_str());
        printf("(Nodes are per-port: after a port change run swap again for the new port.)\n");
        return 2;
    }
    DrvNode* n = restore ? DrvPickForRestore(nodes) : DrvPickForSwap(nodes);
    const wchar_t* inf = restore ? L"C:\\Windows\\INF\\input.inf" : L"C:\\Windows\\INF\\winusb.inf";
    printf("%s %s to: %ls\n", restore ? "Restoring inbox HID driver (input.inf)" : "Binding WinUSB (inbox winusb.inf)",
           restore ? "for" : "to", n->hardwareId.c_str());
    BOOL reboot = FALSE;
    BOOL ok = UpdateDriverForPlugAndPlayDevicesW(NULL, (LPWSTR)n->hardwareId.c_str(),
                (LPWSTR)inf, INSTALLFLAG_FORCE, &reboot);
    if (!ok) {
        DWORD err = GetLastError();
        printf("FAILED: UpdateDriverForPlugAndPlayDevicesW error %lu\n", err);
        if (err == 5) printf("Run as administrator (UAC prompt should appear automatically).\n");
        return 1;
    }
    printf("OK.%s\n", reboot ? " A reboot is recommended." : "");
    if (restore) printf("The keyboard is an ordinary keyboard again.\n");
    else printf("KeySidekick picks the keyboard up automatically; profiles are kept per VID/PID.\n");
    return 0;
}

static int DriverCliMain(int argc, char* argv[], int startIdx) {
    SetConsoleOutputCP(CP_UTF8);
    if (startIdx >= argc) {
        printf("Usage: sidekick.exe --driver swap|restore|status vid_xxxx&pid_yyyy\n");
        printf("Windows 10 1809+ (inbox signed winusb.inf — no Zadig).\n");
        return 1;
    }
    std::string cmd = argv[startIdx];
    std::string vidpid = (startIdx + 1 < argc) ? argv[startIdx + 1] : "";

    if (cmd == "status") {
        std::vector<DrvNode> nodes;
        if (!DrvFindNodes(vidpid, nodes)) {
            printf("No present device nodes match %s\n", vidpid.c_str());
            return 2;
        }
        printf("Matching nodes for %s:\n", vidpid.c_str());
        for (auto& n : nodes) {
            printf("  - %ls\n", n.instanceId.c_str());
            printf("      hardwareId: %ls\n", n.hardwareId.c_str());
            printf("      service:    %ls\n", n.service.empty() ? L"(none)" : n.service.c_str());
        }
        return 0;
    }

    if (cmd == "swap" || cmd == "restore") {
        if (vidpid.empty()) {
            printf("Missing vidpid (vid_xxxx&pid_yyyy).\n");
            return 1;
        }
        // Самоэлевация только для мутирующих команд — UAC ровно в момент свапа.
        if (!IsUserAnAdmin()) {
            wchar_t self[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, self, MAX_PATH);
            char args[1024];
            snprintf(args, sizeof(args), "--driver %s %s", cmd.c_str(), vidpid.c_str());
            wchar_t wargs[1024] = {0};
            MultiByteToWideChar(CP_ACP, 0, args, -1, wargs, 1024);
            HINSTANCE h = ShellExecuteW(NULL, L"runas", self, wargs, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)h <= 32) {
                printf("Elevation cancelled or failed (ShellExecute error %d).\n", (int)(INT_PTR)h);
                return 1;
            }
            return 0;   // elevated copy prints its own result
        }
        return DrvDoBind(vidpid, cmd == "restore");
    }

    printf("Unknown driver command: %s\n", cmd.c_str());
    return 1;
}

int main(int argc, char* argv[]) {
    // --- Version flag: no side effects; works even if another instance runs ---
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("KeySidekick %s\n", APP_VERSION);
            return 0;
        }
    }
    // --- Driver helper mode (before singleton/network): --driver swap|restore|status ---
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--driver") == 0) {
            return DriverCliMain(argc, argv, i + 1);
        }
    }
    // --- Resolve config/log paths: exe dir → CWD fallback ---
    ResolveDataPaths();
    // --- Singleton: only one KeySidekick per user session ---
    keysidekick::AppInstance appInstance(L"KeySidekick", L"main");
    keysidekick::AppInstanceAcquireResult acquire = appInstance.Acquire();
    if (acquire.status == keysidekick::AppInstanceAcquireStatus::AlreadyRunning) {
        // Ask the existing instance to open its dashboard
        appInstance.RequestOpenDashboard();
        return 0;
    }
    g_openDashboardMsg = appInstance.window_message();
    if (acquire.status == keysidekick::AppInstanceAcquireStatus::Error) {
        fprintf(stderr, "Singleton init failed (err=%lu). Another instance may be running.\n", acquire.win32_error);
        return 1;
    }

    InitializeCriticalSection(&g_csProfile);
    InitializeCriticalSection(&g_csRevision);
    InitializeCriticalSection(&g_csIdentified);
    InitializeCriticalSection(&g_csActivity);
    InitializeCriticalSection(&g_csDevInfo);
    InitializeCriticalSection(&g_csLog);
    g_csLogReady = true;
    // C2: событие, через которое DashOp-активация просит ReadLoop переподключить
    // устройство (создаётся до StartHttp — DashOp не может прийти раньше).
    g_reconnectRequestEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    // Phase 4: Generate CSRF token at startup (embedded into dashboard HTML)
    g_csrfToken = keysidekick::GenerateSecurityToken();
    Log("CSRF token generated (%zu chars)", g_csrfToken.size());
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    EnsureBuiltinBasic();
    LoadConfig();
    CreateMsgWindow();
    if (g_trayEnabled) UpdateTray();
    StartHttp();
    StartAutoSwitchTimer();  // Auto-switch profiles based on foreground window

    printf("=== KeySidekick ===\n");
    printf("Device: %s | Profiles: %zu | Active: %s\n", g_deviceVidPid, g_profiles.size(), g_activeProfile.c_str());
    printf("HTTP: %s | Tray: %s\n", g_httpEnabled ? "on" : "off", g_trayEnabled ? "on" : "off");
    printf("Ctrl+C to exit.\n\n");
    Log("Started v2. profiles=%zu active=%s http=%d tray=%d",
        g_profiles.size(), g_activeProfile.c_str(), (int)g_httpEnabled, (int)g_trayEnabled);

    // --- Always-on device loop: if device is absent, keep dashboard/tray
    // alive and retry with backoff instead of exiting. ---
    // Idle-чистота: НИКАКОГО периодического поллинга. Цикл спит в
    // MsgWaitForMultipleObjectsEx и просыпается ТОЛЬКО по сообщению
    // (WM_DEVICECHANGE при plug/unplug, DashOp, tray) либо по экспоненциальному
    // backoff-таймауту (2с → 60с) как страховка от пропущенного события.
    static bool deviceAbsentLogged = false;
    static bool openFailLogged = false;
    while (g_running) {
        std::string path;
        if (!FindDevicePath(path)) {
            if (!deviceAbsentLogged) {
                Log("Device not found, waiting...");
                printf("Device not found. Dashboard at http://localhost:%d/ — waiting for device...\n", g_httpPort);
                deviceAbsentLogged = true;
            }
            // Port-change детект: настроенный VID/PID виден как ОБЫЧНАЯ клавиатура —
            // значит, клавиатуру переткнули в другой порт (узел без серийника
            // привязывается к порту, WinUSB слетел). Уведомляем дашборд/трей.
            {
                std::string pcVp, pcHw;
                bool pc = FindPortChangedKeyboard(pcVp, pcHw);
                bool prev = false;
                EnterCriticalSection(&g_csProfile);
                prev = g_portChangeDetected;
                g_portChangeDetected = pc;
                if (pc) { g_portChangeVidPid = pcVp; g_portChangeHwid = pcHw; }
                LeaveCriticalSection(&g_csProfile);
                if (pc && !prev) {
                    Log("Port change detected: %s is an ordinary keyboard now (new port). Run driver swap.", pcVp.c_str());
                    BumpRevision();   // дашборд увидит через SSE
                } else if (!pc && prev) {
                    g_portChangeDetected = false;
                    BumpRevision();
                }
            }
            DWORD backoffMs = 2000;
            while (g_running && path.empty()) {
                DWORD wr = MsgWaitForMultipleObjectsEx(0, NULL, backoffMs,
                    QS_ALLINPUT | QS_ALLPOSTMESSAGE, 0);
                if (wr == WAIT_OBJECT_0) {
                    PumpMessages();
                    if (g_deviceChangeNotified.exchange(false) && FindDevicePath(path)) break;
                    continue;
                }
                if (!g_running) break;
                if (FindDevicePath(path)) break;
                if (backoffMs < 60000) backoffMs *= 2;
            }
            if (!g_running) break;
            if (path.empty()) continue;
            deviceAbsentLogged = false;
            if (g_portChangeDetected) { g_portChangeDetected = false; BumpRevision(); }
        } else if (deviceAbsentLogged) {
            deviceAbsentLogged = false;
            if (g_portChangeDetected) { g_portChangeDetected = false; BumpRevision(); }
        }
        if (!OpenDevice(path)) {
            if (!openFailLogged) {
                Log("Failed to open device, retry in 5s");
                openFailLogged = true;
            }
            // Тоже не поллим: ждём события/backoff (устройство видно, но не
            // открывается — например, фантомная копия после Zadig).
            DWORD backoffMs = 2000;
            while (g_running) {
                DWORD wr = MsgWaitForMultipleObjectsEx(0, NULL, backoffMs,
                    QS_ALLINPUT | QS_ALLPOSTMESSAGE, 0);
                if (wr == WAIT_OBJECT_0) {
                    PumpMessages();
                    if (g_deviceChangeNotified.exchange(false)) break;
                    continue;
                }
                if (!g_running) break;
                if (backoffMs < 10000) backoffMs *= 2;
                break;   // backoff истёк — пробуем открыть снова
            }
            continue;
        }
        openFailLogged = false;
        // Сбрасываем resume-флаг, выставленный WM_POWERBROADCAST при отсутствии
        // устройства — иначе первый же read-error даст ложный reconnect.
        g_powerResume = false;
        // C2: если реконнект-активация была запрошена, пока устройство
        // отсутствовало, успешный OpenDevice выше её уже удовлетворил —
        // сбрасываем флаг/событие, чтобы ReadLoop не делал лишний teardown.
        if (g_pendingReconnect) {
            g_pendingReconnect = false;
            if (g_reconnectRequestEvent) ResetEvent(g_reconnectRequestEvent);
        }

        ReadLoop();
        CloseDevice();

        if (g_running) {
            Log("Device lost — switching to event-driven wait");
            PumpMessages();
        }
    }

    ReleaseAllTargetedKeys();
    ReleaseAllKeys();
    RemoveTray();
    // H3: аккуратная остановка HTTP — дождаться SSE-клиентов и воркеров,
    // join accept-потока, и ТОЛЬКО ПОТОМ удалять критические секции (M4).
    g_httpRunning = false;
    for (int i = 0; i < 30 && g_sseClientCount > 0; ++i) Sleep(100);
    for (int i = 0; i < 30 && g_httpWorkersActive.load() > 0; ++i) Sleep(100);
    if (g_httpThreadHandle) {
        WaitForSingleObject(g_httpThreadHandle, 3000);
        CloseHandle(g_httpThreadHandle);
        g_httpThreadHandle = NULL;
    }
    if (g_httpWorkerSemaphore) {
        CloseHandle(g_httpWorkerSemaphore);
        g_httpWorkerSemaphore = NULL;
    }
    Log("Stopped");
    DeleteCriticalSection(&g_csRevision);
    DeleteCriticalSection(&g_csProfile);
    DeleteCriticalSection(&g_csIdentified);
    DeleteCriticalSection(&g_csActivity);
    DeleteCriticalSection(&g_csDevInfo);
    // g_csLog НЕ удаляем: отставший HTTP-воркер (DashOp-таймаут до 15с) может
    // вызвать Log() уже после drain'а — EnterCriticalSection на удалённой
    // секции это UB. Утечка одной секции при выходе процесса безвредна.
    return 0;
}
