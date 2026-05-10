/*
 * arbiter.cpp — Game Arbiter (Central Authority)
 *
 * Threading: pthreads only. No <atomic>, no <thread>.
 * IPC:       Shared memory + unnamed semaphores only. No pipes.
 * Signals:
 *   SIGALRM  — ends Ultimate 10-s window; handler sends SIGCONT to ASP (§8)
 *              also used for NPC 3-s timeout (§8 timeout policy)
 *   SIGTERM  — player quit; sent by HIP (§10)
 *   SIGCHLD  — reap dead child processes
 * SFML window lives on the main thread (SFML requirement).
 * Scheduler and deadlock monitor run as pthreads.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <limits.h>
#include <filesystem>

#include <SFML/Graphics.hpp>
#include <string>

#include "shared_memory.h"

// ── Change to your roll number ────────────────────────────────────────────────
static const int ROLL_NUMBER = 0507;

static std::filesystem::path find_assets_dir(const std::filesystem::path& start)
{
    namespace fs = std::filesystem;
    fs::path current = start;
    while (true) {
        fs::path candidate = current / "assets";
        if (fs::exists(candidate) && fs::is_directory(candidate))
            return candidate;
        if (current == current.root_path())
            break;
        current = current.parent_path();
    }
    return fs::path();
}

static std::string resolve_asset_dir(void)
{
    namespace fs = std::filesystem;
    fs::path result;
    
    fprintf(stderr, "[Arbiter] resolve_asset_dir: cwd=%s\n", fs::current_path().c_str());
    
    result = find_assets_dir(fs::current_path());
    if (!result.empty()) {
        fprintf(stderr, "[Arbiter] resolve_asset_dir: found via cwd -> %s\n", result.c_str());
        return result.string();
    }

    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        fprintf(stderr, "[Arbiter] resolve_asset_dir: exe=%s\n", exe_path);
        fs::path exe_dir = fs::path(exe_path).parent_path();
        result = find_assets_dir(exe_dir);
        if (!result.empty()) {
            fprintf(stderr, "[Arbiter] resolve_asset_dir: found via exe dir -> %s\n", result.c_str());
            return result.string();
        }
    }

    fprintf(stderr, "[Arbiter] WARNING: asset dir not found, falling back to ./assets\n");
    return std::string("./assets");
}

static void run_sfml_render(const std::string& asset_dir);
static void* render_thread_func(void* arg);

// ── Globals ───────────────────────────────────────────────────────────────────
static int        shm_fd          = -1;
static GameState* gs              = nullptr;
static volatile sig_atomic_t g_keep_running  = 1;
static volatile sig_atomic_t g_monitor_run   = 1;
// NPC timeout deadline uses wall-clock time to avoid alarm collision with Ultimate.
static time_t g_npc_timeout_deadline = 0;
// Ultimate mode: set to 1 while ASP is SIGSTOPped
static volatile sig_atomic_t g_ultimate_mode = 0;
static volatile sig_atomic_t g_terminate_requested = 0;
static std::string g_asset_dir = "";

// ── Forward declarations ──────────────────────────────────────────────────────
static void log_locked(const char* msg);
static void log_safe(const char* msg);

// ─────────────────────────────────────────────────────────────────────────────
// Signal handlers
// ─────────────────────────────────────────────────────────────────────────────

// §8 — SIGALRM serves dual purpose:
//   1. Ultimate window end → send SIGCONT to ASP
//   2. NPC turn timeout   → force skip
static void sigalrm_handler(int)
{
    if (g_ultimate_mode) {
        // End of 10-s Ultimate window
        g_ultimate_mode = 0;
        if (gs && gs->asp_pid > 0)
            kill(gs->asp_pid, SIGCONT);
        log_safe("[Arbiter] SIGALRM: sent SIGCONT to ASP - Ultimate window ended");
    }
}

// §10 — HIP sends SIGTERM when player quits
static void sigterm_handler(int)
{
    g_keep_running = 0;
    g_monitor_run  = 0;
    g_terminate_requested = 1;
    if (gs) {
        gs->game_running = false;
        gs->game_over = true;
    }
}

// Reap dead children
static void sigchld_handler(int)
{
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

// ─────────────────────────────────────────────────────────────────────────────
// Logging helpers
// ─────────────────────────────────────────────────────────────────────────────
static void log_locked(const char* msg)
{
    ActionLog* L = &gs->log;
    int idx = (L->head + L->count) % MAX_LOG_ENTRIES;
    strncpy(L->entries[idx], msg, LOG_ENTRY_LEN - 1);
    L->entries[idx][LOG_ENTRY_LEN - 1] = '\0';
    if (L->count < MAX_LOG_ENTRIES)
        L->count++;
    else
        L->head = (L->head + 1) % MAX_LOG_ENTRIES;
}

static void log_safe(const char* msg)
{
    sem_wait(&gs->state_mutex);
    log_locked(msg);
    sem_post(&gs->state_mutex);
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared memory
// ─────────────────────────────────────────────────────────────────────────────
static void init_shared_memory(void)
{
    shm_unlink(SHM_NAME);

    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) { perror("shm_open"); exit(1); }

    if (ftruncate(shm_fd, sizeof(GameState)) < 0) { perror("ftruncate"); exit(1); }

    gs = (GameState*)mmap(NULL, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (gs == MAP_FAILED) { perror("mmap"); exit(1); }

    memset(gs, 0, sizeof(GameState));

    sem_init(&gs->state_mutex,    1, 1);
    sem_init(&gs->artifact_mutex, 1, 1);
    for (int i = 0; i < MAX_HIP_PROCS; i++)
        sem_init(&gs->hip_input_sem[i], 1, 0);

    gs->arbiter_pid   = getpid();
    for (int i = 0; i < MAX_HIP_PROCS; i++) {
        gs->hip_pids[i] = -1;
    }
    gs->hip_process_count = 1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gs->player_owner[i] = -1;
    }
    gs->game_running  = true;
    gs->active_entity = -1;
    gs->enemy_goal    = 10;
    gs->enemies_spawned = 0;
    gs->enemies_killed = 0;
    gs->drop_available = false;
    gs->dropped_weapon_id = WPN_NONE;
    gs->drop_expires = 0;

    gs->solar_core    = {-1, -1, true};
    gs->lunar_blade   = {-1, -1, true};
    gs->eclipse_relic = {-1, -1, false};
    gs->hip_ctrl.active_player_idx = -1;
    gs->hip_ctrl.input_consumed = true;
}

static void destroy_shared_memory(void)
{
    if (gs) {
        sem_destroy(&gs->state_mutex);
        sem_destroy(&gs->artifact_mutex);
        for (int i = 0; i < MAX_HIP_PROCS; i++)
            sem_destroy(&gs->hip_input_sem[i]);
        munmap(gs, sizeof(GameState));
        gs = nullptr;
    }
    if (shm_fd >= 0) { close(shm_fd); shm_fd = -1; }
    shm_unlink(SHM_NAME);
}

// ─────────────────────────────────────────────────────────────────────────────
// Entity initialisation
// ─────────────────────────────────────────────────────────────────────────────
static int prompt_party_size(void)
{
    int np = 0;
    char buf[32];

    printf("Choose party size (1-4): ");
    fflush(stdout);
    while (fgets(buf, sizeof(buf), stdin)) {
        np = atoi(buf);
        if (np >= 1 && np <= 4)
            break;
        printf("Invalid entry. Enter a number between 1 and 4: ");
        fflush(stdout);
    }
    if (np < 1 || np > 4)
        np = 2;
    return np;
}

static int prompt_hip_process_count(void)
{
    int count = 0;
    char buf[32];

    printf("Choose human process mode (1=single HIP, 2=two HIP processes): ");
    fflush(stdout);
    while (fgets(buf, sizeof(buf), stdin)) {
        count = atoi(buf);
        if (count == 1 || count == 2)
            break;
        printf("Invalid entry. Enter 1 or 2: ");
        fflush(stdout);
    }
    if (count != 1 && count != 2)
        count = 1;
    return count;
}

static void setup_player_ownership(int num_players, int hip_proc_count)
{
    if (hip_proc_count < 2 || num_players < 2) {
        for (int i = 0; i < num_players; i++)
            gs->player_owner[i] = 0;
        for (int i = num_players; i < MAX_PLAYERS; i++)
            gs->player_owner[i] = -1;
        gs->hip_process_count = 1;
        return;
    }

    int split = (num_players + 1) / 2;
    for (int i = 0; i < num_players; i++) {
        gs->player_owner[i] = (i < split) ? 0 : 1;
    }
    for (int i = num_players; i < MAX_PLAYERS; i++)
        gs->player_owner[i] = -1;
    gs->hip_process_count = 2;
}

static void init_entities(int num_players)
{
    srand(time(NULL));
    fprintf(stderr, "[DEBUG] Initializing game with %d players...\n", num_players);

    int roll_last2    = ROLL_NUMBER % 100;
    int roll_last1    = ROLL_NUMBER % 10;
    int roll_2nd_last = (ROLL_NUMBER / 10) % 10;

    gs->num_players = num_players;
    gs->enemy_goal = 10;
    gs->enemies_spawned = 0;

    gs->num_enemies = 2 + (rand() % 8);
    fprintf(stderr, "[DEBUG] Spawning %d enemies (goal: %d kills)\n", gs->num_enemies, gs->enemy_goal);

    auto spawn_enemy = [&](int slot_idx, int enemy_number) {
        Entity* e = &gs->entities[ENEMY_BASE_IDX + slot_idx];
        memset(e, 0, sizeof(*e));
        e->active    = true;
        e->is_player = false;
        e->index     = slot_idx;
        snprintf(e->name, sizeof(e->name), "Enemy %d", enemy_number);

        e->hp          = roll_last2 + 50 + rand() % 151;
        e->max_hp      = e->hp;
        e->damage      = roll_2nd_last + 10;
        e->speed       = 10 + rand() % 21;
        e->max_stamina = 150.0f;
        e->stamina     = 0.0f;
        e->anim_phase  = (float)(rand() % 628) / 100.0f;
        // Split enemies into two columns
        int half = (gs->num_enemies + 1) / 2;
        float x_col = (slot_idx < half) ? 650.0f : 850.0f;
        int y_idx = (slot_idx < half) ? slot_idx : (slot_idx - half);
        float y_pos = 150.0f + y_idx * 110.0f;
        e->pos_x       = x_col;
        e->pos_y       = y_pos;
        e->stunned = false;
        e->stun_end = 0;
    };

    for (int i = 0; i < num_players; i++) {
        Entity* e   = &gs->entities[i];
        memset(e, 0, sizeof(*e));
        e->active    = true;
        e->is_player = true;
        e->index     = i;
        snprintf(e->name, sizeof(e->name), "Player %d", i + 1);

        e->hp          = ROLL_NUMBER + 100 + rand() % 901;
        e->max_hp      = e->hp;
        e->damage      = roll_last1 + 10;
        e->speed       = 100 / num_players;
        fprintf(stderr, "[DEBUG] %s spawned: HP=%d, DMG=%d, Speed=%d\n", e->name, e->hp, e->damage, e->speed);
        e->max_stamina = 100.0f;
        e->stamina     = 0.0f;
        e->anim_phase  = (float)(rand() % 628) / 100.0f;
        e->pos_x       = 220.0f;
        e->pos_y       = 180.0f + (i + 1) * (480.0f / (num_players + 1));
        e->stunned = false;
        e->stun_end = 0;

        // Initialise inventory to empty
        for (int s = 0; s < INVENTORY_SLOTS; s++) {
            e->inventory[s].weapon_id = WPN_NONE;
            e->inventory[s].is_start  = false;
        }
        e->long_storage_count = 0;
    }

    for (int i = 0; i < gs->num_enemies; i++) {
        spawn_enemy(i, ++gs->enemies_spawned);
        fprintf(stderr, "[DEBUG] Enemy %d spawned\n", gs->enemies_spawned);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §6 — Inventory space allocator
//
// Finds the first run of contiguous free slots large enough for `size`.
// Returns the starting slot index, or -1 if none found.
// PRECONDITION: caller holds state_mutex.
// ─────────────────────────────────────────────────────────────────────────────
static int find_free_run(Entity* e, int size)
{
    int run_start = -1;
    int run_len   = 0;

    for (int s = 0; s < INVENTORY_SLOTS; s++) {
        if (e->inventory[s].weapon_id == WPN_NONE) {
            if (run_start < 0) run_start = s;
            run_len++;
            if (run_len >= size) return run_start;
        } else {
            run_start = -1;
            run_len   = 0;
        }
    }
    return -1;
}

// Place weapon into inventory starting at `slot`.
// PRECONDITION: caller holds state_mutex; slots are free.
static void place_weapon(Entity* e, int weapon_id, int slot)
{
    int sz = WEAPON_TABLE[weapon_id].slot_size;
    for (int s = slot; s < slot + sz && s < INVENTORY_SLOTS; s++) {
        e->inventory[s].weapon_id = weapon_id;
        e->inventory[s].is_start  = (s == slot);
    }
}

// Remove weapon whose block starts at `slot`; return its weapon_id.
// PRECONDITION: caller holds state_mutex.
static int remove_weapon_at(Entity* e, int slot)
{
    int wid = e->inventory[slot].weapon_id;
    if (wid == WPN_NONE) return WPN_NONE;
    int sz = WEAPON_TABLE[wid].slot_size;
    for (int s = slot; s < slot + sz && s < INVENTORY_SLOTS; s++) {
        e->inventory[s].weapon_id = WPN_NONE;
        e->inventory[s].is_start  = false;
    }
    return wid;
}

// §6 — Add weapon to player inventory.
// If no contiguous run exists, evict just enough weapons to long-term storage.
// PRECONDITION: caller holds state_mutex.
static bool inventory_add(Entity* e, int weapon_id, char* log_buf, int log_sz)
{
    int sz = WEAPON_TABLE[weapon_id].slot_size;

    // 1. Try a direct fit
    int slot = find_free_run(e, sz);
    if (slot >= 0) {
        place_weapon(e, weapon_id, slot);
        snprintf(log_buf, log_sz, "%s: picked up %s at slot %d",
                 e->name, WEAPON_TABLE[weapon_id].name, slot);
        return true;
    }

    // 2. Need to evict — find weapon starts and evict from the end until we have room.
    //    "Only swap out as many weapons as are necessary." (§6)
    for (int s = INVENTORY_SLOTS - 1; s >= 0; s--) {
        if (!e->inventory[s].is_start) continue;
        if (e->long_storage_count >= MAX_LONG_STORAGE) {
            snprintf(log_buf, log_sz, "%s: long-term storage full! Cannot add %s",
                     e->name, WEAPON_TABLE[weapon_id].name);
            return false;
        }
        int evicted = remove_weapon_at(e, s);
        e->long_storage[e->long_storage_count++] = evicted;

        slot = find_free_run(e, sz);
        if (slot >= 0) {
            place_weapon(e, weapon_id, slot);
            snprintf(log_buf, log_sz,
                     "%s: evicted %s to storage; placed %s at slot %d",
                     e->name, WEAPON_TABLE[evicted].name,
                     WEAPON_TABLE[weapon_id].name, slot);
            return true;
        }
    }

    snprintf(log_buf, log_sz, "%s: no space for %s even after eviction",
             e->name, WEAPON_TABLE[weapon_id].name);
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// §6 — Swap-in: bring weapon_id from long storage to primary inventory.
// PRECONDITION: caller holds state_mutex.
// ─────────────────────────────────────────────────────────────────────────────
static bool inventory_swap_in(Entity* e, int weapon_id, char* log_buf, int log_sz)
{
    // Remove from long storage
    int found = -1;
    for (int i = 0; i < e->long_storage_count; i++) {
        if (e->long_storage[i] == weapon_id) { found = i; break; }
    }
    if (found < 0) {
        snprintf(log_buf, log_sz, "%s: weapon %s not in long storage",
                 e->name, WEAPON_TABLE[weapon_id].name);
        return false;
    }
    // Remove from storage array
    for (int i = found; i < e->long_storage_count - 1; i++)
        e->long_storage[i] = e->long_storage[i + 1];
    e->long_storage_count--;

    // Now add to primary inventory (may trigger further evictions)
    return inventory_add(e, weapon_id, log_buf, log_sz);
}

static Artifact* artifact_for_weapon(int weapon_id)
{
    if (weapon_id == WPN_SOLAR_CORE) return &gs->solar_core;
    if (weapon_id == WPN_LUNAR_BLADE) return &gs->lunar_blade;
    if (weapon_id == WPN_ECLIPSE_RELIC) return &gs->eclipse_relic;
    return NULL;
}

static bool entity_has_weapon(Entity* e, int weapon_id)
{
    for (int s = 0; s < INVENTORY_SLOTS; s++) {
        if (e->inventory[s].is_start && e->inventory[s].weapon_id == weapon_id)
            return true;
    }
    return false;
}

static bool remove_artifact_from_entity(Entity* e, int weapon_id)
{
    for (int s = 0; s < INVENTORY_SLOTS; s++) {
        if (e->inventory[s].is_start && e->inventory[s].weapon_id == weapon_id) {
            remove_weapon_at(e, s);
            return true;
        }
    }
    return false;
}

static bool grant_artifact_to_waiter(Artifact* art, int weapon_id,
                                     char* log_buf, int log_sz)
{
    if (!art || art->waiter < 0 || art->holder >= 0)
        return false;

    Entity* waiter = &gs->entities[art->waiter];
    if (!waiter->active || waiter->hp <= 0) {
        art->waiter = -1;
        return false;
    }

    if (entity_has_weapon(waiter, weapon_id)) {
        art->holder = art->waiter;
        art->waiter = -1;
        snprintf(log_buf, log_sz,
                 "%s already has %s; granting ownership.",
                 waiter->name, WEAPON_TABLE[weapon_id].name);
        return true;
    }

    if (!inventory_add(waiter, weapon_id, log_buf, log_sz))
        return false;

    art->holder = art->waiter;
    art->waiter = -1;
    return true;
}

static bool acquire_artifact(Entity* actor, Artifact* art,
                             int weapon_id, char* log_buf, int log_sz)
{
    if (!art || weapon_id == WPN_NONE) {
        snprintf(log_buf, log_sz, "%s: invalid artifact request.", actor->name);
        return false;
    }
    if (!art->present) {
        snprintf(log_buf, log_sz, "%s: %s is not available.",
                 actor->name, WEAPON_TABLE[weapon_id].name);
        return false;
    }
    if (art->holder == actor->index) {
        snprintf(log_buf, log_sz, "%s already holds %s.",
                 actor->name, WEAPON_TABLE[weapon_id].name);
        return false;
    }
    if (art->holder < 0) {
        if (!inventory_add(actor, weapon_id, log_buf, log_sz))
            return false;
        art->holder = actor->index;
        art->waiter = -1;
        return true;
    }
    if (art->waiter == actor->index) {
        snprintf(log_buf, log_sz, "%s is already waiting for %s.",
                 actor->name, WEAPON_TABLE[weapon_id].name);
        return false;
    }
    if (art->waiter >= 0) {
        snprintf(log_buf, log_sz,
                 "%s: %s already has a waiting request.",
                 actor->name, WEAPON_TABLE[weapon_id].name);
        return false;
    }
    art->waiter = actor->index;
    snprintf(log_buf, log_sz,
             "%s is waiting for %s held by %s.",
             actor->name, WEAPON_TABLE[weapon_id].name,
             gs->entities[art->holder].name);
    return true;
}

static bool release_artifact(Entity* actor, Artifact* art,
                             int weapon_id, char* log_buf, int log_sz,
                             bool forced = false)
{
    if (!art || weapon_id == WPN_NONE) {
        snprintf(log_buf, log_sz, "%s: invalid artifact release.", actor->name);
        return false;
    }

    if (!forced && art->holder != actor->index) {
        snprintf(log_buf, log_sz, "%s does not hold %s.",
                 actor->name, WEAPON_TABLE[weapon_id].name);
        return false;
    }

    if (art->holder >= 0) {
        Entity* holder = &gs->entities[art->holder];
        remove_artifact_from_entity(holder, weapon_id);
        art->holder = -1;
    }

    if (grant_artifact_to_waiter(art, weapon_id, log_buf, log_sz)) {
        return true;
    }

    if (forced) {
        snprintf(log_buf, log_sz, "%s forced release of %s.",
                 actor->name, WEAPON_TABLE[weapon_id].name);
    } else {
        snprintf(log_buf, log_sz, "%s released %s.",
                 actor->name, WEAPON_TABLE[weapon_id].name);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fork HIP and ASP
// ─────────────────────────────────────────────────────────────────────────────
static void spawn_processes(void)
{
    for (int i = 0; i < gs->hip_process_count; i++) {
        char arg[16];
        snprintf(arg, sizeof(arg), "%d", i);
        pid_t hip = fork();
        if (hip == 0) {
            execl("./hips", "./hips", arg, NULL);
            perror("execl hips");
            exit(1);
        }
        gs->hip_pids[i] = hip;
    }

    pid_t asp = fork();
    if (asp == 0) { execl("./asps", "./asps", NULL); perror("execl asps"); exit(1); }
    gs->asp_pid = asp;
}

// ─────────────────────────────────────────────────────────────────────────────
// Apply action — called inside scheduler while holding state_mutex
// ─────────────────────────────────────────────────────────────────────────────
static void apply_action_locked(PendingAction* act)
{
    Entity* actor = &gs->entities[act->entity_index];
    char    buf[256];  // Increased to prevent truncation warning

    auto check_death = [&](Entity* tgt) {
        if (tgt->hp <= 0 && tgt->active) {
            tgt->active = false;
            fprintf(stderr, "[DEBUG] %s defeated (HP: %d)\n", tgt->name, tgt->hp);
            if (!tgt->is_player) {
                gs->enemies_killed++;
                fprintf(stderr, "[DEBUG] Enemy kills: %d/%d\n", gs->enemies_killed, gs->enemy_goal);
                char kbuf[LOG_ENTRY_LEN];
                snprintf(kbuf, sizeof(kbuf), "%s defeated! Kills: %d/%d",
                         tgt->name, gs->enemies_killed, gs->enemy_goal);
                log_locked(kbuf);

                if (!gs->eclipse_relic.present && gs->enemies_killed >= 3) {
                    gs->eclipse_relic.present = true;
                    // Make Eclipse Relic appear as a weapon drop
                    if (!gs->drop_available) {
                        gs->drop_available = true;
                        gs->dropped_weapon_id = WPN_ECLIPSE_RELIC;
                        gs->drop_expires = time(NULL) + 15;
                        log_locked("The Eclipse Relic appears as a weapon drop!");
                    }
                }

                if (!gs->drop_available && (rand() % 100) < 35) {
                    int choices[] = { WPN_IRON_HALBERD, WPN_VENOM_DAGGER,
                                      WPN_THUNDERSTAFF, WPN_OBSIDIAN_AXE,
                                      WPN_FROSTBOW, WPN_SPLINTER };
                    int r = rand() % (sizeof(choices) / sizeof(choices[0]));
                    gs->drop_available = true;
                    gs->dropped_weapon_id = choices[r];
                    gs->drop_expires = time(NULL) + 15;
                    fprintf(stderr, "[DEBUG] Weapon drop: %s drops %s\n", tgt->name, WEAPON_TABLE[choices[r]].name);
                    char drop_buf[LOG_ENTRY_LEN];
                    snprintf(drop_buf, sizeof(drop_buf), "%s drops %s! Press P to pickup.",
                             tgt->name, WEAPON_TABLE[choices[r]].name);
                    log_locked(drop_buf);
                }

                if (gs->enemies_spawned < gs->enemy_goal) {
                    int next_enemy = ++gs->enemies_spawned;
                    int slot_idx = tgt->index;
                    fprintf(stderr, "[DEBUG] Respawning enemy slot %d as Enemy %d\n", slot_idx, next_enemy);
                    memset(tgt, 0, sizeof(*tgt));
                    tgt->active    = true;
                    tgt->is_player = false;
                    tgt->index     = slot_idx;
                    snprintf(tgt->name, sizeof(tgt->name), "Enemy %d", next_enemy);
                    tgt->hp          = 60 + rand() % 141;
                    tgt->max_hp      = tgt->hp;
                    tgt->damage      = 12 + rand() % 16;
                    tgt->speed       = 10 + rand() % 21;
                    tgt->max_stamina = 150.0f;
                    tgt->stamina     = 0.0f;
                    tgt->anim_phase  = (float)(rand() % 628) / 100.0f;
                    // Split enemies into two columns
                    int half = (gs->num_enemies + 1) / 2;
                    float x_col = (slot_idx < half) ? 650.0f : 850.0f;
                    int y_idx = (slot_idx < half) ? slot_idx : (slot_idx - half);
                    float y_pos = 150.0f + y_idx * 110.0f;
                    tgt->pos_x       = x_col;
                    tgt->pos_y       = y_pos;
                    tgt->stunned = false;
                    tgt->stun_end = 0;
                    char respawn_buf[LOG_ENTRY_LEN];
                    snprintf(respawn_buf, sizeof(respawn_buf), "%s enters the fray!", tgt->name);
                    log_locked(respawn_buf);
                }
            } else {
                char kbuf[LOG_ENTRY_LEN];
                snprintf(kbuf, sizeof(kbuf), "%s has fallen!", tgt->name);
                log_locked(kbuf);
            }
        }
    };

    switch (act->action_code)
    {
    // ── Strike: reduce enemy HP ───────────────────────────────────────────
    case ACTION_ATTACK:
    {
        Entity* tgt = &gs->entities[act->target_index];
        tgt->hp    -= actor->damage;
        if (tgt->hp < 0) tgt->hp = 0;
        check_death(tgt);
        actor->stamina = 0;
        snprintf(buf, sizeof(buf), "%s strikes %s for %d dmg",
                 actor->name, tgt->name, actor->damage);
        break;
    }
    // ── Exhaust: drain enemy stamina ──────────────────────────────────────
    case ACTION_EXHAUST:
    {
        Entity* tgt   = &gs->entities[act->target_index];
        tgt->stamina -= (float)actor->damage;
        if (tgt->stamina < 0) tgt->stamina = 0;
        actor->stamina = 0;
        snprintf(buf, sizeof(buf), "%s exhausts %s for %d stamina",
                 actor->name, tgt->name, actor->damage);
        break;
    }
    // ── Use Weapon: attack with inventory weapon ───────────────────────────
    case ACTION_USE_WEAPON:
    {
        // Verify weapon is in inventory
        int wid   = act->weapon_id;
        bool held = false;
        for (int s = 0; s < INVENTORY_SLOTS; s++) {
            if (actor->inventory[s].is_start &&
                actor->inventory[s].weapon_id == wid) { held = true; break; }
        }
        if (!held || wid == WPN_NONE) {
            snprintf(buf, sizeof(buf), "%s: weapon not in inventory!", actor->name);
            actor->stamina = 0;
            break;
        }
        Entity* tgt = &gs->entities[act->target_index];
        int dmg     = WEAPON_TABLE[wid].damage;
        tgt->hp    -= dmg;
        if (tgt->hp < 0) tgt->hp = 0;
        check_death(tgt);
        actor->stamina = 0;
        snprintf(buf, sizeof(buf), "%s uses %s on %s for %d dmg",
                 actor->name, WEAPON_TABLE[wid].name, tgt->name, dmg);
        break;
    }
    // ── Swap In: retrieve weapon from long-term storage (§6) ──────────────
    case ACTION_SWAP_IN:
    {
        char swap_log[LOG_ENTRY_LEN];
        inventory_swap_in(actor, act->weapon_id, swap_log, sizeof(swap_log));
        actor->stamina = 0;   // costs a full turn; weapon usable next turn
        snprintf(buf, sizeof(buf), "SwapIn: %s", swap_log);
        break;
    }
    // ── Pickup dropped weapon (§6) ────────────────────────────────────────
    case ACTION_PICKUP:
    {
        int wid = act->weapon_id;
        if (!gs->drop_available || wid == WPN_NONE) {
            snprintf(buf, sizeof(buf), "%s: no weapon to pick up.", actor->name);
            actor->stamina = 0;
            break;
        }
        if (inventory_add(actor, wid, buf, sizeof(buf))) {
            gs->drop_available = false;
            gs->dropped_weapon_id = WPN_NONE;
            gs->drop_expires = 0;
            snprintf(buf, sizeof(buf), "%s picks up %s.", actor->name, WEAPON_TABLE[wid].name);
        } else {
            snprintf(buf, sizeof(buf), "%s failed to pick up %s.", actor->name, WEAPON_TABLE[wid].name);
        }
        actor->stamina = 0;
        break;
    }
    // ── Stun target via SIGUSR1 (§5) ──────────────────────────────────────
    case ACTION_STUN:
    {
        Entity* tgt = &gs->entities[act->target_index];
        if (!tgt->active || tgt->hp <= 0) {
            snprintf(buf, sizeof(buf), "%s: invalid stun target.", actor->name);
            actor->stamina = 0;
            break;
        }
        if (tgt->stunned) {
            snprintf(buf, sizeof(buf), "%s is already stunned.", tgt->name);
            actor->stamina = 0;
            break;
        }
        tgt->stunned = true;
        tgt->stun_end = time(NULL) + 3;
        if (tgt->pid > 0)
            kill(tgt->pid, SIGUSR1);
        actor->stamina = 0;
        snprintf(buf, sizeof(buf), "%s stuns %s for 3s.", actor->name, tgt->name);
        break;
    }
    // ── Heal ──────────────────────────────────────────────────────────────
    case ACTION_HEAL:
        actor->hp += (int)(actor->max_hp * 0.10f);
        if (actor->hp > actor->max_hp) actor->hp = actor->max_hp;
        actor->stamina = 0;
        snprintf(buf, sizeof(buf), "%s heals -> %d HP", actor->name, actor->hp);
        break;

    // ── Skip ──────────────────────────────────────────────────────────────
    case ACTION_SKIP:
        actor->stamina = actor->max_stamina * 0.50f;
        snprintf(buf, sizeof(buf), "%s skips (stamina -> 50%%)", actor->name);
        break;

    // ── Acquire Artifact (§7) ──────────────────────────────────────────────
    case ACTION_ACQUIRE_ARTIFACT:
    {
        Artifact* art = artifact_for_weapon(act->weapon_id);
        sem_wait(&gs->artifact_mutex);
        if (!acquire_artifact(actor, art, act->weapon_id, buf, sizeof(buf))) {
            actor->stamina = 0;
        } else {
            actor->stamina = 0;
        }
        sem_post(&gs->artifact_mutex);
        break;
    }

    // ── Release Artifact (§7) ──────────────────────────────────────────────
    case ACTION_RELEASE_ARTIFACT:
    {
        Artifact* art = artifact_for_weapon(act->weapon_id);
        sem_wait(&gs->artifact_mutex);
        if (!release_artifact(actor, art, act->weapon_id, buf, sizeof(buf))) {
            actor->stamina = 0;
        } else {
            actor->stamina = 0;
        }
        sem_post(&gs->artifact_mutex);
        break;
    }

    // ── Ultimate Ability (§8) ─────────────────────────────────────────────
    case ACTION_ULTIMATE:
    {
        bool has_solar = false, has_lunar = false;
        for (int s = 0; s < INVENTORY_SLOTS; s++) {
            if (!actor->inventory[s].is_start) continue;
            if (actor->inventory[s].weapon_id == WPN_SOLAR_CORE) has_solar = true;
            if (actor->inventory[s].weapon_id == WPN_LUNAR_BLADE) has_lunar = true;
        }
        if (!has_solar || !has_lunar) {
            snprintf(buf, sizeof(buf),
                     "%s tried Ultimate - needs Solar Core + Lunar Blade!", actor->name);
            actor->stamina = 0;
            break;
        }
        actor->stamina  = 0;
        g_ultimate_mode = 1;
        // §8: SIGSTOP suspends ASP entirely — no flag in ASP
        if (gs->asp_pid > 0)
            kill(gs->asp_pid, SIGSTOP);
        // SIGALRM fires after 10 s; handler sends SIGCONT (§8)
        alarm(10);
        snprintf(buf, sizeof(buf),
                 "%s ULTIMATE! ASP suspended 10s (SIGSTOP). SIGCONT via SIGALRM.", actor->name);
        break;
    }
    // ── Quit ─────────────────────────────────────────────────────────────
    case ACTION_QUIT:
        gs->game_running = false;
        gs->game_over    = true;
        g_keep_running   = 0;
        snprintf(buf, sizeof(buf), "Game quit by player.");
        break;

    default:
        actor->stamina = 0;
        snprintf(buf, sizeof(buf), "%s: unknown action %d", actor->name, act->action_code);
        break;
    }

    log_locked(buf);
    act->pending = false;
}

static int first_alive_enemy(void)
{
    for (int i = 0; i < gs->num_enemies; i++) {
        Entity* e = &gs->entities[ENEMY_BASE_IDX + i];
        if (e->active && e->hp > 0)
            return ENEMY_BASE_IDX + i;
    }
    return -1;
}

static int first_inventory_weapon(Entity* e)
{
    for (int s = 0; s < INVENTORY_SLOTS; s++) {
        if (e->inventory[s].is_start && e->inventory[s].weapon_id != WPN_NONE)
            return e->inventory[s].weapon_id;
    }
    return WPN_NONE;
}

static int first_long_storage_weapon(Entity* e)
{
    if (e->long_storage_count > 0)
        return e->long_storage[0];
    return WPN_NONE;
}

static int first_held_artifact(Entity* e)
{
    bool has_solar = false;
    bool has_lunar = false;
    for (int s = 0; s < INVENTORY_SLOTS; s++) {
        if (!e->inventory[s].is_start) continue;
        if (e->inventory[s].weapon_id == WPN_SOLAR_CORE) has_solar = true;
        if (e->inventory[s].weapon_id == WPN_LUNAR_BLADE) has_lunar = true;
    }
    if (has_solar) return WPN_SOLAR_CORE;
    if (has_lunar) return WPN_LUNAR_BLADE;
    return WPN_NONE;
}

static int first_free_artifact(void)
{
    if (gs->solar_core.holder < 0 && gs->solar_core.present)
        return WPN_SOLAR_CORE;
    if (gs->lunar_blade.holder < 0 && gs->lunar_blade.present)
        return WPN_LUNAR_BLADE;
    return WPN_NONE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scheduler pthread (§3)
// ─────────────────────────────────────────────────────────────────────────────
static void* scheduler_thread(void* arg)
{
    (void)arg;
    struct timespec tick = {0, 100000000L};   // 100 ms

    while (g_keep_running)
    {
        nanosleep(&tick, NULL);

        sem_wait(&gs->state_mutex);

        if (!gs->game_running || gs->game_over) {
            sem_post(&gs->state_mutex);
            break;
        }

        int total = gs->num_players + gs->num_enemies;

        // 1. Accumulate stamina (§3)
        int   first_ready = -1;

        for (int i = 0; i < total; i++) {
            Entity* e = &gs->entities[i];
            if (!e->active || e->hp <= 0) continue;
            if (e->stunned && time(NULL) >= e->stun_end) {
                e->stunned = false;
                e->stun_end = 0;
            }
            if (e->stunned) continue;

            e->stamina += (float)e->speed * 0.1f;
            if (e->stamina > e->max_stamina) e->stamina = e->max_stamina;

            if (e->stamina >= e->max_stamina) {
                if (first_ready < 0 || i < first_ready) {
                    first_ready = i;
                }
            }
            e->anim_phase += 0.12f;
            if (e->anim_phase > 6.28f) e->anim_phase -= 6.28f;
        }

        // 2. Assign turn (serial execution, §3)
        if (first_ready >= 0 && gs->active_entity < 0) {
            gs->active_entity = first_ready;
            Entity* actor = &gs->entities[first_ready];

            if (actor->is_player) {
                int owner = gs->player_owner[first_ready];
                if (owner < 0 || owner >= MAX_HIP_PROCS)
                    owner = 0;

                fprintf(stderr, "[SCHED] Player %s assigned turn -> HIP %d (speed=%d, stamina=%.1f/%.1f)\n",
                        actor->name, owner, actor->speed, actor->stamina, actor->max_stamina);
                gs->hip_ctrl.active_player_idx = first_ready;
                gs->hip_ctrl.input_consumed     = false;
                gs->hip_ctrl.request_pending    = false;
                gs->hip_ctrl.request_action     = ACTION_NONE;
                gs->hip_ctrl.request_weapon_id  = WPN_NONE;
                gs->hip_ctrl.request_target     = -1;
                fprintf(stderr, "[SCHED] Signaling HIP %d for player %s (idx=%d)\n",
                        owner, actor->name, first_ready + 1);
                sem_post(&gs->hip_input_sem[owner]);
            } else {
                fprintf(stderr, "[SCHED] Enemy %s assigned turn (speed=%d, stamina=%.1f/%.1f)\n",
                        actor->name, actor->speed, actor->stamina, actor->max_stamina);
                gs->hip_ctrl.active_player_idx = -1;
                gs->hip_ctrl.request_pending = false;
            }

            char tbuf[LOG_ENTRY_LEN];
            snprintf(tbuf, sizeof(tbuf), "[Sched] %s's turn",
                     actor->name);
            log_locked(tbuf);

            // §8 NPC timeout: use deadline timer instead of alarm to avoid overlapping
            if (!actor->is_player) {
                g_npc_timeout_deadline = time(NULL) + 3;
            }
        }

        // 3. Process submitted action or handle timeout
        if (gs->active_entity >= 0) {
            Entity* actor = &gs->entities[gs->active_entity];
            PendingAction* act = nullptr;

            if (actor->is_player &&
                gs->hip_action.pending &&
                gs->hip_action.entity_index == gs->active_entity)
            {
                act = &gs->hip_action;
            }
            else if (!actor->is_player &&
                     gs->asp_action.pending &&
                     gs->asp_action.entity_index == gs->active_entity)
            {
                act = &gs->asp_action;
                g_npc_timeout_deadline = 0;
            }

            // §8: NPC timeout — force skip via wall-clock deadline
            if (!act && !actor->is_player && g_npc_timeout_deadline > 0 &&
                time(NULL) >= g_npc_timeout_deadline)
            {
                g_npc_timeout_deadline = 0;
                char fbuf[LOG_ENTRY_LEN];
                snprintf(fbuf, sizeof(fbuf), "[Arbiter] NPC %s timed out - forced Skip",
                         actor->name);
                log_locked(fbuf);
                actor->stamina = actor->max_stamina * 0.5f;
                gs->active_entity = -1;
            }
            else if (act) {
                fprintf(stderr, "[SCHED] %s executing action %d for entity idx %d\n",
                        actor->name, act->action_code, act->entity_index);
                apply_action_locked(act);
                if (act == &gs->hip_action) {
                    gs->hip_ctrl.active_player_idx = -1;
                    gs->hip_ctrl.request_pending = false;
                    gs->hip_ctrl.input_consumed = true;
                }
                gs->active_entity = -1;
            }
        }

        // 4. Check defeat and kill count
        bool all_players_dead = true;
        for (int i = 0; i < gs->num_players; i++) {
            if (gs->entities[i].active && gs->entities[i].hp > 0) {
                all_players_dead = false;
                break;
            }
        }

        int active_enemy_count = 0;
        for (int i = 0; i < gs->num_enemies; i++) {
            Entity* e = &gs->entities[ENEMY_BASE_IDX + i];
            if (e->active && e->hp > 0)
                active_enemy_count++;
        }

        if (gs->drop_available && time(NULL) >= gs->drop_expires) {
            int enemy_idx = -1;
            for (int i = 0; i < gs->num_enemies; i++) {
                Entity* e = &gs->entities[ENEMY_BASE_IDX + i];
                if (e->active && e->hp > 0) {
                    enemy_idx = ENEMY_BASE_IDX + i;
                    break;
                }
            }
            if (enemy_idx >= 0) {
                char drop_msg[LOG_ENTRY_LEN];
                if (inventory_add(&gs->entities[enemy_idx], gs->dropped_weapon_id, drop_msg, sizeof(drop_msg))) {
                    snprintf(drop_msg, sizeof(drop_msg), "%s picked up dropped %s.",
                             gs->entities[enemy_idx].name,
                             WEAPON_TABLE[gs->dropped_weapon_id].name);
                } else {
                    snprintf(drop_msg, sizeof(drop_msg), "%s could not hold dropped %s.",
                             gs->entities[enemy_idx].name,
                             WEAPON_TABLE[gs->dropped_weapon_id].name);
                }
                log_locked(drop_msg);
            }
            gs->drop_available = false;
            gs->dropped_weapon_id = WPN_NONE;
            gs->drop_expires = 0;
        }

        bool enemies_remaining = gs->enemies_spawned < gs->enemy_goal;

        if (all_players_dead || gs->enemies_killed >= gs->enemy_goal) {
            fprintf(stderr, "[DEBUG] Game Over! Players dead: %d, Enemies killed: %d/%d\n", 
                    (int)all_players_dead, gs->enemies_killed, gs->enemy_goal);
            gs->game_over    = true;
            gs->game_running = false;
            gs->player_won   = (gs->enemies_killed >= gs->enemy_goal);
            g_keep_running   = 0;
        }
        else if (active_enemy_count == 0 && !enemies_remaining) {
            fprintf(stderr, "[DEBUG] Game Over! All enemies defeated, no respawns remaining\n");
            gs->game_over    = true;
            gs->game_running = false;
            gs->player_won   = true;
            g_keep_running   = 0;
        }

        sem_post(&gs->state_mutex);
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Deadlock monitor pthread (§7)
// ─────────────────────────────────────────────────────────────────────────────
static void* deadlock_monitor(void* arg)
{
    (void)arg;
    while (g_monitor_run)
    {
        sleep(2);
        if (!g_monitor_run) break;

        sem_wait(&gs->state_mutex);
        sem_wait(&gs->artifact_mutex);

        int sc_h = gs->solar_core.holder;
        int lb_h = gs->lunar_blade.holder;
        int sc_w = gs->solar_core.waiter;
        int lb_w = gs->lunar_blade.waiter;

        if (sc_h >= 0 && lb_h >= 0 && sc_h != lb_h &&
            sc_w == lb_h && lb_w == sc_h)
        {
            // Circular wait — force the Solar Core holder to release (§7)
            char dbuf[LOG_ENTRY_LEN];
            release_artifact(&gs->entities[sc_h], &gs->solar_core,
                             WPN_SOLAR_CORE, dbuf, sizeof(dbuf), true);
            log_locked(dbuf);
        }

        sem_post(&gs->artifact_mutex);
        sem_post(&gs->state_mutex);
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// SFML render — dedicated thread (§9)
// ─────────────────────────────────────────────────────────────────────────────
static void* render_thread_func(void* arg)
{
    (void)arg;  // unused
    fprintf(stderr, "[Arbiter] Render thread started, calling run_sfml_render...\n");
    run_sfml_render(g_asset_dir);
    fprintf(stderr, "[Arbiter] Render thread exiting.\n");
    return nullptr;
}

static void run_sfml_render(const std::string& asset_dir)
{
    const int WIN_W = 1100;
    const int WIN_H = 800;

    fprintf(stderr, "[SFML] Creating window (W=%d, H=%d)...\n", WIN_W, WIN_H);
    sf::RenderWindow window(sf::VideoMode(WIN_W, WIN_H), "Chrono Rift");
    fprintf(stderr, "[SFML] Window created successfully, is_open=%d\n", window.isOpen());
    window.setFramerateLimit(30);

    sf::Font titleFont;
    sf::Font bodyFont;
    fprintf(stderr, "[SFML] Loading fonts...\n");
    bool has_title_font = titleFont.loadFromFile(asset_dir + "/fonts/ROGENZ (DEMO).ttf");
    bool has_body_font = bodyFont.loadFromFile(asset_dir + "/fonts/Xirod.otf");
    fprintf(stderr, "[SFML] Font loads: title=%d body=%d\n", has_title_font, has_body_font);
    if (!has_title_font || !has_body_font) {
        fprintf(stderr, "[SFML] Falling back to system fonts...\n");
        if (!has_title_font)
            has_title_font = titleFont.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf");
        if (!has_body_font)
            has_body_font = bodyFont.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        if (!has_title_font)
            has_title_font = titleFont.loadFromFile("/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf");
        if (!has_body_font)
            has_body_font = bodyFont.loadFromFile("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
        fprintf(stderr, "[SFML] Fallback font loads: title=%d body=%d\n", has_title_font, has_body_font);
    }

    if (!has_title_font && has_body_font) {
        titleFont = bodyFont;
        has_title_font = true;
    }
    if (!has_body_font && has_title_font) {
        bodyFont = titleFont;
        has_body_font = true;
    }
    if (!has_title_font && !has_body_font) {
        fprintf(stderr, "[SFML] ERROR: no usable font loaded, text may render incorrectly\n");
    }

    sf::Texture bgTexture;
    sf::Texture playerTextures[4];
    sf::Texture enemyTextures[9];
    sf::Texture weaponTextures[NUM_WEAPONS];
    bool weaponTexturesLoaded[NUM_WEAPONS] = { false };
    auto load_tex = [&](sf::Texture& tex, const std::string& path, bool& flag) {
        flag = tex.loadFromFile(path);
        if (!flag) {
            fprintf(stderr, "[Arbiter] Failed to load texture: %s\n", path.c_str());
        } else {
            fprintf(stderr, "[Arbiter] Loaded texture: %s\n", path.c_str());
        }
    };

    fprintf(stderr, "[SFML] Loading background...\n");
    bool bgLoaded = bgTexture.loadFromFile(asset_dir + "/images/background.jpg");
    if (!bgLoaded)
        fprintf(stderr, "[Arbiter] Failed to load background: %s/images/background.jpg\n", asset_dir.c_str());
    else
        fprintf(stderr, "[Arbiter] Loaded background successfully\n");

    bool playerTexturesLoaded[4] = { false, false, false, false };
    bool enemyTexturesLoaded[9] = { false, false, false, false, false, false, false, false, false };

    fprintf(stderr, "[SFML] Loading player textures...\n");
    load_tex(playerTextures[0], asset_dir + "/images/player1.png", playerTexturesLoaded[0]);
    load_tex(playerTextures[1], asset_dir + "/images/player2.png", playerTexturesLoaded[1]);
    load_tex(playerTextures[2], asset_dir + "/images/player3.png", playerTexturesLoaded[2]);
    load_tex(playerTextures[3], asset_dir + "/images/player4.png", playerTexturesLoaded[3]);

    fprintf(stderr, "[SFML] Loading enemy textures...\n");
    load_tex(enemyTextures[0], asset_dir + "/images/enemy1.png", enemyTexturesLoaded[0]);
    load_tex(enemyTextures[1], asset_dir + "/images/enemy2.png", enemyTexturesLoaded[1]);
    load_tex(enemyTextures[2], asset_dir + "/images/enemy3.png", enemyTexturesLoaded[2]);
    load_tex(enemyTextures[3], asset_dir + "/images/enemy4.png", enemyTexturesLoaded[3]);
    load_tex(enemyTextures[4], asset_dir + "/images/enemy5.png", enemyTexturesLoaded[4]);
    load_tex(enemyTextures[5], asset_dir + "/images/enemy6.png", enemyTexturesLoaded[5]);
    load_tex(enemyTextures[6], asset_dir + "/images/enemy7.png", enemyTexturesLoaded[6]);
    load_tex(enemyTextures[7], asset_dir + "/images/enemy8.png", enemyTexturesLoaded[7]);
    load_tex(enemyTextures[8], asset_dir + "/images/enemy9.png", enemyTexturesLoaded[8]);

    // Map available images to weapons
    fprintf(stderr, "[SFML] Loading weapon textures...\n");
    load_tex(weaponTextures[WPN_SOLAR_CORE], asset_dir + "/images/Staff10.png", weaponTexturesLoaded[WPN_SOLAR_CORE]);
    load_tex(weaponTextures[WPN_LUNAR_BLADE], asset_dir + "/images/Sword14.png", weaponTexturesLoaded[WPN_LUNAR_BLADE]);
    load_tex(weaponTextures[WPN_IRON_HALBERD], asset_dir + "/images/Axe4.png", weaponTexturesLoaded[WPN_IRON_HALBERD]);
    load_tex(weaponTextures[WPN_VENOM_DAGGER], asset_dir + "/images/Dagger4.png", weaponTexturesLoaded[WPN_VENOM_DAGGER]);
    load_tex(weaponTextures[WPN_THUNDERSTAFF], asset_dir + "/images/Staff5.png", weaponTexturesLoaded[WPN_THUNDERSTAFF]);
    load_tex(weaponTextures[WPN_OBSIDIAN_AXE], asset_dir + "/images/Axe1.png", weaponTexturesLoaded[WPN_OBSIDIAN_AXE]);
    load_tex(weaponTextures[WPN_FROSTBOW], asset_dir + "/images/Bow1.png", weaponTexturesLoaded[WPN_FROSTBOW]);
    load_tex(weaponTextures[WPN_SPLINTER], asset_dir + "/images/Staff1.png", weaponTexturesLoaded[WPN_SPLINTER]);
    load_tex(weaponTextures[WPN_ECLIPSE_RELIC], asset_dir + "/images/Staff10.png", weaponTexturesLoaded[WPN_ECLIPSE_RELIC]); // Use same as Solar Core for now
    fprintf(stderr, "[SFML] All textures loaded, entering main loop...\n");

    auto draw_bar = [&](float x, float y, float val, float maxv,
                        sf::Color col, float w = 180.f, float h = 12.f)
    {
        sf::RectangleShape bg({w, h});
        bg.setPosition(x, y);
        bg.setFillColor({40, 40, 40, 220});
        window.draw(bg);

        float ratio = (maxv > 0.f) ? (val / maxv) : 0.f;
        if (ratio > 1.f) ratio = 1.f;
        sf::RectangleShape bar({w * ratio, h});
        bar.setPosition(x, y);
        bar.setFillColor(col);
        window.draw(bar);
    };

    auto txt = [&](const std::string& s, float x, float y,
                   unsigned sz, sf::Color c) -> sf::Text
    {
        sf::Text t(s, bodyFont, sz);
        if (!has_body_font)
            t.setFont(titleFont);
        t.setPosition(x, y);
        t.setFillColor(c);
        t.setOutlineColor(sf::Color(0, 0, 0, 180));
        t.setOutlineThickness(1.f);
        return t;
    };

    auto txt_title = [&](const std::string& s, float x, float y,
                         unsigned sz, sf::Color c) -> sf::Text
    {
        sf::Text t(s, titleFont, sz);
        if (!has_title_font)
            t.setFont(bodyFont);
        t.setPosition(x, y);
        t.setFillColor(c);
        t.setOutlineColor(sf::Color(0, 0, 0, 200));
        t.setOutlineThickness(2.f);
        return t;
    };

    bool show_welcome = true;
    sf::RectangleShape startButton({240.f, 60.f});
    startButton.setPosition(430.f, 420.f);
    startButton.setFillColor(sf::Color(62, 222, 138));
    startButton.setOutlineThickness(3.f);
    startButton.setOutlineColor(sf::Color(220, 255, 240));

    sf::RectangleShape welcomePanel({880.f, 260.f});
    welcomePanel.setPosition(110.f, 170.f);
    welcomePanel.setFillColor({16, 32, 54, 210});
    welcomePanel.setOutlineThickness(2.f);
    welcomePanel.setOutlineColor({90, 190, 255, 180});

    while (window.isOpen())
    {
        sf::Event ev;
        while (window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) {
                window.close();
                g_keep_running = 0;
                g_monitor_run  = 0;
            }
            else if (ev.type == sf::Event::MouseButtonPressed && ev.mouseButton.button == sf::Mouse::Left) {
                if (show_welcome) {
                    sf::Vector2i mousePos = sf::Mouse::getPosition(window);
                    if (startButton.getGlobalBounds().contains(static_cast<float>(mousePos.x), static_cast<float>(mousePos.y))) {
                        show_welcome = false;
                    }
                }
            }
            else if (ev.type == sf::Event::KeyPressed) {
                if (show_welcome) {
                    if (ev.key.code == sf::Keyboard::Space || ev.key.code == sf::Keyboard::Enter) {
                        show_welcome = false;
                    }
                } else {
                    sem_wait(&gs->state_mutex);
                    bool is_player_turn = (gs->active_entity >= 0 &&
                                           gs->entities[gs->active_entity].active &&
                                           gs->entities[gs->active_entity].is_player);
                    sem_post(&gs->state_mutex);

                    if (is_player_turn) {
                        // Handle movement keys
                        float move_speed = 5.0f;
                        sem_wait(&gs->state_mutex);
                        Entity* player = &gs->entities[gs->active_entity];
                        switch (ev.key.code) {
                            case sf::Keyboard::Up:
                                player->pos_y -= move_speed;
                                if (player->pos_y < 0) player->pos_y = 0;
                                break;
                            case sf::Keyboard::Down:
                                player->pos_y += move_speed;
                                if (player->pos_y > WIN_H - 64) player->pos_y = WIN_H - 64;
                                break;
                            case sf::Keyboard::Left:
                                player->pos_x -= move_speed;
                                if (player->pos_x < 0) player->pos_x = 0;
                                break;
                            case sf::Keyboard::Right:
                                player->pos_x += move_speed;
                                if (player->pos_x > WIN_W - 64) player->pos_x = WIN_W - 64;
                                break;
                            default:
                                break;
                        }
                        sem_post(&gs->state_mutex);
                    }

                    int action = ACTION_NONE;
                    switch (ev.key.code) {
                        case sf::Keyboard::A: action = ACTION_ATTACK; break;
                        case sf::Keyboard::Z: action = ACTION_EXHAUST; break;
                        case sf::Keyboard::W: action = ACTION_USE_WEAPON; break;
                        case sf::Keyboard::X: action = ACTION_SWAP_IN; break;
                        case sf::Keyboard::H: action = ACTION_HEAL; break;
                        case sf::Keyboard::K: action = ACTION_SKIP; break;
                        case sf::Keyboard::U: action = ACTION_ULTIMATE; break;
                        case sf::Keyboard::C: action = ACTION_ACQUIRE_ARTIFACT; break;
                        case sf::Keyboard::V: action = ACTION_RELEASE_ARTIFACT; break;
                        case sf::Keyboard::P: action = ACTION_PICKUP; break;
                        case sf::Keyboard::T: action = ACTION_STUN; break;
                        default: break;
                    }
                    if (action != ACTION_NONE) {
                        sem_wait(&gs->state_mutex);
                        if (gs->active_entity >= 0 &&
                            gs->entities[gs->active_entity].active &&
                            gs->entities[gs->active_entity].is_player &&
                            !gs->hip_action.pending &&
                            !gs->hip_ctrl.request_pending)
                        {
                            Entity* player = &gs->entities[gs->active_entity];
                            int target = first_alive_enemy();
                            int weapon_id = WPN_NONE;

                            switch (action) {
                            case ACTION_USE_WEAPON:
                                weapon_id = first_inventory_weapon(player);
                                break;
                            case ACTION_SWAP_IN:
                                weapon_id = first_long_storage_weapon(player);
                                break;
                            case ACTION_ACQUIRE_ARTIFACT:
                                weapon_id = first_free_artifact();
                                break;
                            case ACTION_RELEASE_ARTIFACT:
                                weapon_id = first_held_artifact(player);
                                break;
                            case ACTION_PICKUP:
                                weapon_id = gs->dropped_weapon_id;
                                break;
                            default:
                                break;
                            }

                            bool valid = true;
                            if ((action == ACTION_ATTACK || action == ACTION_EXHAUST || action == ACTION_USE_WEAPON) && target < 0)
                                valid = false;
                            if ((action == ACTION_USE_WEAPON || action == ACTION_SWAP_IN || action == ACTION_ACQUIRE_ARTIFACT || action == ACTION_RELEASE_ARTIFACT) && weapon_id == WPN_NONE)
                                valid = false;

                            if (valid) {
                                gs->hip_ctrl.request_pending = true;
                                gs->hip_ctrl.request_action  = action;
                                gs->hip_ctrl.request_target  = target;
                                gs->hip_ctrl.request_weapon_id = weapon_id;
                                gs->hip_ctrl.input_consumed = false;
                                {
                                    int owner = gs->player_owner[gs->active_entity];
                                    if (owner < 0 || owner >= MAX_HIP_PROCS)
                                        owner = 0;
                                    sem_post(&gs->hip_input_sem[owner]);
                                }
                            }
                        }
                        sem_post(&gs->state_mutex);
                    }
                }
            }
        }

        if (!g_keep_running) {
            if (g_terminate_requested && gs) {
                sem_wait(&gs->state_mutex);
                gs->game_running = false;
                gs->game_over    = true;
                sem_post(&gs->state_mutex);
            }
            window.close();
            break;
        }

        window.clear({8, 12, 28});

        if (show_welcome) {
            // Welcome screen
            if (bgLoaded) {
                sf::Sprite bgSprite(bgTexture);
                const sf::Vector2u bs = bgTexture.getSize();
                if (bs.x > 0 && bs.y > 0) {
                    float sx = (float)WIN_W / bs.x;
                    float sy = (float)WIN_H / bs.y;
                    float scale = std::max(sx, sy);
                    bgSprite.setScale(scale, scale);
                    float bw = bs.x * scale;
                    float bh = bs.y * scale;
                    bgSprite.setPosition((WIN_W - bw) * 0.5f, (WIN_H - bh) * 0.5f);
                    window.draw(bgSprite);
                }
            } else {
                sf::RectangleShape bg({(float)WIN_W, (float)WIN_H});
                bg.setFillColor({10, 18, 32});
                window.draw(bg);
            }

            window.draw(welcomePanel);
            if (has_body_font || has_title_font) {
                window.draw(txt_title("Welcome to", 170, 205, 36, {160, 238, 255}));
                window.draw(txt_title("CHRONO RIFT", 170, 250, 64, {102, 235, 255}));
                window.draw(txt("A tactical RPG where time bends to your will.", 170, 340, 24, {230, 230, 255}));
                window.draw(txt("Use keys or click the glowing button to begin your adventure.", 170, 380, 18, {255, 215, 110}));
                window.draw(txt("START ADVENTURE", 470, 434, 22, sf::Color::Black));
            }
            window.draw(startButton);
        } else {
            // Game screen
            if (bgLoaded) {
                sf::Sprite bgSprite(bgTexture);
                const sf::Vector2u bs = bgTexture.getSize();
                if (bs.x > 0 && bs.y > 0) {
                    float sx = (float)WIN_W / bs.x;
                    float sy = (float)WIN_H / bs.y;
                    float scale = std::max(sx, sy);
                    bgSprite.setScale(scale, scale);
                    float bw = bs.x * scale;
                    float bh = bs.y * scale;
                    bgSprite.setPosition((WIN_W - bw) * 0.5f, (WIN_H - bh) * 0.5f);
                    window.draw(bgSprite);
                }
            } else {
                sf::RectangleShape bg({(float)WIN_W, (float)WIN_H});
                bg.setFillColor({16, 24, 42});
                window.draw(bg);
            }

            sem_wait(&gs->state_mutex);

            if (has_body_font || has_title_font)
                window.draw(txt_title("CHRONO RIFT", 24, 16, 32, {120, 230, 255}));

            if (has_body_font || has_title_font) {
                window.draw(txt("Kills: " + std::to_string(gs->enemies_killed) + "/" + std::to_string(gs->enemy_goal),
                                840, 24, 18, sf::Color::Yellow));
            }

            sf::Vector2f invPanelPos(20.f, 560.f);
            sf::RectangleShape invPanel({320.f, 240.f});
            invPanel.setPosition(invPanelPos);
            invPanel.setFillColor({8, 12, 20, 220});
            invPanel.setOutlineThickness(2.f);
            invPanel.setOutlineColor({100, 160, 255, 140});
            window.draw(invPanel);

            sf::Vector2f logPanelPos(820.f, 560.f);
            sf::RectangleShape logPanel({260.f, 240.f});
            logPanel.setPosition(logPanelPos);
            logPanel.setFillColor({8, 12, 20, 200});
            logPanel.setOutlineThickness(2.f);
            logPanel.setOutlineColor({255, 210, 120, 160});
            window.draw(logPanel);

            auto draw_entity_sprite = [&](Entity* e) {
                if (!e->active || e->hp <= 0)
                    return;
                float bob = sinf(e->anim_phase) * 10.f;
                float px  = e->pos_x;
                float py  = e->pos_y + bob;
                bool active = (gs->active_entity == (e->is_player ? e->index : ENEMY_BASE_IDX + e->index));

                sf::Color baseColor = e->is_player ? sf::Color(110, 190, 255, 220) : sf::Color(255, 120, 110, 220);
                sf::CircleShape aura(50.f);
                aura.setOrigin(50.f, 50.f);
                aura.setPosition(px, py);
                aura.setFillColor(sf::Color(0, 0, 0, 0));
                aura.setOutlineThickness(active ? 4.f : 2.f);
                aura.setOutlineColor(active ? sf::Color::Yellow : sf::Color(100, 140, 190, 180));
                window.draw(aura);

                sf::CircleShape portrait(42.f);
                portrait.setOrigin(42.f, 42.f);
                portrait.setPosition(px, py);
                portrait.setFillColor(sf::Color(30, 30, 40, 220));
                window.draw(portrait);

                if (e->is_player && e->index >= 0 && e->index < 4 && playerTexturesLoaded[e->index]) {
                    sf::Sprite sprite(playerTextures[e->index]);
                    const sf::Vector2u ts = playerTextures[e->index].getSize();
                    if (ts.x > 0 && ts.y > 0) {
                        float scale = 76.f / std::max(ts.x, ts.y);
                        sprite.setScale(scale, scale);
                        sprite.setOrigin(ts.x / 2.f, ts.y / 2.f);
                    }
                    sprite.setPosition(px, py);
                    window.draw(sprite);
                } else if (!e->is_player && e->index >= 0 && e->index < 9 && enemyTexturesLoaded[e->index]) {
                    sf::Sprite sprite(enemyTextures[e->index]);
                    const sf::Vector2u ts = enemyTextures[e->index].getSize();
                    if (ts.x > 0 && ts.y > 0) {
                        float scale = 76.f / std::max(ts.x, ts.y);
                        sprite.setScale(scale, scale);
                        sprite.setOrigin(ts.x / 2.f, ts.y / 2.f);
                    }
                    sprite.setPosition(px, py);
                    window.draw(sprite);
                } else {
                    sf::CircleShape placeholder(40.f);
                    placeholder.setOrigin(40.f, 40.f);
                    placeholder.setPosition(px, py);
                    placeholder.setFillColor(baseColor);
                    window.draw(placeholder);
                }

                float barX = px - 52.f;
                float barY = py - 80.f;
                draw_bar(barX, barY, (float)e->hp, (float)e->max_hp, e->is_player ? sf::Color(80, 200, 80) : sf::Color(200, 80, 80), 104.f, 12.f);
                draw_bar(barX, barY + 16.f, e->stamina, e->max_stamina, sf::Color(220, 200, 40), 104.f, 12.f);
                if (has_body_font || has_title_font) {
                    window.draw(txt(std::string(e->name) + (active ? " [TURN]" : ""), barX, barY - 20.f, 14, sf::Color::White));
                    window.draw(txt("HP " + std::to_string(e->hp) + "/" + std::to_string(e->max_hp), barX, barY, 12, sf::Color::White));
                    window.draw(txt("STM " + std::to_string((int)e->stamina) + "/" + std::to_string((int)e->max_stamina), barX, barY + 16.f, 12, sf::Color::White));
                    if (e->stunned)
                        window.draw(txt("STUNNED", barX, barY - 34.f, 12, sf::Color::Red));
                }
            };

            for (int i = 0; i < gs->num_players; i++) {
                Entity* e = &gs->entities[i];
                if (!e->active || e->hp <= 0) continue;
                draw_entity_sprite(e);
            }
            for (int i = 0; i < gs->num_enemies; i++) {
                Entity* e = &gs->entities[ENEMY_BASE_IDX + i];
                if (!e->active || e->hp <= 0) continue;
                draw_entity_sprite(e);
            }

            if (has_body_font || has_title_font) {
                auto art_str = [](const Artifact& a) -> std::string {
                    if (!a.present) return "N/A";
                    if (a.holder < 0) return "Free";
                    return "Held";
                };
                window.draw(txt("Inventory", 40, 570, 16, sf::Color(175, 220, 255)));
                window.draw(txt("Solar Core: " + art_str(gs->solar_core), 40, 590, 14, sf::Color::White));
                window.draw(txt("Lunar Blade: " + art_str(gs->lunar_blade), 40, 610, 14, sf::Color::White));
                window.draw(txt("Eclipse Relic: " + art_str(gs->eclipse_relic), 40, 630, 14, sf::Color::White));
                if (gs->drop_available) {
                    window.draw(txt("Dropped: " + std::string(WEAPON_TABLE[gs->dropped_weapon_id].name) +
                                    " (P to pickup)", 40, 650, 13, sf::Color(200, 220, 255)));
                }

                auto player_weapon_count = [&](int idx) {
                    int count = 0;
                    Entity* p = &gs->entities[idx];
                    for (int s = 0; s < INVENTORY_SLOTS; s++) {
                        if (p->inventory[s].is_start && p->inventory[s].weapon_id != WPN_NONE)
                            count++;
                    }
                    return count;
                };

                std::string player_summary = "Players:";
                for (int i = 0; i < gs->num_players; i++) {
                    player_summary += " P" + std::to_string(i + 1) + "=" + std::to_string(player_weapon_count(i));
                    if (i + 1 < gs->num_players) player_summary += ",";
                }
                window.draw(txt(player_summary, 40, 668, 12, sf::Color(200, 220, 255)));

                window.draw(txt("Controls:", 40, 688, 14, sf::Color::White));
                window.draw(txt("A: Attack   Z: Exhaust", 40, 706, 12, sf::Color::White));
                window.draw(txt("W: Use   X: Swap   H: Heal", 40, 722, 12, sf::Color::White));
                window.draw(txt("K: Skip   U: Ultimate   C: Acquire", 40, 738, 12, sf::Color::White));
                window.draw(txt("V: Release   P: Pickup   T: Stun", 40, 754, 12, sf::Color::White));

                float weaponBarY = WIN_H - 82.f;
                const float weaponBarWidth = 400.f;
                sf::RectangleShape weaponBar({weaponBarWidth, 58.f});
                float weaponBarX = (WIN_W - weaponBarWidth) * 0.5f;
                weaponBar.setPosition(weaponBarX, weaponBarY);
                weaponBar.setFillColor({18, 28, 42, 210});
                weaponBar.setOutlineThickness(1.5f);
                weaponBar.setOutlineColor({120, 170, 230, 180});
                window.draw(weaponBar);

                window.draw(txt("Weapon Inventory", weaponBarX + 18.f, weaponBarY + 8.f, 12, sf::Color(180, 220, 255)));

                // Show weapons for ALL players, not just active one
                float weaponIconY = weaponBarY + 30.f;
                float weaponX = weaponBarX + 18.f;
                int total_weapons_shown = 0;

                for (int p = 0; p < gs->num_players && total_weapons_shown < 6; p++) {
                    Entity* player = &gs->entities[p];
                    bool is_active = (gs->active_entity == p);

                    for (int s = 0; s < INVENTORY_SLOTS && total_weapons_shown < 6; s++) {
                        if (player->inventory[s].weapon_id != WPN_NONE && player->inventory[s].is_start) {
                            int wid = player->inventory[s].weapon_id;

                            // Draw background indicating which player owns this weapon
                            sf::Color bgColor = is_active ? sf::Color(100, 150, 200, 180) : sf::Color(80, 100, 140, 150);
                            sf::RectangleShape bg({48.f, 48.f});
                            bg.setPosition(weaponX - 4.f, weaponIconY - 10.f);
                            bg.setFillColor(bgColor);
                            bg.setOutlineThickness(1.f);
                            bg.setOutlineColor(is_active ? sf::Color::Yellow : sf::Color(120, 140, 180, 200));
                            window.draw(bg);

                            if (wid >= 0 && wid < NUM_WEAPONS && weaponTexturesLoaded[wid]) {
                                sf::Sprite weaponSprite(weaponTextures[wid]);
                                const sf::Vector2u ts = weaponTextures[wid].getSize();
                                float scale = 36.f / std::max(1u, std::max(ts.x, ts.y));
                                weaponSprite.setScale(scale, scale);
                                weaponSprite.setPosition(weaponX, weaponIconY - 4.f);
                                window.draw(weaponSprite);
                            } else {
                                sf::RectangleShape placeholder({36.f, 36.f});
                                placeholder.setPosition(weaponX, weaponIconY - 4.f);
                                placeholder.setFillColor({100, 100, 130, 220});
                                placeholder.setOutlineThickness(1.f);
                                placeholder.setOutlineColor({160, 180, 210, 200});
                                window.draw(placeholder);
                                if (wid >= 0 && wid < NUM_WEAPONS) {
                                    window.draw(txt(std::string(1, WEAPON_TABLE[wid].name[0]), weaponX + 10.f, weaponIconY + 6.f, 14, sf::Color::White));
                                }
                            }

                            // Show player number
                            window.draw(txt("P" + std::to_string(p + 1), weaponX - 2.f, weaponIconY - 12.f, 10, is_active ? sf::Color::Yellow : sf::Color(200, 220, 255)));

                            weaponX += 52.f;
                            total_weapons_shown++;
                        }
                    }
                }

                if (total_weapons_shown == 0) {
                    window.draw(txt(" ", invPanelPos.x + 26.f, weaponIconY + 8.f, 12, sf::Color(190, 190, 220)));
                }

                window.draw(txt("Action Log", 830, 570, 16, sf::Color(255, 220, 170)));
                ActionLog* L = &gs->log;
                for (int k = 0; k < L->count && k < 12; k++) {
                    int idx = (L->head + L->count - 1 - k) % MAX_LOG_ENTRIES;
                    window.draw(txt(std::string(L->entries[idx]), 830, 594.f + k * 18.f, 12, {170, 210, 255}));
                }
            }

            if (gs->game_over && (has_body_font || has_title_font)) {
                sf::RectangleShape overlay({(float)WIN_W, (float)WIN_H});
                overlay.setFillColor({0, 0, 0, 175});
                window.draw(overlay);
                std::string msg = gs->player_won ? "VICTORY!" : "DEFEAT!";
                sf::Color col = gs->player_won ? sf::Color::Green : sf::Color::Red;
                window.draw(txt(msg, 360, 320, 64, col));
                window.draw(txt("Close window to exit.", 380, 400, 24, sf::Color::White));
            }

            sem_post(&gs->state_mutex);
        }
        window.display();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(void)
{
    signal(SIGALRM, sigalrm_handler);
    signal(SIGTERM, sigterm_handler);
    signal(SIGCHLD, sigchld_handler);

    int np = prompt_party_size();
    int hip_mode = prompt_hip_process_count();

    init_shared_memory();
    init_entities(np);
    setup_player_ownership(np, hip_mode);
    fprintf(stderr, "[INFO] HIP mode=%d active, player ownership:\n", hip_mode);
    for (int i = 0; i < np; i++) {
        fprintf(stderr, "[INFO]   Player %d -> HIP %d\n", i + 1, gs->player_owner[i]);
    }
    log_safe("Chrono Rift started!");

    spawn_processes();

    pthread_t sched_tid, monitor_tid;
    pthread_create(&sched_tid,   nullptr, scheduler_thread, nullptr);
    pthread_create(&monitor_tid, nullptr, deadlock_monitor, nullptr);

    // SFML must run on main thread - only if display is available
    pthread_t render_tid;
    bool render_started = false;

    g_asset_dir = resolve_asset_dir();
    fprintf(stderr, "[Arbiter] DISPLAY env var: %s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "NOT SET");
    fprintf(stderr, "[Arbiter] Asset dir: %s\n", g_asset_dir.c_str());
    
    if (getenv("DISPLAY")) {
        fprintf(stderr, "[Arbiter] Creating render thread...\n");
        pthread_create(&render_tid, nullptr, render_thread_func, nullptr);
        render_started = true;
        fprintf(stderr, "[Arbiter] Render thread created.\n");
    } else {
        fprintf(stderr, "[INFO] Running in headless mode - no SFML rendering\n");
        // In headless mode, just wait for termination
        while (g_keep_running) {
            sleep(1);
        }
    }

    if (render_started) {
        pthread_join(render_tid, nullptr);
    }

    // Shutdown
    g_keep_running = 0;
    g_monitor_run  = 0;

    pthread_join(sched_tid,   nullptr);
    pthread_join(monitor_tid, nullptr);

    for (int i = 0; i < gs->hip_process_count; i++) {
        if (gs->hip_pids[i] > 0)
            kill(gs->hip_pids[i], SIGTERM);
    }
    if (gs->asp_pid > 0) {
        kill(gs->asp_pid, SIGCONT);
        kill(gs->asp_pid, SIGTERM);
    }
    for (int i = 0; i < gs->hip_process_count; i++) {
        if (gs->hip_pids[i] > 0)
            waitpid(gs->hip_pids[i], nullptr, 0);
    }
    if (gs->asp_pid > 0)
        waitpid(gs->asp_pid, nullptr, 0);

    destroy_shared_memory();
    return 0;
}