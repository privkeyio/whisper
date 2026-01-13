/*
 * whisper utilities - Key loading and parsing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "whisper.h"

/* Load private key from file, trimming whitespace */
static char* read_key_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return NULL;

    char* buf = malloc(256);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    if (!fgets(buf, 256, f)) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);

    /* Trim trailing whitespace */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ')) {
        buf[--len] = '\0';
    }

    return buf;
}

int whisper_load_privkey(const char* nsec_str, const char* nsec_file,
                         nostr_privkey* privkey, nostr_key* pubkey) {
    const char* key_str = nsec_str;
    char* file_buf = NULL;
    char* env_buf = NULL;

    /* Priority: file > argument > environment */
    if (nsec_file) {
        file_buf = read_key_file(nsec_file);
        if (!file_buf) {
            fprintf(stderr, "Error: Could not read key file: %s\n", nsec_file);
            return -1;
        }
        key_str = file_buf;
    } else if (!nsec_str) {
        /* Try environment variable as fallback */
        const char* env_key = getenv("NOSTR_NSEC");
        if (env_key) {
            env_buf = strdup(env_key);
            key_str = env_buf;
        }
    }

    if (!key_str) {
        fprintf(stderr, "Error: No private key provided\n");
        fprintf(stderr, "  Use --nsec, --nsec-file, or set NOSTR_NSEC environment variable\n");
        return -1;
    }

    nostr_error_t err;

    /* Try bech32 (nsec) first */
    if (strncmp(key_str, "nsec1", 5) == 0) {
        err = nostr_privkey_from_bech32(key_str, privkey);
    } else if (strlen(key_str) == 64) {
        /* Try hex */
        err = nostr_privkey_from_hex(key_str, privkey);
    } else {
        fprintf(stderr, "Error: Invalid private key format (expected nsec or 64-char hex)\n");
        if (file_buf) { secure_wipe(file_buf, strlen(file_buf)); free(file_buf); }
        if (env_buf) { secure_wipe(env_buf, strlen(env_buf)); free(env_buf); }
        return -1;
    }

    /* Wipe temporary buffers */
    if (file_buf) {
        secure_wipe(file_buf, strlen(file_buf));
        free(file_buf);
    }
    if (env_buf) {
        secure_wipe(env_buf, strlen(env_buf));
        free(env_buf);
    }

    if (err != NOSTR_OK) {
        fprintf(stderr, "Error: Failed to parse private key: %s\n", nostr_error_string(err));
        return -1;
    }

    /* Derive public key using keypair API */
    nostr_keypair keypair;
    err = nostr_keypair_from_private_key(&keypair, privkey);
    if (err != NOSTR_OK) {
        fprintf(stderr, "Error: Failed to derive public key: %s\n", nostr_error_string(err));
        secure_wipe(privkey, sizeof(*privkey));
        return -1;
    }

    /* Copy pubkey out */
    memcpy(pubkey, &keypair.pubkey, sizeof(*pubkey));
    nostr_keypair_destroy(&keypair);

    return 0;
}

int whisper_parse_pubkey(const char* pubkey_str, nostr_key* pubkey) {
    if (!pubkey_str) return -1;

    nostr_error_t err;

    /* Try bech32 (npub) first */
    if (strncmp(pubkey_str, "npub1", 5) == 0) {
        err = nostr_key_from_bech32(pubkey_str, pubkey);
    } else if (strlen(pubkey_str) == 64) {
        /* Try hex */
        err = nostr_key_from_hex(pubkey_str, pubkey);
    } else {
        fprintf(stderr, "Error: Invalid public key format (expected npub or 64-char hex)\n");
        return -1;
    }

    if (err != NOSTR_OK) {
        fprintf(stderr, "Error: Failed to parse public key: %s\n", nostr_error_string(err));
        return -1;
    }

    return 0;
}
