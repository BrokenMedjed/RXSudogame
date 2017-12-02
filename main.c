#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <locale.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ncurses.h>
#include <pthread.h>
#include <sys/stat.h>

#define SUDOSIGN "RXDSF"

#define CP_WRONG 1

typedef struct {
	uint8_t *s_data;
	uint8_t *s_solution;
	uint8_t *s_protmap;
	uint8_t  s_solvestatus;
	uint32_t s_time;
	// the next one is not in the sudomap but generated by sudoload
	uint32_t s_offset;
} sudogame;

typedef struct {
	char     *s_sign;
	uint16_t  s_count;
	sudogame *s_games;
} sudomap;

typedef struct {
	sudogame *s_game;
	uint16_t  s_index;
	uint8_t   s_xpos;
	uint8_t   s_ypos;
} sudotracker;

typedef struct {
	uint8_t t_hours;
	uint8_t t_mins;
	uint8_t t_secs;
} timer;

char *BOX_ROW_TOP  = "\u2554\u2550\u2564\u2550\u2564\u2550\u2566\u2550\u2564\u2550\u2564\u2550\u2566\u2550\u2564\u2550\u2564\u2550\u2557";
char *BOX_ROW_BOT  = "\u255A\u2550\u2567\u2550\u2567\u2550\u2569\u2550\u2567\u2550\u2567\u2550\u2569\u2550\u2567\u2550\u2567\u2550\u255D";
char *BOX_ROW_MID1 = "\u2551 \u2502 \u2502 \u2551 \u2502 \u2502 \u2551 \u2502 \u2502 \u2551";
char *BOX_ROW_MID2 = "\u255F\u2500\u253C\u2500\u253C\u2500\u256B\u2500\u253C\u2500\u253C\u2500\u256B\u2500\u253C\u2500\u253C\u2500\u2562";
char *BOX_ROW_MID3 = "\u2560\u2550\u256A\u2550\u256A\u2550\u256C\u2550\u256A\u2550\u256A\u2550\u256C\u2550\u256A\u2550\u256A\u2550\u2563";

uint16_t  TERM_MAX_X;
uint16_t  TERM_MAX_Y;

uint16_t  BOX_CEN_X;
uint16_t  BOX_CEN_Y;

uint16_t  BOX_SCALE   = 19;

uint16_t  TIMER_SCALE = 8;

uint16_t  GRID_SCALE  = 17;

uint16_t  TIMER_CEN_X;
uint16_t  TIMER_CEN_Y;

pthread_mutex_t     TIMER_MUTEX;
pthread_mutexattr_t TIMER_MUTEXATTR;

pthread_mutex_t     CHECK_MUTEX;
pthread_mutexattr_t CHECK_MUTEXATTR;

pthread_t TIMER_TID;
pthread_t CHECK_TID;

uint16_t  GRID_CEN_X;
uint16_t  GRID_CEN_Y;

uint16_t __gxpos2abs(uint8_t x) {
	return GRID_CEN_X + (2 * x);
}

uint16_t __gypos2abs(uint8_t y) {
	return GRID_CEN_Y + (2 * y);
}

uint8_t __sudogridputch(uint8_t x, uint8_t y, char chr) {
	if (x > 9 || y > 9) return 1;
	mvprintw(__gypos2abs(y), __gxpos2abs(x), "%c", chr);
	refresh();
	return 0;
}

uint8_t __gridxy2idx(uint8_t x, uint8_t y) {
	if (x > 9 || y > 9) return 0;
	return y * 9 + x;
}

void __gridmove(uint8_t x, uint8_t y) {
	move(__gypos2abs(y), __gxpos2abs(x));
	refresh();
}

