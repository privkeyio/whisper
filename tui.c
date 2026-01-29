/*
 * whisper TUI - Terminal UI for encrypted Nostr DMs
 */

#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>

#ifndef _WIN32
#include <unistd.h>
#include <pthread.h>
#else
#include <windows.h>
#define usleep(x) Sleep((x) / 1000)
#endif

#include "tui.h"

#ifndef HAVE_NOTCURSES

int whisper_tui(const whisper_tui_config* config) {
    (void)config;
    fprintf(stderr, "Error: TUI mode requires notcurses library\n");
    fprintf(stderr, "Install notcurses and rebuild whisper:\n");
    fprintf(stderr, "  apt install libnotcurses-dev  # Debian/Ubuntu\n");
    fprintf(stderr, "  brew install notcurses        # macOS\n");
    fprintf(stderr, "  nix-shell -p notcurses        # Nix\n");
    return WHISPER_EXIT_INVALID_ARGS;
}

#else /* HAVE_NOTCURSES */

#include <wchar.h>
#include <notcurses/notcurses.h>
#include <libwebsockets.h>

#define MAX_MESSAGE_SIZE (64 * 1024)
#define MAX_MESSAGES 1000
#define INPUT_ROWS 2

#define MAX_ALPHA 1.0

typedef struct tui_message {
    char sender_npub[20];
    char* content;
    time_t timestamp;
    bool is_outgoing;
    int fade_in_counter;
    struct tui_message* next;
    struct tui_message* prev;
} tui_message;

typedef struct {
    struct notcurses* nc;
    struct ncplane* status_plane;
    struct ncplane* message_plane;
    struct ncreader* input_reader;

    nostr_relay* relay;
    nostr_privkey privkey;
    nostr_key pubkey;
    nostr_key recipient;
    bool has_recipient;

    tui_message* messages_head;
    tui_message* messages_tail;
    int message_count;
    int scroll_offset;
#ifndef _WIN32
    pthread_mutex_t messages_mutex;
#else
    CRITICAL_SECTION messages_mutex;
#endif

    volatile sig_atomic_t running;
    volatile sig_atomic_t connected;
    volatile sig_atomic_t needs_redraw;

    int idle_ticks;
    double fade_alpha;

    int timeout_ms;
    const char* relay_url;
    char status_text[256];
} tui_context;

static volatile sig_atomic_t g_signal_received = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_signal_received = 1;
}

static void messages_lock(tui_context* ctx) {
#ifndef _WIN32
    pthread_mutex_lock(&ctx->messages_mutex);
#else
    EnterCriticalSection(&ctx->messages_mutex);
#endif
}

static void messages_unlock(tui_context* ctx) {
#ifndef _WIN32
    pthread_mutex_unlock(&ctx->messages_mutex);
#else
    LeaveCriticalSection(&ctx->messages_mutex);
#endif
}

static void messages_mutex_init(tui_context* ctx) {
#ifndef _WIN32
    pthread_mutex_init(&ctx->messages_mutex, NULL);
#else
    InitializeCriticalSection(&ctx->messages_mutex);
#endif
}

static void messages_mutex_destroy(tui_context* ctx) {
#ifndef _WIN32
    pthread_mutex_destroy(&ctx->messages_mutex);
#else
    DeleteCriticalSection(&ctx->messages_mutex);
#endif
}

static void format_short_npub(const nostr_key* key, char* out, size_t out_size) {
    char full_npub[100];
    nostr_key_to_bech32(key, "npub", full_npub, sizeof(full_npub));
    snprintf(out, out_size, "%.12s...", full_npub);
}

static void update_status_bar(tui_context* ctx) {
    if (!ctx->status_plane) return;

    ncplane_erase(ctx->status_plane);

    unsigned cols;
    ncplane_dim_yx(ctx->status_plane, NULL, &cols);

    int alpha = (int)(ctx->fade_alpha * 180);
    if (alpha < 40) alpha = 40;
    uint64_t chan = NCCHANNELS_INITIALIZER(alpha, alpha, alpha, 17, 17, 17);
    ncplane_set_channels(ctx->status_plane, chan);

    char my_npub[20];
    format_short_npub(&ctx->pubkey, my_npub, sizeof(my_npub));

    char recipient_str[32] = "no recipient";
    if (ctx->has_recipient) {
        format_short_npub(&ctx->recipient, recipient_str, sizeof(recipient_str));
    }

    const char* status = ctx->connected ? "connected" : "connecting...";

    ncplane_printf_yx(ctx->status_plane, 0, 1, "whisper  %s  to: %s  %s",
                      my_npub, recipient_str, status);

    if (ctx->status_text[0]) {
        int pos = (int)cols - (int)strlen(ctx->status_text) - 2;
        if (pos > 0) {
            ncplane_printf_yx(ctx->status_plane, 0, pos, "%s", ctx->status_text);
        }
    }
}

