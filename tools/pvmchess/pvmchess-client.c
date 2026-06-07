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
#include "pvmchess.h"

int minimax(char* current_board, int depth, int player_color) {
    if (depth == 0) return evaluate_board(current_board, player_color);

    Move moves[256];
    int count = generate_moves(current_board, player_color, moves);
    
    if (count == 0) {
        if (is_in_check(current_board, player_color)) return -99999;
        return 0;
    }

    int best_score = -999999;
    int i;
    for (i = 0; i < count; i++) {
        char temp_board[64];
        memcpy(temp_board, current_board, 64);
        
        temp_board[moves[i].to] = temp_board[moves[i].from];
        temp_board[moves[i].from] = ' ';

        int score = -minimax(temp_board, depth - 1, 1 - player_color);
        if (score > best_score) best_score = score;
    }
    return best_score;
}

int main(int argc, char** argv) {
    int master_tid = pvm_parent();
    log_action("PVM Client started. Master TID: %x", master_tid);

    char board[64];
    int from, to, player_side;

    pvm_recv(master_tid, MSG_EVAL);
    pvm_upkbyte(board, 64, 1);
    pvm_upkint(&from, 1, 1);
    pvm_upkint(&to, 1, 1);
    pvm_upkint(&player_side, 1, 1);

    log_action("Client evaluating branch: Move piece %d to %d for side %d", from, to, player_side);

    board[to] = board[from];
    board[from] = ' ';

    log_action("Client beginning recursive evaluation at Depth %d", DEPTH - 1);
    int score = -minimax(board, DEPTH - 1, 1 - player_side);
    log_action("Client finished evaluation. Calculated Weight: %d. Sending back to Master.", score);

    pvm_initsend(PvmDataDefault);
    pvm_pkint(&score, 1, 1);
    pvm_pkint(&from, 1, 1);
    pvm_pkint(&to, 1, 1);
    pvm_send(master_tid, MSG_RESULT);

    log_action("Client shutting down.");
    pvm_exit();
    return 0;
}

