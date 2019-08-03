#include "stdafx.h"
#include <Windows.h>
#include <stdio.h>
#include <tlhelp32.h>

#pragma warning(disable:4996) // _CRT_SECURE_NO_WARNINGS substitute
#include "inih/INIReader.h"
#pragma warning(default:4996)

#include "parse.h"

#define HAT_WINDOW L"LaunchUnrealUWindowsClient"
#define HAT_TITLE L"A Hat in Time (64-bit, Final Release Shipping PC, DX9, Modding)"
#define HAT_EXE_NAME L"HatinTimeGame.exe"
#define XINPUT_DLL_NAME L"XINPUT1_3.dll"
#define MSVCR_DLL_NAME L"MSVCR100.dll"
#define PE_SECTION_PTR (0x3C)
#define PE_TS_OFFSET (0x08)

#define FRAMESTEP_SLEEP 5

// game pointers/addresses from config
unsigned int game_timestamp;
char timestamp_str[0x10];
INIReader* ini;
long delta_codes;
long num_rand;
void* xinput_get_state;
void* xinput_get_state_post;
void* tick_code;
void* delta_update[2];
void* delta_data;
void* speedrun_timer;
void* fps_ptr;
void* rand_code[4];
void* realtime_seconds_ptr;

const BYTE nop_delta_buf[8] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
double desired_framerate = 1.0 / 60.0; // desired delta time
float original_fps = 60.f; // fps setting before the TAS started
double original_delta = 1.0 / 60.0; // for restoring framerate properly

// breakpoint code
const BYTE interrupt_op = 0xCC;
const BYTE call_op = 0xE8;
const BYTE nop_op = 0x90;
const BYTE orig_tick = 0x40;
const BYTE orig_xinput = 0xE8;
const BYTE orig_post_xinput = 0x33;

// rand
const BYTE newrand_buf[0x38] = { 0x48, 0x83, 0xEC, 0x10, 0x53, 0x48, 0x31, 0xDB, 0x8B, 0x1D, 0xEE, 0x01, 0x00, 0x00, 0x48, 0x31, 0xC9, 0xFF, 0xC3, 0x3B, 0x1D, 0xDF, 0x01, 0x00, 0x00, 0x48, 0x0F, 0x47, 0xD9, 0x89, 0x1D, 0xD9, 0x01, 0x00, 0x00, 0x48, 0x8D, 0x0D, 0xD6, 0x01, 0x00, 0x00, 0x8B, 0x0C, 0x99, 0xC1, 0xE9, 0x10, 0x89, 0xC8, 0x5B, 0x48, 0x83, 0xC4, 0x10, 0xC3 };
BYTE orig_rand[4][6];
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

void* resolve_ptr_path(const char* path) {
	char* next;
	unsigned long long offset;

	if(!process || !base_address) {
		return NULL;
	}

	unsigned long long resolved = (unsigned long long)base_address + strtoull(path, &next, 0);

	while(path + strlen(path) > next) {
		offset = strtoull(next + 1, &next, 0);
		ReadProcessMemory(process, (void*)resolved, &resolved, 8, NULL);
		resolved += offset;
	}

	return (void*)resolved;
}

