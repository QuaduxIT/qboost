/*!
 * @license Apache-2.0
 * Copyright © 2026 Quadux IT GmbH
 *    ____                  __              __________   ______          __    __  __
 *   / __ \__  ______ _____/ /_  ___  __   /  _/_  __/  / ____/___ ___  / /_  / / / /
 *  / / / / / / / __ `/ __  / / / / |/_/   / /  / /    / / __/ __ `__ \/ __ \/ /_/ /
 * / /_/ / /_/ / /_/ / /_/ / /_/ />  <   _/ /  / /    / /_/ / / / / / / /_/ / __  /
 * \___\_\__,_/\__,_/\__,_/\__,_/_/|_|  /___/ /_/     \____/_/ /_/ /_/_.___/_/ /_/
 *
 * qboost
 * Author: Walter Hoffmann
 */
// qboost-cli.cpp
// -----------------------------------------------------------------------------
// Steuer- und Aktivierungswerkzeug fuer Quadux Boost.
//
// Zwei Aufgabenbereiche:
//   1. Gain-Steuerung (kein Admin noetig) - schreibt/liest den Verstaerkungs-
//      faktor im geteilten Speicher, den die APO in Echtzeit ausliest.
//   2. Aktivierung (Admin noetig) - traegt die APO am Audio-Endpoint ein bzw.
//      wieder aus und startet den Audiodienst neu.
//
// Enthaelt zugleich die komplette Einrichtungslogik (siehe setup.h).
//
// -----------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>       // IAudioMeterInformation
#include <propsys.h>
#include <propvarutil.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <mmsystem.h>          // PlaySound fuer den Selbsttest

#include "../apo/shared.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")

// {02893EAE-2EB8-40C6-8097-E43F39F210AC}
static const CLSID CLSID_QuaduxBoostApo =
{ 0x02893eae, 0x2eb8, 0x40c6, { 0x80, 0x97, 0xe4, 0x3f, 0x39, 0xf2, 0x10, 0xac } };

// PROPERTYKEYs lokal definieren (die Header deklarieren sie nur via
// DEFINE_PROPERTYKEY; ohne INITGUID entstehen keine Symbole).
// PKEY_AudioEndpoint_Disable_SysFx {1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5
static const PROPERTYKEY PKEY_Disable_SysFx =
{ { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 5 };
// Legacy-FX-Effektslots am Endpoint, alle GUID {D04E05A6-594B-4fb6-A80D-01AF5EED7D1D}:
//   ,5 = StreamEffect (SFX), ,6 = ModeEffect (MFX), ,7 = EndpointEffect (EFX)
static const GUID FX_GUID =
{ 0xd04e05a6, 0x594b, 0x4fb6, { 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d } };
static const PROPERTYKEY PKEY_Fx_Stream   = { FX_GUID, 5 };
static const PROPERTYKEY PKEY_Fx_Mode     = { FX_GUID, 6 };
static const PROPERTYKEY PKEY_Fx_Endpoint = { FX_GUID, 7 };
// Composite-Slots (modernes Win11 wertet DIESE aus; Typ = String-Array):
//   ,13 = CompositeStream (SFX), ,14 = CompositeMode (MFX), ,15 = CompositeEndpoint (EFX)
static const PROPERTYKEY PKEY_Comp_Stream   = { FX_GUID, 13 };
static const PROPERTYKEY PKEY_Comp_Mode     = { FX_GUID, 14 };
static const PROPERTYKEY PKEY_Comp_Endpoint = { FX_GUID, 15 };
// PKEY_Device_FriendlyName {a45c254e-df1c-4efd-8020-67d146a850e0},14
static const PROPERTYKEY PKEY_Dev_FriendlyName =
{ { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

// ---- Shared-Zustand (dateigestuetzt, siehe shared.h) -----------------------
static int CmdSet(double percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 800) percent = 800;
    QBoostMapping m;
    if (!QBoostOpen(&m, true) || !m.writable)
    { fwprintf(stderr, L"Zustandsdatei nicht schreibbar.\n"); QBoostClose(&m); return 2; }
    m.data->gain    = static_cast<float>(percent / 100.0);
    m.data->enabled = (percent != 100.0) ? 1 : m.data->enabled;
    wprintf(L"gain=%.3f enabled=%u\n", m.data->gain, m.data->enabled);
    QBoostClose(&m);
    return 0;
}

static int CmdEnable(bool on)
{
    QBoostMapping m;
    if (!QBoostOpen(&m, true) || !m.writable)
    { fwprintf(stderr, L"Zustandsdatei nicht schreibbar.\n"); QBoostClose(&m); return 2; }
    m.data->enabled = on ? 1 : 0;
    wprintf(L"enabled=%u\n", m.data->enabled);
    QBoostClose(&m);
    return 0;
}

static int CmdGet()
{
    QBoostMapping m;
    if (!QBoostOpen(&m, false))
    { wprintf(L"gain=1.000 enabled=0 peak=0 (kein Zustand)\n"); return 0; }
    wprintf(L"gain=%.3f enabled=%u peak=%.3f\n",
            m.data->gain, m.data->enabled, m.data->peak / 1000.0);
    wprintf(L"diag: ctor=%u init=%u fmt=%u lock=%u process=%u channels=%u flags=0x%X"
            L" (isFloat=%u inplace=%u writable=%u)\n",
            m.data->ctorCount, m.data->initCount, m.data->fmtCount,
            m.data->lockCount, m.data->processCount, m.data->channels, m.data->flags,
            (m.data->flags & 1) ? 1 : 0, (m.data->flags & 2) ? 1 : 0, (m.data->flags & 4) ? 1 : 0);
    if (m.data->qiFailCount)
    {
        GUID g; memcpy(&g, (const void*)m.data->lastFailIid, sizeof(GUID));
        wchar_t buf[64]; StringFromGUID2(g, buf, ARRAYSIZE(buf));
        wprintf(L"diag: abgelehnte QueryInterface-Anfragen=%u, zuletzt: %s\n",
                m.data->qiFailCount, buf);
    }
    QBoostClose(&m);
    return 0;
}

// ---- Endpoint-Aktivierung (Admin) ------------------------------------------
static IMMDevice* GetDefaultRender()
{
    IMMDeviceEnumerator* e = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&e)))
        return nullptr;
    IMMDevice* dev = nullptr;
    e->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    e->Release();
    return dev;
}

