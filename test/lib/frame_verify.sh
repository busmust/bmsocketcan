#!/bin/bash
################################################################################
# Library: Frame Verification Helpers
# Provides payload content, DLC, flag, and frame count verification
################################################################################

# CAN FD DLC-to-length mapping (ISO 11898-1)
CANFD_DLC_TO_LEN=(0 1 2 3 4 5 6 7 8 12 16 20 24 32 48 64)

# Convert byte length to expected DLC value
len_to_dlc() {
    local len=$1
    if (( len <= 8 )); then echo $len; return; fi
    if (( len <= 12 )); then echo 9; return; fi
    if (( len <= 16 )); then echo 10; return; fi
    if (( len <= 20 )); then echo 11; return; fi
    if (( len <= 24 )); then echo 12; return; fi
    if (( len <= 32 )); then echo 13; return; fi
    if (( len <= 48 )); then echo 14; return; fi
    if (( len <= 64 )); then echo 15; return; fi
    echo 0
}

# Convert DLC to expected byte length
dlc_to_len() {
    local dlc=$1
    echo "${CANFD_DLC_TO_LEN[$dlc]}"
}

# Parse a candump output line into structured fields.
# Input: candump line like "(1234567890.123456) can0 123#01020304"
# Output vars set by caller reference or printed for capture.
# Returns: id_hex data_hex is_fd is_ext is_rtr brs dlc_byte_count
#
# candump formats:
#   CAN 2.0:  "can0 123#DEADBEEF" or "can0 123#R8"
#   CAN 2.0 ext: "can0 12345678#DEADBEEF"
#   CAN FD:   "can0 123##1DEADBEEF" (## separates, flags byte after ##)
#   CAN FD no BRS: "can0 123##DEADBEEF" (empty flags = no BRS)
parse_candump_line() {
    local line="$1"

    # Extract the frame part (after interface name)
    local frame
    frame=$(echo "$line" | sed -n 's/.*\(can[0-9]\+\)\s\+\(.*\)/\2/p')
    if [ -z "$frame" ]; then
        return 1
    fi

    PARSE_ID=""
    PARSE_DATA=""
    PARSE_IS_FD=false
    PARSE_IS_EXT=false
    PARSE_IS_RTR=false
    PARSE_BRS=false
    PARSE_BYTE_LEN=0

    # Check CAN FD (## separator)
    if [[ "$frame" == *"##"* ]]; then
        PARSE_IS_FD=true
        local id_part="${frame%%##*}"
        local rest="${frame#*##}"
        PARSE_ID="$id_part"

        # FD flags: first char(s) before data
        # candump format: ##F where F is hex flags byte (bit0=BRS, bit1=ESI)
        # If rest starts with hex digit that could be a flag byte
        local flags_str=""
        local data_str=""

        if [[ "$rest" =~ ^([0-9A-Fa-f]{1,2})([0-9A-Fa-f]*)$ ]]; then
            local full="$rest"
            # Ambiguous: need to figure out what's flags and what's data
            # In candump, ## is followed by optional single hex flags byte,
            # then payload. But since flags is 0-3 and data can start with
            # same chars, we rely on known patterns.
            # Actually candump always prints flags as exactly one hex digit.
            # flags=0 means no BRS no ESI, flags=1 means BRS, etc.
            # If there are no data bytes, flags might be omitted or be '0'
            if [ ${#full} -le 1 ]; then
                flags_str="${full}"
                data_str=""
            else
                flags_str="${full:0:1}"
                data_str="${full:1}"
            fi
        elif [ -z "$rest" ]; then
            data_str=""
        else
            data_str="$rest"
        fi

        local flags_val=0
        if [ -n "$flags_str" ]; then
            flags_val=$((16#$flags_str))
        fi
        if (( flags_val & 0x01 )); then PARSE_BRS=true; fi

        PARSE_DATA=$(echo "$data_str" | tr '[:upper:]' '[:lower:]')
        PARSE_BYTE_LEN=$(( ${#PARSE_DATA} / 2 ))

    elif [[ "$frame" == *"#R"* ]]; then
        # RTR frame: 123#R0, 123#R8, etc.
        local id_part="${frame%%#*}"
        PARSE_ID="$id_part"
        PARSE_IS_RTR=true
        PARSE_DATA=""
        PARSE_BYTE_LEN=0
    else
        # CAN 2.0 data frame
        local id_part="${frame%%#*}"
        local data_part="${frame#*#}"
        PARSE_ID="$id_part"
        PARSE_DATA=$(echo "$data_part" | tr '[:upper:]' '[:lower:]')
        PARSE_BYTE_LEN=$(( ${#PARSE_DATA} / 2 ))
    fi

    # Determine if extended ID (candump prints extended IDs with full hex, >= 4 hex digits)
    # Standard IDs are 1-3 hex digits (up to 0x7FF)
    # Extended IDs are printed as full hex value (can be up to 8 hex digits)
    # Heuristic: if ID > 0x7FF, it's extended
    local id_val=$((16#$PARSE_ID))
    if (( id_val > 0x7FF )); then
        PARSE_IS_EXT=true
    fi

    return 0
}

# Generate sequential payload pattern of given length.
# Pattern: 01 02 03 ... (wraps at FF)
generate_payload() {
    local len=$1
    local result=""
    for (( i=1; i<=len; i++ )); do
        result+=$(printf "%02X" $(( i & 0xFF )))
    done
    echo "$result"
}

# Verify a CAN 2.0 frame: send and verify payload content byte-by-byte.
# Usage: verify_can20_frame tx_port rx_port frame_id sent_data_hex
# Returns 0 on success, 1 on failure.
# Sets VERIFY_LOG with detailed result.
verify_can20_frame() {
    local tx=$1
    local rx=$2
    local frame_id=$3
    local sent_data="$4"  # hex string like "DEADBEEF" or "" for 0 bytes

    local id_hex=$(echo "$frame_id" | sed 's/^0x//; s/^0*//' | tr '[:upper:]' '[:lower:]')
    # Pad to at least 3 chars for grep matching
    [ ${#id_hex} -lt 3 ] && id_hex=$(printf "%03s" "$id_hex" | tr ' ' '0')

    local sent_data_lower=$(echo "$sent_data" | tr '[:upper:]' '[:lower:]')
    local sent_len=$(( ${#sent_data} / 2 ))

    local logf="/tmp/verify_can20_$$.log"
    local max_retries=3
    local attempt

    for attempt in $(seq 1 $max_retries); do
        rm -f "$logf"
        timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
        local pid=$!
        sleep 0.1

        sudo cansend $tx "${frame_id}#${sent_data}" >/dev/null 2>&1
        sleep 0.3

        wait $pid 2>/dev/null || true

        if [ ! -s "$logf" ]; then
            continue
        fi

        # Parse received frame
        local recv_line
        recv_line=$(grep -i "$id_hex" "$logf" | head -1)
        if [ -z "$recv_line" ]; then
            continue
        fi

        parse_candump_line "$recv_line"

        # Verify ID match
        local recv_id_lower=$(echo "$PARSE_ID" | tr '[:upper:]' '[:lower:]')
        if [ "$recv_id_lower" != "$id_hex" ]; then
            VERIFY_LOG="ID mismatch: sent=$id_hex recv=$recv_id_lower"
            rm -f "$logf"
            return 1
        fi

        # Verify payload content
        if [ -n "$sent_data_lower" ]; then
            if [ "$PARSE_DATA" != "$sent_data_lower" ]; then
                VERIFY_LOG="Payload mismatch: sent=$sent_data_lower recv=$PARSE_DATA"
                rm -f "$logf"
                return 1
            fi
        fi

        # Verify payload length
        if [ "$PARSE_BYTE_LEN" -ne "$sent_len" ]; then
            VERIFY_LOG="Length mismatch: sent=$sent_len recv=$PARSE_BYTE_LEN"
            rm -f "$logf"
            return 1
        fi

        # Verify not received as FD frame
        if $PARSE_IS_FD; then
            VERIFY_LOG="CAN 2.0 frame received as CAN FD"
            rm -f "$logf"
            return 1
        fi

        rm -f "$logf"
        VERIFY_LOG="OK: id=$id_hex len=$PARSE_BYTE_LEN data=$PARSE_DATA"
        return 0
    done

    rm -f "$logf"
    VERIFY_LOG="No frame received after $max_retries attempts"
    return 1
}

# Verify a CAN FD frame: send and verify payload, DLC mapping, and flags.
# Usage: verify_canfd_frame tx_port rx_port frame_id flags sent_data_hex
#   flags: candump flags byte (0=no BRS, 1=BRS, 5=BRS+ESI)
# Returns 0 on success, 1 on failure.
# Sets VERIFY_LOG with detailed result.
verify_canfd_frame() {
    local tx=$1
    local rx=$2
    local frame_id=$3
    local flags=$4   # 0 or 1 (BRS)
    local sent_data="$5"  # hex string

    local id_hex=$(echo "$frame_id" | sed 's/^0x//; s/^0*//' | tr '[:upper:]' '[:lower:]')
    [ ${#id_hex} -lt 3 ] && id_hex=$(printf "%03s" "$id_hex" | tr ' ' '0')

    local sent_data_lower=$(echo "$sent_data" | tr '[:upper:]' '[:lower:]')
    local sent_len=$(( ${#sent_data} / 2 ))

    # Expected DLC byte count (CAN FD rounds up to next DLC boundary)
    local expected_dlc
    expected_dlc=$(len_to_dlc $sent_len)
    local expected_byte_len
    expected_byte_len=$(dlc_to_len $expected_dlc)

    local logf="/tmp/verify_canfd_$$.log"
    local max_retries=3
    local attempt

    for attempt in $(seq 1 $max_retries); do
        rm -f "$logf"
        timeout 2 candump $rx -n 1 -t A 2>/dev/null > "$logf" &
        local pid=$!
        sleep 0.1

        sudo cansend $tx "${frame_id}##${flags}${sent_data}" >/dev/null 2>&1
        sleep 0.3

        wait $pid 2>/dev/null || true

        if [ ! -s "$logf" ]; then
            continue
        fi

        # For CAN FD, candump uses ## so grep pattern needs to handle that
        local recv_line
        recv_line=$(grep -i "$id_hex" "$logf" | head -1)
        if [ -z "$recv_line" ]; then
            continue
        fi

        parse_candump_line "$recv_line"

        # Verify ID match
        local recv_id_lower=$(echo "$PARSE_ID" | tr '[:upper:]' '[:lower:]')
        if [ "$recv_id_lower" != "$id_hex" ]; then
            VERIFY_LOG="ID mismatch: sent=$id_hex recv=$recv_id_lower"
            rm -f "$logf"
            return 1
        fi

        # Verify received as CAN FD
        if ! $PARSE_IS_FD; then
            VERIFY_LOG="CAN FD frame received as CAN 2.0"
            rm -f "$logf"
            return 1
        fi

        # Verify received byte length matches expected DLC boundary
        if [ "$PARSE_BYTE_LEN" -ne "$expected_byte_len" ]; then
            VERIFY_LOG="DLC length mismatch: sent=${sent_len}B expected_dlc=$expected_dlc expected_len=${expected_byte_len}B recv=${PARSE_BYTE_LEN}B"
            rm -f "$logf"
            return 1
        fi

        # Verify payload content (compare only sent bytes, padding may be zeros)
        if [ -n "$sent_data_lower" ] && [ "$PARSE_BYTE_LEN" -ge "$sent_len" ]; then
            local recv_sent_part="${PARSE_DATA:0:${#sent_data_lower}}"
            if [ "$recv_sent_part" != "$sent_data_lower" ]; then
                VERIFY_LOG="Payload mismatch: sent=$sent_data_lower recv=$recv_sent_part (full=$PARSE_DATA)"
                rm -f "$logf"
                return 1
            fi
        elif [ -n "$sent_data_lower" ]; then
            VERIFY_LOG="Payload too short: sent=$sent_len recv=$PARSE_BYTE_LEN"
            rm -f "$logf"
            return 1
        fi

        # Verify BRS flag if sent with BRS
        if [ "$flags" = "1" ] && ! $PARSE_BRS; then
            VERIFY_LOG="BRS flag missing: sent BRS but received without BRS"
            rm -f "$logf"
            return 1
        fi

        rm -f "$logf"
        VERIFY_LOG="OK: id=$id_hex dlc=$expected_dlc len=$PARSE_BYTE_LEN data=$PARSE_DATA fd=true brs=$PARSE_BRS"
        return 0
    done

    rm -f "$logf"
    VERIFY_LOG="No frame received after $max_retries attempts"
    return 1
}

# Verify exact frame count: send N frames, verify exactly N received.
# Usage: verify_exact_count tx_port rx_port frame_id frame_data count timeout_sec
verify_exact_count() {
    local tx=$1
    local rx=$2
    local frame_id=$3
    local frame_data=$4
    local count=$5
    local timeout=${6:-5}

    local id_hex=$(echo "$frame_id" | sed 's/^0x//; s/^0*//' | tr '[:upper:]' '[:lower:]')
    local logf="/tmp/verify_count_$$.log"

    rm -f "$logf"
    timeout $timeout candump $rx -t A 2>/dev/null > "$logf" &
    local pid=$!
    sleep 0.1

    local i
    for (( i=0; i<count; i++ )); do
        sudo cansend $tx "${frame_id}#${frame_data}" >/dev/null 2>&1
    done

    # Wait for candump to collect
    sleep 1
    wait $pid 2>/dev/null || true

    local received
    received=$(grep -i -c "$id_hex" "$logf" 2>/dev/null) || received=0
    [[ "$received" =~ ^[0-9]+$ ]] || received=0

    rm -f "$logf"

    if [ "$received" -eq "$count" ]; then
        VERIFY_LOG="OK: sent=$count received=$received"
        return 0
    else
        VERIFY_LOG="Count mismatch: sent=$count received=$received"
        return 1
    fi
}

# Verify bitrate configuration by reading back from ip link.
# Usage: verify_bitrate port expected_nominal expected_data
verify_bitrate() {
    local port=$1
    local expected_nominal=$2
    local expected_data=$3

    local output
    output=$(ip -details link show $port 2>/dev/null)

    local bitrate
    bitrate=$(echo "$output" | grep -oP 'bitrate \K[0-9]+')
    local dbitrate
    dbitrate=$(echo "$output" | grep -oP 'dbitrate \K[0-9]+')

    if [ "$bitrate" != "$expected_nominal" ]; then
        VERIFY_LOG="Nominal bitrate mismatch: expected=$expected_nominal actual=$bitrate"
        return 1
    fi

    if [ -n "$expected_data" ] && [ "$dbitrate" != "$expected_data" ]; then
        VERIFY_LOG="Data bitrate mismatch: expected=$expected_data actual=$dbitrate"
        return 1
    fi

    VERIFY_LOG="OK: bitrate=$bitrate dbitrate=$dbitrate"
    return 0
}
