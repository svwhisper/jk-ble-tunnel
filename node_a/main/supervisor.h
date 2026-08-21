/*
 * supervisor.h — watchdog, reachability-probe floor, idle-disconnect timers,
 * round-robin Node-RED polling, measurement-timeout guard (spec §4/§9/§11).
 * Also runs the boot harvest coordinator.
 */
#ifndef SUPERVISOR_H
#define SUPERVISOR_H
void supervisor_start(void);
#endif