int init() {
	// find pid
	window = FindWindow(HAT_WINDOW, HAT_TITLE);
	tid = GetWindowThreadProcessId(window, &pid);

	// get process handle passing in the pid
	process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
	if(process == NULL) {
		_tprintf(L"Failed to open the " HAT_EXE_NAME " process!\n");
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

	// resolve pointers from versions.ini
	//

	// find timestamp in PE
	unsigned int first_section_rel;
	ReadProcessMemory(process, base_address + PE_SECTION_PTR, &first_section_rel, sizeof(first_section_rel), NULL);
	ReadProcessMemory(process, base_address + first_section_rel + PE_TS_OFFSET, &game_timestamp, sizeof(game_timestamp), NULL);

	// init reader
	ini = new INIReader("versions.ini");
	if(ini->ParseError()) {
		_tprintf(L"Failed to load versions.ini!\n");
		return 0;
	}

	// init timestamp
	snprintf(timestamp_str, sizeof(timestamp_str), "%u", game_timestamp);
	if(ini->Get(timestamp_str, "tick_code", "this is bad") == "this is bad") {
		printf("Failed to get section %s!\n", timestamp_str);
		return 0;
	}

	// read config
	delta_codes = ini->GetInteger(timestamp_str, "delta_codes", 1);
	num_rand = ini->GetInteger(timestamp_str, "num_rand", 1);
	xinput_get_state = resolve_ptr_path(ini->Get(timestamp_str, "xinput_get_state", "this is bad").c_str());
	xinput_get_state_post = resolve_ptr_path(ini->Get(timestamp_str, "xinput_get_state_post", "this is bad").c_str());
	tick_code = resolve_ptr_path(ini->Get(timestamp_str, "tick_code", "this is bad").c_str());
	delta_data = resolve_ptr_path(ini->Get(timestamp_str, "delta_data", "this is bad").c_str());
	speedrun_timer = resolve_ptr_path(ini->Get(timestamp_str, "speedrun_timer", "this is bad").c_str());

	char delta_update_str[14] = "delta_updatex";
	for(int i = 0; i < delta_codes; i++) {
		delta_update_str[sizeof(delta_update_str) - 2] = (char)(i + 0x31); // add number to end
		delta_update[i] = resolve_ptr_path(ini->Get(timestamp_str, delta_update_str, "this is bad").c_str());
	}

	char rand_str[6] = "randx";
	for(int i = 0; i < num_rand; i++) {
		rand_str[sizeof(rand_str) - 2] = (char)(i + 0x31); // add number to end
		rand_code[i] = resolve_ptr_path(ini->Get(timestamp_str, rand_str, "this is bad").c_str());
	}

	// rand replacement
	//

	// allocate new memory
	new_rand = VirtualAllocEx(process, (void*)(((unsigned long long)rand_code[0] + 0x10000000) & 0xFFFFFFFFFFFF0000), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if(new_rand == NULL) new_rand = (void*)(((unsigned long long)rand_code[0] + 0x10000000) & 0xFFFFFFFFFFFF0000); // assume we already allocated some here in a previous run
	WriteProcessMemory(process, new_rand, newrand_buf, sizeof(newrand_buf), NULL);
	_tprintf(L"New rand at %p!\n", new_rand);

	// make rand code writable
	// assumes protection is the same, and that the call instruction is the same length (6)
	for(int i = 0; i < num_rand; i++) {
		VirtualProtectEx(process, rand_code[i], sizeof(orig_rand[i]), PAGE_EXECUTE_READWRITE, &old_rand_prot);

		// save old instruction
		ReadProcessMemory(process, rand_code[i], orig_rand[i], sizeof(orig_rand[i]), NULL);
	}

	return 1;
}

void cleanup(bool detach) {
	// game memory
	WriteProcessMemory(process, tick_code, &orig_tick, sizeof(orig_tick), NULL);
	WriteProcessMemory(process, xinput_get_state, &orig_xinput, sizeof(orig_xinput), NULL);
	WriteProcessMemory(process, xinput_get_state_post, &orig_post_xinput, sizeof(orig_post_xinput), NULL);
	WriteProcessMemory(process, delta_data, &original_delta, sizeof(original_delta), NULL); // put back a fixed delta matching the original fps setting
	if(fps_ptr != NULL) WriteProcessMemory(process, fps_ptr, &original_fps, sizeof(original_fps), NULL); // write back original fps value

	// rand jumps
	for(int i = 0; i < num_rand; i++) {
		WriteProcessMemory(process, rand_code[i], orig_rand[i], sizeof(orig_rand[i]), NULL);
		VirtualProtectEx(process, rand_code[i], sizeof(orig_rand[i]), old_rand_prot, &old_rand_prot);
	}
	
	if(detach) {
		// rand code
		VirtualFreeEx(process, new_rand, 0x1000, MEM_RELEASE);

		// debugger stuff
		ResumeThread(thread);
		DebugActiveProcessStop(pid);
		CloseHandle(thread);
		CloseHandle(process);
	}
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
		cleanup(true);

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
	int rshift_down_count = 0;

	float old_realtime_seconds = NAN;
	float realtime_seconds = NAN;

	BYTE* xinput_state_address = NULL;

	CONTEXT context;
	context.ContextFlags = CONTEXT_FULL;

	DEBUG_EVENT debug_event;

	// main loop
	while(true) {
		WriteProcessMemory(process, tick_code, &interrupt_op, sizeof(interrupt_op), NULL);
		WaitForDebugEvent(&debug_event, INFINITE);

		SuspendThread(thread);

		// handle breakpoints, else tell the game to handle it by itself
		// main tick breakpoint -- code continues under these if/else ifs
		if(debug_event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT && debug_event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && debug_event.u.Exception.ExceptionRecord.ExceptionAddress == tick_code) {
			GetThreadContext(thread, &context);
			context.Rip -= 1;
			SetThreadContext(thread, &context);
		}
		// about to call XInputGetState -- save function arguments
		else if(started && debug_event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT && debug_event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && debug_event.u.Exception.ExceptionRecord.ExceptionAddress == xinput_get_state) {
			GetThreadContext(thread, &context);
			context.Rip -= 1;
			xinput_player = context.Rcx;
			xinput_state_address = (BYTE*)context.Rdx;
			SetThreadContext(thread, &context);

			WriteProcessMemory(process, xinput_get_state_post, &interrupt_op, sizeof(interrupt_op), NULL);
			WriteProcessMemory(process, xinput_get_state, &orig_xinput, sizeof(orig_xinput), NULL);
			ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_CONTINUE);
			ResumeThread(thread);
			continue;
		}
		// finished calling XInputGetState -- overwrite output with correct player input as specified in the TAS
		else if(started && debug_event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT && debug_event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && debug_event.u.Exception.ExceptionRecord.ExceptionAddress == xinput_get_state_post) {
			GetThreadContext(thread, &context);
			context.Rip -= 1;
			SetThreadContext(thread, &context);

			// write inputs
			WriteProcessMemory(process, xinput_state_address + 4, &reports[i].gamepads[xinput_player], sizeof(state_report), NULL);

			WriteProcessMemory(process, xinput_get_state, &interrupt_op, sizeof(interrupt_op), NULL);
			WriteProcessMemory(process, xinput_get_state_post, &orig_post_xinput, sizeof(orig_post_xinput), NULL);
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

		// timer
		old_timer_paused = timer_paused; // like in ASL
		ReadProcessMemory(process, (BYTE*)speedrun_timer + 0x10, &timer_paused, sizeof(timer_paused), NULL); // read timer status
		old_game_time = game_time; // like in ASL
		ReadProcessMemory(process, (BYTE*)speedrun_timer + 0x34, &game_time, sizeof(game_time), NULL); // read game time
		old_act_time = act_time; // like in ASL
		ReadProcessMemory(process, (BYTE*)speedrun_timer + 0x3C, &act_time, sizeof(act_time), NULL); // read act time
		old_realtime_seconds = realtime_seconds; // like in ASL
		ReadProcessMemory(process, resolve_ptr_path(ini->Get(timestamp_str, "realtime_seconds", "this is bad").c_str()), &realtime_seconds, sizeof(realtime_seconds), NULL);

		// keys
		old_rshift_down = rshift_down; // like in ASL
		rshift_down = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) == 0x8000; // framestep
		old_lctrl_down = lctrl_down; // like in ASL
		lctrl_down = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) == 0x8000; // exit framestep

		// start if:
		// fullgame type, and realtimeseconds just went less than before (meaning it reset), 
		// IL type, and act timer just started from zero,
		// immediately if type is immediate

		// !started in the beginning should make it not evaluate all the conditions once TAS has started (also means we don't have to be as strict about the conditions)
		if(!started && ((meta.type == FULLGAME && (realtime_seconds < old_realtime_seconds)) || (meta.type == INDIVIDUAL && act_time > 0 && old_act_time == 0) || (meta.type == IMMEDIATE))) {
			// write the input breakpoint on starting
			WriteProcessMemory(process, xinput_get_state, &interrupt_op, sizeof(interrupt_op), NULL); // code
			WriteProcessMemory(process, xinput_get_state_post, &interrupt_op, sizeof(interrupt_op), NULL); // code

			printf("Beginning, parsing %s... ", movie_filename);
			reports = parse_tas(movie_filename, &meta);
			if(reports == NULL) {
				printf("failed...\n");
				break;
			}
			else {
				printf("done!\n");
			}

			desired_framerate = 1.f / meta.fps;

			if(meta.type == IMMEDIATE) {
				_tprintf(L"Starting in two seconds...\n");
				Sleep(2000);
			}

			// patch delta
			for(int i = 0; i < delta_codes; i++) {
				WriteProcessMemory(process, delta_update[i], nop_delta_buf, sizeof(nop_delta_buf), NULL); // code
			}
			WriteProcessMemory(process, delta_data, &desired_framerate, sizeof(desired_framerate), NULL); // data

			started_time = game_time - desired_framerate; // for giving a duration at the end message, subtract a frame, as start time gets set a frame into the replay

			// resolve and read current FPS value, so we can restore it at end-of-TAS
			fps_address = (BYTE*)resolve_ptr_path(ini->Get(timestamp_str, "fps", "this is bad").c_str());
			ReadProcessMemory(process, fps_address, &original_fps, sizeof(original_fps), NULL);
			original_delta = 1.0 / original_fps;

			// write the FPS value that'll be used for the TAS
			WriteProcessMemory(process, fps_address, &meta.fps, sizeof(meta.fps), NULL);

			// write rng
			// sequence length
			WriteProcessMemory(process, (BYTE*)new_rand + 0x1F8, &reports[0].aux.rand_seq_max, sizeof(reports[0].aux.rand_seq_max), NULL);
			// sequence index
			WriteProcessMemory(process, (BYTE*)new_rand + 0x1FC, &zero, sizeof(zero), NULL);
			// sequence
			WriteProcessMemory(process, (BYTE*)new_rand + 0x200, &reports[0].aux.rand_seq, (reports[0].aux.rand_seq_max + 1) * 4, NULL);

			for(int ii = 0; ii < num_rand; ii++) {
				// patch to jump to new rand
				int call_target = (BYTE*)new_rand - rand_code[ii] - 5; // 5 is the length of the call instruction
				printf("call_target is %X\n", call_target);
				WriteProcessMemory(process, rand_code[ii], &call_op, sizeof(call_op), NULL);
				WriteProcessMemory(process, (BYTE*)rand_code[ii] + 1, &call_target, sizeof(call_target), NULL);
				WriteProcessMemory(process, (BYTE*)rand_code[ii] + 5, &nop_op, sizeof(nop_op), NULL); // pad with a nop
			}

			// set start flag
			started = true;
			_tprintf(L"Starting playback!\n");
		}

		// restart loop if it hasn't started yet
		else if(!started) {
			WriteProcessMemory(process, tick_code, &orig_tick, sizeof(orig_tick), NULL);
			ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_CONTINUE);
			ResumeThread(thread);
			continue;
		}

		// reduces desyncs, not sure why this happens though
		if((!old_timer_paused && game_time == old_game_time) || (timer_paused && realtime_seconds == old_realtime_seconds)) {
			WriteProcessMemory(process, tick_code, &orig_tick, sizeof(orig_tick), NULL);
			ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_CONTINUE);
			ResumeThread(thread);
			continue;
		}

		// framestep logic
		if(framestepping || rshift_down) {
			printf("Stepping at frame %d\n", i + 1);
			do {
				Sleep(FRAMESTEP_SLEEP);

				old_rshift_down = rshift_down;
				rshift_down = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) == 0x8000; // framestep
				old_lctrl_down = lctrl_down;
				lctrl_down = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) == 0x8000; // exit framestep

				if(rshift_down) {
					rshift_down_count++;
					framestepping = true;
				}
				else {
					rshift_down_count = 0;
				}

				if(lctrl_down) {
					framestepping = false;
				}
			}
			// every 15 sleeps, after 500ms
			while((!rshift_down || old_rshift_down) && !(rshift_down_count > (500 / FRAMESTEP_SLEEP) && rshift_down_count % 15) && (!lctrl_down));
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

		WriteProcessMemory(process, tick_code, &orig_tick, sizeof(orig_tick), NULL); // code
		ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, DBG_CONTINUE);
		ResumeThread(thread);

		// allow ending the TAS prematurely (0x8000 means key down)
		if(GetKeyState(VK_ESCAPE) & 0x8000) break;

		if(i >= meta.length) break; // break when done
	}
	// end main loop

	// cleanup
	cleanup(false);

	_tprintf(L"Done in %f!\n\n", meta.type == FULLGAME ? realtime_seconds : meta.type == INDIVIDUAL ? act_time : game_time - started_time);
}

