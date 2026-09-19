# RAVEN/KRAISE Architecture

## Runtime Process Mapping

The RAVEN architecture implemented in `kraise_system.cpp` is a true multi-process QNX architecture. It creates and supervises the following independent runtime processes from a single executable:

| Runtime Component | Process / Function | Purpose |
|-------------------|--------------------|---------|
| **Orchestrator (HAM)** | `main()` | The primary orchestrator. Spawns all subsystems via `fork()` and acts as the High Availability Manager (HAM) using `waitpid()`. If any supervised process exits unexpectedly, it instantly forks a replacement. |
| **Supervisor** | `run_supervisor()` | Central IPC server. Attaches the QNX name `kraise/supervisor`, allocates the `KraiseSharedMemory` block, and receives telemetry data/pulses from sensors. |
| **PIR-A** | `run_sensor("svc_pir_a")` | Independent sensor process. Evaluates on a 100ms hardware timer with a 50ms deadline. |
| **PIR-B** | `run_sensor("svc_pir_b")` | Independent sensor process. Evaluates on a 100ms hardware timer with a 50ms deadline. |
| **DHT11-A** | `run_sensor("svc_dht11_a")`| Independent sensor process. Evaluates on a 200ms hardware timer with a 100ms deadline. |
| **DHT11-B** | `run_sensor("svc_dht11_b")`| Independent sensor process. Evaluates on a 200ms hardware timer with a 100ms deadline. |
| **Ultrasonic** | `run_sensor("svc_ultrasonic")`| Independent sensor process. Evaluates on a 50ms hardware timer with a 25ms deadline. |
| **KRAISE Watch/CLI** | `run_cli()` | Read-only terminal dashboard. Connects to the shared memory via `shm_open(O_RDWR)` to map the process-shared mutex, and renders the telemetry health at 4 Hz without interfering with real-time execution. |

## QNX Mechanisms Utilized

The architecture relies on the following native QNX APIs:

- **Processes:** Full isolation using `fork()`.
- **QNX IPC:** `name_attach()` and `name_open()` for named channel resolution. `MsgSend()`, `MsgReceive()`, and `MsgReply()` for synchronous data transfer.
- **Asynchronous Pulses:** `MsgSendPulse()` is used for non-blocking heartbeat transmission to avoid stalling sensors on Supervisor delays.
- **Hardware Timers:** Thread blocking via `timer_create()`, `timer_settime()`, and `SIGEV_PULSE` mapped to `CLOCK_MONOTONIC`.
- **Shared Memory:** `shm_open()`, `ftruncate()`, and `mmap()` for storing global state telemetry, synchronized with `PTHREAD_PROCESS_SHARED` mutexes.
- **Priorities:** Real-time scheduling using `sched_setscheduler()` and `SCHED_FIFO`. (Supervisor=23, Fast Sensors=20, Slow Sensors=15).
- **Execution Timing:** Exact CPU time measured via `pthread_getcpuclockid()`, and wall-clock time measured via QNX `ClockCycles()`.
- **Recovery:** Custom KRAISE Supervisory Recovery loop tracking PIDs via `waitpid()`.