static tui_message* create_message(const char* content, const nostr_key* sender,
                                   time_t timestamp, bool is_outgoing) {
    tui_message* msg = calloc(1, sizeof(tui_message));
    if (!msg) return NULL;

    msg->content = whisper_strip_control_chars(content ? content : "");
    if (!msg->content) {
        free(msg);
        return NULL;
    }
    msg->timestamp = timestamp ? timestamp : time(NULL);
    msg->is_outgoing = is_outgoing;
    msg->fade_in_counter = 0;

    if (sender && !is_outgoing) {
        format_short_npub(sender, msg->sender_npub, sizeof(msg->sender_npub));
    }

    return msg;
}

static void evict_oldest_message_locked(tui_context* ctx) {
    if (!ctx->messages_head) return;

    tui_message* oldest = ctx->messages_head;
    ctx->messages_head = oldest->next;
    if (ctx->messages_head) {
        ctx->messages_head->prev = NULL;
    } else {
        ctx->messages_tail = NULL;
    }
    ctx->message_count--;
    free(oldest->content);
    free(oldest);
}

static void add_message_sorted(tui_context* ctx, tui_message* msg) {
    messages_lock(ctx);

    tui_message* insert_after = NULL;
    for (tui_message* m = ctx->messages_tail; m; m = m->prev) {
        if (m->timestamp <= msg->timestamp) {
            insert_after = m;
            break;
        }
    }

    if (!insert_after) {
        msg->prev = NULL;
        msg->next = ctx->messages_head;
        if (ctx->messages_head) {
            ctx->messages_head->prev = msg;
        }
        ctx->messages_head = msg;
        if (!ctx->messages_tail) {
            ctx->messages_tail = msg;
        }
    } else {
        msg->prev = insert_after;
        msg->next = insert_after->next;
        if (insert_after->next) {
            insert_after->next->prev = msg;
        }
        insert_after->next = msg;
        if (ctx->messages_tail == insert_after) {
            ctx->messages_tail = msg;
        }
    }

    ctx->message_count++;

    while (ctx->message_count > MAX_MESSAGES) {
        evict_oldest_message_locked(ctx);
    }

    ctx->scroll_offset = 0;

    messages_unlock(ctx);

    ctx->idle_ticks = 0;
    ctx->fade_alpha = MAX_ALPHA;
    ctx->needs_redraw = true;
}

static void free_message(tui_message* msg) {
    if (!msg) return;
    free(msg->content);
    free(msg);
}

static void free_all_messages(tui_context* ctx) {
    messages_lock(ctx);
    tui_message* msg = ctx->messages_head;
    while (msg) {
        tui_message* next = msg->next;
        free_message(msg);
        msg = next;
    }
    ctx->messages_head = NULL;
    ctx->messages_tail = NULL;
    ctx->message_count = 0;
    ctx->scroll_offset = 0;
    messages_unlock(ctx);
}

static void relay_state_cb(nostr_relay* relay, nostr_relay_state state, void* user_data) {
    (void)relay;
    tui_context* ctx = (tui_context*)user_data;

    switch (state) {
        case NOSTR_RELAY_CONNECTED:
            ctx->connected = true;
            snprintf(ctx->status_text, sizeof(ctx->status_text), "Connected");
            break;
        case NOSTR_RELAY_ERROR:
            ctx->connected = false;
            snprintf(ctx->status_text, sizeof(ctx->status_text), "Connection error");
            break;
        case NOSTR_RELAY_DISCONNECTED:
            ctx->connected = false;
            ctx->running = false;
            snprintf(ctx->status_text, sizeof(ctx->status_text), "Disconnected");
            break;
        default:
            break;
    }
    ctx->needs_redraw = true;
}

