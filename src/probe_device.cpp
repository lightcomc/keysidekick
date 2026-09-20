// probe_device.cpp
// WinUSB device diagnostics utility.
// Usage: probe_device [vid_xxxx&pid_yyyy]
//   With a VID/PID filter: probe only the WinUSB device whose device path
//     contains that (lowercase) substring.
//   Without arguments: probe every WinUSB device currently present.
// Run AFTER replacing the driver with WinUSB via Zadig (see ZADIG_INSTRUCTIONS.md).
// Prints: device path, interface GUID, endpoints, HID descriptor.
//
// Build: build.bat (g++ -o probe_device.exe probe_device.cpp -lsetupapi -lwinusb)

#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <initguid.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// WinUSB assigns an interface GUID to the device. By default Zadig uses the
// standard GUID_DEVINTERFACE_WINUSB, but a custom one is possible.
// We probe both: the standard WinUSB GUID and a brute-force scan of all
// device interfaces matching the VID/PID filter.

// Official GUID_DEVINTERFACE_WINUSB (assigned by Zadig when WinUSB is selected):
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB,
    0xDEE824E7, 0x7296, 0x4E41, 0x8C, 0x39, 0x0A, 0x9C, 0x1B, 0xDA, 0x4A, 0x2F);

// Alternative — libusb-win32 GUID (if Zadig installed libusb0):
DEFINE_GUID(GUID_DEVINTERFACE_LIBUSB0,
    0xF9F3FF14, 0xAEBE, 0x4Dde, 0xAE, 0x40, 0x72, 0x75, 0xF5, 0x34, 0x4A, 0x4A);

// Custom GUID that Zadig may have assigned to a specific device (from the registry):
// {901A2603-A95E-4CA8-86BF-FB0547C06B64}
DEFINE_GUID(GUID_DEVINTERFACE_TARGET_WINUSB,
    0x901A2603, 0xA95E, 0x4CA8, 0x86, 0xBF, 0xFB, 0x05, 0x47, 0xC0, 0x6B, 0x64);

bool ProbeDevice(const char* devicePath);  // forward declaration; false = probe failed

// Optional lowercase "vid_xxxx&pid_yyyy" filter. Empty = probe ALL WinUSB devices.
static char g_vidPidFilter[64] = {0};

// Счётчики для кода возврата: сколько устройств совпало с фильтром и сколько из
// них не удалось опросить (не открылось или не инициализировался WinUSB).
static int g_matchedCount = 0;
static int g_probeFailedCount = 0;

void PrintErr(const char* where) {
    DWORD e = GetLastError();
    char buf[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, e, 0, buf, 256, NULL);
    printf("  [ERROR %s] code=%lu: %s\n", where, e, buf);
}

// Enumerate all device interfaces of the three GUIDs; probe the ones that
// match the VID/PID filter (or all of them when the filter is empty).
bool FindAndProbe() {
    const GUID* guids[] = {
        &GUID_DEVINTERFACE_TARGET_WINUSB,
        &GUID_DEVINTERFACE_WINUSB,
        &GUID_DEVINTERFACE_LIBUSB0,
        NULL
    };

    bool foundAny = false;

    for (int g = 0; guids[g] != NULL; g++) {
        printf("=== Searching GUID #%d ===\n", g);
        HDEVINFO hDevInfo = SetupDiGetClassDevs(guids[g], NULL, NULL,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hDevInfo == INVALID_HANDLE_VALUE) {
            PrintErr("SetupDiGetClassDevs");
            continue;
        }

        SP_DEVICE_INTERFACE_DATA ifData = { sizeof(ifData) };
        for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, guids[g], idx, &ifData); idx++) {
            // Get the detail (device path)
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailA(hDevInfo, &ifData, NULL, 0, &needed, NULL);
            if (needed == 0) continue;

            BYTE* buf = new BYTE[needed];
            PSP_DEVICE_INTERFACE_DETAIL_DATA_A pDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)buf;
            pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

            if (!SetupDiGetDeviceInterfaceDetailA(hDevInfo, &ifData, pDetail, needed, NULL, NULL)) {
                delete[] buf;
                continue;
            }

            // Does the path contain the VID/PID filter (if any)?
            char pathLower[1024];
            strncpy(pathLower, pDetail->DevicePath, sizeof(pathLower)-1);
            pathLower[sizeof(pathLower)-1] = 0;
            for (char* p = pathLower; *p; p++) *p = (char)tolower((unsigned char)*p);

            bool isTarget = (g_vidPidFilter[0] == 0) ||
                            (strstr(pathLower, g_vidPidFilter) != NULL);

            printf("\n[%lu] %s\n", idx, isTarget ? "*** TARGET DEVICE ***" : "(not target)");
            printf("  Path: %s\n", pDetail->DevicePath);

            if (isTarget) {
                foundAny = true;
                g_matchedCount++;
                if (!ProbeDevice(pDetail->DevicePath)) g_probeFailedCount++;
            }
            delete[] buf;
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    return foundAny;
}