#include "setup.h"   // Einrichtung: install / uninstall / selftest

static int CmdStatus()
{
    IMMDevice* dev = GetDefaultRender();
    if (!dev) { fwprintf(stderr, L"Kein Standard-Wiedergabegeraet.\n"); return 3; }
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)))
    {
        PROPVARIANT name; PropVariantInit(&name);
        if (SUCCEEDED(store->GetValue(PKEY_Dev_FriendlyName, &name)) && name.vt == VT_LPWSTR)
            wprintf(L"Geraet: %s\n", name.pwszVal);
        PropVariantClear(&name);
        store->Release();
    }
    dev->Release();

    // Die Effektkette steht in FxProperties - NICHT im PropertyStore des
    // Geraets. Deshalb hier direkt aus der Registry lesen.
    std::wstring ep;
    if (!QbDefaultEndpointKey(ep)) { fwprintf(stderr, L"Endpoint nicht ermittelbar.\n"); return 3; }

    HKEY h = QbOpenFxKey(ep, false);
    if (!h) { fwprintf(stderr, L"FxProperties nicht lesbar.\n"); return 3; }

    const wchar_t* names[3] = { L"SFX (Stream)  ", L"MFX (Mode)    ", L"EFX (Endpoint)" };
    for (int slot = 5; slot <= 7; ++slot)
    {
        std::wstring clsid, modes;
        bool has = QbReadString(h, QbValueName(kFxGuid, slot), clsid);
        QbReadMulti(h, QbValueName(kModesGuid, slot), modes);
        if (!has || clsid.empty()) { wprintf(L"%s: (keiner)\n", names[slot - 5]); continue; }
        bool ours = (_wcsicmp(clsid.c_str(), kOurClsid) == 0);
        wprintf(L"%s: %s%s\n", names[slot - 5], clsid.c_str(),
                ours ? L"  <-- Quadux Boost" : L"");
        if (ours && !modes.empty()) wprintf(L"                  Modi: %s\n", modes.c_str());
    }
    RegCloseKey(h);
    return CmdGet();
}

static int CmdRestartAudio()
{
    // Erfordert Admin. Nutzt sc.exe fuer Robustheit ueber Abhaengigkeiten.
    wprintf(L"Starte Audiodienste neu ...\n");
    int a = _wsystem(L"net stop audiosrv /y >nul 2>&1");
    int b = _wsystem(L"net start audiosrv >nul 2>&1");
    (void)a; (void)b;
    wprintf(L"Audiodienst neu gestartet.\n");
    return 0;
}

// Misst den Spitzenpegel direkt am Endpoint (nach der Effektkette).
// Unabhaengig davon, ob die APO in die Zustandsdatei schreiben darf.
static int CmdMeter(int seconds)
{
    IMMDevice* dev = GetDefaultRender();
    if (!dev) { fwprintf(stderr, L"Kein Standard-Wiedergabegeraet.\n"); return 3; }
    IAudioMeterInformation* meter = nullptr;
    HRESULT hr = dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&meter);
    if (FAILED(hr)) { fwprintf(stderr, L"Meter nicht verfuegbar: 0x%08lx\n", hr); dev->Release(); return 3; }

    float maxPeak = 0.0f;
    const int steps = seconds * 10;
    for (int i = 0; i < steps; ++i)
    {
        float p = 0.0f;
        if (SUCCEEDED(meter->GetPeakValue(&p)) && p > maxPeak) maxPeak = p;
        Sleep(100);
    }
    wprintf(L"endpoint-peak-max=%.4f\n", maxPeak);
    meter->Release(); dev->Release();
    return 0;
}

