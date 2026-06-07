#ifndef PVMCHESS_H
#define PVMCHESS_H

/*
 * MattX - The Modern Single System Image (SSI) Cluster
 * 
 * Copyright (c) 2026 by Matthias Rechenburg
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Commercial licensing options are available upon request.
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include "pvm3.h"

#define MSG_EVAL 1
#define MSG_RESULT 2
#define DEPTH 4

int postmove(const char *move_str);

typedef struct {
    int from;
    int to;
} Move;

static const char INITIAL_BOARD[64] = {
    'R','N','B','Q','K','B','N','R',
    'P','P','P','P','P','P','P','P',
    ' ',' ',' ',' ',' ',' ',' ',' ',
    ' ',' ',' ',' ',' ',' ',' ',' ',
    ' ',' ',' ',' ',' ',' ',' ',' ',
    ' ',' ',' ',' ',' ',' ',' ',' ',
    'p','p','p','p','p','p','p','p',
    'r','n','b','q','k','b','n','r'
};

// --- LOGGING FUNCTIONS ---
static void log_action(const char* fmt, ...) {
    FILE* f = fopen("pvmchess.log", "a");
    if (!f) return;
    
    time_t now;
    time(&now);
    char time_str[26];
    ctime_r(&now, time_str);
    time_str[24] = '\0'; 

    fprintf(f, "[%s] [TID: %x] ", time_str, pvm_mytid());
    
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    
    fprintf(f, "\n");
    fclose(f);
}

static void log_board_state(const char* board, const char* label) {
    FILE* f = fopen("board.log", "a");
    if (!f) return;
    
    time_t now;
    time(&now);
    char time_str[26];
    ctime_r(&now, time_str);
    time_str[24] = '\0';

    fprintf(f, "\n[%s][TID: %x] --- Chess Board State: %s ---\n", time_str, pvm_mytid(), label);
    int r;
    int c;
    for (r = 7; r >= 0; r--) {
        fprintf(f, "[TID: %x] %d | ", pvm_mytid(), r + 1);
        for (c = 0; c < 8; c++) {
            fprintf(f, "%c ", board[r * 8 + c]);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "[TID: %x]     ----------------\n", pvm_mytid());
    fprintf(f, "[TID: %x]     a b c d e f g h\n\n", pvm_mytid());
    fclose(f);
}

// --- CHESS LOGIC FUNCTIONS ---
static int get_color(char piece) {
    if (piece == ' ') return -1;
    return isupper(piece) ? 0 : 1; 
}

static int is_clear_path(const char* board, int from, int to) {
    int r1 = from / 8, c1 = from % 8;
    int r2 = to / 8, c2 = to % 8;
    int dr = (r2 > r1) ? 1 : ((r2 < r1) ? -1 : 0);
    int dc = (c2 > c1) ? 1 : ((c2 < c1) ? -1 : 0);
    
    int r = r1 + dr, c = c1 + dc;
    while (r != r2 || c != c2) {
        if (board[r * 8 + c] != ' ') return 0;
        r += dr;
        c += dc;
    }
    return 1;
}

static int is_valid_piece_move(const char* board, int from, int to, int player_color) {
    if (from == to) return 0;
    char p = board[from];
    char target = board[to];
    
    if (target != ' ' && get_color(target) == player_color) return 0; 

    int r1 = from / 8, c1 = from % 8;
    int r2 = to / 8, c2 = to % 8;
    int dr = abs(r2 - r1);
    int dc = abs(c2 - c1);

    switch (tolower(p)) {
        case 'p': { 
            int dir = (player_color == 0) ? 1 : -1;
            int start_row = (player_color == 0) ? 1 : 6;
            if (c1 == c2 && r2 == r1 + dir && target == ' ') return 1;
            if (c1 == c2 && r1 == start_row && r2 == r1 + 2 * dir && target == ' ' && board[(r1 + dir) * 8 + c1] == ' ') return 1;
            if (dc == 1 && r2 == r1 + dir && target != ' ' && get_color(target) != player_color) return 1;
            return 0;
        }
        case 'n': 
            return (dr == 2 && dc == 1) || (dr == 1 && dc == 2);
        case 'b': 
            if (dr != dc) return 0;
            return is_clear_path(board, from, to);
        case 'r': 
            if (dr != 0 && dc != 0) return 0;
            return is_clear_path(board, from, to);
        case 'q': 
            if (dr != dc && dr != 0 && dc != 0) return 0;
            return is_clear_path(board, from, to);
        case 'k': 
            return (dr <= 1 && dc <= 1);
    }
    return 0;
}

static int is_in_check(const char* board, int player_color) {
    int king_pos = -1;
    char king_char = (player_color == 0) ? 'K' : 'k';
    int i;
    for (i = 0; i < 64; i++) {
        if (board[i] == king_char) { king_pos = i; break; }
    }
    if (king_pos == -1) return 0; 
    
    for (i = 0; i < 64; i++) {
        if (board[i] != ' ' && get_color(board[i]) == 1 - player_color) {
            if (is_valid_piece_move(board, i, king_pos, 1 - player_color)) return 1;
        }
    }
    return 0;
}

static int generate_moves(const char* board, int player_color, Move* moves) {
    int count = 0;
    int from;
    int to;
    for (from = 0; from < 64; from++) {
        if (get_color(board[from]) == player_color) {
            for (to = 0; to < 64; to++) {
                if (is_valid_piece_move(board, from, to, player_color)) {
                    char temp[64];
                    memcpy(temp, board, 64);
                    temp[to] = temp[from];
                    temp[from] = ' ';
                    
                    if (!is_in_check(temp, player_color)) {
                        moves[count].from = from;
                        moves[count].to = to;
                        count++;
                    }
                }
            }
        }
    }
    return count;
}

static int evaluate_board(const char* board, int player_color) {
    int score = 0;
    int i;
    for (i = 0; i < 64; i++) {
        char p = board[i];
        int val = 0;
        if (p == 'p' || p == 'P') val = 10;
        else if (p == 'n' || p == 'N' || p == 'b' || p == 'B') val = 30;
        else if (p == 'r' || p == 'R') val = 50;
        else if (p == 'q' || p == 'Q') val = 90;
        else if (p == 'k' || p == 'K') val = 900;
        
        if (get_color(p) == player_color) score += val;
        else if (get_color(p) != -1) score -= val;
    }
    return score;
}

#endif


