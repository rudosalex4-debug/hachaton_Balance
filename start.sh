#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
docker compose up -d --build --wait
printf '\nDecision Lens: http://localhost:%s\n' "${APP_PORT:-8088}"
