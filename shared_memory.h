#pragma once
#include <semaphore.h>
#include <sys/types.h>
#include <stdbool.h>

// ─── Shared memory name ───────────────────────────────────────────────────────
#define SHM_NAME          "/chrono_rift_shm"

// ─── Sizes ────────────────────────────────────────────────────────────────────
#define MAX_PLAYERS       4
#define MAX_ENEMIES       9
#define MAX_HIP_PROCS     2
#define MAX_ENTITIES      (MAX_PLAYERS + MAX_ENEMIES)
#define INVENTORY_SLOTS   20
#define MAX_LONG_STORAGE  50
#define MAX_LOG_ENTRIES   20
#define LOG_ENTRY_LEN     128

// ─── Enemy index offset ───────────────────────────────────────────────────────
#define ENEMY_BASE_IDX    MAX_PLAYERS

// ─── Weapon IDs ───────────────────────────────────────────────────────────────
#define WPN_NONE          0
#define WPN_SOLAR_CORE    1   // 10 slots, 95 dmg  — artifact
#define WPN_LUNAR_BLADE   2   // 10 slots, 90 dmg  — artifact
#define WPN_IRON_HALBERD  3   //  7 slots, 55 dmg
#define WPN_VENOM_DAGGER  4   //  4 slots, 30 dmg
#define WPN_THUNDERSTAFF  5   //  6 slots, 50 dmg
#define WPN_OBSIDIAN_AXE  6   //  5 slots, 45 dmg
#define WPN_FROSTBOW      7   //  6 slots, 48 dmg
#define WPN_SPLINTER      8   //  2 slots, 12 dmg
#define WPN_ECLIPSE_RELIC 9   // 10 slots, 120 dmg — rare artifact

// ─── Action codes ─────────────────────────────────────────────────────────────
#define ACTION_NONE       0
#define ACTION_ATTACK     1   // Strike  — reduces target HP
#define ACTION_EXHAUST    2   // Exhaust — reduces target stamina
#define ACTION_USE_WEAPON 3
#define ACTION_SWAP_IN    4
#define ACTION_HEAL       5
#define ACTION_SKIP       6
#define ACTION_ULTIMATE   7
#define ACTION_ACQUIRE_ARTIFACT 8
#define ACTION_RELEASE_ARTIFACT 9
#define ACTION_PICKUP    10
#define ACTION_QUIT      99

// ─── Stun action (used internally by stun mechanic) ──────────────────────────
#define ACTION_STUN       11  // Arbiter sets stunned=true then sends SIGUSR1

// ─── Weapon info table (read-only, embedded in header) ───────────────────────
typedef struct {
    int  id;
    char name[24];
    int  slot_size;
    int  damage;
} WeaponInfo;

static const WeaponInfo WEAPON_TABLE[] = {
    { WPN_NONE,         "None",           0,  0  },
    { WPN_SOLAR_CORE,   "Solar Core",    10, 95  },
    { WPN_LUNAR_BLADE,  "Lunar Blade",   10, 90  },
    { WPN_IRON_HALBERD, "Iron Halberd",   7, 55  },
    { WPN_VENOM_DAGGER, "Venom Dagger",   4, 30  },
    { WPN_THUNDERSTAFF, "Thunderstaff",   6, 50  },
    { WPN_OBSIDIAN_AXE, "Obsidian Axe",   5, 45  },
    { WPN_FROSTBOW,     "Frostbow",       6, 48  },
    { WPN_SPLINTER,     "Splinter Stick", 2, 12  },
    { WPN_ECLIPSE_RELIC, "Eclipse Relic", 10, 120 },
};
#define NUM_WEAPONS 10

// ─── Inventory slot ───────────────────────────────────────────────────────────
typedef struct {
    int  weapon_id;  // WPN_NONE = empty
    bool is_start;   // true only on the first slot of a weapon block
} InvSlot;

