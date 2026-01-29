/*
 * whisper - Encrypted DM pipe for Nostr (NIP-17 + NIP-44)
 *
 * Usage:
 *   echo "secret" | whisper send --to npub1... --nsec nsec1... --relay wss://...
 *   whisper recv --nsec nsec1... --relay wss://...
 */

#ifndef WHISPER_H
#define WHISPER_H

#include <nostr.h>
#include <stdint.h>
#include <stdbool.h>

/* Exit codes */
#define WHISPER_EXIT_OK              0
#define WHISPER_EXIT_INVALID_ARGS    1
#define WHISPER_EXIT_KEY_ERROR       2
#define WHISPER_EXIT_RELAY_ERROR     3
#define WHISPER_EXIT_CRYPTO_ERROR    4
#define WHISPER_EXIT_TIMEOUT         5

/* Default timeout in milliseconds */
#define WHISPER_DEFAULT_TIMEOUT_MS   5000

/* Configuration for send command */
typedef struct {
    const char* recipient;       /* npub or hex pubkey */
    const char* nsec;            /* nsec or hex private key */
    const char* nsec_file;       /* path to file containing nsec */
    const char* relay_url;       /* relay URL */
    const char* subject;         /* optional subject */
    const char* reply_to;        /* optional event ID to reply to */
    int timeout_ms;              /* relay timeout */
} whisper_send_config;

/* Configuration for recv command */
typedef struct {
    const char* nsec;            /* nsec or hex private key */
    const char* nsec_file;       /* path to file containing nsec */
    const char* relay_url;       /* relay URL */
    int64_t since;               /* only messages after this timestamp */
    int limit;                   /* max messages (0 = unlimited) */
    bool json_output;            /* output raw JSON */
    int timeout_ms;              /* connection timeout */
} whisper_recv_config;

/* Send a DM, reading content from stdin */
int whisper_send(const whisper_send_config* config);

/* Receive DMs, writing to stdout */
int whisper_recv(const whisper_recv_config* config);

/* Utility: load private key from string or file */
int whisper_load_privkey(const char* nsec_str, const char* nsec_file,
                         nostr_privkey* privkey, nostr_key* pubkey);

/* Utility: parse pubkey from npub or hex */
int whisper_parse_pubkey(const char* pubkey_str, nostr_key* pubkey);

/* Utility: strip control characters from string (returns malloc'd copy) */
char* whisper_strip_control_chars(const char* input);

#endif /* WHISPER_H */
