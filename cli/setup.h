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
// setup.h
// -----------------------------------------------------------------------------
// Einrichtung von Quadux Boost: registrieren, in die Effektkette einhaengen,
// die Verstaerkung MESSEN und bei Bedarf vollstaendig zurueckbauen.
//
// Wird vom MSI aufgerufen (qboost-cli install / uninstall), funktioniert aber
// genauso eigenstaendig. Erfordert Administratorrechte.
//
// Wird von qboost-cli.cpp eingebunden, NACHDEM dort GetDefaultRender() und die
// Shared-State-Helfer definiert sind (reiner Textinclude, kein eigenes Modul).
//
// -----------------------------------------------------------------------------
#pragma once
#include <string>
#include <vector>
#include <cmath>

#define QB_SLOT_DEFAULT      5      // SFX - der einzige Slot, der verarbeitet
#define QB_GAIN_DEFAULT    200      // Standard-Lautstaerke nach der Installation

static const wchar_t* kFxGuid    = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}";
static const wchar_t* kModesGuid = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b}";
static const wchar_t* kOurClsid  = L"{02893EAE-2EB8-40C6-8097-E43F39F210AC}";
static const wchar_t* kEndpointBase =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";

// Alle Wiedergabe-Verarbeitungsmodi. Nur DEFAULT einzutragen reicht nicht:
// Streams in anderen Modi (Media, Movie ...) blieben sonst unverstaerkt.
static const wchar_t* const kModeList[] = {
    L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}",   // DEFAULT
    L"{4780004E-7133-41D8-8C74-660DADD2C0EE}",   // MEDIA
    L"{B26FEB0D-EC94-477C-9494-D1AB8E753F6E}",   // MOVIE
    L"{98951333-B9CD-48B1-A0A3-FF40682D73F7}",   // COMMUNICATIONS
    L"{9CF2A70B-F377-403B-BD6B-360863E0355C}",   // NOTIFICATION
};

static std::wstring QbProgramDataDir()
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    return std::wstring(base) + L"\\" + QBOOST_SUBDIR;
}

static std::wstring QbApoDllPath()
{
    wchar_t base[MAX_PATH];
    // ProgramW6432 zeigt IMMER auf das 64-Bit-Verzeichnis - auch aus einem
    // Prozess unter WOW64, wo %ProgramFiles% auf "Program Files (x86)" zeigt.
    DWORD n = GetEnvironmentVariableW(L"ProgramW6432", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        n = GetEnvironmentVariableW(L"ProgramFiles", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(base, MAX_PATH, L"C:\\Program Files");
    return std::wstring(base) + L"\\QuaduxBoost\\QuaduxBoostApo.dll";
}

// Registry-Schluessel des Standard-Wiedergabegeraets.
// IMMDevice::GetId() liefert "{0.0.0.00000000}.{<endpoint-guid>}"; der Teil
// hinter dem letzten Punkt ist exakt der Unterschluesselname.
static bool QbDefaultEndpointKey(std::wstring& outKey)
{
    IMMDevice* dev = GetDefaultRender();
    if (!dev) return false;
    LPWSTR id = nullptr;
    HRESULT hr = dev->GetId(&id);
    dev->Release();
    if (FAILED(hr) || !id) return false;
    std::wstring s(id);
    CoTaskMemFree(id);
    size_t dot = s.rfind(L'.');
    if (dot == std::wstring::npos || dot + 1 >= s.size()) return false;
    outKey = std::wstring(kEndpointBase) + L"\\" + s.substr(dot + 1);
    return true;
}

// FxProperties mit genau den Rechten oeffnen, die Administratoren dort haben
// (SetValue + QueryValue - FullControl anzufordern schlaegt fehl).
static HKEY QbOpenFxKey(const std::wstring& endpointKey, bool write)
{
    HKEY h = nullptr;
    REGSAM sam = write ? (KEY_SET_VALUE | KEY_QUERY_VALUE) : KEY_QUERY_VALUE;
    std::wstring path = endpointKey + L"\\FxProperties";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, sam, &h) != ERROR_SUCCESS)
        return nullptr;
    return h;
}

