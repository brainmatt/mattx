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
 
#include <unistd.h> /* read, write, close */
#include <sys/socket.h> /* socket, connect */
#include <netinet/in.h> /* struct sockaddr_in, struct sockaddr */
#include <netdb.h> /* struct hostent, gethostbyname */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pvmchess.h"

char board[64];
int color_to_move = 0;
int engine_side = 1;

// Feature 2: In-Memory History of Moves to detect Remis
Move move_history[4096];
int total_moves = 0;

void error(const char *msg) { perror(msg); exit(0); }


void check_game_over(Move m, char captured_piece) {
    // 1. Detect if King is taken
    if (captured_piece == 'K') { // White King Taken -> Black Wins
        FILE* f = fopen("board.log", "a");
        fprintf(f, "\n*** GAME OVER ***\nWinner: Black\nReason: White King Taken\nGame Statistics: Total moves played = %d\n\n", total_moves + 1);
        fclose(f);
        log_action("Game Over. Black wins. Exiting.");
        printf("0-1 {Black mates}\n");
        pvm_exit();
        exit(0);
    } else if (captured_piece == 'k') { // Black King Taken -> White Wins
        FILE* f = fopen("board.log", "a");
        fprintf(f, "\n*** GAME OVER ***\nWinner: White\nReason: Black King Taken\nGame Statistics: Total moves played = %d\n\n", total_moves + 1);
        fclose(f);
        log_action("Game Over. White wins. Exiting.");
        printf("1-0 {White mates}\n");
        pvm_exit();
        exit(0);
    }

    // 2. Detect "Remis" by evaluating repeating patterns 
    int move_occurrences = 0;
    
    // Only check the last 100 entries of the history
    int start_idx = (total_moves > 100) ? total_moves - 100 : 0;
    int i;
    for (i = start_idx; i < total_moves; i++) {
        // Checking if this exact piece move happened in the recent history
        if (move_history[i].from == m.from && move_history[i].to == m.to) {
            move_occurrences++;
        }
    }
    
    // Trigger if this same exact move has been executed 5 times previously (this makes it the 6th time)
    if (move_occurrences >= 5) {
        FILE* f = fopen("board.log", "a");
        fprintf(f, "\n*** GAME OVER ***\nResult: REMIS (Draw)\nReason: Recurring move pattern detected (Move %c%c-%c%c occurred 6 times)\nGame Statistics: Total moves played = %d\n\n", 
            'a' + (m.from % 8), '1' + (m.from / 8), 'a' + (m.to % 8), '1' + (m.to / 8), total_moves + 1);
        fclose(f);
        log_action("Game Over. Remis. Exiting.");
        printf("1/2-1/2 {Draw by repetition}\n");
        pvm_exit();
        exit(0);
    }
}

void apply_move(Move m, const char* label) {
    log_action("Applying move: %d to %d (%s)", m.from, m.to, label);
    
    char captured_piece = board[m.to];
    
    // Apply move physically
    board[m.to] = board[m.from];
    board[m.from] = ' ';
    color_to_move = 1 - color_to_move;
    
    log_board_state(board, label); 
    
    // Feature 1 & 2: Evaluate End Game Logic
    check_game_over(m, captured_piece);
    
    // Append to memory history
    move_history[total_moves] = m;
    total_moves++;
}

void do_pvm_move() {
    log_action("Generating legal branches for Engine...");
    Move moves[256];
    int count = generate_moves(board, engine_side, moves);
    
    if (count == 0) {
        log_action("No legal moves available. Engine resigns.");
        
        // Determine the winner based on engine's side
        const char* winner = (engine_side == 1) ? "White" : "Black";
        
        // Log game statistics and winner to the board logfile
        FILE* f = fopen("board.log", "a");
        if (f) {
            fprintf(f, "\n*** GAME OVER ***\nWinner: %s\nReason: Engine has no legal moves and resigns\nGame Statistics: Total moves played = %d\n\n", winner, total_moves);
            fclose(f);
        }
        
        // Notify XBoard and end the chess program
        if (engine_side == 1) {
            printf("1-0 {Resign}\n");
        } else {
            printf("0-1 {Resign}\n");
        }
        
        pvm_exit();
        exit(0);
    }

    log_action("Found %d legal branches. Spawning %d PVM Clients.", count, count);
    int* tids = malloc(count * sizeof(int));
    int i;
    for (i = 0; i < count; i++) {
        int info = pvm_spawn("pvmchess-client", NULL, PvmTaskDefault, "", 1, &tids[i]);
        if (info < 1) {
            log_action("ERROR: Failed to spawn client %d", i);
            exit(1);
        }
        
        pvm_initsend(PvmDataDefault);
        pvm_pkbyte(board, 64, 1);
        pvm_pkint(&moves[i].from, 1, 1);
        pvm_pkint(&moves[i].to, 1, 1);
        pvm_pkint(&engine_side, 1, 1);
        pvm_send(tids[i], MSG_EVAL);
    }

    int best_score = -9999999;
    Move best_move = moves[0];
    for (i = 0; i < count; i++) {
        int score, from, to;
        int bufid = pvm_recv(-1, MSG_RESULT);
        int bytes, msgtag, sender_tid;
        pvm_bufinfo(bufid, &bytes, &msgtag, &sender_tid);

        pvm_upkint(&score, 1, 1);
        pvm_upkint(&from, 1, 1);
        pvm_upkint(&to, 1, 1);

        log_action("Received score %d for branch %d-%d from Client TID: %x", score, from, to, sender_tid);

        if (score > best_score) {
            best_score = score;
            best_move.from = from;
            best_move.to = to;
        }
    }
    free(tids);

    log_action("Best move decided: %d to %d with score %d", best_move.from, best_move.to, best_score);
    
    apply_move(best_move, "Engine Move");
    
    printf("move %c%c-%c%c\n", 
        'a' + (best_move.from % 8), '1' + (best_move.from / 8),
        'a' + (best_move.to % 8), '1' + (best_move.to / 8));


    char move_str[16];
    snprintf(move_str, sizeof(move_str), "%c%c-%c%c\n",
    'a' + (best_move.from % 8), '1' + (best_move.from / 8),
    'a' + (best_move.to % 8),   '1' + (best_move.to / 8));

    printf("%s", move_str);
    int pr = 0;
    pr = postmove(move_str);
    pr++;
}





