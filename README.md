# RAVEN — Real-Time RTOS Monitoring & Recovery

RAVEN is a QNX-based smart-city RTOS platform for monitoring sensor/service health, CPU usage, deadlines and IPC behavior, detecting faults, and recovering affected services. Smart-city services must continue operating predictably even when individual services become delayed, overloaded, or unavailable.

## ⭐ MAIN IMPLEMENTATION

**`kraise_system.cpp`**

This is the primary RAVEN/KRAISE C++ implementation. It is intentionally written as a comprehensive, single-source orchestrator that, upon execution, implements the entire runtime RAVEN architecture. It creates, manages, and supervises all required runtime microkernel processes (Sensors, Supervisor, and Dashboard) via native OS process instantiation.

---

## 🏛 QNX Architecture

The implementation leverages a pure microkernel multi-process architecture. Rather than collapsing services into threads, RAVEN preserves full process isolation:

### Process Mapping

| Runtime Component | Process / Function | Purpose |
|-------------------|--------------------|---------|
| **Orchestrator**  | `main()` | Spawns all subsystems via `fork()` and acts as the High Availability Manager (HAM). |
| **Supervisor**    | `run_supervisor()` | Central IPC server managing system health and shared memory telemetry. |
| **PIR-A**         | `run_sensor("svc_pir_a")` | Independent sensor process evaluating on a 100ms hardware timer. |
| **PIR-B**         | `run_sensor("svc_pir_b")` | Independent sensor process evaluating on a 100ms hardware timer. |
| **DHT11-A**       | `run_sensor("svc_dht11_a")`| Independent sensor process evaluating on a 200ms hardware timer. |
| **DHT11-B**       | `run_sensor("svc_dht11_b")`| Independent sensor process evaluating on a 200ms hardware timer. |
| **Ultrasonic**    | `run_sensor("svc_ultrasonic")`| Independent sensor process evaluating on a 50ms hardware timer. |
| **KRAISE CLI**    | `run_cli()` | Read-only terminal dashboard mapping the telemetry shared memory. |

### QNX Technologies Used
- `fork()` for memory-isolated process creation
- QNX Native IPC: `name_attach`, `name_open`, `MsgSend`, `MsgReceive`, `MsgReply`
- Asynchronous Pulses: `MsgSendPulse()`
- Real-time Hardware Timers: `timer_create()`, `timer_settime()`, `SIGEV_PULSE`
- POSIX Shared Memory: `shm_open()`, `mmap()`, with `PTHREAD_PROCESS_SHARED` mutexes
- Precise CPU/Wall timing: `pthread_getcpuclockid()`, `CLOCK_MONOTONIC`, `ClockCycles()`

---

## 🛡 Fault Detection & Recovery

RAVEN actively monitors system execution for the following fault types:
- **Process Failure:** Handled via the Orchestrator's `waitpid()` loop.
- **Heartbeat Failure:** Handled via asynchronous pulses. Missed beats trigger a degradation in health state.
- **Deadline Misses:** Measured via `ClockCycles()` vs configured deadline limits.

### Recovery Mechanism
When a supervised service exits or crashes, the **KRAISE Supervisory Recovery** (a custom application-level orchestrator built on `fork/waitpid`, distinct from the official QNX HAM subsystem) immediately intercepts the exit status and forks a fresh instance of the failed component, restoring it to the system.

---

## 🛠 Build & Run Instructions

### 1. Build
Use the provided batch script on a Windows host with the QNX 8.0 SDP installed. This will compile `kraise_system.cpp` into the target `kraise_system` binary.
```bat
.\build\build_kraise_system.bat
```

### 2. Deploy
Deploy the binary to your QNX Raspberry Pi target (IP: `169.254.43.14`). The deploy script handles stopping old processes, clearing stale shared memory, and setting executable permissions.
```bat
.\scripts\deploy_kraise_system.bat
```

### 3. Run
Connect to your QNX target via SSH and execute the orchestrator:
```sh
ssh -c aes256-ctr -o MACs=hmac-sha2-256 qnxuser@169.254.43.14
cd /data/home/qnxuser/kraise
./kraise_system
```
*The KRAISE CLI dashboard will immediately appear and begin rendering telemetry at 4 Hz.*

---

## 💥 Fault Injection Demonstration

You can manually demonstrate RAVEN's fault detection and recovery:

1. **NORMAL:** Observe the CLI dashboard showing all services as `HEALTHY`.
2. **FAULT:** In a separate SSH session, deliberately kill a sensor process:
   ```sh
   slay -f svc_pir_a
   ```
3. **DETECTION & RECOVERY:** The orchestrator will instantly log an alert:
   `[HAM ALERT] PID XXXX exited (status=0). Initiating recovery...`
4. **NORMAL:** The CLI will show the new PID for `svc_pir_a`, increment the `RESTARTS` counter, and the system will return to a nominal state.

---

## 📊 System Profiler

The project includes a trace script (`scripts/run_kraise_trace.sh`) to capture real-world microkernel message passing, process recovery, and timer interrupts. The resulting `.kev` trace can be opened in the QNX Momentics IDE System Profiler. 

See [docs/system-profiler.md](docs/system-profiler.md) for full instructions on generating and viewing traces.

---

## 🌐 Optional Web Dashboard

The repository also includes a React/Vite-based web dashboard in the `optional_frontend_dashboard/` directory. This frontend visualizes the telemetry from the QNX sensors (PIR-A, PIR-B, DHT11-A, DHT11-B, Ultrasonic) and demonstrates the fault recovery events graphically. 

To run the dashboard on your host machine:
```sh
cd optional_frontend_dashboard
npm install
npm run dev
```
