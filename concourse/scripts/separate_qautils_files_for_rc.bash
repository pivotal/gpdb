#!/bin/bash

set -euo pipefail

main() {
  ABS_PATH_OUTPUT_TARBALL="$(pwd)/$OUTPUT_TARBALL"
  ABS_QAUTILS_FILES="$(pwd)/$QAUTILS_FILES"
  ABS_PATH_QAUTILS_TARBALL="$(pwd)/$QAUTILS_TARBALL"
  QAUTILS_DIR="$(mktemp -d)"

  INTERMEDIATE_PLACE="$(mktemp -d)"

  # use eval/ls to expand any globs in the input param
  tar zxf "$(eval ls "${INPUT_TARBALL}")" -C "$INTERMEDIATE_PLACE"

  pushd "$INTERMEDIATE_PLACE"
    echo "Move files listed in $ABS_QAUTILS_FILES"
    while read -r file; do
      if [ -f "$file" ]; then
        TARGET_QAUTILS_DIR="$QAUTILS_DIR/$(dirname "$file")"
        echo "Moving $file to directory $TARGET_QAUTILS_DIR"
        mkdir -p "$TARGET_QAUTILS_DIR"
        mv "$file" "$TARGET_QAUTILS_DIR"
      else
        echo "File $file does not exist, skipping moving it"
      fi
    done < "$ABS_QAUTILS_FILES"
    tar czf "$ABS_PATH_OUTPUT_TARBALL" -- *
  popd

  pushd "$QAUTILS_DIR"
    tar czf "$ABS_PATH_QAUTILS_TARBALL" -- *
  popd
}

main "$@"
