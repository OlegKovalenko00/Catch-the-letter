#!/bin/bash


set -e

echo "=== Catch-the-letter Linux Tests ==="
echo ""

BASE_URL="http://127.0.0.1:8080"
BROWSER_WORKER_URL="http://127.0.0.1:8090"
ERRORS=0


test_endpoint() {
    local name=$1
    local url=$2
    local method=${3:-GET}

    echo "Testing: $name"

    if [ "$method" = "GET" ]; then
        if curl -s -m 30 "$url" > /dev/null 2>&1; then
            echo "  [OK]"
        else
            echo "  [ERROR] Failed"
            ERRORS=$((ERRORS + 1))
        fi
    elif [ "$method" = "POST" ]; then
        if curl -s -X POST -m 30 "$url" > /dev/null 2>&1; then
            echo "  [OK]"
        else
            echo "  [ERROR] Failed"
            ERRORS=$((ERRORS + 1))
        fi
    fi
}

echo "API Endpoints:"
test_endpoint "GET /api/status" "$BASE_URL/api/status" "GET"
test_endpoint "GET /health (browser-worker)" "$BROWSER_WORKER_URL/health" "GET"
test_endpoint "POST /api/test/browser" "$BASE_URL/api/test/browser" "POST"
test_endpoint "POST /api/test/telegram" "$BASE_URL/api/test/telegram" "POST"
test_endpoint "POST /api/test/imap" "$BASE_URL/api/test/imap" "POST"
test_endpoint "POST /api/test/llm" "$BASE_URL/api/test/llm" "POST"

echo ""
echo "Demo flows:"
test_endpoint "POST /api/demo/create" "$BASE_URL/api/demo/create" "POST"
test_endpoint "POST /api/demo/create-auth" "$BASE_URL/api/demo/create-auth" "POST"

echo ""
if [ $ERRORS -eq 0 ]; then
    echo "=== All tests passed ==="
else
    echo "=== $ERRORS test(s) failed ==="
fi

echo ""
echo "Web UI available at: $BASE_URL"