// ─── Entity ───────────────────────────────────────────────────────────────────
typedef struct {
    bool  active;
    bool  is_player;
    int   index;         // 0-based within its category
    char  name[24];
    pid_t pid;           // owning process — Arbiter sends SIGUSR1 here for stun

    // stats
    int   hp;
    int   max_hp;
    int   damage;
    int   speed;

    // stamina (§3)
    float stamina;
    float max_stamina;

    // world position for rendering moving entities
    float pos_x;
    float pos_y;
    float anim_phase;

    // stun (§5)
    // Arbiter sets stunned = true then sends SIGUSR1 to entity.pid.
    // Signal handler in HIP/ASP calls sleep(3) — no flag polling by target.
    bool  stunned;
    time_t stun_end;

    // inventory — players only (§6)
    InvSlot inventory[INVENTORY_SLOTS];
    int     long_storage[MAX_LONG_STORAGE];
    int     long_storage_count;
} Entity;

// ─── Artifact resource table entry (§7) ──────────────────────────────────────
typedef struct {
    int  holder;   // entity index currently holding it, -1 = free
    int  waiter;   // entity index waiting to acquire it, -1 = none
    bool present;  // eclipse relic starts false; others start true
} Artifact;

// ─── Pending action: IPC channel HIP/ASP → Arbiter (no pipes allowed, §4) ───
typedef struct {
    bool pending;
    int  entity_index;
    int  action_code;
    int  target_index;
    int  weapon_id;
} PendingAction;

// ─── Action log (ring buffer, written by Arbiter) ─────────────────────────────
typedef struct {
    char entries[MAX_LOG_ENTRIES][LOG_ENTRY_LEN];
    int  head;
    int  count;
} ActionLog;

// ─── HIP input buffer: Arbiter tells HIP which player's turn it is ────────────
// HIP reads this to activate the correct player thread.
typedef struct {
    int  active_player_idx;   // -1 = no player turn pending
    bool input_consumed;      // HIP sets true after writing action

    // Renderer writes desired request; HIP translates this into a real action.
    bool request_pending;
    int  request_action;
    int  request_target;
    int  request_weapon_id;
} HipControl;

// ─── Master shared memory block ───────────────────────────────────────────────
//
// Signal usage summary (§5, §8, §10):
//   SIGUSR1  Arbiter → HIP or ASP   Stun notification. Handler calls sleep(3).
//   SIGSTOP  Arbiter → ASP          Suspend ASP for Ultimate 10-s window (§8).
//                                   Kernel delivers — ASP never checks a flag.
//   SIGCONT  Arbiter → ASP          Resume ASP. Sent inside SIGALRM handler.
//   SIGALRM  kernel  → Arbiter      Fires after alarm(10); handler sends SIGCONT.
//   SIGTERM  HIP     → Arbiter      Player chose Quit (§10).
//   SIGCHLD  kernel  → Arbiter      Child exited; Arbiter calls waitpid().
//
// IPC rule (§4): NO pipes anywhere. Only shared memory + unnamed semaphores.
//
typedef struct {
    // Unnamed semaphores — pshared=1 so they work across processes
    sem_t state_mutex;      // protects all fields below
    sem_t artifact_mutex;   // protects the three Artifact entries
    sem_t hip_input_sem[MAX_HIP_PROCS];    // signals each HIP process when input or active player arrives

    // PIDs written by Arbiter after fork()
    pid_t arbiter_pid;
    pid_t hip_pids[MAX_HIP_PROCS];
    int   hip_process_count;
    int   player_owner[MAX_PLAYERS];
    pid_t asp_pid;

    // Game state
    bool  game_running;
    bool  game_over;
    bool  player_won;
    int   enemies_killed;   // game ends at enemy_goal
    int   enemy_goal;
    int   enemies_spawned;  // total enemies created so far
    int   active_entity;    // index into entities[], -1 = scheduler still deciding

    // dropped weapon state
    bool  drop_available;
    int   dropped_weapon_id;
    time_t drop_expires;

    // Entities
    int    num_players;
    int    num_enemies;
    Entity entities[MAX_ENTITIES];

    // Artifact table (§7)
    Artifact solar_core;
    Artifact lunar_blade;
    Artifact eclipse_relic;

    // IPC: action slots — one per process, written by HIP/ASP, cleared by Arbiter
    PendingAction hip_action;
    PendingAction asp_action;

    // HIP control: Arbiter signals HIP which player thread should act
    HipControl hip_ctrl;

    // UI action log (§9)
    ActionLog log;
} GameState;