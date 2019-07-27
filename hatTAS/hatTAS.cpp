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

#define GET_STATE_CODE_OFFSET (0x9AC3EA)
#define POST_GET_STATE_CODE_OFFSET (0x9AC3EF)
#define TICK_CODE_OFFSET (0x3D7A70)
#define DELTA_WRITE_CODE_OFFSET (0x9D7F86)
#define DELTA_DATA_OFFSET (0x104D6E8)
#define TOTAL_FRAMES_OFFSET (0x11D07A0)
#define TIMER_OFFSET (0x10719D0)
#define FPS_PTR_OFFSET (0x11C27E0)
// msvcr offsets
#define RAND_CODE_OFFSET (0x7947D)

const BYTE nop_delta_buf[8] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
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
char* movie_filename = NULL;

// layout
layout_def* layout = NULL;
char* layout_filename = NULL;

BYTE* base_address = NULL;
BYTE* xinput_address = NULL;
BYTE* msvcr_address = NULL;

BYTE* fps_address = NULL;
void* new_rand = NULL;
unsigned long old_rand_prot;

HWND window;
HANDLE process;
HANDLE thread;
DWORD pid;
DWORD tid;

// only call after process handle has been made
BYTE* resolve_ptr(void* address) {
	BYTE* resolved;
	ReadProcessMemory(process, address, &resolved, 8, NULL);
	return resolved;
}

