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
// QuaduxBoostApo.cpp
// -----------------------------------------------------------------------------
// Quadux Boost - eine System-Effekt-APO (Audio Processing Object) fuer Windows,
// die den Wiedergabepegel eines Audio-Endpoints ueber 100 % anheben kann.
//
// Die eigentliche DSP ist trivial (sample *= gain, hart begrenzt). Der Wert
// liegt in der Einbindung: Windows laedt diese DLL in seine Audio-Engine
// (audiodg.exe) und ruft APOProcess() fuer jeden Audioblock des Geraets auf.
//
// Der Verstaerkungsfaktor wird live aus einer geteilten Zustandsdatei
// gesetzt (siehe shared.h), sodass Aenderungen ohne Neuladen der DLL greifen.
//
// -----------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>   // IAudioSystemEffects3
#include <audioapotypes.h>
#include <audiomediatype.h>
#include <mmreg.h>
#include <new>
#include <cstring>
#include <cstdio>
#include <cwchar>

#include "shared.h"

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT lokal definieren, damit weder ks.h noch
// ksmedia.h eingebunden werden muessen (ksmedia.h verlangt zwingend ks.h davor).
//   {00000003-0000-0010-8000-00AA00389B71}
static const GUID QBOOST_SUBTYPE_IEEE_FLOAT =
{ 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// audiobaseprocessingobject.lib stellt RegisterAPO/UnregisterAPO bereit.
#pragma comment(lib, "audiobaseprocessingobject.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

// -----------------------------------------------------------------------------
// CLSID der APO. Fest, generiert einmalig. Aendern => Neuregistrierung noetig.
//   {02893EAE-2EB8-40C6-8097-E43F39F210AC}
// -----------------------------------------------------------------------------
static const CLSID CLSID_QuaduxBoostApo =
{ 0x02893eae, 0x2eb8, 0x40c6, { 0x80, 0x97, 0xe4, 0x3f, 0x39, 0xf2, 0x10, 0xac } };

static const wchar_t* const kFriendlyName = L"Quadux Boost APO";
static const wchar_t* const kClsidString  = L"{02893EAE-2EB8-40C6-8097-E43F39F210AC}";

static HMODULE  g_hModule    = nullptr;
static LONG     g_lockCount  = 0;   // Server-Lock + lebende Objekte

#ifdef QBOOST_FORCE_GAIN
// Diagnosebau: Markierungsdatei schreiben, um zu sehen, wie weit Windows kommt.
// (Zeigt zugleich, ob audiodg ueberhaupt Dateien schreiben darf.)
// Meldet ueber eine Named Pipe. audiodg darf keine Dateien schreiben, kann
// aber Pipes oeffnen (dieser Weg ist bei Equalizer APO als gangbar belegt).
static void QBoostMark(const wchar_t* name, const char* text)
{
    UNREFERENCED_PARAMETER(name);
    // Mehrfach versuchen: der Empfaenger haelt nicht durchgehend eine freie
    // Pipe-Instanz bereit, sonst gehen Meldungen verloren.
    for (int attempt = 0; attempt < 60; ++attempt)
    {
        HANDLE h = CreateFileW(L"\\\\.\\pipe\\QuaduxBoostLog", GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(h, text, (DWORD)strlen(text), &written, nullptr);
            FlushFileBuffers(h);
            CloseHandle(h);
            return;
        }
        if (GetLastError() == ERROR_PIPE_BUSY)
            WaitNamedPipeW(L"\\\\.\\pipe\\QuaduxBoostLog", 50);
        else
            Sleep(10);
    }
}
#define QBOOST_MARK(n, t) QBoostMark(n, t)
#else
#define QBOOST_MARK(n, t) ((void)0)
#endif

// =============================================================================
//  Die APO
// =============================================================================
class CBoostApo final
    : public IAudioProcessingObjectRT
    , public IAudioProcessingObject
    , public IAudioProcessingObjectConfiguration
    // IAudioSystemEffects3 erbt von 2 und dieses von IAudioSystemEffects,
    // deckt also alle drei Generationen ab. Moderne Windows-Versionen fragen
    // diese Schnittstellen ab; fehlen sie, verwirft die Engine die APO
    // kommentarlos und liefert Stille.
    , public IAudioSystemEffects3
    // Wird von Windows 11 beim Aufbau der Effektkette abgefragt.
    , public IAudioProcessingObjectNotifications
{
public:
    // Windows erzeugt System-Effekt-APOs per COM-AGGREGATION, uebergibt also
    // ein aeusseres IUnknown. Wird das abgelehnt, kommt die APO nie zustande
    // und die Engine liefert Stille. Deshalb hier das klassische Muster aus
    // delegierendem und nicht delegierendem IUnknown.
    class CInner final : public IUnknown
    {
    public:
        CBoostApo* owner = nullptr;
        STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
        { return owner->InnerQueryInterface(riid, ppv); }
        STDMETHOD_(ULONG, AddRef)() override  { return owner->InnerAddRef(); }
        STDMETHOD_(ULONG, Release)() override { return owner->InnerRelease(); }
    };

    explicit CBoostApo(IUnknown* pOuter) : m_ref(1)
    {
        InterlockedIncrement(&g_lockCount);
        m_inner.owner = this;
        // Ohne Aggregation sind wir unser eigenes steuerndes IUnknown.
        m_controlling = pOuter ? pOuter : static_cast<IUnknown*>(&m_inner);
        // Diagnose-Anbindung so frueh wie moeglich, damit auch Aufrufe VOR
        // LockForProcess sichtbar werden (Interface-Aushandlung!).
        OpenShared();
        if (m_map.data && m_map.writable) m_map.data->ctorCount++;
        QBOOST_MARK(nullptr, pOuter ? "CTOR (aggregiert)\n" : "CTOR (eigenstaendig)\n");
    }

    IUnknown* InnerUnknown() { return static_cast<IUnknown*>(&m_inner); }

    // ---- IUnknown: delegiert an das steuernde IUnknown --------------------
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    { return m_controlling->QueryInterface(riid, ppv); }
    STDMETHOD_(ULONG, AddRef)() override  { return m_controlling->AddRef(); }
    STDMETHOD_(ULONG, Release)() override { return m_controlling->Release(); }

    // ---- Nicht delegierendes IUnknown (die eigentliche Umsetzung) --------
    HRESULT InnerQueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown))
        {
            // Bei Aggregation MUSS hier das nicht delegierende IUnknown kommen.
            *ppv = static_cast<IUnknown*>(&m_inner);
        }
        else if (riid == __uuidof(IAudioProcessingObjectRT))
        {
            *ppv = static_cast<IAudioProcessingObjectRT*>(this);
        }
        else if (riid == __uuidof(IAudioProcessingObject))
        {
            *ppv = static_cast<IAudioProcessingObject*>(this);
        }
        else if (riid == __uuidof(IAudioProcessingObjectConfiguration))
        {
            *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
        }
        else if (riid == __uuidof(IAudioSystemEffects))
        {
            *ppv = static_cast<IAudioSystemEffects*>(static_cast<IAudioSystemEffects3*>(this));
        }
        else if (riid == __uuidof(IAudioSystemEffects2))
        {
            *ppv = static_cast<IAudioSystemEffects2*>(static_cast<IAudioSystemEffects3*>(this));
        }
        else if (riid == __uuidof(IAudioSystemEffects3))
        {
            *ppv = static_cast<IAudioSystemEffects3*>(this);
        }
        else if (riid == __uuidof(IAudioProcessingObjectNotifications))
        {
            *ppv = static_cast<IAudioProcessingObjectNotifications*>(this);
        }
        else
        {
            // Diagnose: welche Schnittstelle wollte die Engine, die wir nicht haben?
            if (m_map.data && m_map.writable)
            {
                m_map.data->qiFailCount++;
                const unsigned* raw = reinterpret_cast<const unsigned*>(&riid);
                for (int i = 0; i < 4; ++i) m_map.data->lastFailIid[i] = raw[i];
            }
#ifdef QBOOST_FORCE_GAIN
            {
                char line[128];
                sprintf_s(line, "QI-ABGELEHNT {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
                          riid.Data1, riid.Data2, riid.Data3,
                          riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
                          riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
                QBoostMark(L"diag.txt", line);
            }
#endif
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
#ifdef QBOOST_FORCE_GAIN
        {
            char l[128];
            sprintf_s(l, "QI-OK %08lX-%04X-%04X\n", riid.Data1, riid.Data2, riid.Data3);
            QBoostMark(nullptr, l);
        }
#endif
        InnerAddRef();          // NICHT delegieren: eigene Lebensdauer zaehlen
        return S_OK;
    }

    ULONG InnerAddRef() { return InterlockedIncrement(&m_ref); }

    ULONG InnerRelease()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // ---- IAudioProcessingObject -------------------------------------------
    STDMETHOD(Reset)() override { return S_OK; }

    STDMETHOD(GetLatency)(HNSTIME* pTime) override
    {
        if (!pTime) return E_POINTER;
        *pTime = 0;                 // reiner Gain, keine zusaetzliche Latenz
        return S_OK;
    }

    STDMETHOD(GetRegistrationProperties)(APO_REG_PROPERTIES** ppRegProps) override
    {
        if (!ppRegProps) return E_POINTER;
        QBOOST_MARK(nullptr, "GETREGPROPS\n");
        auto* p = static_cast<APO_REG_PROPERTIES*>(
            CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
        if (!p) return E_OUTOFMEMORY;
        FillRegProps(*p);
        *ppRegProps = p;
        return S_OK;
    }

    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE* pbyData) override
    {
        UNREFERENCED_PARAMETER(cbDataSize);
        UNREFERENCED_PARAMETER(pbyData);
        if (m_map.data && m_map.writable) m_map.data->initCount++;
#ifdef QBOOST_FORCE_GAIN
        {
            char l[220];
            // Layout nicht raten: den Puffer nach bekannten Modus-Kennungen
            // absuchen und melden, welcher Modus wo steht.
            static const struct { const char* name; GUID id; } kModes[] = {
              { "DEFAULT",        { 0xc18e2f7e,0x933d,0x4965,{0xb7,0xd1,0x1e,0xef,0x22,0x8d,0x2a,0xf3} } },
              { "RAW",            { 0x9e90ea20,0xb493,0x4fd1,{0xa1,0xa8,0x7e,0x13,0x61,0xa9,0x56,0xcf} } },
              { "COMMUNICATIONS", { 0x98951333,0xb9cd,0x48b1,{0xa0,0xa3,0xff,0x40,0x68,0x2d,0x73,0xf7} } },
              { "NOTIFICATION",   { 0x9cf2a70b,0xf377,0x403b,{0xbd,0x6b,0x36,0x08,0x63,0xe0,0x35,0x5c} } },
              { "MEDIA",          { 0x4780004e,0x7133,0x41d8,{0x8c,0x74,0x66,0x0d,0xad,0xd2,0xc0,0xee} } },
              { "MOVIE",          { 0xb26feb0d,0xec94,0x477c,{0x94,0x94,0xd1,0xab,0x8e,0x75,0x3f,0x6e} } },
            };
            const char* found = "keiner";
            int at = -1;
            if (pbyData && cbDataSize >= sizeof(GUID))
            {
                for (UINT32 off = 0; off + sizeof(GUID) <= cbDataSize && at < 0; off += 4)
                    for (int k = 0; k < 6; ++k)
                        if (memcmp(pbyData + off, &kModes[k].id, sizeof(GUID)) == 0)
                        { found = kModes[k].name; at = (int)off; break; }
            }
            sprintf_s(l, "INITIALIZE cbData=%u modus=%s @%d\n", cbDataSize, found, at);
            QBoostMark(nullptr, l);
        }
#endif
        return S_OK;                // keine Init-Daten erforderlich
    }

    STDMETHOD(IsInputFormatSupported)(IAudioMediaType* pOpposite,
                                      IAudioMediaType* pRequested,
                                      IAudioMediaType** ppSupported) override
    {
        return AcceptFormat(pOpposite, pRequested, ppSupported);
    }

    STDMETHOD(IsOutputFormatSupported)(IAudioMediaType* pOpposite,
                                       IAudioMediaType* pRequested,
                                       IAudioMediaType** ppSupported) override
    {
        return AcceptFormat(pOpposite, pRequested, ppSupported);
    }

    STDMETHOD(GetInputChannelCount)(UINT32* pCount) override
    {
        QBOOST_MARK(nullptr, "GETINPUTCHANNELCOUNT\n");
        if (!pCount) return E_POINTER;
        *pCount = m_channels ? m_channels : 2;
        return S_OK;
    }

    // ---- IAudioProcessingObjectConfiguration ------------------------------
    STDMETHOD(LockForProcess)(UINT32 nIn, APO_CONNECTION_DESCRIPTOR** ppIn,
                              UINT32 nOut, APO_CONNECTION_DESCRIPTOR** ppOut) override
    {
        QBOOST_MARK(L"diag.txt", "LOCKFORPROCESS\n");
        if (nIn < 1 || nOut < 1 || !ppIn || !ppOut || !ppIn[0]) return E_INVALIDARG;

        // Ein- und Ausgangsformat streng pruefen. Lieber hier sauber ablehnen
        // (Windows nimmt uns dann aus der Kette und der Ton laeuft normal
        // weiter) als spaeter falsch rechnen und Stille erzeugen.
        if (!ppOut[0] || !ppIn[0]->pFormat || !ppOut[0]->pFormat)
            return APOERR_FORMAT_NOT_SUPPORTED;

        UNCOMPRESSEDAUDIOFORMAT fin{}, fout{};
        if (FAILED(ppIn[0]->pFormat->GetUncompressedAudioFormat(&fin)) ||
            FAILED(ppOut[0]->pFormat->GetUncompressedAudioFormat(&fout)))
            return APOERR_FORMAT_NOT_SUPPORTED;

        if (fin.guidFormatType != QBOOST_SUBTYPE_IEEE_FLOAT ||
            fin.dwBytesPerSampleContainer != 4 ||
            fin.dwValidBitsPerSample      != 32 ||
            fin.dwSamplesPerFrame          == 0 ||
            fin.dwSamplesPerFrame != fout.dwSamplesPerFrame ||
            fin.fFramesPerSecond  != fout.fFramesPerSecond  ||
            fout.guidFormatType   != QBOOST_SUBTYPE_IEEE_FLOAT)
        {
            QBOOST_MARK(nullptr, "LOCK ABGELEHNT (Format)\n");
            return APOERR_FORMAT_NOT_SUPPORTED;
        }
        QBOOST_MARK(nullptr, "LOCK OK\n");

        m_channels = fin.dwSamplesPerFrame;
        m_isFloat  = true;
        OpenShared();               // Gain-Quelle anbinden

        // Diagnose: festhalten, dass und womit gesperrt wurde.
        if (m_map.data && m_map.writable)
        {
            m_map.data->lockCount++;
            m_map.data->channels = m_channels;
            unsigned f = 0;
            if (m_isFloat) f |= 1u;
            if (ppIn[0]->pBuffer == ppOut[0]->pBuffer) f |= 2u;
            f |= 4u;                                   // Mapping ist schreibbar
            m_map.data->flags = f;
        }
        m_locked = true;
        return S_OK;
    }

    STDMETHOD(UnlockForProcess)() override
    {
        m_locked = false;
        CloseShared();
        return S_OK;
    }

    // ---- IAudioProcessingObjectRT (Echtzeit-Pfad!) ------------------------
    // Kein malloc, kein Lock, kein Logging, keine transzendenten Funktionen.
    STDMETHOD_(void, APOProcess)(UINT32 nIn, APO_CONNECTION_PROPERTY** ppIn,
                                 UINT32 nOut, APO_CONNECTION_PROPERTY** ppOut) override
    {
        if (nIn == 0 || nOut == 0 || !ppIn || !ppOut) return;
        APO_CONNECTION_PROPERTY* in  = ppIn[0];
        APO_CONNECTION_PROPERTY* out = ppOut[0];
        if (!in || !out) return;
#ifdef QBOOST_FORCE_GAIN
        // Nur den allerersten Aufruf melden (Echtzeitpfad nicht fluten).
        if (!m_processReported)
        {
            m_processReported = true;
            char l[160];
            sprintf_s(l, "PROCESS frames=%u flags=%u inplace=%d ch=%u\n",
                      in->u32ValidFrameCount, (unsigned)in->u32BufferFlags,
                      (in->pBuffer == out->pBuffer) ? 1 : 0, m_channels);
            QBoostMark(nullptr, l);
        }
#endif

        const UINT32 frames = in->u32ValidFrameCount;

        if (in->u32BufferFlags == BUFFER_SILENT)
        {
            out->u32ValidFrameCount = frames;
            out->u32BufferFlags     = BUFFER_SILENT;
            return;
        }

        float* src = reinterpret_cast<float*>(in->pBuffer);
        float* dst = reinterpret_cast<float*>(out->pBuffer);
        const UINT32 count = frames * m_channels;

        float g  = 1.0f;
        bool  on = false;
#ifdef QBOOST_FORCE_GAIN
        // Diagnosebau: fester Verstaerkungsfaktor, KEIN Zugriff auf die
        // Zustandsdatei. Dient dem Nachweis, ob die APO ueberhaupt laeuft.
        g  = (float)(QBOOST_FORCE_GAIN);
        on = true;
        QBoostShared* s = nullptr;
#else
        QBoostShared* s = m_map.data;
        if (s && m_map.writable) s->processCount++;   // Diagnose
        if (s && s->magic == QBOOST_MAGIC)
        {
            on = (s->enabled != 0);
            g  = s->gain;
            if (g < QBOOST_GAIN_MIN) g = QBOOST_GAIN_MIN;
            if (g > QBOOST_GAIN_MAX) g = QBOOST_GAIN_MAX;
        }
#endif

        if (m_isFloat && on && g != 1.0f)
        {
            float peak = 0.0f;
            for (UINT32 i = 0; i < count; ++i)
            {
                float y = src[i] * g;
                if (y >  1.0f) y =  1.0f;   // harter Begrenzer gegen Clipping-Ueberlauf
                else if (y < -1.0f) y = -1.0f;
                float a = y < 0 ? -y : y;
                if (a > peak) peak = a;
                dst[i] = y;
            }
            if (s && m_map.writable) s->peak = static_cast<unsigned int>(peak * 1000.0f);
        }
        else if (dst != src)
        {
            memcpy(dst, src, static_cast<size_t>(count) * sizeof(float));
        }

        out->u32ValidFrameCount = frames;
        out->u32BufferFlags     = BUFFER_VALID;
    }

    STDMETHOD_(UINT32, CalcInputFrames)(UINT32 outFrames) override  { return outFrames; }
    STDMETHOD_(UINT32, CalcOutputFrames)(UINT32 inFrames) override  { return inFrames; }

    // ---- IAudioSystemEffects2 / 3 -----------------------------------------
    // Quadux Boost bietet keine schaltbaren Windows-Systemeffekte an, meldet
    // also eine leere Liste. Wichtig ist, dass die Aufrufe ERFOLGREICH sind.
    // Wir melden GENAU EINEN Effekt (unsere eigene Kennung). Eine leere Liste
    // fuehrt dazu, dass Windows die APO als wirkungslos einstuft und gar nicht
    // erst in den Verarbeitungsgraphen einhaengt.
    STDMETHOD(GetEffectsList)(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE Event) override
    {
        QBOOST_MARK(nullptr, "GETEFFECTSLIST\n");
        UNREFERENCED_PARAMETER(Event);
        if (!ppEffectsIds || !pcEffects) return E_POINTER;
        auto* ids = static_cast<LPGUID>(CoTaskMemAlloc(sizeof(GUID)));
        if (!ids) return E_OUTOFMEMORY;
        *ids = CLSID_QuaduxBoostApo;
        *ppEffectsIds = ids;
        *pcEffects    = 1;
        return S_OK;
    }

    STDMETHOD(GetControllableSystemEffectsList)(AUDIO_SYSTEMEFFECT** effects,
                                                UINT* numEffects, HANDLE event) override
    {
        QBOOST_MARK(nullptr, "GETCONTROLLABLEEFFECTS\n");
        UNREFERENCED_PARAMETER(event);
        if (!effects || !numEffects) return E_POINTER;
        auto* e = static_cast<AUDIO_SYSTEMEFFECT*>(CoTaskMemAlloc(sizeof(AUDIO_SYSTEMEFFECT)));
        if (!e) return E_OUTOFMEMORY;
        ZeroMemory(e, sizeof(*e));
        e->id          = CLSID_QuaduxBoostApo;
        e->canSetState = TRUE;
        e->state       = AUDIO_SYSTEMEFFECT_STATE_ON;
        *effects    = e;
        *numEffects = 1;
        return S_OK;
    }

    STDMETHOD(SetAudioSystemEffectState)(GUID effectId, AUDIO_SYSTEMEFFECT_STATE state) override
    {
        UNREFERENCED_PARAMETER(effectId);
        UNREFERENCED_PARAMETER(state);
        return S_OK;
    }

    // ---- IAudioProcessingObjectNotifications ------------------------------
    // Wir wollen keine Benachrichtigungen, melden also eine leere Liste.
    STDMETHOD(GetApoNotificationRegistrationInfo)(
        APO_NOTIFICATION_DESCRIPTOR** apoNotifications, DWORD* count) override
    {
        QBOOST_MARK(nullptr, "NOTIFICATIONREGINFO\n");
        if (apoNotifications) *apoNotifications = nullptr;
        if (count)            *count            = 0;
        return S_OK;
    }

    STDMETHOD_(void, HandleNotification)(APO_NOTIFICATION* apoNotification) override
    {
        UNREFERENCED_PARAMETER(apoNotification);
    }

    // ---- Registrierungs-Eigenschaften (auch von DllRegisterServer genutzt) -
    static void FillRegProps(APO_REG_PROPERTIES& p)
    {
        ZeroMemory(&p, sizeof(p));
        p.clsid   = CLSID_QuaduxBoostApo;
        p.Flags   = static_cast<APO_FLAG>(APO_FLAG_INPLACE |
                                          APO_FLAG_SAMPLESPERFRAME_MUST_MATCH |
                                          APO_FLAG_FRAMESPERSECOND_MUST_MATCH |
                                          APO_FLAG_BITSPERSAMPLE_MUST_MATCH);
        wcscpy_s(p.szFriendlyName,  ARRAYSIZE(p.szFriendlyName),  kFriendlyName);
        wcscpy_s(p.szCopyrightInfo, ARRAYSIZE(p.szCopyrightInfo), L"(c) Quadux IT - Apache-2.0");
        p.u32MajorVersion        = 1;
        p.u32MinorVersion        = 0;
        p.u32MinInputConnections  = 1;
        p.u32MaxInputConnections  = 1;
        p.u32MinOutputConnections = 1;
        p.u32MaxOutputConnections = 1;
        p.u32MaxInstances        = APO_MAX_INSTANCES;
        // Flags exakt wie der Windows-eigene GFX-APO (13 = INPLACE |
        // FRAMESPERSECOND_MUST_MATCH | BITSPERSAMPLE_MUST_MATCH). Die
        // Kanalzahl bewusst NICHT erzwingen.
        p.Flags = static_cast<APO_FLAG>(APO_FLAG_INPLACE |
                                        APO_FLAG_FRAMESPERSECOND_MUST_MATCH |
                                        APO_FLAG_BITSPERSAMPLE_MUST_MATCH);
        p.u32NumAPOInterfaces    = 1;
        p.iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);
    }