void *thr_timer(void *arg) {
	sudotracker *sudo = (sudotracker *) arg;
	timer timeseg = { 0, 0, 0 };
	int ret = pthread_mutex_trylock(&TIMER_MUTEX);
	if (ret == EOWNERDEAD) {
		pthread_mutex_consistent(&TIMER_MUTEX);
	} else if (!ret || ret == EBUSY) {
		pthread_mutex_unlock(&TIMER_MUTEX);
	} else pthread_exit(0);
	for (uint32_t blah = 0; blah < sudo->s_game->s_time; blah++) {
		timeseg.t_secs++;
		if (timeseg.t_secs >= 60) {
			timeseg.t_mins++;
			timeseg.t_secs = 0;
		}
		if (timeseg.t_mins >= 60) {
			timeseg.t_hours++;
			timeseg.t_mins = 0;
		}

	}
	mvprintw(TIMER_CEN_Y, TIMER_CEN_X, "%02hhu:%02hhu:%02hhu", timeseg.t_hours, timeseg.t_mins, timeseg.t_secs);
	__gridmove(sudo->s_xpos, sudo->s_ypos);
	while (1) {
		if (sudo->s_game->s_solvestatus) {
			sleep(1); // breakpoint for pthread_cancel
			continue;
		}
		sudo->s_game->s_time++;
		timeseg.t_secs++;
		if (timeseg.t_secs >= 60) {
			timeseg.t_mins++;
			timeseg.t_secs = 0;
		}
		if (timeseg.t_mins >= 60) {
			timeseg.t_hours++;
			timeseg.t_mins = 0;
		}
		pthread_mutex_lock(&TIMER_MUTEX);
		mvprintw(TIMER_CEN_Y, TIMER_CEN_X, "%02hhu:%02hhu:%02hhu", timeseg.t_hours, timeseg.t_mins, timeseg.t_secs);
		__gridmove(sudo->s_xpos, sudo->s_ypos);
		pthread_mutex_unlock(&TIMER_MUTEX);
		sleep(1);
	}
	pthread_exit(0);
}

void *thr_check(void *arg) {
	sudotracker *sudo = (sudotracker *) arg;
	uint8_t *hashmap = malloc(81 * sizeof(uint8_t));
	if (!hashmap) pthread_exit(0);
	int ret = pthread_mutex_trylock(&CHECK_MUTEX);
	if (ret == EOWNERDEAD) {
		pthread_mutex_consistent(&CHECK_MUTEX);
	} else if (!ret || ret == EBUSY) {
		pthread_mutex_unlock(&CHECK_MUTEX);
	} else pthread_exit(0);
	uint8_t  pos_x   = 0;
	uint8_t  pos_y   = 0;
	for (pos_y = 0; pos_y < 9; pos_y++) {
		for (pos_x = 0; pos_x < 9; pos_x++) {
			pthread_mutex_lock(&CHECK_MUTEX);
			__gridmove(pos_x, pos_y);
			if (sudo->s_game->s_data[__gridxy2idx(pos_x, pos_y)] != sudo->s_game->s_solution[__gridxy2idx(pos_x, pos_y)]) {
				hashmap[__gridxy2idx(pos_x, pos_y)] = 1;
				chgat(1, A_NORMAL, CP_WRONG, NULL);
				refresh();
			}
			pthread_mutex_unlock(&CHECK_MUTEX);
		}
	}
	__gridmove(sudo->s_xpos, sudo->s_ypos);
	sleep(2);
	for (pos_y = 0; pos_y < 9; pos_y++) {
		for (pos_x = 0; pos_x < 9; pos_x++) {
			pthread_mutex_lock(&CHECK_MUTEX);
			__gridmove(pos_x, pos_y);
			if (hashmap[__gridxy2idx(pos_x, pos_y)]) {
				chgat(1, A_NORMAL, 0, NULL);
				refresh();
			}
			pthread_mutex_unlock(&CHECK_MUTEX);
		}
	}
	pthread_mutex_lock(&CHECK_MUTEX);
	__gridmove(sudo->s_xpos, sudo->s_ypos);
	pthread_mutex_unlock(&CHECK_MUTEX);
	memset(hashmap, 0, 81 * sizeof(uint8_t));
	free(hashmap);
	pthread_exit(0);
}

