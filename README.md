# whisper

Encrypted DM pipe for Nostr using NIP-17 + NIP-44. Unix-style CLI for private messaging.

```bash
# Send encrypted DM
echo "secret message" | whisper send --to npub1... --relay wss://relay.damus.io

# Receive encrypted DMs
whisper recv --relay wss://relay.damus.io
```

## Install

### Nix (recommended)

```bash
# Run directly
nix run github:privkeyio/whisper -- send --to npub1... --relay wss://...

# Or install
nix profile install github:privkeyio/whisper

# Dev shell
nix develop github:privkeyio/whisper
```

### Manual Build

#### 1. Build Dependencies

```bash
# Build libnostr-c with relay and NIP-17 support
cd ../libnostr-c/build
cmake .. -DNOSTR_FEATURE_NIP17=ON -DNOSTR_FEATURE_RELAY=ON
make
```

### 2. Build whisper

```bash
make
```

### 3. Set Up Your Key (Secure)

```bash
# Create secure key directory
mkdir -p ~/.config/whisper
chmod 700 ~/.config/whisper

# Store your nsec (get from your Nostr client)
echo "nsec1your_private_key_here" > ~/.config/whisper/nsec
chmod 600 ~/.config/whisper/nsec
```

### 4. Send a Message

```bash
# Using key file (most secure - no shell history)
echo "hey!" | ./whisper.sh send \
  --to npub1recipient... \
  --nsec-file ~/.config/whisper/nsec \
  --relay wss://relay.damus.io

# Using environment variable
export NOSTR_NSEC=nsec1...
echo "hey!" | ./whisper.sh send --to npub1... --relay wss://relay.damus.io
```

### 5. Receive Messages

```bash
# Stream incoming DMs
./whisper.sh recv --nsec-file ~/.config/whisper/nsec --relay wss://relay.damus.io

# Get last 10 messages as JSON
./whisper.sh recv --nsec-file ~/.config/whisper/nsec --relay wss://relay.damus.io --limit 10 --json
```

## Usage

```
whisper - Encrypted Nostr DMs (NIP-17)

Usage:
  whisper send --to <npub> --relay <url> [key options]
  whisper recv --relay <url> [key options]

Key options (in order of priority):
  --nsec-file <path>    Read key from file (most secure)
  --nsec <nsec|hex>     Key as argument (visible in history)
  NOSTR_NSEC env var    Fallback if no --nsec option

Send options:
  --to <npub|hex>       Recipient public key
  --relay <url>         Relay URL
  --subject <text>      Optional subject
  --timeout <ms>        Timeout (default: 5000)

Recv options:
  --relay <url>         Relay URL
  --since <timestamp>   Only messages after timestamp
  --limit <n>           Max messages (0 = stream)
  --json                Output JSON format
  --timeout <ms>        Timeout (default: 5000)
```

## Security

**Key Protection:**
- Use `--nsec-file` to avoid keys in shell history
- Set file permissions: `chmod 600 ~/.config/whisper/nsec`
- Keys are wiped from memory using `secure_wipe()` before exit
- Environment variable `NOSTR_NSEC` as fallback (visible to child processes)

**Protocol:**
- NIP-17: Private Direct Messages (triple-wrapped)
- NIP-44: Modern encryption (XChaCha20-Poly1305)
- NIP-59: Gift Wrap (ephemeral sender key)

Messages are encrypted and wrapped so relays cannot read content or see the real sender.

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Invalid arguments |
| 2 | Key parsing error |
| 3 | Relay connection failed |
| 4 | Encryption/decryption failed |
| 5 | Timeout |

## Examples

```bash
# Send secret to a friend
echo "meet at the usual spot, 9pm" | whisper send \
  --to npub1friend... \
  --nsec-file ~/.config/whisper/nsec \
  --relay wss://relay.damus.io

# Pipe from another command
cat secret.txt | whisper send --to npub1... --relay wss://relay.damus.io

# Monitor inbox continuously
whisper recv --relay wss://relay.damus.io | tee inbox.log

# Export messages as JSON for processing
whisper recv --relay wss://relay.damus.io --limit 100 --json > messages.json
```

## Building from Source

### Prerequisites

- libnostr-c (with NIP-17 and relay features)
- libwebsockets
- libcjson
- libsecp256k1
- noscrypt
- OpenSSL

### Compile

```bash
# Standard build
make

# With custom libnostr-c path
LIBNOSTR_DIR=/path/to/libnostr-c make
```

## License

AGPL-3.0
