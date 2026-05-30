#include "stdafx.h"
#include "NtHookEngine.h"
#include "distorm.h"

#include <stdlib.h>

// 10000 hooks should be enough
#define MAX_HOOKS 10000

typedef struct _HOOK_INFO
{
	ULONG_PTR Function;	// Address of the original function

	ULONG_PTR Hook;		// Address of the function to call
						// instead of the original

	ULONG_PTR Bridge;   // Address of the instruction bridge
						// necessary because of the hook jmp
						// which overwrites instructions

} HOOK_INFO, *PHOOK_INFO;

static HOOK_INFO HookInfo[MAX_HOOKS];

static UINT NumberOfHooks = 0;

static BYTE *pBridgeBuffer = NULL; // Here are going to be stored all the bridges

static UINT CurrentBridgeBufferSize = 0; // This number is incremented as
								         // the bridge buffer is growing

#ifdef _M_IX86

#define JUMP_WORST		10		// Worst case scenario

#else ifdef _M_AMD64

#define JUMP_WORST		14		// Worst case scenario

#endif


static HOOK_INFO *GetHookInfoFromFunction(ULONG_PTR OriginalFunction)
{
	if (NumberOfHooks == 0)
		return NULL;

	for (UINT x = 0; x < NumberOfHooks; x++)
	{
		if (HookInfo[x].Function == OriginalFunction)
			return &HookInfo[x];
	}

	return NULL;
}

//
// This function  retrieves the necessary size for the jump
//

static UINT GetJumpSize(ULONG_PTR PosA, ULONG_PTR PosB)
{
	ULONG_PTR res = max(PosA, PosB) - min(PosA, PosB);

	// if you want to handle relative jumps
	/*if (res <= (ULONG_PTR) 0x7FFF0000)
	{
		return 5; // jmp rel
	}
	else
	{*/
		// jmp [xxx] + addr

#ifdef _M_IX86

		return 10;

#else ifdef _M_AMD64

		return 14;

#endif
	//}

	return 0; // error
}

//
// This function writes unconditional jumps
// both for x86 and x64
//

static VOID WriteJump(VOID *pAddress, ULONG_PTR JumpTo)
{
	DWORD dwOldProtect = 0;

	VirtualProtect(pAddress, JUMP_WORST, PAGE_EXECUTE_READWRITE, &dwOldProtect);

	BYTE *pCur = (BYTE *) pAddress;

	// if you want to handle relative jumps
	/*if (JumpTo - (ULONG_PTR) pAddress <= (ULONG_PTR) 0x7FFF0000)
	{
		*pCur = 0xE9;  // jmp rel

		DWORD RelAddr = (DWORD) (JumpTo - (ULONG_PTR) pAddress) - 5;

		memcpy(++pCur, &RelAddr, sizeof (DWORD));
	}
	else
	{*/

#ifdef _M_IX86

		*pCur = 0xff;     // jmp [addr]
		*(++pCur) = 0x25;
		pCur++;
		*((DWORD *) pCur) = (DWORD)(((ULONG_PTR) pCur) + sizeof (DWORD));
		pCur += sizeof (DWORD);
		*((ULONG_PTR *)pCur) = JumpTo;

#else ifdef _M_AMD64

		*pCur = 0xff;		// jmp [rip+addr]
		*(++pCur) = 0x25;
		*((DWORD *) ++pCur) = 0; // addr = 0
		pCur += sizeof (DWORD);
		*((ULONG_PTR *)pCur) = JumpTo;

#endif
	//}

	DWORD dwBuf = 0;	// nessary othewrise the function fails

	VirtualProtect(pAddress, JUMP_WORST, dwOldProtect, &dwBuf);
}


//
// This function creates a bridge of the original function
//

static VOID *CreateBridge(ULONG_PTR Function, const UINT JumpSize)
{
	if (pBridgeBuffer == NULL) return NULL;

#define MAX_INSTRUCTIONS 100

	_DecodeResult res;
	_DecodedInst decodedInstructions[MAX_INSTRUCTIONS];
	unsigned int decodedInstructionsCount = 0;

#ifdef _M_IX86

	_DecodeType dt = Decode32Bits;

#else ifdef _M_AMD64

	_DecodeType dt = Decode64Bits;

#endif

	_OffsetType offset = 0;

	res = distorm_decode(offset,	// offset for buffer
		(const BYTE *) Function,	// buffer to disassemble
		50,							// function size (code size to disasm)
									// 50 instr should be _quite_ enough
		dt,							// x86 or x64?
		decodedInstructions,		// decoded instr
		MAX_INSTRUCTIONS,			// array size
		&decodedInstructionsCount	// how many instr were disassembled?
		);

	if (res == DECRES_INPUTERR)
		return NULL;

	DWORD InstrSize = 0;

	VOID *pBridge = (VOID *) &pBridgeBuffer[CurrentBridgeBufferSize];

	for (UINT x = 0; x < decodedInstructionsCount; x++)
	{
		if (InstrSize >= JumpSize)
			break;

		BYTE *pCurInstr = (BYTE *) (InstrSize + (ULONG_PTR) Function);

		//
		// This is an sample attempt of handling a jump
		// It works, but it converts the jz to jmp
		// since I didn't write the code for writing
		// conditional jumps
		//
		/*
		if (*pCurInstr == 0x74) // jz near
		{
			ULONG_PTR Dest = (InstrSize + (ULONG_PTR) Function)
				+ (char) pCurInstr[1];

			WriteJump(&pBridgeBuffer[CurrentBridgeBufferSize], Dest);

			CurrentBridgeBufferSize += JumpSize;
		}
		else
		{*/
			memcpy(&pBridgeBuffer[CurrentBridgeBufferSize],
				(VOID *) pCurInstr, decodedInstructions[x].size);

			CurrentBridgeBufferSize += decodedInstructions[x].size;
		//}

		InstrSize += decodedInstructions[x].size;
	}

	WriteJump(&pBridgeBuffer[CurrentBridgeBufferSize], Function + InstrSize);
	CurrentBridgeBufferSize += GetJumpSize((ULONG_PTR) &pBridgeBuffer[CurrentBridgeBufferSize],
		Function + InstrSize);

	return pBridge;
}

