//#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h> // Notification Area Icon
#include <wchar.h>
#include <AudioClient.h>
#include <mfapi.h>
#include <mmdeviceapi.h>

#include "SendStereoMixToPulseAudioServer.h"

#pragma pack(1)
#include "pulse/simple.h" // PulseAudio pa_simple API

#pragma comment(lib, "libpulse-simple.dll.a")
#pragma comment(lib, "avrt.lib")   // multimedia class scheduler Thread Priority
#pragma comment(lib, "mfplat.lib") // Media Foundation WASAPI

HINSTANCE g_hInst;
char g_PulseAudioServer[100];
volatile BOOL g_working = TRUE;
LPCWSTR g_szTitle = L"Send StereoMix to Pulseaudio server";
LPCWSTR g_szWindowClass = L"SendStereoMixToPulseAudioServer";
HICON g_PULSEAUDIOICON;
HICON g_PULSEAUDIOICONGRAY;
NOTIFYICONDATA nid = { 0 };

static BOOL NotifyIcon_SetTipText(CONST WCHAR *format, ...)
{
    if (!format) return FALSE;

    va_list args;
    va_start(args, format);
    int ret = vswprintf_s(nid.szTip, _countof(nid.szTip), format, args);
    va_end(args);

    nid.uFlags = NIF_TIP | NIF_INFO;
    wcscpy_s(nid.szInfo, _countof(nid.szInfo), nid.szTip);
    return Shell_NotifyIcon(NIM_MODIFY, &nid);
}

static BOOL NotifyIcon_SetIcon(HICON hIcon)
{
    nid.uFlags = NIF_ICON;
    nid.hIcon = hIcon;
    return Shell_NotifyIcon(NIM_MODIFY, &nid);
}

#define GOTO_IF_FAILED_UPDATE_TIP(expr, label) \
    do { \
        const HRESULT _hr = (expr); \
        if (FAILED(_hr)) { \
            NotifyIcon_SetTipText(L"ERROR: %S failed with HRESULT=0x%08lx", L ## #expr, _hr); \
            goto label; \
        } \
    } while(0)


static DWORD WINAPI StreamThread(LPVOID lpParam)
{
    // AvThreadPriority
    HANDLE hAvTaskThread = nullptr;
    DWORD dwAvTaskIndex = 0;

    // WASAPI
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    WAVEFORMATEX WaveCaptureFormat = { 0 };
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pAudioCaptureClient = nullptr;
    HANDLE hCaptureReady = nullptr;

    // pulseaudio
    int pa_error=0;
    pa_simple* s = NULL;

    GOTO_IF_FAILED_UPDATE_TIP(CoInitializeEx(nullptr, COINIT_MULTITHREADED), finish);
    GOTO_IF_FAILED_UPDATE_TIP(MFStartup(MF_VERSION, MFSTARTUP_LITE), finish);

    // Set high priority
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    hAvTaskThread = AvSetMmThreadCharacteristicsW(L"Pro Audio", &dwAvTaskIndex);
    if (hAvTaskThread)
    {
        AvSetMmThreadPriority(hAvTaskThread, AVRT_PRIORITY_CRITICAL);
    }

    GOTO_IF_FAILED_UPDATE_TIP(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator), finish);

    GOTO_IF_FAILED_UPDATE_TIP(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice), finish);

    GOTO_IF_FAILED_UPDATE_TIP(pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        (void**)&pAudioClient), finish);

    WaveCaptureFormat.wFormatTag = WAVE_FORMAT_PCM;
    WaveCaptureFormat.nChannels = 2;
    WaveCaptureFormat.nSamplesPerSec = 44100;
    WaveCaptureFormat.wBitsPerSample = 16;
