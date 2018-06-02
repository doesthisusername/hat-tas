#include "stdafx.h"
#include <Windows.h>
#include <stdio.h>
#include <tlhelp32.h>
#include "parse.h"

#define HAT_EXE_NAME L"HatinTimeGame.exe"
#define DINPUT_DLL_NAME L"DINPUT8.dll"
// divide by 4 because of pointer arithmetic
#define PIB_GET_INTERFACE_OFFSET (0x89ABF0 / 4)
#define GETDF_DI_JOYSTICK_OFFSET (0xFCB0 / 4)

#define TICK_CODE_OFFSET (0x3E8840 / 4)
#define DELTA_WRITE_CODE_OFFSET (0x12CC76 / 4)
#define ANALOG_WRITE_CODE_OFFSET (0x1697A / 4)
#define BUTTON_WRITE_CODE_OFFSET (0x6CE1 / 4)
#define DELTA_DATA_OFFSET (0x75DAF8 / 4)
#define INPUT_DATA_PTR_OFFSET (0x1195850 / 4)
#define TOTAL_FRAMES_OFFSET (0x11802E0 / 4)
#define TIMER_OFFSET (0x101B720 / 4)
#define FPS_PTR_OFFSET (0x116D250 / 4)

// it only wants to write on 4-byte aligned boundaries, so the first two bytes are filler, as the code we like isn't 4-byte aligned
const BYTE nop_delta_buf[10] = { 0x79, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
const double desired_framerate = 1.0 / 60.0; // desired delta time
float original_fps = 60.f;

// breakpoint code
const BYTE interrupt_op = 0xCC;
const BYTE orig_tick = 0x48;

// dinput analog nop
const BYTE nop_analog_buf[6] = { 0x8B, 0xC3, 0x90, 0x90, 0x90, 0x90 };
const BYTE orig_analog_buf[6] = { 0x8B, 0xC3, 0x41, 0x89, 0x1C, 0x17 };

// dinput button nop
const BYTE nop_button_buf[5] = { 0x75, 0x90, 0x90, 0x90, 0x90 };
const BYTE orig_button_buf[5] = { 0x75, 0x41, 0x88, 0x1C, 0x17 };

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

	// find pid
	HWND window = FindWindow(L"LaunchUnrealUWindowsClient", L"A Hat in Time (64-bit, Final Release Shipping PC, DX9)");
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
	unsigned long* dinput_address = NULL;

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
			else if(_tcsicmp(entry.szModule, DINPUT_DLL_NAME) == 0) dinput_address = (unsigned long*)entry.modBaseAddr;
			if(base_address != NULL && dinput_address != NULL) break;
		}
		while(Module32Next(snapshot, &entry));
	}
	CloseHandle(snapshot);
	if(base_address == NULL || dinput_address == NULL) {
		_tprintf(L"Failed to find base address of HatinTimeGame.exe or DINPUT8.dll!\nExiting...\n");
		return 0;
	}

	// debug for breakpoints
	DebugActiveProcess(pid);
	DebugSetProcessKillOnExit(FALSE); // we want it to keep running at end-of-TAS


	// patch delta
	WriteProcessMemory(process, base_address + PIB_GET_INTERFACE_OFFSET + DELTA_WRITE_CODE_OFFSET, nop_delta_buf, sizeof(nop_delta_buf), NULL); // code
	WriteProcessMemory(process, base_address + PIB_GET_INTERFACE_OFFSET + DELTA_DATA_OFFSET, &desired_framerate, sizeof(desired_framerate), NULL); // data
	_tprintf(L"Wrote a fixed delta time of %lf!\n\n", desired_framerate);

	// add software breakpoint
	WriteProcessMemory(process, base_address + TICK_CODE_OFFSET, &interrupt_op, sizeof(interrupt_op), NULL);

	Sleep(750); // give user time to give hat focus
	printf(meta.type == IMMEDIATE ? "Playing %s!\n" : "Waiting to play %s!\n", meta.name);

	bool started = false;
	int i = 0;

	double started_time = -1; // for giving an accurate ending time
	double game_time = -1;
	double old_game_time = -1;
	double act_time = -1;
	double old_act_time = -1;

	bool timer_paused = false;
	bool old_timer_paused = false;

	BYTE* analog_address = NULL;
	BYTE* buttons_address = NULL;
	BYTE* fps_address = NULL;

	CONTEXT context;
	DEBUG_EVENT debug_event;

	// TODO: make the loop less intensive
	// main loop
	while(true) {
		// allow ending the TAS prematurely (0x8000 means key down)
		if(GetKeyState(VK_ESCAPE) & 0x8000) break;

		WriteProcessMemory(process, base_address + TICK_CODE_OFFSET, &interrupt_op, sizeof(interrupt_op), NULL);
		WaitForDebugEvent(&debug_event, INFINITE);

		SuspendThread(thread);

		// handle breakpoint, else tell the game to handle it by itself
		if(debug_event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT && debug_event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && debug_event.u.Exception.ExceptionRecord.ExceptionAddress == base_address + TICK_CODE_OFFSET) {
			GetThreadContext(thread, &context);
			context.Rip -= 1;
			SetThreadContext(thread, &context);
		}
		else {
			ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_EXCEPTION_NOT_HANDLED);
			ResumeThread(thread);
			continue;
		}

		/*
		** breakpoint hit!
		*/
		
		old_timer_paused = timer_paused; // like in ASL
		ReadProcessMemory(process, base_address + TIMER_OFFSET + (0x10 / 4), &timer_paused, sizeof(timer_paused), NULL); // read timer status
		old_game_time = game_time; // like in ASL
		ReadProcessMemory(process, base_address + TIMER_OFFSET + (0x34 / 4), &game_time, sizeof(game_time), NULL); // read game time
		old_act_time = act_time; // like in ASL
		ReadProcessMemory(process, base_address + TIMER_OFFSET + (0x3C / 4), &act_time, sizeof(act_time), NULL); // read act time
	
		// TODO: implement fullgame type
		// start if fullgame type, and game timer just started/game timer just resumed, OR start if IL type, and act timer just started from zero, OR start immediately if type is immediate
		// !started in the beginning should make it not evaluate all the conditions once TAS has started
		if(!started && (meta.type == FULLGAME && game_time > 0 && (old_game_time == 0 || (!timer_paused && old_timer_paused)) || (meta.type == INDIVIDUAL && act_time > 0 && old_act_time == 0) || (meta.type == IMMEDIATE))) {
			// write the input code patch on starting
			WriteProcessMemory(process, dinput_address + ANALOG_WRITE_CODE_OFFSET, nop_analog_buf, sizeof(nop_analog_buf), NULL); // code
			WriteProcessMemory(process, dinput_address + GETDF_DI_JOYSTICK_OFFSET + BUTTON_WRITE_CODE_OFFSET, nop_button_buf, sizeof(nop_button_buf), NULL); // code
			started_time = game_time - desired_framerate; // for giving a duration at the end message, subtract a frame, as start time gets set a frame into the replay

			// pointer path traversal																													 
			analog_address = resolve_ptr(base_address + INPUT_DATA_PTR_OFFSET);
			analog_address = resolve_ptr(analog_address + 0x760);
			analog_address = resolve_ptr(analog_address + 0xA0);
			analog_address = resolve_ptr(analog_address + 0x8); // analog_address now points to the right place
			buttons_address = analog_address + 0xE4; // buttons

			// resolve and read current FPS value, so we can restore it at end-of-TAS
			fps_address = resolve_ptr(base_address + FPS_PTR_OFFSET) + 0x710;
			ReadProcessMemory(process, fps_address, &original_fps, sizeof(original_fps), NULL);

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

		// write inputs
		WriteProcessMemory(process, analog_address, &reports[i].analog, sizeof(analog_report), NULL);
		WriteProcessMemory(process, buttons_address, &reports[i].buttons, sizeof(buttons_report), NULL);

		// write commands (just SPEED at the moment)
		// only write if it ever changes, for performance - not sure if needed though
		if(meta.changes_speed) WriteProcessMemory(process, fps_address, &reports[i].aux.speed, sizeof(float), NULL);

		WriteProcessMemory(process, base_address + TICK_CODE_OFFSET, &orig_tick, sizeof(orig_tick), NULL);
		ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_CONTINUE);
		ResumeThread(thread);

		// increment local frame count
		i++;
		if(i >= meta.length) break; // break when done
	}
	// end main loop

	// cleanup
	WriteProcessMemory(process, dinput_address + ANALOG_WRITE_CODE_OFFSET, orig_analog_buf, sizeof(orig_analog_buf), NULL); // put original back so dinput device analog sticks work again
	WriteProcessMemory(process, dinput_address + GETDF_DI_JOYSTICK_OFFSET + BUTTON_WRITE_CODE_OFFSET, orig_button_buf, sizeof(orig_button_buf), NULL); // put original back so dinput device buttons work again
	WriteProcessMemory(process, fps_address, &original_fps, sizeof(original_fps), NULL); // write back original fps value

	DebugActiveProcessStop(pid);
	CloseHandle(thread);
	CloseHandle(process);
	_tprintf(L"Done in %lf!\n", meta.type == INDIVIDUAL ? act_time : game_time - started_time);

	return 0;
}