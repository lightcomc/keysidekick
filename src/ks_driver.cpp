// ks_driver.cpp — KeySidekick driver helper (portable, no sidekick.exe needed)
//
// Binds/unbinds the WinUSB driver for a specific USB keyboard using Windows'
// INBOX signed INFs — no Zadig, no self-signed certificates, no downloads.
//
//   ks_driver.exe status  vid_0406&pid_2814   — list matching device nodes + driver
//   ks_driver.exe swap    vid_0406&pid_2814   — bind winusb.inf to the keyboard (MI_00)
//   ks_driver.exe restore vid_0406&pid_2814   — bind input.inf back (normal keyboard)
//
// Requires Windows 10 1809+ (inbox C:\Windows\INF\winusb.inf). Driver install
// needs admin rights; the tool self-elevates via a UAC prompt when needed.
// Exit codes: 0 ok, 1 error, 2 no matching device found.
//
// License: GPL-3.0 (see LICENSE in the repo root).

#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string>
#include <vector>

struct Node {
    std::wstring instanceId;
    std::wstring hardwareId;   // USB\VID_xxxx&PID_yyyy&MI_xx
    std::wstring service;      // WinUSB / HidUsb / kbdhid / ...
};

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}

static std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

static std::wstring GetProp(HDEVINFO hDev, PSP_DEVINFO_DATA d, DWORD prop) {
    wchar_t buf[512] = {0};
    DWORD type = 0, needed = 0;
    if (SetupDiGetDeviceRegistryPropertyW(hDev, d, prop, &type, (PBYTE)buf, sizeof(buf), &needed)) {
        return std::wstring(buf);
    }
    return std::wstring();
}

// Find all present device nodes whose hardware ID starts with USB\VID_xxxx&PID_yyyy
// (case-insensitive, vidpid like "vid_0406&pid_2814").
static bool FindNodes(const std::string& vidpid, std::vector<Node>& nodes) {
    // Parse "vid_xxxx&pid_yyyy" (case-insensitive) into USB\VID_xxxx&PID_yyyy
    std::wstring pat = Utf8ToWide(vidpid);
    if (pat.empty()) return false;
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
        std::wstring hwid = GetProp(hDev, &dd, SPDRP_HARDWAREID);
        std::wstring l = LowerW(hwid);
        if (l.find(needle) == std::wstring::npos) continue;
        Node n;
        wchar_t iid[256] = {0};
        SetupDiGetDeviceInstanceIdW(hDev, &dd, iid, 256, NULL);
        n.instanceId = iid;
        n.service = GetProp(hDev, &dd, SPDRP_SERVICE);
        n.hardwareId = hwid;
        nodes.push_back(n);
    }
    SetupDiDestroyDeviceInfoList(hDev);
    return !nodes.empty();
}

// Prefer the keyboard interface node (MI_00); fall back to the first WinUSB node.
static Node* PickForSwap(std::vector<Node>& nodes) {
    for (auto& n : nodes)
        if (LowerW(n.hardwareId).find(L"&mi_00") != std::wstring::npos) return &n;
    return nodes.empty() ? NULL : &nodes[0];
}

static Node* PickForRestore(std::vector<Node>& nodes) {
    for (auto& n : nodes)
        if (LowerW(n.service) == L"winusb") return &n;
    return nodes.empty() ? NULL : &nodes[0];
}

