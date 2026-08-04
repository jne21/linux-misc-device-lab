#!/usr/bin/env bash

set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="$ROOT_DIR/bin"

MODULE_NAME="jne_demo"
MODULE_PATH="$BIN_DIR/$MODULE_NAME.ko"
DEVICE_PATH="/dev/jne_demo"

DEBUGFS_ROOT="/sys/kernel/debug"
DEBUGFS_DIR="$DEBUGFS_ROOT/jne_demo"
DEBUGFS_STATUS_PATH="$DEBUGFS_DIR/status"
DEBUGFS_MESSAGES_PATH="$DEBUGFS_DIR/messages"
DEBUGFS_STATS_PATH="$DEBUGFS_DIR/stats"

STATUS_INTERVAL_PATH="/sys/module/$MODULE_NAME/parameters/status_interval_ms"

TEMP_DIR="$(mktemp -d)"
DEBUGFS_MOUNTED_BY_TEST=false
POLL_PID=""

fail()
{
    echo "FAIL: $1" >&2
    exit 1
}

pass()
{
    echo "PASS: $1"
}

assert_equals()
{
    local expected="$1"
    local actual="$2"
    local message="$3"

    if [[ "$actual" != "$expected" ]]; then
        fail "$message: expected '$expected', got '$actual'"
    fi
}

assert_file_contains_line()
{
    local file="$1"
    local expected="$2"
    local message="$3"

    if ! grep -Fqx "$expected" "$file"; then
        fail "$message"
    fi
}

assert_text_contains_line()
{
    local text="$1"
    local expected="$2"
    local message="$3"

    if ! grep -Fqx "$expected" <<< "$text"; then
        fail "$message"
    fi
}

get_stat()
{
    local name="$1"
    local content="$2"

    awk -F ': ' -v name="$name" '$1 == name { print $2 }' <<< "$content"
}

assert_unsigned_integer()
{
    local value="$1"
    local message="$2"

    if [[ ! "$value" =~ ^[0-9]+$ ]]; then
        fail "$message: '$value'"
    fi
}

clear_queue()
{
    "$BIN_DIR/ioctl_test" >/dev/null
}

wait_for_device()
{
    local attempt

    for attempt in $(seq 1 20); do
        if [[ -c "$DEVICE_PATH" ]]; then
            return 0
        fi

        sleep 0.1
    done

    fail "$DEVICE_PATH was not created"
}

wait_for_stat()
{
    local name="$1"
    local expected="$2"
    local attempt
    local stats
    local actual

    for attempt in $(seq 1 20); do
        stats="$(sudo cat "$DEBUGFS_STATS_PATH")"
        actual="$(get_stat "$name" "$stats")"

        if [[ "$actual" == "$expected" ]]; then
            printf '%s' "$stats"
            return 0
        fi

        sleep 0.05
    done

    fail "Timed out waiting for $name=$expected"
}

mount_debugfs()
{
    if grep -qs " $DEBUGFS_ROOT debugfs " /proc/mounts; then
        return
    fi

    sudo mount -t debugfs none "$DEBUGFS_ROOT"
    DEBUGFS_MOUNTED_BY_TEST=true
}

load_module()
{
    sudo rmmod "$MODULE_NAME" 2>/dev/null || true
    sudo insmod "$MODULE_PATH"
    wait_for_device
}

unload_module()
{
    sudo rmmod "$MODULE_NAME"
}

cleanup()
{
    if [[ -n "$POLL_PID" ]]; then
        kill "$POLL_PID" 2>/dev/null || true
        wait "$POLL_PID" 2>/dev/null || true
    fi

    sudo rmmod "$MODULE_NAME" 2>/dev/null || true
    rm -rf "$TEMP_DIR"

    if [[ "$DEBUGFS_MOUNTED_BY_TEST" == true ]]; then
        sudo umount "$DEBUGFS_ROOT" 2>/dev/null || true
    fi
}

test_module_loading()
{
    [[ -c "$DEVICE_PATH" ]] || fail "$DEVICE_PATH was not created"
    pass "Module loading and device creation"
}

test_debugfs_status()
{
    local status_output

    sudo test -d "$DEBUGFS_DIR" ||
        fail "debugfs directory was not created"

    sudo test -f "$DEBUGFS_STATUS_PATH" ||
        fail "debugfs status file was not created"

    sudo test -f "$DEBUGFS_MESSAGES_PATH" ||
        fail "debugfs messages file was not created"

    sudo test -f "$DEBUGFS_STATS_PATH" ||
        fail "debugfs stats file was not created"

    clear_queue
    status_output="$(sudo cat "$DEBUGFS_STATUS_PATH")"

    assert_text_contains_line "$status_output" "messages: 0" \
        "debugfs status contains an unexpected message count"

    assert_text_contains_line "$status_output" "capacity: 16" \
        "debugfs status contains an unexpected queue capacity"

    assert_text_contains_line "$status_output" "available: 16" \
        "debugfs status contains an unexpected available count"

    assert_text_contains_line "$status_output" "status_reporting: enabled" \
        "debugfs status reporting state is incorrect"

    assert_text_contains_line "$status_output" "status_interval_ms: 5000" \
        "debugfs status interval is incorrect"

    pass "debugfs status interface"
}

