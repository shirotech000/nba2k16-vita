#pragma once
// cpu cores we can hand to the game. stock vita apps get cores 0-2, with
// the CapUnlocker plugin loaded they can also use core 3 (the "system" one).

void cpu_init(void);               // probes once, call early on the main thread
int cpu_capunlocker(void);         // 1 if core 3 is ours
int cpu_count(void);               // 3 or 4
int cpu_game_mask(void);           // affinity mask for game threads
void cpu_place_game_thread(void);  // applies cpu_game_mask() to the calling thread
void cpu_pin_render_thread(void);  // calling thread gets core 0 alone (pin_render=0 to skip)
