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
// apotest.cpp
// -----------------------------------------------------------------------------
// Testet die APO ISOLIERT im eigenen Prozess: laedt die DLL direkt per
// LoadLibrary + DllGetClassObject (keine Registrierung, kein Admin, kein
// Eingriff ins System) und schickt einen bekannten Audioblock hindurch.
//
// Beantwortet die Frage: rechnet APOProcess korrekt - oder scheitert schon
// die COM-/Format-Aushandlung?
//
// -----------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <audioenginebaseapo.h>
#include <audioapotypes.h>
#include <audiomediatype.h>
#include <mmreg.h>
#include <cstdio>
#include <cmath>
#include <new>

#include "../apo/shared.h"

static const CLSID CLSID_QuaduxBoostApo =
{ 0x02893eae, 0x2eb8, 0x40c6, { 0x80, 0x97, 0xe4, 0x3f, 0x39, 0xf2, 0x10, 0xac } };
static const GUID SUBTYPE_IEEE_FLOAT =
{ 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

#define CHANNELS   2
#define SAMPLERATE 48000
#define FRAMES     480          // 10 ms

// --- Minimale IAudioMediaType-Implementierung (float32 stereo 48k) ----------
class TestMediaType : public IAudioMediaType
{
public:
    TestMediaType() : m_ref(1)
    {
        ZeroMemory(&m_wfx, sizeof(m_wfx));
        m_wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        m_wfx.Format.nChannels       = CHANNELS;
        m_wfx.Format.nSamplesPerSec  = SAMPLERATE;
        m_wfx.Format.wBitsPerSample  = 32;
        m_wfx.Format.nBlockAlign     = CHANNELS * 4;
        m_wfx.Format.nAvgBytesPerSec = SAMPLERATE * CHANNELS * 4;
        m_wfx.Format.cbSize          = 22;
        m_wfx.Samples.wValidBitsPerSample = 32;
        m_wfx.dwChannelMask          = 3;   // FL | FR
        m_wfx.SubFormat              = SUBTYPE_IEEE_FLOAT;
    }
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioMediaType))
        { *ppv = static_cast<IAudioMediaType*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override  { return InterlockedIncrement(&m_ref); }
    STDMETHOD_(ULONG, Release)() override
    { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }

    STDMETHOD(IsCompressedFormat)(BOOL* p) override { if (p) *p = FALSE; return S_OK; }
    STDMETHOD(IsEqual)(IAudioMediaType*, DWORD* pdwFlags) override
    { if (pdwFlags) *pdwFlags = 0xFFFFFFFF; return S_OK; }
    STDMETHOD_(const WAVEFORMATEX*, GetAudioFormat)() override
    { return reinterpret_cast<const WAVEFORMATEX*>(&m_wfx); }
    STDMETHOD(GetUncompressedAudioFormat)(UNCOMPRESSEDAUDIOFORMAT* p) override
    {
        if (!p) return E_POINTER;
        p->guidFormatType   = SUBTYPE_IEEE_FLOAT;
        p->dwSamplesPerFrame= CHANNELS;
        p->dwBytesPerSampleContainer = 4;
        p->dwValidBitsPerSample      = 32;
        p->fFramesPerSecond = (float)SAMPLERATE;
        p->dwChannelMask    = 3;
        return S_OK;
    }
private:
    LONG m_ref;
    WAVEFORMATEXTENSIBLE m_wfx;
};