private:
    enum { APO_MAX_INSTANCES = 0xFFFFFFFF };

    static bool IsFloat32(const WAVEFORMATEX* wfx)
    {
        if (wfx->wBitsPerSample != 32) return false;
        if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22)
        {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
            return ext->SubFormat == QBOOST_SUBTYPE_IEEE_FLOAT;
        }
        return false;
    }

    // Prueft, ob wir ein Format WIRKLICH verarbeiten koennen.
    //
    // Frueher wurde hier jedes Format bedingungslos akzeptiert. Das war der
    // Grund fuer komplette Stille: die Engine handelte daraufhin auch
    // Kombinationen aus (etwa abweichende Kanalzahl zwischen Ein- und
    // Ausgang), die APOProcess nicht korrekt bedienen kann.
    //
    // Vertrag laut SDK:
    //   S_OK                        Format wird unveraendert unterstuetzt
    //   APOERR_FORMAT_NOT_SUPPORTED Format nicht unterstuetzt (ppSupported bleibt unberuehrt)
    // Bei Ablehnung nimmt Windows die APO sauber aus der Kette - der Ton
    // laeuft dann ohne Verstaerkung weiter, statt auszufallen.
    HRESULT AcceptFormat(IAudioMediaType* pOppositeFormat,
                         IAudioMediaType* pRequested,
                         IAudioMediaType** ppSupported)
    {
        if (m_map.data && m_map.writable) m_map.data->fmtCount++;
        QBOOST_MARK(L"diag.txt", "FORMATCHECK\n");
        if (!ppSupported || !pRequested) return E_POINTER;

        UNCOMPRESSEDAUDIOFORMAT req{};
        if (FAILED(pRequested->GetUncompressedAudioFormat(&req)))
            return APOERR_FORMAT_NOT_SUPPORTED;
#ifdef QBOOST_FORCE_GAIN
        {
            char l[192];
            sprintf_s(l, "FMT req: type=%08lX ch=%u bytes=%u bits=%u rate=%.0f opp=%s\n",
                      req.guidFormatType.Data1, req.dwSamplesPerFrame,
                      req.dwBytesPerSampleContainer, req.dwValidBitsPerSample,
                      req.fFramesPerSecond, pOppositeFormat ? "ja" : "nein");
            QBoostMark(nullptr, l);
        }
#endif
        // Wir rechnen ausschliesslich in 32-Bit-Gleitkomma.
        if (req.guidFormatType        != QBOOST_SUBTYPE_IEEE_FLOAT ||
            req.dwBytesPerSampleContainer != 4 ||
            req.dwValidBitsPerSample      != 32 ||
            req.dwSamplesPerFrame          == 0)
        {
            QBOOST_MARK(nullptr, "FMT ABGELEHNT (kein float32)\n");
            return APOERR_FORMAT_NOT_SUPPORTED;
        }

        // Reiner Gain: Kanalzahl und Abtastrate muessen auf beiden Seiten
        // uebereinstimmen. Misch- oder Umtastarbeit uebernehmen wir nicht.
        if (pOppositeFormat)
        {
            UNCOMPRESSEDAUDIOFORMAT opp{};
            if (FAILED(pOppositeFormat->GetUncompressedAudioFormat(&opp)))
                return APOERR_FORMAT_NOT_SUPPORTED;
            if (opp.dwSamplesPerFrame != req.dwSamplesPerFrame ||
                opp.fFramesPerSecond  != req.fFramesPerSecond)
            {
                QBOOST_MARK(nullptr, "FMT ABGELEHNT (Kanal/Rate ungleich)\n");
                return APOERR_FORMAT_NOT_SUPPORTED;
            }
        }

        QBOOST_MARK(nullptr, "FMT AKZEPTIERT\n");
        *ppSupported = pRequested;
        pRequested->AddRef();
        return S_OK;
    }

    void OpenShared()
    {
#ifdef QBOOST_FORCE_GAIN
        return;                     // Diagnosebau: bewusst kein Dateizugriff
#else
        if (m_map.data) return;
        // Dateigestuetztes Mapping. create=true, damit die APO das Segment
        // anlegen kann, falls das CLI noch nicht lief (audiodg besitzt das
        // Global-Privileg nicht, kann aber Dateien in ProgramData mappen).
        QBoostOpen(&m_map, true);
#endif
    }

    void CloseShared()
    {
        QBoostClose(&m_map);
    }

    LONG          m_ref;
    CInner        m_inner;              // nicht delegierendes IUnknown
    IUnknown*     m_controlling = nullptr;  // aeusseres bzw. eigenes IUnknown
    UINT32        m_channels = 2;
    bool          m_isFloat  = false;
    bool          m_locked   = false;
    bool          m_processReported = false;   // nur fuer den Diagnosebau
    QBoostMapping m_map{};