int init() {
	// find pid
	window = FindWindow(HAT_WINDOW, HAT_TITLE);
	tid = GetWindowThreadProcessId(window, &pid);

	// get process handle passing in the pid
	process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
	if(process == NULL) {
		_tprintf(L"Failed to open the HatinTimeGame process!\n");
		return 0;
	}
	// get thread handle passing in the tid
	thread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
	if(thread == NULL) {
		_tprintf(L"Failed to open the main thread!\n");
		return 0;
	}

	// find base addresses and store them
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
	if(snapshot == INVALID_HANDLE_VALUE) {
		_tprintf(L"Failed to create snapshot of modules!\n");
		return 0;
	}
	MODULEENTRY32 entry;
	entry.dwSize = sizeof(MODULEENTRY32);
	if(Module32First(snapshot, &entry)) {
		do {
			if(_tcsicmp(entry.szModule, HAT_EXE_NAME) == 0) base_address = entry.modBaseAddr;
			else if(_tcsicmp(entry.szModule, XINPUT_DLL_NAME) == 0) xinput_address = entry.modBaseAddr;
			else if(_tcsicmp(entry.szModule, MSVCR_DLL_NAME) == 0) msvcr_address = entry.modBaseAddr;
			if(base_address != NULL && xinput_address != NULL && msvcr_address != NULL) break;
		}
		while(Module32Next(snapshot, &entry));
	}
	CloseHandle(snapshot);
	if(base_address == NULL || xinput_address == NULL || msvcr_address == NULL) {
		_tprintf(L"Failed to find base address of " HAT_EXE_NAME ", " XINPUT_DLL_NAME ", or " MSVCR_DLL_NAME "!\n");
		return 0;
	}

	// debug for breakpoints
	DebugActiveProcess(pid);
	DebugSetProcessKillOnExit(FALSE); // we want it to keep running at end-of-TAS

	// rand replacement
	//

	// allocate new memory
	new_rand = VirtualAllocEx(process, NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	WriteProcessMemory(process, new_rand, newrand_buf, sizeof(newrand_buf), NULL);
	_tprintf(L"New rand at 0x%08X!\n", new_rand);

	// make rand code writable
	VirtualProtectEx(process, msvcr_address + RAND_CODE_OFFSET, 3, PAGE_EXECUTE_READWRITE, &old_rand_prot);

	// save old instruction
	ReadProcessMemory(process, msvcr_address + RAND_CODE_OFFSET, orig_rand, sizeof(orig_rand), NULL);

	return 1;
}

// google
void ClearScreen() {
	HANDLE                     hStdOut;
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	DWORD                      count;
	DWORD                      cellCount;
	COORD                      homeCoords = { 0, 0 };

	hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if(hStdOut == INVALID_HANDLE_VALUE) return;

	/* Get the number of cells in the current buffer */
	if(!GetConsoleScreenBufferInfo(hStdOut, &csbi)) return;
	cellCount = csbi.dwSize.X *csbi.dwSize.Y;

	/* Fill the entire buffer with spaces */
	if(!FillConsoleOutputCharacter(hStdOut, (TCHAR) ' ', cellCount, homeCoords, &count)) return;

	/* Fill the entire buffer with the current colors and attributes */
	if(!FillConsoleOutputAttribute(hStdOut, csbi.wAttributes, cellCount, homeCoords, &count)) return;

	/* Move the cursor home */
	SetConsoleCursorPosition(hStdOut, homeCoords);
}

BOOL WINAPI cleanup_handler(DWORD signal) {
	if(signal == CTRL_C_EVENT) {
		// game memory
		WriteProcessMemory(process, base_address + TICK_CODE_OFFSET, &orig_tick, sizeof(orig_tick), NULL);
		WriteProcessMemory(process, base_address + GET_STATE_CODE_OFFSET, &orig_xinput, sizeof(orig_xinput), NULL);
		WriteProcessMemory(process, base_address + POST_GET_STATE_CODE_OFFSET, &orig_post_xinput, sizeof(orig_post_xinput), NULL);
		WriteProcessMemory(process, base_address + DELTA_DATA_OFFSET, &original_delta, sizeof(original_delta), NULL); // put back a fixed delta matching the original fps setting
		if(fps_address != NULL) WriteProcessMemory(process, fps_address, &original_fps, sizeof(original_fps), NULL); // write back original fps value

		// rand
		WriteProcessMemory(process, msvcr_address + RAND_CODE_OFFSET, orig_rand, sizeof(orig_rand), NULL);
		VirtualFreeEx(process, new_rand, 0x1000, MEM_RELEASE);
		VirtualProtectEx(process, msvcr_address + RAND_CODE_OFFSET, 3, old_rand_prot, &old_rand_prot);

		ResumeThread(thread);
		DebugActiveProcessStop(pid);
		CloseHandle(thread);
		CloseHandle(process);

		exit(0);
		return TRUE;
	}

	return FALSE;
}

void play_movie() {
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

	bool rshift_down = false;
	bool old_rshift_down = false;
	bool lctrl_down = false;
	bool old_lctrl_down = false;
	bool framestepping = false;

	BYTE* xinput_state_address = NULL;

	CONTEXT context;
	context.ContextFlags = CONTEXT_FULL;

	DEBUG_EVENT debug_event;

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
		else if(started && debug_event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT && debug_event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && debug_event.u.Exception.ExceptionRecord.ExceptionAddress == base_address + GET_STATE_CODE_OFFSET) {
			GetThreadContext(thread, &context);
			context.Rip -= 1;
			xinput_player = context.Rcx;
			xinput_state_address = (BYTE*)context.Rdx;
			SetThreadContext(thread, &context);

			WriteProcessMemory(process, base_address + POST_GET_STATE_CODE_OFFSET, &interrupt_op, sizeof(interrupt_op), NULL);
			WriteProcessMemory(process, base_address + GET_STATE_CODE_OFFSET, &orig_xinput, sizeof(orig_xinput), NULL);
			ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_CONTINUE);
			ResumeThread(thread);
			continue;
		}
		// finished calling XInputGetState -- overwrite output with correct player input as specified in the TAS
		else if(started && debug_event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT && debug_event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && debug_event.u.Exception.ExceptionRecord.ExceptionAddress == base_address + POST_GET_STATE_CODE_OFFSET) {
			GetThreadContext(thread, &context);
			context.Rip -= 1;
			SetThreadContext(thread, &context);

			// write inputs
			WriteProcessMemory(process, xinput_state_address + 4, &reports[i].gamepads[xinput_player], sizeof(state_report), NULL);

			WriteProcessMemory(process, base_address + GET_STATE_CODE_OFFSET, &interrupt_op, sizeof(interrupt_op), NULL);
			WriteProcessMemory(process, base_address + POST_GET_STATE_CODE_OFFSET, &orig_post_xinput, sizeof(orig_post_xinput), NULL);
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
		ReadProcessMemory(process, base_address + TIMER_OFFSET + 0x10, &timer_paused, sizeof(timer_paused), NULL); // read timer status
		old_game_time = game_time; // like in ASL
		ReadProcessMemory(process, base_address + TIMER_OFFSET + 0x34, &game_time, sizeof(game_time), NULL); // read game time
		old_act_time = act_time; // like in ASL
		ReadProcessMemory(process, base_address + TIMER_OFFSET + 0x3C, &act_time, sizeof(act_time), NULL); // read act time
		old_rshift_down = rshift_down; // like in ASL
		rshift_down = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) == 0x8000; // framestep
		old_lctrl_down = lctrl_down; // like in ASL
		lctrl_down = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) == 0x8000; // exit framestep

		// start if fullgame type, and game timer just started/game timer just resumed, OR start if IL type, and act timer just started from zero, OR start immediately if type is immediate
		// !started in the beginning should make it not evaluate all the conditions once TAS has started
		if(!started && (meta.type == FULLGAME && game_time > 0 && (old_game_time == 0 || (!timer_paused && old_timer_paused)) || (meta.type == INDIVIDUAL && act_time > 0 && old_act_time == 0) || (meta.type == IMMEDIATE))) {
			// write the input breakpoint on starting
			WriteProcessMemory(process, base_address + GET_STATE_CODE_OFFSET, &interrupt_op, sizeof(interrupt_op), NULL); // code
			WriteProcessMemory(process, base_address + POST_GET_STATE_CODE_OFFSET, &interrupt_op, sizeof(interrupt_op), NULL); // code

			printf("Beginning, parsing %s... ", movie_filename);
			reports = parse_tas(movie_filename, &meta);
			_tprintf(L"done!\n");
			desired_framerate = 1.f / meta.fps;

			if(meta.type == IMMEDIATE) {
				_tprintf(L"Starting in two seconds...\n");
				Sleep(2000);
			}

			// patch delta
			WriteProcessMemory(process, base_address + DELTA_WRITE_CODE_OFFSET, nop_delta_buf, sizeof(nop_delta_buf), NULL); // code
			WriteProcessMemory(process, base_address + DELTA_DATA_OFFSET, &desired_framerate, sizeof(desired_framerate), NULL); // data

			started_time = game_time - desired_framerate; // for giving a duration at the end message, subtract a frame, as start time gets set a frame into the replay

			// resolve and read current FPS value, so we can restore it at end-of-TAS
			fps_address = resolve_ptr(base_address + FPS_PTR_OFFSET) + 0x710;
			ReadProcessMemory(process, fps_address, &original_fps, sizeof(original_fps), NULL);
			original_delta = 1.0 / original_fps;

			// write the FPS value that'll be used for the TAS
			WriteProcessMemory(process, fps_address, &meta.fps, sizeof(meta.fps), NULL);

			// reset rng
			// sequence length
			WriteProcessMemory(process, (BYTE*)new_rand + 0x1F8, &zero, sizeof(zero), NULL);
			// sequence index
			WriteProcessMemory(process, (BYTE*)new_rand + 0x1FC, &zero, sizeof(zero), NULL);
			// sequence
			WriteProcessMemory(process, (BYTE*)new_rand + 0x200, &zero, sizeof(zero), NULL);

			// patch to jump to new rand
			int jump_target = (BYTE*)new_rand - (msvcr_address + RAND_CODE_OFFSET) - 5; // 5 is the length of the jmp instruction
			WriteProcessMemory(process, msvcr_address + RAND_CODE_OFFSET, &jmp_op, sizeof(jmp_op), NULL);
			WriteProcessMemory(process, msvcr_address + RAND_CODE_OFFSET + 1, &jump_target, sizeof(jump_target), NULL);

			// set start flag
			started = true;
			_tprintf(L"Starting playback!\n");
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

		// framestep logic
		if(framestepping || rshift_down) {
			printf("Stepping at frame %d\n", i);
			do {
				Sleep(5);

				old_rshift_down = rshift_down;
				rshift_down = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) == 0x8000; // framestep
				old_lctrl_down = lctrl_down;
				lctrl_down = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) == 0x8000; // exit framestep

				if(rshift_down) framestepping = true;
				if(lctrl_down) framestepping = false;
			}
			while((!rshift_down || old_rshift_down) && !lctrl_down);
		}

		// draw layout
		if(layout != NULL) {
			ClearScreen();

			// iterate items
			for(int ii = 0; ii < layout->item_n; ii++) {
				const layout_itm* item = layout->items[ii];
				const layout_type type = item->type;
				const layout_op op = item->op;
				
				// likely not actually a u64; gets cast later
				// also intermediate storage during resolving
				unsigned long long* resolved = (unsigned long long*)calloc(item->ppath_n, sizeof(unsigned long long));
				unsigned long long final_value_fixed = 0;
				double final_value_float = 0;

				// iterate ppaths
				for(int iii = 0; iii < item->ppath_n; iii++) {
					// iterate offsets
					for(int iiii = 0; iiii < item->ppaths[iii].offset_n; iiii++) {
						// resolve
						if(iiii < item->ppaths[iii].offset_n - 1) {
							resolved[iii] = (unsigned long long)resolve_ptr((void*)(resolved[iii] + item->ppaths[iii].offsets[iiii]));
						}
						// last iter
						else {
							resolved[iii] += item->ppaths[iii].offsets[iiii];
							ReadProcessMemory(process, base_address + resolved[iii], &resolved[iii], sizeof(resolved[iii]), NULL);
						}
					}

					// perform basic ops (this is awful)
					switch(op) {
						case OP_ADD: {
							switch(type) {
								case ITM_U32: final_value_fixed += (unsigned long long)*(unsigned int*)&resolved[iii]; break;
								case ITM_S32: final_value_fixed += (unsigned long long)*(int*)&resolved[iii]; break;
								case ITM_F32: final_value_float += (double)*(float*)&resolved[iii]; break;
								case ITM_F64: final_value_float += *(double*)&resolved[iii]; break;
							}
							break;
						}
						case OP_MAG: {
							switch(type) {
								case ITM_F32: final_value_float += pow((double)*(float*)&resolved[iii], 2); break;
								case ITM_F64: final_value_float += pow(*(double*)&resolved[iii], 2); break;
							}
							break;
						}
					}
				}

				if(op == OP_MAG) {
					final_value_float = sqrt(final_value_float);
				}

				if(type == ITM_F32 || type == ITM_F64) {
					printf(layout->formats[ii], final_value_float);
				}
				else {
					printf(layout->formats[ii], final_value_fixed);
				}

				free(resolved);
			}
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
	WriteProcessMemory(process, msvcr_address + RAND_CODE_OFFSET, orig_rand, sizeof(orig_rand), NULL);
	WriteProcessMemory(process, base_address + GET_STATE_CODE_OFFSET, &orig_xinput, sizeof(orig_xinput), NULL);
	WriteProcessMemory(process, base_address + POST_GET_STATE_CODE_OFFSET, &orig_post_xinput, sizeof(orig_post_xinput), NULL);
	WriteProcessMemory(process, fps_address, &original_fps, sizeof(original_fps), NULL);

	_tprintf(L"Done in %f!\n\n", meta.type == INDIVIDUAL ? act_time : game_time - started_time);
}

