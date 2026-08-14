#include "target_internal.h"

#include <stdio.h>

int main(int argument_count, char **arguments)
{
    char digest[65];
    int size_mb = 0;
    if(argument_count != 3) return 2;
    if(firmware_update_validate_paths(arguments[1], arguments[2],
                                      &size_mb, digest) != 0)
        return 1;
    printf("%d %s\n", size_mb, digest);
    return 0;
}
