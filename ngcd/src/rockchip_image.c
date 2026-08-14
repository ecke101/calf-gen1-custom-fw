#include "ngcd_rk.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define EXPOSURE_OPERATION_OFFSET 0x01cU
#define EXPOSURE_AUTO_FPS_FIXED_OFFSET 0x118U
#define EXPOSURE_AUTO_FPS_VALUE_OFFSET 0x11cU
#define EXPOSURE_RANGE_ENABLE_OFFSET 0x47cU
#define EXPOSURE_TIME_MIN_OFFSET 0x480U
#define EXPOSURE_TIME_MAX_OFFSET 0x484U
#define EXPOSURE_GAIN_MIN_OFFSET 0x488U
#define EXPOSURE_GAIN_MAX_OFFSET 0x48cU
#define LINEAR_EXPOSURE_COMPENSATION_OFFSET 0x014U
#define EFFECT_VALUE_OFFSET 0x008U
#define QUERY_LINEAR_EXPOSURE_TIME_OFFSET 0x3acU
#define QUERY_SENSOR_EXPOSURE_REGISTER_OFFSET 0x3ccU
#define QUERY_SENSOR_GAIN_REGISTER_OFFSET 0x3d0U
#define QUERY_LINEAR_ANALOG_GAIN_OFFSET 0x3b0U
#define QUERY_LINEAR_DIGITAL_GAIN_OFFSET 0x3b4U
#define QUERY_LINEAR_ISP_GAIN_OFFSET 0x3b8U
#define QUERY_LINEAR_ISO_OFFSET 0x3bcU

/* Preserve the failed AIQ transaction stage and sensor in API diagnostics. */
enum exposure_pair_error {
    EXPOSURE_PAIR_CONTEXT = -10,
    EXPOSURE_PAIR_READ_ORIGINAL = -12,
    EXPOSURE_PAIR_WRITE = -20,
    EXPOSURE_PAIR_READBACK = -22,
    EXPOSURE_PAIR_OPERATION = -30,
    EXPOSURE_PAIR_RANGE = -32,
    EXPOSURE_PAIR_TIME_MIN = -40,
    EXPOSURE_PAIR_TIME_MAX = -42,
    EXPOSURE_PAIR_GAIN_MIN = -50,
    EXPOSURE_PAIR_GAIN_MAX = -52,
};

static float capture_frame_rate(void)
{
    static const char path[] = "/tmp/calf-capture-fps";
    char text[8];
    ssize_t count;
    unsigned int fps = 0U;
    size_t index;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return 30.0f;
    count = read(descriptor, text, sizeof(text));
    (void)close(descriptor);
    if (count <= 0)
        return 30.0f;
    for (index = 0U; index < (size_t)count; ++index) {
        if (text[index] == '\n' || text[index] == '\r')
            break;
        if (text[index] < '0' || text[index] > '9')
            return 30.0f;
        fps = fps * 10U + (unsigned int)(text[index] - '0');
    }
    return fps >= 2U && fps <= 30U ? (float)fps : 30.0f;
}

static void set_capture_frame_rate(unsigned char *attributes, float fps)
{
    attributes[EXPOSURE_AUTO_FPS_FIXED_OFFSET] = 1U;
    memcpy(attributes + EXPOSURE_AUTO_FPS_VALUE_OFFSET, &fps, sizeof(fps));
}

static size_t control_offset(enum ngcd_rk_acp_control control)
{
    switch (control) {
    case NGCD_RK_ACP_BRIGHTNESS:
        return 8U;
    case NGCD_RK_ACP_CONTRAST:
        return 9U;
    case NGCD_RK_ACP_SATURATION:
        return 10U;
    case NGCD_RK_ACP_HUE:
        return 11U;
    }
    return NGCD_RK_AIQ_ACP_ATTR_SIZE;
}

static unsigned char stock_value(int value)
{
    return (unsigned char)(((unsigned int)value * 25U + 1U) / 2U);
}

static int active_sensor_indices(const struct ngcd_rk_graph *graph,
                                 unsigned int *sensor_index, size_t *count)
{
    size_t index;
    *count = 0;
    if (graph->sensor_count == 0 || graph->sensor_count > 2)
        return -1;
    for (index = 0; index < 2U; ++index)
        if ((graph->sensor_mask & (1U << (unsigned int)index)) != 0U)
            sensor_index[(*count)++] = (unsigned int)index;
    return *count == graph->sensor_count ? 0 : -1;
}

static void rollback(const struct ngcd_rk_graph *graph,
                     const struct ngcd_rk_aiq_acp_attr *original,
                     const unsigned int *sensor_index,
                     size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index)
        (void)graph->api->aiq_set_acp(graph->api_context,
                                      graph->sensor_handle[sensor_index[index]],
                                      &original[index]);
}