void __sudogamefree(sudogame *game) {
	if (game->s_data) {
		memset(game->s_data, 0, 81 * sizeof(uint8_t));
		free(game->s_data);
	}
	if (game->s_solution) {
		memset(game->s_solution, 0, 81 * sizeof(uint8_t));
		free(game->s_solution);
	}
	if (game->s_protmap) {
		memset(game->s_protmap, 0, 81 * sizeof(uint8_t));
		free(game->s_protmap);
	}
	game->s_time        = 0;
	game->s_solvestatus = 0;
	game->s_offset      = 0;
}

void __sudomapfree(sudomap *map) {
	for (uint16_t ctr = 0; ctr < map->s_count; ctr++) {
		__sudogamefree(&(map->s_games[ctr]));
	}
	memset(map->s_sign, 0, 6 * sizeof(char));
	free(map->s_sign);
	free(map->s_games);
	map->s_count = 0;
}

uint32_t __sudogamesize(sudogame *game) {
	uint32_t ret = 0;
	ret += sizeof(game->s_data);
	ret += sizeof(game->s_solution);
	ret += sizeof(game->s_protmap);
	ret += sizeof(game->s_solvestatus);
	ret += sizeof(game->s_time);
	ret += sizeof(game->s_offset);
	return ret;
}

uint32_t __sudomapsize(sudomap *map) {
	uint32_t ret = 0;
	ret += sizeof(map->s_count);
	ret += sizeof(map->s_sign);
	for (uint16_t ctr = 0; ctr < map->s_count; ctr++) {
		ret += __sudogamesize(&(map->s_games[ctr]));
	}
	return ret;
}

uint8_t sudogen(char *outname, uint16_t many) {
	FILE *out = fopen(outname, "w");
	if (!out) return 1; // output file open error "fopen(out, write)"
	char *cmd = malloc(44 * sizeof(char));
	if (!cmd) {
		fclose(out);
		return 2; // command malloc error "malloc(qqwing)"
	}
	memset(cmd, 0, 44 * sizeof(char));
	snprintf(cmd, 44 * sizeof(char), "%s %hu", "qqwing --compact --solution --generate", many);
	FILE *qqpipe = popen(cmd, "r");
	free(cmd);
	if (!qqpipe) {
		fclose(out);
		if (!errno) return 3; // internal popen malloc error "malloc(popen(qqwing))"
		else return 4; // popen general error "popen(qqwing)"
	}
	char *line = malloc(11 * sizeof(char));
	if (!line) {
		pclose(qqpipe);
		fclose(out);
		return 5; // malloc error for tmp buffer "malloc(buf)"
	}
	memset(line, 0, 11 * sizeof(char));
	uint8_t *protmap = malloc(81 * sizeof(uint8_t));
	if (!protmap) {
		pclose(qqpipe);
		fclose(out);
		return 6; // malloc error for tmp protectmap "malloc(protmap)"
	}
	memset(protmap, 0, 81 * sizeof(uint8_t));
	char    *sign = SUDOSIGN; // signature
	uint8_t  sep  = 0xFF;     // entry separator
	uint8_t  msep = 0xFE;     // sudoku separator
	uint8_t  dsep = 0xFD;     // data section separator
	uint8_t  psep = 0xFC;     // protectmap separator
	uint8_t  zero = 0;        // generic 0 and solve status
	uint32_t time = 0;        // elapsed time
	fwrite((void *) sign,  sizeof(char),     6, out);
	fwrite((void *) &many, sizeof(uint16_t), 1, out);
	fwrite((void *) &sep,  sizeof(uint8_t),  1, out);
	uint8_t pm_idx = 0;
	uint8_t usemsep = 1;
	fseek(out, 0, SEEK_CUR);
	while (fgets(line, 11 * sizeof(char), qqpipe)) {
		if (line[0] != '\n') {
			for (uint8_t ctr = 0; ctr < 9; ctr++) {
				if (line[ctr] != '.') {
					uint8_t num = line[ctr] - 48;
					fwrite((void *) &num, sizeof(uint8_t), 1, out);
					protmap[pm_idx] = 1;
				} else {
					fwrite((void *) &zero, sizeof(uint8_t), 1, out);
				}
				pm_idx++;
			}
		} else {
			pm_idx = 0;
			if (usemsep) {
				fseek(out, 0, SEEK_CUR);
				fwrite((void *) &psep,   sizeof(uint8_t),  1, out);
				fwrite((void *) protmap, sizeof(uint8_t), 81, out);
				fwrite((void *) &msep,   sizeof(uint8_t),  1, out);
				usemsep = 0;
			} else {
				fseek(out, 0, SEEK_CUR);
				fwrite((void *) &dsep,  sizeof(uint8_t),  1, out);
				fwrite((void *) &time,  sizeof(uint32_t), 1, out);
				fwrite((void *) &zero,  sizeof(uint8_t),  1, out);
				fwrite((void *) &sep,   sizeof(uint8_t),  1, out);
				memset(protmap, 0, 81 * sizeof(uint8_t));
				usemsep = 1;
			}
			memset(protmap, 0, 81 * sizeof(uint8_t));
		}
		memset(line, 0, 11 * sizeof(char));
	}
	pclose(qqpipe);
	free(line);
	free(protmap);
	return 0;
}

