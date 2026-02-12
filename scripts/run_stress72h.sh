#!/bin/bash

# 72-hour stress test runner
# Usage: ./scripts/run_stress72h.sh [output_log]

DURATION_HOURS=72
END_TIME=$(date -v +${DURATION_HOURS}H +%s)
LOG_FILE=${1:-"stress_test_72h.log"}

echo "Starting ${DURATION_HOURS}-hour stress test..."
echo "Results will be logged to ${LOG_FILE}"
echo "Start time: $(date)" > "$LOG_FILE"

while [ $(date +%s) -lt $END_TIME ]; do
    CURRENT_TIME=$(date)
    echo "[$CURRENT_TIME] Running benchmark iteration..." >> "$LOG_FILE"
    
    # Run standard benchmark
    ./build/rift_metrics --size 104857600 --mode rift >> "$LOG_FILE" 2>&1
    
    # Run with 5% loss
    echo "[$CURRENT_TIME] Running loss iteration (5%)..." >> "$LOG_FILE"
    # Start loss sim background
    ./build/rift_losssim 9000 9001 5 &
    SIM_PID=$!
    sleep 1
    ./build/rift_metrics --size 10485760 --port 9000 >> "$LOG_FILE" 2>&1
    kill $SIM_PID 2>/dev/null || true
    
    # Sleep 60 seconds between iterations
    sleep 60
done

echo "Stress test complete at $(date)" >> "$LOG_FILE"
