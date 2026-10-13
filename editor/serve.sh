#!/usr/bin/env sh
# Local test server:  ./serve.sh [port]   then open http://localhost:8080
cd "$(dirname "$0")" && echo "http://localhost:${1:-8080}" && python3 -m http.server "${1:-8080}"