static void message_cb(const char* message_type, const char* data, void* user_data) {
    tui_context* ctx = (tui_context*)user_data;

    if (strcmp(message_type, "OK") == 0) {
        snprintf(ctx->status_text, sizeof(ctx->status_text), "Sent");
    } else if (strcmp(message_type, "NOTICE") == 0) {
        snprintf(ctx->status_text, sizeof(ctx->status_text), "Notice: %.50s", data);
    } else if (strcmp(message_type, "EOSE") == 0) {
        snprintf(ctx->status_text, sizeof(ctx->status_text), "Ready");
    }
    ctx->needs_redraw = true;
}

static void event_cb(const nostr_event* event, void* user_data) {
    tui_context* ctx = (tui_context*)user_data;

    if (event->kind != 1059) return;

    nostr_event* rumor = NULL;
    nostr_key sender_pubkey;
    nostr_error_t err = nostr_nip17_unwrap_dm(event, &ctx->privkey, &rumor, &sender_pubkey);

    if (err != NOSTR_OK) return;

    if (ctx->has_recipient && memcmp(&sender_pubkey, &ctx->recipient, sizeof(nostr_key)) != 0) {
        nostr_event_destroy(rumor);
        return;
    }

    tui_message* msg = create_message(rumor->content, &sender_pubkey,
                                      rumor->created_at, false);
    if (msg) {
        add_message_sorted(ctx, msg);
    }

    nostr_event_destroy(rumor);
    ctx->needs_redraw = true;
}

static int connect_relay(tui_context* ctx) {
    if (ctx->relay_url && strncmp(ctx->relay_url, "wss://", 6) != 0) {
        snprintf(ctx->status_text, sizeof(ctx->status_text),
                 "Warning: insecure relay (not wss://)");
    }

    if (nostr_relay_create(&ctx->relay, ctx->relay_url) != NOSTR_OK) {
        return -1;
    }

    nostr_relay_set_message_callback(ctx->relay, message_cb, ctx);

    if (nostr_relay_connect(ctx->relay, relay_state_cb, ctx) != NOSTR_OK) {
        nostr_relay_destroy(ctx->relay);
        ctx->relay = NULL;
        return -1;
    }

    int timeout_ms = (ctx->timeout_ms > 0) ? ctx->timeout_ms : WHISPER_DEFAULT_TIMEOUT_MS;
    int elapsed_ms = 0;
    while (!ctx->connected && ctx->running && elapsed_ms < timeout_ms) {
        usleep(100000);
        elapsed_ms += 100;
    }

    if (!ctx->connected) {
        return -1;
    }

    return 0;
}

static int subscribe_dms(tui_context* ctx) {
    if (!ctx->relay || !ctx->connected) return -1;

    char pubkey_hex[65];
    nostr_key_to_hex(&ctx->pubkey, pubkey_hex, sizeof(pubkey_hex));

    char filter[512];
    snprintf(filter, sizeof(filter),
             "{\"kinds\":[1059],\"#p\":[\"%s\"]}", pubkey_hex);

    if (nostr_subscribe(ctx->relay, "dm-inbox", filter, event_cb, ctx) != NOSTR_OK) {
        return -1;
    }

    return 0;
}

static void send_dm(tui_context* ctx, const char* content) {
    if (!ctx->has_recipient) {
        snprintf(ctx->status_text, sizeof(ctx->status_text),
                 "No recipient. Use /to <npub>");
        ctx->needs_redraw = true;
        return;
    }

    if (!ctx->relay || !ctx->connected) {
        snprintf(ctx->status_text, sizeof(ctx->status_text), "Not connected");
        ctx->needs_redraw = true;
        return;
    }

    nostr_event* dm = NULL;
    nostr_error_t err = nostr_nip17_send_dm(
        &dm, content, &ctx->privkey, &ctx->recipient,
        NULL, NULL, 0);

    if (err != NOSTR_OK) {
        snprintf(ctx->status_text, sizeof(ctx->status_text),
                 "Failed to create DM");
        ctx->needs_redraw = true;
        return;
    }

    if (nostr_publish_event(ctx->relay, dm) == NOSTR_OK) {
        tui_message* msg = create_message(content, NULL, 0, true);
        if (msg) {
            add_message_sorted(ctx, msg);
        }
        snprintf(ctx->status_text, sizeof(ctx->status_text), "Sending...");
    } else {
        snprintf(ctx->status_text, sizeof(ctx->status_text), "Send failed");
    }

    nostr_event_destroy(dm);
    ctx->needs_redraw = true;
}