int ngcd_rk_image_set_acp(struct ngcd_rk_graph *graph,
                          enum ngcd_rk_acp_control control, int value,
                          int *readback)
{
    struct ngcd_rk_aiq_acp_attr original[2];
    struct ngcd_rk_aiq_acp_attr updated;
    struct ngcd_rk_aiq_acp_attr verified;
    unsigned int sensor_index[2];
    size_t offset = control_offset(control);
    size_t count = 0;
    size_t index;
    unsigned char raw;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_get_acp == NULL ||
        graph->api->aiq_set_acp == NULL || readback == NULL ||
        offset >= NGCD_RK_AIQ_ACP_ATTR_SIZE || value < 0 || value > 20)
        return -1;
    if (active_sensor_indices(graph, sensor_index, &count) != 0)
        return -1;
    raw = stock_value(value);
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (graph->sensor_handle[sensor] == NULL ||
            graph->api->aiq_get_acp(graph->api_context,
                                    graph->sensor_handle[sensor],
                                    &original[index]) != 0)
            return -1;
    }
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        memcpy(&updated, &original[index], sizeof(updated));
        updated.bytes[offset] = raw;
        if (graph->api->aiq_set_acp(graph->api_context,
                                    graph->sensor_handle[sensor],
                                    &updated) != 0 ||
            graph->api->aiq_get_acp(graph->api_context,
                                    graph->sensor_handle[sensor],
                                    &verified) != 0 ||
            verified.bytes[offset] != raw) {
            rollback(graph, original, sensor_index, index + 1U);
            return -1;
        }
    }
    *readback = value;
    return 0;
}

static void rollback_sharpness(const struct ngcd_rk_graph *graph,
                               const unsigned int *original,
                               const unsigned int *sensor_index,
                               size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index)
        (void)graph->api->aiq_set_sharpness(
            graph->api_context, graph->sensor_handle[sensor_index[index]],
            original[index]);
}

int ngcd_rk_image_set_sharpness(struct ngcd_rk_graph *graph, int value,
                                int *readback)
{
    unsigned int original[2];
    unsigned int sensor_index[2];
    unsigned int raw;
    unsigned int verified;
    size_t count = 0;
    size_t index;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_get_sharpness == NULL ||
        graph->api->aiq_set_sharpness == NULL || readback == NULL ||
        value < 0 || value > 20 ||
        active_sensor_indices(graph, sensor_index, &count) != 0)
        return -1;
    raw = (unsigned int)value * 5U;
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (graph->sensor_handle[sensor] == NULL ||
            graph->api->aiq_get_sharpness(
                graph->api_context, graph->sensor_handle[sensor],
                &original[index]) != 0)
            return -1;
    }
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (graph->api->aiq_set_sharpness(
                graph->api_context, graph->sensor_handle[sensor], raw) != 0 ||
            graph->api->aiq_get_sharpness(
                graph->api_context, graph->sensor_handle[sensor],
                &verified) != 0 || verified != raw) {
            rollback_sharpness(graph, original, sensor_index, index + 1U);
            return -1;
        }
    }
    *readback = value;
    return 0;
}

struct nr_strengths {
    unsigned int all;
    unsigned int spatial;
    unsigned int temporal;
};

static int get_noise_reduction(const struct ngcd_rk_graph *graph,
                               unsigned int sensor,
                               struct nr_strengths *strengths)
{
    void *context = graph->sensor_handle[sensor];
    return graph->api->aiq_get_anr(graph->api_context, context,
                                   &strengths->all) == 0 &&
           graph->api->aiq_get_spatial_nr(graph->api_context, context,
                                          &strengths->spatial) == 0 &&
           graph->api->aiq_get_temporal_nr(graph->api_context, context,
                                           &strengths->temporal) == 0
               ? 0
               : -1;
}

static int set_noise_reduction(const struct ngcd_rk_graph *graph,
                               unsigned int sensor,
                               const struct nr_strengths *strengths)
{
    void *context = graph->sensor_handle[sensor];
    return graph->api->aiq_set_anr(graph->api_context, context,
                                   strengths->all) == 0 &&
           graph->api->aiq_set_spatial_nr(graph->api_context, context,
                                          strengths->spatial) == 0 &&
           graph->api->aiq_set_temporal_nr(graph->api_context, context,
                                           strengths->temporal) == 0
               ? 0
               : -1;
}

static void rollback_noise_reduction(const struct ngcd_rk_graph *graph,
                                     const struct nr_strengths *original,
                                     const unsigned int *sensor_index,
                                     size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index)
        (void)set_noise_reduction(graph, sensor_index[index], &original[index]);
}

int ngcd_rk_image_set_noise_reduction(struct ngcd_rk_graph *graph, int value,
                                      int *readback)
{
    struct nr_strengths original[2];
    struct nr_strengths requested;
    struct nr_strengths verified;
    unsigned int sensor_index[2];
    unsigned int raw;
    size_t count = 0;
    size_t index;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_get_anr == NULL || graph->api->aiq_set_anr == NULL ||
        graph->api->aiq_get_spatial_nr == NULL ||
        graph->api->aiq_set_spatial_nr == NULL ||
        graph->api->aiq_get_temporal_nr == NULL ||
        graph->api->aiq_set_temporal_nr == NULL || readback == NULL ||
        value < 0 || value > 20 ||
        active_sensor_indices(graph, sensor_index, &count) != 0)
        return -1;
    raw = (unsigned int)value * 5U;
    requested.all = raw;
    requested.spatial = raw;
    requested.temporal = raw;
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (graph->sensor_handle[sensor] == NULL ||
            get_noise_reduction(graph, sensor, &original[index]) != 0)
            return -1;
    }
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (set_noise_reduction(graph, sensor, &requested) != 0 ||
            get_noise_reduction(graph, sensor, &verified) != 0 ||
            verified.all != raw || verified.spatial != raw ||
            verified.temporal != raw) {
            rollback_noise_reduction(graph, original, sensor_index,
                                     index + 1U);
            return -1;
        }
    }
    *readback = value;
    return 0;
}

