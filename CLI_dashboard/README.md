# Standalone CLI Dashboard

This folder contains the **CLI Dashboard viewer module** extracted from the main `kraise_system.cpp` file. 

### Why is this in a separate folder?
The actual running RAVEN implementation (`kraise_system.cpp`) intentionally bundles the entire microkernel environment—including this CLI dashboard—into a single orchestrated executable using `fork()`. This guarantees the dashboard automatically runs synchronously with the sensor tasks when the application starts.

However, for modular code review, the CLI dashboard logic has been extracted into this standalone `cli_dashboard.cpp` file.

### What it does
The CLI dashboard demonstrates true **zero-copy read-only monitoring**:
1. Connects to the POSIX shared memory `/dev/shmem/kraise_telemetry`.
2. Employs `PTHREAD_PROCESS_SHARED` mutex locks (`pthread_mutex_lock`).
3. Takes a fast stack-local snapshot (`memcpy`) of the telemetry data to prevent lock contention.
4. Safely renders `uint64_t` metrics via `PRIu64` format specifiers at a constant 4 Hz (`250000` µs refresh rate).
5. Fully isolated: if the CLI dashboard is killed or crashes, the core sensor system remains 100% unaffected.

### Standalone Compilation (Optional)
Because it has its own `main()` wrapper in this folder, you can optionally compile this file directly if you wish to run a second dashboard viewer in another SSH terminal:
```sh
qcc -Vgcc_ntoaarch64le -o cli_dashboard cli_dashboard.cpp -lm
./cli_dashboard
```