// Nimmt Diagnosemeldungen der APO aus audiodg entgegen (Named Pipe).
// audiodg darf keine Dateien schreiben, aber Pipes oeffnen.
static int CmdLogServer(int seconds)
{
    // Offene DACL, damit der eingeschraenkte audiodg-Token schreiben darf.
    SECURITY_ATTRIBUTES sa{}; SECURITY_DESCRIPTOR sd{};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = &sd;

    const DWORD deadline = GetTickCount() + (DWORD)seconds * 1000;
    wprintf(L"Warte %d s auf Meldungen aus audiodg ...\n", seconds);
    fflush(stdout);

    // Mehrere Instanzen parallel vorhalten, damit zwischen zwei Verbindungen
    // keine Luecke entsteht, in der Meldungen verloren gehen.
    const int N = 4;
    HANDLE pipes[N];
    HANDLE evts[N];
    OVERLAPPED ovs[N]{};
    for (int i = 0; i < N; ++i)
    {
        pipes[i] = CreateNamedPipeW(L"\\\\.\\pipe\\QuaduxBoostLog",
                                    PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                                    PIPE_TYPE_BYTE | PIPE_WAIT,
                                    PIPE_UNLIMITED_INSTANCES, 0, 4096, 0, &sa);
        if (pipes[i] == INVALID_HANDLE_VALUE)
        {
            fwprintf(stderr, L"Pipe nicht erstellbar: %lu\n", GetLastError());
            return 3;
        }
        evts[i] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ovs[i].hEvent = evts[i];
        ConnectNamedPipe(pipes[i], &ovs[i]);
    }

    while (GetTickCount() < deadline)
    {
        DWORD w = WaitForMultipleObjects(N, evts, FALSE, 250);
        if (w == WAIT_TIMEOUT) continue;
        int i = (int)(w - WAIT_OBJECT_0);
        if (i < 0 || i >= N) break;

        ResetEvent(evts[i]);
        // Verbundene Instanz synchron leerlesen.
        char buf[1024]; DWORD read = 0;
        for (;;)
        {
            OVERLAPPED rov{}; rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL ok = ReadFile(pipes[i], buf, sizeof(buf) - 1, &read, &rov);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
                ok = GetOverlappedResult(pipes[i], &rov, &read, TRUE);
            CloseHandle(rov.hEvent);
            if (!ok || read == 0) break;
            buf[read] = 0;
            printf("%s", buf);
            fflush(stdout);
        }
        DisconnectNamedPipe(pipes[i]);
        ConnectNamedPipe(pipes[i], &ovs[i]);   // sofort wieder bereitstellen
    }

    for (int i = 0; i < N; ++i) { CloseHandle(pipes[i]); CloseHandle(evts[i]); }
    wprintf(L"(Ende)\n");
    fflush(stdout);
    return 0;
}

static void Usage()
{
    wprintf(
        L"qboost-cli - Steuerung fuer Quadux Boost\n\n"
        L"  set <prozent>   Verstaerkung setzen (0-800, 100 = neutral)\n"
        L"  on | off        Boost global ein-/ausschalten\n"
        L"  get             aktuellen Gain/Status ausgeben\n"
        L"  status          Effektslots + Gain anzeigen\n"
        L"  meter [sek]     Spitzenpegel am Endpoint messen (Standard 3 s)\n"
        L"  restart-audio   Audiodienst neu starten              [Admin]\n"
        L"\n"
        L"  install [slot]  einrichten und MESSEN, ob es wirkt   [Admin]\n"
        L"  uninstall       vollstaendig zurueckbauen            [Admin]\n"
        L"  selftest        prueft per Testton, ob verstaerkt wird\n"
        L"  logserver [sek] Diagnosemeldungen der APO empfangen\n");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { Usage(); return 1; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int rc = 1;
    const wchar_t* c = argv[1];
    if      (!_wcsicmp(c, L"set") && argc >= 3) rc = CmdSet(_wtof(argv[2]));
    else if (!_wcsicmp(c, L"on"))               rc = CmdEnable(true);
    else if (!_wcsicmp(c, L"off"))              rc = CmdEnable(false);
    else if (!_wcsicmp(c, L"get"))              rc = CmdGet();
    else if (!_wcsicmp(c, L"status"))           rc = CmdStatus();
    else if (!_wcsicmp(c, L"meter"))            rc = CmdMeter(argc >= 3 ? _wtoi(argv[2]) : 3);
    else if (!_wcsicmp(c, L"logserver"))        rc = CmdLogServer(argc >= 3 ? _wtoi(argv[2]) : 60);
    else if (!_wcsicmp(c, L"install"))          rc = CmdInstall(argc >= 3 ? _wtoi(argv[2]) : 0);
    else if (!_wcsicmp(c, L"uninstall"))        rc = CmdUninstall();
    else if (!_wcsicmp(c, L"selftest"))         rc = CmdSelfTest();
    else if (!_wcsicmp(c, L"restart-audio"))    rc = CmdRestartAudio();
    else Usage();
    CoUninitialize();
    return rc;
}