static void store_u32(unsigned char *bytes, size_t offset, unsigned int value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static unsigned int load_u32(const unsigned char *bytes, size_t offset)
{
    unsigned int value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static void store_f32(unsigned char *bytes, size_t offset, float value)
{
    memcpy(bytes + offset, &value, sizeof(value));
}

static float load_f32(const unsigned char *bytes, size_t offset)
{
    float value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static bool exposure_range_matches(float actual, float requested)
{
    float difference = actual - requested;
    /* With anti-flicker enabled, AIQ can quantize a requested manual ceiling
     * to the preceding mains-aligned exposure before returning the range. At
     * 4 fps in a 50 Hz environment, 250 ms therefore reads back as 240 ms.
     * Keep this bounded to 10%; a stale range from the preceding 8/15-fps
     * preview remains far outside the accepted window. */
    float tolerance = requested * 0.10f + 0.000001f;
    if (difference < 0.0f)
        difference = -difference;
    return difference <= tolerance;
}

static void rollback_exposure(
    const struct ngcd_rk_graph *graph,
    const struct ngcd_rk_aiq_exp_sw_attr *original,
    const unsigned int *sensor_index, size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index)
        (void)graph->api->aiq_set_exposure(
            graph->api_context, graph->sensor_handle[sensor_index[index]],
            &original[index]);
}

int ngcd_rk_image_set_iso(struct ngcd_rk_graph *graph, unsigned int iso,
                          unsigned int *readback)
{
    struct ngcd_rk_aiq_exp_sw_attr original[2];
    struct ngcd_rk_aiq_exp_sw_attr updated;
    struct ngcd_rk_aiq_exp_sw_attr verified;
    unsigned int sensor_index[2];
    float gain_min;
    float gain_max;
    float frame_rate = capture_frame_rate();
    unsigned int gain;
    size_t count = 0;
    size_t index;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_get_exposure == NULL ||
        graph->api->aiq_set_exposure == NULL || readback == NULL ||
        (iso != 0U && iso % 100U != 0U) ||
        active_sensor_indices(graph, sensor_index, &count) != 0)
        return -1;
    gain = iso / 100U;
    if (iso != 0U &&
        (gain < 1U || gain > 128U || (gain & (gain - 1U)) != 0U))
        return -1;
    if (iso == 0U) {
        gain_min = 1.0f;
        gain_max = 128.0f;
    } else {
        gain_min = (float)gain;
        gain_max = gain_min;
    }
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (graph->sensor_handle[sensor] == NULL ||
            graph->api->aiq_get_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &original[index]) != 0)
            return -1;
    }
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        memcpy(&updated, &original[index], sizeof(updated));
        store_u32(updated.bytes, EXPOSURE_OPERATION_OFFSET, 1U);
        set_capture_frame_rate(updated.bytes, frame_rate);
        updated.bytes[EXPOSURE_RANGE_ENABLE_OFFSET] = 1U;
        store_f32(updated.bytes, EXPOSURE_GAIN_MIN_OFFSET, gain_min);
        store_f32(updated.bytes, EXPOSURE_GAIN_MAX_OFFSET, gain_max);
        if (graph->api->aiq_set_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &updated) != 0 ||
            graph->api->aiq_get_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &verified) != 0 ||
            load_u32(verified.bytes, EXPOSURE_OPERATION_OFFSET) != 1U ||
            verified.bytes[EXPOSURE_RANGE_ENABLE_OFFSET] != 1U ||
            load_f32(verified.bytes, EXPOSURE_GAIN_MIN_OFFSET) != gain_min ||
            load_f32(verified.bytes, EXPOSURE_GAIN_MAX_OFFSET) != gain_max) {
            rollback_exposure(graph, original, sensor_index, index + 1U);
            return -1;
        }
    }
    *readback = iso;
    return 0;
}

int ngcd_rk_image_set_exposure(struct ngcd_rk_graph *graph, float seconds,
                               bool automatic, float *readback)
{
    struct ngcd_rk_aiq_exp_sw_attr original[2];
    struct ngcd_rk_aiq_exp_sw_attr updated;
    struct ngcd_rk_aiq_exp_sw_attr verified;
    unsigned int sensor_index[2];
    float requested = automatic ? 0.0f : seconds;
    float frame_rate = capture_frame_rate();
    size_t count = 0;
    size_t index;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_get_exposure == NULL ||
        graph->api->aiq_set_exposure == NULL || readback == NULL ||
        (!automatic && (!(seconds > 0.0f) || seconds > 12.0f)) ||
        active_sensor_indices(graph, sensor_index, &count) != 0)
        return -1;
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (graph->sensor_handle[sensor] == NULL ||
            graph->api->aiq_get_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &original[index]) != 0)
            return -1;
    }
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        memcpy(&updated, &original[index], sizeof(updated));
        store_u32(updated.bytes, EXPOSURE_OPERATION_OFFSET, 1U);
        set_capture_frame_rate(updated.bytes, frame_rate);
        updated.bytes[EXPOSURE_RANGE_ENABLE_OFFSET] = 1U;
        store_f32(updated.bytes, EXPOSURE_TIME_MIN_OFFSET, requested);
        store_f32(updated.bytes, EXPOSURE_TIME_MAX_OFFSET, requested);
        if (graph->api->aiq_set_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &updated) != 0 ||
            graph->api->aiq_get_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &verified) != 0 ||
            load_u32(verified.bytes, EXPOSURE_OPERATION_OFFSET) != 1U ||
            verified.bytes[EXPOSURE_RANGE_ENABLE_OFFSET] != 1U ||
            load_f32(verified.bytes, EXPOSURE_TIME_MIN_OFFSET) != requested ||
            load_f32(verified.bytes, EXPOSURE_TIME_MAX_OFFSET) != requested) {
            rollback_exposure(graph, original, sensor_index, index + 1U);
            return -1;
        }
    }
    *readback = automatic ? -1.0f : requested;
    return 0;
}

