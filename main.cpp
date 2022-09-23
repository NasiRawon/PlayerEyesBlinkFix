#include "skse64/PluginAPI.h"
#include "skse64_common/skse_version.h"
#include "skse64_common/Relocation.h"
#include "skse64_common/SafeWrite.h"
#include "skse64_common/BranchTrampoline.h"
#include "skse64/GameReferences.h"
#include "skse64/GameInput.h"
#include "skse64/NiExtraData.h"
#include "xbyak/xbyak.h"
#include <shlobj.h>
#include <time.h>
#include <Psapi.h>
#include "PatternScanner.h"

#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "Psapi.lib")

IDebugLog				gLog;
PluginHandle			g_pluginHandle = kPluginHandle_Invalid;
void* g_moduleHandle = nullptr;

uintptr_t g_jumpAddr = 0;
uintptr_t g_debugAddr = 0;
uintptr_t g_playerAddr = 0;

MMRESULT g_timerID = 0;
UINT g_resolution = 30;
INT32 g_timer = 0;
double g_step = 3;
bool g_run = false;

void MainGetAddresses()
{
	//const std::array<BYTE, 7> jumpPattern = { 0x0F, 0xB6, 0x91, 0xDD, 0x0B, 0x00, 0x00 };
	//const std::array<BYTE, 7> debugPattern = { 0xF6, 0x86, 0xD9, 0x0B, 0x00, 0x00, 0x10 };
	const std::array<BYTE, 7> jumpPattern = { 0x0F, 0xB6, 0x91, 0xE5, 0x0B, 0x00, 0x00 };
	const std::array<BYTE, 7> debugPattern = { 0xF6, 0x86, 0xE1, 0x0B, 0x00, 0x00, 0x10 };
	const std::array<BYTE, 8> pattern = { 0x8B, 0x40, 0x10, 0xC1, 0xE8, 0x17, 0xA8, 0x01 };

	//g_jumpAddr = (uintptr_t)scan_memory(jumpPattern, 0x2A, false); //SE

	g_jumpAddr = (uintptr_t)scan_memory(jumpPattern, 0x29, false); // AE
	g_debugAddr = (uintptr_t)scan_memory(debugPattern, 0x1E, true, 0x1, 0x5);
	g_playerAddr = (uintptr_t)scan_memory(pattern, 0x23, true, 0x3, 0x7);
}

void UpdateExpression(PlayerCharacter* player, float value)
{
	BSFaceGenAnimationData* faceAnimData = nullptr;
	faceAnimData = player->GetFaceGenAnimationData();
	if (faceAnimData)
	{
		BSFaceGenKeyframeMultiple* keyframe = nullptr;
		keyframe = &faceAnimData->keyFrames[7];
		if (keyframe && keyframe->count == 17)
		{
			keyframe->values[0] = value;
			keyframe->values[1] = value;
		}
	}
}

void PlayerBlink(UINT uID)
{
	PlayerCharacter* player = *(PlayerCharacter**)g_playerAddr;
	if (player && player->loadedState)
	{
		INT32 timer = g_timer;
		timer--;
		g_timer = timer;
		if (timer < 0)
		{
			timer = ((rand() % 50) * 4) + 100;
			g_timer = timer;
		}
		if (timer >= 0 && timer < 5)
		{
			float time = g_timer;
			if (timer <= 3)
			{
				float value = time / g_step;

				UpdateExpression(player, value);
			}
			else
			{
				float step = g_step;
				float value = 1 - ((time - step) / step);

				UpdateExpression(player, value);
			}
		}
	}
}

void CALLBACK TimeProc(
	UINT uID, UINT uMsg, DWORD_PTR dwUser,
	DWORD_PTR dw1, DWORD_PTR dw2)
{
	PlayerBlink(uID);
}

bool Start()
{
	bool ret = false;
	if (g_timerID == 0)
	{
		UINT delay = 30;
		timeBeginPeriod(g_resolution);
		g_timerID = timeSetEvent(delay, g_resolution, TimeProc, 0, TIME_PERIODIC);
		if (g_timerID != 0)
			ret = true;
	}

	return ret;
}

