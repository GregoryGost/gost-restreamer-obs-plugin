# Terminal C++ debugging — GDB, LLDB, CDB

Optional, targeted runtime diagnosis only. Source analysis, a failing test and actual program side effects must be established first; starting a debugger launches the program or changes a process.

## Before a session

1. Record observed failure, reproduction, test/fixture, source revision and expected behavior.
2. Use the target project's declared debugger/executable/build profile. Verify debugger/compiler version and symbol availability (e.g. -g, debug PDB; optimizations can alter variable visibility).
3. Identify all effects of starting the program/test: files, plugins, camera/microphone, network, shared state, DB, ports, credentials, production environment or live GUI. Execute only if explicitly permitted.
4. Verify the terminal supports persistent interactive stdin into the same process before starting an interactive debugger; do not leave it hanging when only output streaming works.
5. No installations, ptrace policy or elevation changes, debug listeners or attach to running/shared/production processes by default.

## Example terminal workflow

For an already authorized isolated program:

    gdb --args /approved/path/program --test-mode
    (gdb) break ClassName::method
    (gdb) run
    (gdb) bt
    (gdb) frame 0
    (gdb) print variable
    (gdb) quit

Or with LLDB:

    lldb -- /approved/path/program --test-mode
    (lldb) breakpoint set --name ClassName::method
    (lldb) run
    (lldb) bt
    (lldb) frame variable
    (lldb) quit

CDB/WinDbg commands and VS Code debug-adapter launch configurations are platform-specific; use an actually configured supported adapter and verified debuggee path. Commands are illustrative and debugger invocations must match installed versions.

## Noninteractive fallback

If no persistent stdin is supported, use the approved test command, bounded logging, existing backtrace, crash dump/core analysis if authorized, and selected sanitizers. Do not substitute a hidden debugger script with uncontrolled commands or attach to existing services.

## Closure

Record verified root cause or unresolved competing hypotheses, clean up only self-created processes/artifacts and rerun normal tests/build on the final source. Debugger breakpoints and runtime inspection are not test acceptance. Never disclose secrets from stack frames in a PR.