uint8_t sudoload(char *inname, sudomap *ret) {
	FILE *in = fopen(inname, "r");
	if (!in) return 1; // input file fopen error "fopen(in, read)"
	struct stat instat;
	uint8_t statret = fstat(fileno(in), &instat);
	if (statret < 0) {
		fclose(in);
		return 2; // input file fstat error "fstat(in)"
	}
	if (instat.st_size < ((6 * sizeof(char)) + sizeof(uint16_t) + sizeof(uint8_t))) {
		fclose(in);
		return 3; // input file size error "fstat(in) filesize"
	}
	ret->s_sign = malloc(6 * sizeof(char));
	if (!ret->s_sign) {
		fclose(in);
		return 4; // signature str malloc error "malloc(signature)"
	}
	memset(ret->s_sign, 0, 6 * sizeof(char));
	fread(ret->s_sign, sizeof(char), 6, in);
	if (strncmp(ret->s_sign, SUDOSIGN, 5) != 0) {
		fclose(in);
		free(ret->s_sign);
		return 5; // input file format verification failed "signature"
	}
	fseek(in, 0, SEEK_CUR); // if you ever see me doing this, I'm just resetting the pos of the stream
	fread((void *) &ret->s_count, sizeof(uint16_t), 1, in);
	fseek(in, 1, SEEK_CUR); // skip entry separator
	ret->s_games = malloc(ret->s_count * sizeof(sudogame));
	if (!ret->s_games) {
		fclose(in);
		free(ret->s_sign);
		return 6; // game stack malloc error "malloc(gamestack)"
	}
	for (uint16_t gameidx = 0; gameidx < ret->s_count; gameidx++) {
		uint8_t emalloc = 0;
		ret->s_games[gameidx].s_data     = malloc(81 * sizeof(uint8_t));
		ret->s_games[gameidx].s_solution = malloc(81 * sizeof(uint8_t));
		ret->s_games[gameidx].s_protmap  = malloc(81 * sizeof(uint8_t));
		if (!ret->s_games[gameidx].s_data)     emalloc = 1; else
		if (!ret->s_games[gameidx].s_solution) emalloc = 2; else
		if (!ret->s_games[gameidx].s_protmap)  emalloc = 3;
		if (emalloc) {
			fclose(in);
			__sudomapfree(ret);
			return 6 + emalloc; // 7: game data malloc error "malloc(game data)", 8: game solution malloc error "malloc(game solution)", 9: game protmap malloc error "malloc(game protmap)"
		}
		fseek(in, 0, SEEK_CUR);
		ret->s_games[gameidx].s_offset = (uint32_t) ftell(in);
		fread(ret->s_games[gameidx].s_data, sizeof(uint8_t), 81, in);
		fseek(in, 1, SEEK_CUR); // skip protectmap separator
		fread(ret->s_games[gameidx].s_protmap, sizeof(uint8_t), 81, in);
		fseek(in, 1, SEEK_CUR); // skip solution separator
		fread(ret->s_games[gameidx].s_solution, sizeof(uint8_t), 81, in);
		fseek(in, 1, SEEK_CUR); // skip data section separator
		fread((void *) &ret->s_games[gameidx].s_time,        sizeof(uint32_t), 1, in);
		fread((void *) &ret->s_games[gameidx].s_solvestatus, sizeof(uint8_t),  1, in);
		fseek(in, 1, SEEK_CUR); // skip entry separator
	}
	return 0;
}

