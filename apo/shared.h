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
// shared.h - gemeinsamer Zustand zwischen APO-DLL (laeuft in audiodg.exe,
// Session 0) und dem Steuerwerkzeug qboost-cli (User-Session).
//
// Transport: ein dateigestuetztes Speicher-Mapping unter
//   %ProgramData%\QuaduxBoost\state.bin
// Beide Prozesse mappen dieselbe Datei. Das ist sessionuebergreifend sichtbar
// OHNE das Global-Namespace-Privileg (SeCreateGlobalPrivilege), das ein
// normaler User-Prozess nicht besitzt.
//
#pragma once
#include <windows.h>

#define QBOOST_MAGIC     0x51424f32u   // "QBO2"
#define QBOOST_GAIN_MIN  0.0f
#define QBOOST_GAIN_MAX  8.0f          // ~ +18 dB, entspricht 800 %
#define QBOOST_SUBDIR    L"QuaduxBoost"
#define QBOOST_FILENAME  L"state.bin"

#pragma pack(push, 8)
typedef struct QBoostShared
{
    volatile unsigned int  magic;    // == QBOOST_MAGIC wenn initialisiert
    volatile unsigned int  enabled;  // 0 = Passthrough, 1 = Boost aktiv
    volatile float         gain;     // linearer Faktor (1.0 = neutral)
    volatile unsigned int  peak;     // letzter Spitzenpegel *1000 (VU, von APO)
    // --- Diagnose: von der APO in audiodg geschrieben ---
    volatile unsigned int  lockCount;    // Aufrufe von LockForProcess
    volatile unsigned int  processCount; // Aufrufe von APOProcess
    volatile unsigned int  channels;     // beim Lock ausgehandelte Kanalzahl
    volatile unsigned int  flags;        // Bit0 isFloat, Bit1 inplace, Bit2 mapWritable
    volatile unsigned int  ctorCount;    // erzeugte APO-Instanzen
    volatile unsigned int  initCount;    // Aufrufe von Initialize
    volatile unsigned int  fmtCount;     // Format-Abfragen
    volatile unsigned int  qiFailCount;  // abgelehnte QueryInterface-Anfragen
    volatile unsigned int  lastFailIid[4]; // zuletzt abgelehnte IID (roh)
} QBoostShared;
#pragma pack(pop)

// Handle-Buendel, das die Zuordnung offen haelt.
typedef struct QBoostMapping
{
    HANDLE        hFile;
    HANDLE        hMap;
    QBoostShared* data;
    bool          writable;
} QBoostMapping;

// Vollen Pfad zu state.bin ermitteln (%ProgramData% mit Fallback C:\ProgramData).
static inline void QBoostStatePath(wchar_t* out, size_t cch)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(out, cch, L"%s\\%s\\%s", base, QBOOST_SUBDIR, QBOOST_FILENAME);
}

// Verzeichnis %ProgramData%\QuaduxBoost anlegen (idempotent).
static inline void QBoostEnsureDir()
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, MAX_PATH, L"%s\\%s", base, QBOOST_SUBDIR);
    CreateDirectoryW(dir, nullptr);
}

// Zuordnung oeffnen. create=true legt Datei/Verzeichnis bei Bedarf an.
// Versucht Schreibzugriff, faellt bei Bedarf auf Nur-Lesen zurueck.
// Rueckgabe: true bei Erfolg; m->data zeigt dann auf den geteilten Zustand.
static inline bool QBoostOpen(QBoostMapping* m, bool create)
{
    ZeroMemory(m, sizeof(*m));
    wchar_t path[MAX_PATH];
    QBoostStatePath(path, MAX_PATH);
    if (create) QBoostEnsureDir();

    DWORD disp   = create ? OPEN_ALWAYS : OPEN_EXISTING;
    DWORD access = GENERIC_READ | GENERIC_WRITE;
    m->hFile = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    m->writable = true;
    if (m->hFile == INVALID_HANDLE_VALUE)
    {
        // Nur-Lese-Zugriff versuchen (z.B. eingeschraenkter audiodg-Token).
        m->hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        m->writable = false;
    }
    if (m->hFile == INVALID_HANDLE_VALUE) { m->hFile = nullptr; return false; }

    // Datei auf Struktgroesse bringen (nur beim Erstellen/Schreibzugriff).
    if (m->writable)
    {
        LARGE_INTEGER sz{}; GetFileSizeEx(m->hFile, &sz);
        if (sz.QuadPart < (LONGLONG)sizeof(QBoostShared))
        {
            LARGE_INTEGER pos; pos.QuadPart = sizeof(QBoostShared);
            SetFilePointerEx(m->hFile, pos, nullptr, FILE_BEGIN);
            SetEndOfFile(m->hFile);
        }
    }

    DWORD prot   = m->writable ? PAGE_READWRITE : PAGE_READONLY;
    DWORD mapAcc = m->writable ? FILE_MAP_WRITE : FILE_MAP_READ;
    m->hMap = CreateFileMappingW(m->hFile, nullptr, prot, 0, sizeof(QBoostShared), nullptr);
    if (!m->hMap) { CloseHandle(m->hFile); m->hFile = nullptr; return false; }

    m->data = (QBoostShared*)MapViewOfFile(m->hMap, mapAcc, 0, 0, sizeof(QBoostShared));
    if (!m->data)
    {
        CloseHandle(m->hMap); CloseHandle(m->hFile);
        m->hMap = nullptr; m->hFile = nullptr;
        return false;
    }
    if (m->writable && m->data->magic != QBOOST_MAGIC)
    {
        m->data->gain = 1.0f; m->data->enabled = 0; m->data->peak = 0;
        m->data->magic = QBOOST_MAGIC;
    }
    return true;
}

static inline void QBoostClose(QBoostMapping* m)
{
    if (m->data)  { UnmapViewOfFile(m->data); m->data = nullptr; }
    if (m->hMap)  { CloseHandle(m->hMap);     m->hMap = nullptr; }
    if (m->hFile) { CloseHandle(m->hFile);    m->hFile = nullptr; }
}