#define BITS_PER_BYTE 8
    WaveCaptureFormat.nBlockAlign = WaveCaptureFormat.nChannels * WaveCaptureFormat.wBitsPerSample / BITS_PER_BYTE;
    WaveCaptureFormat.nAvgBytesPerSec = WaveCaptureFormat.nSamplesPerSec * WaveCaptureFormat.nBlockAlign;

    GOTO_IF_FAILED_UPDATE_TIP(pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        (REFERENCE_TIME)2'000'000, // request shared audiodata buffer 2'000'000 = 0.2s 
        0,
        &WaveCaptureFormat,
        nullptr), finish);

    GOTO_IF_FAILED_UPDATE_TIP(pAudioClient->GetService(__uuidof(IAudioCaptureClient),
        (void**)&pAudioCaptureClient), finish);

    // Set event callback handle for audiodata ready notifications
    hCaptureReady = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (hCaptureReady == nullptr)
    {
        NotifyIcon_SetTipText(L"ERROR: CreateEvent failed.");
        goto finish;
    }
    GOTO_IF_FAILED_UPDATE_TIP(pAudioClient->SetEventHandle(hCaptureReady), finish);

    // PulseAudio Sample format to use
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = WaveCaptureFormat.nSamplesPerSec;
    ss.channels = (uint8_t)WaveCaptureFormat.nChannels;

    if (!(s = pa_simple_new(g_PulseAudioServer, "SendStereoMixToPulseAudioServer.exe", PA_STREAM_PLAYBACK,
        NULL, "StereoMix", &ss, NULL, NULL, &pa_error)))
    {
        NotifyIcon_SetTipText(L"ERROR: pa_simple_new() failed: %d", pa_error);
        goto finish;
    }

    GOTO_IF_FAILED_UPDATE_TIP(pAudioClient->Start(), finish);

    NotifyIcon_SetTipText(L"Connected to Pulseaudio@%S.\nStreaming.", g_PulseAudioServer);
    NotifyIcon_SetIcon(g_PULSEAUDIOICON);

    while (g_working)
    {
        // Wait for capture ready event with timeout
        DWORD WaitStatus = WaitForSingleObject(hCaptureReady, 10000);
        if (WaitStatus != WAIT_OBJECT_0)
        {
            if (WaitStatus == WAIT_TIMEOUT)
            {
                // No audio data for 10 seconds, continue waiting
                continue;
            }
            else
            {
                break; // something went wrong
            }
        }

        // Process all available packets
        UINT32 FramesInNextPacket = 0;
        int PacketsInSeries = 0;
        HRESULT hr = pAudioCaptureClient->GetNextPacketSize(&FramesInNextPacket);
        while (SUCCEEDED(hr) && FramesInNextPacket > 0)
        {
            BYTE* pData = nullptr;
            UINT32 FramesToRead = 0;
            DWORD flags = 0;
            UINT64 DevicePosition = 0;
            UINT64 QPCPosition = 0;

            hr = pAudioCaptureClient->GetBuffer(&pData, &FramesToRead, &flags, &DevicePosition, &QPCPosition);
            if (SUCCEEDED(hr))
            {
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                {
                    // TODO: statistic
                }

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                {
                    // TODO: statistic
                }
                else
                {
                    if (++PacketsInSeries > 20)
                    {
                        // Do not send one packet to PulseAudio server.
                        // Due to network or cpu heavy load, program can not send all audio data
                        // thus audio delay occur, to fix that, skip one packet of audio data
                    }
                    else
                    {
                        UINT32 BufferSize = FramesToRead * WaveCaptureFormat.nBlockAlign;
                        // pa_simple_write - is blocking until all data has been transmitted
                        if (pa_simple_write(s, pData, (size_t)BufferSize, &pa_error) < 0)
                        {
                            NotifyIcon_SetTipText(L"ERROR: pa_simple_write() failed: %d", pa_error);
                            goto finish;
                        }
                    }
                }
                pAudioCaptureClient->ReleaseBuffer(FramesToRead);
            }
            hr = pAudioCaptureClient->GetNextPacketSize(&FramesInNextPacket);
        }
    }

    NotifyIcon_SetTipText(L"Idle.");
