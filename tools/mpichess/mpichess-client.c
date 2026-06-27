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
#include "mpichess.h"

int my_tid = 0;

int minimax(char* current_board, int depth, int player_color) {
    if (depth == 0) return evaluate_board(current_board, player_color);

    Move moves[256];
    int count = generate_moves(current_board, player_color, moves);
    
    if (count == 0) {
        if (is_in_check(current_board, player_color)) return -99999;
        return 0;
    }

    int best_score = -999999;
    for (int i = 0; i < count; i++) {
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
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_tid); // Rank in its own world (usually 0 for spawned singletons, but intercomm rank matters)
    
    MPI_Comm master_comm;
    MPI_Comm_get_parent(&master_comm);
    
    if (master_comm == MPI_COMM_NULL) {
        log_action("ERROR: No parent MPI process found. Exiting.");
        MPI_Finalize();
        return 1;
    }

    log_action("MPI Client started. Awaiting branch from Master.");

    EvalMsg msg;
    MPI_Recv(&msg, sizeof(EvalMsg), MPI_BYTE, 0, MSG_EVAL, master_comm, MPI_STATUS_IGNORE);

    char board[64];
    memcpy(board, msg.board, 64);
    int from = msg.from;
    int to = msg.to;
    int player_side = msg.engine_side;

    log_action("Client evaluating branch: Move piece %d to %d for side %d", from, to, player_side);

    board[to] = board[from];
    board[from] = ' ';

    log_action("Client beginning recursive evaluation at Depth %d", DEPTH - 1);
    int score = -minimax(board, DEPTH - 1, 1 - player_side);
    log_action("Client finished evaluation. Calculated Weight: %d. Sending back to Master.", score);

    ResultMsg res;
    res.score = score;
    res.from = from;
    res.to = to;
    MPI_Send(&res, sizeof(ResultMsg), MPI_BYTE, 0, MSG_RESULT, master_comm);

    log_action("Client shutting down.");
    
    MPI_Comm_disconnect(&master_comm);
    MPI_Finalize();
    return 0;
}

