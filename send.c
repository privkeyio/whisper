/*
 * whisper send - Send encrypted DM via NIP-17
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#define sleep(x) Sleep((x) * 1000)
#endif
#include "whisper.h"

#define MAX_MESSAGE_SIZE (64 * 1024)  /* 64KB max message */

static volatile int g_published = 0;
static volatile int g_connected = 0;

static void relay_state_cb(nostr_relay* relay, nostr_relay_state state, void* user_data) {
    (void)relay;
    (void)user_data;
    if (state == NOSTR_RELAY_CONNECTED) {
        g_connected = 1;
    } else if (state == NOSTR_RELAY_ERROR) {
        g_connected = -1;
    }
}

static void message_cb(const char* message_type, const char* data, void* user_data) {
    (void)user_data;
    if (strcmp(message_type, "OK") == 0) {
        g_published = 1;
    } else if (strcmp(message_type, "NOTICE") == 0) {
        fprintf(stderr, "Relay notice: %s\n", data);
    }
}

/* Read all stdin into buffer */
static char* read_stdin(size_t* out_len) {
    char* buf = malloc(MAX_MESSAGE_SIZE);
    if (!buf) return NULL;

    size_t total = 0;
    size_t n;
    while ((n = fread(buf + total, 1, MAX_MESSAGE_SIZE - total - 1, stdin)) > 0) {
        total += n;
        if (total >= MAX_MESSAGE_SIZE - 1) break;
    }
    buf[total] = '\0';

    /* Trim trailing newline */
    while (total > 0 && (buf[total-1] == '\n' || buf[total-1] == '\r')) {
        buf[--total] = '\0';
    }

    *out_len = total;
    return buf;
}

int whisper_send(const whisper_send_config* config) {
    int ret = WHISPER_EXIT_OK;
    nostr_privkey privkey;
    nostr_key sender_pubkey;
    nostr_key recipient_pubkey;
    nostr_event* dm = NULL;
    nostr_relay* relay = NULL;
    char* content = NULL;

    /* Initialize libnostr */
    if (nostr_init() != NOSTR_OK) {
        fprintf(stderr, "Error: Failed to initialize libnostr\n");
        return WHISPER_EXIT_CRYPTO_ERROR;
    }

    /* Load sender private key */
    if (whisper_load_privkey(config->nsec, config->nsec_file, &privkey, &sender_pubkey) != 0) {
        fprintf(stderr, "Error: Failed to load private key\n");
        ret = WHISPER_EXIT_KEY_ERROR;
        goto cleanup;
    }

    /* Parse recipient pubkey */
    if (whisper_parse_pubkey(config->recipient, &recipient_pubkey) != 0) {
        fprintf(stderr, "Error: Invalid recipient pubkey\n");
        ret = WHISPER_EXIT_KEY_ERROR;
        goto cleanup;
    }

    /* Read message from stdin */
    size_t content_len;
    content = read_stdin(&content_len);
    if (!content || content_len == 0) {
        fprintf(stderr, "Error: No message content (pipe message via stdin)\n");
        ret = WHISPER_EXIT_INVALID_ARGS;
        goto cleanup;
    }

    /* Create NIP-17 DM (gift-wrapped) */
    nostr_error_t err = nostr_nip17_send_dm(
        &dm,
        content,
        &privkey,
        &recipient_pubkey,
        config->subject,
        NULL,  /* reply_to - TODO: parse event ID */
        0      /* created_at = now */
    );

    if (err != NOSTR_OK) {
        fprintf(stderr, "Error: Failed to create DM: %s\n", nostr_error_string(err));
        ret = WHISPER_EXIT_CRYPTO_ERROR;
        goto cleanup;
    }

    /* Connect to relay */
    if (nostr_relay_create(&relay, config->relay_url) != NOSTR_OK) {
        fprintf(stderr, "Error: Failed to create relay\n");
        ret = WHISPER_EXIT_RELAY_ERROR;
        goto cleanup;
    }

    nostr_relay_set_message_callback(relay, message_cb, NULL);

    if (nostr_relay_connect(relay, relay_state_cb, NULL) != NOSTR_OK) {
        fprintf(stderr, "Error: Failed to connect to relay\n");
        ret = WHISPER_EXIT_RELAY_ERROR;
        goto cleanup;
    }

    /* Wait for connection - use sleep() to let libwebsockets process events */
    int wait_sec = 0;
    int timeout_sec = (config->timeout_ms + 999) / 1000;  /* Round up to seconds */
    while (g_connected == 0 && wait_sec < timeout_sec) {
        sleep(1);
        wait_sec++;
        if (g_connected == -1) {
            fprintf(stderr, "Error: Relay connection failed\n");
            ret = WHISPER_EXIT_RELAY_ERROR;
            goto cleanup;
        }
    }

    if (g_connected != 1) {
        fprintf(stderr, "Error: Relay connection timeout (try increasing --timeout)\n");
        ret = WHISPER_EXIT_TIMEOUT;
        goto cleanup;
    }

    /* Publish the gift-wrapped DM */
    if (nostr_publish_event(relay, dm) != NOSTR_OK) {
        fprintf(stderr, "Error: Failed to publish event\n");
        ret = WHISPER_EXIT_RELAY_ERROR;
        goto cleanup;
    }

    /* Wait for OK response */
    wait_sec = 0;
    while (g_published == 0 && wait_sec < timeout_sec) {
        sleep(1);
        wait_sec++;
    }

    if (!g_published) {
        fprintf(stderr, "Warning: No confirmation received (message may still be delivered)\n");
    }

    char id_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(&id_hex[i * 2], 3, "%02x", dm->id[i]);
    }
    id_hex[64] = '\0';
    printf("%s\n", id_hex);

cleanup:
    /* Secure wipe private key */
    secure_wipe(&privkey, sizeof(privkey));

    if (content) free(content);
    if (dm) nostr_event_destroy(dm);
    if (relay) {
        /* Let nostr_relay_destroy handle cleanup - it checks state internally */
        nostr_relay_destroy(relay);
    }
    nostr_cleanup();

    return ret;
}
