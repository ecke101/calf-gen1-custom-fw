#include "target_internal.h"

static size_t power_string_length(const char *text)
{
    size_t length = 0;
    while(text[length] != '\0') ++length;
    return length;
}

static const char *power_find_text(const char *text, const char *needle)
{
    size_t index;
    size_t needle_length = power_string_length(needle);
    for(index = 0; text[index] != '\0'; ++index) {
        size_t matched = 0;
        while(matched < needle_length &&
              text[index + matched] == needle[matched])
            ++matched;
        if(matched == needle_length) return text + index;
    }
    return (const char *)0;
}

static int power_read_text(const char *path, char *buffer, size_t capacity)
{
    int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    size_t used = 0;
    if(descriptor < 0 || capacity == 0) return -1;
    while(used + 1u < capacity) {
        size_t available = capacity - used - 1u;
        ssize_t count = read(descriptor, buffer + used, available);
        if(count < 0 || count > (ssize_t)available) {
            close(descriptor);
            return -1;
        }
        if(count == 0) break;
        used += (size_t)count;
    }
    if(used + 1u == capacity) {
        char extra;
        if(read(descriptor, &extra, 1) > 0) {
            close(descriptor);
            return -1;
        }
    }
    if(close(descriptor) != 0 || used >= capacity) return -1;
    buffer[used] = '\0';
    return 0;
}

static int hex_digit_value(char character)
{
    if(character >= '0' && character <= '9') return character - '0';
    if(character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if(character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

static int regmap_word(const char *registers, const char *key,
                       unsigned *value)
{
    const char *cursor = registers;
    for(;;) {
        int index;
        unsigned parsed = 0;
        cursor = power_find_text(cursor, key);
        if(cursor == (const char *)0) return -1;
        if(cursor == registers || cursor[-1] == '\n') {
            cursor += power_string_length(key);
            for(index = 0; index < 4; ++index) {
                int digit = hex_digit_value(cursor[index]);
                if(digit < 0) return -1;
                parsed = parsed * 16u + (unsigned)digit;
            }
            *value = parsed;
            return 0;
        }
        ++cursor;
    }
}

int target_power_parse_registers(const char *registers, int recording,
                                 calf_power_sample_t *sample)
{
    unsigned adc_vbus_psys;
    unsigned adc_ibat;
    unsigned adc_iin_cmpin;
    unsigned adc_vsys_vbat;
    unsigned charge_option_1;
    unsigned adc_option;
    sample->usb_mv = 0;
    sample->usb_ma = 0;
    sample->usb_mw = 0;
    sample->battery_mv = 0;
    sample->battery_ma = 0;
    sample->battery_mw = 0;
    sample->device_mw = 0;
    sample->recording = recording != 0;
    sample->valid = 0;
    if(regmap_word(registers, "26: ", &adc_vbus_psys) != 0 ||
       regmap_word(registers, "28: ", &adc_ibat) != 0 ||
       regmap_word(registers, "2a: ", &adc_iin_cmpin) != 0 ||
       regmap_word(registers, "2c: ", &adc_vsys_vbat) != 0 ||
       regmap_word(registers, "30: ", &charge_option_1) != 0 ||
       regmap_word(registers, "3a: ", &adc_option) != 0)
        return -1;
    return calf_power_decode_bq25703(adc_vbus_psys, adc_ibat,
                                    adc_iin_cmpin, adc_vsys_vbat,
                                    charge_option_1, adc_option,
                                    recording, sample);
}

int target_read_power_sample(calf_power_sample_t *sample, int recording)
{
    char registers[1024];
    if(power_read_text(POWER_REGMAP_PATH, registers, sizeof(registers)) != 0)
        return -1;
    return target_power_parse_registers(registers, recording, sample);
}