bool Stop()
{
	bool ret = false;
	if (g_timerID != 0)
	{
		if (timeKillEvent(g_timerID) == TIMERR_NOERROR)
		{
			g_timerID = 0;
			ret = true;
		}
		timeEndPeriod(g_resolution);
	}

	PlayerCharacter* player = *(PlayerCharacter**)g_playerAddr;
	if (player && player->loadedState)
		UpdateExpression(player, 0);

	return ret;
}

void JumpProcessButton_Hook(ButtonEvent* evn)
{
	typedef void(*DebugNotification_t)(const char*, bool, bool);
	const DebugNotification_t DebugNotification = (DebugNotification_t)g_debugAddr;

	static bool bProcessLongTap = false;

	bool IsDown = (evn->flags > 0) && (evn->timer == 0.0f);

	if (!IsDown)
	{
		if (bProcessLongTap && evn->timer > 2.0f)
		{
			bProcessLongTap = false;

			MMRESULT id = g_timerID;
			g_timerID = id;

			bool state = false;
			if (id != 0)
			{
				state = Stop();
				DebugNotification((state ? "PlayerBlink has been disabled." : "Error"), false, true);
			}
			else
			{
				state = Start();
				DebugNotification((state ? "PlayerBlink has been enabled." : "Error"), false, true);
			}
		}
		return;
	}

	bProcessLongTap = true;
}

extern "C"
{

	__declspec(dllexport) SKSEPluginVersionData SKSEPlugin_Version =
	{
		SKSEPluginVersionData::kVersion,

		1,
		"Player Eyes Blink Fix",

		"NasGorTelorCeplok",
		"",

		0,
		2,	// version independent
		{ 0 },

		0,	// works with any version of the script extender. you probably do not need to put anything here
	};

	bool SKSEPlugin_Load(const SKSEInterface* skse)
	{
		gLog.OpenRelative(CSIDL_MYDOCUMENTS, "\\My Games\\Skyrim Special Edition\\SKSE\\PlayerBlink.log");

		g_pluginHandle = skse->GetPluginHandle();

		if (skse->isEditor)
		{
			_MESSAGE("loaded in editor, marking as incompatible");
			return false;
		}

		if (!g_branchTrampoline.Create(1024 * 64))
		{
			_ERROR("couldn't create branch trampoline. this is fatal. skipping remainder of init process.");
			return false;
		}

		if (!g_localTrampoline.Create(1024 * 64, g_moduleHandle))
		{
			_ERROR("couldn't create codegen buffer. this is fatal. skipping remainder of init process.");
			return false;
		}

		_MESSAGE("Load");

		MainGetAddresses();

		struct InstallHookJumpProcessButton_Code : Xbyak::CodeGenerator {
			InstallHookJumpProcessButton_Code(void* buf, UInt64 funcAddr) : Xbyak::CodeGenerator(4096, buf)
			{
				Xbyak::Label retn1Label;
				Xbyak::Label retn2Label;
				Xbyak::Label funcLabel;
				Xbyak::Label jumpLabel;

				sub(rsp, 0x20);
				push(rax);
				push(rbx);

				mov(rcx, rbx); // rbx = ButtonEvent
				call(ptr[rip + funcLabel]);

				pop(rbx);
				pop(rax);
				add(rsp, 0x20);

				// overwritten codes
				// movss xmm1,dword ptr ds:[rbx+28] 
				movss(xmm1, dword[rbx + 0x28]); // AE only
				xorps(xmm0, xmm0);
				test(al, al);
				je(jumpLabel);
				jmp(ptr[rip + retn1Label]);

				L(funcLabel);
				dq(funcAddr);

				L(jumpLabel);
				jmp(ptr[rip + retn2Label]);

				L(retn1Label);
				dq(g_jumpAddr + 0x6);

				L(retn2Label);
				dq(g_jumpAddr + 0x86); // AE
				//dq(g_jumpAddr + 0x8C); // SE
			}
		};

		void* codeBuf = g_localTrampoline.StartAlloc();
		InstallHookJumpProcessButton_Code code(codeBuf, GetFnAddr(JumpProcessButton_Hook));
		g_localTrampoline.EndAlloc(code.getCurr());

		g_branchTrampoline.Write6Branch(g_jumpAddr, uintptr_t(code.getCode()));

		srand(time(0));
		Start();

		return true;
	}

}