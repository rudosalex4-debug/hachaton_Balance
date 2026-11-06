#!/bin/sh
set -eu
CDPATH= cd -P "$(dirname "$0")"
docker compose up -d --build "$@"
docker compose ps
