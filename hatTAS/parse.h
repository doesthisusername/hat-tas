#pragma once

#include <Windows.h>
#include <stdio.h>
#include <string>

#define AXIS_MIN -32768
#define AXIS_MAX 32767

#define BTN_DPAD_UP 0x0001
#define BTN_DPAD_DOWN 0x0002
#define BTN_DPAD_LEFT 0x0004
#define BTN_DPAD_RIGHT 0x0008
#define BTN_START 0x0010
#define BTN_BACK 0x0020
#define BTN_LEFT_THUMB 0x0040
#define BTN_RIGHT_THUMB 0x0080
#define BTN_LB 0x0100
#define BTN_RB 0x0200
#define BTN_A 0x1000
#define BTN_B 0x2000
#define BTN_X 0x4000
#define BTN_Y 0x8000

#define ALLOWED_PLAYERS 2
#define ASSUMED_LINE_COUNT 0x1000
#define ASSUMED_COLUMN_COUNT 0x80

enum parse_error {
	FILE_NOT_FOUND_ERROR = -1,
	NO_LEN_SPECIFIED_ERROR = -2,
	TOO_MANY_PLAYERS_ERROR = -3
};

enum tas_type {
	FULLGAME,
	INDIVIDUAL,
	IMMEDIATE
};

struct tas_metadata {
	char* name = "Generic TAS";
	tas_type type = INDIVIDUAL;
	long length;
	long player_count = 1;
	float fps = 60.f;
	bool changes_speed = false;
};

// mimicking how the game does it, so we can do fewer writes
struct state_report {
	WORD button_state = 0; // bitmask of buttons
	BYTE left_trigger = 0;
	BYTE right_trigger = 0;
	SHORT lx = 0;
	SHORT ly = 0;
	SHORT rx = 0;
	SHORT ry = 0;
};

// extra data not related to inputs
struct aux_data {
	float speed = 60.f;
	int rand_seq_max = 0;
	unsigned int rand_seq[0x10] = { 0 };
};

struct input_report {
	state_report gamepads[ALLOWED_PLAYERS];
	aux_data aux;
};

bool starts_with(const char* input, const char* search);
input_report* parse_tas(const char* filename, tas_metadata* meta_out);