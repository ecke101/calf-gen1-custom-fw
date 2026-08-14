#include "ngcd_audio.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ngcd_audio_control {
    void *library;
    int (*mixer_open)(void **, int);
    int (*mixer_attach)(void *, const char *);
    int (*mixer_register)(void *, void *, void **);
    int (*mixer_load)(void *);
    int (*mixer_close)(void *);
    void *(*first_element)(void *);
    void *(*next_element)(void *);
    const char *(*element_name)(void *);
    int (*has_capture_volume)(void *);
    int (*capture_volume_range)(void *, long *, long *);
    int (*set_capture_volume)(void *, long);
    int (*set_enum_item)(void *, int, unsigned int);
};

static int load_symbol(void *library, const char *name, void *output,
                       size_t output_size)
{
    void *address;
    (void)dlerror();
    address = dlsym(library, name);
    if (address == NULL || dlerror() != NULL || output_size != sizeof(address))
        return -1;
    memcpy(output, &address, output_size);
    return 0;
}

#define LOAD(control, member, name)                                           \
    do {                                                                      \
        if (load_symbol((control)->library, (name), &(control)->member,        \
                        sizeof((control)->member)) != 0)                       \
            goto fail;                                                        \
    } while (0)

int ngcd_audio_control_open(struct ngcd_audio_control **output)
{
    struct ngcd_audio_control *control;
    if (output == NULL)
        return -1;
    *output = NULL;
    control = calloc(1U, sizeof(*control));
    if (control == NULL)
        return -1;
    control->library = dlopen("/usr/lib/libasound.so.2",
                              RTLD_NOW | RTLD_LOCAL);
    if (control->library == NULL)
        goto fail;
    LOAD(control, mixer_open, "snd_mixer_open");
    LOAD(control, mixer_attach, "snd_mixer_attach");
    LOAD(control, mixer_register, "snd_mixer_selem_register");
    LOAD(control, mixer_load, "snd_mixer_load");
    LOAD(control, mixer_close, "snd_mixer_close");
    LOAD(control, first_element, "snd_mixer_first_elem");
    LOAD(control, next_element, "snd_mixer_elem_next");
    LOAD(control, element_name, "snd_mixer_selem_get_name");
    LOAD(control, has_capture_volume,
         "snd_mixer_selem_has_capture_volume");
    LOAD(control, capture_volume_range,
         "snd_mixer_selem_get_capture_volume_range");
    LOAD(control, set_capture_volume,
         "snd_mixer_selem_set_capture_volume_all");
    LOAD(control, set_enum_item, "snd_mixer_selem_set_enum_item");
    *output = control;
    return 0;

fail:
    ngcd_audio_control_close(control);
    return -1;
}

void ngcd_audio_control_close(struct ngcd_audio_control *control)
{
    if (control == NULL)
        return;
    if (control->library != NULL)
        (void)dlclose(control->library);
    free(control);
}

static int mixer_open_card(struct ngcd_audio_control *control,
                           const char *card, void **mixer)
{
    *mixer = NULL;
    if (control->mixer_open(mixer, 0) < 0 || *mixer == NULL)
        return -1;
    if (control->mixer_attach(*mixer, card) < 0 ||
        control->mixer_register(*mixer, NULL, NULL) < 0 ||
        control->mixer_load(*mixer) < 0) {
        (void)control->mixer_close(*mixer);
        *mixer = NULL;
        return -1;
    }
    return 0;
}

static void *find_element(struct ngcd_audio_control *control, void *mixer,
                          const char *name)
{
    void *element = control->first_element(mixer);
    while (element != NULL) {
        const char *candidate = control->element_name(element);
        if (candidate != NULL && strcmp(candidate, name) == 0)
            return element;
        element = control->next_element(element);
    }
    return NULL;
}

static int set_named_capture_volume(struct ngcd_audio_control *control,
                                    void *mixer, const char *name,
                                    long volume)
{
    void *element = find_element(control, mixer, name);
    return element != NULL && control->has_capture_volume(element) != 0 &&
                   control->set_capture_volume(element, volume) >= 0
               ? 0 : -1;
}

static int set_builtin_input(struct ngcd_audio_control *control, void *mixer,
                             int input)
{
    void *left = find_element(control, mixer, "Left Capture Mux");
    void *right = find_element(control, mixer, "Right Capture Mux");
    unsigned int item = input == 0 ? 1U : 0U;
    if (left == NULL || right == NULL ||
        control->set_enum_item(left, 0, item) < 0 ||
        control->set_enum_item(right, 0, item) < 0)
        return -1;
    return 0;
}

static int set_usb_volume(struct ngcd_audio_control *control, void *mixer,
                          int volume)
{
    void *element = control->first_element(mixer);
    while (element != NULL && control->has_capture_volume(element) == 0)
        element = control->next_element(element);
    if (element != NULL) {
        long minimum;
        long maximum;
        long requested;
        if (control->capture_volume_range(element, &minimum, &maximum) < 0 ||
            maximum < minimum)
            return -1;
        requested = minimum + (maximum - minimum) * volume / 100;
        return control->set_capture_volume(element, requested) >= 0 ? 0 : -1;
    }
    return -1;
}

int ngcd_audio_control_apply(struct ngcd_audio_control *control, int input,
                             int volume)
{
    static const int pga_gain[11] = {
        0, 7, 13, 18, 22, 25, 27, 28, 29, 30, 31,
    };
    static const int digital_gain[11] = {
        0, 90, 91, 92, 92, 92, 92, 93, 94, 95, 96,
    };
    void *mixer = NULL;
    int result = -1;
    int index;
    if (control == NULL || input < 0 || input > 2 ||
        volume < 0 || volume > 100)
        return -1;
    if (mixer_open_card(control, input == 2 ? "hw:1" : "hw:0", &mixer) != 0)
        return -1;
    if (input == 2) {
        result = set_usb_volume(control, mixer, volume);
    } else {
        index = volume / 10;
        if (set_builtin_input(control, mixer, input) == 0 &&
            set_named_capture_volume(control, mixer, "Capture",
                                     pga_gain[index]) == 0 &&
            set_named_capture_volume(control, mixer, "Digital",
                                     digital_gain[index]) == 0)
            result = 0;
    }
    if (control->mixer_close(mixer) < 0)
        result = -1;
    return result;
}

int ngcd_audio_control_detect_input(struct ngcd_audio_control *control,
                                    int *input)
{
    static const char card1[] = "/proc/asound/card1/id";
    static const char extcon[] =
        "/sys/devices/platform/sound-card/extcon/extcon4/state";
    char state[64];
    FILE *file;
    (void)control;
    if (input == NULL)
        return -1;
    file = fopen(card1, "r");
    if (file != NULL) {
        (void)fclose(file);
        *input = 2;
        return 0;
    }
    file = fopen(extcon, "r");
    if (file == NULL) {
        *input = 0;
        return 0;
    }
    state[0] = '\0';
    (void)fgets(state, sizeof(state), file);
    if (fclose(file) != 0)
        return -1;
    *input = strncmp(state, "MICROPHONE=1", 12U) == 0 ? 1 : 0;
    return 0;
}