int ngcd_rk_image_set_exposure_iso(struct ngcd_rk_graph *graph,
                                   float seconds, bool exposure_automatic,
                                   unsigned int iso,
                                   float *exposure_readback,
                                   unsigned int *iso_readback)
{
    struct ngcd_rk_aiq_exp_sw_attr original[2];
    struct ngcd_rk_aiq_exp_sw_attr updated;
    struct ngcd_rk_aiq_exp_sw_attr verified;
    unsigned int sensor_index[2];
    unsigned int gain = iso / 100U;
    float requested_time = exposure_automatic ? 0.0f : seconds;
    float frame_rate = capture_frame_rate();
    float gain_min;
    float gain_max;
    size_t count = 0U;
    size_t index;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_get_exposure == NULL ||
        graph->api->aiq_set_exposure == NULL ||
        exposure_readback == NULL || iso_readback == NULL ||
        (!exposure_automatic &&
         (!(seconds > 0.0f) || seconds > 12.0f)) ||
        (iso != 0U && (iso % 100U != 0U || gain < 1U || gain > 128U ||
                      (gain & (gain - 1U)) != 0U)) ||
        active_sensor_indices(graph, sensor_index, &count) != 0)
        return -1;
    if (iso == 0U) {
        gain_min = 1.0f;
        gain_max = 128.0f;
    } else {
        gain_min = (float)gain;
        gain_max = gain_min;
    }
    for (index = 0U; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (graph->sensor_handle[sensor] == NULL)
            return EXPOSURE_PAIR_CONTEXT - (int)sensor;
        if (graph->api->aiq_get_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &original[index]) != 0)
            return EXPOSURE_PAIR_READ_ORIGINAL - (int)sensor;
    }
    for (index = 0U; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        int result = 0;
        memcpy(&updated, &original[index], sizeof(updated));
        store_u32(updated.bytes, EXPOSURE_OPERATION_OFFSET, 1U);
        set_capture_frame_rate(updated.bytes, frame_rate);
        updated.bytes[EXPOSURE_RANGE_ENABLE_OFFSET] = 1U;
        store_f32(updated.bytes, EXPOSURE_TIME_MIN_OFFSET, requested_time);
        store_f32(updated.bytes, EXPOSURE_TIME_MAX_OFFSET, requested_time);
        store_f32(updated.bytes, EXPOSURE_GAIN_MIN_OFFSET, gain_min);
        store_f32(updated.bytes, EXPOSURE_GAIN_MAX_OFFSET, gain_max);
        if (graph->api->aiq_set_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &updated) != 0)
            result = EXPOSURE_PAIR_WRITE - (int)sensor;
        else if (graph->api->aiq_get_exposure(
                     graph->api_context, graph->sensor_handle[sensor],
                     &verified) != 0)
            result = EXPOSURE_PAIR_READBACK - (int)sensor;
        else if (load_u32(verified.bytes, EXPOSURE_OPERATION_OFFSET) != 1U)
            result = EXPOSURE_PAIR_OPERATION - (int)sensor;
        else if (verified.bytes[EXPOSURE_RANGE_ENABLE_OFFSET] != 1U)
            result = EXPOSURE_PAIR_RANGE - (int)sensor;
        else if (!exposure_range_matches(
                     load_f32(verified.bytes, EXPOSURE_TIME_MIN_OFFSET),
                     requested_time))
            result = EXPOSURE_PAIR_TIME_MIN - (int)sensor;
        else if (!exposure_range_matches(
                     load_f32(verified.bytes, EXPOSURE_TIME_MAX_OFFSET),
                     requested_time))
            result = EXPOSURE_PAIR_TIME_MAX - (int)sensor;
        else if (load_f32(verified.bytes, EXPOSURE_GAIN_MIN_OFFSET) !=
                 gain_min)
            result = EXPOSURE_PAIR_GAIN_MIN - (int)sensor;
        else if (load_f32(verified.bytes, EXPOSURE_GAIN_MAX_OFFSET) !=
                 gain_max)
            result = EXPOSURE_PAIR_GAIN_MAX - (int)sensor;
        if (result != 0) {
            rollback_exposure(graph, original, sensor_index, index + 1U);
            return result;
        }
    }
    *exposure_readback = exposure_automatic ? -1.0f : requested_time;
    *iso_readback = iso;
    return 0;
}

