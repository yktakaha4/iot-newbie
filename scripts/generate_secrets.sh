#!/bin/sh
set -eu

sketch_dir="${1:-}"

if [ -z "$sketch_dir" ]; then
  echo "Usage: $0 SKETCH_DIR [ENV_FILE]" >&2
  echo "Example: $0 apps/TRHCheckerM5Paper" >&2
  exit 1
fi

if [ ! -d "$sketch_dir" ]; then
  echo "Sketch directory not found: $sketch_dir" >&2
  exit 1
fi

env_file="${2:-$sketch_dir/.env}"
example_file="$sketch_dir/.env.example"
output_file="$sketch_dir/secrets.h"

if [ ! -f "$env_file" ]; then
  echo "Missing $env_file." >&2
  if [ -f "$example_file" ]; then
    echo "Copy $example_file to $env_file and fill in local values." >&2
  fi
  exit 1
fi

get_env_value() {
  key="$1"
  value="$(grep -E "^${key}=" "$env_file" | tail -n 1 | sed "s/^${key}=//")"
  value="${value%\"}"
  value="${value#\"}"
  value="${value%\'}"
  value="${value#\'}"
  printf '%s' "$value"
}

escape_cpp_string() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

wifi_ssid="$(get_env_value WIFI_SSID)"
wifi_password="$(get_env_value WIFI_PASSWORD)"

if [ -z "$wifi_ssid" ]; then
  echo "WIFI_SSID is empty in $env_file." >&2
  exit 1
fi

mkdir -p "$(dirname "$output_file")"
cat > "$output_file" <<EOF
#pragma once

#define WIFI_SSID "$(escape_cpp_string "$wifi_ssid")"
#define WIFI_PASSWORD "$(escape_cpp_string "$wifi_password")"
EOF

echo "Generated $output_file"
