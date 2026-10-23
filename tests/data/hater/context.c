/* Code Hater with context: Rust in a table of messages, or as what a
 * search looks for, is data, not propaganda. Said in a comment, or in
 * what the program prints, it is. */
#include <stdio.h>
#include <string.h>

static const char *const replies[] = {
    "We are not rewriting it in Rust.",
    "The borrow checker is not invited.",
    "Привет, Rust: нет.",
};

static int mentions_rust(const char *text)
{
    return strstr(text, "rust") != NULL || strcmp(text, "Rust") == 0;
}

int main(void)
{
    /* TODO: rewrite it in Rust */
    printf("%s %s\n", replies[0], mentions_rust("x") ? "yes" : "no");
    printf("%s, %s, %s\n", "Rust", "Rust again", "and Rust once more");
    puts("Привет, Rust!");
    return 0;
}
