# Chrono Rift — Game Arbiter (`arbiter.cpp`)

> A multi-process, multi-threaded tactical RPG built on Linux using POSIX APIs.

---

## Overview

The **Arbiter** is the central authority of Chrono Rift. It owns the shared memory segment, spawns all child processes, manages the turn-based scheduler, enforces game rules, and drives the SFML graphical window.

---

## Architecture

```
arbiter (main process)
├── Shared Memory (POSIX shm)  ←→  HIP process(es)  [human input]
│                              ←→  ASP process       [AI/NPC logic]
├── pthread: scheduler_thread      — turn & stamina engine
├── pthread: deadlock_monitor      — artifact deadlock resolution
└── pthread: render_thread_func    — SFML window (requires DISPLAY)
```

---

## IPC & Concurrency Model

| Mechanism | Purpose |
|---|---|
| POSIX shared memory (`shm_open` / `mmap`) | Single `GameState` struct shared across all processes |
| Unnamed semaphores (`sem_init`) | `state_mutex`, `artifact_mutex`, per-HIP input semaphores |
| `pthreads` only | No `<atomic>` or `<thread>` |
| No pipes | All IPC goes through shared memory |

---

## Signal Handling

| Signal | Source | Effect |
|---|---|---|
| `SIGALRM` | Kernel timer | Ends the 10-second Ultimate window → sends `SIGCONT` to ASP; also fires NPC 3-second turn timeout |
| `SIGTERM` | HIP process (player quit) | Sets `game_running = false`, `game_over = true`, cleans up |
| `SIGCHLD` | Child process exit | Reaps dead child processes via `waitpid(WNOHANG)` |
| `SIGUSR1` | Arbiter → target entity | Stuns an entity for 3 seconds |
| `SIGSTOP` / `SIGCONT` | Arbiter → ASP | Freezes ASP during Ultimate ability (§8) |

---

## Processes

### HIP (Human Input Process)
Handles player keyboard input. One or two HIP processes can be spawned at startup. Each HIP owns a subset of players determined by `player_owner[]`.

### ASP (AI/NPC Scheduler Process)
Controls enemy entities. Suspended via `SIGSTOP` during a player's Ultimate ability and resumed by `SIGCONT` sent from the `SIGALRM` handler after 10 seconds.

---

## Game Mechanics

### Turn Scheduler (§3)
- Runs as a pthread, ticking every **100 ms**
- Each entity accumulates stamina at a rate proportional to its `speed`
- The entity that reaches `max_stamina` first gets the next turn
- Serial execution: only one entity acts at a time

### Inventory System (§6)
- Each player has a fixed-size primary inventory (`INVENTORY_SLOTS`)
- Weapons occupy a variable number of contiguous slots (`slot_size`)
- If no contiguous run is available, weapons are evicted to **long-term storage** (fewest evictions possible)
- Dropped weapons expire after **15 seconds**; an enemy picks them up if unclaimed

### Artifacts (§7)
Three special weapons with ownership semantics:

| Artifact | Unlock condition |
|---|---|
| Solar Core | Available from game start |
| Lunar Blade | Available from game start |
| Eclipse Relic | Appears after 3 enemy kills |

- Only one entity can **hold** an artifact at a time
- A second entity can **wait** for it
- Deadlock (circular wait between Solar Core and Lunar Blade) is detected and resolved every 2 seconds by the monitor thread — the Solar Core holder is forced to release

### Ultimate Ability (§8)
- Requires both **Solar Core** and **Lunar Blade** in the active player's inventory
- Sends `SIGSTOP` to ASP, pausing all NPC activity for **10 seconds**
- `SIGALRM` fires after 10 s; its handler sends `SIGCONT` to resume ASP

### NPC Timeout (§8)
- If an NPC does not submit an action within **3 seconds**, the scheduler force-skips its turn (stamina set to 50%)
- Implemented via wall-clock deadline to avoid colliding with the Ultimate `alarm()`

---

## Win / Loss Conditions

| Condition | Result |
|---|---|
| `enemies_killed >= enemy_goal` (10 kills) | **Victory** |
| All players reach 0 HP | **Defeat** |
| Player closes the window / sends quit action | Game ends |

Enemies respawn into vacated slots until the kill goal is reached.

---

## Player Stat Derivation

Stats are seeded from the last digits of `ROLL_NUMBER` (default `0507`):

| Stat | Formula |
|---|---|
| Player HP | `ROLL_NUMBER + 100 + rand() % 901` |
| Player damage | `roll_last1 + 10` |
| Player speed | `100 / num_players` |
| Enemy HP | `roll_last2 + 50 + rand() % 151` |
| Enemy damage | `roll_2nd_last + 10` |

---

## Controls (SFML window)

| Key | Action |
|---|---|
| `A` | Attack |
| `Z` | Exhaust (drain enemy stamina) |
| `W` | Use weapon from inventory |
| `X` | Swap in weapon from long-term storage |
| `H` | Heal (restore 10% max HP) |
| `K` | Skip turn (gain 50% stamina) |
| `U` | Ultimate ability |
| `C` | Acquire artifact |
| `V` | Release artifact |
| `P` | Pick up dropped weapon |
| `T` | Stun target |
| `↑ ↓ ← →` | Move active player sprite |

---

## Asset Directory Resolution

The arbiter searches for an `assets/` directory by walking up from the current working directory, then from the executable's directory. Falls back to `./assets` if not found.

Expected structure:
```
assets/
├── fonts/
│   ├── ROGENZ (DEMO).ttf
│   └── Xirod.otf
└── images/
    ├── background.jpg
    ├── player1.png ... player4.png
    ├── enemy1.png  ... enemy9.png
    └── *.png  (weapon sprites)
```

---

## Build & Run

```bash
# Build (example — adjust to your Makefile)
g++ -std=c++17 arbiter.cpp -o arbiter -lpthread -lrt -lsfml-graphics -lsfml-window -lsfml-system

# Run
./arbiter
```

At startup you will be prompted for:
1. **Party size** (1–4 players)
2. **HIP process mode** (1 = single process, 2 = two HIP processes)

Set the `DISPLAY` environment variable for the SFML window. Without it, the arbiter runs in headless mode.

---

## Dependencies

- Linux (POSIX APIs: `shm_open`, `mmap`, `sem_init`, `pthreads`, `signals`)
- [SFML](https://www.sfml-dev.org/) ≥ 2.5
- C++17 (`std::filesystem`)
