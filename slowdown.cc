//#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <Psapi.h>

#include "slowdown_common.h"

#include <cassert>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

static constexpr const char* SlowdownDll        = "slowdown_hook.dll";
static constexpr int         DefaultFrameTimeMs = 16; // ~60 fps

static std::string GetLastErrorString(DWORD* pOutError)
{
    const DWORD error = GetLastError();
    *pOutError = error;
    if (error == 0)
    {
        return {};
    }
    LPSTR messageBuffer = nullptr;
    const size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
    const std::string message(messageBuffer, size);
    LocalFree(messageBuffer);
    return message;
}

#define LOG_ERROR(FORMAT, ...) fprintf(stderr, FORMAT "\n", __VA_ARGS__)
#define LOG_LAST_ERROR(FORMAT, ...) \
{ \
    DWORD error; \
    const std::string errorStr = GetLastErrorString(&error); \
    fprintf(stderr, FORMAT " (0x%x) : %s", __VA_ARGS__, error, errorStr.c_str()); \
}

static bool InjectDll(HANDLE hProcess, const char* dllPath)
{
    HMODULE kernel32 = GetModuleHandle("KERNEL32.dll");
    if (kernel32 == NULL)
    {
        LOG_LAST_ERROR("Failed to resolve KERNEL32.dll");
        return false;
    }
    FARPROC loadLibrary = GetProcAddress(kernel32, "LoadLibraryA");
    if (loadLibrary == NULL)
    {
        LOG_LAST_ERROR("Failed to resolve LoadLibraryA");
        return false;
    }
    const size_t dllPathLen = strlen(dllPath);
    const size_t remoteDllPathSize = dllPathLen + 1;
    char* remoteDllPath = (char*)VirtualAllocEx(hProcess, NULL, remoteDllPathSize, MEM_COMMIT, PAGE_READWRITE);
    if (remoteDllPath == NULL)
    {
        LOG_LAST_ERROR("Failed to allocate remote DLL path buffer");
        return false;
    }
    const char zero = '\0';
    WriteProcessMemory(hProcess, remoteDllPath, dllPath, dllPathLen, NULL);
    WriteProcessMemory(hProcess, remoteDllPath+dllPathLen, &zero, 1, NULL);
    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibrary, remoteDllPath, 0, NULL);
    if (hThread == NULL)
    {
        LOG_LAST_ERROR("Failed to create remote thread");
        VirtualFreeEx(hProcess, remoteDllPath, remoteDllPathSize, MEM_RELEASE);
        return false;
    }
    WaitForSingleObject(hThread, 4000); // Wait for a bit to let the injected DLL start up.
    return true;
}

int main(int argc, const char** argv)
{
    printf("Slow Down (%s-bit)\n", sizeof(void*) == 4 ? "32" : "64");
    if ((argc < 2) || (std::string(argv[1]) == "--help"))
    {
        fprintf(stderr, "Usage: %s <exe> [options]\n", argv[0]);
        static constexpr const char* options[]
        {
            "Options:",
            "  --ms   <number>   Constant amount of time to sleep every frame",
            "  --mspf <number>   Desired frame time in milliseconds",
            "  --fps  <number>   Desired frame rate",
            "  --busywait        Use a busy-wait instead of putting the CPU to sleep.",
            "                    More accurate timing but uses more CPU.",
            "  --log",
            "",
            "Options --fps, --mspf, and --ms are mutually exclusive",
            "By default, a target of ~60 fps is set.",
        };
        for (const char* line : options) fprintf(stderr, "%s\n", line);
        return 1;
    }
    int  timeMs = DefaultFrameTimeMs; // Target frame time or constant sleep time.
    bool constantSleep = false;
    bool busyWait = false;
    bool log = false;
    const fs::path gameExePath = fs::absolute(argv[1]);
    for (int i = 2; i < argc; ++i)
    {
        if (strcmp(argv[i], "--fps") == 0)
        {
            i++;
            if (i < argc)
            {
                const int fps = atoi(argv[i]);
                if (fps <= 0)
                {
                    fprintf(stderr, "fps must be >0\n");
                    return 1;
                }
                assert(fps > 0);
                timeMs = int(1000.0 / double(fps));
            }
        }
        else if (strcmp(argv[i], "--mspf") == 0)
        {
            i++;
            if (i < argc)
            {
                timeMs = atoi(argv[i]);
            }
        }
        else if (strcmp(argv[i], "--ms") == 0)
        {
            i++;
            if (i < argc)
            {
                timeMs = atoi(argv[i]);
            }
            constantSleep = true;
        }
        else if (strcmp(argv[i], "--busywait") == 0)
        {
            busyWait = true;
        }
        else if (strcmp(argv[i], "--log") == 0)
        {
            log = true;
        }
        else
        {
            fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
        }
    }

    const fs::path workDir = gameExePath.parent_path();

    char slowdownPath[MAX_PATH] = {};
    GetModuleFileNameA(NULL, slowdownPath, sizeof(slowdownPath));
    const fs::path slowdownDir = fs::path(slowdownPath).parent_path();

    if (constantSleep)
    {
        printf("Sleep %d ms/frame (constant sleep)\n", timeMs);
    }
    else
    {
        printf("%d ms/frame (~%d fps)\n", timeMs, 1000 / timeMs);
    }
    constexpr const char* boolStr[2] {"disabled", "enabled"};
    printf("Busy wait: %s\n", boolStr[busyWait]);
    printf("Logging:   %s\n", boolStr[log]);

    // Pass arguments to injected DLL through environment.
    SetEnvironmentVariable(
        constantSleep ? SlowdownEnvSleepTimeMs : SlowdownEnvFrameTimeMs,
        std::to_string(timeMs).c_str());
    if (log) SetEnvironmentVariable(SlowdownEnvLog, "1");
    if (busyWait) SetEnvironmentVariable(SlowdownEnvBusyWait, "1");
    // Start process suspended.
    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(gameExePath.string().c_str(), NULL, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, workDir.string().c_str(), &si, &pi) == FALSE)
    {
        LOG_LAST_ERROR("Failed to start program: %s", gameExePath.string().c_str());
        return 1;
    }
    // Inject.
    const fs::path slowdownDll = fs::absolute(slowdownDir / SlowdownDll);
    if (!InjectDll(pi.hProcess, slowdownDll.string().c_str()))
    {
        fprintf(stderr, "Failed to inject DLL\n");
        return 1;
    }
    // Resume process.
    if (ResumeThread(pi.hThread) == -1)
    {
        LOG_LAST_ERROR("ResumeThread() failed on started process (suspended)\n");
        if (TerminateProcess(pi.hProcess, 0) == FALSE)
        {
            LOG_LAST_ERROR("Failed to terminate started process, manual cleanup required\n");
        }
        return 1;
    }
    // Wait for process.
    WaitForSingleObject(pi.hThread, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
