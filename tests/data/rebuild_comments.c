/* Braceless bodies with comments around them: rebuilt, each gets its braces
 * after the code, never inside a // comment, and else if stays else if
 * (tests/CMakeLists.txt). */
int f(int x)
{
    if (x > 1) // big
        x = 2;
    if (x < 0)
        // a comment on its own
        /* and another */
        return -1;
    else if (x == 0) // "not // a comment"
        return 0;
    else if (x == 5)
        return 5;
    for (int i = 0; i < 3; ++i)  /* block */  // line
        x += i;
    return x;
}