test_nonblocking_read()
{
    local output

    clear_queue
    output="$("$BIN_DIR/nonblock_read")"

    assert_equals "No data available" "$output" \
        "Non-blocking read did not report an empty queue"

    pass "Non-blocking read from empty queue"
}

test_debugfs_messages()
{
    local messages_output
    local messages_output_again
    local actual_message

    clear_queue
    printf 'Debug message' > "$DEVICE_PATH"

    messages_output="$(sudo cat "$DEBUGFS_MESSAGES_PATH")"

    assert_text_contains_line "$messages_output" "messages: 1" \
        "debugfs messages file contains an unexpected message count"

    assert_text_contains_line "$messages_output" \
        '[0] length=13 data="Debug message"' \
        "debugfs messages file does not contain the expected message"

    messages_output_again="$(sudo cat "$DEBUGFS_MESSAGES_PATH")"

    assert_equals "$messages_output" "$messages_output_again" \
        "Reading debugfs messages modified the queue"

    actual_message="$(timeout 2 cat "$DEVICE_PATH")"

    assert_equals "Debug message" "$actual_message" \
        "debugfs messages read modified the queued message"

    pass "debugfs queue inspection"
}

test_debugfs_stats()
{
    local stats_before
    local stats_after
    local stats_message

    local opens_before
    local closes_before
    local messages_read_before
    local messages_written_before
    local bytes_read_before
    local bytes_written_before
    local async_jobs_before

    local opens_after
    local closes_after
    local messages_read_after
    local messages_written_after
    local bytes_read_after
    local bytes_written_after
    local async_jobs_after
    local async_jobs_expected

    clear_queue
    stats_before="$(sudo cat "$DEBUGFS_STATS_PATH")"

    opens_before="$(get_stat opens "$stats_before")"
    closes_before="$(get_stat closes "$stats_before")"
    messages_read_before="$(get_stat messages_read "$stats_before")"
    messages_written_before="$(get_stat messages_written "$stats_before")"
    bytes_read_before="$(get_stat bytes_read "$stats_before")"
    bytes_written_before="$(get_stat bytes_written "$stats_before")"
    async_jobs_before="$(get_stat async_jobs "$stats_before")"

    assert_unsigned_integer "$opens_before" "Invalid opens counter"
    assert_unsigned_integer "$closes_before" "Invalid closes counter"
    assert_unsigned_integer "$messages_read_before" "Invalid messages_read counter"
    assert_unsigned_integer "$messages_written_before" "Invalid messages_written counter"
    assert_unsigned_integer "$bytes_read_before" "Invalid bytes_read counter"
    assert_unsigned_integer "$bytes_written_before" "Invalid bytes_written counter"
    assert_unsigned_integer "$async_jobs_before" "Invalid async_jobs counter"

    printf 'Stats message' > "$DEVICE_PATH"
    stats_message="$(timeout 2 cat "$DEVICE_PATH")"

    assert_equals "Stats message" "$stats_message" \
        "Statistics test did not read the expected message"

    async_jobs_expected=$((async_jobs_before + 1))
    stats_after="$(wait_for_stat async_jobs "$async_jobs_expected")"

    opens_after="$(get_stat opens "$stats_after")"
    closes_after="$(get_stat closes "$stats_after")"
    messages_read_after="$(get_stat messages_read "$stats_after")"
    messages_written_after="$(get_stat messages_written "$stats_after")"
    bytes_read_after="$(get_stat bytes_read "$stats_after")"
    bytes_written_after="$(get_stat bytes_written "$stats_after")"
    async_jobs_after="$(get_stat async_jobs "$stats_after")"

    assert_equals "$((opens_before + 2))" "$opens_after" \
        "Global open counter is incorrect"

    assert_equals "$((closes_before + 2))" "$closes_after" \
        "Global close counter is incorrect"

    assert_equals "$((messages_read_before + 1))" "$messages_read_after" \
        "Global messages-read counter is incorrect"

    assert_equals "$((messages_written_before + 1))" "$messages_written_after" \
        "Global messages-written counter is incorrect"

    assert_equals "$((bytes_read_before + 13))" "$bytes_read_after" \
        "Global bytes-read counter is incorrect"

    assert_equals "$((bytes_written_before + 13))" "$bytes_written_after" \
        "Global bytes-written counter is incorrect"

    assert_equals "$async_jobs_expected" "$async_jobs_after" \
        "Global asynchronous-job counter is incorrect"

    pass "debugfs global statistics"
}

