#!/usr/bin/env bash

set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="$ROOT_DIR/bin"
MODULE_NAME="jne_demo"
MODULE_PATH="$BIN_DIR/$MODULE_NAME.ko"
DEVICE_PATH="/dev/jne_demo"
DEBUGFS_ROOT="/sys/kernel/debug"
DEBUGFS_DIR="$DEBUGFS_ROOT/jne_demo"
STATUS_INTERVAL_PATH="/sys/module/$MODULE_NAME/parameters/status_interval_ms"

TEMP_DIR="$(mktemp -d)"
DEBUGFS_MOUNTED_BY_TEST=false
poll_pid=""

cleanup()
{
if [[ -n "$poll_pid" ]]; then
kill "$poll_pid" 2>/dev/null || true
wait "$poll_pid" 2>/dev/null || true
fi

```
sudo rmmod "$MODULE_NAME" 2>/dev/null || true
rm -rf "$TEMP_DIR"

if [[ "$DEBUGFS_MOUNTED_BY_TEST" == true ]]; then
    sudo umount "$DEBUGFS_ROOT" 2>/dev/null || true
fi
```

}

fail()
{
echo "FAIL: $1" >&2
exit 1
}

pass()
{
echo "PASS: $1"
}

get_stat()
{
    local name="$1"
    local content="$2"

    awk -F ': ' -v name="$name" '$1 == name { print $2 }' <<< "$content"
}

trap cleanup EXIT

cd "$ROOT_DIR"

echo "Building project..."
make

[[ -f "$MODULE_PATH" ]] || fail "Kernel module was not built"

if ! grep -qs " $DEBUGFS_ROOT debugfs " /proc/mounts; then
sudo mount -t debugfs none "$DEBUGFS_ROOT"
DEBUGFS_MOUNTED_BY_TEST=true
fi

sudo rmmod "$MODULE_NAME" 2>/dev/null || true
sudo insmod "$MODULE_PATH"

for _ in $(seq 1 20); do
[[ -e "$DEVICE_PATH" ]] && break
sleep 0.1
done

[[ -c "$DEVICE_PATH" ]] || fail "$DEVICE_PATH was not created"
pass "Module loading and device creation"

"$BIN_DIR/ioctl_test" >/dev/null

sudo test -d "$DEBUGFS_DIR" ||
    fail "debugfs directory was not created"

sudo test -f "$DEBUGFS_DIR/status" ||
    fail "debugfs status file was not created"

sudo test -f "$DEBUGFS_DIR/messages" ||
    fail "debugfs messages file was not created"

sudo test -f "$DEBUGFS_DIR/stats" ||
    fail "debugfs stats file was not created"

status_output="$(sudo cat "$DEBUGFS_DIR/status")"

grep -q "^messages: 0$" <<< "$status_output" ||
fail "debugfs status contains an unexpected message count"

grep -q "^capacity: 16$" <<< "$status_output" ||
fail "debugfs status contains an unexpected queue capacity"

grep -q "^available: 16$" <<< "$status_output" ||
fail "debugfs status contains an unexpected available count"

grep -q "^status_reporting: enabled$" <<< "$status_output" ||
fail "debugfs status reporting state is incorrect"

grep -q "^status_interval_ms: 5000$" <<< "$status_output" ||
fail "debugfs status interval is incorrect"

pass "debugfs status interface"

nonblock_output="$("$BIN_DIR/nonblock_read")"

[[ "$nonblock_output" == "No data available" ]] ||
fail "Non-blocking read did not report an empty queue"

pass "Non-blocking read from empty queue"

printf 'Debug message' > "$DEVICE_PATH"

messages_output="$(sudo cat "$DEBUGFS_DIR/messages")"

grep -q "^messages: 1$" <<< "$messages_output" ||
fail "debugfs messages file contains an unexpected message count"

grep -Fqx '[0] length=13 data="Debug message"' <<< "$messages_output" ||
    fail "debugfs messages file does not contain the expected message"

messages_output_again="$(sudo cat "$DEBUGFS_DIR/messages")"

[[ "$messages_output_again" == "$messages_output" ]] ||
fail "Reading debugfs messages modified the queue"

actual_message="$(timeout 2 cat "$DEVICE_PATH")"

[[ "$actual_message" == "Debug message" ]] ||
fail "debugfs messages read modified the queued message"

pass "debugfs queue inspection"

stats_before="$(sudo cat "$DEBUGFS_DIR/stats")"

opens_before="$(get_stat opens "$stats_before")"
closes_before="$(get_stat closes "$stats_before")"
messages_read_before="$(get_stat messages_read "$stats_before")"
messages_written_before="$(get_stat messages_written "$stats_before")"
bytes_read_before="$(get_stat bytes_read "$stats_before")"
bytes_written_before="$(get_stat bytes_written "$stats_before")"
async_jobs_before="$(get_stat async_jobs "$stats_before")"

for value in \
    "$opens_before" \
    "$closes_before" \
    "$messages_read_before" \
    "$messages_written_before" \
    "$bytes_read_before" \
    "$bytes_written_before" \
    "$async_jobs_before"; do
    [[ "$value" =~ ^[0-9]+$ ]] ||
        fail "debugfs stats contains an invalid counter"
done

printf 'Stats message' > "$DEVICE_PATH"

stats_message="$(timeout 2 cat "$DEVICE_PATH")"

[[ "$stats_message" == "Stats message" ]] ||
    fail "Statistics test did not read the expected message"

async_jobs_expected=$((async_jobs_before + 1))