static std::wstring QbValueName(const wchar_t* guid, int slot)
{
    wchar_t buf[128];
    swprintf_s(buf, L"%s,%d", guid, slot);
    return buf;
}

static bool QbReadString(HKEY h, const std::wstring& name, std::wstring& out)
{
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(h, name.c_str(), nullptr, &type, nullptr, &cb) != ERROR_SUCCESS) return false;
    std::vector<BYTE> buf(cb + 2, 0);
    if (RegQueryValueExW(h, name.c_str(), nullptr, &type, buf.data(), &cb) != ERROR_SUCCESS) return false;
    out.assign(reinterpret_cast<wchar_t*>(buf.data()));
    return true;
}

// REG_MULTI_SZ lesen; Eintraege mit ';' verbunden (Format der Sicherungsdatei).
static bool QbReadMulti(HKEY h, const std::wstring& name, std::wstring& joined)
{
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(h, name.c_str(), nullptr, &type, nullptr, &cb) != ERROR_SUCCESS) return false;
    std::vector<BYTE> buf(cb + 4, 0);
    if (RegQueryValueExW(h, name.c_str(), nullptr, &type, buf.data(), &cb) != ERROR_SUCCESS) return false;
    const wchar_t* p = reinterpret_cast<const wchar_t*>(buf.data());
    joined.clear();
    while (*p) { if (!joined.empty()) joined += L';'; joined += p; p += wcslen(p) + 1; }
    return true;
}

