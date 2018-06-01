#include "stdafx.h"
#include "parse.h"


// TODO: realloc if more lines/columns than anticipated

bool starts_with(const char* input, const char* search) {
	size_t length = strlen(search);
	for(int i = 0; i < length; i++) {
		if(input[i] != search[i]) return false;
	}

	return true;
}

// parses a tas input file for a hat in time
input_report* parse_tas(const char* filename, tas_metadata* meta_out) {
	FILE* input;
	fopen_s(&input, filename, "r");
	
	// file not found
	if(input == NULL) throw FILE_NOT_FOUND_ERROR;

	// get size
	fseek(input, 0, SEEK_END);
	long file_length = ftell(input);
	rewind(input);

	// read file
	char** file_data = NULL; // char array of each line
	int line_num = 0; // current line
	int column_num = -1; // current column - it's -1 because then we can increment it in the beginning of the loop iteration, so it's always 0 or more
	int character; // current character

	// allocate stuff
	file_data = (char**)calloc(ASSUMED_LINE_COUNT, sizeof(*file_data));
	for(int i = 0; i < ASSUMED_LINE_COUNT; i++) {
		file_data[i] = (char*)calloc(ASSUMED_COLUMN_COUNT, sizeof(*file_data));
	}

	// read whole file into a series of lines
	while((character = fgetc(input)) != EOF) {
		column_num++;

		// 0x0A = \n
		if(character == 0x0A) {
			file_data[line_num][column_num] = (char)0x00; // null terminate
			line_num++; // new line
			column_num = -1; // new line, so set column back
			continue;
		}
		// 0x0D = \r
		else if(character == 0x0D) continue;

		file_data[line_num][column_num] = (char)character;
	}

	int cur_line = 0;
	// loop until the first "frame" line is reached, filling out metadata
	while(file_data[cur_line][0] < 0x30 || file_data[cur_line][0] > 0x39) { // 0x30 -> 0, 0x39 -> 9
		if(starts_with(file_data[cur_line], "name: ")) meta_out->name = file_data[cur_line] + 6; // "name: " is 6 
		else if(starts_with(file_data[cur_line], "type: ")) meta_out->type = (starts_with(file_data[cur_line] + 6, "fullgame") ? FULLGAME : starts_with(file_data[cur_line] + 6, "IL") ? INDIVIDUAL : IMMEDIATE); // "type: " is 6
		else if(starts_with(file_data[cur_line], "length: ")) meta_out->length = strtol(file_data[cur_line] + 8, NULL, 10); // "length: " is 8
		cur_line++;
	}

	// no length set in file
	if(meta_out->length == NULL) throw NO_LEN_SPECIFIED_ERROR;

	input_report* reports;
	input_report tmp_report;

	// allocate
	reports = (input_report*)calloc(meta_out->length, sizeof(*reports));

	// create reports based on frame data
	long cur_frame = 1;
	long old_frame = 1;
	int frame_line = 0;
	char* next_token;

	cur_line -= 1; // not a good way but it works, otherwise it'd skip the first frame line
	// each iteration is a frame line
	while(true) {
		if(cur_line == line_num) break; // break at EOF
		cur_line++;

		// if it's a comment
		if(starts_with(file_data[cur_line], "//")) continue; // notice we're not incrementing frame_line, as it's not a frame
		// not a comment, let's find the frame number
		char* token = strtok_s(file_data[cur_line], ":", &next_token);
		if(token == NULL) continue; // line doesn't start with a number, could be blank
		cur_frame = strtol(token, NULL, 10); // convert frame count string to an integer

		// fill in the gap frames
		for(int i = old_frame; i < cur_frame; i++) {
			reports[i - 1] = tmp_report;
		}

		// parse position data
		while(token != NULL) {
			// get new token (input)
			token = strtok_s(NULL, " ", &next_token);

			// process it
			bool negated = false;
			if(token == NULL || starts_with(token, "//")) break; // comment or null detected, ignore rest of line

			// set negation flag
			if(starts_with(token, "~")) {
				token++; // ignore the tilde
				negated = true;
			}
			// it's being dumb, only starts_with() works...
			// buttons 
			if(starts_with(token, "A")) tmp_report.buttons.a = !negated ? 0x80 : 0;
			else if(starts_with(token, "B")) tmp_report.buttons.b = !negated ? 0x80 : 0;
			else if(starts_with(token, "X")) tmp_report.buttons.x = !negated ? 0x80 : 0;
			else if(starts_with(token, "Y")) tmp_report.buttons.y = !negated ? 0x80 : 0;
			else if(starts_with(token, "LB")) tmp_report.buttons.lb = !negated ? 0x80 : 0;
			else if(starts_with(token, "RB")) tmp_report.buttons.rb = !negated ? 0x80 : 0;
			else if(starts_with(token, "LT")) tmp_report.buttons.lt = !negated ? 0x80 : 0;
			else if(starts_with(token, "RT")) tmp_report.buttons.rt = !negated ? 0x80 : 0;
			else if(starts_with(token, "START")) tmp_report.buttons.start = !negated ? 0x80 : 0;
			else if(starts_with(token, "SELECT")) tmp_report.buttons.select = !negated ? 0x80 : 0;

			// dpad
			else if(starts_with(token, "UP")) tmp_report.pov.up = !negated;
			else if(starts_with(token, "RIGHT")) tmp_report.pov.right = !negated;
			else if(starts_with(token, "DOWN")) tmp_report.pov.down = !negated;
			else if(starts_with(token, "LEFT")) tmp_report.pov.left = !negated;

			// analog -- auto-detects number base (or at least it should)
			else if(starts_with(token, "LX")) tmp_report.analog.lx = !negated ? strtol(token + 3, NULL, NULL) : AXIS_MAX / 2;
			else if(starts_with(token, "LY")) tmp_report.analog.ly = !negated ? strtol(token + 3, NULL, NULL) : AXIS_MAX / 2;
			else if(starts_with(token, "RX")) tmp_report.analog.rx = !negated ? strtol(token + 3, NULL, NULL) : AXIS_MAX / 2;
			else if(starts_with(token, "RY")) tmp_report.analog.ry = !negated ? strtol(token + 3, NULL, NULL) : AXIS_MAX / 2;

			// commands
			else if(starts_with(token, "SPEED")) {
				tmp_report.aux.speed = !negated ? strtof(token + 6, NULL) * 60.f : 60.f;
				meta_out->changes_speed = true; // try to optimize the WriteProcessMemory() calls a little
			}

			// probably really inefficient, doesn't matter too much, as we're not playing while parsing
			long hat;
			if(tmp_report.pov.up) {
				if(tmp_report.pov.right) hat = 4500;
				else if(tmp_report.pov.left) hat = 31500;
				else hat = 0;
			}
			else if(tmp_report.pov.right) {
				if(tmp_report.pov.down) hat = 13500;
				else hat = 9000;
			}
			else if(tmp_report.pov.down) {
				if(tmp_report.pov.left) hat = 22500;
				else hat = 18000;
			}
			else if(tmp_report.pov.left) {
				hat = 27000;
			}
			else hat = -1; // neutral

			tmp_report.analog.hat = hat;
		}
		
		// finish up the frame line
		reports[cur_frame - 1] = tmp_report;
		old_frame = cur_frame;
		frame_line++;
	}

	// fill in the gap frames at the end
	for(int i = cur_frame; i < meta_out->length + 1; i++) {
		reports[i - 1] = tmp_report;
	}

	// so i don't forget
	fclose(input);
	free(file_data);

	return reports;
}