static void Fail(const char* what, HRESULT hr)
{
    printf("  FEHLER bei %s: 0x%08lX\n", what, (unsigned long)hr);
}

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("=== Quadux Boost - Isolierter APO-Test ===\n\n");

    // DLL direkt laden, ohne Registrierung
    HMODULE mod = LoadLibraryW(L"..\\..\\apo\\build\\QuaduxBoostApo.dll");
    if (!mod) mod = LoadLibraryW(L"QuaduxBoostApo.dll");
    if (!mod) { printf("DLL nicht ladbar (%lu)\n", GetLastError()); return 1; }
    printf("[1] DLL geladen.\n");

    typedef HRESULT (STDAPICALLTYPE *PFNGetClassObject)(REFCLSID, REFIID, void**);
    auto pGCO = (PFNGetClassObject)GetProcAddress(mod, "DllGetClassObject");
    if (!pGCO) { printf("DllGetClassObject fehlt\n"); return 1; }

    IClassFactory* factory = nullptr;
    HRESULT hr = pGCO(CLSID_QuaduxBoostApo, __uuidof(IClassFactory), (void**)&factory);
    if (FAILED(hr)) { Fail("DllGetClassObject", hr); return 1; }
    printf("[2] Klassenfabrik erhalten.\n");

    IAudioProcessingObject* apo = nullptr;
    hr = factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject), (void**)&apo);
    factory->Release();
    if (FAILED(hr)) { Fail("CreateInstance", hr); return 1; }
    printf("[3] APO instanziiert.\n");

    IAudioProcessingObjectConfiguration* cfg = nullptr;
    IAudioProcessingObjectRT* rt = nullptr;
    hr = apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration), (void**)&cfg);
    if (FAILED(hr)) { Fail("QI Configuration", hr); return 1; }
    hr = apo->QueryInterface(__uuidof(IAudioProcessingObjectRT), (void**)&rt);
    if (FAILED(hr)) { Fail("QI RT", hr); return 1; }
    printf("[4] Interfaces Configuration + RT vorhanden.\n");

    hr = apo->Initialize(0, nullptr);
    printf("[5] Initialize: 0x%08lX %s\n", (unsigned long)hr, SUCCEEDED(hr) ? "(ok)" : "(FEHLER)");

    // Formataushandlung pruefen
    TestMediaType* mt = new TestMediaType();
    IAudioMediaType* supported = nullptr;
    hr = apo->IsInputFormatSupported(nullptr, mt, &supported);
    printf("[6] IsInputFormatSupported: 0x%08lX %s\n", (unsigned long)hr,
           (hr == S_OK) ? "(akzeptiert)" : (hr == S_FALSE ? "(nur aehnlich)" : "(ABGELEHNT)"));
    if (supported) supported->Release();

    // Puffer vorbereiten (INPLACE: ein Puffer fuer Ein- und Ausgabe)
    const UINT32 count = FRAMES * CHANNELS;
    float* buf = (float*)_aligned_malloc(count * sizeof(float), 16);
    for (UINT32 i = 0; i < count; ++i) buf[i] = 0.25f;   // konstanter Pegel

    APO_CONNECTION_DESCRIPTOR inDesc{}, outDesc{};
    inDesc.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
    inDesc.pBuffer = (UINT_PTR)buf;
    inDesc.u32MaxFrameCount = FRAMES;
    inDesc.pFormat = mt;
    inDesc.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
    outDesc = inDesc;

    APO_CONNECTION_DESCRIPTOR* pIn[1]  = { &inDesc };
    APO_CONNECTION_DESCRIPTOR* pOut[1] = { &outDesc };
    hr = cfg->LockForProcess(1, pIn, 1, pOut);
    printf("[7] LockForProcess: 0x%08lX %s\n", (unsigned long)hr, SUCCEEDED(hr) ? "(ok)" : "(FEHLER)");
    if (FAILED(hr)) { printf("\n>>> Die Kette scheitert hier. <<<\n"); return 1; }

    // Gain auf 2.0 setzen
    QBoostMapping map;
    if (QBoostOpen(&map, true) && map.writable)
    {
        map.data->gain = 2.0f; map.data->enabled = 1;
        printf("[8] Gain in state.bin auf 2.0 gesetzt (schreibbar).\n");
    }
    else printf("[8] WARNUNG: state.bin nicht schreibbar - Test laeuft mit Standard.\n");

    APO_CONNECTION_PROPERTY inProp{}, outProp{};
    inProp.pBuffer = (UINT_PTR)buf;
    inProp.u32ValidFrameCount = FRAMES;
    inProp.u32BufferFlags = BUFFER_VALID;
    inProp.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
    outProp = inProp;

    APO_CONNECTION_PROPERTY* ppIn[1]  = { &inProp };
    APO_CONNECTION_PROPERTY* ppOut[1] = { &outProp };
    rt->APOProcess(1, ppIn, 1, ppOut);

    printf("[9] APOProcess ausgefuehrt.\n");
    printf("    Eingang war 0.25 je Sample, Gain 2.0 -> erwartet 0.50\n");
    printf("    Ergebnis[0]=%.4f  [1]=%.4f  [letztes]=%.4f\n",
           buf[0], buf[1], buf[count-1]);
    printf("    Ausgabe-Flags=%u  ValidFrames=%u\n",
           (unsigned)outProp.u32BufferFlags, outProp.u32ValidFrameCount);

    bool ok = (fabs(buf[0] - 0.5f) < 0.001f);
    printf("\n>>> DSP-Test: %s <<<\n", ok ? "BESTANDEN" : "FEHLGESCHLAGEN");

    if (map.data) { map.data->gain = 1.0f; QBoostClose(&map); }
    cfg->UnlockForProcess();
    rt->Release(); cfg->Release(); apo->Release(); mt->Release();
    _aligned_free(buf);
    CoUninitialize();
    return ok ? 0 : 1;
}