public:
    virtual ~CBoostApo()
    {
        CloseShared();
        InterlockedDecrement(&g_lockCount);
    }
};

// =============================================================================
//  Klassenfabrik
// =============================================================================
class CBoostApoFactory final : public IClassFactory
{
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory))
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override  { return 2; }   // statische Singleton-Fabrik
    STDMETHOD_(ULONG, Release)() override { return 1; }

    STDMETHOD(CreateInstance)(IUnknown* pOuter, REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;

        QBOOST_MARK(nullptr, pOuter ? "CREATEINSTANCE (aggregiert)\n"
                                    : "CREATEINSTANCE (eigenstaendig)\n");

        // COM-Regel: bei Aggregation darf ausschliesslich IUnknown
        // angefordert werden.
        if (pOuter && riid != __uuidof(IUnknown))
        {
            QBOOST_MARK(nullptr, "CREATEINSTANCE abgelehnt (riid != IUnknown)\n");
            return CLASS_E_NOAGGREGATION;
        }

        CBoostApo* obj = new (std::nothrow) CBoostApo(pOuter);
        if (!obj) return E_OUTOFMEMORY;

        if (pOuter)
        {
            // Das innere IUnknown zurueckgeben; Referenz gehoert dem Aeusseren.
            *ppv = obj->InnerUnknown();
            return S_OK;
        }

        HRESULT hr = obj->InnerQueryInterface(riid, ppv);
        obj->InnerRelease();
        return hr;
    }

    STDMETHOD(LockServer)(BOOL lock) override
    {
        if (lock) InterlockedIncrement(&g_lockCount);
        else      InterlockedDecrement(&g_lockCount);
        return S_OK;
    }
};

