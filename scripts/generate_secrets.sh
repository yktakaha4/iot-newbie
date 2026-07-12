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

escape_cpp_string() {
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

mkdir -p "$(dirname "$output_file")"
cat > "$output_file" <<EOF
#pragma once

EOF

defined_count=0
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    ''|\#*) continue ;;
  esac

  key="${line%%=*}"
  value="${line#*=}"

  if [ "$key" = "$line" ]; then
    echo "Invalid line in $env_file: $line" >&2
    exit 1
  fi

  case "$key" in
    [A-Za-z_][A-Za-z0-9_]*) ;;
    *)
      echo "Invalid key in $env_file: $key" >&2
      exit 1
      ;;
  esac

  value="${value%\"}"
  value="${value#\"}"
  value="${value%\'}"
  value="${value#\'}"

  printf '#define %s "%s"\n' "$key" "$(escape_cpp_string "$value")" >> "$output_file"
  defined_count=$((defined_count + 1))
done < "$env_file"

if [ "$defined_count" -eq 0 ]; then
  echo "No KEY=VALUE entries found in $env_file." >&2
  exit 1
fi

echo "Generated $output_file"
