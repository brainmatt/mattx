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
#include <errno.h>
#include "mpichess.h"

/*
 * mpichess-queuing: same master as mpichess.c, but the "mpichess-client"
 * branch evaluators are not all spawned at once. At most NUM_CLIENTS clients
 * are alive at the same time; a new one is spawned only when a previous one
 * has delivered its result (sliding window queue).
 *
 * Runtime configuration (re-read before every engine move), file ./mpichess.conf
 * or the file named by the environment variable MPICHESS_CONF:
 *
 *   NUM_CLIENTS=4       max. simultaneous clients (0 = unlimited, legacy behaviour)
 *   SPAWN_DELAY_MS=0    min. gap in ms between two client spawns
 */
#define MPICHESS_CONF_ENV          "MPICHESS_CONF"
#define MPICHESS_CONF_DEFAULT_PATH "./mpichess.conf"
#define DEFAULT_NUM_CLIENTS        4
#define DEFAULT_SPAWN_DELAY_MS     0
#define MAX_CLIENTS                256   /* == size of Move moves[256] */
#define MAX_CONF_VALUE             1000000
#define POLL_SLEEP_MS              1     /* gentle wait: don't busy-poll on the home node */

typedef struct {
    int num_clients;      /* 0 = unlimited */
    int spawn_delay_ms;
    const char *source;   /* "file" or "default" (for the log) */
} QueueConf;

int my_tid = 0;
char board[64];
int color_to_move = 0;
int engine_side = 1;

static char *conf_trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int conf_parse_int(const char *val, int *out)
{
    char *endp;
    long v;

    errno = 0;
    v = strtol(val, &endp, 10);
    if (errno || endp == val || *endp != '\0' || v < 0 || v > MAX_CONF_VALUE)
        return -1;
    *out = (int)v;
    return 0;
}

/*
 * Reads NUM_CLIENTS / SPAWN_DELAY_MS. Missing file, missing key or an invalid
 * value never aborts the game: the safe default for that key is used and a
 * NOTICE goes to mpichess.log.
 */
static void load_queue_conf(QueueConf *qc)
{
    const char *path = getenv(MPICHESS_CONF_ENV);
    char line[256];
    int lineno = 0;
    FILE *f;

    qc->num_clients = DEFAULT_NUM_CLIENTS;
    qc->spawn_delay_ms = DEFAULT_SPAWN_DELAY_MS;
    qc->source = "default";

    if (!path || !*path)
        path = MPICHESS_CONF_DEFAULT_PATH;

    f = fopen(path, "r");
    if (!f) {
        log_action("NOTICE: Cannot open config '%s' (%s). Using defaults.", path, strerror(errno));
        return;
    }
    qc->source = "file";

    while (fgets(line, sizeof(line), f)) {
        char *p, *eq, *hash, *key, *val;
        int n;

        lineno++;
        if (!strchr(line, '\n') && !feof(f)) {
            int c;
            log_action("NOTICE: %s:%d: line too long, ignored.", path, lineno);
            while ((c = fgetc(f)) != '\n' && c != EOF)
                ;
            continue;
        }

        p = conf_trim(line);
        if (*p == '\0' || *p == '#')
            continue;

        eq = strchr(p, '=');
        if (!eq) {
            log_action("NOTICE: %s:%d: expected KEY=VALUE, ignored.", path, lineno);
            continue;
        }
        *eq = '\0';
        key = conf_trim(p);
        val = eq + 1;
        hash = strchr(val, '#');          /* allow trailing "# comment" */
        if (hash)
            *hash = '\0';
        val = conf_trim(val);

        if (strcmp(key, "NUM_CLIENTS") == 0) {
            if (conf_parse_int(val, &n) == 0)
                qc->num_clients = n;
            else
                log_action("NOTICE: %s:%d: invalid NUM_CLIENTS '%s', keeping %d.", path, lineno, val, qc->num_clients);
        } else if (strcmp(key, "SPAWN_DELAY_MS") == 0) {
            if (conf_parse_int(val, &n) == 0)
                qc->spawn_delay_ms = n;
            else
                log_action("NOTICE: %s:%d: invalid SPAWN_DELAY_MS '%s', keeping %d.", path, lineno, val, qc->spawn_delay_ms);
        } else {
            log_action("NOTICE: %s:%d: unknown key '%s', ignored.", path, lineno, key);
        }
    }
    fclose(f);
}

/* 0 = unlimited; never more than there are branches, never more than MAX_CLIENTS */
static int effective_limit(const QueueConf *qc, int count)
{
    int lim = qc->num_clients;

    if (lim <= 0 || lim > count)
        lim = count;
    if (lim > MAX_CLIENTS)
        lim = MAX_CLIENTS;
    return lim;
}

// Feature 2: In-Memory History of Moves to detect Remis
Move move_history[4096];
int total_moves = 0;

