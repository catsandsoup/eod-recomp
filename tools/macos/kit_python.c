#include <Python.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    char **args = calloc((size_t)argc + 3, sizeof(*args));
    if (args == NULL)
        return EXIT_FAILURE;

    // Every worker also launches this executable through sys.executable.
    unsetenv("__PYVENV_LAUNCHER__");
    args[0] = argv[0];
    args[1] = "-I";
    args[2] = "-B";
    for (int i = 1; i < argc; ++i)
        args[i + 2] = argv[i];
    int status = Py_BytesMain(argc + 2, args);
    free(args);
    return status;
}
