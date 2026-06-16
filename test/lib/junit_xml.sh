#!/bin/bash
# JUnit XML Report Generator for BMCAN Tests
# Usage: source lib/junit_xml.sh

# Global variables for test results
# Only initialize if not already defined (for multiple sourcing)
# Note: Arrays must be checked with ${#@} to see if already set
if [ ${#TEST_CASES[@]} -eq 0 ] 2>/dev/null || [ -z "${TEST_CASES+x}" ]; then
    TESTSUITE_NAME="BMCAN Tests"
    TEST_CASES=()
    TEST_RESULTS=()
    TEST_MESSAGES=()
    TEST_TIMES=()
    TEST_START_TIME=""
    BMCAN_TOTAL_FAILURES=0
fi

# Initialize test suite
init_testsuite() {
    TESTSUITE_NAME="$1"
    TEST_CASES=()
    TEST_RESULTS=()
    TEST_MESSAGES=()
    TEST_TIMES=()
    TEST_START_TIME=$(date +%s)
}

# Append to existing test suite (for modules - doesn't reset arrays)
append_testsuite() {
    # Keep existing test data, just ensure start time is set
    if [ -z "$TEST_START_TIME" ]; then
        TEST_START_TIME=$(date +%s)
    fi
}

# Start a test case
start_testcase() {
    local name="$1"
    TEST_CASES+=("$name")
    TEST_RESULTS+=(0)
    TEST_MESSAGES+=("")
    TEST_TIMES+=(0)
}

# End test case with result
end_testcase() {
    local result="$1"
    local message="$2"
    local idx=$((${#TEST_CASES[@]} - 1))
    TEST_RESULTS[$idx]=$result
    TEST_MESSAGES[$idx]="$message"

    if [ "$result" != "0" ] && [ "$result" != "pass" ]; then
        BMCAN_TOTAL_FAILURES=$((BMCAN_TOTAL_FAILURES + 1))
    fi

    local end_time=$(date +%s)
    TEST_TIMES[$idx]=$(($end_time - $TEST_START_TIME))
}

# Record test failure
test_fail() {
    local message="$1"
    local idx=$((${#TEST_CASES[@]} - 1))
    TEST_RESULTS[$idx]=1
    TEST_MESSAGES[$idx]="$message"
    BMCAN_TOTAL_FAILURES=$((BMCAN_TOTAL_FAILURES + 1))
}

# Record test pass
test_pass() {
    local message="$1"
    local idx=$((${#TEST_CASES[@]} - 1))
    TEST_RESULTS[$idx]=0
    TEST_MESSAGES[$idx]="$message"
}

# Generate JUnit XML report
generate_junit_xml() {
    local output_file="$1"
    local timestamp=$(date -Iseconds 2>/dev/null || date)
    local time=0
    if [ -n "$TEST_START_TIME" ]; then
        time=$(($(date +%s) - TEST_START_TIME))
    fi

    # Calculate statistics
    local total=${#TEST_CASES[@]}
    local failures=0
    local errors=0
    for r in "${TEST_RESULTS[@]}"; do
        if [ $r -ne 0 ]; then
            ((failures++))
        fi
    done

    # Write XML header
    cat > "$output_file" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<testsuites name="$TESTSUITE_NAME" tests="$total" failures="$failures" errors="$errors" time="$time" timestamp="$timestamp">
<testsuite name="$TESTSUITE_NAME" tests="$total" failures="$failures" errors="$errors" time="$time" timestamp="$timestamp">
EOF

    # Write test cases
    local i=0
    while [ $i -lt ${#TEST_CASES[@]} ]; do
        local name="${TEST_CASES[$i]}"
        local result="${TEST_RESULTS[$i]}"
        local message="${TEST_MESSAGES[$i]}"
        local testcase_time="${TEST_TIMES[$i]}"

        if [ $result -eq 0 ]; then
            # Pass - write simple testcase tag
            echo "  <testcase name=\"$name\" time=\"$testcase_time\"/>" >> "$output_file"
        else
            # Failure - escape XML special characters simply
            msg_escaped=$(echo "$message" | sed 's/&/\\&amp;/g; s/</\\&lt;/g; s/>/\\&gt;/g')
            echo "  <testcase name=\"$name\" time=\"$testcase_time\">" >> "$output_file"
            echo "    <failure type=\"AssertionError\" message=\"$msg_escaped\"/>" >> "$output_file"
            echo "  </testcase>" >> "$output_file"
        fi
        ((i++))
    done

    # Close tags
    echo "</testsuite>" >> "$output_file"
    echo "</testsuites>" >> "$output_file"

    echo "JUnit XML report generated: $output_file"
}

# Print summary to console
print_test_summary() {
    local total=${#TEST_CASES[@]}
    local passed=0
    local failed=0

    for r in "${TEST_RESULTS[@]}"; do
        # Ensure r is numeric
        [[ "$r" =~ ^[0-9]+$ ]] || r=1
        if [ $r -eq 0 ]; then
            ((passed++))
        else
            ((failed++))
        fi
    done

    echo ""
    echo "========================================"
    echo "Test Summary"
    echo "========================================"
    echo "Total:   $total"
    echo "Passed:  $passed"
    echo "Failed:  $failed"
    echo "========================================"

    if [ $failed -gt 0 ]; then
        echo ""
        echo "Failed Tests:"
        local i=0
        while [ $i -lt ${#TEST_CASES[@]} ]; do
            local result=${TEST_RESULTS[$i]:-0}
            # Validate result is numeric
            [[ "$result" =~ ^[0-9]+$ ]] || result=0
            if [ "$result" -ne 0 ]; then
                local name=${TEST_CASES[$i]:-"<unnamed>"}
                local msg=${TEST_MESSAGES[$i]:-"No message"}
                echo "  X $name"
                echo "    $msg"
            fi
            ((i++))
        done
        echo "========================================"
    fi

    return $failed
}
