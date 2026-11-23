#!/usr/bin/env bash
# Black-box checks of the CSOPESY CLI.
# Usage, from the repo root after a Release x64 build: bash tests/cli.sh

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXE="$ROOT/x64/Release/CSOPESY.exe"
WORK="$ROOT/tests/work"
OUT="$WORK/out.txt"     # raw program output
CLEAN="$WORK/clean.txt" # output without CR, form feeds (cls) and prompts
passed=0
failed=0

# run "<config.txt contents, or empty for none>" "<shell commands that echo CLI input>"
run() {
    rm -rf "$WORK" && mkdir -p "$WORK"
    if [ -n "$1" ]; then printf '%s\n' "$1" > "$WORK/config.txt"; fi
    (cd "$WORK" && eval "$2" | timeout 120 "$EXE" > "$OUT" 2>&1)
    status=$?
    tr -d '\r\f' < "$OUT" | sed 's/root:\\> //g' > "$CLEAN"
}

pass() { passed=$((passed + 1)); }
fail() { failed=$((failed + 1)); echo "FAIL: $1"; }
expect() { if grep -qE -- "$2" "$CLEAN"; then pass; else fail "$1"; fi; }
reject() { if grep -qE -- "$2" "$CLEAN"; then fail "$1"; else pass; fi; }
verify() { if "${@:2}"; then pass; else fail "$1"; fi; }
count() { grep -cE -- "$1" "$CLEAN"; }
runningOnCoreZero() { grep -E 'Core: 0 ' "$CLEAN" | awk '{print $1}' | sort -u | wc -l; }
# Instructions run between the first two process-smi outputs. Measuring between two samples
# taken while the program runs keeps the result independent of how long it takes to start.
progressBetweenSamples() {
    grep -E '^Current instruction line' "$CLEAN" | grep -oE '[0-9]+$' | awk 'NR == 1 {a = $1} NR == 2 {b = $1} END {print b - a}'
}

DEFAULT=$'num-cpu 4\nscheduler "rr"\nquantum-cycles 5\nbatch-process-freq 1\nmin-ins 1000\nmax-ins 2000\ndelay-per-exec 0'
QUICK=$'num-cpu 4\nscheduler "rr"\nquantum-cycles 5\nbatch-process-freq 1\nmin-ins 60\nmax-ins 60\ndelay-per-exec 0'
LONG=$'num-cpu 1\nscheduler "fcfs"\nquantum-cycles 5\nbatch-process-freq 1\nmin-ins 100000\nmax-ins 100000\ndelay-per-exec 0'
BATCH=$'num-cpu 4\nscheduler "rr"\nquantum-cycles 5\nbatch-process-freq 1\nmin-ins 1000\nmax-ins 1000\ndelay-per-exec 0'
HOLD=$'num-cpu 3\nscheduler "fcfs"\nquantum-cycles 5\nbatch-process-freq 1\nmin-ins 1000\nmax-ins 1000\ndelay-per-exec 4294967296'
STRESS=$'num-cpu 128\nscheduler "rr"\nquantum-cycles 2\nbatch-process-freq 1\nmin-ins 1\nmax-ins 50\ndelay-per-exec 0'
TINY=$'num-cpu 4\nscheduler "rr"\nquantum-cycles 5\nbatch-process-freq 1\nmin-ins 100\nmax-ins 100\ndelay-per-exec 0\nmax-overall-mem 256\nmem-per-frame 64\nmin-mem-per-proc 256\nmax-mem-per-proc 1024'
MEMORY_DEFAULTS='max-overall-mem 16384, mem-per-frame 256, min-mem-per-proc 4096, max-mem-per-proc 4096'
# The spec's sample screen -c program, as typed in the console (with \" inside the quotes).
SPEC='DECLARE varA 10; DECLARE varB 5; ADD varA varA varB; WRITE 0x500 varA; READ varC 0x500; PRINT(\"Result: \" + varC)'

# Only "initialize" and "exit" work before initialize.
run "$DEFAULT" "echo 'screen -ls'; echo scheduler-start; echo exit"
verify "exit works before initialize" test "$status" -eq 0
verify "commands before initialize are refused" test "$(count '^Please run "initialize" first\.$')" -eq 2
expect "banner lists the developers" '^Campo, Benette Enzo$'
verify "prompt is root:\\>" grep -q 'root:\\> ' "$OUT"

