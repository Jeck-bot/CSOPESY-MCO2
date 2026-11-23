CSOPESY MCO2 - Multitasking OS
==============================

Group members
-------------
Campo, Benette Enzo
Garcia, Andrea Gayle
Morgan, Jack Owen
Tenorio, Jeroen Ralph

Entry point
-----------
src/main.cpp contains the main() function.

Requirements
------------
Windows with Visual Studio 2022 (or the Visual Studio 2022 Build Tools)
and the "Desktop development with C++" workload.

Build
-----
Visual Studio: open CSOPESY.vcxproj, select Release | x64, then
Build > Build Solution.

Command line (Developer Command Prompt for VS 2022):
    msbuild CSOPESY.vcxproj /p:Configuration=Release /p:Platform=x64

Run
---
Run from this folder so the program finds config.txt:
    x64\Release\CSOPESY.exe
or press Ctrl+F5 in Visual Studio.

Type "initialize" first. It loads config.txt, which can be edited
without recompiling.

config.txt
----------
Space-separated "key value" pairs. Missing keys keep their defaults.
    num-cpu, scheduler, quantum-cycles, batch-process-freq, min-ins, max-ins,
    delay-per-exec               as in MCO1
    max-overall-mem              total main memory in bytes (default 16384)
    mem-per-frame                frame and page size in bytes (default 256)
    min-mem-per-proc             memory of scheduler-start processes, rolled
    max-mem-per-proc             from this range (defaults 4096 and 4096)
Memory sizes are powers of 2 from 64 to 65536 bytes.

Commands
--------
Main menu:
    initialize                 load config.txt (required before other commands)
    screen -s <process name> <memory size>
                               create a process and open its screen
    screen -c <process name> [<memory size>] "<instructions>"
                               create a process that runs 1 to 50 instructions,
                               separated by semicolons, and open its screen;
                               without a size, one is rolled as for
                               scheduler-start
    screen -s <process name>   reopen a running process's screen
    screen -r <process name>   reopen a running process's screen, or say why
                               a process shut down
    screen -ls                 show CPU utilization and process lists
    process-smi                show CPU and memory use and each process's memory
    vmstat                     show memory, CPU tick and paging counts
    scheduler-start            start generating dummy processes
    scheduler-stop             stop generating dummy processes
    report-util                save the screen -ls report to csopesy-log.txt
    exit                       quit

Process screen:
    process-smi                show process info and logs
    exit                       return to the main menu

screen -c instructions (keywords ignore case; DECLARE(x, 5) style also works):
    DECLARE x 5                ADD x y 1            SUBTRACT x y z
    READ x 0x500               WRITE 0x500 x        SLEEP 3
    PRINT("text" + x)          FOR([ADD x x 1; PRINT(x)], 3)
Example:
    screen -c process2 "DECLARE varA 10; DECLARE varB 5; ADD varA varA varB; WRITE 0x500 varA; READ varC 0x500; PRINT(\"Result: \" + varC)"

Memory
------
Each process has its own memory. Its first 64 bytes hold up to 32 uint16
variables; READ and WRITE use addresses (in hex) anywhere in its memory.
Using an address outside it shuts the process down.

Memory is demand-paged. A page is loaded into a frame when an instruction
first needs it, which costs a CPU tick (a page fault). When no frame is free,
the page loaded longest ago is moved to the backing store,
csopesy-backing-store.txt in the folder the program runs from. The file
lists every swapped-out page and can be opened at any time.
