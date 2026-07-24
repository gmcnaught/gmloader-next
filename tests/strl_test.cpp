// Host-native unit test for the bionic strlcpy/strlcat thunks.
//
// These are the size-bounded copies glibc does not provide, so gmloader
// implements them for the loaded Android .so. A bug here silently corrupts the
// heap of a process we cannot debug easily, so the truncation paths are worth
// pinning down. Build/run:
//   make -f Makefile.gmloader strl-test
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Declared exactly as stdio.cpp defines them (C++ linkage, ABI_ATTR is empty
// off-ARM), so this links against the real thunk objects.
size_t strlcat_impl(char *dst, const char *src, size_t maxlen);
size_t strlcpy_impl(char *dst, const char *src, size_t maxlen);

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); fails++; } } while (0)

// dst buffer of `maxlen` bytes inside a larger canary region: anything written
// past maxlen is a buffer overflow and shows up as a disturbed canary.
#define CANARY 0x7E
static char buf[256];

static void arm(size_t maxlen, const char *initial) {
    memset(buf, CANARY, sizeof(buf));
    memset(buf, 0, maxlen);
    if (initial) memcpy(buf, initial, strlen(initial) + 1);
}

static int canary_intact(size_t maxlen) {
    for (size_t i = maxlen; i < sizeof(buf); i++)
        if ((unsigned char)buf[i] != CANARY) {
            printf("  overflow: byte %zu past a %zu-byte buffer was written\n", i - maxlen, maxlen);
            return 0;
        }
    return 1;
}

static void test_strlcat_fits(void) {
    arm(16, "abc");
    size_t r = strlcat_impl(buf, "def", 16);
    CHECK(r == 6);
    CHECK(!strcmp(buf, "abcdef"));
    CHECK(canary_intact(16));
}

static void test_strlcat_truncates(void) {
    // 8-byte buffer already holding 3 chars: only 4 more chars fit (+NUL).
    arm(8, "abc");
    size_t r = strlcat_impl(buf, "0123456789", 8);
    CHECK(r == 13);                      // BSD returns the length it TRIED to build
    CHECK(!strcmp(buf, "abc0123"));      // 3 + 4 chars, NUL at index 7
    CHECK(canary_intact(8));             // must not write past the 8th byte
}

static void test_strlcat_exact_fit(void) {
    arm(8, "abc");
    size_t r = strlcat_impl(buf, "1234", 8);
    CHECK(r == 7);
    CHECK(!strcmp(buf, "abc1234"));
    CHECK(canary_intact(8));
}

static void test_strlcat_no_room(void) {
    // dst already fills the buffer bar the NUL: nothing can be appended.
    arm(8, "abcdefg");
    size_t r = strlcat_impl(buf, "xyz", 8);
    CHECK(r == 10);
    CHECK(!strcmp(buf, "abcdefg"));
    CHECK(canary_intact(8));
}

static void test_strlcat_empty_dst(void) {
    arm(8, "");
    size_t r = strlcat_impl(buf, "0123456789", 8);
    CHECK(r == 10);
    CHECK(!strcmp(buf, "0123456"));
    CHECK(canary_intact(8));
}

static void test_strlcpy(void) {
    arm(8, NULL);
    size_t r = strlcpy_impl(buf, "0123456789", 8);
    CHECK(r == 10);
    CHECK(!strcmp(buf, "0123456"));
    CHECK(canary_intact(8));

    arm(8, NULL);
    r = strlcpy_impl(buf, "abc", 8);
    CHECK(r == 3);
    CHECK(!strcmp(buf, "abc"));
    CHECK(canary_intact(8));
}

int main(void) {
    test_strlcat_fits();
    test_strlcat_truncates();
    test_strlcat_exact_fit();
    test_strlcat_no_room();
    test_strlcat_empty_dst();
    test_strlcpy();
    if (fails) { printf("%d checks FAILED\n", fails); return 1; }
    printf("strlcpy/strlcat thunks OK\n");
    return 0;
}
