#!/bin/bash
set -euo pipefail

# Cross builds pass their own objcopy (e.g. the NDK llvm-objcopy) through the environment.
objcopy_tool=${OBJCOPY:-objcopy}

usage() {
	echo "$0 /path/to/input/file [-o /path/to/output/file]" >&2
}

if (( $# == 0 )); then
	usage
	exit 2
fi

input_dir=$(cd -- "$(dirname -- "$1")" && pwd)
input_file="$input_dir/$(basename -- "$1")"
output_file="$input_file.dbg"
shift

while getopts ":o:" opt; do
	case $opt in
		o)
			output_dir=$(cd -- "$(dirname -- "$OPTARG")" && pwd)
			output_file="$output_dir/$(basename -- "$OPTARG")"
			;;
		*) usage; exit 2 ;;
	esac
done
shift "$((OPTIND - 1))"
[[ $# == 0 && $input_file != "$output_file" ]] || { usage; exit 2; }
"$objcopy_tool" "$input_file" "$output_file"
"$objcopy_tool" --add-gnu-debuglink="$output_file" "$input_file"