static void handle_command(tui_context* ctx, const char* cmd) {
    if (strcmp(cmd, "/quit") == 0 || strcmp(cmd, "/q") == 0) {
        ctx->running = false;
    } else if (strcmp(cmd, "/clear") == 0) {
        free_all_messages(ctx);
        ctx->needs_redraw = true;
    } else if (strncmp(cmd, "/to ", 4) == 0) {
        const char* npub = cmd + 4;
        while (*npub && isspace((unsigned char)*npub)) npub++;

        if (whisper_parse_pubkey(npub, &ctx->recipient) == 0) {
            ctx->has_recipient = true;
            free_all_messages(ctx);
            snprintf(ctx->status_text, sizeof(ctx->status_text), "Recipient set");
        } else {
            snprintf(ctx->status_text, sizeof(ctx->status_text), "Invalid npub");
        }
        ctx->needs_redraw = true;
    } else if (strcmp(cmd, "/help") == 0) {
        snprintf(ctx->status_text, sizeof(ctx->status_text),
                 "/to <npub> /clear /quit");
        ctx->needs_redraw = true;
    } else {
        snprintf(ctx->status_text, sizeof(ctx->status_text),
                 "Unknown: %s", cmd);
        ctx->needs_redraw = true;
    }
}

static int setup_ui(tui_context* ctx) {
    struct ncplane* std = notcurses_stdplane(ctx->nc);
    if (!std) {
        return -1;
    }

    unsigned urows, ucols;
    ncplane_dim_yx(std, &urows, &ucols);

    int srows = (int)urows;
    int scols = (int)ucols;
    if (scols < 1) scols = 1;

    int message_rows = srows - INPUT_ROWS - 2;
    if (message_rows < 1) message_rows = 1;

    int input_y = srows - INPUT_ROWS;
    if (input_y < 0) input_y = 0;

    ncplane_set_bg_rgb8(std, 0, 0, 0);
    ncplane_erase(std);

    struct ncplane_options status_opts = {
        .y = 0, .x = 0,
        .rows = 1, .cols = (unsigned)scols,
    };
    ctx->status_plane = ncplane_create(std, &status_opts);
    if (!ctx->status_plane) {
        return -1;
    }

    struct ncplane_options msg_opts = {
        .y = 1, .x = 0,
        .rows = (unsigned)message_rows, .cols = (unsigned)scols,
    };
    ctx->message_plane = ncplane_create(std, &msg_opts);
    if (!ctx->message_plane) {
        ncplane_destroy(ctx->status_plane);
        ctx->status_plane = NULL;
        return -1;
    }
    ncplane_set_bg_rgb8(ctx->message_plane, 0, 0, 0);

    struct ncplane_options input_opts = {
        .y = input_y, .x = 0,
        .rows = INPUT_ROWS, .cols = (unsigned)scols,
    };
    struct ncplane* input_plane = ncplane_create(std, &input_opts);
    if (!input_plane) {
        ncplane_destroy(ctx->message_plane);
        ctx->message_plane = NULL;
        ncplane_destroy(ctx->status_plane);
        ctx->status_plane = NULL;
        return -1;
    }

    struct ncreader_options reader_opts = {
        .tchannels = NCCHANNELS_INITIALIZER(190, 190, 190, 17, 17, 17),
        .flags = NCREADER_OPTION_HORSCROLL | NCREADER_OPTION_CURSOR,
    };
    ctx->input_reader = ncreader_create(input_plane, &reader_opts);
    if (!ctx->input_reader) {
        ncplane_destroy(input_plane);
        ncplane_destroy(ctx->message_plane);
        ctx->message_plane = NULL;
        ncplane_destroy(ctx->status_plane);
        ctx->status_plane = NULL;
        return -1;
    }

    return 0;
}

typedef struct {
    char sender_npub[20];
    char content[512];
    time_t timestamp;
    bool is_outgoing;
} render_msg_copy;

