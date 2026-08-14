#include "target_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

size_t string_length(const char *text)
{
    return strlen(text);
}

static void gallery_path(char *path, size_t capacity, int number)
{
    int written = snprintf(path, capacity,
                           "/mnt/mmcblk1p1/DCIM/100_CALF/V%07d.jpg",
                           number);
    assert(written > 0 && (size_t)written < capacity);
}

int main(void)
{
    gallery_state_t gallery;
    char path[GALLERY_PATH_SIZE];
    char expected[GALLERY_PATH_SIZE];
    int index;

    gallery_init(&gallery);
    assert(gallery.paths == (char **)0);
    assert(gallery.count == 0);
    assert(gallery.capacity == 0);

    for(index = 0; index < 600; ++index) {
        gallery_path(path, sizeof(path), 1000000 + index);
        assert(gallery_add_path(&gallery, path) == 0);
    }
    assert(gallery.count == 600);
    assert(gallery.capacity >= gallery.count);
    assert(gallery.capacity > 256);

    assert(gallery_sort_paths(&gallery) == 0);
    gallery_path(expected, sizeof(expected), 1000599);
    assert(strcmp(gallery.paths[0], expected) == 0);
    gallery_path(expected, sizeof(expected), 1000000);
    assert(strcmp(gallery.paths[599], expected) == 0);
    for(index = 1; index < gallery.count; ++index)
        assert(strcmp(gallery.paths[index - 1], gallery.paths[index]) >= 0);

    gallery.index = gallery.count - 1;
    gallery_remove_path(&gallery, gallery.index);
    assert(gallery.count == 599);
    assert(gallery.index == 598);
    gallery_path(expected, sizeof(expected), 1000001);
    assert(strcmp(gallery.paths[gallery.index], expected) == 0);

    gallery_remove_path(&gallery, 0);
    assert(gallery.count == 598);
    gallery_path(expected, sizeof(expected), 1000598);
    assert(strcmp(gallery.paths[0], expected) == 0);

    gallery_destroy(&gallery);
    assert(gallery.paths == (char **)0);
    assert(gallery.count == 0);
    assert(gallery.capacity == 0);
    assert(gallery.preview_pixels == (uint32_t *)0);
    return 0;
}
