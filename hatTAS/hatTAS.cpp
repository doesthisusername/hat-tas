#include "stdafx.h"
#include <Windows.h>
#include <stdio.h>
#include <tlhelp32.h>
#include "parse.h"

#define HAT_WINDOW L"LaunchUnrealUWindowsClient"
#define HAT_TITLE L"A Hat in Time (64-bit, Final Release Shipping PC, DX9, Modding)"
#define HAT_EXE_NAME L"HatinTimeGame.exe"
#define XINPUT_DLL_NAME L"XINPUT1_3.dll"
#define MSVCR_DLL_NAME L"MSVCR100.dll"
// divide by 4 because of pointer arithmetic
#define GET_STATE_CODE_OFFSET (0x9AC3EA) // except this one because it's for a breakpoint
#define POST_GET_STATE_CODE_OFFSET (0x9AC3EF) // this one too
#define TICK_CODE_OFFSET (0x3D7A70 / 4) // maybe? need to test
#define DELTA_WRITE_CODE_OFFSET (0x9D7F86 / 4)
#define DELTA_DATA_OFFSET (0x104D6E8 / 4)
#define TOTAL_FRAMES_OFFSET (0x11D07A0 / 4)
#define TIMER_OFFSET (0x10719D0 / 4)
#define FPS_PTR_OFFSET (0x11C27E0 / 4)
// msvcr offsets
#define RAND_CODE_OFFSET (0x7947D)