static void render_message_copy(struct ncplane* plane, const render_msg_copy* msg, int row) {
    struct tm tm_buf;
    struct tm* tm_info = localtime_r(&msg->timestamp, &tm_buf);
    char time_str[6];
    if (tm_info) {
        strftime(time_str, sizeof(time_str), "%H:%M", tm_info);
    } else {
        snprintf(time_str, sizeof(time_str), "??:??");
    }

    uint64_t time_chan = NCCHANNELS_INITIALIZER(100, 100, 100, 0, 0, 0);
    ncplane_set_channels(plane, time_chan);
    ncplane_printf_yx(plane, row, 1, "%s", time_str);

    const char* sender = msg->is_outgoing ? "you" : msg->sender_npub;
    uint64_t sender_chan;
    if (msg->is_outgoing) {
        sender_chan = NCCHANNELS_INITIALIZER(130, 170, 210, 0, 0, 0);
    } else {
        sender_chan = NCCHANNELS_INITIALIZER(180, 160, 120, 0, 0, 0);
    }
    ncplane_set_channels(plane, sender_chan);
    ncplane_printf_yx(plane, row, 7, "%-12s", sender);

    uint64_t content_chan;
    if (msg->is_outgoing) {
        content_chan = NCCHANNELS_INITIALIZER(180, 200, 220, 0, 0, 0);
    } else {
        content_chan = NCCHANNELS_INITIALIZER(200, 200, 200, 0, 0, 0);
    }
    ncplane_set_channels(plane, content_chan);
    ncplane_printf_yx(plane, row, 20, "%s", msg->content);
}

static void render_messages(tui_context* ctx) {
    if (!ctx->message_plane) return;

    ncplane_erase(ctx->message_plane);

    unsigned rows;
    ncplane_dim_yx(ctx->message_plane, &rows, NULL);

    messages_lock(ctx);

    int visible_rows = (int)rows;
    int total_messages = ctx->message_count;
    int start_idx = total_messages - visible_rows - ctx->scroll_offset;
    if (start_idx < 0) start_idx = 0;

    tui_message* msg = ctx->messages_head;
    int idx = 0;
    while (msg && idx < start_idx) {
        msg = msg->next;
        idx++;
    }

    int row = visible_rows - 1;
    int displayed = 0;
    render_msg_copy display_msgs[256];
    int display_count = 0;

    while (msg && displayed < visible_rows) {
        if (display_count < 256) {
            render_msg_copy* copy = &display_msgs[display_count++];
            memcpy(copy->sender_npub, msg->sender_npub, sizeof(copy->sender_npub));
            snprintf(copy->content, sizeof(copy->content), "%s", msg->content ? msg->content : "");
            copy->timestamp = msg->timestamp;
            copy->is_outgoing = msg->is_outgoing;
        }
        msg = msg->next;
        displayed++;
    }

    messages_unlock(ctx);

    for (int i = display_count - 1; i >= 0 && row >= 0; i--) {
        render_message_copy(ctx->message_plane, &display_msgs[i], row);
        row--;
    }
}

static void render(tui_context* ctx) {
    update_status_bar(ctx);
    render_messages(ctx);
    notcurses_render(ctx->nc);
    ctx->needs_redraw = false;
}

static void handle_input(tui_context* ctx, uint32_t key, ncinput* ni) {
    ctx->idle_ticks = 0;
    ctx->fade_alpha = MAX_ALPHA;

    if (key == 'q' && ncinput_ctrl_p(ni)) {
        ctx->running = false;
        return;
    }

    if (key == NCKEY_ENTER && !ncinput_shift_p(ni)) {
        char* content = ncreader_contents(ctx->input_reader);
        if (content && strlen(content) > 0) {
            char* trimmed = content;
            while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
            size_t len = strlen(trimmed);
            while (len > 0 && isspace((unsigned char)trimmed[len-1])) {
                trimmed[--len] = '\0';
            }

            if (trimmed[0] == '/') {
                handle_command(ctx, trimmed);
            } else if (len > 0) {
                send_dm(ctx, trimmed);
            }
        }
        free(content);
        ncreader_clear(ctx->input_reader);
        ctx->needs_redraw = true;
        return;
    }

    if (key == NCKEY_PGUP || (key == 'k' && ncinput_ctrl_p(ni))) {
        if (ctx->message_plane) {
            unsigned rows;
            ncplane_dim_yx(ctx->message_plane, &rows, NULL);
            int max_scroll = ctx->message_count - (int)rows;
            if (max_scroll < 0) max_scroll = 0;
            ctx->scroll_offset++;
            if (ctx->scroll_offset > max_scroll) ctx->scroll_offset = max_scroll;
            ctx->needs_redraw = true;
        }
        return;
    }

    if (key == NCKEY_PGDOWN || (key == 'j' && ncinput_ctrl_p(ni))) {
        if (ctx->message_plane) {
            ctx->scroll_offset--;
            if (ctx->scroll_offset < 0) ctx->scroll_offset = 0;
            ctx->needs_redraw = true;
        }
        return;
    }

    if (ctx->input_reader) {
        ncreader_offer_input(ctx->input_reader, ni);
        ctx->needs_redraw = true;
    }
}

