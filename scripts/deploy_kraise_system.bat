@echo off
setlocal

set SSH_OPT=-c aes256-ctr -o MACs=hmac-sha2-256 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
set TARGET=qnxuser@169.254.43.14
set DEST_DIR=/data/home/qnxuser/kraise
set BINARY=build\kraise_system

echo ============================================================
echo  KRAISE DEPLOY SCRIPT
echo  Target: %TARGET%
echo  Dest:   %DEST_DIR%
echo ============================================================

REM ------------------------------------------------------------------
REM STEP 1: Verify binary exists on host
REM ------------------------------------------------------------------
if not exist %BINARY% (
    echo [DEPLOY] ERROR: %BINARY% not found. Run build_kraise_system.bat first.
    exit /b 1
)
echo [DEPLOY] Binary confirmed: %BINARY%

REM ------------------------------------------------------------------
REM STEP 2: Kill ALL old KRAISE processes on target
REM ------------------------------------------------------------------
echo [DEPLOY] Stopping old processes on target...
ssh %SSH_OPT% %TARGET% "slay -f kraise_system svc_pir_a svc_pir_b svc_dht11_a svc_dht11_b svc_ultrasonic 2>/dev/null; sleep 1; pidin ar 2>/dev/null | grep kraise || echo NO_KRAISE_RUNNING"

REM ------------------------------------------------------------------
REM STEP 3: Remove stale shared memory
REM ------------------------------------------------------------------
echo [DEPLOY] Removing stale shared memory...
ssh %SSH_OPT% %TARGET% "rm -f /dev/shmem/kraise_telemetry && echo SHM_REMOVED || echo SHM_NOT_PRESENT"

REM ------------------------------------------------------------------
REM STEP 4: Create destination directory
REM ------------------------------------------------------------------
ssh %SSH_OPT% %TARGET% "mkdir -p %DEST_DIR%"

REM ------------------------------------------------------------------
REM STEP 5: Deploy the binary
REM ------------------------------------------------------------------
echo [DEPLOY] Copying kraise_system to target...
scp %SSH_OPT% %BINARY% %TARGET%:%DEST_DIR%/kraise_system
if %ERRORLEVEL% NEQ 0 (
    echo [DEPLOY] ERROR: scp failed. Is the board booted and reachable?
    exit /b 1
)

REM ------------------------------------------------------------------
REM STEP 6: Set executable permission and verify
REM ------------------------------------------------------------------
ssh %SSH_OPT% %TARGET% "chmod +x %DEST_DIR%/kraise_system && ls -lh %DEST_DIR%/kraise_system && file %DEST_DIR%/kraise_system"

echo.
echo ============================================================
echo  DEPLOYMENT COMPLETE
echo  To run:
echo    ssh %SSH_OPT% %TARGET%
echo    cd %DEST_DIR%
echo    ./kraise_system
echo.
echo  To generate trace:
echo    sh /data/home/qnxuser/kraise/run_kraise_trace.sh
echo ============================================================
endlocal
