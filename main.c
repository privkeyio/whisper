/*
 * whisper - Encrypted DM pipe for Nostr (NIP-17 + NIP-44)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "whisper.h"

static void print_usage(void) {
    fprintf(stderr, "whisper - Encrypted Nostr DMs (NIP-17)\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  whisper send --to <npub> --relay <url> [key options]\n");
    fprintf(stderr, "  whisper recv --relay <url> [key options]\n\n");
    fprintf(stderr, "Key options (in order of priority):\n");
    fprintf(stderr, "  --nsec-file <path>    Read key from file (most secure)\n");
    fprintf(stderr, "  --nsec <nsec|hex>     Key as argument (visible in history)\n");
    fprintf(stderr, "  NOSTR_NSEC env var    Fallback if no --nsec option\n\n");
    fprintf(stderr, "Send options:\n");
    fprintf(stderr, "  --to <npub|hex>       Recipient public key\n");
    fprintf(stderr, "  --relay <url>         Relay URL\n");
    fprintf(stderr, "  --subject <text>      Optional subject\n");
    fprintf(stderr, "  --reply-to <id>       Reply to event ID\n");
    fprintf(stderr, "  --timeout <ms>        Timeout (default: 5000)\n\n");
    fprintf(stderr, "Recv options:\n");
    fprintf(stderr, "  --relay <url>         Relay URL\n");
    fprintf(stderr, "  --since <timestamp>   Only messages after timestamp\n");
    fprintf(stderr, "  --limit <n>           Max messages (0 = stream)\n");
    fprintf(stderr, "  --json                Output raw JSON\n");
    fprintf(stderr, "  --timeout <ms>        Timeout (default: 5000)\n\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  # Secure: key from file\n");
    fprintf(stderr, "  echo \"hello\" | whisper send --to npub1... --nsec-file ~/.nostr/key --relay wss://relay.damus.io\n\n");
    fprintf(stderr, "  # Using environment variable\n");
    fprintf(stderr, "  export NOSTR_NSEC=nsec1...\n");
    fprintf(stderr, "  whisper recv --relay wss://relay.damus.io\n");
}

static struct option long_options[] = {
    {"to",        required_argument, 0, 't'},
    {"nsec",      required_argument, 0, 'n'},
    {"nsec-file", required_argument, 0, 'f'},
    {"relay",     required_argument, 0, 'r'},
    {"subject",   required_argument, 0, 's'},
    {"reply-to",  required_argument, 0, 'p'},
    {"since",     required_argument, 0, 'S'},
    {"limit",     required_argument, 0, 'l'},
    {"json",      no_argument,       0, 'j'},
    {"timeout",   required_argument, 0, 'T'},
    {"help",      no_argument,       0, 'h'},
    {0, 0, 0, 0}
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return WHISPER_EXIT_INVALID_ARGS;
    }

    const char* command = argv[1];

    /* Skip command for option parsing */
    argc--;
    argv++;
    optind = 1;

    /* Common options */
    const char* nsec = NULL;
    const char* nsec_file = NULL;
    const char* relay_url = NULL;
    int timeout_ms = WHISPER_DEFAULT_TIMEOUT_MS;

    /* Send-specific options */
    const char* recipient = NULL;
    const char* subject = NULL;
    const char* reply_to = NULL;

    /* Recv-specific options */
    int64_t since = 0;
    int limit = 0;
    bool json_output = false;

    int opt;
    while ((opt = getopt_long(argc, argv, "t:n:f:r:s:p:S:l:jT:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 't': recipient = optarg; break;
            case 'n': nsec = optarg; break;
            case 'f': nsec_file = optarg; break;
            case 'r': relay_url = optarg; break;
            case 's': subject = optarg; break;
            case 'p': reply_to = optarg; break;
            case 'S': since = strtoll(optarg, NULL, 10); break;
            case 'l': limit = atoi(optarg); break;
            case 'j': json_output = true; break;
            case 'T': timeout_ms = atoi(optarg); break;
            case 'h':
                print_usage();
                return WHISPER_EXIT_OK;
            default:
                print_usage();
                return WHISPER_EXIT_INVALID_ARGS;
        }
    }

    if (strcmp(command, "send") == 0) {
        if (!recipient) {
            fprintf(stderr, "Error: --to is required for send\n");
            return WHISPER_EXIT_INVALID_ARGS;
        }
        if (!relay_url) {
            fprintf(stderr, "Error: --relay is required\n");
            return WHISPER_EXIT_INVALID_ARGS;
        }
        /* Key can come from --nsec, --nsec-file, or NOSTR_NSEC env var */

        whisper_send_config config = {
            .recipient = recipient,
            .nsec = nsec,
            .nsec_file = nsec_file,
            .relay_url = relay_url,
            .subject = subject,
            .reply_to = reply_to,
            .timeout_ms = timeout_ms
        };

        return whisper_send(&config);

    } else if (strcmp(command, "recv") == 0) {
        if (!relay_url) {
            fprintf(stderr, "Error: --relay is required\n");
            return WHISPER_EXIT_INVALID_ARGS;
        }
        /* Key can come from --nsec, --nsec-file, or NOSTR_NSEC env var */

        whisper_recv_config config = {
            .nsec = nsec,
            .nsec_file = nsec_file,
            .relay_url = relay_url,
            .since = since,
            .limit = limit,
            .json_output = json_output,
            .timeout_ms = timeout_ms
        };

        return whisper_recv(&config);

    } else if (strcmp(command, "help") == 0 || strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        print_usage();
        return WHISPER_EXIT_OK;
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        print_usage();
        return WHISPER_EXIT_INVALID_ARGS;
    }
}
