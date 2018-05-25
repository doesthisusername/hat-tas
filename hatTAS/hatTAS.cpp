#include "stdafx.h"
#include <Windows.h>
#include <stdio.h>
#include <tlhelp32.h>
#include "parse.h"

#define HAT_EXE_NAME L"HatinTimeGame.exe"
#define DINPUT_DLL_NAME L"DINPUT8.dll"
// divide by 4 because reasons
#define PIB_GET_INTERFACE_OFFSET (0x89ABF0 / 4)
#define GETDF_DI_JOYSTICK_OFFSET (0xFCB0 / 4)

#define DELTA_WRITE_CODE_OFFSET (0x12CC76 / 4)
#define ANALOG_WRITE_CODE_OFFSET (0x1697A / 4)
#define BUTTON_WRITE_CODE_OFFSET (0x6CE1 / 4)
#define DELTA_DATA_OFFSET (0x75DAF8 / 4)
#define INPUT_DATA_PTR_OFFSET (0x1195850 / 4)
#define TOTAL_FRAMES_OFFSET (0x11802E0 / 4)
#define TIMER_OFFSET (0x101B720 / 4)
#define FPS_PTR_OFFSET (0x116D250 / 4)

// it only wants to write on 4-byte aligned boundaries, so the first two bytes are filler, as the code we like isn't 4-byte aligned
BYTE nop_delta_buf[10] = { 0x79, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
const double desired_framerate = 1.0 / 60.0;
float original_fps = 60.f;

// dinput analog
BYTE nop_analog_buf[6] = { 0x8B, 0xC3, 0x90, 0x90, 0x90, 0x90 };
BYTE orig_analog_buf[6] = { 0x8B, 0xC3, 0x41, 0x89, 0x1C, 0x17 };

// dinput button
BYTE nop_button_buf[5] = { 0x75, 0x90, 0x90, 0x90, 0x90 };
BYTE orig_button_buf[5] = { 0x75, 0x41, 0x88, 0x1C, 0x17 };

tas_metadata meta;
input_report* reports;

HANDLE process;

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
		_tprintf(L"The specified process couldn't be found!\nExiting...\n");
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
		_tprintf(L"Failed to find base address of executable or DINPUT8.dll!\nExiting...\n");
		return 0;
	}

	// patch delta
	WriteProcessMemory(process, base_address + PIB_GET_INTERFACE_OFFSET + DELTA_WRITE_CODE_OFFSET, nop_delta_buf, sizeof(nop_delta_buf), NULL); // code
	WriteProcessMemory(process, base_address + PIB_GET_INTERFACE_OFFSET + DELTA_DATA_OFFSET, &desired_framerate, sizeof(desired_framerate), NULL); // data
	_tprintf(L"Wrote a fixed delta time of %lf!\n\n", desired_framerate);

	Sleep(750); // give user time to give hat focus
	printf("Playing %s!\n", meta.name);

	bool started = false;
	int i = 0;
	int old_frame_count = 0;
	int frame_count = 0;
	double old_act_time = -1;
	double act_time = -1;
	BYTE* analog_address = NULL;
	BYTE* buttons_address = NULL;
	BYTE* fps_address = NULL;

	// TODO: make the loop less intensive
	// main loop
	while(true) {
		old_frame_count = frame_count; // like in ASL
		ReadProcessMemory(process, base_address + TOTAL_FRAMES_OFFSET, &frame_count, 4, NULL); // read frame count
									   
		// if this is a new frame, adjust inputs accordingly
		if(frame_count == old_frame_count + 1) {
			if(i >= meta.length) break;

			old_act_time = act_time;
			ReadProcessMemory(process, base_address + TIMER_OFFSET + (0x3C / 4), &act_time, 8, NULL); // read act time

			// TODO: implement fullgame type
			// start if IL type, and act timer just started, OR start immediately if type is immediate
			if((meta.type == INDIVIDUAL && act_time > 0 && old_act_time == 0) || (meta.type == IMMEDIATE)) {
				// write the code patch on starting
				WriteProcessMemory(process, dinput_address + ANALOG_WRITE_CODE_OFFSET, nop_analog_buf, sizeof(nop_analog_buf), NULL); // code
				WriteProcessMemory(process, dinput_address + GETDF_DI_JOYSTICK_OFFSET + BUTTON_WRITE_CODE_OFFSET, nop_button_buf, sizeof(nop_button_buf), NULL); // code

				// pointer path traversal when starting
				analog_address = resolve_ptr(base_address + INPUT_DATA_PTR_OFFSET);
				analog_address = resolve_ptr(analog_address + 0x760);
				analog_address = resolve_ptr(analog_address + 0xA0);
				analog_address = resolve_ptr(analog_address + 0x8); // analog_address now points to the right place
				buttons_address = analog_address + 0xE4; // buttons

				fps_address = resolve_ptr(base_address + FPS_PTR_OFFSET) + 0x710;
				ReadProcessMemory(process, fps_address, &original_fps, sizeof(float), NULL); // so we can restore it at end-of-TAS
				
				// set start flag
				started = true;
			}

			// restart loop if it hasn't started yet
			if(!started) continue;
			
			// write inputs
			WriteProcessMemory(process, analog_address, &reports[i].analog, sizeof(analog_report), NULL);
			WriteProcessMemory(process, buttons_address, &reports[i].buttons, sizeof(buttons_report), NULL);
			WriteProcessMemory(process, buttons_address + 0x20, &reports[i].analog.hat, sizeof(long), NULL); // double-write hat inputs, not sure if needed
			// write commands
			WriteProcessMemory(process, fps_address, &reports[i].aux.speed, sizeof(float), NULL);

			// increment local frame count
			i++;
		}
	}
	
	// cleanup
	WriteProcessMemory(process, dinput_address + ANALOG_WRITE_CODE_OFFSET, orig_analog_buf, sizeof(orig_analog_buf), NULL); // put original back so dinput devices work again
	WriteProcessMemory(process, dinput_address + GETDF_DI_JOYSTICK_OFFSET + BUTTON_WRITE_CODE_OFFSET, orig_button_buf, sizeof(orig_button_buf), NULL); // ^
	WriteProcessMemory(process, fps_address, &original_fps, sizeof(original_fps), NULL); // write back original fps value
	CloseHandle(process);
	_tprintf(L"Done at %lf!\n", act_time);

	return 0;
}