# initialize reads config.txt.
run "$DEFAULT" "echo initialize; echo initialize; echo exit"
expect "initialize summary" "^Initialized: num-cpu 4, scheduler rr, quantum-cycles 5, batch-process-freq 1, min-ins 1000, max-ins 2000, delay-per-exec 0, $MEMORY_DEFAULTS\$"
expect "second initialize" '^Already initialized\.$'
verify "backing store file created" test -f "$WORK/csopesy-backing-store.txt"

# The memory parameters are read and checked.
run "$TINY" "echo initialize; echo exit"
expect "memory parameters" '^Initialized: .*, max-overall-mem 256, mem-per-frame 64, min-mem-per-proc 256, max-mem-per-proc 1024$'
run "${TINY/mem-per-frame 64/mem-per-frame 100}" "echo initialize; echo exit"
expect "memory sizes must be powers of 2" 'Invalid mem-per-frame "100": must be a power of 2 from 64 to 65536\.'

# A bad or missing config.txt leaves the console uninitialized.
run 'num-cpu 0' "echo initialize; echo 'screen -ls'; echo exit"
expect "invalid value reported" 'Invalid num-cpu "0"'
expect "still uninitialized after a bad config" '^Please run "initialize" first\.$'
run "" "echo initialize; echo exit"
expect "missing config reported" 'Could not open config\.txt'

# One-line config, quoted upper-case scheduler, and the "delays-per-exec" spelling.
run 'num-cpu 2 scheduler "FCFS" quantum-cycles 3 batch-process-freq 4 min-ins 5 max-ins 6 delays-per-exec 7' "echo initialize; echo exit"
expect "config variants" "^Initialized: num-cpu 2, scheduler fcfs, quantum-cycles 3, batch-process-freq 4, min-ins 5, max-ins 6, delay-per-exec 7, $MEMORY_DEFAULTS\$"

# screen -s, process-smi, Finished!, then the finished process can't be opened again.
run "$QUICK" "echo initialize; echo 'screen -s A 256'; sleep 3; echo process-smi; echo exit; echo 'screen -r A'; echo 'screen -s A 256'; echo exit"
expect "process name" '^Process name: A$'
expect "process id" '^ID: 1$'
expect "logs header" '^Logs:$'
expect "log line format" '^\([0-9]{2}/[0-9]{2}/[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}(AM|PM)\) Core:[0-9]+ "Hello world from A!"$'
expect "finished" '^Finished!$'
expect "screen -r on a finished process" '^Process A not found\.$'
expect "screen -s on a finished name" '^Process A already exists\.$'

# screen -s needs a memory size that is a power of 2 from 64 to 65536.
run "$QUICK" "echo initialize; echo 'screen -s a 100'; echo 'screen -s b 131072'; echo 'screen -s c 32'; echo 'screen -s d abc'; echo 'screen -s e'; echo 'screen -s'; echo exit"
verify "invalid memory sizes" test "$(count '^invalid memory allocation$')" -eq 4
verify "screen -s usage" test "$(count '^Usage: screen -s <process name> <memory size>$')" -eq 2
reject "no process was made by screen -s" '^Process name:'

# A running process shows its progress and can be reopened with screen -s or screen -r.
run "$LONG" "echo initialize; echo 'screen -s B 1024'; sleep 1; echo process-smi; echo exit; echo 'screen -s B'; echo exit; echo 'screen -r B'; echo exit; echo 'screen -r nobody'; echo exit"
expect "current instruction line" '^Current instruction line: [0-9]+$'
expect "lines of code" '^Lines of code: 100000$'
verify "running process reopens" test "$(count '^Process name: B$')" -ge 4
expect "unknown process" '^Process nobody not found\.$'
reject "running name is not refused" 'already exists'

# Unknown commands, in the main menu and inside a screen.
run "$LONG" "echo initialize; echo foo; echo 'screen -s C 1024'; echo 'screen -ls'; echo exit; echo exit"
verify "unknown commands" test "$(count '^Command not recognized\.$')" -eq 2