static CBoostApoFactory g_factory;

// =============================================================================
//  DLL-Exporte
// =============================================================================
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
#ifdef QBOOST_FORCE_GAIN
    {
        char l[220];
        sprintf_s(l, "GETCLASSOBJECT clsid=%08lX riid=%08lX-%04X-%04X passt=%d\n",
                  rclsid.Data1, riid.Data1, riid.Data2, riid.Data3,
                  (rclsid == CLSID_QuaduxBoostApo) ? 1 : 0);
        QBoostMark(nullptr, l);
    }
#endif
    if (rclsid != CLSID_QuaduxBoostApo)
    {
        QBOOST_MARK(nullptr, "GETCLASSOBJECT: falsche CLSID\n");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    HRESULT hr = g_factory.QueryInterface(riid, ppv);
#ifdef QBOOST_FORCE_GAIN
    {
        char l[96];
        sprintf_s(l, "GETCLASSOBJECT -> 0x%08lX\n", (unsigned long)hr);
        QBoostMark(nullptr, l);
    }
#endif
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return (g_lockCount == 0) ? S_OK : S_FALSE;
}

// --- Registry-Hilfen ---------------------------------------------------------
static LONG SetString(HKEY key, const wchar_t* name, const wchar_t* val)
{
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(val),
                          static_cast<DWORD>((wcslen(val) + 1) * sizeof(wchar_t)));
}

