#include "ngcd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define USB_GADGET_ROOT "/sys/kernel/config/usb_gadget"
#define USB_GADGET_NAME "calf-net"
#define USB_GADGET_PATH USB_GADGET_ROOT "/" USB_GADGET_NAME
#define MODE_TYPE_MASK 0170000U
#define MODE_DIRECTORY 0040000U
#define MODE_SYMLINK 0120000U

static int make_path(char path[256], const char *suffix)
{
    int count = snprintf(path, 256U, "%s/%s", USB_GADGET_PATH, suffix);
    return count > 0 && count < 256 ? 0 : -1;
}

static int path_directory(const char *path)
{
    struct statx status;
    if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_TYPE, &status) != 0)
        return errno == ENOENT ? 0 : -1;
    return (status.stx_mode & MODE_TYPE_MASK) == MODE_DIRECTORY ? 1 : -1;
}

static int create_directory(const char *path)
{
    if (mkdir(path, 0755) == 0)
        return 0;
    return errno == EEXIST && path_directory(path) == 1 ? 0 : -1;
}

static int write_file(const char *path, const char *value)
{
    size_t length = strlen(value);
    size_t offset = 0U;
    int descriptor = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return -1;
    while (offset < length) {
        ssize_t written = write(descriptor, value + offset, length - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            (void)close(descriptor);
            return -1;
        }
        offset += (size_t)written;
    }
    return close(descriptor) == 0 ? 0 : -1;
}

static int write_attribute(const char *suffix, const char *value)
{
    char path[256];
    return make_path(path, suffix) == 0 ? write_file(path, value) : -1;
}

static void read_usb_string(const char *path, char *output,
                            size_t output_size, const char *fallback)
{
    size_t length = 0U;
    int descriptor;
    ssize_t bytes;
    if (output_size == 0U)
        return;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    bytes = descriptor >= 0 ? read(descriptor, output, output_size - 1U) : -1;
    if (descriptor >= 0)
        (void)close(descriptor);
    if (bytes > 0) {
        size_t index;
        length = (size_t)bytes;
        while (length > 0U && (output[length - 1U] == '\n' ||
                              output[length - 1U] == '\r'))
            --length;
        for (index = 0U; index < length; ++index) {
            unsigned char byte = (unsigned char)output[index];
            if (byte < 0x20U || byte > 0x7eU) {
                length = 0U;
                break;
            }
        }
    }
    if (length == 0U) {
        length = strlen(fallback);
        if (length >= output_size)
            length = output_size - 1U;
        memcpy(output, fallback, length);
    }
    output[length] = '\0';
}

static int remove_link(const char *suffix)
{
    struct statx status;
    char path[256];
    if (make_path(path, suffix) != 0)
        return -1;
    if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_TYPE, &status) != 0)
        return errno == ENOENT ? 0 : -1;
    if ((status.stx_mode & MODE_TYPE_MASK) != MODE_SYMLINK)
        return -1;
    return unlink(path);
}

static int remove_directory(const char *suffix)
{
    char path[256];
    int state;
    if (make_path(path, suffix) != 0)
        return -1;
    state = path_directory(path);
    if (state <= 0)
        return state;
    return rmdir(path);
}

const char *ngcd_usb_ethernet_udc(const char *port)
{
    if (port != NULL && strcasecmp(port, "USB1") == 0)
        return "fc000000.usb";
    if (port != NULL && strcasecmp(port, "USB2") == 0)
        return "fc400000.usb";
    return NULL;
}

const char *ngcd_usb_ethernet_function(const char *operating_system)
{
    if (operating_system != NULL && strcasecmp(operating_system, "win") == 0)
        return "rndis.usb0";
    if (operating_system != NULL && strcasecmp(operating_system, "mac") == 0)
        return "ecm.usb0";
    return NULL;
}

int ngcd_usb_ethernet_close(void)
{
    struct statx status;
    int result = 0;
    if (statx(AT_FDCWD, USB_GADGET_PATH, AT_SYMLINK_NOFOLLOW, STATX_TYPE,
              &status) != 0)
        return errno == ENOENT ? 0 : -1;
    if ((status.stx_mode & MODE_TYPE_MASK) != MODE_DIRECTORY)
        return -1;

    if (write_attribute("UDC", "\n") != 0)
        result = -1;
    if (remove_link("os_desc/c.1") != 0)
        result = -1;
    if (remove_link("configs/c.1/rndis.usb0") != 0)
        result = -1;
    if (remove_link("configs/c.1/ecm.usb0") != 0)
        result = -1;
    if (remove_directory("functions/rndis.usb0") != 0)
        result = -1;
    if (remove_directory("functions/ecm.usb0") != 0)
        result = -1;
    if (remove_directory("configs/c.1/strings/0x409") != 0)
        result = -1;
    if (remove_directory("configs/c.1") != 0)
        result = -1;
    if (remove_directory("strings/0x409") != 0)
        result = -1;
    if (result == 0 && rmdir(USB_GADGET_PATH) != 0)
        result = -1;
    return result;
}

