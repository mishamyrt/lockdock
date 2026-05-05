#!/bin/sh
set -eu

VERSION="$1"
TMPDIR="$(mktemp -d /tmp/lockdock-smoke.XXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT HUP INT TERM

run_expect() {
    expected_status="$1"
    name="$2"
    shift 2

    stdout_file="$TMPDIR/$name.stdout"
    stderr_file="$TMPDIR/$name.stderr"

    if "$@" >"$stdout_file" 2>"$stderr_file"; then
        status=0
    else
        status=$?
    fi

    if [ "$status" -ne "$expected_status" ]; then
        echo "Smoke test '$name' failed: expected exit $expected_status, got $status" >&2
        echo "--- stdout ---" >&2
        cat "$stdout_file" >&2
        echo "--- stderr ---" >&2
        cat "$stderr_file" >&2
        exit 1
    fi
}

assert_contains() {
    file="$1"
    text="$2"

    if ! grep -F "$text" "$file" >/dev/null 2>&1; then
        echo "Expected '$text' in $file" >&2
        echo "--- file contents ---" >&2
        cat "$file" >&2
        exit 1
    fi
}

run_expect 0 cli_version ./build/lockdock version
assert_contains "$TMPDIR/cli_version.stdout" "lockdock $VERSION"

run_expect 0 cli_help ./build/lockdock help
assert_contains "$TMPDIR/cli_help.stdout" "Usage:"

run_expect 1 cli_missing_args ./build/lockdock
assert_contains "$TMPDIR/cli_missing_args.stdout" "Usage:"

run_expect 1 cli_unknown ./build/lockdock nope
assert_contains "$TMPDIR/cli_unknown.stderr" "Unknown command: nope"

run_expect 1 cli_lock_missing_target ./build/lockdock lock
assert_contains "$TMPDIR/cli_lock_missing_target.stderr" "Usage:"

run_expect 0 daemon_version ./build/lockdockd version
assert_contains "$TMPDIR/daemon_version.stdout" "lockdockd $VERSION"

run_expect 0 daemon_help ./build/lockdockd help
assert_contains "$TMPDIR/daemon_help.stdout" "Usage:"

run_expect 1 daemon_unknown ./build/lockdockd nope
assert_contains "$TMPDIR/daemon_unknown.stderr" "Unknown command: nope"
