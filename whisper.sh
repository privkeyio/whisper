#!/bin/bash
# whisper - Encrypted DM pipe for Nostr
# Wrapper script to set library paths

DIR="$(cd "$(dirname "$0")" && pwd)"

# Add library paths for dependencies
NOSCRYPT_LIB="$DIR/../noscrypt/build"
LIBNOSTR_LIB="$DIR/../libnostr-c/build"

export LD_LIBRARY_PATH="$NOSCRYPT_LIB:$LIBNOSTR_LIB:$LD_LIBRARY_PATH"

exec "$DIR/whisper" "$@"
