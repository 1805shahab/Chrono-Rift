/*
 * ASP.cpp — Automated Strategic Process (ASP)
 *
 * Threading: pthreads only. No <atomic>, no <thread>.
 * IPC:       Shared memory + semaphores only. No pipes (§4).
 *
 * §2  One pthread per enemy (NPC) character.
 *     Only the thread for the currently active enemy writes an action.
 *     Other enemy threads idle.
 *
 * §5  SIGUSR1 = stun signal sent by Arbiter to this process.
 *     Handler calls sleep(3). Pure signal-based — ASP never polls a flag.
 *
 * §8  Ultimate Ability pause: Arbiter sends SIGSTOP to this entire process.
 *     The kernel suspends every thread. Arbiter sends SIGCONT (via its
 *     SIGALRM handler) after 10 s. ASP reads NO flag — purely OS-managed.
 *
 * NPC AI: 70% Strike, 30% Skip (replace with smarter logic as desired).
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
static GameState*   gs        = NULL;
static volatile int asp_alive = 1;

// ─────────────────────────────────────────────────────────────────────────────
// §5 — Stun handler
// Arbiter sends SIGUSR1 to this process (asp_pid) when any NPC is stunned.
// sleep(3) suspends the calling thread — no flag polling whatsoever.
// ─────────────────────────────────────────────────────────────────────────────
static void sigusr1_handler(int)
{
    sleep(3);
}

// §8 — SIGSTOP/SIGCONT are kernel-managed; cannot be caught.
//       ASP installs NO handlers for them. That is correct by design.

static void sigterm_handler(int)
{
    asp_alive = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Attach to the shared memory segment Arbiter already created
// ─────────────────────────────────────────────────────────────────────────────
static void attach_shm(void)
{
    int fd = -1;
    // Retry up to 2 s in case Arbiter is still initialising
    for (int i = 0; i < 20 && fd < 0; i++) {
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (fd < 0) usleep(100000);
    }
    if (fd < 0) { perror("asp: shm_open"); exit(1); }

    gs = (GameState*)mmap(NULL, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) { perror("asp: mmap"); exit(1); }
    close(fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pick a random alive player to target (no pipes — uses shared memory)
// ─────────────────────────────────────────────────────────────────────────────
static int pick_target(void)
{
    int target = 0;
    sem_wait(&gs->state_mutex);
    for (int attempt = 0; attempt < 20; attempt++) {
        int i = rand() % gs->num_players;
        if (gs->entities[i].active && gs->entities[i].hp > 0) {
            target = i;
            break;
        }
    }
    sem_post(&gs->state_mutex);
    return target;
}

// ─────────────────────────────────────────────────────────────────────────────
// NPC decision: "think", then write a PendingAction into shared memory.
// §4: no pipes — write directly to gs->asp_action under state_mutex.
// ─────────────────────────────────────────────────────────────────────────────
static void do_npc_turn(int eidx)
{
    // Simulate AI "thinking" (200 ms – 1 s)
    struct timespec think = { 0, 200000000L + (long)(rand() % 800000000L) };
    nanosleep(&think, NULL);

    PendingAction act;
    memset(&act, 0, sizeof(act));
    act.entity_index = eidx;

    // Simple AI: 70% Strike, 30% Skip
    if ((rand() % 100) < 70) {
        act.action_code  = ACTION_ATTACK;
        act.target_index = pick_target();
    } else {
        act.action_code = ACTION_SKIP;
    }

    // Write action — no pipes (§4)
    sem_wait(&gs->state_mutex);
    act.pending    = true;
    gs->asp_action = act;
    sem_post(&gs->state_mutex);
}

// ─────────────────────────────────────────────────────────────────────────────
// NPC pthread — one per enemy character (§2 requirement).
// Threads run concurrently; only the active one submits an action.
// Proper synchronisation: all state reads/writes under state_mutex.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct { int entity_index; } NpcArg;

static void* npc_thread(void* arg)
{
    int eidx = ((NpcArg*)arg)->entity_index;

    while (asp_alive)
    {
        // ── Read game state atomically ─────────────────────────────────────
        sem_wait(&gs->state_mutex);
        bool game_on  = gs->game_running && !gs->game_over;
        bool alive    = gs->entities[eidx].active && gs->entities[eidx].hp > 0;
        bool my_turn  = (gs->active_entity == eidx);
        bool stunned  = gs->entities[eidx].stunned;
        bool pending  = gs->asp_action.pending;   // another NPC already submitted?
        sem_post(&gs->state_mutex);

        if (!game_on || !alive) break;   // clean exit — game over or NPC dead

        if (my_turn && !stunned && !pending) {
            // This NPC's turn and no other action pending
            do_npc_turn(eidx);
            usleep(100000);   // brief pause after submitting
        } else {
            usleep(50000);    // idle at ~20 Hz
        }
    }

    return NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(void)
{
    // §5: stun via SIGUSR1 — handler calls sleep(3), no flag polling
    signal(SIGUSR1, sigusr1_handler);

    // §8: DO NOT install SIGSTOP or SIGCONT handlers — they are kernel-managed.
    //     SIGSTOP cannot be caught or ignored; SIGCONT resumes automatically.

    signal(SIGTERM, sigterm_handler);

    srand((unsigned int)time(NULL));

    attach_shm();

    // Wait for Arbiter to finish initialising shared memory
    while (!gs->game_running) usleep(10000);

    // Register this process's PID with all enemy entities.
    // The Arbiter uses entity.pid to send SIGUSR1 for the stun mechanic (§5).
    pid_t my_pid = getpid();
    sem_wait(&gs->state_mutex);
    for (int i = 0; i < gs->num_enemies; i++)
        gs->entities[ENEMY_BASE_IDX + i].pid = my_pid;
    sem_post(&gs->state_mutex);

    int       num_enemies = gs->num_enemies;
    pthread_t threads[MAX_ENEMIES];
    NpcArg    args[MAX_ENEMIES];

    // §2: one dedicated pthread per enemy character
    for (int i = 0; i < num_enemies; i++) {
        args[i].entity_index = ENEMY_BASE_IDX + i;
        if (pthread_create(&threads[i], NULL, npc_thread, &args[i]) != 0) {
            perror("asp: pthread_create");
            exit(1);
        }
    }

    for (int i = 0; i < num_enemies; i++)
        pthread_join(threads[i], NULL);

    printf("[ASP] all NPC threads finished — exiting.\n");
    return 0;
}