static int DoStatus(const std::string& vidpid) {
    std::vector<Node> nodes;
    if (!FindNodes(vidpid, nodes)) {
        printf("No present device nodes match %s\n", vidpid.c_str());
        printf("Plug the keyboard in and re-run. (Node list is per-port; a moved\n");
        printf("port creates a new node — run 'swap' again for the new port.)\n");
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

static int DoSwap(const std::string& vidpid) {
    std::vector<Node> nodes;
    if (!FindNodes(vidpid, nodes)) {
        printf("No device nodes match %s — is the keyboard plugged in?\n", vidpid.c_str());
        return 2;
    }
    Node* n = PickForSwap(nodes);
    printf("Binding WinUSB (inbox winusb.inf) to: %ls\n", n->hardwareId.c_str());
    BOOL reboot = FALSE;
    BOOL ok = UpdateDriverForPlugAndPlayDevicesW(NULL, (LPWSTR)n->hardwareId.c_str(),
                L"C:\\Windows\\INF\\winusb.inf", INSTALLFLAG_FORCE, &reboot);
    if (!ok) {
        DWORD err = GetLastError();
        printf("FAILED: UpdateDriverForPlugAndPlayDevicesW error %lu\n", err);
        if (err == 5) printf("Run as administrator (UAC prompt should appear automatically).\n");
        return 1;
    }
    printf("OK. WinUSB bound%s.\n", reboot ? " — a reboot is recommended" : "");
    printf("KeySidekick will pick the keyboard up automatically; profiles are kept per VID/PID.\n");
    return 0;
}

static int DoRestore(const std::string& vidpid) {
    std::vector<Node> nodes;
    if (!FindNodes(vidpid, nodes)) {
        printf("No device nodes match %s — is the keyboard plugged in?\n", vidpid.c_str());
        return 2;
    }
    Node* n = PickForRestore(nodes);
    printf("Restoring inbox HID driver (input.inf) to: %ls\n", n->hardwareId.c_str());
    BOOL reboot = FALSE;
    BOOL ok = UpdateDriverForPlugAndPlayDevicesW(NULL, (LPWSTR)n->hardwareId.c_str(),
                L"C:\\Windows\\INF\\input.inf", INSTALLFLAG_FORCE, &reboot);
    if (!ok) {
        DWORD err = GetLastError();
        printf("FAILED: error %lu (try Device Manager: Uninstall device, then replug).\n", err);
        return 1;
    }
    printf("OK. The keyboard is an ordinary keyboard again%s.\n",
           reboot ? " — a reboot is recommended" : "");
    return 0;
}

static void Usage() {
    printf("KeySidekick driver helper (portable, no sidekick.exe needed)\n\n");
    printf("Usage:\n");
    printf("  ks_driver.exe status  vid_xxxx&pid_yyyy\n");
    printf("  ks_driver.exe swap    vid_xxxx&pid_yyyy   (bind WinUSB — after port change run again)\n");
    printf("  ks_driver.exe restore vid_xxxx&pid_yyyy   (back to a normal keyboard)\n\n");
    printf("Windows 10 1809+ required (uses the inbox signed winusb.inf — no Zadig).\n");
}

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) { Usage(); return 1; }
    std::wstring cmd = argv[1];
    // Self-elevate ONLY for driver-mutating commands (status is read-only).
    // swap/restore need admin rights (any driver binding does).
    if (!IsUserAnAdmin() && (cmd == L"swap" || cmd == L"restore")) {
        wchar_t self[MAX_PATH] = {0};
        GetModuleFileNameW(NULL, self, MAX_PATH);
        wchar_t args[1024] = {0};
        wchar_t* p = args;
        for (int i = 1; i < argc; ++i) {
            p += swprintf(p, 1024 - (p - args), L"\"%s\" ", argv[i]);
        }
        HINSTANCE h = ShellExecuteW(NULL, L"runas", self, args, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)h <= 32) {
            printf("Elevation cancelled or failed (ShellExecute error %d).\n", (int)(INT_PTR)h);
            return 1;
        }
        return 0;   // elevated copy prints its own result
    }

    std::string vidpid;
    if (argc >= 3) {
        int len = WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, NULL, 0, NULL, NULL);
        std::string s(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, &s[0], len, NULL, NULL);
        vidpid = s;
    }

    if (cmd == L"status" || cmd == L"swap" || cmd == L"restore") {
        if (vidpid.empty()) { Usage(); return 1; }
        if (cmd == L"status") return DoStatus(vidpid);
        if (cmd == L"swap") return DoSwap(vidpid);
        return DoRestore(vidpid);
    }
    Usage();
    return 1;
}