test_runtime_module_parameter()
{
    local actual_interval

    echo 1000 | sudo tee "$STATUS_INTERVAL_PATH" >/dev/null
    actual_interval="$(cat "$STATUS_INTERVAL_PATH")"

    assert_equals "1000" "$actual_interval" \
        "status interval was not changed through sysfs"

    if echo 10 | sudo tee "$STATUS_INTERVAL_PATH" >/dev/null 2>&1; then
        fail "Invalid status interval was accepted"
    fi

    actual_interval="$(cat "$STATUS_INTERVAL_PATH")"

    assert_equals "1000" "$actual_interval" \
        "Invalid write changed the status interval"

    pass "Validated runtime module parameter"
}

test_fifo_order()
{
    local first
    local second
    local third

    clear_queue

    printf 'Message one\n' > "$DEVICE_PATH"
    printf 'Message two\n' > "$DEVICE_PATH"
    printf 'Message three\n' > "$DEVICE_PATH"

    first="$(timeout 2 cat "$DEVICE_PATH")"
    second="$(timeout 2 cat "$DEVICE_PATH")"
    third="$(timeout 2 cat "$DEVICE_PATH")"

    assert_equals "Message one" "$first" "First FIFO message is incorrect"
    assert_equals "Message two" "$second" "Second FIFO message is incorrect"
    assert_equals "Message three" "$third" "Third FIFO message is incorrect"

    pass "FIFO message order"
}

test_poll_notification()
{
    local output_file="$TEMP_DIR/poll-output.txt"

    clear_queue

    timeout 3 "$BIN_DIR/poll_read" > "$output_file" &
    POLL_PID=$!

    sleep 0.2
    printf 'Message through poll\n' > "$DEVICE_PATH"

    wait "$POLL_PID" || fail "poll_read timed out or failed"
    POLL_PID=""

    assert_file_contains_line "$output_file" \
        "Read 21 bytes: Message through poll" \
        "poll_read did not receive the expected message"

    pass "poll() notification"
}

test_ioctl_commands()
{
    local output_file="$TEMP_DIR/ioctl-output.txt"

    clear_queue
    printf 'Message for ioctl\n' > "$DEVICE_PATH"
    "$BIN_DIR/ioctl_test" > "$output_file"

    grep -q "^Buffer length:" "$output_file" ||
        fail "GET_LENGTH did not return a result"

    assert_file_contains_line "$output_file" "Buffer cleared" \
        "CLEAR did not complete"

    assert_file_contains_line "$output_file" "Buffer length: 0 bytes" \
        "Queue is not empty after CLEAR"

    pass "ioctl commands"
}

test_per_open_statistics()
{
    local output_file="$TEMP_DIR/state-output.txt"

    clear_queue
    "$BIN_DIR/file_state_test" > "$output_file"

    assert_file_contains_line "$output_file" "Messages read:    1" \
        "Per-open read statistics are incorrect"

    assert_file_contains_line "$output_file" "Messages written: 2" \
        "Per-open write statistics are incorrect"

    pass "Per-open statistics"
}

test_nonblocking_write()
{
    local index
    local output

    clear_queue

    for index in $(seq 1 16); do
        printf 'Queue message %02d\n' "$index" > "$DEVICE_PATH"
    done

    output="$("$BIN_DIR/nonblock_write")"

    assert_equals "Queue is full" "$output" \
        "Non-blocking write did not report a full queue"

    clear_queue
    pass "Non-blocking write to full queue"
}

test_module_unloading()
{
    unload_module

    [[ ! -e "$DEVICE_PATH" ]] ||
        fail "$DEVICE_PATH still exists after module removal"

    if sudo test -e "$DEBUGFS_DIR"; then
        fail "$DEBUGFS_DIR still exists after module removal"
    fi

    pass "Module unloading and device removal"
}

main()
{
    cd "$ROOT_DIR"

    echo "Building project..."
    make

    [[ -f "$MODULE_PATH" ]] || fail "Kernel module was not built"

    mount_debugfs
    load_module

    test_module_loading
    test_debugfs_status
    test_nonblocking_read
    test_debugfs_messages
    test_debugfs_stats
    test_runtime_module_parameter
    test_fifo_order
    test_poll_notification
    test_ioctl_commands
    test_per_open_statistics
    test_nonblocking_write
    test_module_unloading

    trap - EXIT
    rm -rf "$TEMP_DIR"

    if [[ "$DEBUGFS_MOUNTED_BY_TEST" == true ]]; then
        sudo umount "$DEBUGFS_ROOT"
    fi

    echo
    echo "All integration tests passed."
}

trap cleanup EXIT
main "$@"