static void run_event_loop(tui_context* ctx) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 };
    ncinput ni;
    bool subscribed = false;

    ctx->running = true;
    ctx->fade_alpha = MAX_ALPHA;

    while (ctx->running && !g_signal_received) {
        if (ctx->connected && !subscribed) {
            if (subscribe_dms(ctx) == 0) {
                subscribed = true;
            }
        }

        uint32_t key = notcurses_get(ctx->nc, &ts, &ni);

        if (key == (uint32_t)-1) {
            continue;
        }

        if (key == 0) {
            ctx->idle_ticks++;
            if (ctx->needs_redraw) {
                render(ctx);
            }
            continue;
        }

        handle_input(ctx, key, &ni);

        if (ctx->needs_redraw) {
            render(ctx);
        }
    }
}

static void cleanup(tui_context* ctx) {
    secure_wipe(&ctx->privkey, sizeof(ctx->privkey));

    free_all_messages(ctx);
    messages_mutex_destroy(ctx);

    if (ctx->relay) {
        if (ctx->relay->state == NOSTR_RELAY_CONNECTED) {
            nostr_relay_unsubscribe(ctx->relay, "dm-inbox");
        }
        nostr_relay_destroy(ctx->relay);
    }

    if (ctx->input_reader) {
        ncreader_destroy(ctx->input_reader, NULL);
    }
    if (ctx->nc) {
        notcurses_stop(ctx->nc);
    }

    nostr_cleanup();
}

int whisper_tui(const whisper_tui_config* config) {
    tui_context ctx = {0};
    g_signal_received = 0;

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

    if (nostr_init() != NOSTR_OK) {
        fprintf(stderr, "Error: Failed to initialize libnostr\n");
        return WHISPER_EXIT_CRYPTO_ERROR;
    }

    if (whisper_load_privkey(config->nsec, config->nsec_file,
                             &ctx.privkey, &ctx.pubkey) != 0) {
        fprintf(stderr, "Error: Failed to load private key\n");
        nostr_cleanup();
        return WHISPER_EXIT_KEY_ERROR;
    }

    if (config->recipient) {
        if (whisper_parse_pubkey(config->recipient, &ctx.recipient) == 0) {
            ctx.has_recipient = true;
        } else {
            fprintf(stderr, "Warning: Invalid recipient, use /to to set\n");
        }
    }

    ctx.relay_url = config->relay_url;
    ctx.timeout_ms = config->timeout_ms;
    messages_mutex_init(&ctx);
    snprintf(ctx.status_text, sizeof(ctx.status_text), "Starting...");

    struct notcurses_options nc_opts = {
        .flags = NCOPTION_SUPPRESS_BANNERS,
    };
    ctx.nc = notcurses_core_init(&nc_opts, NULL);
    if (!ctx.nc) {
        fprintf(stderr, "Error: Failed to initialize notcurses\n");
        messages_mutex_destroy(&ctx);
        secure_wipe(&ctx.privkey, sizeof(ctx.privkey));
        nostr_cleanup();
        return WHISPER_EXIT_CRYPTO_ERROR;
    }

    if (setup_ui(&ctx) != 0) {
        fprintf(stderr, "Error: Failed to setup UI (terminal too small?)\n");
        notcurses_stop(ctx.nc);
        messages_mutex_destroy(&ctx);
        secure_wipe(&ctx.privkey, sizeof(ctx.privkey));
        nostr_cleanup();
        return WHISPER_EXIT_CRYPTO_ERROR;
    }
    render(&ctx);

    lws_set_log_level(0, NULL);

    if (connect_relay(&ctx) != 0) {
        snprintf(ctx.status_text, sizeof(ctx.status_text), "Connection failed");
        render(&ctx);
    }

    run_event_loop(&ctx);

    cleanup(&ctx);

    return WHISPER_EXIT_OK;
}

#endif /* HAVE_NOTCURSES */