finish:
    if (pAudioClient) pAudioClient->Stop();
    if (s) pa_simple_free(s);
    if (pAudioCaptureClient) pAudioCaptureClient->Release();
    if (pAudioClient) pAudioClient->Release();
    if (pDevice) pDevice->Release();
    if (pEnumerator) pEnumerator->Release();
    if (hCaptureReady) CloseHandle(hCaptureReady);

    if (hAvTaskThread) AvRevertMmThreadCharacteristics(hAvTaskThread);
    MFShutdown();
    CoUninitialize();

    NotifyIcon_SetIcon(g_PULSEAUDIOICONGRAY);
    g_working = FALSE;
    SendMessage(nid.hWnd, WM_COMMAND, IDM_STOPSTREAMING, 0); // Update popup menu

    return 0;
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    static HMENU hPopupMenu;
    static HANDLE hStreamingThread;
    switch (message)
    {
    case WM_CREATE:
        hPopupMenu = CreatePopupMenu();
        AppendMenu(hPopupMenu, MF_STRING, IDM_STARTSTREAMING, L"Start streaming");
        AppendMenu(hPopupMenu, MF_SEPARATOR, 0, NULL);
        AppendMenu(hPopupMenu, MF_STRING, IDM_EXIT, L"Exit Program");
        SendMessage(hWnd, WM_COMMAND, IDM_STARTSTREAMING, 0); // Autostart streaming
        break;
    case WM_TRAYMSG: // Message from tray icon
        switch (lParam) {
        case WM_RBUTTONUP: // Right click - show context menu
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hWnd); // Bring window to foreground so menu doesn't vanish immediately
            TrackPopupMenu(hPopupMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            break;
        case WM_LBUTTONUP:
            break;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDM_STARTSTREAMING:
            if (hStreamingThread != NULL) {
                CloseHandle(hStreamingThread);
                hStreamingThread = NULL;
            }
            g_working = TRUE;
            hStreamingThread = CreateThread(NULL, 0, StreamThread, NULL, 0, NULL);
            if (hStreamingThread == NULL)
            {
                g_working = FALSE;
            }
            else
            {
                ModifyMenu(hPopupMenu, 0, MF_BYPOSITION | MF_STRING, IDM_STOPSTREAMING, L"Stop streaming");
            }
            break;
        case IDM_STOPSTREAMING:
            g_working = FALSE;
            ModifyMenu(hPopupMenu, 0, MF_BYPOSITION | MF_STRING, IDM_STARTSTREAMING, L"Start streaming");
            break;
        case IDM_EXIT:
            g_working = FALSE;
            WaitForSingleObject(hStreamingThread, 10'000); // wait thread exit max 10s
            CloseHandle(hStreamingThread);
            DestroyMenu(hPopupMenu);
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);

    g_hInst = hInstance;

    if (NULL != FindWindow(g_szWindowClass, NULL)) {
        return -1;
    }

    if (lstrlen(lpCmdLine) < 1) {
        MessageBox(NULL, L"Error!\nUsage : SendStereoMixToPulseAudioServer.exe Name/IP", g_szTitle, MB_ICONERROR);
        return -1;
    }
    wcstombs_s(NULL, g_PulseAudioServer, sizeof(g_PulseAudioServer), lpCmdLine, sizeof(g_PulseAudioServer) - 1);

    g_PULSEAUDIOICON     = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_PULSEAUDIOICON));
    g_PULSEAUDIOICONGRAY = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_PULSEAUDIOICONGRAY));

    WNDCLASS wc = { 0 };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hInst;
    wc.hIcon = g_PULSEAUDIOICON;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = g_szWindowClass;
    RegisterClass(&wc);

    HWND hWnd = CreateWindowEx(WS_EX_NOACTIVATE, g_szWindowClass, g_szTitle,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL, g_hInst, NULL);
    if (!hWnd) {
        MessageBox(NULL, L"Failed to create window", g_szTitle, MB_ICONERROR);
        return FALSE;
    }

    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hWnd;
    nid.uID = 100; // not used
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYMSG; // Message sent to Window Proc on clicks
    nid.hIcon = g_PULSEAUDIOICONGRAY;  // Set gray Pulseaudio logo to tray icon at startup
    nid.hBalloonIcon = g_PULSEAUDIOICON;
//    wcscpy_s(nid.szInfoTitle, _countof(nid.szInfoTitle), g_szTitle);
    wcscpy_s(nid.szTip, _countof(nid.szTip), g_szTitle );
    Shell_NotifyIcon(NIM_ADD, &nid);   // Show tray icon

    MSG msg = { 0 };
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Shell_NotifyIcon(NIM_DELETE, &nid);
    return (int)msg.wParam;
}