int ngcd_rk_image_query_exposure(const struct ngcd_rk_graph *graph,
                                 float *seconds, unsigned int *iso)
{
    struct ngcd_rk_aiq_exp_query_info information;
    unsigned int sensor_index[2];
    int raw_iso;
    size_t count = 0;
    float analog_gain;
    float digital_gain;
    float isp_gain;
    float raw_seconds;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_query_exposure == NULL || seconds == NULL ||
        iso == NULL ||
        active_sensor_indices(graph, sensor_index, &count) != 0 ||
        count == 0U || graph->sensor_handle[sensor_index[0]] == NULL)
        return -1;
    memset(&information, 0, sizeof(information));
    if (graph->api->aiq_query_exposure(
            graph->api_context, graph->sensor_handle[sensor_index[0]],
            &information) != 0)
        return -1;
    memcpy(&raw_seconds,
           information.bytes + QUERY_LINEAR_EXPOSURE_TIME_OFFSET,
           sizeof(raw_seconds));
    memcpy(&raw_iso, information.bytes + QUERY_LINEAR_ISO_OFFSET,
           sizeof(raw_iso));
    if (!(raw_seconds > 0.0f) || raw_seconds > 3600.0f)
        return -1;
    if (raw_iso <= 0) {
        float derived_iso;
        memcpy(&analog_gain,
               information.bytes + QUERY_LINEAR_ANALOG_GAIN_OFFSET,
               sizeof(analog_gain));
        memcpy(&digital_gain,
               information.bytes + QUERY_LINEAR_DIGITAL_GAIN_OFFSET,
               sizeof(digital_gain));
        memcpy(&isp_gain,
               information.bytes + QUERY_LINEAR_ISP_GAIN_OFFSET,
               sizeof(isp_gain));
        derived_iso = 100.0f * analog_gain * digital_gain * isp_gain;
        if (!(derived_iso >= 1.0f) || derived_iso > 65535.0f)
            return -1;
        raw_iso = (int)(derived_iso + 0.5f);
    }
    if (raw_iso > 65535)
        return -1;
    *seconds = raw_seconds;
    *iso = (unsigned int)raw_iso;
    return 0;
}

int ngcd_rk_image_query_sensor_registers(
    const struct ngcd_rk_graph *graph, uint32_t exposure_register[2],
    uint32_t gain_register[2])
{
    struct ngcd_rk_aiq_exp_query_info information;
    unsigned int index;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_query_exposure == NULL ||
        graph->sensor_count != 2U || exposure_register == NULL ||
        gain_register == NULL)
        return -1;
    for (index = 0U; index < 2U; ++index) {
        memset(&information, 0, sizeof(information));
        if (graph->sensor_handle[index] == NULL ||
            graph->api->aiq_query_exposure(
                graph->api_context, graph->sensor_handle[index],
                &information) != 0)
            return -1;
        memcpy(&exposure_register[index],
               information.bytes + QUERY_SENSOR_EXPOSURE_REGISTER_OFFSET,
               sizeof(exposure_register[index]));
        memcpy(&gain_register[index],
               information.bytes + QUERY_SENSOR_GAIN_REGISTER_OFFSET,
               sizeof(gain_register[index]));
        if (exposure_register[index] == 0U || gain_register[index] == 0U)
            return -1;
    }
    return 0;
}

static void rollback_linear_exposure(
    const struct ngcd_rk_graph *graph,
    const struct ngcd_rk_aiq_lin_exp_attr *original,
    const unsigned int *sensor_index, size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index)
        (void)graph->api->aiq_set_linear_exposure(
            graph->api_context, graph->sensor_handle[sensor_index[index]],
            &original[index]);
}

int ngcd_rk_image_set_exposure_compensation(struct ngcd_rk_graph *graph,
                                             int value, int *readback)
{
    static const float stock_compensation[] = {
        -100.0f, -58.4900016784668f, -25.889999389648438f, 0.0f,
        25.889999389648438f, 58.4900016784668f, 100.0f,
    };
    struct ngcd_rk_aiq_lin_exp_attr original[2];
    struct ngcd_rk_aiq_lin_exp_attr updated;
    struct ngcd_rk_aiq_lin_exp_attr verified;
    unsigned int sensor_index[2];
    float requested;
    size_t count = 0;
    size_t index;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_get_linear_exposure == NULL ||
        graph->api->aiq_set_linear_exposure == NULL || readback == NULL ||
        value < -3 || value > 3 ||
        active_sensor_indices(graph, sensor_index, &count) != 0)
        return -1;
    requested = stock_compensation[value + 3];
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (graph->sensor_handle[sensor] == NULL ||
            graph->api->aiq_get_linear_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &original[index]) != 0)
            return -1;
    }
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        memcpy(&updated, &original[index], sizeof(updated));
        store_f32(updated.bytes, LINEAR_EXPOSURE_COMPENSATION_OFFSET,
                  requested);
        if (graph->api->aiq_set_linear_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &updated) != 0 ||
            graph->api->aiq_get_linear_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &verified) != 0 ||
            load_f32(verified.bytes,
                     LINEAR_EXPOSURE_COMPENSATION_OFFSET) != requested) {
            rollback_linear_exposure(graph, original, sensor_index,
                                     index + 1U);
            return -1;
        }
    }
    *readback = value;
    return 0;
}

struct white_balance_state {
    unsigned int mode;
    unsigned int kelvin;
};

static int get_white_balance(const struct ngcd_rk_graph *graph,
                             unsigned int sensor,
                             struct white_balance_state *state)
{
    void *context = graph->sensor_handle[sensor];
    if (graph->api->aiq_get_white_balance_mode(
            graph->api_context, context, &state->mode) != 0 ||
        state->mode > 1U)
        return -1;
    state->kelvin = 0U;
    if (state->mode != 0U &&
        graph->api->aiq_get_white_balance_ct(
            graph->api_context, context, &state->kelvin) != 0)
        return -1;
    return 0;
}

static int set_white_balance(const struct ngcd_rk_graph *graph,
                             unsigned int sensor,
                             const struct white_balance_state *state)
{
    void *context = graph->sensor_handle[sensor];
    if (state->mode == 0U)
        return graph->api->aiq_set_white_balance_mode(
            graph->api_context, context, 0U);
    return graph->api->aiq_set_white_balance_ct(
        graph->api_context, context, state->kelvin);
}

