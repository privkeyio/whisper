/*
 * whisper recv - Receive encrypted DMs via NIP-17
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#define sleep(x) Sleep((x) * 1000)
#endif
#include "whisper.h"

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_connected = 0;
static int g_message_count = 0;

/* Recv context passed to callbacks */
typedef struct {
    nostr_privkey privkey;
    nostr_key pubkey;
    bool json_output;
    int limit;
} recv_context;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void relay_state_cb(nostr_relay* relay, nostr_relay_state state, void* user_data) {
    (void)relay;
    (void)user_data;
    switch (state) {
        case NOSTR_RELAY_CONNECTED:    g_connected = 1; break;
        case NOSTR_RELAY_ERROR:        g_connected = -1; break;
        case NOSTR_RELAY_DISCONNECTED: g_running = 0; break;
        default: break;
    }
}

static void message_cb(const char* message_type, const char* data, void* user_data) {
    (void)user_data;
    if (strcmp(message_type, "EOSE") == 0) {
        /* End of stored events - now streaming live */
    } else if (strcmp(message_type, "NOTICE") == 0) {
        fprintf(stderr, "Relay notice: %s\n", data);
    }
}

static void event_cb(const nostr_event* event, void* user_data) {
    recv_context* ctx = (recv_context*)user_data;

    if (event->kind != 1059) {
        return;
    }

    nostr_event* rumor = NULL;
    nostr_key sender_pubkey;
    nostr_error_t err = nostr_nip17_unwrap_dm(event, &ctx->privkey, &rumor, &sender_pubkey);

    if (err != NOSTR_OK) {
        return;
    }

    g_message_count++;

    if (ctx->json_output) {
        /* JSON output */
        char sender_npub[100];
        nostr_key_to_bech32(&sender_pubkey, "npub", sender_npub, sizeof(sender_npub));

        /* Escape content for JSON */
        const char* content = rumor->content ? rumor->content : "";
        printf("{\"from\":\"%s\",\"content\":\"", sender_npub);
        for (const char* p = content; *p; p++) {
            switch (*p) {
                case '"': printf("\\\""); break;
                case '\\': printf("\\\\"); break;
                case '\n': printf("\\n"); break;
                case '\r': printf("\\r"); break;
                case '\t': printf("\\t"); break;
                default:
                    if ((unsigned char)*p < 32) {
                        printf("\\u%04x", (unsigned char)*p);
                    } else {
                        putchar(*p);
                    }
            }
        }
        printf("\",\"created_at\":%lld}\n", (long long)rumor->created_at);
    } else {
        /* Human-readable output */
        char sender_npub[100];
        nostr_key_to_bech32(&sender_pubkey, "npub", sender_npub, sizeof(sender_npub));

        /* Format timestamp */
        time_t ts = (time_t)rumor->created_at;
        struct tm tm_buf;
        struct tm* tm_info = localtime_r(&ts, &tm_buf);
        char time_str[32];
        if (tm_info) {
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", tm_info);
        } else {
            snprintf(time_str, sizeof(time_str), "(invalid time)");
        }

        /* Truncate npub for display */
        char short_npub[16];
        snprintf(short_npub, sizeof(short_npub), "%.12s...", sender_npub);

        const char* raw_content = rumor->content ? rumor->content : "(empty)";
        char* content = whisper_strip_control_chars(raw_content);
        printf("%s %s %s\n", time_str, short_npub, content ? content : "(empty)");
        free(content);
    }

    fflush(stdout);
    nostr_event_destroy(rumor);

    /* Check limit */
    if (ctx->limit > 0 && g_message_count >= ctx->limit) {
        g_running = 0;
    }
}

int whisper_recv(const whisper_recv_config* config) {
    int ret = WHISPER_EXIT_OK;
    recv_context ctx = {0};
    nostr_relay* relay = NULL;

    /* Set up signal handler for clean shutdown */
#ifndef _WIN32
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#else
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif

    /* Initialize libnostr */
    if (nostr_init() != NOSTR_OK) {
        fprintf(stderr, "Error: Failed to initialize libnostr\n");
        return WHISPER_EXIT_CRYPTO_ERROR;
    }

    /* Load private key */
    if (whisper_load_privkey(config->nsec, config->nsec_file, &ctx.privkey, &ctx.pubkey) != 0) {
        fprintf(stderr, "Error: Failed to load private key\n");
        ret = WHISPER_EXIT_KEY_ERROR;
        goto cleanup;
    }

    ctx.json_output = config->json_output;
    ctx.limit = config->limit;

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
    int timeout_sec = (config->timeout_ms + 999) / 1000;
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

    /* Build subscription filter for gift wrap events addressed to us */
    char pubkey_hex[65];
    nostr_key_to_hex(&ctx.pubkey, pubkey_hex, sizeof(pubkey_hex));

    char filter[512];
    if (config->since > 0) {
        snprintf(filter, sizeof(filter),
            "{\"kinds\":[1059],\"#p\":[\"%s\"],\"since\":%lld}",
            pubkey_hex, (long long)config->since);
    } else {
        snprintf(filter, sizeof(filter),
            "{\"kinds\":[1059],\"#p\":[\"%s\"]}",
            pubkey_hex);
    }

    /* Subscribe */
    if (nostr_subscribe(relay, "dm-inbox", filter, event_cb, &ctx) != NOSTR_OK) {
        fprintf(stderr, "Error: Failed to subscribe\n");
        ret = WHISPER_EXIT_RELAY_ERROR;
        goto cleanup;
    }

    /* Main loop - wait for messages */
    while (g_running && relay->state == NOSTR_RELAY_CONNECTED) {
        sleep(1);
    }

cleanup:
    /* Secure wipe private key */
    secure_wipe(&ctx.privkey, sizeof(ctx.privkey));

    if (relay) {
        if (relay->state == NOSTR_RELAY_CONNECTED) {
            nostr_relay_unsubscribe(relay, "dm-inbox");
        }
        /* Let nostr_relay_destroy handle cleanup - it checks state internally */
        nostr_relay_destroy(relay);
    }
    nostr_cleanup();

    return ret;
}