STDAPI DllRegisterServer()
{
    wchar_t dllPath[MAX_PATH]{};
    if (!GetModuleFileNameW(g_hModule, dllPath, MAX_PATH))
        return HRESULT_FROM_WIN32(GetLastError());

    // 1) COM-Registrierung unter HKLM\SOFTWARE\Classes\CLSID\{clsid}
    wchar_t sub[256];
    swprintf_s(sub, L"SOFTWARE\\Classes\\CLSID\\%s", kClsidString);
    HKEY hClsid = nullptr;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, nullptr, 0,
                             KEY_WRITE, nullptr, &hClsid, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    SetString(hClsid, nullptr, kFriendlyName);

    HKEY hInproc = nullptr;
    r = RegCreateKeyExW(hClsid, L"InprocServer32", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hInproc, nullptr);
    if (r == ERROR_SUCCESS)
    {
        SetString(hInproc, nullptr, dllPath);
        SetString(hInproc, L"ThreadingModel", L"Both");
        RegCloseKey(hInproc);
    }
    RegCloseKey(hClsid);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);

    // 2) APO-Registrierung im System-APO-Katalog
    APO_REG_PROPERTIES props;
    CBoostApo::FillRegProps(props);
    return RegisterAPO(&props);
}

STDAPI DllUnregisterServer()
{
    UnregisterAPO(CLSID_QuaduxBoostApo);

    wchar_t sub[256];
    swprintf_s(sub, L"SOFTWARE\\Classes\\CLSID\\%s", kClsidString);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, sub);
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = hInst;
        DisableThreadLibraryCalls(hInst);
        QBOOST_MARK(L"diag.txt", "DLLMAIN\n");
    }
    return TRUE;
}
