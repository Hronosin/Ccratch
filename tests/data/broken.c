/* Intentionally broken: the parser must survive and report errors. */
int main(void)
{
    int x = undeclared_thing + 1;
    return x
}
