/*
 * HIP.cpp — Human Interfacing Process (HIP)
 *
 * Threading:  pthreads only. No <atomic>, no <thread>.
 * IPC:        Shared memory + unnamed semaphores only. NO pipes (§4).
 *
 * ── Section 2 requirements satisfied ────────────────────────────────────────
 *  • Separate process from the Arbiter.
 *  • Multi-threaded: one pthread per human player character.
 *  • Only the thread for the currently active player (as set by the Arbiter
 *    via gs->active_entity) reads and processes input.
 *  • All other player threads remain idle (poll at low frequency).
 *  • HIP does NOT modify global game state directly.
 *    It writes only to gs->hip_action; the Arbiter applies the change.
 *
 * ── Section 5 (Stun) ────────────────────────────────────────────────────────
 *  • Arbiter sends SIGUSR1 to hip_pid when a player is stunned.
 *  • Signal handler calls sleep(3) — no flag polling in any player thread.
 *
 * ── Section 10 (Quit) ───────────────────────────────────────────────────────
 *  • If the player types "quit", HIP sends SIGTERM to arbiter_pid.
 *
 * Input format ──────────────────────────────────────────────────────────
 * HIP reads keyboard-based requests from the Arbiter renderer via shared
 * memory. The active player thread consumes only renderer requests for the
 * current active player and writes an action into gs->hip_action.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <pthread.h>
#include <time.h>

#include "shared_memory.h"

// ── Globals ───────────────────────────────────────────────────────────────────
static GameState*   gs         = NULL;
static volatile int hip_alive  = 1;
static int          g_hip_index = 0;

// ─────────────────────────────────────────────────────────────────────────────
// §5 — Stun handler for HIP
// Arbiter sends SIGUSR1 to hip_pid when any player is stunned.
// sleep(3) halts the receiving thread; no flag polling.
// ─────────────────────────────────────────────────────────────────────────────
static void sigusr1_handler(int)
{
    sleep(3);
}

// §10 — SIGTERM from Arbiter (game over) or our own quit → exit cleanly
static void sigterm_handler(int)
{
    hip_alive = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Attach to the shared memory segment the Arbiter already created
// ─────────────────────────────────────────────────────────────────────────────
static void attach_shm(void)
{
    int fd = -1;
    for (int i = 0; i < 20 && fd < 0; i++) {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd < 0) usleep(100000);
    }
    if (fd < 0) { perror("hip: shm_open"); exit(1); }

    gs = (GameState*)mmap(NULL, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) { perror("hip: mmap"); exit(1); }
    close(fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// Process one player request from Arbiter's renderer and write hip_action.
// PRECONDITION: It is confirmed to be pidx's turn (active_entity == pidx).
// ─────────────────────────────────────────────────────────────────────────────
static void handle_player_request(int pidx)
{
    PendingAction act;
    memset(&act, 0, sizeof(act));
    act.entity_index = pidx;

    bool enemy_alive[MAX_ENEMIES] = {0};
    sem_wait(&gs->state_mutex);
    int req_action = gs->hip_ctrl.request_action;
    int req_target = gs->hip_ctrl.request_target;
    int req_weapon = gs->hip_ctrl.request_weapon_id;
    int ne = gs->num_enemies;
    for (int i = 0; i < ne; i++) {
        Entity* enemy = &gs->entities[ENEMY_BASE_IDX + i];
        enemy_alive[i] = (enemy->active && enemy->hp > 0);
    }
    sem_post(&gs->state_mutex);

    int target = -1;
    if (req_target >= ENEMY_BASE_IDX && req_target < ENEMY_BASE_IDX + ne)
        target = req_target;
    if (target < 0) {
        for (int i = 0; i < ne; i++) {
            if (enemy_alive[i]) {
                target = ENEMY_BASE_IDX + i;
                break;
            }
        }
    }

    switch (req_action) {
    case ACTION_ATTACK:
        act.action_code  = ACTION_ATTACK;
        act.target_index = target;
        break;
    case ACTION_EXHAUST:
        act.action_code  = ACTION_EXHAUST;
        act.target_index = target;
        break;
    case ACTION_USE_WEAPON:
        act.action_code  = ACTION_USE_WEAPON;
        act.weapon_id    = req_weapon;
        act.target_index = target;
        break;
    case ACTION_SWAP_IN:
        act.action_code = ACTION_SWAP_IN;
        act.weapon_id   = req_weapon;
        break;
    case ACTION_HEAL:
        act.action_code = ACTION_HEAL;
        break;
    case ACTION_SKIP:
        act.action_code = ACTION_SKIP;
        break;
    case ACTION_ULTIMATE:
        act.action_code = ACTION_ULTIMATE;
        break;
    case ACTION_ACQUIRE_ARTIFACT:
        act.action_code = ACTION_ACQUIRE_ARTIFACT;
        act.weapon_id   = req_weapon;
        break;
    case ACTION_RELEASE_ARTIFACT:
        act.action_code = ACTION_RELEASE_ARTIFACT;
        act.weapon_id   = req_weapon;
        break;
    case ACTION_PICKUP:
        act.action_code = ACTION_PICKUP;
        act.weapon_id   = req_weapon;
        break;
    default:
        act.action_code = ACTION_SKIP;
        break;
    }

    sem_wait(&gs->state_mutex);
    if (gs->active_entity == pidx && !gs->hip_action.pending) {
        act.pending = true;
        gs->hip_action = act;
        gs->hip_ctrl.request_pending = false;
        gs->hip_ctrl.input_consumed = true;
        fprintf(stderr, "[HIP %d] Player %d request accepted: action=%d target=%d weapon=%d\n",
                g_hip_index, pidx + 1, act.action_code, act.target_index, act.weapon_id);
    } else {
        fprintf(stderr, "[HIP %d] Player %d request ignored: active_entity=%d pending=%d\n",
                g_hip_index, pidx + 1, gs->active_entity, gs->hip_action.pending);
    }
    sem_post(&gs->state_mutex);
}
// ─────────────────────────────────────────────────────────────────────────────
// Player pthread — one per human character (§2)
// Idles until the Arbiter designates this player as active_entity.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct { int player_index; } PlayerArg;

static void* player_thread(void* arg)
{
    int pidx = ((PlayerArg*)arg)->player_index;

    while (hip_alive)
    {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_nsec += 100000000L; // 100 ms
        if (timeout.tv_nsec >= 1000000000L) {
            timeout.tv_sec += 1;
            timeout.tv_nsec -= 1000000000L;
        }
        sem_timedwait(&gs->hip_input_sem[g_hip_index], &timeout);

        // ── Sample game state ─────────────────────────────────────────────
        sem_wait(&gs->state_mutex);
        bool game_on  = gs->game_running && !gs->game_over;
        bool alive    = gs->entities[pidx].active && gs->entities[pidx].hp > 0;
        bool my_turn  = (gs->hip_ctrl.active_player_idx == pidx);
        bool stunned  = gs->entities[pidx].stunned;
        bool pending  = gs->hip_action.pending;
        bool request_pending = gs->hip_ctrl.request_pending;
        sem_post(&gs->state_mutex);

        if (!game_on || !alive) break;    // game ended or this character died

        if (my_turn && !stunned && !pending && request_pending) {
            // §2: only the active player thread consumes renderer input requests.
            handle_player_request(pidx);
            usleep(50000);
        } else {
            // All other threads idle (§2 requirement)
            usleep(20000);   // light CPU while waiting
        }
    }

    return NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    // §5: stun via SIGUSR1 — handler calls sleep(3)
    signal(SIGUSR1, sigusr1_handler);

    // §10: game over or player quit
    signal(SIGTERM, sigterm_handler);

    int hip_index = 0;
    if (argc > 1) {
        hip_index = atoi(argv[1]);
        if (hip_index < 0 || hip_index >= MAX_HIP_PROCS)
            hip_index = 0;
    }
    g_hip_index = hip_index;

    attach_shm();

    // Wait for Arbiter to finish initialising
    while (!gs->game_running) usleep(10000);

    pid_t my_pid = getpid();
    sem_wait(&gs->state_mutex);
    if (hip_index >= 0 && hip_index < MAX_HIP_PROCS)
        gs->hip_pids[hip_index] = my_pid;
    fprintf(stderr, "[HIP] hip_index=%d pid=%d attached. Owned players:", hip_index, my_pid);
    for (int i = 0; i < gs->num_players; i++) {
        if (gs->player_owner[i] == hip_index) {
            fprintf(stderr, " %d", i + 1);
            gs->entities[i].pid = my_pid;
        }
    }
    fprintf(stderr, "\n");
    sem_post(&gs->state_mutex);

    int       num_players = gs->num_players;
    pthread_t threads[MAX_PLAYERS];
    PlayerArg args[MAX_PLAYERS];
    int       thread_count = 0;

    // §2: one dedicated pthread per human player character
    for (int i = 0; i < num_players; i++) {
        if (gs->player_owner[i] != hip_index)
            continue;

        args[thread_count].player_index = i;
        if (pthread_create(&threads[thread_count], NULL, player_thread, &args[thread_count]) != 0) {
            perror("hip: pthread_create");
            exit(1);
        }
        thread_count++;
    }

    if (thread_count == 0) {
        while (hip_alive) {
            usleep(100000);
        }
    } else {
        for (int i = 0; i < thread_count; i++)
            pthread_join(threads[i], NULL);
    }

    printf("[HIP] hip_index=%d exiting after %d player threads.\n", hip_index, thread_count);
    return 0;
}