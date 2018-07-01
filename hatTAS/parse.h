#pragma once

#include <Windows.h>
#include <stdio.h>
#include <string>

#define AXIS_MIN 0
#define AXIS_MAX 65535

#define BTN_UP 0
#define BTN_DOWN 1

#define ASSUMED_LINE_COUNT 0x1000
#define ASSUMED_COLUMN_COUNT 0x80

enum parse_error {
	FILE_NOT_FOUND_ERROR = -1,
	NO_LEN_SPECIFIED_ERROR = -2
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
	float fps = 60.f;
	bool changes_speed = false;
};

// mimicking how the game does it, so we can do fewer writes
// binary inputs
struct buttons_report {
	BYTE x = 0;
	BYTE a = 0;
	BYTE b = 0;
	BYTE y = 0;
	BYTE lb = 0;
	BYTE rb = 0;
	BYTE lt = 0;
	BYTE rt = 0;
	BYTE select = 0;
	BYTE start = 0;
};

// analog inputs (analog sticks as well as dpad)
struct analog_report {
	long ry = AXIS_MAX / 2;
	long rx = AXIS_MAX / 2;
	long ly = AXIS_MAX / 2;
	long lx = AXIS_MAX / 2;
	long hat = -1;
};

// game doesn't do this, I just do it because I can't figure out how to calculate correct pov value otherwise
struct pov_help {
	bool up = false;
	bool right = false;
	bool down = false;
	bool left = false;
};

// extra data not related to inputs
struct aux_data {
	float speed = 60.f;
};

struct input_report {
	buttons_report buttons;
	analog_report analog;
	pov_help pov;
	aux_data aux;
};

bool starts_with(const char* input, const char* search);
input_report* parse_tas(const char* filename, tas_metadata* meta_out);