static bool QbWriteString(HKEY h, const std::wstring& name, const std::wstring& val)
{
    return RegSetValueExW(h, name.c_str(), 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(val.c_str()),
                          (DWORD)((val.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

static bool QbWriteMulti(HKEY h, const std::wstring& name, const std::vector<std::wstring>& items)
{
    std::wstring blob;
    for (size_t i = 0; i < items.size(); ++i) { blob += items[i]; blob += L'\0'; }
    blob += L'\0';
    return RegSetValueExW(h, name.c_str(), 0, REG_MULTI_SZ,
                          reinterpret_cast<const BYTE*>(blob.data()),
                          (DWORD)(blob.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// DLL laden und ihre Selbstregistrierung aufrufen (ersetzt regsvr32).
static bool QbRegisterApo(bool reg)
{
    std::wstring dll = QbApoDllPath();
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) { fwprintf(stderr, L"DLL nicht ladbar: %s (%lu)\n", dll.c_str(), GetLastError()); return false; }
    typedef HRESULT (STDAPICALLTYPE *PFNReg)(void);
    PFNReg fn = (PFNReg)GetProcAddress(m, reg ? "DllRegisterServer" : "DllUnregisterServer");
    HRESULT hr = fn ? fn() : E_FAIL;
    FreeLibrary(m);
    if (FAILED(hr)) {
        fwprintf(stderr, L"%s fehlgeschlagen: 0x%08lX\n",
                 reg ? L"Registrieren" : L"Abmelden", (unsigned long)hr);
        return false;
    }
    return true;
}

static void QbRestartAudio()
{
    wprintf(L"  Starte Audiodienste neu ...\n");
    fflush(stdout);
    // AudioEndpointBuilder baut die Effektkette auf; Audiosrv haengt davon ab.
    _wsystem(L"net stop audioendpointbuilder /y >nul 2>&1");
    Sleep(1000);
    _wsystem(L"net start audioendpointbuilder >nul 2>&1");
    _wsystem(L"net start audiosrv >nul 2>&1");
    Sleep(3000);
}

// --- Testton -----------------------------------------------------------------
static std::wstring QbToneFile()
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"qboost_tone.wav";
}

static bool QbWriteToneFile()
{
    const int sr = 44100, frames = sr;      // 1 Sekunde, wird geloopt
    // Bewusst leise: bei 300 %% darf der Begrenzer NICHT greifen, sonst misst
    // der Test 1.0 und das Verhaeltnis waere falsch (gemessen: amp 8000 ->
    // Pegel 0.44, mal 3 = Anschlag).
    const double amp = 2500.0;
    std::vector<BYTE> f;
    struct P {
        std::vector<BYTE>& b;
        void u32(unsigned v) { for (int i = 0; i < 4; ++i) b.push_back((BYTE)(v >> (8 * i))); }
        void u16(unsigned short v) { for (int i = 0; i < 2; ++i) b.push_back((BYTE)(v >> (8 * i))); }
        void tag(const char* t) { for (int i = 0; i < 4; ++i) b.push_back((BYTE)t[i]); }
    } p{ f };

    const unsigned dataBytes = frames * 4;  // stereo, 16 Bit
    p.tag("RIFF"); p.u32(36 + dataBytes);
    p.tag("WAVE"); p.tag("fmt "); p.u32(16);
    p.u16(1); p.u16(2); p.u32(sr); p.u32(sr * 4); p.u16(4); p.u16(16);
    p.tag("data"); p.u32(dataBytes);
    for (int i = 0; i < frames; ++i) {
        double t = 2.0 * 3.14159265358979 * 220.0 * i / sr;
        short v = (short)(amp * sin(t));
        p.u16((unsigned short)v); p.u16((unsigned short)v);
    }

    HANDLE h = CreateFileW(QbToneFile().c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    WriteFile(h, f.data(), (DWORD)f.size(), &w, nullptr);
    CloseHandle(h);
    return w == f.size();
}

// Spitzenpegel am Endpoint ueber die angegebene Dauer.
static float QbMeasurePeak(int millis)
{
    IMMDevice* dev = GetDefaultRender();
    if (!dev) return -1.0f;
    IAudioMeterInformation* meter = nullptr;
    HRESULT hr = dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&meter);
    if (FAILED(hr)) { dev->Release(); return -1.0f; }
    float mx = 0.0f;
    for (int i = 0; i < millis / 100; ++i) {
        float pk = 0.0f;
        if (SUCCEEDED(meter->GetPeakValue(&pk)) && pk > mx) mx = pk;
        Sleep(100);
    }
    meter->Release(); dev->Release();
    return mx;
}

static void QbSetGain(double percent, bool enable)
{
    QBoostMapping m;
    if (QBoostOpen(&m, true) && m.writable) {
        m.data->gain    = (float)(percent / 100.0);
        m.data->enabled = enable ? 1 : 0;
    }
    QBoostClose(&m);
}

// Ergebnis des Selbsttests. UNVERIFIABLE ist wichtig: laeuft der Test als
// Dienst in Sitzung 0 (so ruft das MSI ihn auf), erreicht der Testton die
// Lautsprecher nicht - das ist KEIN Fehlschlag, sondern schlicht nicht messbar.
enum QbTestResult { QB_TEST_PASS, QB_TEST_FAIL, QB_TEST_UNVERIFIABLE };

// Misst, ob die APO wirklich verstaerkt: Pegel bei 100 % gegen 300 %.
// Das ist der einzige belastbare Nachweis - "DLL geladen" sagt nichts darueber,
// ob sie auch verarbeitet.
static QbTestResult QbSelfTest(bool verbose, float* outRatio)
{
    if (outRatio) *outRatio = 0.0f;
    if (!QbWriteToneFile()) { fwprintf(stderr, L"Testton nicht erzeugbar.\n"); return QB_TEST_UNVERIFIABLE; }

    PlaySoundW(QbToneFile().c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_LOOP);
    Sleep(1200);

    QbSetGain(100, true);  Sleep(400);
    float p1 = QbMeasurePeak(2000);
    QbSetGain(300, true);  Sleep(400);
    float p3 = QbMeasurePeak(2000);

    PlaySoundW(nullptr, nullptr, 0);

    float ratio = (p1 > 0.0001f) ? (p3 / p1) : 0.0f;
    if (outRatio) *outRatio = ratio;
    if (verbose)
        wprintf(L"  Pegel bei 100%% = %.4f | bei 300%% = %.4f | Verhaeltnis = %.2f\n",
                p1, p3, ratio);
    // Ueberhaupt kein Signal: der Ton kam nicht am Endpoint an (Sitzung 0,
    // stummgeschaltet, anderes Standardgeraet). Das ist kein Fehlschlag.
    if (p1 <= 0.005f) {
        if (verbose) wprintf(L"  Kein Signal messbar - nicht ueberpruefbar.\n");
        return QB_TEST_UNVERIFIABLE;
    }
    // Erwartet ~3.0. Grosszuegige Schranke, weil der Begrenzer bei lauten
    // Passagen frueher greifen kann.
    return (ratio > 2.4f) ? QB_TEST_PASS : QB_TEST_FAIL;
}

// --- Sicherung der urspruenglichen Effektkette --------------------------------
static std::wstring QbBackupPath() { return QbProgramDataDir() + L"\\fx-backup.txt"; }

static void QbSaveBackup(const std::wstring& endpointKey, int slot,
                         const std::wstring& origFx, const std::wstring& origModes)
{
    CreateDirectoryW(QbProgramDataDir().c_str(), nullptr);
    std::wstring text = L"endpoint=" + endpointKey + L"\n"
                      + L"slot="     + std::to_wstring(slot) + L"\n"
                      + L"effect="   + origFx + L"\n"
                      + L"modes="    + origModes + L"\n";
    HANDLE h = CreateFileW(QbBackupPath().c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const wchar_t bom = 0xFEFF;
    DWORD w = 0;
    WriteFile(h, &bom, sizeof(bom), &w, nullptr);
    WriteFile(h, text.c_str(), (DWORD)(text.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
}

static bool QbLoadBackup(std::wstring& endpointKey, int& slot,
                         std::wstring& origFx, std::wstring& origModes)
{
    HANDLE h = CreateFileW(QbBackupPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    std::vector<BYTE> buf(size + 2, 0);
    ReadFile(h, buf.data(), size, &got, nullptr);
    CloseHandle(h);

    const wchar_t* p = reinterpret_cast<const wchar_t*>(buf.data());
    if (*p == 0xFEFF) ++p;
    std::wstring all(p);

    std::wstring slotStr;
    struct F {
        std::wstring& all;
        void get(const wchar_t* key, std::wstring& out) {
            size_t i = all.find(key);
            if (i == std::wstring::npos) return;
            i += wcslen(key);
            size_t e = all.find(L'\n', i);
            out = all.substr(i, (e == std::wstring::npos ? all.size() : e) - i);
            while (!out.empty() && out[out.size() - 1] == L'\r') out.erase(out.size() - 1);
        }
    } fld{ all };

    fld.get(L"endpoint=", endpointKey);
    fld.get(L"slot=",     slotStr);
    fld.get(L"effect=",   origFx);
    fld.get(L"modes=",    origModes);
    slot = slotStr.empty() ? QB_SLOT_DEFAULT : _wtoi(slotStr.c_str());
    return !endpointKey.empty();
}

// Traegt die APO in einen Slot ein und sichert vorher den Originalzustand.
static bool QbApplySlot(const std::wstring& endpointKey, int slot)
{
    HKEY h = QbOpenFxKey(endpointKey, true);
    if (!h) { fwprintf(stderr, L"FxProperties nicht schreibbar (Adminrechte noetig).\n"); return false; }

    std::wstring origFx, origModes;
    QbReadString(h, QbValueName(kFxGuid, slot), origFx);
    QbReadMulti (h, QbValueName(kModesGuid, slot), origModes);

    // Nur sichern, wenn dort NICHT schon unsere eigene APO steht. Sonst wuerde
    // eine zweite Installation die Sicherung mit unserer eigenen CLSID
    // ueberschreiben - der Rueckbau koennte den Originaleffekt dann nie mehr
    // wiederherstellen.
    if (_wcsicmp(origFx.c_str(), kOurClsid) != 0)
        QbSaveBackup(endpointKey, slot, origFx, origModes);
    else
        wprintf(L"  (bereits eingetragen - Sicherung bleibt unangetastet)\n");

    std::vector<std::wstring> modes;
    for (size_t i = 0; i < sizeof(kModeList) / sizeof(kModeList[0]); ++i)
        modes.push_back(kModeList[i]);

    bool ok = QbWriteString(h, QbValueName(kFxGuid, slot), kOurClsid)
           && QbWriteMulti (h, QbValueName(kModesGuid, slot), modes);
    RegCloseKey(h);
    return ok;
}

static void QbRestoreSlot(const std::wstring& endpointKey, int slot,
                          const std::wstring& origFx, const std::wstring& origModes)
{
    HKEY h = QbOpenFxKey(endpointKey, true);
    if (!h) return;

    if (origFx.empty()) RegDeleteValueW(h, QbValueName(kFxGuid, slot).c_str());
    else                QbWriteString(h, QbValueName(kFxGuid, slot), origFx);

    if (origModes.empty()) RegDeleteValueW(h, QbValueName(kModesGuid, slot).c_str());
    else {
        std::vector<std::wstring> items;
        std::wstring cur;
        for (size_t i = 0; i < origModes.size(); ++i) {
            if (origModes[i] == L';') { items.push_back(cur); cur.clear(); }
            else cur += origModes[i];
        }
        if (!cur.empty()) items.push_back(cur);
        QbWriteMulti(h, QbValueName(kModesGuid, slot), items);
    }
    RegCloseKey(h);
}

static int CmdUninstall()
{
    wprintf(L"Quadux Boost wird entfernt ...\n");
    std::wstring ep, fx, modes;
    int slot = QB_SLOT_DEFAULT;

    if (QbLoadBackup(ep, slot, fx, modes)) {
        QbRestoreSlot(ep, slot, fx, modes);
        wprintf(L"  Urspruengliche Effektkette wiederhergestellt.\n");
    } else if (QbDefaultEndpointKey(ep)) {
        // Keine Sicherung: nur die eigenen Eintraege entfernen, nichts anderes anfassen.
        HKEY h = QbOpenFxKey(ep, true);
        if (h) {
            for (int s = 5; s <= 7; ++s) {
                std::wstring cur;
                if (QbReadString(h, QbValueName(kFxGuid, s), cur) &&
                    _wcsicmp(cur.c_str(), kOurClsid) == 0)
                    RegDeleteValueW(h, QbValueName(kFxGuid, s).c_str());
            }
            RegCloseKey(h);
        }
        wprintf(L"  Keine Sicherung gefunden - eigene Eintraege entfernt.\n");
    }

    QbRegisterApo(false);
    QbRestartAudio();
    wprintf(L"Rueckbau abgeschlossen.\n");
    return 0;
}

static int CmdInstall(int forcedSlot)
{
    wprintf(L"=== Quadux Boost wird eingerichtet ===\n");

    std::wstring dll = QbApoDllPath();
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"APO-DLL fehlt: %s\n", dll.c_str());
        return 2;
    }
    if (!QbRegisterApo(true)) return 2;
    wprintf(L"  APO registriert.\n");

    std::wstring ep;
    if (!QbDefaultEndpointKey(ep)) {
        fwprintf(stderr, L"Standard-Wiedergabegeraet nicht gefunden.\n");
        return 3;
    }
    wprintf(L"  Geraet: %s\n", ep.c_str() + wcslen(kEndpointBase) + 1);

    // Zustandsverzeichnis mit offener ACL - audiodg muss die Datei lesen koennen.
    CreateDirectoryW(QbProgramDataDir().c_str(), nullptr);
    {
        std::wstring cmd = L"icacls \"" + QbProgramDataDir() + L"\" /grant *S-1-1-0:(OI)(CI)M /T >nul 2>&1";
        _wsystem(cmd.c_str());
    }

    // Der gemessen funktionierende Slot zuerst; die anderen als Rueckfall,
    // falls andere Hardware sich anders verhaelt.
    int slots[3] = { QB_SLOT_DEFAULT, 6, 7 };
    if (forcedSlot) { slots[0] = forcedSlot; slots[1] = 0; slots[2] = 0; }

    for (int i = 0; i < 3; ++i) {
        int slot = slots[i];
        if (!slot) break;
        wprintf(L"\n  --- Slot %d ---\n", slot);
        if (!QbApplySlot(ep, slot)) return 3;
        QbRestartAudio();

        float ratio = 0.0f;
        QbTestResult res = QbSelfTest(true, &ratio);

        if (res == QB_TEST_PASS) {
            QbSetGain(QB_GAIN_DEFAULT, true);
            wprintf(L"\nFERTIG - Quadux Boost arbeitet (Slot %d, Faktor %.2f gemessen).\n", slot, ratio);
            wprintf(L"Lautstaerke steht auf %d %%.  Aendern:  qboost-cli set <prozent>\n", QB_GAIN_DEFAULT);
            return 0;
        }
        if (res == QB_TEST_UNVERIFIABLE) {
            // Typisch beim Aufruf aus dem MSI: dort laeuft alles als Dienst in
            // Sitzung 0, wo der Testton die Lautsprecher nie erreicht. Die
            // Einrichtung ist deshalb NICHT falsch - nur hier nicht messbar.
            QbSetGain(QB_GAIN_DEFAULT, true);
            wprintf(L"\nEingerichtet (Slot %d) - hier nicht messbar.\n", slot);
            wprintf(L"Bitte in einer normalen Sitzung pruefen:  qboost-cli selftest\n");
            return 0;
        }
        wprintf(L"  Slot %d verstaerkt nicht - setze zurueck.\n", slot);
        std::wstring e2, f2, m2;
        int s2 = slot;
        if (QbLoadBackup(e2, s2, f2, m2)) QbRestoreSlot(e2, s2, f2, m2);
    }

    wprintf(L"\nKein Slot hat funktioniert - vollstaendiger Rueckbau.\n");
    CmdUninstall();
    return 4;
}

static int CmdSelfTest()
{
    // Vorherigen Zustand merken und danach wiederherstellen.
    float before = (float)(QB_GAIN_DEFAULT / 100.0);
    unsigned wasOn = 1;
    {
        QBoostMapping m;
        if (QBoostOpen(&m, false) && m.data) { before = m.data->gain; wasOn = m.data->enabled; }
        QBoostClose(&m);
    }

    wprintf(L"Selbsttest laeuft (Testton, etwa 7 Sekunden) ...\n");
    float ratio = 0.0f;
    QbTestResult res = QbSelfTest(true, &ratio);
    QbSetGain(before * 100.0, wasOn != 0);

    if (res == QB_TEST_PASS) {
        wprintf(L"ERGEBNIS: Verstaerkung arbeitet (Faktor %.2f).\n", ratio);
        return 0;
    }
    if (res == QB_TEST_UNVERIFIABLE) {
        wprintf(L"ERGEBNIS: nicht ueberpruefbar - am Geraet kam kein Signal an.\n");
        wprintf(L"          (Lautstaerke auf 0? anderes Standardgeraet? Sitzung 0?)\n");
        return 2;
    }
    wprintf(L"ERGEBNIS: keine Verstaerkung messbar.\n");
    return 1;
}
