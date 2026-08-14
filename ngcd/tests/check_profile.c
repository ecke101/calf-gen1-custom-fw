#include "ngcd.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    int index;
    if (argc < 2) {
        fprintf(stderr, "usage: %s PROFILE...\n", argv[0]);
        return 2;
    }
    for (index = 1; index < argc; ++index) {
        struct ngcd_profile profile;
        char error[256];
        if (ngcd_profile_load(argv[index], &profile, error, sizeof(error)) != 0) {
            fprintf(stderr, "%s: %s\n", argv[index], error);
            return 1;
        }
        printf("%s: %s, %zu encoder(s)\n", argv[index],
               profile.camera_mode, profile.encoder_count);
    }
    return 0;
}
