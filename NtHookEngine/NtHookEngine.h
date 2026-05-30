#pragma once

#include "stdafx.h"

// Original API.

BOOL InitNtEng();
BOOL HookFunction(ULONG_PTR OriginalFunction, ULONG_PTR NewFunction);
VOID UnhookFunction(ULONG_PTR Function);
ULONG_PTR GetOriginalFunction(ULONG_PTR Hook);

// Extended API. Must still call InitNtEng().

using NtEngHook = UINT;

NtEngHook HookFunctionEx(LPCSTR dllName, LPCSTR functionName, void* hookFunction);
BOOL      UnhookFunctionEx(NtEngHook handle);

template <class FuncProto, class... Args>
auto CallOriginalFunctionEx(FuncProto hook, Args&&... args)
{
    auto original = (FuncProto)(GetOriginalFunction((ULONG_PTR)(hook)));
    return original(args...);
}