# Commands ignore case; the spec's other spellings work.
run "$QUICK" "echo INITIALIZE; echo 'Screen -LS'; echo screen-ls; echo scheduler-test; echo scheduler-stop; echo exit"
verify "screen -ls spellings" test "$(count '^Running processes:$')" -eq 2
expect "scheduler-test starts generation" '^Scheduler started generating processes\.$'
expect "scheduler-stop stops generation" '^Scheduler stopped generating processes\.$'

# scheduler-start makes p01, p02, ... every batch cycle (200 ms) here; scheduler-stop ends it. About 2 s
# of generation makes about 10 processes, so p450 can only exist if generation went on after the stop.
run "$BATCH" "echo initialize; echo scheduler-start; echo scheduler-start; sleep 2; echo scheduler-stop; echo scheduler-stop; sleep 4; echo 'screen -ls'; echo 'screen -r p05'; echo exit; echo 'screen -r p450'; echo exit"
expect "already generating" '^Scheduler is already generating processes\.$'
expect "not generating" '^Scheduler is not generating processes\.$'
expect "all cores busy" '^CPU utilization: 100%$'
expect "cores used" '^Cores used: 4$'
expect "cores available" '^Cores available: 0$'
expect "running row format" '^p[0-9]{2,} +\([0-9]{2}/[0-9]{2}/[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}(AM|PM)\)  Core: 0 +[0-9]+ / 1000$'
expect "generated process opens" '^Process name: p05$'
expect "generation stopped" '^Process p450 not found\.$'

# Utilization percentage with a fraction.
run "$HOLD" "echo initialize; echo 'screen -s A 1024'; echo exit; echo 'screen -s B 1024'; echo exit; sleep 1; echo 'screen -ls'; echo exit"
expect "two of three cores" '^CPU utilization: 66\.67%$'
expect "cores used 2" '^Cores used: 2$'
expect "cores available 1" '^Cores available: 1$'

# report-util saves exactly what screen -ls prints.
run "$HOLD" "echo initialize; echo 'screen -s A 1024'; echo exit; sleep 1; echo 'screen -ls'; echo report-util; echo exit"
expect "report path" '^Report generated at .*/tests/work/csopesy-log\.txt!$'
verify "report file written" test -f "$WORK/csopesy-log.txt"
listed=$(awk '/^CPU utilization/{on=1; d=0} on{print} on && /^-{40}$/{d++; if (d == 2) on=0}' "$CLEAN")
verify "report matches screen -ls" test "$listed" == "$(tr -d '\r' < "$WORK/csopesy-log.txt")"

# FCFS keeps a busy-waiting process on its core; RR rotates the two processes.
cmds="echo initialize; echo 'screen -s A 1024'; echo exit; echo 'screen -s B 1024'; echo exit; sleep 1"
for i in 1 2 3 4 5 6 7 8 9 10; do cmds="$cmds; echo 'screen -ls'; sleep 0.2"; done
run "${HOLD/num-cpu 3/num-cpu 1}" "$cmds; echo exit"
verify "fcfs keeps one process on the core" test "$(runningOnCoreZero)" -eq 1
run "$(printf '%s' "${HOLD/num-cpu 3/num-cpu 1}" | sed 's/"fcfs"/"rr"/')" "$cmds; echo exit"
verify "rr rotates processes" test "$(runningOnCoreZero)" -eq 2

# delay-per-exec slows a process down.
samples="echo initialize; echo 'screen -s A 1024'; sleep 1; echo process-smi; sleep 2; echo process-smi; echo exit; echo exit"
run "$LONG" "$samples"
fast=$(progressBetweenSamples)
run "${LONG/delay-per-exec 0/delay-per-exec 9}" "$samples"
slow=$(progressBetweenSamples)
verify "delay slows progress ($fast vs $slow)" test "${fast:-0}" -gt $(( ${slow:-0} * 3 ))

