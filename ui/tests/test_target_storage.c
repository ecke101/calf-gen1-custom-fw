#include "target_internal.h"

#include <string.h>

int main(int argument_count, char **arguments)
{
    static const char contents[] = "new config\n";
    if(argument_count != 3) return 2;
    return target_write_atomic_file(arguments[1], arguments[2],
                                    contents, strlen(contents),
                                    0640u, -1, -1) == 0 ? 0 : 1;
}
