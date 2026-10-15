/* goto.c: the Linux kernel's favourite way out, and what Dijkstra warned about */
#include <stdio.h>
#include <stdlib.h>

static int load(const char *path)
{
    int err = -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        goto out;

    char *buf = malloc(4096);
    if (!buf)
        goto close_file;

    if (fread(buf, 1, 4096, f) == 0)
        goto free_buf;

    printf("%.16s\n", buf);
    err = 0;

free_buf:
    free(buf);
close_file:
    fclose(f);
out:
    return err;
}

int main(int argc, char **argv)
{
    int tries = 0;
retry:
    if (load(argc > 1 ? argv[1] : "goto.c") != 0 && ++tries < 3)
        goto retry;
    return 0;
}
