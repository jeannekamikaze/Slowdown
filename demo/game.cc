#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

// Set to 1 to throttle frame time for experimenting with different methods.
// Set to 0 to test the slowdown hook.
#define THROTTLE_FRAME_TIME 0

static const char* WINDOW_CLASS_NAME = "Game";

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

static void BusyWaitMicros(LONGLONG micros)
{
    LARGE_INTEGER freq, start, now;

    QueryPerformanceFrequency(&freq);
    assert(freq.QuadPart > 0);
    const LONGLONG freqUs = freq.QuadPart / 1'000'000;

    LONGLONG elapsed;
    QueryPerformanceCounter(&start);
    do
    {
        QueryPerformanceCounter(&now);
        elapsed = (now.QuadPart - start.QuadPart) / freqUs;
    }
    while (elapsed < micros);
}

static void WaitMicros(LONGLONG micros)
{
    LARGE_INTEGER freq, start, now;

    QueryPerformanceFrequency(&freq);
    assert(freq.QuadPart > 0);
    const LONGLONG freqUs = freq.QuadPart / 1'000'000;

    LONGLONG elapsed;
    QueryPerformanceCounter(&start);
    do
    {
        QueryPerformanceCounter(&now);
        elapsed = (now.QuadPart - start.QuadPart) / freqUs;
        const LONGLONG remainingUs = micros - elapsed;
        if (remainingUs > 4'000)
        {
            Sleep(1);
        }
        // else busy-wait.
    }
    while (elapsed < micros);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hInstPrev, PSTR cmdline, int cmdshow)
{
    WNDCLASS wc
    {
        .lpfnWndProc   = WindowProc,
        .hInstance     = hInstance,
        .hCursor       = LoadCursor(NULL, IDC_ARROW),
        .lpszClassName = WINDOW_CLASS_NAME,
    };

    if (!RegisterClass(&wc))
    {
        return -1;
    }

    HWND hwnd = CreateWindowEx(
        0,
        WINDOW_CLASS_NAME,
        "Game",
        WS_OVERLAPPEDWINDOW/*WS_POPUP | WS_VISIBLE*/,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        480,
        NULL,
        NULL,
        hInstance,
        NULL
    );
    if (!hwnd)
    {
        return -1;
    }
    ShowWindow(hwnd, cmdshow);

    HDC hdc = GetDC(hwnd);

    LARGE_INTEGER freq, prev, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    const LONGLONG freqUs = freq.QuadPart / 1'000'000ULL;

    bool running = true;
    MSG msg;

    // Game state example
    LONGLONG gameTimeUs = 0;
#if THROTTLE_FRAME_TIME
    const LONGLONG desiredFrameTimeUs = 10'000; // 10ms.
#endif

    while (running)
    {
        // Poll all pending Windows messages (non-blocking)
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running)
        {
            break;
        }

        // Time step
        QueryPerformanceCounter(&now);
        const LONGLONG elapsedUs = (now.QuadPart - prev.QuadPart) / freqUs;
        prev = now;
        gameTimeUs += elapsedUs;
        const double elapsedMs   = double(elapsedUs)  / double(1'000);
        const double gameTimeSec = double(gameTimeUs) / double(1'000'000);

        // Render; simple GDI example: clear and draw frame time

        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW+1));

        char buf[128];
        const int W = GetSystemMetrics(SM_CXSCREEN);
        const int H = GetSystemMetrics(SM_CYSCREEN);
        sprintf(buf, "Frame time: %.3f ms  GameTime: %.2f s  Metrics: %dx%d", elapsedMs, gameTimeSec, W, H);
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkMode(hdc, TRANSPARENT);
        DrawText(hdc, buf, -1, &rc, DT_LEFT | DT_TOP);
        // OutputDebugStringA() throttles every N messages and skews timing.
        //OutputDebugStringA(buf);

        //Sleep(100);

#if THROTTLE_FRAME_TIME
        // Throttling tests.
        //
        // Sleep() has very long and random delays. This is very noticeable for
        // value ~10ms. Sleep(10) reults in an average frame time of ~15ms.
        // sleep_for() isn't any better.
        //
        // The busy-wait with WaitMicros() is the most accurate but it has high
        // CPU usage.
        using namespace std::chrono_literals;
        // Rough sleep time, not taking into account frame time.
        const LONGLONG sleepTimeUs = desiredFrameTimeUs;
        const LONGLONG sleepTimeMs = sleepTimeUs / 1'000;
        //Sleep(sleepTimeMs);
        //std::this_thread::sleep_for(std::chrono::microseconds(sleepTimeUs));
        BusyWaitMicros(sleepTimeUs);
        //WaitMicros(sleepTimeUs);
#endif // THROTTLE_FRAME_TIME
    }

    ReleaseDC(hwnd, hdc);
    CloseWindow(hwnd);

    return 0;
}
