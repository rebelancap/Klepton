// Unity scene-load banner parser behind KL_TRIM_ON_LOAD — `make trimload`, in
// `make check`.
//
// HARD BULLET launch 3 (2026-10-09): Unity's message carries a trailing "\n",
// so the bare `Loading:` end marker passed the old `msg[8] != '\0'` test and
// every scene name was logged with the newline inside the quotes.
#include <stdio.h>
#include <string.h>
#include "../runtime/klepton.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); \
    else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int main(void) {
    printf("=== Unity Loading: banner parser ===\n");
    char s[64];
    CHECK(kl_unity_load_banner("Loading:Main Menu\n", s, sizeof s) == 1
          && strcmp(s, "Main Menu") == 0, "named load, trailing newline stripped (\"%s\")", s);
    CHECK(kl_unity_load_banner("Loading:Stairway", s, sizeof s) == 1
          && strcmp(s, "Stairway") == 0, "named load, no newline");
    CHECK(kl_unity_load_banner("Loading:\n", s, sizeof s) == 2 && s[0] == 0,
          "bare end marker with newline = end (2)");
    CHECK(kl_unity_load_banner("Loading:", s, sizeof s) == 2, "bare end marker = end (2)");
    CHECK(kl_unity_load_banner("Loading: \r\n", s, sizeof s) == 2, "whitespace-only = end (2)");
    CHECK(kl_unity_load_banner("Loading OpenXR loader library at path: x", s, sizeof s) == 0,
          "the OpenXR loader line is not a banner");
    CHECK(kl_unity_load_banner("Unloading:Main Menu", s, sizeof s) == 0, "other text = 0");
    char tiny[4];
    CHECK(kl_unity_load_banner("Loading:Basement\n", tiny, sizeof tiny) == 1
          && strcmp(tiny, "Bas") == 0, "long name truncated, terminated");
    printf("%s (%d failure(s))\n", fails ? "FAILED" : "all checks pass", fails);
    return fails != 0;
}