int main(int argc, char* argv[]) {
	bool repeat = false;

	for(int i = 1; i < argc; i++) {
		if(argv[i][0] == '-') {
			switch(argv[i][1]) {
				case 'r': repeat = true; break;
				case 'l': layout_filename = argv[i] + 2; break;
			}
		}
		else movie_filename = argv[i];
	}

	if(movie_filename == NULL) {
		_tprintf(L"A TAS movie filename wasn't specified!\nUsage: hatTAS.exe [-r] [-l<layout.hlay>] <movie.htas>\n\nExiting...\n");
		return 0;
	}

	if(!init()) {
		_tprintf(L"Failed initializing...\nExiting...\n");
		return 0;
	}

	// catch ctrl+c for cleanup reasons
	SetConsoleCtrlHandler(cleanup_handler, TRUE);

	// parse once, for example to get correct starting condition
	reports = parse_tas(movie_filename, &meta);

	// parse layout just once
	if(layout_filename != NULL) {
		layout = parse_lay(layout_filename);
	}

	_tprintf(L"Done initializing, ready!\n\n");

	// main main loop
	do {
		play_movie();
	}
	while(repeat);

	// cleanup
	//

	// game memory
	WriteProcessMemory(process, base_address + TICK_CODE_OFFSET, &orig_tick, sizeof(orig_tick), NULL);
	WriteProcessMemory(process, base_address + GET_STATE_CODE_OFFSET, &orig_xinput, sizeof(orig_xinput), NULL);
	WriteProcessMemory(process, base_address + POST_GET_STATE_CODE_OFFSET, &orig_post_xinput, sizeof(orig_post_xinput), NULL);
	WriteProcessMemory(process, base_address + DELTA_DATA_OFFSET, &original_delta, sizeof(original_delta), NULL); // put back a fixed delta matching the original fps setting
	if(fps_address != NULL) WriteProcessMemory(process, fps_address, &original_fps, sizeof(original_fps), NULL); // write back original fps value

	// rand
	WriteProcessMemory(process, msvcr_address + RAND_CODE_OFFSET, orig_rand, sizeof(orig_rand), NULL);
	VirtualFreeEx(process, new_rand, 0x1000, MEM_RELEASE);
	VirtualProtectEx(process, msvcr_address + RAND_CODE_OFFSET, 3, old_rand_prot, &old_rand_prot);

	ResumeThread(thread);
	DebugActiveProcessStop(pid);
	CloseHandle(thread);
	CloseHandle(process);

	return 0;
}