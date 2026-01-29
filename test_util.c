/*
 * Tests for whisper utility functions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* whisper_strip_control_chars(const char* input);

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do { \
    tests_run++; \
    printf("  %s... ", #name); \
    test_##name(); \
    tests_passed++; \
    printf("OK\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        exit(1); \
    } \
} while(0)

char* whisper_strip_control_chars(const char* input) {
    if (!input) return NULL;
    size_t len = strlen(input);
    char* output = malloc(len + 1);
    if (!output) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '\t' || c == '\n' || c >= 0x80 || (c >= 0x20 && c < 0x7F)) {
            output[j++] = (char)c;
        }
    }
    output[j] = '\0';
    return output;
}

TEST(strip_null_input) {
    char* result = whisper_strip_control_chars(NULL);
    ASSERT(result == NULL);
}

TEST(strip_empty_string) {
    char* result = whisper_strip_control_chars("");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "");
    free(result);
}

TEST(strip_ascii_passthrough) {
    char* result = whisper_strip_control_chars("Hello, World!");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "Hello, World!");
    free(result);
}

TEST(strip_preserves_tabs_newlines) {
    char* result = whisper_strip_control_chars("line1\n\tindented");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "line1\n\tindented");
    free(result);
}

TEST(strip_removes_control_chars) {
    char* result = whisper_strip_control_chars("a\001\002\003b");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "ab");
    free(result);
}

TEST(strip_removes_del) {
    char* result = whisper_strip_control_chars("a\177b");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "ab");
    free(result);
}

TEST(strip_preserves_utf8_emoji) {
    char* result = whisper_strip_control_chars("Hello 👋 World 🌍!");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "Hello 👋 World 🌍!");
    free(result);
}

TEST(strip_preserves_utf8_accented) {
    char* result = whisper_strip_control_chars("café résumé naïve");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "café résumé naïve");
    free(result);
}

TEST(strip_preserves_utf8_chinese) {
    char* result = whisper_strip_control_chars("你好世界");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "你好世界");
    free(result);
}

TEST(strip_preserves_utf8_japanese) {
    char* result = whisper_strip_control_chars("こんにちは");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "こんにちは");
    free(result);
}

TEST(strip_mixed_utf8_and_control) {
    char* result = whisper_strip_control_chars("🔑\x01key\177🔐");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "🔑key🔐");
    free(result);
}

int main(void) {
    printf("Running util tests:\n");

    RUN(strip_null_input);
    RUN(strip_empty_string);
    RUN(strip_ascii_passthrough);
    RUN(strip_preserves_tabs_newlines);
    RUN(strip_removes_control_chars);
    RUN(strip_removes_del);
    RUN(strip_preserves_utf8_emoji);
    RUN(strip_preserves_utf8_accented);
    RUN(strip_preserves_utf8_chinese);
    RUN(strip_preserves_utf8_japanese);
    RUN(strip_mixed_utf8_and_control);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return 0;
}