static void rollback_white_balance(
    const struct ngcd_rk_graph *graph,
    const struct white_balance_state *original,
    const unsigned int *sensor_index, size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index)
        (void)set_white_balance(graph, sensor_index[index], &original[index]);
}

int ngcd_rk_image_set_white_balance(struct ngcd_rk_graph *graph,
                                    unsigned int kelvin,
                                    unsigned int *readback)
{
    struct white_balance_state original[2];
    struct white_balance_state requested;
    struct white_balance_state verified;
    unsigned int sensor_index[2];
    size_t count = 0;
    size_t index;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_get_white_balance_mode == NULL ||
        graph->api->aiq_set_white_balance_mode == NULL ||
        graph->api->aiq_get_white_balance_ct == NULL ||
        graph->api->aiq_set_white_balance_ct == NULL || readback == NULL ||
        (kelvin != 0U && (kelvin < 1000U || kelvin > 10000U)) ||
        active_sensor_indices(graph, sensor_index, &count) != 0)
        return -1;
    requested.mode = kelvin == 0U ? 0U : 1U;
    requested.kelvin = kelvin;
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (graph->sensor_handle[sensor] == NULL ||
            get_white_balance(graph, sensor, &original[index]) != 0)
            return -1;
    }
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (set_white_balance(graph, sensor, &requested) != 0 ||
            get_white_balance(graph, sensor, &verified) != 0 ||
            verified.mode != requested.mode ||
            (requested.mode != 0U && verified.kelvin != kelvin)) {
            rollback_white_balance(graph, original, sensor_index,
                                   index + 1U);
            return -1;
        }
    }
    *readback = kelvin;
    return 0;
}

struct flicker_state {
    unsigned char enabled;
    unsigned int mode;
    unsigned int frequency;
};

static int get_flicker(const struct ngcd_rk_graph *graph,
                       unsigned int sensor, struct flicker_state *state)
{
    void *context = graph->sensor_handle[sensor];
    if (graph->api->aiq_get_flicker_enabled(
            graph->api_context, context, &state->enabled) != 0 ||
        graph->api->aiq_get_flicker_mode(
            graph->api_context, context, &state->mode) != 0 ||
        graph->api->aiq_get_power_line_frequency(
            graph->api_context, context, &state->frequency) != 0 ||
        state->enabled > 1U || state->mode > 1U || state->frequency > 2U)
        return -1;
    return 0;
}

static int set_flicker(const struct ngcd_rk_graph *graph,
                       unsigned int sensor, const struct flicker_state *state)
{
    void *context = graph->sensor_handle[sensor];
    if (graph->api->aiq_set_flicker_enabled(
            graph->api_context, context, 0U) != 0 ||
        graph->api->aiq_set_flicker_mode(
            graph->api_context, context, state->mode) != 0 ||
        graph->api->aiq_set_power_line_frequency(
            graph->api_context, context, state->frequency) != 0)
        return -1;
    return state->enabled == 0U
               ? 0
               : graph->api->aiq_set_flicker_enabled(
                     graph->api_context, context, 1U);
}

static int apply_flicker_control(const struct ngcd_rk_graph *graph,
                                 unsigned int sensor,
                                 enum ngcd_rk_flicker_control control,
                                 const struct flicker_state *current)
{
    void *context = graph->sensor_handle[sensor];
    if (control == NGCD_RK_FLICKER_OFF)
        return current->enabled == 0U
                   ? 0
                   : graph->api->aiq_set_flicker_enabled(
                         graph->api_context, context, 0U) == 0 ? 0 : -2;
    if (graph->api->aiq_set_flicker_mode(
            graph->api_context, context,
            control == NGCD_RK_FLICKER_AUTO ? 1U : 0U) != 0)
        return -3;
    if (control != NGCD_RK_FLICKER_AUTO &&
        graph->api->aiq_set_power_line_frequency(
            graph->api_context, context,
            control == NGCD_RK_FLICKER_50HZ ? 1U : 2U) != 0)
        return -4;
    return current->enabled == 1U
               ? 0
               : graph->api->aiq_set_flicker_enabled(
                     graph->api_context, context, 1U) == 0 ? 0 : -5;
}

static void rollback_flicker(const struct ngcd_rk_graph *graph,
                             const struct flicker_state *original,
                             const unsigned int *sensor_index, size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index)
        (void)set_flicker(graph, sensor_index[index], &original[index]);
}

static int flicker_matches(const struct flicker_state *state,
                           enum ngcd_rk_flicker_control control)
{
    if (control == NGCD_RK_FLICKER_OFF)
        return state->enabled == 0U;
    if (state->enabled != 1U)
        return 0;
    if (control == NGCD_RK_FLICKER_AUTO)
        return state->mode == 1U;
    return state->mode == 0U &&
           state->frequency ==
               (control == NGCD_RK_FLICKER_50HZ ? 1U : 2U);
}