# screen -c runs the spec's sample program, with a memory size or a rolled one (4096 by default).
spec_program() {
    echo initialize
    printf '%s\n' "screen -c p2 4096 \"$SPEC\""
    sleep 1; echo process-smi; echo exit
    printf '%s\n' "screen -c process2 \"$SPEC\""
    sleep 1; echo process-smi; echo exit; echo exit
}
run "$QUICK" spec_program
# The screen that screen -c opens may already show the end, so count at least one per process.
verify "spec program prints Result: 15" test "$(count 'Core:[0-9]+ "Result: 15"$')" -ge 2
verify "spec programs finish" test "$(count '^Finished!$')" -ge 2
expect "screen -c opens the process" '^Process name: process2$'

# screen -c needs 1 to 50 valid instructions in quotes, and a valid memory size if one is given.
FIFTY_ONE=$(for i in $(seq 51); do printf 'SLEEP 1; '; done)
bad_programs() {
    echo initialize
    echo 'screen -c q 256 ""'
    printf '%s\n' "screen -c q 256 \"$FIFTY_ONE\""
    echo 'screen -c q 256 "FOO 1"'
    echo 'screen -c q'
    echo 'screen -c q 100 "SLEEP 1"'
    echo exit
}
run "$QUICK" bad_programs
verify "invalid screen -c commands" test "$(count '^invalid command$')" -eq 4
expect "invalid screen -c memory size" '^invalid memory allocation$'
reject "no process was made by screen -c" '^Process name:'

# A memory access violation shuts the process down; process-smi and screen -r report it.
run "$QUICK" "echo initialize; echo 'screen -c f 64 \"WRITE 0x500 1\"'; sleep 1; echo process-smi; echo exit; echo 'screen -r f'; echo 'screen -ls'; echo exit"
expect "violation on screen -r" '^Process f shut down due to memory access violation error that occurred at [0-9]{2}:[0-9]{2}:[0-9]{2}\. 0x500 invalid\.$'
verify "violation in process-smi too" test "$(count '^Process f shut down due to memory access violation')" -ge 2
expect "shut-down process listed with its progress" '^f +\(.*\)  Finished  0 / 1$'

# Main menu process-smi and vmstat. A runs DECLARE (page 0, 256 bytes) and then busy-waits.
run "$HOLD" "echo initialize; echo 'screen -c A 1024 \"DECLARE x 1; PRINT(x)\"'; echo exit; sleep 1; echo process-smi; echo vmstat; echo exit"
expect "process-smi header" '^\| PROCESS-SMI V01\.00 Driver Version: 01\.00 \|$'
expect "process-smi cpu" '^CPU-Util: 33\.33%$'
expect "process-smi memory usage" '^Memory Usage: 256B / 16384B$'
expect "process-smi memory util" '^Memory Util: 1\.56%$'
expect "process-smi process list" '^Running processes and memory usage:$'
expect "process-smi row" '^A 256B$'
expect "vmstat total memory" '^ +16384 B total memory$'
expect "vmstat used memory" '^ +256 B used memory$'
expect "vmstat free memory" '^ +16128 B free memory$'
for label in 'idle cpu ticks' 'active cpu ticks' 'total cpu ticks' 'pages paged in' 'pages paged out'; do
    expect "vmstat $label" "^ +[0-9]+ $label\$"
done

# With 4 frames for many processes, pages go to the backing store and come back.
run "$TINY" "echo initialize; echo scheduler-start; sleep 3; echo scheduler-stop; echo vmstat; echo exit"
pagedOut=$(grep -E 'pages paged out$' "$CLEAN" | grep -oE '[0-9]+' | head -1)
verify "pages paged out when memory is short (${pagedOut:-none})" test "${pagedOut:-0}" -gt 0
expect "tiny total memory" '^ +256 B total memory$'
verify "backing store lists swapped-out pages" test "$(grep -c ' page [0-9]*:' "$WORK/csopesy-backing-store.txt")" -gt 0

# 128 cores with nonstop batch generation stays responsive and exits cleanly.
run "$STRESS" "echo initialize; echo scheduler-start; sleep 20; echo 'screen -ls'; echo scheduler-stop; echo exit"
expect "stress run reports" '^Cores used: [0-9]+$'
verify "stress run exits cleanly" test "$status" -eq 0

# The end of input closes the console.
run "$QUICK" "echo initialize"
verify "end of input exits" test "$status" -eq 0

echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
