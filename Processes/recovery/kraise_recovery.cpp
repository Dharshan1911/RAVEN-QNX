#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/syspage.h>

#include "../common/kraise_telemetry.h"
#include "../common/kraise_ipc.h"
#include "../common/kraise_config.h"

uint64_t cyclesToUs(uint64_t cycles) {
    uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    return (cycles * 1000000ULL) / cps;
}

int main() {
    printf("[RECOVERY] Starting PID %d\n", getpid());
    
    struct sched_param param;
    param.sched_priority = PRIO_RECOVERY;
    sched_setscheduler(0, SCHED_FIFO, &param);

    int shm_fd = -1;
    while ((shm_fd = shm_open(KRAISE_SHM_NAME, O_RDWR, 0666)) == -1) { usleep(100000); }
    KraiseSharedMemory* shm = (KraiseSharedMemory*)mmap(NULL, sizeof(KraiseSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    name_attach_t* attach = name_attach(NULL, RECOVERY_ATTACH_NAME, 0);
    
    while(1) {
        if (shm->header.shutdown_requested) break;

        struct _pulse pulse;
        int rcvid = MsgReceivePulse(attach->chid, &pulse, sizeof(pulse), NULL);
        if (rcvid == -1) continue;

        if (pulse.code == PULSE_FAULT) {
            int sid = pulse.value.sival_int;
            if (!validServiceId(sid)) continue;

            uint64_t start_rto = ClockCycles();

            pthread_mutex_lock(&shm->lock);
            SensorTelemetry t = shm->services[sid];
            shm->services[sid].health_state = HEALTH_RECOVERING;
            pthread_mutex_unlock(&shm->lock);

            printf("\033[33m[RECOVERY] Initiating recovery for %s (PID %d)\033[0m\n", t.name, t.pid);

            if (t.restart_count >= MAX_RESTART_ATTEMPTS) {
                printf("\033[31m[RECOVERY] %s reached max restarts! FAILED_PERMANENTLY.\033[0m\n", t.name);
                continue;
            }

            char cmd[256];
            snprintf(cmd, sizeof(cmd), "slay -f %s 2>/dev/null", t.name);
            system(cmd);

            snprintf(cmd, sizeof(cmd), "/data/home/qnxuser/kraise/%s %s %d %llu &", t.name, t.name, sid, t.period_us);
            system(cmd);

            pthread_mutex_lock(&shm->lock);
            shm->services[sid].restart_count++;
            shm->last_rto_us = cyclesToUs(ClockCycles() - start_rto);
            strcpy(shm->last_recovery_svc, t.name);
            pthread_mutex_unlock(&shm->lock);

            printf("\\033[32m[RECOVERY] Restarted %s (RTO: %llu us)\\033[0m\\n", t.name, shm->last_rto_us);
        }
    }
    return 0;
}