bool ProbeDevice(const char* devicePath) {
    printf("\n--- Probing target device ---\n");

    // Convert path to wide. Результат проверяем: при слишком длинном пути
    // конвертация не выполняется, и CreateFileW ушёл бы с неинициализированным
    // буфером вместо внятной ошибки.
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_ACP, 0, devicePath, -1, wpath, 1024) == 0) {
        PrintErr("MultiByteToWideChar (device path too long?)");
        return false;
    }

    HANDLE hDevice = CreateFileW(wpath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (hDevice == INVALID_HANDLE_VALUE) {
        PrintErr("CreateFile (try without GENERIC_WRITE)");
        // Retry read-only
        hDevice = CreateFileW(wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice == INVALID_HANDLE_VALUE) {
            PrintErr("CreateFile (read-only)");
            return false;
        }
        printf("  (opened read-only)\n");
    }

    WINUSB_INTERFACE_HANDLE hWinUsb = NULL;
    if (!WinUsb_Initialize(hDevice, &hWinUsb)) {
        PrintErr("WinUsb_Initialize (device NOT on the WinUSB driver?)");
        CloseHandle(hDevice);
        return false;
    }
    printf("  WinUSB initialized OK\n");

    // Query interface settings (first setting).
    // Структура обнулена: при ошибке запроса bNumEndpoints остаётся 0, поэтому
    // цикл ниже не пойдёт по мусорному счётчику. Раньше ошибка игнорировалась —
    // и строка интерфейса, ради которой утилита и запускается, молча пропадала.
    USB_INTERFACE_DESCRIPTOR ifDesc = {0};
    if (WinUsb_QueryInterfaceSettings(hWinUsb, 0, &ifDesc)) {
        printf("  Interface: bInterfaceClass=0x%02X bInterfaceSubClass=0x%02X bNumEndpoints=%u\n",
            ifDesc.bInterfaceClass, ifDesc.bInterfaceSubClass, ifDesc.bNumEndpoints);
    } else {
        PrintErr("WinUsb_QueryInterfaceSettings (endpoints unknown, pipe scan skipped)");
    }

    // Query pipes
    printf("  Endpoints:\n");
    for (BYTE i = 0; i < ifDesc.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pipeInfo;
        if (WinUsb_QueryPipe(hWinUsb, 0, i, &pipeInfo)) {
            const char* dir = (pipeInfo.PipeId & 0x80) ? "IN " : "OUT";
            const char* type = " ?";
            switch (pipeInfo.PipeType) {
                case UsbdPipeTypeControl:     type = "Ctrl"; break;
                case UsbdPipeTypeIsochronous: type = "Iso "; break;
                case UsbdPipeTypeBulk:        type = "Bulk"; break;
                case UsbdPipeTypeInterrupt:   type = "Intr"; break;
            }
            printf("    pipe %u: id=0x%02X %s %s maxPkt=%u interval=%u\n",
                i, pipeInfo.PipeId, dir, type, pipeInfo.MaximumPacketSize, pipeInfo.Interval);
        }
    }

    // Try to read the HID descriptor (via control transfer)
    printf("\n  Reading HID descriptor (control transfer)...\n");
    WINUSB_SETUP_PACKET pkt = {0};
    pkt.RequestType = 0x81;   // Device-to-host, Standard, Interface
    pkt.Request = 0x06;       // GET_DESCRIPTOR
    pkt.Value = 0x2100;       // HID descriptor type=0x21
    pkt.Index = 0;            // interface 0
    pkt.Length = 9;
    BYTE hidDescBuf[16] = {0};
    ULONG transferred = 0;
    if (WinUsb_ControlTransfer(hWinUsb, pkt, hidDescBuf, sizeof(hidDescBuf), &transferred, NULL)) {
        printf("    HID descriptor (%lu bytes):", transferred);
        for (ULONG i = 0; i < transferred; i++) printf(" %02X", hidDescBuf[i]);
        printf("\n");
        // The HID descriptor reveals the report descriptor length.
        // wDescriptorLength лежит в байтах 7-8, поэтому короткий трансфер в 7-8
        // байт не содержит его целиком — длину из неотправленных байтов не берём.
        if (transferred >= 9) {
            USHORT reportDescLen = hidDescBuf[7] | (hidDescBuf[8] << 8);
            printf("    HID descriptor bcdHID=%02X%02X reportDescLen=%u\n",
                hidDescBuf[3], hidDescBuf[2], reportDescLen);
        }
    } else {
        PrintErr("WinUsb_ControlTransfer (HID descriptor)");
    }

    // Try to read the HID Report Descriptor
    printf("\n  Reading HID Report Descriptor...\n");
    pkt.Value = 0x2200;   // REPORT descriptor type=0x22
    pkt.Length = 256;
    BYTE reportDesc[256] = {0};
    if (WinUsb_ControlTransfer(hWinUsb, pkt, reportDesc, sizeof(reportDesc), &transferred, NULL)) {
        printf("    Report descriptor (%lu bytes):\n", transferred);
        for (ULONG i = 0; i < transferred; i++) {
            if (i % 16 == 0) printf("    %04lX:", i);
            printf(" %02X", reportDesc[i]);
            if (i % 16 == 15) printf("\n");
        }
        if (transferred % 16 != 0) printf("\n");
    } else {
        PrintErr("WinUsb_ControlTransfer (report descriptor)");
    }

    printf("\n  >>> Probe complete (test read omitted — see sidekick for reading).\n");
    // Test read removed: WinUsb_ReadPipe blocks until a key is pressed, which
    // would hang probe_device in non-interactive mode. Use sidekick.exe for reading.

    WinUsb_Free(hWinUsb);
    CloseHandle(hDevice);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc >= 2) {
        const char* arg = argv[1];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0 ||
            strcmp(arg, "/?") == 0 || strcmp(arg, "-help") == 0) {
            printf("Usage: probe_device [vid_xxxx&pid_yyyy]\n");
            printf("  With a VID/PID filter: probe only the WinUSB device whose path\n");
            printf("    contains it (lowercase, e.g. probe_device vid_1234&pid_abcd).\n");
            printf("  Without arguments: probe every WinUSB device currently present.\n");
            return 0;
        }
        strncpy(g_vidPidFilter, arg, sizeof(g_vidPidFilter)-1);
        g_vidPidFilter[sizeof(g_vidPidFilter)-1] = 0;
        for (char* p = g_vidPidFilter; *p; p++) *p = (char)tolower((unsigned char)*p);
    }

    printf("=== WinUSB Device Probe ===\n");
    if (g_vidPidFilter[0]) {
        printf("Target (VID/PID filter): %s\n\n", g_vidPidFilter);
    } else {
        printf("Target: ALL WinUSB devices present (no filter; pass vid_xxxx&pid_yyyy to narrow)\n\n");
    }

    if (!FindAndProbe()) {
        printf("\n*** No WinUSB device found. ***\n");
        printf("Possible reasons:\n");
        printf("  1. The driver has not been replaced via Zadig yet (see ZADIG_INSTRUCTIONS.md)\n");
        printf("  2. The device uses a non-standard GUID — check Device Manager\n");
        printf("  3. The device is disconnected\n");
        return 1;
    }

    // Код возврата должен отражать результат опроса, а не только факт находки:
    // иначе вызывающий скрипт считает успехом случай, когда ни одно устройство
    // открыть не удалось.
    if (g_probeFailedCount > 0) {
        printf("\n*** %d of %d matched device(s) failed to probe. ***\n",
            g_probeFailedCount, g_matchedCount);
        if (g_probeFailedCount == g_matchedCount) return 2;
    }

    printf("\n=== Probe complete ===\n");
    return 0;
}
