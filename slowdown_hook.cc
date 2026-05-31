#include <Windows.h>

#include "slowdown_common.h"

#include <ddraw.h>
#include <d3dkmthk.h>
#include <NtHookEngine.h>
#include <TlHelp32.h>
#include <windowsx.h> // GET_X_LPARAM, GET_Y_LPARAM

#include <cassert>
#include <cstdint>
#include <format>
#include <string>

// Logging.
static thread_local char g_logBuf[1024];
#define LOG_INFO(FORMAT, ...) \
{\
    snprintf(g_logBuf, sizeof(g_logBuf), "[Slowdown] " FORMAT, __VA_ARGS__);\
    OutputDebugStringA(g_logBuf);\
}
#define LOG_DEBUG(FORMAT, ...) \
{\
    if (g_env.log)\
    {\
        LOG_INFO(FORMAT, __VA_ARGS__)\
    }\
}

// Parameters passed by the injector through the environment.
struct Environment
{
    int  frameTimeMs = 0;     // Desired frame time.
    int  sleepTimeMs = 0;     // Constant amount of time to sleep between frames.
    bool busyWait    = false; // Busy-wait instead of sleep.
    bool log         = false; // Whether to log hooks and window messages.

    bool Init();
};
static Environment g_env = {};

static bool ReadEnvInt(const char* envVar, int* pOut)
{
    char buf[64] = {};
    if (GetEnvironmentVariable(envVar, buf, sizeof(buf)) == 0)
    {
        return false;
    }
    *pOut = std::stoi(buf);
    return true;
}

static bool ReadEnvBool(const char* envVar, bool* pOut)
{
    char buf[2] = {}; // Need at least 2 chars for API call to succeed.
    if (GetEnvironmentVariable(envVar, buf, sizeof(buf)) == 0)
    {
        return false;
    }
    *pOut = (buf[0] == '1') ? true : false;
    return true;
}

bool Environment::Init()
{
    if (!ReadEnvInt(SlowdownEnvFrameTimeMs, &frameTimeMs) &&
        !ReadEnvInt(SlowdownEnvSleepTimeMs, &sleepTimeMs)) return false;
    ReadEnvBool(SlowdownEnvBusyWait, &busyWait); // optional
    ReadEnvBool(SlowdownEnvLog, &log); // optional
    return true;
}

// Busy-wait for the given number of microseconds.
static void BusyWaitMicros(LONGLONG micros)
{
    static thread_local bool first = true;
    static thread_local LONGLONG freqUs;
    if (first)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        assert(freq.QuadPart > 0);
        freqUs = freq.QuadPart / 1'000'000;
    }

    LONGLONG elapsed;
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    do
    {
        QueryPerformanceCounter(&now);
        elapsed = (now.QuadPart - start.QuadPart) / freqUs;
    }
    while (elapsed < micros);
}

// Sleep or busy-wait for the given number of milliseconds.
//
// See the comments in the game demo. Sleep() has long, random delays.
// A busy-wait is more accurate at the expense of high CPU usage.
static void Throttle(LONGLONG micros, bool busyWait)
{
    if (busyWait) BusyWaitMicros(micros);
    else Sleep(micros / 1000);
}

// Throttle by a variable amount of time depending on how much time has elapsed
// since subsequent calls to this function.
static void ThrottleVariable(LONGLONG desiredFrameTimeUs)
{
    if (desiredFrameTimeUs <= 0)
    {
        return;
    }
    static bool first = true;
    static LARGE_INTEGER freq = {}, prev = {}, now = {};
    if (first)
    {
        // Initialization.
        first = false;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&prev); // First frame has 0 elapsed time.
    }
    // Game loop current frame essentially ends here. Take the time now.
    QueryPerformanceCounter(&now);
    assert(freq.QuadPart != 0);
    assert(prev.QuadPart != 0);
    assert(now.QuadPart  != 0);
    const LONGLONG elapsedUs
        = (now.QuadPart > prev.QuadPart) // Clock can "go backwards" if the thread is scheduled on a different CPU depending on the hardware.
        ? ((now.QuadPart - prev.QuadPart) * 1'000'000ULL) / freq.QuadPart
        : 0;
    if (elapsedUs < desiredFrameTimeUs)
    {
        const LONGLONG sleepTimeUs = desiredFrameTimeUs - elapsedUs;
        assert(sleepTimeUs > 0);
        LOG_DEBUG("Throttle(%llu ms = %llu(desired) - %llu(elapsed))", sleepTimeUs/1000, desiredFrameTimeUs/1000, elapsedUs/1000);
        Throttle(sleepTimeUs, g_env.busyWait);
    }
    else
    {
        LOG_DEBUG("No sleep: %llu(elapsed) >= %llu(desired)", elapsedUs/1000, desiredFrameTimeUs/1000);
    }
    // Game loop starts next frame next. Measure the time it takes for the next throttle.
    QueryPerformanceCounter(&prev);
}

