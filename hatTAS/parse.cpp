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

// remember to free!
char** read_lines(const char* filename, int* out_lines) {
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

	*out_lines = line_num;
	fclose(input);
	return file_data;
}

// parses a tas input file for a hat in time
input_report* parse_tas(const char* filename, tas_metadata* meta_out) {
	int line_num;
	char** lines = read_lines(filename, &line_num);

	int cur_line = 0;
	// loop until the first "frame" line is reached, filling out metadata
	while(lines[cur_line][0] < 0x30 || lines[cur_line][0] > 0x39) { // 0x30 -> 0, 0x39 -> 9
		if(starts_with(lines[cur_line], "name: ")) meta_out->name = lines[cur_line] + 6; // "name: " is 6 
		else if(starts_with(lines[cur_line], "type: ")) meta_out->type = (starts_with(lines[cur_line] + 6, "fullgame") ? FULLGAME : starts_with(lines[cur_line] + 6, "IL") ? INDIVIDUAL : IMMEDIATE); // "type: " is 6
		else if(starts_with(lines[cur_line], "players: ")) meta_out->player_count = strtol(lines[cur_line] + 9, NULL, 10); // "players: " is 9
		else if(starts_with(lines[cur_line], "length: ")) meta_out->length = strtol(lines[cur_line] + 8, NULL, 10); // "length: " is 8
		else if(starts_with(lines[cur_line], "fps: ")) meta_out->fps = strtof(lines[cur_line] + 5, NULL); // "fps: " is 5

		cur_line++;
	}

	// no length set in file
	if(meta_out->length == NULL) throw NO_LEN_SPECIFIED_ERROR;
	// too many players specified
	if(meta_out->player_count > ALLOWED_PLAYERS) throw TOO_MANY_PLAYERS_ERROR;

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
		if(starts_with(lines[cur_line], "//")) continue; // notice we're not incrementing frame_line, as it's not a frame
		// not a comment, let's find the frame number
		char* token = strtok_s(lines[cur_line], ":", &next_token);
		if(token == NULL) continue; // line doesn't start with a number, could be blank
		cur_frame = strtol(token, NULL, 10); // convert frame count string to an integer

		// too long, issue error
		if(cur_frame > meta_out->length) {
			printf("Error: frame %d is bigger than the length %d!\n", cur_frame, meta_out->length);
			return NULL;
		}

		// fill in the gap frames
		for(int i = old_frame; i < cur_frame; i++) {
			reports[i - 1] = tmp_report;
		}

		long player_id = 0;

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
			// it's being dumb, only starts_with() works... also sorry for the mess
			// | means go to next player (cycles back automatically)
			if(starts_with(token, "|")) player_id = player_id == meta_out->player_count - 1 ? 0 : player_id + 1;

			// buttons 
			else if(starts_with(token, "A")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_A;
			else if(starts_with(token, "B")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_B;
			else if(starts_with(token, "X")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_X;
			else if(starts_with(token, "Y")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_Y;
			else if(starts_with(token, "LB")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_LB;
			else if(starts_with(token, "RB")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_RB;
			else if(starts_with(token, "LT")) tmp_report.gamepads[player_id].left_trigger = !negated ? 0xFF : 0;
			else if(starts_with(token, "RT")) tmp_report.gamepads[player_id].right_trigger = !negated ? 0xFF : 0;
			else if(starts_with(token, "START")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_START;
			else if(starts_with(token, "BACK")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_BACK;
			else if(starts_with(token, "L3")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_LEFT_THUMB;
			else if(starts_with(token, "R3")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_RIGHT_THUMB;

			// dpad
			else if(starts_with(token, "UP")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_DPAD_UP;
			else if(starts_with(token, "RIGHT")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_DPAD_RIGHT;
			else if(starts_with(token, "DOWN")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_DPAD_DOWN;
			else if(starts_with(token, "LEFT")) tmp_report.gamepads[player_id].button_state ^= (-!negated ^ tmp_report.gamepads[player_id].button_state) & BTN_DPAD_LEFT;

			// analog -- auto-detects number base (or at least it should)
			else if(starts_with(token, "LX")) tmp_report.gamepads[player_id].lx = !negated ? strtol(token + 3, NULL, NULL) : 0;
			else if(starts_with(token, "LY")) tmp_report.gamepads[player_id].ly = !negated ? strtol(token + 3, NULL, NULL) : 0;
			else if(starts_with(token, "RX")) tmp_report.gamepads[player_id].rx = !negated ? strtol(token + 3, NULL, NULL) : 0;
			else if(starts_with(token, "RY")) tmp_report.gamepads[player_id].ry = !negated ? strtol(token + 3, NULL, NULL) : 0;

			// commands
			else if(starts_with(token, "SPEED")) {
				tmp_report.aux.speed = !negated ? strtof(token + 6, NULL) * meta_out->fps : meta_out->fps;
				meta_out->changes_speed = true; // try to optimize the WriteProcessMemory() calls a little
			}
			// comma-separated list of rand values to cyclically return
			else if(starts_with(token, "RNG")) {
				if(!negated) {
					int n = 0;

					char* sub_char = token + 4; // "RNG:" is 4 characters
					char* sub_token = sub_char;

					// comma-separate
					while(*sub_char != '\0') {
						if(*sub_char == ',' || *(sub_char + 1) == '\0') {
							if(*sub_char == ',') *sub_char = '\0';

							tmp_report.aux.rand_seq[n++] = atoi(sub_token) << 0x10;
							if(atoi(sub_token) > 32767) {
								printf("Warning: random value of %s is too high (over 32767); this will result in behaviour not normally in the game\n", sub_token);
							}

							sub_token = sub_char + 1;
						}

						sub_char++;
					}

					tmp_report.aux.rand_seq_max = n - 1;
				}
				else {
					tmp_report.aux.rand_seq_max = 0;
					tmp_report.aux.rand_seq[0] = 0;
				}
			}
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

	free(lines);

	return reports;
}

// NOTE: if we ever get multiple layouts/layout hotswap, we'll need to free a bunch of stuff allocated here
layout_def* parse_lay(const char* filename) {
	int line_num;
	char** lines = read_lines(filename, &line_num);

	layout_def* layout = (layout_def*)malloc(sizeof(layout_def));
	layout->item_n = atoi(lines[0]);

	layout->items = (layout_itm**)malloc(layout->item_n * sizeof(layout_itm*));
	layout->formats = (char**)malloc(layout->item_n * sizeof(char*));

	for(int i = 0; i < layout->item_n; i++) {
		layout_itm* item = (layout_itm*)malloc(sizeof(layout_itm));
		item->type = (layout_type)lines[i + 1][0];
		item->op = (layout_op)lines[i + 1][1];
		item->ppath_n = 0;
		item->ppaths[0].offset_n = 0;

		layout->items[i] = item; // this still gets filled in the loop below
		layout->formats[i] = lines[i + layout->item_n + 1];

		const char* ppath_lit = lines[i + 1] + 2; // skips type and op
		for(int ii = 0; ppath_lit[ii] != 0x00; ii++) {
			if(ppath_lit[ii] == ',') {
				item->ppath_n++;
				item->ppaths[item->ppath_n].offset_n = 0;
			}
			else {
				item->ppaths[item->ppath_n].offsets[item->ppaths[item->ppath_n].offset_n++] = strtoull(ppath_lit + ii, NULL, 16);
				do {
					ii++;
				}
				while(ppath_lit[ii] != '+' && ppath_lit[ii] != 0x00);
			}
		}
	}

	free(lines);
	return layout;
}