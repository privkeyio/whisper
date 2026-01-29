/*
 * whisper TUI - Terminal UI for encrypted Nostr DMs
 */

#ifndef WHISPER_TUI_H
#define WHISPER_TUI_H

#include <stdbool.h>
#include "whisper.h"

typedef struct {
    const char* nsec;
    const char* nsec_file;
    const char* relay_url;
    const char* recipient;
    int timeout_ms;
} whisper_tui_config;

int whisper_tui(const whisper_tui_config* config);

#endif /* WHISPER_TUI_H */