//
// Initializes the engine
//

BOOL InitNtEng()
{
	pBridgeBuffer = (BYTE *) VirtualAlloc(
		NULL, MAX_HOOKS * (JUMP_WORST * 3), MEM_COMMIT, PAGE_EXECUTE_READWRITE);

	return (pBridgeBuffer == NULL) ? FALSE : TRUE;
}

//
// Hooks a function
//

BOOL HookFunction(ULONG_PTR OriginalFunction, ULONG_PTR NewFunction)
{
	//
	// Check if the function has already been hooked
	// If so, no disassembling is necessary since we already
	// have our bridge
	//

	HOOK_INFO *hinfo = GetHookInfoFromFunction(OriginalFunction);

	if (hinfo)
	{
		WriteJump((VOID *) OriginalFunction, NewFunction);
	}
	else
	{
		if (NumberOfHooks == MAX_HOOKS)
			return FALSE;

		VOID *pBridge = CreateBridge(OriginalFunction, GetJumpSize(OriginalFunction, NewFunction));

		if (pBridge == NULL)
			return FALSE;

		HookInfo[NumberOfHooks].Function = OriginalFunction;
		HookInfo[NumberOfHooks].Bridge = (ULONG_PTR) pBridge;
		HookInfo[NumberOfHooks].Hook = NewFunction;

		NumberOfHooks++;

		WriteJump((VOID *) OriginalFunction, NewFunction);
	}

	return TRUE;
}


//
// Unhooks a function
//

VOID UnhookFunction(ULONG_PTR Function)
{
	//
	// Check if the function has already been hooked
	// If not, I can't unhook it
	//

	HOOK_INFO *hinfo = GetHookInfoFromFunction(Function);

	if (hinfo)
	{
		//
		// Replaces the hook jump with a jump to the bridge
		// I'm not completely unhooking since I'm not
		// restoring the original bytes
		//

		WriteJump((VOID *) hinfo->Function, hinfo->Bridge);
	}
}

//
// Get the bridge to call instead of the original function from hook
//

ULONG_PTR GetOriginalFunction(ULONG_PTR Hook)
{
	if (NumberOfHooks == 0)
		return NULL;

	for (UINT x = 0; x < NumberOfHooks; x++)
	{
		if (HookInfo[x].Hook == Hook)
			return HookInfo[x].Bridge;
	}

	return NULL;
}

// -----------------------------------------------------------------------------
// Extended API

struct HookEx
{
	NtEngHook handle;
	HMODULE   module;
	FARPROC   orgFunc;
};

static HookEx HooksEx[MAX_HOOKS];

// It does not necessarily match NumberOfHooks if the client mixes both the
// original and the extended APIs.
// Start at 1 since NULL is used for failure.
static UINT NumberOfHooksEx = 1;

NtEngHook HookFunctionEx(LPCSTR dllName, LPCSTR functionName, void* hookFunction)
{
	if (NumberOfHooksEx == MAX_HOOKS)
		return NULL;

	HookEx& hook = HooksEx[NumberOfHooksEx];

	hook.module = LoadLibraryA(dllName);
    if (hook.module == NULL)
    {
        return NULL;
    }
    hook.orgFunc = GetProcAddress(hook.module, functionName);
    if (hook.orgFunc == NULL)
    {
        return NULL;
    }
    if (HookFunction((ULONG_PTR)hook.orgFunc, (ULONG_PTR)hookFunction) == FALSE)
    {
        return NULL;
    }

	const NtEngHook handle = (NtEngHook)NumberOfHooksEx;
	NumberOfHooksEx++;
    return handle;
}

BOOL UnhookFunctionEx(NtEngHook handle)
{
	const UINT index = (UINT)handle;
	if (index >= NumberOfHooksEx)
		return FALSE;

	HookEx& hook = HooksEx[index];

	BOOL result = TRUE;
	if (hook.orgFunc != nullptr)
	{
		UnhookFunction((ULONG_PTR)hook.orgFunc);
		hook.orgFunc = nullptr;
	}
    if (hook.module != NULL)
    {
        result = FreeLibrary(hook.module);
        hook.module  = NULL;
    }
    return result;
}