int main(int argc, char* argv[]) {
	bool repeat = false;

	for(int i = 1; i < argc; i++) {
		if(argv[i][0] == '-') {
			switch(argv[i][1]) {
				case 'r': repeat = true; break;
				//case 'l': layout_filename = argv[i] + 2; break;
				// above is disabled because it's really laggy
			}
		}
		else movie_filename = argv[i];
	}

	if(movie_filename == NULL) {
		_tprintf(L"A TAS movie filename wasn't specified!\nUsage: hatTAS.exe [-r] <movie.htas>\n\nExiting...\n");
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

	if(reports != NULL) {
		// parse layout just once
		if(layout_filename != NULL) {
			layout = parse_lay(layout_filename);
		}

		// resolve and read current FPS value, so we can restore it at end-of-TAS
		fps_address = (BYTE*)resolve_ptr_path(ini->Get(timestamp_str, "fps", "this is bad").c_str());
		ReadProcessMemory(process, fps_address, &original_fps, sizeof(original_fps), NULL);
		original_delta = 1.0 / original_fps;

		// write the FPS value that'll be used for the TAS
		WriteProcessMemory(process, fps_address, &meta.fps, sizeof(meta.fps), NULL);

		desired_framerate = 1.f / meta.fps;

		// patch delta
		for(int i = 0; i < delta_codes; i++) {
			WriteProcessMemory(process, delta_update[i], nop_delta_buf, sizeof(nop_delta_buf), NULL); // code
		}
		WriteProcessMemory(process, delta_data, &desired_framerate, sizeof(desired_framerate), NULL); // data

		_tprintf(L"Done initializing, ready!\n\n");

		// main main loop
		do {
			play_movie();
		}
		while(repeat);
	}
	else {
		printf("Failed to parse properly, quitting...\n");
	}

	// cleanup
	//

	cleanup(true);

	return 0;
}