int ngcd_usb_ethernet_set(const char *port, const char *operating_system)
{
    const char *udc = ngcd_usb_ethernet_udc(port);
    const char *function = ngcd_usb_ethernet_function(operating_system);
    char function_suffix[96];
    char function_path[256];
    char config_link[256];
    char serial[128];
    char manufacturer[64];
    char product[128];
    char udc_path[128];
    struct statx status;
    int count;
    int gadget_state;

    if (udc == NULL || function == NULL)
        return -1;
    count = snprintf(udc_path, sizeof(udc_path), "/sys/class/udc/%s", udc);
    if (count <= 0 || (size_t)count >= sizeof(udc_path) ||
        statx(AT_FDCWD, udc_path, 0, STATX_TYPE, &status) != 0 ||
        (status.stx_mode & MODE_TYPE_MASK) != MODE_DIRECTORY)
        return -1;
    gadget_state = path_directory(USB_GADGET_PATH);
    if (gadget_state < 0)
        return -1;
    if (gadget_state == 1 && ngcd_usb_ethernet_close() != 0)
        return -1;

    if (create_directory(USB_GADGET_PATH) != 0 ||
        write_attribute("idVendor", "0x1d6b\n") != 0 ||
        write_attribute("idProduct", "0x0104\n") != 0 ||
        write_attribute("bDeviceClass", "0xef\n") != 0 ||
        write_attribute("bDeviceSubClass", "0x02\n") != 0 ||
        write_attribute("bDeviceProtocol", "0x01\n") != 0)
        goto fail;

    read_usb_string("/param/serial_number", serial, sizeof(serial),
                    "0123456789ABCDEF");
    read_usb_string("/etc/manufacturer", manufacturer, sizeof(manufacturer),
                    "REALIA");
    read_usb_string("/etc/product", product, sizeof(product),
                    "VR180 Camera");
    if (create_directory(USB_GADGET_PATH "/strings/0x409") != 0 ||
        write_attribute("strings/0x409/serialnumber", serial) != 0 ||
        write_attribute("strings/0x409/manufacturer", manufacturer) != 0 ||
        write_attribute("strings/0x409/product", product) != 0 ||
        create_directory(USB_GADGET_PATH "/configs/c.1") != 0 ||
        create_directory(USB_GADGET_PATH "/configs/c.1/strings/0x409") != 0 ||
        write_attribute("configs/c.1/strings/0x409/configuration",
                        "USB Ethernet") != 0 ||
        write_attribute("configs/c.1/MaxPower", "250\n") != 0)
        goto fail;

    count = snprintf(function_suffix, sizeof(function_suffix), "functions/%s",
                     function);
    if (count <= 0 || (size_t)count >= sizeof(function_suffix) ||
        make_path(function_path, function_suffix) != 0 ||
        create_directory(function_path) != 0)
        goto fail;

    if (strcmp(function, "rndis.usb0") == 0 &&
        (write_attribute("functions/rndis.usb0/os_desc/interface.rndis/compatible_id",
                         "RNDIS\n") != 0 ||
         write_attribute("functions/rndis.usb0/os_desc/interface.rndis/sub_compatible_id",
                         "\n") != 0 ||
         write_attribute("os_desc/use", "1\n") != 0 ||
         write_attribute("os_desc/b_vendor_code", "0xcd\n") != 0 ||
         write_attribute("os_desc/qw_sign", "MSFT100\n") != 0))
        goto fail;

    count = snprintf(config_link, sizeof(config_link), "%s/configs/c.1/%s",
                     USB_GADGET_PATH, function);
    if (count <= 0 || (size_t)count >= sizeof(config_link) ||
        symlink(function_path, config_link) != 0)
        goto fail;
    if (strcmp(function, "rndis.usb0") == 0 &&
        symlink(USB_GADGET_PATH "/configs/c.1",
                USB_GADGET_PATH "/os_desc/c.1") != 0)
        goto fail;
    if (write_attribute("UDC", udc) != 0)
        goto fail;
    return 0;

fail:
    (void)ngcd_usb_ethernet_close();
    return -1;
}