// it only wants to write on 4-byte aligned boundaries, so the first two bytes are filler, as our target code isn't 4-byte aligned
const BYTE nop_delta_buf[10] = { 0x7D, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
double desired_framerate = 1.0 / 60.0; // desired delta time
float original_fps = 60.f; // fps setting before the TAS started
double original_delta = 1.0 / 60.0; // for restoring framerate properly

// breakpoint code
const BYTE interrupt_op = 0xCC;
const BYTE jmp_op = 0xE9;
const BYTE orig_tick = 0x40;
const BYTE orig_xinput = 0xE8;
const BYTE orig_post_xinput = 0x33;

// rand
const BYTE newrand_buf[0x37] = { 0x53, 0x48, 0x31, 0xDB, 0x8B, 0x1D, 0xF2, 0x01, 0x00, 0x00, 0x48, 0x31, 0xC9, 0xFF, 0xC3, 0x3B, 0x1D, 0xE3, 0x01, 0x00, 0x00, 0x48, 0x0F, 0x47, 0xD9, 0x89, 0x1D, 0xDD, 0x01, 0x00, 0x00, 0x48, 0x8D, 0x0D, 0xDA, 0x01, 0x00, 0x00, 0x8B, 0x0C, 0x99, 0x89, 0x48, 0x1C, 0xC1, 0xE9, 0x10, 0x89, 0xC8, 0x5B, 0x48, 0x83, 0xC4, 0x28, 0xC3 };
BYTE orig_rand[5];
const int zero = 0;

// movie
tas_metadata meta;
input_report* reports;

HANDLE process;
HANDLE thread;

// only call after process handle has been made
BYTE* resolve_ptr(void* address) {
	BYTE* resolved;
	ReadProcessMemory(process, address, &resolved, 8, NULL);
	return resolved;
}

int main(int argc, char* argv[]) {
	if(argc < 2) {
		_tprintf(L"A TAS movie filename wasn't specified!\nExiting...\n");
		return 0;
	}

	_tprintf(L"Parsing file...\n");
	reports = parse_tas(argv[1], &meta);
	_tprintf(L"Parsed file!\n");
	desired_framerate = 1.f / meta.fps;

	// find pid
	HWND window = FindWindow(HAT_WINDOW, HAT_TITLE);
	DWORD pid = 0;
	DWORD tid = GetWindowThreadProcessId(window, &pid);

	// get process handle passing in the pid
	process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
	if(process == NULL) {
		_tprintf(L"Failed to open the HatinTimeGame process!\nExiting...\n");
		return 0;
	}
	// get thread handle passing in the tid
	thread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
	if(thread == NULL) {
		_tprintf(L"Failed to open the main thread!\nExiting...\n");
		return 0;
	}

	unsigned long* base_address = NULL;
	unsigned long* xinput_address = NULL;
	unsigned long* msvcr_address = NULL;

	// find base addresses and store them
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
	if(snapshot == INVALID_HANDLE_VALUE) {
		_tprintf(L"Failed to create snapshot of modules!\nExiting...\n");
		return 0;
	}
	MODULEENTRY32 entry;
	entry.dwSize = sizeof(MODULEENTRY32);
	if(Module32First(snapshot, &entry)) {
		do {
			if(_tcsicmp(entry.szModule, HAT_EXE_NAME) == 0) base_address = (unsigned long*)entry.modBaseAddr;
			else if(_tcsicmp(entry.szModule, XINPUT_DLL_NAME) == 0) xinput_address = (unsigned long*)entry.modBaseAddr;
			else if(_tcsicmp(entry.szModule, MSVCR_DLL_NAME) == 0) msvcr_address = (unsigned long*)entry.modBaseAddr;
			if(base_address != NULL && xinput_address != NULL && msvcr_address != NULL) break;
		}
		while(Module32Next(snapshot, &entry));
	}
	CloseHandle(snapshot);
	if(base_address == NULL || xinput_address == NULL || msvcr_address == NULL) {
		_tprintf(L"Failed to find base address of " HAT_EXE_NAME ", " XINPUT_DLL_NAME ", or " MSVCR_DLL_NAME "!\nExiting...\n");
		return 0;
	}

	// debug for breakpoints
	DebugActiveProcess(pid);
	DebugSetProcessKillOnExit(FALSE); // we want it to keep running at end-of-TAS

	// patch delta
	WriteProcessMemory(process, base_address + DELTA_WRITE_CODE_OFFSET, nop_delta_buf, sizeof(nop_delta_buf), NULL); // code
	WriteProcessMemory(process, base_address + DELTA_DATA_OFFSET, &desired_framerate, sizeof(desired_framerate), NULL); // data
	_tprintf(L"Wrote a fixed delta time of %f!\n", desired_framerate);

	// playing
	//

	printf(meta.type == IMMEDIATE ? "Playing %s in two seconds!\n" : "Waiting to play %s!\n", meta.name);
	Sleep(2000); // give user time to give hat focus

	bool started = false;
	int i = 0;
	long long xinput_player = -1;

	double started_time = -1; // for giving an accurate ending time
	double game_time = -1;
	double old_game_time = -1;
	double act_time = -1;
	double old_act_time = -1;

	bool timer_paused = false;
	bool old_timer_paused = false;

	BYTE* xinput_state_address = NULL;
	BYTE* fps_address = NULL;

	CONTEXT context;
	context.ContextFlags = CONTEXT_FULL;

	DEBUG_EVENT debug_event;

	// rand
	void* new_rand = NULL;
	unsigned long old_rand_prot;

	// main loop
	while(true) {
		WriteProcessMemory(process, base_address + TICK_CODE_OFFSET, &interrupt_op, sizeof(interrupt_op), NULL);
		WaitForDebugEvent(&debug_event, INFINITE);

		SuspendThread(thread);

		// handle breakpoints, else tell the game to handle it by itself
		// main tick breakpoint -- code continues under these if/else ifs
		if(debug_event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT && debug_event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && debug_event.u.Exception.ExceptionRecord.ExceptionAddress == base_address + TICK_CODE_OFFSET) {
			GetThreadContext(thread, &context);
			context.Rip -= 1;
			SetThreadContext(thread, &context);
		}
		// about to call XInputGetState -- save function arguments
		else if(started && debug_event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT && debug_event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && debug_event.u.Exception.ExceptionRecord.ExceptionAddress == (BYTE*)base_address + GET_STATE_CODE_OFFSET) {
			GetThreadContext(thread, &context);
			context.Rip -= 1;
			xinput_player = context.Rcx;
			xinput_state_address = (BYTE*)context.Rdx;
			SetThreadContext(thread, &context);

			WriteProcessMemory(process, (BYTE*)base_address + POST_GET_STATE_CODE_OFFSET, &interrupt_op, sizeof(interrupt_op), NULL);
			WriteProcessMemory(process, (BYTE*)base_address + GET_STATE_CODE_OFFSET, &orig_xinput, sizeof(orig_xinput), NULL);
			ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_CONTINUE);
			ResumeThread(thread);
			continue;
		}
		// finished calling XInputGetState -- overwrite output with correct player input as specified in the TAS
		else if(started && debug_event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT && debug_event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && debug_event.u.Exception.ExceptionRecord.ExceptionAddress == (BYTE*)base_address + POST_GET_STATE_CODE_OFFSET) {
			GetThreadContext(thread, &context);
			context.Rip -= 1;
			SetThreadContext(thread, &context);

			// write inputs
			WriteProcessMemory(process, xinput_state_address + 4, &reports[i].gamepads[xinput_player], sizeof(state_report), NULL);

			WriteProcessMemory(process, (BYTE*)base_address + GET_STATE_CODE_OFFSET, &interrupt_op, sizeof(interrupt_op), NULL);
			WriteProcessMemory(process, (BYTE*)base_address + POST_GET_STATE_CODE_OFFSET, &orig_post_xinput, sizeof(orig_post_xinput), NULL);
			ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_CONTINUE);
			ResumeThread(thread);
			continue;
		}
		else {
			ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_EXCEPTION_NOT_HANDLED);
			ResumeThread(thread);
			continue;
		}

		/*
		** main breakpoint hit!
		*/
		
		old_timer_paused = timer_paused; // like in ASL
		ReadProcessMemory(process, base_address + TIMER_OFFSET + (0x10 / 4), &timer_paused, sizeof(timer_paused), NULL); // read timer status
		old_game_time = game_time; // like in ASL
		ReadProcessMemory(process, base_address + TIMER_OFFSET + (0x34 / 4), &game_time, sizeof(game_time), NULL); // read game time
		old_act_time = act_time; // like in ASL
		ReadProcessMemory(process, base_address + TIMER_OFFSET + (0x3C / 4), &act_time, sizeof(act_time), NULL); // read act time
	
		// start if fullgame type, and game timer just started/game timer just resumed, OR start if IL type, and act timer just started from zero, OR start immediately if type is immediate
		// !started in the beginning should make it not evaluate all the conditions once TAS has started
		if(!started && (meta.type == FULLGAME && game_time > 0 && (old_game_time == 0 || (!timer_paused && old_timer_paused)) || (meta.type == INDIVIDUAL && act_time > 0 && old_act_time == 0) || (meta.type == IMMEDIATE))) {
			// write the input breakpoint on starting
			WriteProcessMemory(process, (BYTE*)base_address + GET_STATE_CODE_OFFSET, &interrupt_op, sizeof(interrupt_op), NULL); // code
			WriteProcessMemory(process, (BYTE*)base_address + POST_GET_STATE_CODE_OFFSET, &interrupt_op, sizeof(interrupt_op), NULL); // code

			started_time = game_time - desired_framerate; // for giving a duration at the end message, subtract a frame, as start time gets set a frame into the replay

			// resolve and read current FPS value, so we can restore it at end-of-TAS
			fps_address = resolve_ptr(base_address + FPS_PTR_OFFSET) + 0x710;
			ReadProcessMemory(process, fps_address, &original_fps, sizeof(original_fps), NULL);
			original_delta = 1.0 / original_fps;

			// write the FPS value that'll be used for the TAS
			WriteProcessMemory(process, fps_address, &meta.fps, sizeof(meta.fps), NULL);

			// rand replacement
			//

			// allocate new memory
			new_rand = VirtualAllocEx(process, NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			WriteProcessMemory(process, new_rand, newrand_buf, sizeof(newrand_buf), NULL);
			_tprintf(L"New rand at 0x%08X!\n\n", new_rand);

			// make rand code writable
			VirtualProtectEx(process, (BYTE*)msvcr_address + RAND_CODE_OFFSET, 3, PAGE_EXECUTE_READWRITE, &old_rand_prot);

			// save old instruction
			ReadProcessMemory(process, (BYTE*)msvcr_address + RAND_CODE_OFFSET, orig_rand, sizeof(orig_rand), NULL);

			// patch to jump to new rand
			int jump_target = (BYTE*)new_rand - ((BYTE*)msvcr_address + RAND_CODE_OFFSET) - 5; // 5 is the length of the jmp instruction
			WriteProcessMemory(process, (BYTE*)msvcr_address + RAND_CODE_OFFSET, &jmp_op, sizeof(jmp_op), NULL);
			WriteProcessMemory(process, (BYTE*)msvcr_address + RAND_CODE_OFFSET + 1, &jump_target, sizeof(jump_target), NULL);

			// set start flag
			started = true;
			if(meta.type != IMMEDIATE) {
				printf("Starting playback!\n");
			}
		}

		// restart loop if it hasn't started yet
		else if(!started) {
			WriteProcessMemory(process, base_address + TICK_CODE_OFFSET, &orig_tick, sizeof(orig_tick), NULL);
			ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_CONTINUE);
			ResumeThread(thread);
			continue;
		}

		// reduces desyncs, not sure why this happens though
		if(game_time == old_game_time) {
			WriteProcessMemory(process, base_address + TICK_CODE_OFFSET, &orig_tick, sizeof(orig_tick), NULL);
			ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_CONTINUE);
			ResumeThread(thread);
			continue;
		}

		// write commands
		// only write if it ever changes, for performance - not sure if needed though
		if(meta.changes_speed) WriteProcessMemory(process, fps_address, &reports[i].aux.speed, sizeof(reports[i].aux.speed), NULL);

		// write rng
		// sequence length
		WriteProcessMemory(process, (BYTE*)new_rand + 0x1F8, &reports[i].aux.rand_seq_max, sizeof(reports[i].aux.rand_seq_max), NULL);
		// sequence index
		WriteProcessMemory(process, (BYTE*)new_rand + 0x1FC, &zero, sizeof(zero), NULL);
		// sequence
		WriteProcessMemory(process, (BYTE*)new_rand + 0x200, &reports[i].aux.rand_seq, (reports[i].aux.rand_seq_max + 1) * 4, NULL);

		// increment local frame count
		i++;

		WriteProcessMemory(process, base_address + TICK_CODE_OFFSET, &orig_tick, sizeof(orig_tick), NULL); // code
		ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_CONTINUE);
		ResumeThread(thread);

		// allow ending the TAS prematurely (0x8000 means key down)
		if(GetKeyState(VK_ESCAPE) & 0x8000) break;

		if(i >= meta.length) break; // break when done
	}
	// end main loop

	// cleanup
	//

	// game memory
	WriteProcessMemory(process, (BYTE*)base_address + GET_STATE_CODE_OFFSET, &orig_xinput, sizeof(orig_xinput), NULL);
	WriteProcessMemory(process, (BYTE*)base_address + POST_GET_STATE_CODE_OFFSET, &orig_post_xinput, sizeof(orig_post_xinput), NULL);
	WriteProcessMemory(process, base_address + DELTA_DATA_OFFSET, &original_delta, sizeof(original_delta), NULL); // put back a fixed delta matching the original fps setting
	WriteProcessMemory(process, fps_address, &original_fps, sizeof(original_fps), NULL); // write back original fps value

	// rand
	WriteProcessMemory(process, (BYTE*)msvcr_address + RAND_CODE_OFFSET, orig_rand, sizeof(orig_rand), NULL);
	VirtualFreeEx(process, new_rand, 0x1000, MEM_RELEASE);
	VirtualProtectEx(process, (BYTE*)msvcr_address + RAND_CODE_OFFSET, 3, old_rand_prot, &old_rand_prot);

	DebugActiveProcessStop(pid);
	CloseHandle(thread);
	CloseHandle(process);
	_tprintf(L"Done in %lf!\n", meta.type == INDIVIDUAL ? act_time : game_time - started_time);

	return 0;
}