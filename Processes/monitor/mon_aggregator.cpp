#include "../common/telemetry.h"
#include "../common/timing.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <pthread.h>
#include <map>
#include <string>

// We use a map to keep track of the latest health state of each service.
std::map<std::string, TaskHealth> system_state;

int main() {
    printf("[MONITOR] Starting mon_aggregator...\n");

    // 1. Set priority
    struct sched_param param;
    param.sched_priority = MONITOR_PRIORITY;
    pthread_setschedparam(pthread_self(), SCHED_RR, &param);

    // 2. Create channel to receive messages
    name_attach_t *attach = name_attach(NULL, "mon_aggregator", 0);
    if (attach == NULL) {
        perror("[MONITOR] Failed to create channel");
        return -1;
    }

    printf("[MONITOR] Listening for telemetry on channel ID: %d\n", attach->chid);

    // 3. Receive Loop
    while (1) {
        union {
            uint16_t type;
            HeartbeatMsg heartbeat;
        } msg;

        int rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);
        if (rcvid == -1) {
            perror("[MONITOR] MsgReceive failed");
            continue;
        }
        
        // Handle QNX system pulses (e.g., from timers, OS events, or disconnects)
        if (rcvid == 0) {
            switch (msg.heartbeat.type) {
                case _PULSE_CODE_DISCONNECT:
                    ConnectDetach(msg.heartbeat.health.pid);
                    break;
                // We will handle custom pulses (like timer sweeps) in the fault_detector
            }
            continue;
        }

        // Handle custom application messages
        if (msg.type == RAVEN_MSG_HEARTBEAT) {
            // Update the state map
            system_state[msg.heartbeat.health.name] = msg.heartbeat.health;
            
            // Immediately unblock the sender. This native IPC capability is why 
            // QNX is preferred over TCP/IP sockets for inter-process coordination.
            MsgReply(rcvid, 0, NULL, 0);
            
            // We can output a simplified log
            // printf("[MONITOR] Received heartbeat from %s (Exec: %llu us)\n", 
            //         msg.heartbeat.health.name, msg.heartbeat.health.executionTimeUs);
        } else {
            // Unknown message
            MsgError(rcvid, ENOSYS);
        }
    }

    name_detach(attach, 0);
    return 0;
}