uint8_t sudogamesave(char *outname, sudogame *game) {
	if (!game->s_offset) return 1; // offset doesn't exist "offset"
	FILE *out = fopen(outname, "r+");
	if (!out) return 2; // fopen error "fopen(out, random)"
	fseek(out, game->s_offset, SEEK_SET);
	if (game->s_offset != ftell(out)) {
		fclose(out);
		return 3; // fseek error "fseek(out)"
	}
	fseek(out, 0, SEEK_CUR);
	fwrite((void *) game->s_data, sizeof(uint8_t), 81, out);
	fseek(out, 165, SEEK_CUR); // skip over protmap + separator, solution + separator and data separator
	fwrite((void *) &game->s_time,        sizeof(uint32_t), 1, out);
	fwrite((void *) &game->s_solvestatus, sizeof(uint8_t),  1, out);
	fclose(out);
	return 0;
}

void sudoprint(sudotracker *tracker) {
	for (tracker->s_ypos = 0; tracker->s_ypos < 9; tracker->s_ypos++) {
		for (tracker->s_xpos = 0; tracker->s_xpos < 9; tracker->s_xpos++) {
			uint8_t gidx = __gridxy2idx(tracker->s_xpos, tracker->s_ypos);
			if (tracker->s_game->s_data[gidx] > 0) {
				__sudogridputch(tracker->s_xpos, tracker->s_ypos, tracker->s_game->s_data[gidx] + 48);
				if (tracker->s_game->s_protmap[gidx]) {
					mvchgat(__gypos2abs(tracker->s_ypos), __gxpos2abs(tracker->s_xpos), 1, A_REVERSE, 0, NULL);
					refresh();
				}
			} else {
				__sudogridputch(tracker->s_xpos, tracker->s_ypos, ' ');
			}
		}
	}
	tracker->s_ypos = 0;
	tracker->s_xpos = 0;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		printf("usage: %s <generate <count> | load> <filename>\n", argv[0]);
		return 1;
	}
	sudomap map = { NULL, 0, NULL };
	if (!strncmp(argv[1], "generate", 8)) {
		if (argc < 4) {
			printf("usage: %s generate <count> <filename>\n", argv[0]);
			return 1;
		}
		for (uint8_t i = 0; i < strlen(argv[2]); i++) {
			if (!isdigit(argv[2][i])) {
				printf("generate: Invalid count: %s\n", argv[2]);
				return 1;
			}
		}
		if (strlen(argv[2]) > 5) {
			printf("generate: Count string too long (max 5 chars)\n");
			return 1;
		}
		uint32_t count = strtoul(argv[2], NULL, 10);
		if (count > UINT16_MAX) {
			printf("generate: Count too large (max %u)\n", UINT16_MAX);
			return 1;
		}
		uint8_t ret = sudogen(argv[3], (uint16_t) count);
		switch(ret) {
			case 1:
				printf("sudogen fopen(%s, write): %s\n", argv[3], strerror(errno));
				return 1;
			case 2:
				printf("sudogen malloc(qqwing): Couldn't malloc qqwing command\n");
				return 1;
			case 3:
				printf("sudogen malloc(popen(qqwing)): popen internal malloc error\n");
				return 1;
			case 4:
				printf("sudogen popen(qqwing): %s\n", strerror(errno));
				return 1;
			case 5:
				printf("sudogen malloc(buf): Couldn't malloc line buffer\n");
				return 1;
			case 6:
				printf("sudogen malloc(protmap): Couldn't malloc temporary protectmap\n");
				return 1;
		}
		return 0;
	} else if (!strncmp(argv[1], "load", 4)) {
		uint8_t ret = sudoload(argv[2], &map);
		switch(ret) {
			case 1:
				printf("sudoload fopen(%s, read): %s\n", argv[2], strerror(errno));
				return 1;
			case 2:
				printf("sudoload fstat(%s): %s\n", argv[2], strerror(errno));
				return 1;
			case 3:
				printf("sudoload fstat(%s) filesize: File too small\n", argv[2]);
				return 1;
			case 4:
				printf("sudoload malloc(signature): Couldn't allocate memory for signature string\n");
				return 1;
			case 5:
				printf("sudoload signature: Invalid signature in %s\n", argv[2]);
				return 1;
			case 6:
				printf("sudoload malloc(gamestack): Couldn't malloc game stack\n");
				return 1;
			case 7:
				printf("sudoload malloc(game data): Couldn't malloc game data in stack\n");
				return 1;
			case 8:
				printf("sudoload malloc(game solution): Couldn't malloc game solution in stack\n");
				return 1;
			case 9:
				printf("sudoload malloc(game protectmap): Couldn't malloc game protectmap in stack\n");
				return 1;
		}
		printf("sudoload success: Loaded %hu games from %s\n", map.s_count, argv[2]);
		printf("sudoload memory : Sudomap pointers consume %u bytes of memory\n", __sudomapsize(&map));
		printf("Press <Return> to continue...\n");
		fgetc(stdin);
	}
	setlocale(LC_ALL, "");
	initscr();
	getmaxyx(stdscr, TERM_MAX_Y, TERM_MAX_X);
	if (TERM_MAX_X < BOX_SCALE || TERM_MAX_Y < BOX_SCALE + 4) {
		printf("Your terminal doesn't fit the box\n");
		endwin();
		return 1;
	}
	start_color();
	init_pair(CP_WRONG, COLOR_BLACK, COLOR_RED);
	BOX_CEN_X   = (TERM_MAX_X / 2) - (BOX_SCALE   / 2);
	BOX_CEN_Y   = (TERM_MAX_Y / 2) - (BOX_SCALE   / 2);
	GRID_CEN_X  = BOX_CEN_X + 1;
	GRID_CEN_Y  = BOX_CEN_Y + 1;
	TIMER_CEN_X = (TERM_MAX_X / 2) - (TIMER_SCALE / 2);
	TIMER_CEN_Y = BOX_CEN_Y - 2;
	pthread_mutexattr_init(&TIMER_MUTEXATTR);
	pthread_mutexattr_init(&CHECK_MUTEXATTR);
	pthread_mutexattr_setrobust(&TIMER_MUTEXATTR, PTHREAD_MUTEX_ROBUST);
	pthread_mutexattr_setrobust(&CHECK_MUTEXATTR, PTHREAD_MUTEX_ROBUST);
	pthread_mutex_init(&TIMER_MUTEX, &TIMER_MUTEXATTR);
	pthread_mutex_init(&CHECK_MUTEX, &CHECK_MUTEXATTR);
	cbreak();
	noecho();
	keypad(stdscr, 1);
	mvprintw(BOX_CEN_Y,      BOX_CEN_X, "%s", BOX_ROW_TOP);
	mvprintw(BOX_CEN_Y + 1,  BOX_CEN_X, "%s", BOX_ROW_MID1);
	mvprintw(BOX_CEN_Y + 2,  BOX_CEN_X, "%s", BOX_ROW_MID2);
	mvprintw(BOX_CEN_Y + 3,  BOX_CEN_X, "%s", BOX_ROW_MID1);
	mvprintw(BOX_CEN_Y + 4,  BOX_CEN_X, "%s", BOX_ROW_MID2);
	mvprintw(BOX_CEN_Y + 5,  BOX_CEN_X, "%s", BOX_ROW_MID1);
	mvprintw(BOX_CEN_Y + 6,  BOX_CEN_X, "%s", BOX_ROW_MID3);
	mvprintw(BOX_CEN_Y + 7,  BOX_CEN_X, "%s", BOX_ROW_MID1);
	mvprintw(BOX_CEN_Y + 8,  BOX_CEN_X, "%s", BOX_ROW_MID2);
	mvprintw(BOX_CEN_Y + 9,  BOX_CEN_X, "%s", BOX_ROW_MID1);
	mvprintw(BOX_CEN_Y + 10, BOX_CEN_X, "%s", BOX_ROW_MID2);
	mvprintw(BOX_CEN_Y + 11, BOX_CEN_X, "%s", BOX_ROW_MID1);
	mvprintw(BOX_CEN_Y + 12, BOX_CEN_X, "%s", BOX_ROW_MID3);
	mvprintw(BOX_CEN_Y + 13, BOX_CEN_X, "%s", BOX_ROW_MID1);
	mvprintw(BOX_CEN_Y + 14, BOX_CEN_X, "%s", BOX_ROW_MID2);
	mvprintw(BOX_CEN_Y + 15, BOX_CEN_X, "%s", BOX_ROW_MID1);
	mvprintw(BOX_CEN_Y + 16, BOX_CEN_X, "%s", BOX_ROW_MID2);
	mvprintw(BOX_CEN_Y + 17, BOX_CEN_X, "%s", BOX_ROW_MID1);
	mvprintw(BOX_CEN_Y + 18, BOX_CEN_X, "%s", BOX_ROW_BOT);
	refresh();
	sudotracker tracker = { &(map.s_games[0]), 0, 0, 0 };
	sudoprint(&tracker);
	mvprintw(BOX_CEN_Y + 19, BOX_CEN_X, "Game 1/%hu", map.s_count);
	if (tracker.s_game->s_solvestatus) {
		mvprintw(BOX_CEN_Y + 20, BOX_CEN_X, "Solved    ");
	} else mvprintw(BOX_CEN_Y + 20, BOX_CEN_X, "Not solved");
	__gridmove(tracker.s_xpos, tracker.s_ypos);
	TIMER_TID = 0;
	CHECK_TID = 0;
	uint8_t esave = 0;
	pthread_create(&TIMER_TID, 0, &thr_timer, (void *) &tracker);
	pthread_detach(TIMER_TID);
	while (1) {
		int ch = getch();
		if (isdigit(ch)) {
			if (tracker.s_game->s_solvestatus) continue;
			if (tracker.s_game->s_protmap[__gridxy2idx(tracker.s_xpos, tracker.s_ypos)]) continue;
			tracker.s_game->s_data[__gridxy2idx(tracker.s_xpos, tracker.s_ypos)] = (uint8_t) (ch - 48);
			if (ch != '0') {
				__sudogridputch(tracker.s_xpos, tracker.s_ypos, (char) ch);
			} else __sudogridputch(tracker.s_xpos, tracker.s_ypos, ' ');
			__gridmove(tracker.s_xpos, tracker.s_ypos);
		} else if (ch == 'q') {
			if (!tracker.s_game->s_solvestatus) esave = sudogamesave(argv[2], tracker.s_game);
			break;
		} else if (ch == 'c') {
			if (tracker.s_game->s_solvestatus) continue;
			if (!memcmp(tracker.s_game->s_data, tracker.s_game->s_solution, 81 * sizeof(uint8_t))) {
				tracker.s_game->s_solvestatus = 1;
				esave = sudogamesave(argv[2], tracker.s_game);
				if (esave) break;
				mvprintw(BOX_CEN_Y + 20, BOX_CEN_X, "Solved    ");
				__gridmove(tracker.s_xpos, tracker.s_ypos);
			} else {
				pthread_create(&CHECK_TID, 0, &thr_check, (void *) &tracker);
				pthread_detach(CHECK_TID);
			}
		} else if (ch == 'w') {
			if (tracker.s_ypos < 1) continue;
			tracker.s_ypos--;
			__gridmove(tracker.s_xpos, tracker.s_ypos);
		} else if (ch == 's') {
			if (tracker.s_ypos > 7) continue;
			tracker.s_ypos++;
			__gridmove(tracker.s_xpos, tracker.s_ypos);
		} else if (ch == 'a') {
			if (tracker.s_xpos < 1) continue;
			tracker.s_xpos--;
			__gridmove(tracker.s_xpos, tracker.s_ypos);
		} else if (ch == 'd') {
			if (tracker.s_xpos > 7) continue;
			tracker.s_xpos++;
			__gridmove(tracker.s_xpos, tracker.s_ypos);
		} else if (ch == KEY_RIGHT) {
			if (tracker.s_index > map.s_count - 2) continue;
			pthread_cancel(TIMER_TID);
			if (!tracker.s_game->s_solvestatus) esave = sudogamesave(argv[2], tracker.s_game);
			if (esave) break;
			tracker.s_index++;
			tracker.s_game = &(map.s_games[tracker.s_index]);
			sudoprint(&tracker);
			mvprintw(BOX_CEN_Y + 19, BOX_CEN_X, "Game %hu/%hu", tracker.s_index + 1, map.s_count);
			if (tracker.s_game->s_solvestatus) {
				mvprintw(BOX_CEN_Y + 20, BOX_CEN_X, "Solved    ");
			} else mvprintw(BOX_CEN_Y + 20, BOX_CEN_X, "Not solved");
			__gridmove(tracker.s_xpos, tracker.s_ypos);
			pthread_create(&TIMER_TID, 0, &thr_timer, (void *) &tracker);
			pthread_detach(TIMER_TID);
		} else if (ch == KEY_LEFT) {
			if (tracker.s_index < 1) continue;
			pthread_cancel(TIMER_TID);
			if (!tracker.s_game->s_solvestatus) esave = sudogamesave(argv[2], tracker.s_game);
			if (esave) break;
			tracker.s_index--;
			tracker.s_game = &(map.s_games[tracker.s_index]);
			sudoprint(&tracker);
			mvprintw(BOX_CEN_Y + 19, BOX_CEN_X, "Game %hu/%hu", tracker.s_index + 1, map.s_count);
			if (tracker.s_game->s_solvestatus) {
				mvprintw(BOX_CEN_Y + 20, BOX_CEN_X, "Solved    ");
			} else mvprintw(BOX_CEN_Y + 20, BOX_CEN_X, "Not solved");
			__gridmove(tracker.s_xpos, tracker.s_ypos);
			pthread_create(&TIMER_TID, 0, &thr_timer, (void *) &tracker);
			pthread_detach(TIMER_TID);
		}
	}
	pthread_cancel(TIMER_TID);
	__sudomapfree(&map);
	endwin();
	switch(esave) {
		case 1:
			printf("sudogamesave offset: Offset of current game not in memory\n");
			return 1;
		case 2:
			printf("sudogamesave fopen(%s, random): Can't open file for writing\n", argv[2]);
			return 1;
		case 3:
			printf("sudogamesave fseek(%s): Offset of current game doesn't exist in file\n", argv[2]);
			return 1;
	}
	return 0;
}