int ngcd_rk_image_set_flicker(struct ngcd_rk_graph *graph,
                              enum ngcd_rk_flicker_control control,
                              enum ngcd_rk_flicker_control *readback)
{
    struct flicker_state original[2];
    struct flicker_state verified;
    unsigned int sensor_index[2];
    size_t count = 0;
    size_t index;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_get_flicker_enabled == NULL ||
        graph->api->aiq_set_flicker_enabled == NULL ||
        graph->api->aiq_get_flicker_mode == NULL ||
        graph->api->aiq_set_flicker_mode == NULL ||
        graph->api->aiq_get_power_line_frequency == NULL ||
        graph->api->aiq_set_power_line_frequency == NULL ||
        readback == NULL || control < NGCD_RK_FLICKER_OFF ||
        control > NGCD_RK_FLICKER_60HZ ||
        active_sensor_indices(graph, sensor_index, &count) != 0)
        return -1;
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (graph->sensor_handle[sensor] == NULL ||
            get_flicker(graph, sensor, &original[index]) != 0)
            return -1;
    }
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        int apply_result;
        if (flicker_matches(&original[index], control))
            continue;
        apply_result = apply_flicker_control(graph, sensor, control,
                                             &original[index]);
        if (apply_result != 0) {
            rollback_flicker(graph, original, sensor_index, index + 1U);
            return apply_result;
        }
        if (get_flicker(graph, sensor, &verified) != 0) {
            rollback_flicker(graph, original, sensor_index, index + 1U);
            return -6;
        }
        if (!flicker_matches(&verified, control)) {
            rollback_flicker(graph, original, sensor_index, index + 1U);
            return -7;
        }
    }
    *readback = control;
    return 0;
}

static void rollback_effect(
    const struct ngcd_rk_graph *graph,
    const struct ngcd_rk_aiq_effect_attr *original,
    const unsigned int *sensor_index, size_t count)
{
    size_t index;
    for (index = 0; index < count; ++index)
        (void)graph->api->aiq_set_effect(
            graph->api_context, graph->sensor_handle[sensor_index[index]],
            &original[index]);
}

int ngcd_rk_image_set_effect(struct ngcd_rk_graph *graph,
                             unsigned int effect, unsigned int *readback)
{
    struct ngcd_rk_aiq_effect_attr original[2];
    struct ngcd_rk_aiq_effect_attr updated;
    struct ngcd_rk_aiq_effect_attr verified;
    unsigned int sensor_index[2];
    size_t count = 0;
    size_t index;
    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_get_effect == NULL ||
        graph->api->aiq_set_effect == NULL || readback == NULL || effect > 5U ||
        active_sensor_indices(graph, sensor_index, &count) != 0)
        return -1;
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        if (graph->sensor_handle[sensor] == NULL ||
            graph->api->aiq_get_effect(
                graph->api_context, graph->sensor_handle[sensor],
                &original[index]) != 0)
            return -1;
    }
    for (index = 0; index < count; ++index) {
        unsigned int sensor = sensor_index[index];
        memcpy(&updated, &original[index], sizeof(updated));
        store_u32(updated.bytes, EFFECT_VALUE_OFFSET, effect);
        if (graph->api->aiq_set_effect(
                graph->api_context, graph->sensor_handle[sensor],
                &updated) != 0 ||
            graph->api->aiq_get_effect(
                graph->api_context, graph->sensor_handle[sensor],
                &verified) != 0 ||
            load_u32(verified.bytes, EFFECT_VALUE_OFFSET) != effect) {
            rollback_effect(graph, original, sensor_index, index + 1U);
            return -1;
        }
    }
    *readback = effect;
    return 0;
}

static int nearest_level(unsigned int raw, unsigned int scale,
                         int maximum, int *level)
{
    unsigned int rounded;
    if (scale == 0U || raw > 255U || level == NULL)
        return -1;
    rounded = (raw + scale / 2U) / scale;
    if (rounded > (unsigned int)maximum)
        rounded = (unsigned int)maximum;
    *level = (int)rounded;
    return 0;
}

static int nearest_acp_level(unsigned int raw, int *level)
{
    unsigned int best_difference = UINT32_MAX;
    int best = 0;
    int candidate;
    if (raw > 255U || level == NULL)
        return -1;
    for (candidate = 0; candidate <= 20; ++candidate) {
        unsigned int expected = stock_value(candidate);
        unsigned int difference = raw > expected ? raw - expected
                                                  : expected - raw;
        if (difference < best_difference) {
            best_difference = difference;
            best = candidate;
        }
    }
    *level = best;
    return 0;
}

static int decode_exposure(
    const struct ngcd_rk_aiq_exp_sw_attr *attribute,
    struct ngcd_rk_image_readback *readback)
{
    float time_min = load_f32(attribute->bytes, EXPOSURE_TIME_MIN_OFFSET);
    float time_max = load_f32(attribute->bytes, EXPOSURE_TIME_MAX_OFFSET);
    float gain_min = load_f32(attribute->bytes, EXPOSURE_GAIN_MIN_OFFSET);
    float gain_max = load_f32(attribute->bytes, EXPOSURE_GAIN_MAX_OFFSET);
    unsigned int gain;
    if (time_min == 0.0f && time_max == 0.0f) {
        readback->exposure_automatic = true;
        readback->exposure_seconds = 0.0f;
    } else if (time_min == time_max && time_min > 0.0f &&
               time_min <= 12.0f) {
        readback->exposure_automatic = false;
        readback->exposure_seconds = time_min;
    } else {
        return -1;
    }
    if (gain_min == 1.0f && gain_max == 128.0f) {
        readback->iso_automatic = true;
        readback->iso = 0U;
        return 0;
    }
    if (gain_min != gain_max || gain_min < 1.0f || gain_min > 128.0f)
        return -1;
    gain = (unsigned int)gain_min;
    if ((float)gain != gain_min || (gain & (gain - 1U)) != 0U)
        return -1;
    readback->iso_automatic = false;
    readback->iso = gain * 100U;
    return 0;
}