static std::string WindowMessageToString(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_MOUSEMOVE:
    {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        const bool left   = (wParam & MK_LBUTTON)  != 0;
        const bool right  = (wParam & MK_RBUTTON)  != 0;
        const bool middle = (wParam & MK_MBUTTON)  != 0;
        const bool shift  = (wParam & MK_SHIFT)    != 0;
        const bool ctrl   = (wParam & MK_CONTROL)  != 0;
        const bool x1     = (wParam & MK_XBUTTON1) != 0;
        const bool x2     = (wParam & MK_XBUTTON2) != 0;
        return std::format("WM_MOUSEMOVE({}, {}) [L:{}, R:{}, M:{}, SHFT:{}, CTRL:{}, x1:{}, x2:{}]",
            x, y, left, right, middle, shift, ctrl, x1, x2);
    }
    case WM_PAINT: return "WM_PAINT";
    case WM_TIMER: return "WM_TIMER";
    default:       return std::to_string(message);
    }
}

// Window message hook.
static LRESULT CALLBACK GetMessageHook(int nCode, WPARAM wParam, LPARAM lParam)
{
#define LOG_MSG(FORMAT, ...) LOG_DEBUG("[GetMessageHook] " FORMAT, __VA_ARGS__)
    if (nCode >= 0)
    {
        const MSG* msg = (const MSG*)lParam;
        if (msg)
        {
            const std::string messageStr = WindowMessageToString(msg->message, wParam, lParam);
            LOG_MSG("%s", messageStr.c_str());
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

struct ThreadMessageHook
{
    DWORD threadId = 0;
    HHOOK hook     = NULL;
};

// Monitors threads and hooks them with SetWindowsHookExA().
// Used to log window messages when debugging.
class ThreadMonitor
{
public:
    static ThreadMonitor& Instance() { static ThreadMonitor m; return m; }

    bool Init();
    void Destroy();

private:
    static DWORD __stdcall MonitorThreads(LPVOID pParam);

    static constexpr DWORD hookingThreadWaitTimeout = 1000;

    ThreadMessageHook messageHooks[32] = {}; // 1 hook per thread; 32 threads should be enough.
    int nextHook = 0; // Index into array above.

    CRITICAL_SECTION messageHooksCs; // Protects the hooks array.
    HANDLE stopEvent     = NULL;     // Event used to stop the hooking thread.
    HANDLE hookingThread = NULL;     // Thread to enumerate other threads and install window hooks.
};

bool ThreadMonitor::Init()
{
    InitializeCriticalSection(&messageHooksCs);
    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (stopEvent == NULL) return false;
    hookingThread = CreateThread(NULL, 0, ThreadMonitor::MonitorThreads, NULL, 0, NULL);
    if (hookingThread == NULL) return false;
    return true;
}

void ThreadMonitor::Destroy()
{
    SetEvent(stopEvent);
    WaitForSingleObject(hookingThread, hookingThreadWaitTimeout * 4);
    CloseHandle(hookingThread);
    CloseHandle(stopEvent);
    EnterCriticalSection(&messageHooksCs);
    for (ThreadMessageHook& tmh : messageHooks)
    {
        if (tmh.hook != NULL)
        {
            UnhookWindowsHookEx(tmh.hook);
            tmh.hook = NULL;
            tmh.threadId = 0;
        }
    }
    LeaveCriticalSection(&messageHooksCs);
    DeleteCriticalSection(&messageHooksCs);
}

// Hooking thread.
DWORD __stdcall ThreadMonitor::MonitorThreads(LPVOID pParam)
{
    ThreadMonitor& m = ThreadMonitor::Instance();

    const DWORD pid = GetCurrentProcessId();
    while (WaitForSingleObject(m.stopEvent, m.hookingThreadWaitTimeout) == WAIT_TIMEOUT)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, pid);
        if (snapshot != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te = { sizeof(te) };
            if (Thread32First(snapshot, &te) == TRUE)
            {
                EnterCriticalSection(&m.messageHooksCs);
                do
                {
                    if (te.th32OwnerProcessID == pid)
                    {
                        bool alreadyHooked = false;
                        for (int i = 0; i < m.nextHook; ++i)
                        {
                            ThreadMessageHook& tmh = m.messageHooks[i];
                            if (tmh.threadId == te.th32ThreadID)
                            {
                                alreadyHooked = true;
                                break;
                            }
                        }
                        if (!alreadyHooked)
                        {
                            HHOOK hook = SetWindowsHookExA(WH_GETMESSAGE, GetMessageHook, NULL, te.th32ThreadID);
                            if (hook)
                            {
                                LOG_DEBUG("Hooked thread %d", te.th32ThreadID);
                                ThreadMessageHook& tmh = m.messageHooks[m.nextHook++];
                                tmh.threadId = te.th32ThreadID;
                                tmh.hook = hook;
                            }
                            else
                            {
                                //LOG_DEBUG("Failed to hook thread %d", te.th32ThreadID);
                                //assert(false);
                            }
                        }
                    }
                }
                while (Thread32Next(snapshot, &te) == TRUE);
                LeaveCriticalSection(&m.messageHooksCs);
            }
            CloseHandle(snapshot);
        }
    }
    return 0;
}

static bool PatchVtable(void** vtable, size_t index, void* pNewFunc, void** ppOutOldFunc)
{
    void** ppOldFunc = &vtable[index];
    DWORD oldProtect;
    if (VirtualProtect(ppOldFunc, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect) == FALSE)
    {
        return false;
    }
    *ppOutOldFunc = *ppOldFunc;
    *ppOldFunc = pNewFunc;
    if (VirtualProtect(ppOldFunc, sizeof(void*), oldProtect, &oldProtect) == FALSE)
    {
        return false;
    }
    return true;
}

static BOOL __stdcall GetCursorPos_hook(LPPOINT lpPoint)
{
    BOOL result = CallOriginalFunctionEx(GetCursorPos_hook, lpPoint);
    if ((result == TRUE) && (lpPoint != nullptr))
    {
        LOG_DEBUG("GetCursorPos() -> (%d, %d)", lpPoint->x, lpPoint->y)
    }
    return result;
}

static int __stdcall MessageBoxA_hook(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType)
{
    return CallOriginalFunctionEx(MessageBoxA_hook, hWnd, lpText, "Hooked!", uType);
}

static BOOL __stdcall PeekMessageA_hook(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
{
    const BOOL result = CallOriginalFunctionEx(PeekMessageA_hook, lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
    LOG_DEBUG("PeekMessageA() -> %d", result);
    if (result == 0) // No messages pending -> start frame.
    {
        if (g_env.frameTimeMs > 0)
        {
            ThrottleVariable(g_env.frameTimeMs * 1000);
        }
        if (g_env.sleepTimeMs > 0)
        {
            Throttle(g_env.sleepTimeMs * 1000, g_env.busyWait);
        }
    }
    return result;
}

static BOOL __stdcall WaitMessage_hook()
{
    LOG_DEBUG("WaitMessage");
    return CallOriginalFunctionEx(WaitMessage_hook);
}

static HWND __stdcall CreateWindowExA_hook(
    DWORD     dwExStyle,
    LPCSTR    lpClassName,
    LPCSTR    lpWindowName,
    DWORD     dwStyle,
    int       X,
    int       Y,
    int       nWidth,
    int       nHeight,
    HWND      hWndParent,
    HMENU     hMenu,
    HINSTANCE hInstance,
    LPVOID    lpParam)
{
    return CallOriginalFunctionEx(CreateWindowExA_hook,
        dwExStyle,
        lpClassName,
        lpWindowName,
        /*dwStyle*/WS_POPUP | WS_VISIBLE,
        /*X*/CW_USEDEFAULT,
        /*Y*/CW_USEDEFAULT,
        /*nWidth*/640,
        /*nHeight*/480,
        hWndParent,
        hMenu,
        hInstance,
        lpParam);
}

using SetCooperativeLevel_t = HRESULT(__stdcall*)(IDirectDraw* pDirectDraw, HWND hWnd, DWORD dwFlags);
static SetCooperativeLevel_t g_pSetCooperativeLevelOriginal = nullptr;
static HRESULT __stdcall SetCooperativeLevel_hook(IDirectDraw* pDirectDraw, HWND hWnd, DWORD dwFlags)
{
    LOG_DEBUG("Disable fullscreen exclusive");
    //dwFlags &= ~(DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN); // Disable fullscreen exclusive.
    dwFlags = DDSCL_NORMAL;
    assert(g_pSetCooperativeLevelOriginal);
    return g_pSetCooperativeLevelOriginal(pDirectDraw, hWnd, dwFlags);
}

static HRESULT __stdcall DirectDrawCreate_hook(
  GUID         *lpGUID,
  LPDIRECTDRAW *lplpDD,
  IUnknown     *pUnkOuter)
{
    LOG_DEBUG("DirectDrawCreate");
    const HRESULT result = CallOriginalFunctionEx(DirectDrawCreate_hook, lpGUID, lplpDD, pUnkOuter);
    if (result == S_OK)
    {
        IDirectDraw* pDirectDraw = *lplpDD;
        constexpr int SetCooperativeLevelIndex = 20; // 0x50 / sizeof(void*) = 20
        void** vtable = *(void***)pDirectDraw;
        void* pOldFunc = nullptr;
        if (PatchVtable(vtable, SetCooperativeLevelIndex, SetCooperativeLevel_hook, &pOldFunc))
        {
            g_pSetCooperativeLevelOriginal = (SetCooperativeLevel_t)pOldFunc;
        }
        else
        {
            assert(false);
            // In release, allow the application to continue.
        }
    }
    return result;
}

struct Hook
{
    Hook(const char* dllName, const char* funcName, void* hookFunc)
        : m_dllName{dllName}, m_funcName{funcName}, m_hookFunc{hookFunc} {}

    const char* m_dllName;
    const char* m_funcName;
    void*       m_hookFunc;
    NtEngHook   handle = NULL;
};

static Hook hooks[]
{
    { "USER32.dll", "PeekMessageA", PeekMessageA_hook },
    // The following are kept for debugging purposes.
    //
    //{ "USER32.dll", "MessageBoxA",  MessageBoxA_hook  },
    { "USER32.dll", "WaitMessage",  WaitMessage_hook  },
    //{ "USER32.dll", "GetCursorPos", GetCursorPos_hook },
    //
    // An attempt to force the game to run in windowed mode. Have not succeeded
    // but it would be handy if we can get this to work.
    //{ "USER32.dll", "CreateWindowExA",  CreateWindowExA_hook  },
    //{ "Ddraw.dll",  "DirectDrawCreate", DirectDrawCreate_hook },
};

static bool InstallHooks()
{
    bool result = true;
    for (Hook& hook : hooks)
    {
        hook.handle = HookFunctionEx(hook.m_dllName, hook.m_funcName, hook.m_hookFunc);
        if (hook.handle == NULL)
        {
            result = false;
            break;
        }
    }
    return result;
}

static bool UninstallHooks()
{
    bool result = true;
    for (Hook& hook : hooks)
    {
        if (UnhookFunctionEx(hook.handle) == FALSE)
        {
            result = false;
            // Continue to uninstall as many hooks as we can.
        }
    }
    return result;
}

static bool Init()
{
    if (!g_env.Init()) return false;
    if (InitNtEng() == FALSE) return false;
    if (!InstallHooks()) return false;

    if (g_env.log)
    {
        if (!ThreadMonitor::Instance().Init()) return false;
    }

    if (g_env.frameTimeMs > 0)
    {
        LOG_INFO("Target frame time: %d", g_env.frameTimeMs);
    }
    if (g_env.sleepTimeMs > 0)
    {
        LOG_INFO("Sleep time:        %d", g_env.sleepTimeMs);
    }
    LOG_INFO("Logging:           %d", g_env.log);
    return true;
}

static bool Shutdown()
{
    if (g_env.log)
    {
        ThreadMonitor::Instance().Destroy();
    }
    return UninstallHooks();
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD dwReason, LPVOID reserved)
{
    bool result = false;
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        LOG_INFO("Injected");
        result = Init();
        assert(result);
        break;
    case DLL_PROCESS_DETACH:
        result = Shutdown();
        break;
    default:
        break;
    }
    return result ? TRUE : FALSE;
}
