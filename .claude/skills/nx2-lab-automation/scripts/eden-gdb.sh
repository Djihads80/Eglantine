#!/bin/sh
# eden-cli under gdb, for `lab_ctl launch --binary <this>`: SIGINT to eden-cli stops it and dumps
# every thread's stack to stdout (emulator.log). dynarmic uses SIGSEGV for guest memory: pass it.
REPO=${REPO:-/home/kraibse/clones/Eglantine}
exec gdb -q -batch -nx \
  -ex 'set pagination off' -ex 'set print thread-events off' \
  -ex 'handle SIGSEGV nostop noprint pass' -ex 'handle SIGBUS nostop noprint pass' \
  -ex 'handle SIGUSR1 nostop noprint pass' -ex 'handle SIGUSR2 nostop noprint pass' \
  -ex 'handle SIGPIPE nostop noprint pass' -ex 'handle SIG32 nostop noprint pass' \
  -ex 'handle SIG33 nostop noprint pass' -ex 'handle SIG34 nostop noprint pass' \
  -ex run \
  -ex 'echo \n=====GDB-THREADS=====\n' -ex 'thread apply all bt 25' -ex 'kill' \
  --args "$REPO/build/bin/eden-cli" "$@"