static int decode_compensation(
    const struct ngcd_rk_aiq_lin_exp_attr *attribute, int *value)
{
    static const float stock[] = {
        -100.0f, -58.4900016784668f, -25.889999389648438f, 0.0f,
        25.889999389648438f, 58.4900016784668f, 100.0f,
    };
    float raw = load_f32(attribute->bytes,
                         LINEAR_EXPOSURE_COMPENSATION_OFFSET);
    size_t index;
    for (index = 0U; index < sizeof(stock) / sizeof(stock[0]); ++index) {
        float difference = raw - stock[index];
        if (difference < 0.0f)
            difference = -difference;
        if (difference < 0.01f) {
            *value = (int)index - 3;
            return 0;
        }
    }
    return -1;
}

int ngcd_rk_image_read(const struct ngcd_rk_graph *graph,
                       struct ngcd_rk_image_readback *readback)
{
    struct ngcd_rk_image_readback first;
    unsigned int sensor_index[2];
    size_t count = 0U;
    size_t index;
    if (graph == NULL || graph->api == NULL || readback == NULL ||
        graph->api->aiq_get_acp == NULL ||
        graph->api->aiq_get_sharpness == NULL ||
        graph->api->aiq_get_anr == NULL ||
        graph->api->aiq_get_spatial_nr == NULL ||
        graph->api->aiq_get_temporal_nr == NULL ||
        graph->api->aiq_get_exposure == NULL ||
        graph->api->aiq_get_linear_exposure == NULL ||
        graph->api->aiq_get_white_balance_mode == NULL ||
        graph->api->aiq_get_white_balance_ct == NULL ||
        graph->api->aiq_get_flicker_enabled == NULL ||
        graph->api->aiq_get_flicker_mode == NULL ||
        graph->api->aiq_get_power_line_frequency == NULL ||
        graph->api->aiq_get_effect == NULL ||
        active_sensor_indices(graph, sensor_index, &count) != 0)
        return -1;
    memset(&first, 0, sizeof(first));
    for (index = 0U; index < count; ++index) {
        struct ngcd_rk_image_readback current;
        struct ngcd_rk_aiq_acp_attr acp;
        struct ngcd_rk_aiq_exp_sw_attr exposure;
        struct ngcd_rk_aiq_lin_exp_attr linear;
        struct ngcd_rk_aiq_effect_attr effect;
        struct nr_strengths noise;
        struct white_balance_state white_balance;
        struct flicker_state flicker;
        unsigned int sharpness;
        unsigned int sensor = sensor_index[index];
        memset(&current, 0, sizeof(current));
        if (graph->sensor_handle[sensor] == NULL ||
            graph->api->aiq_get_acp(
                graph->api_context, graph->sensor_handle[sensor], &acp) != 0 ||
            graph->api->aiq_get_sharpness(
                graph->api_context, graph->sensor_handle[sensor],
                &sharpness) != 0 ||
            get_noise_reduction(graph, sensor, &noise) != 0 ||
            graph->api->aiq_get_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &exposure) != 0 ||
            graph->api->aiq_get_linear_exposure(
                graph->api_context, graph->sensor_handle[sensor],
                &linear) != 0 ||
            get_white_balance(graph, sensor, &white_balance) != 0 ||
            get_flicker(graph, sensor, &flicker) != 0 ||
            graph->api->aiq_get_effect(
                graph->api_context, graph->sensor_handle[sensor],
                &effect) != 0 ||
            decode_exposure(&exposure, &current) != 0 ||
            decode_compensation(&linear,
                                &current.exposure_compensation) != 0 ||
            nearest_acp_level(
                acp.bytes[control_offset(NGCD_RK_ACP_BRIGHTNESS)],
                &current.brightness) != 0 ||
            nearest_acp_level(
                acp.bytes[control_offset(NGCD_RK_ACP_CONTRAST)],
                &current.contrast) != 0 ||
            nearest_acp_level(
                acp.bytes[control_offset(NGCD_RK_ACP_SATURATION)],
                &current.saturation) != 0 ||
            nearest_acp_level(acp.bytes[control_offset(NGCD_RK_ACP_HUE)],
                              &current.hue) != 0 ||
            nearest_level(sharpness, 5U, 20, &current.sharpness) != 0 ||
            noise.all != noise.spatial || noise.all != noise.temporal ||
            nearest_level(noise.all, 5U, 20,
                          &current.noise_reduction) != 0)
            return -1;
        current.white_balance_automatic = white_balance.mode == 0U;
        current.white_balance_kelvin = white_balance.kelvin;
        if (flicker.enabled == 0U)
            current.flicker = NGCD_RK_FLICKER_OFF;
        else if (flicker.mode == 1U)
            current.flicker = NGCD_RK_FLICKER_AUTO;
        else if (flicker.frequency == 1U)
            current.flicker = NGCD_RK_FLICKER_50HZ;
        else if (flicker.frequency == 2U)
            current.flicker = NGCD_RK_FLICKER_60HZ;
        else
            return -1;
        current.effect = load_u32(effect.bytes, EFFECT_VALUE_OFFSET);
        if ((!current.white_balance_automatic &&
             (current.white_balance_kelvin < 1000U ||
              current.white_balance_kelvin > 10000U)) ||
            current.effect > 5U)
            return -1;
        if (index == 0U)
            first = current;
        else if (memcmp(&first, &current, sizeof(first)) != 0)
            return -1;
    }
    *readback = first;
    return 0;
}