for _ in $(seq 1 20); do
    stats_after="$(sudo cat "$DEBUGFS_DIR/stats")"
    async_jobs_after="$(get_stat async_jobs "$stats_after")"

    if [[ "$async_jobs_after" == "$async_jobs_expected" ]]; then
        break
    fi

    sleep 0.05
done

opens_after="$(get_stat opens "$stats_after")"
closes_after="$(get_stat closes "$stats_after")"
messages_read_after="$(get_stat messages_read "$stats_after")"
messages_written_after="$(get_stat messages_written "$stats_after")"
bytes_read_after="$(get_stat bytes_read "$stats_after")"
bytes_written_after="$(get_stat bytes_written "$stats_after")"
async_jobs_after="$(get_stat async_jobs "$stats_after")"

[[ "$opens_after" -eq $((opens_before + 2)) ]] ||
    fail "Global open counter is incorrect"

[[ "$closes_after" -eq $((closes_before + 2)) ]] ||
    fail "Global close counter is incorrect"

[[ "$messages_read_after" -eq $((messages_read_before + 1)) ]] ||
    fail "Global messages-read counter is incorrect"

[[ "$messages_written_after" -eq $((messages_written_before + 1)) ]] ||
    fail "Global messages-written counter is incorrect"

[[ "$bytes_read_after" -eq $((bytes_read_before + 13)) ]] ||
    fail "Global bytes-read counter is incorrect"

[[ "$bytes_written_after" -eq $((bytes_written_before + 13)) ]] ||
    fail "Global bytes-written counter is incorrect"

[[ "$async_jobs_after" -eq "$async_jobs_expected" ]] ||
    fail "Global asynchronous-job counter is incorrect"

pass "debugfs global statistics"

echo 1000 | sudo tee "$STATUS_INTERVAL_PATH" >/dev/null

actual_interval="$(cat "$STATUS_INTERVAL_PATH")"

[[ "$actual_interval" == "1000" ]] ||
fail "status interval was not changed through sysfs"

if echo 10 | sudo tee "$STATUS_INTERVAL_PATH" >/dev/null 2>&1; then
fail "Invalid status interval was accepted"
fi

actual_interval="$(cat "$STATUS_INTERVAL_PATH")"

[[ "$actual_interval" == "1000" ]] ||
fail "Invalid write changed the status interval"

pass "Validated runtime module parameter"

printf 'Message one\n' > "$DEVICE_PATH"
printf 'Message two\n' > "$DEVICE_PATH"
printf 'Message three\n' > "$DEVICE_PATH"

first="$(timeout 2 cat "$DEVICE_PATH")"
second="$(timeout 2 cat "$DEVICE_PATH")"
third="$(timeout 2 cat "$DEVICE_PATH")"

[[ "$first" == "Message one" ]] || fail "First FIFO message is incorrect"
[[ "$second" == "Message two" ]] || fail "Second FIFO message is incorrect"
[[ "$third" == "Message three" ]] || fail "Third FIFO message is incorrect"

pass "FIFO message order"

"$BIN_DIR/ioctl_test" >/dev/null

timeout 3 "$BIN_DIR/poll_read" > "$TEMP_DIR/poll-output.txt" &
poll_pid=$!

sleep 0.2
printf 'Message through poll\n' > "$DEVICE_PATH"

wait "$poll_pid" || fail "poll_read timed out or failed"
poll_pid=""

grep -q "Message through poll" "$TEMP_DIR/poll-output.txt" ||
fail "poll_read did not receive the expected message"

pass "poll() notification"

printf 'Message for ioctl\n' > "$DEVICE_PATH"
"$BIN_DIR/ioctl_test" > "$TEMP_DIR/ioctl-output.txt"

grep -q "Buffer length:" "$TEMP_DIR/ioctl-output.txt" ||
fail "GET_LENGTH did not return a result"

grep -q "Buffer cleared" "$TEMP_DIR/ioctl-output.txt" ||
fail "CLEAR did not complete"

grep -q "Buffer length: 0 bytes" "$TEMP_DIR/ioctl-output.txt" ||
fail "Queue is not empty after CLEAR"

pass "ioctl commands"

"$BIN_DIR/file_state_test" > "$TEMP_DIR/state-output.txt"

grep -q "Messages read:    1" "$TEMP_DIR/state-output.txt" ||
fail "Per-open read statistics are incorrect"

grep -q "Messages written: 2" "$TEMP_DIR/state-output.txt" ||
fail "Per-open write statistics are incorrect"

pass "Per-open statistics"

"$BIN_DIR/ioctl_test" >/dev/null

for index in $(seq 1 16); do
printf 'Queue message %02d\n' "$index" > "$DEVICE_PATH"
done

nonblock_write_output="$("$BIN_DIR/nonblock_write")"

[[ "$nonblock_write_output" == "Queue is full" ]] ||
fail "Non-blocking write did not report a full queue"

pass "Non-blocking write to full queue"

"$BIN_DIR/ioctl_test" >/dev/null

sudo rmmod "$MODULE_NAME"

[[ ! -e "$DEVICE_PATH" ]] ||
fail "$DEVICE_PATH still exists after module removal"

if sudo test -e "$DEBUGFS_DIR"; then
    fail "$DEBUGFS_DIR still exists after module removal"
fi

pass "Module unloading and device removal"

trap - EXIT
rm -rf "$TEMP_DIR"

if [[ "$DEBUGFS_MOUNTED_BY_TEST" == true ]]; then
sudo umount "$DEBUGFS_ROOT"
fi

echo
echo "All integration tests passed."