int postmove(const char *move_str)
{
    /* first what are we going to send and where are we going to send it? */
    int portno =        5002;
    char *host =        "localhost";
    char *message_fmt = "GET /postmove?move=%s HTTP/1.0\r\n\r\n";

    struct hostent *server;
    struct sockaddr_in serv_addr;
    int sockfd, bytes, sent, received, total;
    char message[1024],response[4096];

    // if (argc < 2) { puts("Parameters: <move>"); exit(0); }

    /* fill in the parameters */
    sprintf(message,message_fmt,move_str);
    printf("Request:\n%s\n",message);

    /* create the socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("ERROR opening socket");

    /* lookup the ip address */
    server = gethostbyname(host);
    if (server == NULL) error("ERROR no such host");

    /* fill in the structure */
    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
    memcpy(&serv_addr.sin_addr.s_addr,server->h_addr,server->h_length);

    /* connect the socket */
    if (connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr)) < 0)
        error("ERROR connecting");

    /* send the request */
    total = strlen(message);
    sent = 0;
    do {
        bytes = write(sockfd,message+sent,total-sent);
        if (bytes < 0)
            error("ERROR writing message to socket");
        if (bytes == 0)
            break;
        sent+=bytes;
    } while (sent < total);

    /* receive the response */
    memset(response,0,sizeof(response));
    total = sizeof(response)-1;
    received = 0;
    do {
        bytes = read(sockfd,response+received,total-received);
        if (bytes < 0)
            error("ERROR reading response from socket");
        if (bytes == 0)
            break;
        received+=bytes;
    } while (received < total);

    /*
     * if the number of received bytes is the total size of the
     * array then we have run out of space to store the response
     * and it hasn't all arrived yet - so that's a bad thing
     */
    if (received == total)
        error("ERROR storing complete response from socket");

    /* close the socket */
    close(sockfd);

    /* process response */
    printf("Response:\n%s\n",response);

    return 0;
}






int main(int argc, char** argv) {
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);

    log_action("--- PVM Master Chess Program Started ---");
    memcpy(board, INITIAL_BOARD, 64);
    log_board_state(board, "Initial Board (Startup)");

    char line[256];
    while (fgets(line, 256, stdin)) {
        log_action("XBoard command received: %s", line);
        
        if (strncmp(line, "xboard", 6) == 0) {
            printf("feature done=1\n");
        } else if (strncmp(line, "new", 3) == 0) {
            memcpy(board, INITIAL_BOARD, 64);
            color_to_move = 0;
            engine_side = 1;
            total_moves = 0; // Reset Memory History on new game
            log_action("New game started");
            log_board_state(board, "Initial Board (New Game)");
            
        } else if (strncmp(line, "force", 5) == 0) {
            engine_side = -1;
            log_action("Engine forced to wait");
        } else if (strncmp(line, "go", 2) == 0) {
            engine_side = color_to_move;
            do_pvm_move();
        } else if (strncmp(line, "quit", 4) == 0) {
            log_action("Quitting master.");
            break;
        } else if (line[0] >= 'a' && line[0] <= 'h' && line[1] >= '1' && line[1] <= '8') {
            int from = (line[0] - 'a') + (line[1] - '1') * 8;
            int to = (line[2] - 'a') + (line[3] - '1') * 8;
            
            Move legal_moves[256];
            int count = generate_moves(board, color_to_move, legal_moves);
            int is_legal = 0;
            int i;
            for (i = 0; i < count; i++) {
                if (legal_moves[i].from == from && legal_moves[i].to == to) {
                    is_legal = 1;
                    break;
                }
            }
            
            if (is_legal) {
                Move m = {from, to};
                apply_move(m, "Human Move");

                if (color_to_move == engine_side) {
                    do_pvm_move();
                }
            } else {
                log_action("NOTICE: Illegal human move attempted (%c%c%c%c) according to game piece rules. Ignored.", line[0], line[1], line[2], line[3]);
            }
        }
    }

    pvm_exit();
    return 0;
}