void check_game_over(Move m, char captured_piece) {
    // 1. Detect if King is taken
    if (captured_piece == 'K') { // White King Taken -> Black Wins
        FILE* f = fopen("board.log", "a");
        fprintf(f, "\n*** GAME OVER ***\nWinner: Black\nReason: White King Taken\nGame Statistics: Total moves played = %d\n\n", total_moves + 1);
        fclose(f);
        log_action("Game Over. Black wins. Exiting.");
        printf("0-1 {Black mates}\n");
        MPI_Finalize();
        exit(0);
    } else if (captured_piece == 'k') { // Black King Taken -> White Wins
        FILE* f = fopen("board.log", "a");
        fprintf(f, "\n*** GAME OVER ***\nWinner: White\nReason: Black King Taken\nGame Statistics: Total moves played = %d\n\n", total_moves + 1);
        fclose(f);
        log_action("Game Over. White wins. Exiting.");
        printf("1-0 {White mates}\n");
        MPI_Finalize();
        exit(0);
    }

    // 2. Detect "Remis" by evaluating repeating patterns 
    int move_occurrences = 0;
    
    // Only check the last 100 entries of the history
    int start_idx = (total_moves > 100) ? total_moves - 100 : 0;
    
    for (int i = start_idx; i < total_moves; i++) {
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
        MPI_Finalize();
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




int postmove(const char *move_str)
{
    /* first what are we going to send and where are we going to send it? */
    int portno =        5003;
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
    if (sockfd < 0) log_action("ERROR opening socket");

    /* lookup the ip address */
    server = gethostbyname(host);
    if (server == NULL) log_action("ERROR no such host");

    /* fill in the structure */
    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
    memcpy(&serv_addr.sin_addr.s_addr,server->h_addr,server->h_length);

    /* connect the socket */
    if (connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr)) < 0)
        log_action("ERROR connecting");

    /* send the request */
    total = strlen(message);
    sent = 0;
    do {
        bytes = write(sockfd,message+sent,total-sent);
        if (bytes < 0)
            log_action("ERROR writing message to socket");
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
            log_action("ERROR reading response from socket");
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
        log_action("ERROR storing complete response from socket");

    /* close the socket */
    close(sockfd);

    /* process response */
    printf("Response:\n%s\n",response);

    return 0;
}



typedef struct {
    MPI_Comm  comm;       /* intercomm to exactly one client */
    int       move_idx;   /* branch evaluated by this slot */
    ResultMsg res;        /* MPI_Irecv target */
    double    t_start;    /* spawn time in ms, for the log */
} ClientSlot;

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/*
 * Sliding window queue: evaluate all "count" branches with at most "limit"
 * mpichess-client processes alive at the same time.
 *
 * Every client gets its own intercommunicator (MPI_Comm_spawn with maxprocs=1),
 * so a client can be retired the moment it has delivered its result and its
 * slot is refilled from the queue right away. A client is "finished" when its
 * result was received and the intercomm disconnected (the process needs a few
 * more ms to exit, so NUM_CLIENTS+1 processes can briefly coexist).
 *
 * The best move is chosen in move generation order (strict ">"), so the result
 * does not depend on the order in which the clients happen to finish.
 */
static void run_branch_queue(const Move *moves, int count, int limit, int spawn_delay_ms,
                             Move *best_move, int *best_score)
{
    static ClientSlot slot[MAX_CLIENTS];
    MPI_Request req[MAX_CLIENTS];
    int scores[MAX_CLIENTS];
    int next = 0, active = 0, done = 0;
    double t_queue = now_ms();
    double t_last_spawn = -1.0;

    for (int s = 0; s < limit; s++) {
        req[s] = MPI_REQUEST_NULL;
        slot[s].comm = MPI_COMM_NULL;
    }

    while (done < count) {
        /* 1. Refill free slots from the queue */
        while (active < limit && next < count) {
            EvalMsg msg;
            int s = 0;
            int err;

            while (req[s] != MPI_REQUEST_NULL)
                s++;

            if (spawn_delay_ms > 0 && t_last_spawn >= 0.0) {
                double wait = spawn_delay_ms - (now_ms() - t_last_spawn);
                if (wait > 0.0)
                    sleep_ms((int)(wait + 0.999));
            }

            err = MPI_Comm_spawn("./mpichess-client", MPI_ARGV_NULL, 1, MPI_INFO_NULL, 0, MPI_COMM_SELF, &slot[s].comm, MPI_ERRCODES_IGNORE);
            t_last_spawn = now_ms();
            if (err != MPI_SUCCESS) {
                log_action("ERROR: Failed to spawn MPI client for branch %d-%d", moves[next].from, moves[next].to);
                exit(1);
            }
            MPI_Comm_set_errhandler(slot[s].comm, MPI_ERRORS_RETURN);

            memcpy(msg.board, board, 64);
            msg.from = moves[next].from;
            msg.to = moves[next].to;
            msg.engine_side = engine_side;

            err = MPI_Send(&msg, sizeof(EvalMsg), MPI_BYTE, 0, MSG_EVAL, slot[s].comm);
            if (err != MPI_SUCCESS) {
                log_action("ERROR: Failed to send branch %d-%d to client slot %d", msg.from, msg.to, s);
                exit(1);
            }
            err = MPI_Irecv(&slot[s].res, sizeof(ResultMsg), MPI_BYTE, 0, MSG_RESULT, slot[s].comm, &req[s]);
            if (err != MPI_SUCCESS) {
                log_action("ERROR: Failed to post result receive for client slot %d", s);
                exit(1);
            }

            slot[s].move_idx = next;
            slot[s].t_start = t_last_spawn;
            next++;
            active++;
            log_action("Spawned client slot %d for branch %d-%d (active=%d, queued=%d)",
                       s, msg.from, msg.to, active, count - next);
        }

        /* 2. Wait for ANY client to deliver its result (gentle poll, no busy spin) */
        int idx = MPI_UNDEFINED;
        int flag = 0;
        for (;;) {
            int err = MPI_Testany(limit, req, &idx, &flag, MPI_STATUS_IGNORE);
            if (err != MPI_SUCCESS) {
                log_action("ERROR: MPI_Testany failed while waiting for clients");
                exit(1);
            }
            if (flag)
                break;
            sleep_ms(POLL_SLEEP_MS);
        }
        if (idx == MPI_UNDEFINED) {
            log_action("ERROR: Queue has no outstanding client requests (done=%d of %d)", done, count);
            exit(1);
        }

        /* 3. Harvest the result and release the slot */
        ClientSlot *c = &slot[idx];
        int mi = c->move_idx;

        if (c->res.from != moves[mi].from || c->res.to != moves[mi].to)
            log_action("NOTICE: Client slot %d answered for branch %d-%d but was given %d-%d",
                       idx, c->res.from, c->res.to, moves[mi].from, moves[mi].to);

        scores[mi] = c->res.score;
        active--;
        done++;
        log_action("Received score %d for branch %d-%d from client slot %d after %.2fs (active=%d, queued=%d)",
                   c->res.score, moves[mi].from, moves[mi].to, idx,
                   (now_ms() - c->t_start) / 1000.0, active, count - next);

        MPI_Comm_disconnect(&c->comm);
    }

    /* 4. Deterministic selection: first best score in move generation order */
    for (int i = 0; i < count; i++) {
        if (scores[i] > *best_score) {
            *best_score = scores[i];
            best_move->from = moves[i].from;
            best_move->to = moves[i].to;
        }
    }

    log_action("Queue drained: %d branches evaluated in %.2fs with limit %d",
               count, (now_ms() - t_queue) / 1000.0, limit);
}

void do_mpi_move() {
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
        
        MPI_Finalize();
        exit(0);
    }

    QueueConf qc;
    load_queue_conf(&qc);
    int limit = effective_limit(&qc, count);
    log_action("Found %d legal branches. Queue limit NUM_CLIENTS=%d (effective %d, config source: %s), SPAWN_DELAY_MS=%d.",
               count, qc.num_clients, limit, qc.source, qc.spawn_delay_ms);

    int best_score = -9999999;
    Move best_move = moves[0];

    run_branch_queue(moves, count, limit, qc.spawn_delay_ms, &best_move, &best_score);

    log_action("Best move decided: %d to %d with score %d", best_move.from, best_move.to, best_score);
    
    apply_move(best_move, "Engine Move");
    
    printf("move %c%c%c%c\n", 
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




int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &my_tid);

    /* Report MPI errors (e.g. a failed MPI_Comm_spawn) instead of aborting silently,
     * so the ERROR lines in do_mpi_move()/run_branch_queue() are reachable. */
    MPI_Comm_set_errhandler(MPI_COMM_SELF, MPI_ERRORS_RETURN);

    setbuf(stdin, NULL);
    setbuf(stdout, NULL);

    log_action("--- MPI Master Chess Program Started ---");
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
            do_mpi_move();
        } else if (strncmp(line, "quit", 4) == 0) {
            log_action("Quitting master.");
            break;
        } else if (line[0] >= 'a' && line[0] <= 'h' && line[1] >= '1' && line[1] <= '8') {
            int from = (line[0] - 'a') + (line[1] - '1') * 8;
            int to = (line[2] - 'a') + (line[3] - '1') * 8;
            
            Move legal_moves[256];
            int count = generate_moves(board, color_to_move, legal_moves);
            int is_legal = 0;
            
            for (int i = 0; i < count; i++) {
                if (legal_moves[i].from == from && legal_moves[i].to == to) {
                    is_legal = 1;
                    break;
                }
            }
            
            if (is_legal) {
                Move m = {from, to};
                apply_move(m, "Human Move");

                if (color_to_move == engine_side) {
                    do_mpi_move();
                }
            } else {
                log_action("NOTICE: Illegal human move attempted (%c%c%c%c) according to game piece rules. Ignored.", line[0], line[1], line[2], line[3]);
            }
        }
    }

    MPI_Finalize();
    return 0;
}

