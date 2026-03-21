#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
REMOTE_DIR="${REMOTE_DIR:-/opt/aegis}"
COMPOSE_FILE="docker-compose.api.yml"
DIST_DIR="${REPO_ROOT}/dist/advin"
ENV_FILE="${ENV_FILE:-${SCRIPT_DIR}/.env}"
SSH_OPTS="${SSH_OPTS:-}"

usage() {
  cat <<EOF
Usage: $(basename "$0") <ssh-target>

Builds linux/amd64 Aegis binaries, uploads them and the Compose bundle to the target,
and starts the stack in ${REMOTE_DIR}.

Optional environment variables:
  REMOTE_DIR   Remote deployment directory. Default: ${REMOTE_DIR}
  ENV_FILE     Local env file to upload as .env. Default: ${ENV_FILE}
  SSH_OPTS     Extra options passed to ssh/scp.
EOF
}

if [[ $# -ne 1 ]]; then
  usage
  exit 1
fi

REMOTE_HOST="$1"

mkdir -p "${DIST_DIR}"

echo "Building aegis-api..."
(
  cd "${REPO_ROOT}"
  CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -o "${DIST_DIR}/aegis-api" ./cmd/api
)

echo "Building aegis-jobs..."
(
  cd "${REPO_ROOT}"
  CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -o "${DIST_DIR}/aegis-jobs" ./cmd/jobs
)

echo "Preparing ${REMOTE_HOST}:${REMOTE_DIR}..."
ssh ${SSH_OPTS} "${REMOTE_HOST}" "mkdir -p '${REMOTE_DIR}/bin'"

echo "Uploading binaries and Compose file..."
scp ${SSH_OPTS} \
  "${DIST_DIR}/aegis-api" \
  "${DIST_DIR}/aegis-jobs" \
  "${SCRIPT_DIR}/${COMPOSE_FILE}" \
  "${REMOTE_HOST}:${REMOTE_DIR}/"

ssh ${SSH_OPTS} "${REMOTE_HOST}" \
  "chmod 755 '${REMOTE_DIR}/aegis-api' '${REMOTE_DIR}/aegis-jobs' && mv '${REMOTE_DIR}/aegis-api' '${REMOTE_DIR}/bin/aegis-api' && mv '${REMOTE_DIR}/aegis-jobs' '${REMOTE_DIR}/bin/aegis-jobs'"

if [[ -f "${ENV_FILE}" ]]; then
  echo "Uploading env file from ${ENV_FILE}..."
  scp ${SSH_OPTS} "${ENV_FILE}" "${REMOTE_HOST}:${REMOTE_DIR}/.env"
else
  echo "No local env file at ${ENV_FILE}; leaving remote .env unchanged."
fi

echo "Starting stack..."
ssh ${SSH_OPTS} "${REMOTE_HOST}" \
  "cd '${REMOTE_DIR}' && docker compose -f '${COMPOSE_FILE}' up -d --remove-orphans"

echo "Deployment finished."
