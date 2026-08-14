#include "target_internal.h"
#include "calf_sha256.h"

static int update_read_exact(int descriptor, unsigned char *buffer,
                             size_t count)
{
    size_t used = 0;
    while(used < count) {
        ssize_t result = read(descriptor, buffer + used, count - used);
        if(result <= 0) return -1;
        used += (size_t)result;
    }
    return 0;
}

static int update_string_equal(const char *left, const char *right)
{
    size_t index = 0;
    while(left[index] != '\0' && left[index] == right[index]) ++index;
    return left[index] == '\0' && right[index] == '\0';
}

static int update_read_text(const char *path, char *buffer, size_t capacity)
{
    int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    size_t used = 0;
    if(descriptor < 0 || capacity == 0) return -1;
    while(used + 1u < capacity) {
        size_t available = capacity - used - 1u;
        ssize_t count = read(descriptor, buffer + used, available);
        if(count < 0) {
            close(descriptor);
            return -1;
        }
        if(count == 0) break;
        if(count > (ssize_t)available) {
            close(descriptor);
            return -1;
        }
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

static int parse_tar_octal(const unsigned char *field, size_t length,
                           unsigned long *value)
{
    size_t index = 0;
    unsigned long parsed = 0;
    int found = 0;
    while(index < length && (field[index] == ' ' || field[index] == '\0'))
        ++index;
    while(index < length && field[index] >= '0' && field[index] <= '7') {
        if(parsed > 0x1ffffffful) return -1;
        parsed = parsed * 8ul + (unsigned long)(field[index] - '0');
        found = 1;
        ++index;
    }
    while(index < length && (field[index] == ' ' || field[index] == '\0'))
        ++index;
    if(!found || index != length) return -1;
    *value = parsed;
    return 0;
}

static int firmware_member_index(const char *name)
{
    static const char *const allowed[] = {
        "uboot.img", "recovery.img", "boot.img", "resource.img",
        "rootfs.img", "app.img", "local.img",
    };
    size_t index;
    for(index = 0; index < sizeof(allowed) / sizeof(allowed[0]); ++index)
        if(update_string_equal(name, allowed[index])) return (int)index;
    return -1;
}

static int sha256_descriptor(int descriptor, char hexadecimal[65])
{
    static const char digits[] = "0123456789abcdef";
    calf_sha256_t context;
    unsigned char buffer[16384];
    unsigned char digest[32];
    size_t index;
    if(lseek(descriptor, 0, SEEK_SET) != 0) return -1;
    calf_sha256_init(&context);
    for(;;) {
        ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if(count < 0) return -1;
        if(count == 0) break;
        calf_sha256_update(&context, buffer, (size_t)count);
    }
    calf_sha256_final(&context, digest);
    for(index = 0; index < sizeof(digest); ++index) {
        hexadecimal[index * 2u] = digits[digest[index] >> 4];
        hexadecimal[index * 2u + 1u] = digits[digest[index] & 15u];
    }
    hexadecimal[64] = '\0';
    return lseek(descriptor, 0, SEEK_SET) == 0 ? 0 : -1;
}

static int firmware_identity_matches(const char *identity_path,
                                     const char digest[65])
{
    static const char prefix[] =
        "CALF-VR180-GEN1 2.2.1\nSHA256 ";
    char identity[128];
    size_t index;
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t identity_length = 0;
    if(update_read_text(identity_path, identity, sizeof(identity)) != 0)
        return 0;
    while(identity[identity_length] != '\0') ++identity_length;
    if(identity_length != prefix_length + 65u) return 0;
    for(index = 0; index < prefix_length; ++index)
        if(identity[index] != prefix[index]) return 0;
    for(index = 0; index < 64; ++index)
        if(identity[prefix_length + index] != digest[index]) return 0;
    return identity[prefix_length + 64u] == '\n' &&
           identity[prefix_length + 65u] == '\0';
}

int firmware_update_validate_paths(const char *archive_path,
                                   const char *identity_path,
                                   int *size_mb, char digest[65])
{
    unsigned char header[512];
    off_t file_size;
    int descriptor;
    int members = 0;
    int found_app = 0;
    unsigned member_mask = 0;
    int result = -1;
    digest[0] = '\0';
    descriptor = open(archive_path, O_RDONLY | O_NOFOLLOW);
    if(descriptor < 0) return -1;
    file_size = lseek(descriptor, 0, SEEK_END);
    if(file_size < (off_t)(1024l * 1024l) ||
       file_size > (off_t)2147483648l ||
       sha256_descriptor(descriptor, digest) != 0)
        goto done;
    while(members < 16) {
        unsigned long stored_checksum;
        unsigned long member_size;
        unsigned long checksum = 0;
        unsigned long padded;
        char name[101];
        int member_index;
        size_t index;
        int zero = 1;
        if(update_read_exact(descriptor, header, sizeof(header)) != 0)
            goto done;
        for(index = 0; index < sizeof(header); ++index)
            if(header[index] != 0) zero = 0;
        if(zero) {
            ssize_t trailing_count;
            while((trailing_count = read(descriptor, header,
                                         sizeof(header))) > 0) {
                for(index = 0; index < (size_t)trailing_count; ++index)
                    if(header[index] != 0) goto done;
            }
            if(trailing_count < 0) goto done;
            result = members > 0 && found_app ? 0 : -1;
            break;
        }
        if(parse_tar_octal(header + 148, 8, &stored_checksum) != 0 ||
           parse_tar_octal(header + 124, 12, &member_size) != 0)
            goto done;
        for(index = 0; index < sizeof(header); ++index)
            checksum += index >= 148 && index < 156
                            ? (unsigned long)' ' : header[index];
        if(checksum != stored_checksum ||
           (header[156] != '\0' && header[156] != '0'))
            goto done;
        for(index = 0; index < 100 && header[index] != '\0'; ++index)
            name[index] = (char)header[index];
        if(index == 0 || index == 100) goto done;
        name[index] = '\0';
        member_index = firmware_member_index(name);
        if(member_index < 0 ||
           (member_mask & (1u << (unsigned)member_index)) != 0)
            goto done;
        member_mask |= 1u << (unsigned)member_index;
        if(update_string_equal(name, "app.img")) found_app = 1;
        padded = (member_size + 511ul) & ~511ul;
        if((off_t)padded < 0 || lseek(descriptor, (off_t)padded, SEEK_CUR) < 0 ||
           lseek(descriptor, 0, SEEK_CUR) > file_size)
            goto done;
        ++members;
    }
done:
    close(descriptor);
    if(result == 0 && !firmware_identity_matches(identity_path, digest))
        result = -1;
    if(result == 0) {
        if(size_mb != (int *)0)
            *size_mb = (int)((file_size + 1048575l) / 1048576l);
    }
    else digest[0] = '\0';
    return result;
}

int firmware_update_validate(int *size_mb, char digest[65])
{
    return firmware_update_validate_paths(FIRMWARE_UPDATE_PATH,
                                          FIRMWARE_UPDATE_IDENTITY_PATH,
                                          size_mb, digest);
}
