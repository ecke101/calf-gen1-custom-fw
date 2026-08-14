#include "ngcd_rk.h"
#include "ngcd_mp4.h"

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    RK_MODULE_VENC = 4,
    RK_MODULE_VPSS = 6,
    RK_MODULE_VI = 8,
    RK_MODULE_VO = 9,
    RK_MODULE_AVS = 17,
    RK_AI_DEVICE = 0,
    RK_AI_CHANNEL = 0,
    RK_AUDIO_SAMPLE_RATE = 48000,
    RK_AUDIO_CHANNELS = 2,
    RK_VO_DEVICE_LCD = 3,
    RK_VO_VIDEO_LAYER = 3,
    RK_SNAPSHOT_CHANNEL = 4,
    VENC_VALIDATION_POLLS = 750,
    RK_FRAME_INFO_SIZE = 160,
};

#define RK_VI_NOT_CONFIG UINT32_C(0xa0088007)

static int drain_audio(struct ngcd_rk_graph *graph, bool write_sample,
                       unsigned int limit);
static int drain_record_stream(struct ngcd_rk_graph *graph,
                               unsigned int limit);

struct raw_capture_start {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned int ready;
    bool released;
};

struct raw_capture_request {
    struct raw_capture_start *start;
    const struct ngcd_rk_api *api;
    void *api_context;
    void *sensor_context;
    int count;
    const char *capture_directory;
    char *output_directory;
    int result;
};

struct offline_enqueue_request {
    struct raw_capture_start *start;
    const struct ngcd_rk_api *api;
    void *api_context;
    void *sensor_context;
    void *raw_data;
    int result;
};

struct offline_start_request {
    struct raw_capture_start *start;
    const struct ngcd_rk_api *api;
    void *api_context;
    void *sensor_context;
    int result;
};

static void *capture_raw_thread(void *opaque)
{
    struct raw_capture_request *request = opaque;
    pthread_mutex_lock(&request->start->mutex);
    ++request->start->ready;
    if (request->start->ready == 2U)
        pthread_cond_broadcast(&request->start->condition);
    while (!request->start->released)
        pthread_cond_wait(&request->start->condition,
                          &request->start->mutex);
    pthread_mutex_unlock(&request->start->mutex);
    request->result = request->api->aiq_capture_raw(
        request->api_context, request->sensor_context, request->count,
        request->capture_directory, request->output_directory);
    return NULL;
}

static void *offline_enqueue_thread(void *opaque)
{
    struct offline_enqueue_request *request = opaque;
    pthread_mutex_lock(&request->start->mutex);
    ++request->start->ready;
    if (request->start->ready == 2U)
        pthread_cond_broadcast(&request->start->condition);
    while (!request->start->released)
        pthread_cond_wait(&request->start->condition,
                          &request->start->mutex);
    pthread_mutex_unlock(&request->start->mutex);
    request->result = request->api->offline_sensor_enqueue(
        request->api_context, request->sensor_context, request->raw_data);
    return NULL;
}

static void *offline_start_thread(void *opaque)
{
    struct offline_start_request *request = opaque;
    pthread_mutex_lock(&request->start->mutex);
    ++request->start->ready;
    if (request->start->ready == 2U)
        pthread_cond_broadcast(&request->start->condition);
    while (!request->start->released)
        pthread_cond_wait(&request->start->condition,
                          &request->start->mutex);
    pthread_mutex_unlock(&request->start->mutex);
    request->result = request->api->offline_sensor_run(
        request->api_context, request->sensor_context);
    return NULL;
}

static int start_offline_sensors(struct ngcd_rk_graph *graph)
{
    struct raw_capture_start start;
    struct offline_start_request request[2];
    pthread_t thread[2];
    unsigned int created = 0U;
    unsigned int index;
    int result = -1;
    memset(&start, 0, sizeof(start));
    memset(request, 0, sizeof(request));
    if (pthread_mutex_init(&start.mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&start.condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&start.mutex);
        return -1;
    }
    for (index = 0U; index < 2U; ++index) {
        request[index].start = &start;
        request[index].api = graph->api;
        request[index].api_context = graph->api_context;
        request[index].sensor_context = graph->sensor_handle[index];
        request[index].result = -1;
        if (request[index].sensor_context == NULL ||
            pthread_create(&thread[index], NULL, offline_start_thread,
                           &request[index]) != 0)
            break;
        ++created;
    }
    pthread_mutex_lock(&start.mutex);
    if (created == 2U) {
        while (start.ready < 2U)
            pthread_cond_wait(&start.condition, &start.mutex);
    }
    start.released = true;
    pthread_cond_broadcast(&start.condition);
    pthread_mutex_unlock(&start.mutex);
    for (index = 0U; index < created; ++index)
        (void)pthread_join(thread[index], NULL);
    if (created == 2U && request[0].result == 0 && request[1].result == 0)
        result = 0;
    (void)pthread_cond_destroy(&start.condition);
    (void)pthread_mutex_destroy(&start.mutex);
    return result;
}

static void put_u32(unsigned char *buffer, size_t offset, uint32_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static void put_u16(unsigned char *buffer, size_t offset, uint16_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static void put_be16(unsigned char *buffer, size_t offset, uint16_t value)
{
    buffer[offset] = (unsigned char)(value >> 8U);
    buffer[offset + 1U] = (unsigned char)value;
}

static void put_exif_entry(unsigned char *tiff, size_t offset, uint16_t tag,
                           uint16_t type, uint32_t count, uint32_t value)
{
    put_u16(tiff, offset, tag);
    put_u16(tiff, offset + 2U, type);
    put_u32(tiff, offset + 4U, count);
    put_u32(tiff, offset + 8U, value);
}

static void put_pointer(unsigned char *buffer, size_t offset, const void *value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static uint32_t get_u32(const unsigned char *buffer, size_t offset)
{
    uint32_t value;
    memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

static void *get_pointer(const unsigned char *buffer, size_t offset)
{
    void *value;
    memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

static int is_stitched(const struct ngcd_profile *profile)
{
    return profile->stitch.width > 0 && profile->stitch.height > 0;
}

static unsigned int graph_sensor_count(const struct ngcd_profile *profile)
{
    if (is_stitched(profile))
        return 2;
    return 1;
}

static int graph_sensor_base(const struct ngcd_profile *profile)
{
    return strcmp(profile->camera_mode, "SENSOR_1") == 0 ? 1 : 0;
}

static const struct ngcd_video_geometry *geometry_at(
    const struct ngcd_video_geometry *geometry, size_t count, size_t index)
{
    if (count == 0)
        return NULL;
    return &geometry[index < count ? index : 0];
}

static int validate_api(const struct ngcd_rk_api *api)
{
    return api != NULL && api->system_init != NULL &&
           api->system_exit != NULL && api->bind != NULL &&
           api->unbind != NULL && api->sensor_start != NULL &&
           api->sensor_stop != NULL && api->sensor_synchronize != NULL &&
           api->prepare_directory != NULL &&
           api->wait_output != NULL && api->vi_get_dev_attr != NULL &&
           api->vi_set_dev_attr != NULL && api->vi_get_dev_enabled != NULL &&
           api->vi_enable_dev != NULL && api->vi_disable_dev != NULL &&
           api->vi_bind_pipe != NULL && api->vi_set_channel_attr != NULL &&
           api->vi_enable_channel != NULL &&
           api->vi_disable_channel != NULL &&
           api->vpss_create_group != NULL &&
           api->vpss_destroy_group != NULL &&
           api->vpss_set_device != NULL &&
           api->vpss_enable_backup != NULL &&
           api->vpss_start_group != NULL && api->vpss_stop_group != NULL &&
           api->vpss_set_channel_attr != NULL &&
           api->vpss_enable_channel != NULL &&
           api->vpss_disable_channel != NULL &&
           api->avs_set_working_set != NULL &&
           api->avs_create_group != NULL &&
           api->avs_destroy_group != NULL &&
           api->avs_start_group != NULL && api->avs_stop_group != NULL &&
           api->avs_set_channel_attr != NULL &&
           api->avs_enable_channel != NULL &&
           api->avs_disable_channel != NULL &&
           api->venc_create_channel != NULL &&
           api->venc_destroy_channel != NULL &&
           api->venc_set_rc_param != NULL &&
           api->venc_start_receive != NULL &&
           api->venc_stop_receive != NULL &&
           api->venc_get_stream != NULL &&
           api->venc_release_stream != NULL &&
           api->venc_request_idr != NULL &&
           api->venc_get_h264_vui != NULL &&
           api->venc_set_h264_vui != NULL &&
           api->venc_get_h265_vui != NULL &&
           api->venc_set_h265_vui != NULL &&
           api->ai_set_pub_attr != NULL && api->ai_enable != NULL &&
           api->ai_disable != NULL && api->ai_enable_channel != NULL &&
           api->ai_disable_channel != NULL &&
           api->ai_set_channel_param != NULL &&
           api->ai_enable_resample != NULL &&
           api->ai_disable_resample != NULL && api->ai_get_frame != NULL &&
           api->ai_release_frame != NULL &&
           api->mb_handle_to_address != NULL && api->mb_get_size != NULL;
}

static void stop_validation_encoder(struct ngcd_rk_graph *graph)
{
    const struct ngcd_rk_api *api = graph->api;
    if (graph->validation_bound) {
        struct ngcd_rk_channel source = {RK_MODULE_AVS, 0, 0};
        struct ngcd_rk_channel destination = {RK_MODULE_VENC, 0, 0};
        (void)api->unbind(graph->api_context, &source, &destination);
    }
    if (graph->validation_receiving)
        (void)api->venc_stop_receive(graph->api_context, 0);
    if (graph->validation_started)
        (void)api->venc_destroy_channel(graph->api_context, 0);
    graph->validation_bound = false;
    graph->validation_receiving = false;
    graph->validation_started = false;
}

static void stop_snapshot_encoder(struct ngcd_rk_graph *graph)
{
    if (graph->snapshot_channel_started) {
        (void)graph->api->venc_stop_receive(graph->api_context,
                                            RK_SNAPSHOT_CHANNEL);
        (void)graph->api->venc_destroy_channel(graph->api_context,
                                                RK_SNAPSHOT_CHANNEL);
    }
    graph->snapshot_channel_started = false;
}

static void discard_stacked_snapshot(struct ngcd_rk_graph *graph)
{
    if (graph->stacked_snapshot_handle != NULL && graph->api != NULL &&
        graph->api->mmz_free != NULL)
        (void)graph->api->mmz_free(graph->api_context,
                                  graph->stacked_snapshot_handle);
    graph->stacked_snapshot_handle = NULL;
    graph->stacked_snapshot_width = 0U;
    graph->stacked_snapshot_height = 0U;
    graph->stacked_snapshot_stride = 0U;
    graph->stacked_snapshot_virtual_height = 0U;
}

static unsigned int integer_sqrt(unsigned int value)
{
    unsigned int result = 0U;
    unsigned int bit = 1U << 30U;
    while (bit > value)
        bit >>= 2U;
    while (bit != 0U) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1U) + bit;
        } else {
            result >>= 1U;
        }
        bit >>= 2U;
    }
    return result;
}

static void build_luma_stack_table(unsigned char table[48000],
                                   unsigned int shift)
{
    unsigned int value;
    for (value = 0U; value < 48000U; ++value) {
        unsigned int combined = integer_sqrt(value << shift);
        if (combined > 0U && combined < 80U) {
            /* AVS frames have already passed through the ISP, so very weak
             * signal can sit just above studio black in every sub-exposure.
             * Give that recoverable shadow detail a restrained photographic
             * toe while keeping exact black at black. The lift peaks at about
             * eleven code values and vanishes before the midtones. */
            unsigned int lift =
                20U * combined * (80U - combined) /
                (80U * (combined + 8U));
            combined += lift;
        }
        table[value] =
            (unsigned char)(combined > 219U ? 219U : combined);
    }
}

static unsigned int limited_luma_signal(unsigned char value)
{
    unsigned int signal = value > 16U ? (unsigned int)value - 16U : 0U;
    return signal > 219U ? 219U : signal;
}

static int rounded_signed_division(int numerator, unsigned int denominator)
{
    if (denominator == 0U)
        return 0;
    if (numerator >= 0)
        return (numerator + (int)(denominator / 2U)) / (int)denominator;
    return -((-numerator + (int)(denominator / 2U)) / (int)denominator);
}

static int stacked_chroma_center(
    uint16_t chroma_sum, unsigned int count,
    uint16_t input_luma_sum, unsigned int output_luma_sum)
{
    int centered_sum = (int)chroma_sum - (int)(128U * count);
    return rounded_signed_division(
        centered_sum * (int)output_luma_sum,
        (unsigned int)input_luma_sum);
}

static unsigned int chroma_scale_limit(unsigned int current,
                                       unsigned int numerator,
                                       unsigned int denominator)
{
    unsigned int candidate;
    if (denominator == 0U)
        return current;
    candidate = (numerator << 12U) / denominator;
    return candidate < current ? candidate : current;
}

static void compress_stacked_chroma(int *chroma_u, int *chroma_v,
                                    unsigned int luma_0,
                                    unsigned int luma_1,
                                    unsigned int luma_2,
                                    unsigned int luma_3)
{
    unsigned int scale = 4096U;
    unsigned int minimum_luma = luma_0;
    unsigned int maximum_luma = luma_0;
    unsigned int minimum_base;
    unsigned int maximum_base;
    int delta[3];
    size_t index;

    if (luma_1 < minimum_luma) minimum_luma = luma_1;
    if (luma_2 < minimum_luma) minimum_luma = luma_2;
    if (luma_3 < minimum_luma) minimum_luma = luma_3;
    if (luma_1 > maximum_luma) maximum_luma = luma_1;
    if (luma_2 > maximum_luma) maximum_luma = luma_2;
    if (luma_3 > maximum_luma) maximum_luma = luma_3;

    /* Preserve the U/V vector (and therefore hue) while bringing it into the
     * legal studio-range chroma box. This is preferable to clipping U and V
     * independently, which rotates saturated colors. */
    if (*chroma_u > 112 || *chroma_u < -112)
        scale = chroma_scale_limit(
            scale, 112U,
            (unsigned int)(*chroma_u < 0 ? -*chroma_u : *chroma_u));
    if (*chroma_v > 112 || *chroma_v < -112)
        scale = chroma_scale_limit(
            scale, 112U,
            (unsigned int)(*chroma_v < 0 ? -*chroma_v : *chroma_v));
    *chroma_u = rounded_signed_division(*chroma_u * (int)scale, 4096U);
    *chroma_v = rounded_signed_division(*chroma_v * (int)scale, 4096U);

    /* Estimate the BT.709 RGB gamut for the whole NV12 2x2 block. A strict
     * projection can make bright colored lights look unnaturally pastel, so
     * move one quarter of the way toward the legal boundary. The resulting
     * soft shoulder retains ordinary color, suppresses colored black-level
     * noise, and avoids the hard per-channel clipping that made long stacks
     * appear oversaturated. */
    minimum_base = 298U * minimum_luma;
    maximum_base = 298U * maximum_luma;
    delta[0] = 459 * *chroma_v;
    delta[1] = -55 * *chroma_u - 136 * *chroma_v;
    delta[2] = 541 * *chroma_u;
    scale = 4096U;
    for (index = 0U; index < sizeof(delta) / sizeof(delta[0]); ++index) {
        if (delta[index] > 0)
            scale = chroma_scale_limit(
                scale, maximum_base < 65280U ? 65280U - maximum_base : 0U,
                (unsigned int)delta[index]);
        else if (delta[index] < 0)
            scale = chroma_scale_limit(
                scale, minimum_base, (unsigned int)-delta[index]);
    }
    scale = (3U * 4096U + scale + 2U) / 4U;
    *chroma_u = rounded_signed_division(*chroma_u * (int)scale, 4096U);
    *chroma_v = rounded_signed_division(*chroma_v * (int)scale, 4096U);
}

static bool night_stack_count_valid(unsigned int count)
{
    return count == 2U || count == 4U || count == 8U || count == 16U ||
           count == 24U;
}

static unsigned int night_stack_shift(unsigned int count)
{
    if (count <= 2U)
        return 1U;
    if (count <= 4U)
        return 2U;
    if (count <= 8U)
        return 3U;
    return count <= 16U ? 4U : 5U;
}

static int stack_avs_frames(struct ngcd_rk_graph *graph, unsigned int count)
{
    static unsigned char luma_table[48000];
    static unsigned int table_shift;
    static bool table_ready;
    _Alignas(16) unsigned char frame[RK_FRAME_INFO_SIZE];
    unsigned char *destination = NULL;
    uint16_t *luma_accumulator = NULL;
    uint16_t *chroma_accumulator = NULL;
    uint16_t *block_luma_accumulator = NULL;
    uint32_t destination_width = 0U;
    uint32_t destination_height = 0U;
    uint32_t destination_stride = 0U;
    uint32_t destination_virtual_height = 0U;
    size_t destination_required = 0U;
    size_t luma_pixels = 0U;
    size_t chroma_pixels = 0U;
    size_t chroma_blocks = 0U;
    unsigned int shift;
    unsigned int rounding;
    unsigned int frame_index;
    int result = -1;

    if (graph == NULL || graph->api == NULL ||
        !night_stack_count_valid(count) ||
        graph->api->avs_get_channel_frame == NULL ||
        graph->api->avs_release_channel_frame == NULL ||
        graph->api->mmz_alloc == NULL || graph->api->mmz_free == NULL ||
        graph->api->mb_handle_to_address == NULL ||
        graph->api->mb_get_size == NULL)
        return -1;
    shift = night_stack_shift(count);
    rounding = 1U << (shift - 1U);
    discard_stacked_snapshot(graph);
    if (!table_ready || table_shift != shift) {
        build_luma_stack_table(luma_table, shift);
        table_shift = shift;
        table_ready = true;
    }
    for (frame_index = 0U; frame_index < count; ++frame_index) {
        unsigned char *source;
        void *source_handle;
        uint32_t width;
        uint32_t height;
        uint32_t stride;
        uint32_t virtual_height;
        uint32_t format;
        size_t required;
        size_t row;
        bool acquired = false;
        bool frame_valid = false;

        memset(frame, 0, sizeof(frame));
        if (graph->api->avs_get_channel_frame(graph->api_context, 0, 0,
                                              frame, 3000) != 0)
            goto finished;
        acquired = true;
        source_handle = get_pointer(frame, 0U);
        width = get_u32(frame, 8U);
        height = get_u32(frame, 12U);
        stride = get_u32(frame, 16U);
        virtual_height = get_u32(frame, 20U);
        format = get_u32(frame, 28U);
        source = get_pointer(frame, 48U);
        if (source == NULL && source_handle != NULL)
            source = graph->api->mb_handle_to_address(graph->api_context,
                                                       source_handle);
        if (source_handle == NULL || source == NULL || format != 0U ||
            width == 0U || (width & 1U) != 0U ||
            height == 0U || (height & 1U) != 0U ||
            stride < width || virtual_height < height ||
            stride > 16384U || virtual_height > 8192U ||
            (size_t)stride > SIZE_MAX / (size_t)virtual_height) {
            goto release;
        }
        required = (size_t)stride * (size_t)virtual_height;
        if (required > SIZE_MAX - required / 2U)
            goto release;
        required += required / 2U;
        if (graph->api->mb_get_size(graph->api_context, source_handle) <
            required)
            goto release;
        if (frame_index == 0U) {
            if (width != (uint32_t)graph->snapshot_width ||
                height != (uint32_t)graph->snapshot_height ||
                graph->api->mmz_alloc(graph->api_context,
                                      &graph->stacked_snapshot_handle,
                                      required) != 0 ||
                graph->stacked_snapshot_handle == NULL)
                goto release;
            destination = graph->api->mb_handle_to_address(
                graph->api_context, graph->stacked_snapshot_handle);
            if (destination == NULL ||
                graph->api->mb_get_size(graph->api_context,
                                        graph->stacked_snapshot_handle) <
                    required)
                goto release;
            destination_width = width;
            destination_height = height;
            destination_stride = stride;
            destination_virtual_height = virtual_height;
            destination_required = required;
            if ((size_t)width > SIZE_MAX / (size_t)height)
                goto release;
            luma_pixels = (size_t)width * (size_t)height;
            chroma_pixels = luma_pixels / 2U;
            chroma_blocks = luma_pixels / 4U;
            luma_accumulator = calloc(luma_pixels,
                                      sizeof(*luma_accumulator));
            chroma_accumulator = calloc(chroma_pixels,
                                        sizeof(*chroma_accumulator));
            block_luma_accumulator = calloc(
                chroma_blocks, sizeof(*block_luma_accumulator));
            if (luma_accumulator == NULL || chroma_accumulator == NULL ||
                block_luma_accumulator == NULL)
                goto release;
            memset(destination, 16, (size_t)stride * virtual_height);
            memset(destination + (size_t)stride * virtual_height, 128,
                   (size_t)stride * virtual_height / 2U);
        } else if (width != destination_width ||
                   height != destination_height ||
                   stride != destination_stride ||
                   virtual_height != destination_virtual_height ||
                   required != destination_required) {
            goto release;
        }
        for (row = 0U; row < height; ++row) {
            size_t column;
            const unsigned char *input_row = source + row * stride;
            uint16_t *sum_row = luma_accumulator + row * width;
            uint16_t *block_row = block_luma_accumulator +
                (row / 2U) * (width / 2U);
            for (column = 0U; column < width; column += 2U) {
                unsigned int signal_0 =
                    limited_luma_signal(input_row[column]);
                unsigned int signal_1 =
                    limited_luma_signal(input_row[column + 1U]);
                sum_row[column] = (uint16_t)(sum_row[column] +
                    ((signal_0 * signal_0 + rounding) >> shift));
                sum_row[column + 1U] = (uint16_t)(
                    sum_row[column + 1U] +
                    ((signal_1 * signal_1 + rounding) >> shift));
                block_row[column / 2U] = (uint16_t)(
                    block_row[column / 2U] + signal_0 + signal_1);
            }
        }
        {
            size_t chroma_offset = (size_t)stride * virtual_height;
            for (row = 0U; row < height / 2U; ++row) {
                size_t column;
                const unsigned char *input_row =
                    source + chroma_offset + row * stride;
                uint16_t *sum_row = chroma_accumulator + row * width;
                for (column = 0U; column < width; column += 2U) {
                    sum_row[column] =
                        (uint16_t)(sum_row[column] + input_row[column]);
                    sum_row[column + 1U] = (uint16_t)(
                        sum_row[column + 1U] + input_row[column + 1U]);
                }
            }
        }
        frame_valid = true;
release:
        if (acquired &&
            graph->api->avs_release_channel_frame(graph->api_context, 0, 0,
                                                   frame) != 0)
            goto finished;
        if (!frame_valid)
            goto finished;
    }
    {
        size_t row;
        for (row = 0U; row < destination_height; ++row) {
            size_t column;
            unsigned char *output_row =
                destination + row * destination_stride;
            const uint16_t *sum_row =
                luma_accumulator + row * destination_width;
            for (column = 0U; column < destination_width; ++column) {
                unsigned int sum = sum_row[column];
                if (sum >= 48000U)
                    sum = 47999U;
                output_row[column] =
                    (unsigned char)(16U + luma_table[sum]);
            }
        }
        for (row = 0U; row < destination_height / 2U; ++row) {
            size_t column;
            unsigned char *output_row = destination +
                (size_t)destination_stride * destination_virtual_height +
                row * destination_stride;
            const uint16_t *sum_row =
                chroma_accumulator + row * destination_width;
            const uint16_t *block_row = block_luma_accumulator +
                row * (destination_width / 2U);
            const unsigned char *luma_row_0 =
                destination + row * 2U * destination_stride;
            const unsigned char *luma_row_1 =
                luma_row_0 + destination_stride;
            for (column = 0U; column < destination_width; column += 2U) {
                unsigned int output_luma_sum =
                    limited_luma_signal(luma_row_0[column]) +
                    limited_luma_signal(luma_row_0[column + 1U]) +
                    limited_luma_signal(luma_row_1[column]) +
                    limited_luma_signal(luma_row_1[column + 1U]);
                uint16_t input_luma_sum = block_row[column / 2U];
                int chroma_u = stacked_chroma_center(
                    sum_row[column], count, input_luma_sum, output_luma_sum);
                int chroma_v = stacked_chroma_center(
                    sum_row[column + 1U], count, input_luma_sum,
                    output_luma_sum);
                compress_stacked_chroma(
                    &chroma_u, &chroma_v,
                    limited_luma_signal(luma_row_0[column]),
                    limited_luma_signal(luma_row_0[column + 1U]),
                    limited_luma_signal(luma_row_1[column]),
                    limited_luma_signal(luma_row_1[column + 1U]));
                output_row[column] = (unsigned char)(128 + chroma_u);
                output_row[column + 1U] = (unsigned char)(128 + chroma_v);
            }
        }
    }
    graph->stacked_snapshot_width = destination_width;
    graph->stacked_snapshot_height = destination_height;
    graph->stacked_snapshot_stride = destination_stride;
    graph->stacked_snapshot_virtual_height = destination_virtual_height;
    result = 0;

finished:
    free(block_luma_accumulator);
    free(chroma_accumulator);
    free(luma_accumulator);
    if (result != 0)
        discard_stacked_snapshot(graph);
    return result;
}

static void drain_avs_queue(struct ngcd_rk_graph *graph)
{
    _Alignas(16) unsigned char frame[RK_FRAME_INFO_SIZE];
    unsigned int drained;
    if (graph == NULL || graph->api == NULL ||
        graph->api->avs_get_channel_frame == NULL ||
        graph->api->avs_release_channel_frame == NULL)
        return;
    for (drained = 0U; drained < 8U; ++drained) {
        memset(frame, 0, sizeof(frame));
        if (graph->api->avs_get_channel_frame(graph->api_context, 0, 0,
                                              frame, 0) != 0)
            break;
        if (graph->api->avs_release_channel_frame(graph->api_context, 0, 0,
                                                  frame) != 0)
            break;
    }
}

static int start_snapshot_encoder(struct ngcd_rk_graph *graph)
{
    struct ngcd_rk_venc_chn_attr attribute;
    unsigned char jpeg_parameter[NGCD_RK_VENC_JPEG_PARAM_SIZE];
    uint64_t pixels;
    uint64_t buffer_size;
    uint32_t aligned_width;
    uint32_t aligned_height;

    if (graph->snapshot_channel_started)
        return 0;
    if (graph->snapshot_width <= 0 || graph->snapshot_height <= 0 ||
        graph->api->venc_get_jpeg_param == NULL ||
        graph->api->venc_set_jpeg_param == NULL)
        return -1;
    pixels = (uint64_t)(unsigned int)graph->snapshot_width *
             (uint64_t)(unsigned int)graph->snapshot_height;
    buffer_size = pixels + pixels / 2U;
    if (buffer_size > UINT32_MAX)
        return -1;
    aligned_width = ((uint32_t)graph->snapshot_width + 15U) & ~15U;
    aligned_height = ((uint32_t)graph->snapshot_height + 15U) & ~15U;
    memset(&attribute, 0, sizeof(attribute));
    put_u32(attribute.bytes, 0U, 15U); /* RK_VIDEO_ID_JPEG */
    put_u32(attribute.bytes, 12U, (uint32_t)buffer_size);
    put_u32(attribute.bytes, 24U, (uint32_t)graph->snapshot_width);
    put_u32(attribute.bytes, 28U, (uint32_t)graph->snapshot_height);
    put_u32(attribute.bytes, 32U, aligned_width);
    put_u32(attribute.bytes, 36U, aligned_height);
    put_u32(attribute.bytes, 40U, 3U);
    if (graph->api->venc_create_channel(graph->api_context,
                                         RK_SNAPSHOT_CHANNEL,
                                         &attribute) != 0) {
        fprintf(stderr, "ngcd: snapshot failed during JPEG channel creation\n");
        return -1;
    }
    graph->snapshot_channel_started = true;
    memset(jpeg_parameter, 0, sizeof(jpeg_parameter));
    if (graph->api->venc_get_jpeg_param(graph->api_context,
                                        RK_SNAPSHOT_CHANNEL,
                                        jpeg_parameter) != 0) {
        fprintf(stderr, "ngcd: snapshot failed during JPEG parameter read\n");
        stop_snapshot_encoder(graph);
        return -1;
    }
    put_u32(jpeg_parameter, 0U, 95U);
    if (graph->api->venc_set_jpeg_param(graph->api_context,
                                        RK_SNAPSHOT_CHANNEL,
                                        jpeg_parameter) != 0) {
        fprintf(stderr, "ngcd: snapshot failed during JPEG parameter write\n");
        stop_snapshot_encoder(graph);
        return -1;
    }
    return 0;
}

static int write_exif(FILE *file, const struct ngcd_rk_graph *graph,
                      const struct ngcd_rk_exif_metadata *metadata)
{
    unsigned char segment[260];
    unsigned char *tiff = segment + 10U;
    size_t entry_offset;
    uint32_t data_offset;
    uint32_t original_time_offset;
    uint32_t digitized_time_offset;
    uint16_t exif_entry_count = 4U;
    static const char make[] = "VIEWPT";
    static const char model[] = "CALF VR180";
    static const char software[] = "ngcd-c-0.1";
    if (file == NULL || graph == NULL || metadata == NULL ||
        strlen(metadata->datetime) != 19U ||
        ((metadata->exposure_numerator == 0U) !=
         (metadata->exposure_denominator == 0U)))
        return -1;
    if (metadata->exposure_denominator != 0U)
        ++exif_entry_count;
    if (metadata->iso != 0U)
        ++exif_entry_count;
    memset(segment, 0, sizeof(segment));
    segment[0] = 0xffU;
    segment[1] = 0xe1U;
    put_be16(segment, 2U, 258U);
    memcpy(segment + 4U, "Exif\0\0", 6U);
    tiff[0] = 'I';
    tiff[1] = 'I';
    put_u16(tiff, 2U, 42U);
    put_u32(tiff, 4U, 8U);
    put_u16(tiff, 8U, 5U);
    put_exif_entry(tiff, 10U, 0x010fU, 2U, sizeof(make), 74U);
    put_exif_entry(tiff, 22U, 0x0110U, 2U, sizeof(model), 81U);
    put_exif_entry(tiff, 34U, 0x0131U, 2U, sizeof(software), 92U);
    put_exif_entry(tiff, 46U, 0x0132U, 2U, 20U, 103U);
    put_exif_entry(tiff, 58U, 0x8769U, 4U, 1U, 124U);
    put_u32(tiff, 70U, 0U);
    memcpy(tiff + 74U, make, sizeof(make));
    memcpy(tiff + 81U, model, sizeof(model));
    memcpy(tiff + 92U, software, sizeof(software));
    memcpy(tiff + 103U, metadata->datetime, 19U);
    put_u16(tiff, 124U, exif_entry_count);
    entry_offset = 126U;
    data_offset = 124U + 2U + (uint32_t)exif_entry_count * 12U + 4U;
    if (metadata->exposure_denominator != 0U) {
        put_exif_entry(tiff, entry_offset, 0x829aU, 5U, 1U, data_offset);
        put_u32(tiff, data_offset, metadata->exposure_numerator);
        put_u32(tiff, data_offset + 4U, metadata->exposure_denominator);
        entry_offset += 12U;
        data_offset += 8U;
    }
    if (metadata->iso != 0U) {
        put_exif_entry(tiff, entry_offset, 0x8827U, 3U, 1U,
                       metadata->iso);
        entry_offset += 12U;
    }
    original_time_offset = data_offset;
    digitized_time_offset = data_offset + 20U;
    put_exif_entry(tiff, entry_offset, 0x9003U, 2U, 20U,
                   original_time_offset);
    entry_offset += 12U;
    put_exif_entry(tiff, entry_offset, 0x9004U, 2U, 20U,
                   digitized_time_offset);
    entry_offset += 12U;
    put_exif_entry(tiff, entry_offset, 0xa002U, 4U, 1U,
                   (uint32_t)graph->snapshot_width);
    entry_offset += 12U;
    put_exif_entry(tiff, entry_offset, 0xa003U, 4U, 1U,
                   (uint32_t)graph->snapshot_height);
    entry_offset += 12U;
    put_u32(tiff, entry_offset, 0U);
    memcpy(tiff + original_time_offset, metadata->datetime, 19U);
    memcpy(tiff + digitized_time_offset, metadata->datetime, 19U);
    return fwrite(segment, 1U, sizeof(segment), file) == sizeof(segment)
               ? 0 : -1;
}

int ngcd_rk_graph_snapshot(
    struct ngcd_rk_graph *graph, const char *path,
    const struct ngcd_rk_exif_metadata *metadata)
{
    _Alignas(16) unsigned char frame[RK_FRAME_INFO_SIZE];
    _Alignas(16) unsigned char
        pack[NGCD_RK_VENC_PACK_SIZE * NGCD_RK_VENC_PACK_COUNT];
    _Alignas(16) unsigned char stream[NGCD_RK_VENC_STREAM_SIZE];
    int32_t receive_count = 1;
    uint32_t pack_count;
    uint32_t index;
    int hardware_result;
    FILE *file = NULL;
    bool frame_acquired = false;
    bool stacked_frame = false;
    bool stream_acquired = false;
    bool success = false;
    const char *failed_operation = "precondition check";

    if (graph == NULL || graph->api == NULL || path == NULL ||
        path[0] == '\0' || metadata == NULL ||
        strlen(metadata->datetime) != 19U || !graph->avs_group_started ||
        !graph->avs_channel_started ||
        graph->api->avs_get_channel_frame == NULL ||
        graph->api->avs_release_channel_frame == NULL ||
        graph->api->venc_send_frame == NULL ||
        graph->api->mb_handle_to_address == NULL ||
        start_snapshot_encoder(graph) != 0)
        return -1;
    failed_operation = "JPEG receive start";
    if (graph->api->venc_start_receive(graph->api_context,
                                        RK_SNAPSHOT_CHANNEL,
                                        &receive_count) != 0)
        goto done;
    memset(frame, 0, sizeof(frame));
    if (graph->stacked_snapshot_handle != NULL) {
        failed_operation = "stacked frame validation";
        if (graph->stacked_snapshot_width !=
                (uint32_t)graph->snapshot_width ||
            graph->stacked_snapshot_height !=
                (uint32_t)graph->snapshot_height ||
            graph->stacked_snapshot_stride < graph->stacked_snapshot_width ||
            graph->stacked_snapshot_virtual_height <
                graph->stacked_snapshot_height)
            goto done;
        put_pointer(frame, 0U, graph->stacked_snapshot_handle);
        put_u32(frame, 8U, graph->stacked_snapshot_width);
        put_u32(frame, 12U, graph->stacked_snapshot_height);
        put_u32(frame, 16U, graph->stacked_snapshot_stride);
        put_u32(frame, 20U, graph->stacked_snapshot_virtual_height);
        put_u32(frame, 28U, 0U); /* RK_FMT_YUV420SP */
        stacked_frame = true;
    } else {
        failed_operation = "AVS frame acquisition";
        hardware_result = graph->api->avs_get_channel_frame(
            graph->api_context, 0, 0, frame, 3000);
        if (hardware_result != 0) {
            fprintf(stderr, "ngcd: AVS frame acquisition returned 0x%x\n",
                    (unsigned int)hardware_result);
            goto done;
        }
        frame_acquired = true;
    }
    failed_operation = "JPEG frame submission";
    if (graph->api->venc_send_frame(graph->api_context, RK_SNAPSHOT_CHANNEL,
                                     frame, 3000) != 0)
        goto done;
    if (stacked_frame) {
        discard_stacked_snapshot(graph);
        stacked_frame = false;
    } else {
        failed_operation = "AVS frame release";
        int release_result = graph->api->avs_release_channel_frame(
            graph->api_context, 0, 0, frame);
        frame_acquired = false;
        if (release_result != 0)
            goto done;
    }
    memset(pack, 0, sizeof(pack));
    memset(stream, 0, sizeof(stream));
    put_pointer(stream, 0U, pack);
    failed_operation = "JPEG packet acquisition";
    if (graph->api->venc_get_stream(graph->api_context, RK_SNAPSHOT_CHANNEL,
                                    stream, 3000) != 0)
        goto done;
    stream_acquired = true;
    memcpy(&pack_count, stream + 16U, sizeof(pack_count));
    failed_operation = "JPEG packet validation";
    if (pack_count == 0U || pack_count > NGCD_RK_VENC_PACK_COUNT)
        goto done;
    failed_operation = "temporary JPEG creation";
    file = fopen(path, "wb");
    if (file == NULL)
        goto done;
    for (index = 0; index < pack_count; ++index) {
        const unsigned char *descriptor =
            pack + (size_t)index * NGCD_RK_VENC_PACK_SIZE;
        void *handle;
        void *address;
        uint32_t size;
        memcpy(&handle, descriptor, sizeof(handle));
        memcpy(&size, descriptor + 8U, sizeof(size));
        address = graph->api->mb_handle_to_address(graph->api_context,
                                                    handle);
        failed_operation = "JPEG packet write";
        if (address == NULL || size == 0U)
            goto done;
        if (index == 0U) {
            const unsigned char *bytes = address;
            if (size < 2U || bytes[0] != 0xffU || bytes[1] != 0xd8U ||
                fwrite(bytes, 1U, 2U, file) != 2U ||
                write_exif(file, graph, metadata) != 0 ||
                (size > 2U && fwrite(bytes + 2U, 1U, size - 2U, file) !=
                                  size - 2U))
                goto done;
        } else if (fwrite(address, 1U, size, file) != size) {
            goto done;
        }
    }
    failed_operation = "JPEG sync";
    if (fflush(file) != 0 || fdatasync(fileno(file)) != 0 ||
        fclose(file) != 0) {
        file = NULL;
        goto done;
    }
    file = NULL;
    success = true;

done:
    if (file != NULL)
        (void)fclose(file);
    if (stream_acquired) {
        failed_operation = "JPEG packet release";
        if (graph->api->venc_release_stream(graph->api_context,
                                            RK_SNAPSHOT_CHANNEL,
                                            stream) != 0)
            success = false;
    }
    if (frame_acquired)
        (void)graph->api->avs_release_channel_frame(graph->api_context,
                                                     0, 0, frame);
    if (stacked_frame)
        discard_stacked_snapshot(graph);
    if (!success) {
        fprintf(stderr, "ngcd: snapshot failed during %s\n",
                failed_operation);
        (void)unlink(path);
        stop_snapshot_encoder(graph);
        return -1;
    }
    return 0;
}

static int fail_validation_encoder(struct ngcd_rk_graph *graph,
                                   const char *operation)
{
    fprintf(stderr, "ngcd: stitched output validation failed during %s\n",
            operation);
    stop_validation_encoder(graph);
    graph->validation_pending = false;
    graph->validation_failed = true;
    return -1;
}

static int start_validation_encoder(struct ngcd_rk_graph *graph)
{
    struct ngcd_rk_venc_chn_attr attribute;
    struct ngcd_rk_venc_rc_param rate_control;
    struct ngcd_rk_channel source = {RK_MODULE_AVS, 0, 0};
    struct ngcd_rk_channel destination = {RK_MODULE_VENC, 0, 0};
    int32_t receive_count = -1;
    unsigned char vui[NGCD_RK_VENC_VUI_SIZE];
    uint32_t codec;

    if (ngcd_rk_encoder_attributes(&graph->validation_encoder, &attribute,
                                   &rate_control) != 0)
        return fail_validation_encoder(graph, "configuration");
    if (graph->api->venc_create_channel(graph->api_context, 0,
                                         &attribute) != 0)
        return fail_validation_encoder(graph, "channel creation");
    graph->validation_started = true;
    if (graph->api->venc_set_rc_param(graph->api_context, 0,
                                      &rate_control) != 0)
        return fail_validation_encoder(graph, "rate control");
    memset(vui, 0, sizeof(vui));
    memcpy(&codec, attribute.bytes, sizeof(codec));
    if (codec == 8U) {
        if (graph->api->venc_get_h264_vui(graph->api_context, 0, vui) != 0)
            return fail_validation_encoder(graph, "H.264 VUI read");
        vui[22] = graph->validation_encoder.color_range == 0 ? 1U : 0U;
        if (graph->api->venc_set_h264_vui(graph->api_context, 0, vui) != 0)
            return fail_validation_encoder(graph, "H.264 VUI write");
    } else {
        if (graph->api->venc_get_h265_vui(graph->api_context, 0, vui) != 0)
            return fail_validation_encoder(graph, "H.265 VUI read");
        vui[26] = graph->validation_encoder.color_range == 0 ? 1U : 0U;
        if (graph->api->venc_set_h265_vui(graph->api_context, 0, vui) != 0)
            return fail_validation_encoder(graph, "H.265 VUI write");
    }
    if (graph->api->venc_start_receive(graph->api_context, 0,
                                        &receive_count) != 0)
        return fail_validation_encoder(graph, "receive start");
    graph->validation_receiving = true;
    if (graph->api->bind(graph->api_context, &source, &destination) != 0)
        return fail_validation_encoder(graph, "AVS bind");
    graph->validation_bound = true;
    return 0;
}

static void stop_record_hardware(struct ngcd_rk_graph *graph)
{
    struct ngcd_rk_channel source = {RK_MODULE_AVS, 0, 0};
    struct ngcd_rk_channel destination = {RK_MODULE_VENC, 0, 0};
    if (graph->record_bound)
        (void)graph->api->unbind(graph->api_context, &source, &destination);
    if (graph->record_receiving)
        (void)graph->api->venc_stop_receive(graph->api_context, 0);
    if (graph->record_channel_started)
        (void)graph->api->venc_destroy_channel(graph->api_context, 0);
    graph->record_bound = false;
    graph->record_receiving = false;
    graph->record_channel_started = false;
    memset(&graph->record_encoder, 0, sizeof(graph->record_encoder));
}

static void stop_audio_hardware(struct ngcd_rk_graph *graph)
{
    if (graph->audio_resample_started)
        (void)graph->api->ai_disable_resample(
            graph->api_context, RK_AI_DEVICE, RK_AI_CHANNEL);
    if (graph->audio_channel_started)
        (void)graph->api->ai_disable_channel(
            graph->api_context, RK_AI_DEVICE, RK_AI_CHANNEL);
    if (graph->audio_device_started)
        (void)graph->api->ai_disable(graph->api_context, RK_AI_DEVICE);
    graph->audio_resample_started = false;
    graph->audio_channel_started = false;
    graph->audio_device_started = false;
}

static int start_audio_hardware(struct ngcd_rk_graph *graph)
{
    struct ngcd_rk_aio_attr attribute;
    uint32_t channel_parameter[2] = {8U, 0U};
    char card_id[64];
    char card[88];
    char card_path[32];
    FILE *card_file;
    size_t card_id_length;
    int length;
    if (graph->audio_device_started && graph->audio_channel_started &&
        graph->audio_resample_started)
        return 0;
    if (graph->audio_device_started || graph->audio_channel_started ||
        graph->audio_resample_started)
        return -1;
    if (graph->audio_input != 2) {
        memcpy(card_id, "rockchipwm8904", sizeof("rockchipwm8904"));
        card_id_length = sizeof("rockchipwm8904") - 1U;
    } else {
        length = snprintf(card_path, sizeof(card_path),
                          "/proc/asound/card1/id");
        if (length <= 0 || (size_t)length >= sizeof(card_path))
            return -1;
        card_file = fopen(card_path, "r");
        if (card_file == NULL ||
            fgets(card_id, sizeof(card_id), card_file) == NULL) {
            if (card_file != NULL)
                (void)fclose(card_file);
            return -1;
        }
        if (fclose(card_file) != 0)
            return -1;
        card_id_length = strlen(card_id);
        while (card_id_length > 0U &&
               (card_id[card_id_length - 1U] == '\n' ||
                card_id[card_id_length - 1U] == '\r'))
            card_id[--card_id_length] = '\0';
    }
    length = snprintf(card, sizeof(card), "default:CARD=%s", card_id);
    if (card_id_length == 0U || length <= 0 ||
        (size_t)length >= sizeof(card))
        return -1;
    memset(&attribute, 0, sizeof(attribute));
    put_u32(attribute.bytes, 0U, RK_AUDIO_CHANNELS);
    put_u32(attribute.bytes, 4U, RK_AUDIO_SAMPLE_RATE);
    put_u32(attribute.bytes, 8U, 1U);
    put_u32(attribute.bytes, 12U, RK_AUDIO_SAMPLE_RATE);
    put_u32(attribute.bytes, 16U, 1U);
    put_u32(attribute.bytes, 20U, 1U);
    put_u32(attribute.bytes, 24U, 4U);
    /* Closing and syncing an 8K MP4 can briefly stall the service loop.  Keep
     * enough capture buffers to bridge that split-finalization interval. */
    put_u32(attribute.bytes, 28U, 32U);
    put_u32(attribute.bytes, 32U, 1024U);
    put_u32(attribute.bytes, 36U, RK_AUDIO_CHANNELS);
    memcpy(attribute.bytes + 40U, card, (size_t)length + 1U);
    if (graph->api->ai_set_pub_attr(graph->api_context, RK_AI_DEVICE,
                                    &attribute) != 0 ||
        graph->api->ai_enable(graph->api_context, RK_AI_DEVICE) != 0)
        goto fail;
    graph->audio_device_started = true;
    if (graph->api->ai_enable_channel(
            graph->api_context, RK_AI_DEVICE, RK_AI_CHANNEL) != 0)
        goto fail;
    graph->audio_channel_started = true;
    if (graph->api->ai_set_channel_param(
            graph->api_context, RK_AI_DEVICE, RK_AI_CHANNEL,
            channel_parameter) != 0)
        goto fail;
    if (graph->api->ai_enable_resample(
            graph->api_context, RK_AI_DEVICE, RK_AI_CHANNEL,
            RK_AUDIO_SAMPLE_RATE) != 0)
        goto fail;
    graph->audio_resample_started = true;
    return 0;
fail:
    stop_audio_hardware(graph);
    return -1;
}

int ngcd_rk_graph_set_audio_input(struct ngcd_rk_graph *graph, int input)
{
    if (graph == NULL || input < 0 || input > 2 ||
        graph->audio_device_started || graph->audio_channel_started ||
        graph->audio_resample_started)
        return -1;
    graph->audio_input = input;
    return 0;
}

static int start_record_hardware(
    struct ngcd_rk_graph *graph, const struct ngcd_encoder_state *encoder)
{
    struct ngcd_rk_venc_chn_attr attribute;
    struct ngcd_rk_venc_rc_param rate_control;
    struct ngcd_rk_channel source = {RK_MODULE_AVS, 0, 0};
    struct ngcd_rk_channel destination = {RK_MODULE_VENC, 0, 0};
    unsigned char vui[NGCD_RK_VENC_VUI_SIZE];
    int32_t receive_count = -1;

    if (graph->record_channel_started || graph->record_receiving ||
        graph->record_bound) {
        if (graph->record_channel_started && graph->record_receiving &&
            graph->record_bound &&
            memcmp(&graph->record_encoder, encoder, sizeof(*encoder)) == 0)
            return 0;
        return -1;
    }
    if (ngcd_rk_encoder_attributes(encoder, &attribute, &rate_control) != 0 ||
        graph->api->venc_create_channel(graph->api_context, 0,
                                         &attribute) != 0)
        return -1;
    graph->record_channel_started = true;
    if (graph->api->venc_set_rc_param(graph->api_context, 0,
                                      &rate_control) != 0)
        goto fail;
    memset(vui, 0, sizeof(vui));
    if (strcmp(encoder->codec, "H264") == 0) {
        if (graph->api->venc_get_h264_vui(graph->api_context, 0, vui) != 0)
            goto fail;
        vui[22] = encoder->color_range == 0 ? 1U : 0U;
        if (graph->api->venc_set_h264_vui(graph->api_context, 0, vui) != 0)
            goto fail;
    } else {
        if (graph->api->venc_get_h265_vui(graph->api_context, 0, vui) != 0)
            goto fail;
        vui[26] = encoder->color_range == 0 ? 1U : 0U;
        if (graph->api->venc_set_h265_vui(graph->api_context, 0, vui) != 0)
            goto fail;
    }
    if (graph->api->venc_start_receive(graph->api_context, 0,
                                        &receive_count) != 0)
        goto fail;
    graph->record_receiving = true;
    if (graph->api->bind(graph->api_context, &source, &destination) != 0)
        goto fail;
    graph->record_bound = true;
    graph->record_encoder = *encoder;
    return 0;
fail:
    stop_record_hardware(graph);
    return -1;
}

int ngcd_rk_graph_activate_encoder(
    struct ngcd_rk_graph *graph, const struct ngcd_encoder_state *encoder)
{
    if (graph == NULL || graph->api == NULL || encoder == NULL ||
        (strcmp(encoder->codec, "H264") != 0 &&
         strcmp(encoder->codec, "H265") != 0) ||
        graph->validation_pending ||
        graph->validation_started || !graph->avs_group_started ||
        !graph->avs_channel_started)
        return -1;
    return start_record_hardware(graph, encoder);
}

void ngcd_rk_graph_record_abort(struct ngcd_rk_graph *graph)
{
    if (graph == NULL || graph->api == NULL)
        return;
    if (graph->record_writer != NULL)
        ngcd_mp4_abort(graph->record_writer);
    graph->record_writer = NULL;
    free(graph->record_buffer);
    graph->record_buffer = NULL;
    graph->record_buffer_capacity = 0U;
    graph->recording = false;
    graph->record_wait_keyframe = false;
}

static bool access_unit_is_keyframe(const unsigned char *data, size_t size,
                                    bool h265)
{
    size_t index;
    for (index = 0U; index + 4U < size; ++index) {
        size_t nal;
        if (data[index] != 0U || data[index + 1U] != 0U)
            continue;
        if (data[index + 2U] == 1U)
            nal = index + 3U;
        else if (index + 4U < size && data[index + 2U] == 0U &&
                 data[index + 3U] == 1U)
            nal = index + 4U;
        else
            continue;
        if ((!h265 && (data[nal] & 0x1fU) == 5U) ||
            (h265 && ((data[nal] >> 1U) & 0x3fU) >= 16U &&
             ((data[nal] >> 1U) & 0x3fU) <= 23U))
            return true;
    }
    return false;
}

int ngcd_rk_graph_record_start(
    struct ngcd_rk_graph *graph, const struct ngcd_encoder_state *encoder,
    const char *temporary_path)
{
    if (graph == NULL || graph->api == NULL || encoder == NULL ||
        temporary_path == NULL ||
        (strcmp(encoder->codec, "H264") != 0 &&
         strcmp(encoder->codec, "H265") != 0) ||
        graph->recording || graph->validation_pending ||
        !graph->avs_group_started || !graph->avs_channel_started ||
        ngcd_rk_graph_activate_encoder(graph, encoder) != 0 ||
        start_audio_hardware(graph) != 0 ||
        drain_record_stream(graph, 256U) != 0 ||
        drain_audio(graph, false, 64U) != 0)
        return -1;
    graph->recording_failed = false;
    if ((strcmp(encoder->codec, "H265") == 0
             ? ngcd_mp4_open_h265(&graph->record_writer, temporary_path,
                                  (unsigned int)encoder->width,
                                  (unsigned int)encoder->height,
                                  (unsigned int)encoder->fps)
             : ngcd_mp4_open(&graph->record_writer, temporary_path,
                             (unsigned int)encoder->width,
                             (unsigned int)encoder->height,
                             (unsigned int)encoder->fps)) != 0)
        goto fail;
    /* Some Rockit revisions reject an IDR request while an already-running
     * channel still has queued output.  Waiting for the next natural GOP is
     * safe, so forced IDR is an optimization rather than a start condition. */
    (void)graph->api->venc_request_idr(graph->api_context, 0, true);
    graph->record_wait_keyframe = true;
    graph->recording = true;
    return 0;
fail:
    ngcd_rk_graph_record_abort(graph);
    return -1;
}

int ngcd_rk_graph_record_stop(struct ngcd_rk_graph *graph)
{
    struct ngcd_mp4_writer *writer;
    int result;
    if (graph == NULL || graph->api == NULL || !graph->recording ||
        graph->record_writer == NULL)
        return -1;
    if (drain_audio(graph, true, 64U) != 0)
        return -1;
    writer = graph->record_writer;
    graph->record_writer = NULL;
    graph->recording = false;
    graph->record_wait_keyframe = false;
    free(graph->record_buffer);
    graph->record_buffer = NULL;
    graph->record_buffer_capacity = 0U;
    result = ngcd_mp4_close(writer);
    if (result != 0)
        graph->recording_failed = true;
    return result;
}

int ngcd_rk_graph_capture_raw(struct ngcd_rk_graph *graph, int count,
                              const char *capture_directory,
                              char output_directory[2][128])
{
    struct raw_capture_start start;
    struct raw_capture_request request[2];
    pthread_t thread[2];
    unsigned int created = 0U;
    unsigned int index;
    int stack_result = 0;
    int result = -1;

    if (graph == NULL || graph->api == NULL ||
        graph->api->aiq_capture_raw == NULL || graph->sensor_count != 2U ||
        graph->sensor_handle[0] == NULL || graph->sensor_handle[1] == NULL ||
        count <= 0 || count > 32 || capture_directory == NULL ||
        output_directory == NULL)
        return -1;
    memset(output_directory, 0, 2U * 128U);
    memset(&start, 0, sizeof(start));
    memset(request, 0, sizeof(request));
    if (pthread_mutex_init(&start.mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&start.condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&start.mutex);
        return -1;
    }
    for (index = 0U; index < 2U; ++index) {
        request[index].start = &start;
        request[index].api = graph->api;
        request[index].api_context = graph->api_context;
        request[index].sensor_context = graph->sensor_handle[index];
        request[index].count = count;
        request[index].capture_directory = capture_directory;
        request[index].output_directory = output_directory[index];
        request[index].result = -1;
        if (pthread_create(&thread[index], NULL, capture_raw_thread,
                           &request[index]) != 0)
            break;
        ++created;
    }
    pthread_mutex_lock(&start.mutex);
    if (created == 2U) {
        while (start.ready < 2U)
            pthread_cond_wait(&start.condition, &start.mutex);
        pthread_mutex_unlock(&start.mutex);
        if (night_stack_count_valid((unsigned int)count))
            drain_avs_queue(graph);
        pthread_mutex_lock(&start.mutex);
        start.released = true;
        pthread_cond_broadcast(&start.condition);
    } else {
        start.released = true;
        pthread_cond_broadcast(&start.condition);
    }
    pthread_mutex_unlock(&start.mutex);
    if (created == 2U && night_stack_count_valid((unsigned int)count))
        stack_result = stack_avs_frames(graph, (unsigned int)count);
    for (index = 0U; index < created; ++index)
        (void)pthread_join(thread[index], NULL);
    /* This firmware's AIQ capture worker reliably returns status and clears
     * its per-camera count files, but some builds leave the documented
     * output-directory buffers empty even after publishing both RAW dirs.
     * The snapshot coordinator discovers and validates those dirs itself. */
    if (created == 2U && request[0].result == 0 &&
        request[1].result == 0 && stack_result == 0)
        result = 0;
    (void)pthread_cond_destroy(&start.condition);
    (void)pthread_mutex_destroy(&start.mutex);
    if (result == 0)
        fprintf(stderr, "ngcd: captured %d synchronized RAW frame%s\n",
                count, count == 1 ? "" : "s");
    else
        fprintf(stderr,
                "ngcd: synchronized RAW capture failed (%d, %d, stack %d)\n",
                request[0].result, request[1].result, stack_result);
    return result;
}

int ngcd_rk_graph_read_white_balance(
    const struct ngcd_rk_graph *graph, struct ngcd_rk_aiq_wb_gain gain[2])
{
    unsigned int index;
    if (graph == NULL || graph->api == NULL || gain == NULL ||
        graph->sensor_count != 2U ||
        graph->api->aiq_get_white_balance_gain == NULL)
        return -1;
    for (index = 0U; index < 2U; ++index) {
        if (graph->sensor_handle[index] == NULL ||
            graph->api->aiq_get_white_balance_gain(
                graph->api_context, graph->sensor_handle[index],
                &gain[index]) != 0)
            return -1;
    }
    return 0;
}

int ngcd_rk_graph_offline_enqueue(
    struct ngcd_rk_graph *graph, void *const raw_data[2], bool discard_output)
{
    struct raw_capture_start start;
    struct offline_enqueue_request request[2];
    pthread_t thread[2];
    unsigned int created = 0U;
    unsigned int index;
    int result = -1;
    if (graph == NULL || graph->api == NULL || raw_data == NULL ||
        graph->sensor_count != 2U ||
        graph->api->offline_sensor_enqueue == NULL)
        return -1;
    memset(&start, 0, sizeof(start));
    memset(request, 0, sizeof(request));
    if (pthread_mutex_init(&start.mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&start.condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&start.mutex);
        return -1;
    }
    for (index = 0U; index < 2U; ++index) {
        if (graph->sensor_handle[index] == NULL || raw_data[index] == NULL)
            break;
        request[index].start = &start;
        request[index].api = graph->api;
        request[index].api_context = graph->api_context;
        request[index].sensor_context = graph->sensor_handle[index];
        request[index].raw_data = raw_data[index];
        request[index].result = -1;
        if (pthread_create(&thread[index], NULL, offline_enqueue_thread,
                           &request[index]) != 0)
            break;
        ++created;
    }
    pthread_mutex_lock(&start.mutex);
    if (created == 2U) {
        while (start.ready < 2U)
            pthread_cond_wait(&start.condition, &start.mutex);
    }
    start.released = true;
    pthread_cond_broadcast(&start.condition);
    pthread_mutex_unlock(&start.mutex);
    for (index = 0U; index < created; ++index)
        (void)pthread_join(thread[index], NULL);
    if (created == 2U && request[0].result == 0 && request[1].result == 0) {
        if (!discard_output) {
            result = 0;
        } else if (graph->api->wait_output(
                       graph->api_context, true, 0, 0, 10000) == 0) {
            /* A non-blocking priming frame may also have survived AVS's
             * asynchronous mesh initialization.  Remove every completed
             * warm-up so the following snapshot cannot pick an older pass. */
            drain_avs_queue(graph);
            result = 0;
        }
    }
    (void)pthread_cond_destroy(&start.condition);
    (void)pthread_mutex_destroy(&start.mutex);
    return result;
}

int ngcd_rk_graph_stack_snapshot(struct ngcd_rk_graph *graph, int count)
{
    int result;
    if (graph == NULL || count <= 0 ||
        !night_stack_count_valid((unsigned int)count))
        return -1;
    drain_avs_queue(graph);
    result = stack_avs_frames(graph, (unsigned int)count);
    if (result == 0)
        fprintf(stderr, "ngcd: stacked %d in-memory snapshot frames\n", count);
    else
        fprintf(stderr, "ngcd: in-memory %d-frame snapshot stack failed\n",
                count);
    return result;
}

int ngcd_rk_graph_record_size(struct ngcd_rk_graph *graph, uint64_t *bytes)
{
    if (graph == NULL || !graph->recording || graph->record_writer == NULL)
        return -1;
    return ngcd_mp4_current_size(graph->record_writer, bytes);
}

int ngcd_rk_graph_record_duration(struct ngcd_rk_graph *graph,
                                  uint64_t *microseconds)
{
    if (graph == NULL || !graph->recording || graph->record_writer == NULL)
        return -1;
    return ngcd_mp4_duration(graph->record_writer, microseconds);
}

int ngcd_rk_graph_record_camm_gyro(
    struct ngcd_rk_graph *graph, uint64_t monotonic_ns,
    float x_radians_per_second, float y_radians_per_second,
    float z_radians_per_second)
{
    if (graph == NULL || !graph->recording || graph->record_writer == NULL)
        return -1;
    return ngcd_mp4_write_camm_gyro(
        graph->record_writer, monotonic_ns / UINT64_C(1000),
        x_radians_per_second, y_radians_per_second,
        z_radians_per_second);
}

static int record_tick(struct ngcd_rk_graph *graph)
{
    _Alignas(16) unsigned char
        pack[NGCD_RK_VENC_PACK_SIZE * NGCD_RK_VENC_PACK_COUNT];
    _Alignas(16) unsigned char stream[NGCD_RK_VENC_STREAM_SIZE];
    uint32_t pack_count;
    uint64_t pts = 0U;
    size_t total = 0U;
    size_t index;
    int result;
    memset(pack, 0, sizeof(pack));
    memset(stream, 0, sizeof(stream));
    put_pointer(stream, 0U, pack);
    result = graph->api->venc_get_stream(graph->api_context, 0, stream, 0);
    if (result != 0)
        return 0;
    memcpy(&pack_count, stream + 16U, sizeof(pack_count));
    if (pack_count == 0U || pack_count > NGCD_RK_VENC_PACK_COUNT)
        goto fail;
    for (index = 0U; index < pack_count; ++index) {
        const unsigned char *descriptor =
            pack + index * NGCD_RK_VENC_PACK_SIZE;
        uint32_t size;
        memcpy(&size, descriptor + 8U, sizeof(size));
        if (size == 0U || total > SIZE_MAX - size)
            goto fail;
        if (index == 0U)
            memcpy(&pts, descriptor + 16U, sizeof(pts));
        total += size;
    }
    if (total > graph->record_buffer_capacity) {
        unsigned char *resized = realloc(graph->record_buffer, total);
        if (resized == NULL)
            goto fail;
        graph->record_buffer = resized;
        graph->record_buffer_capacity = total;
    }
    total = 0U;
    for (index = 0U; index < pack_count; ++index) {
        const unsigned char *descriptor =
            pack + index * NGCD_RK_VENC_PACK_SIZE;
        void *handle;
        void *address;
        uint32_t size;
        memcpy(&handle, descriptor, sizeof(handle));
        memcpy(&size, descriptor + 8U, sizeof(size));
        address = graph->api->mb_handle_to_address(graph->api_context,
                                                    handle);
        if (address == NULL)
            goto fail;
        memcpy(graph->record_buffer + total, address, size);
        total += size;
    }
    if (graph->record_wait_keyframe &&
        !access_unit_is_keyframe(
            graph->record_buffer, total,
            strcmp(graph->record_encoder.codec, "H265") == 0)) {
        result = 0;
    } else {
        graph->record_wait_keyframe = false;
        result = strcmp(graph->record_encoder.codec, "H265") == 0
                     ? ngcd_mp4_write_h265(graph->record_writer,
                                           graph->record_buffer, total, pts)
                     : ngcd_mp4_write_h264(graph->record_writer,
                                           graph->record_buffer, total, pts);
    }
    if (graph->api->venc_release_stream(graph->api_context, 0, stream) != 0 ||
        result != 0)
        goto abort_without_release;
    return 0;
fail:
    (void)graph->api->venc_release_stream(graph->api_context, 0, stream);
abort_without_release:
    graph->recording_failed = true;
    ngcd_rk_graph_record_abort(graph);
    return -1;
}

static int audio_tick(struct ngcd_rk_graph *graph, bool write_sample)
{
    struct ngcd_rk_audio_frame frame;
    void *handle;
    void *address;
    uint64_t pts_us;
    uint32_t bit_width;
    uint32_t sound_mode;
    uint32_t data_size;
    uint32_t sample_rate;
    size_t block_size;
    size_t total;
    int result;
    memset(&frame, 0, sizeof(frame));
    result = graph->api->ai_get_frame(
        graph->api_context, RK_AI_DEVICE, RK_AI_CHANNEL, &frame, NULL, 0);
    if (result != 0)
        return 0;
    memcpy(&handle, frame.bytes, sizeof(handle));
    memcpy(&bit_width, frame.bytes + 8U, sizeof(bit_width));
    memcpy(&sound_mode, frame.bytes + 12U, sizeof(sound_mode));
    memcpy(&pts_us, frame.bytes + 16U, sizeof(pts_us));
    memcpy(&data_size, frame.bytes + 28U, sizeof(data_size));
    memcpy(&sample_rate, frame.bytes + 36U, sizeof(sample_rate));
    /* This Rockit revision uses u32Len for the complete interleaved buffer,
     * despite newer public headers describing it as a per-channel length. */
    total = data_size;
    address = graph->api->mb_handle_to_address(graph->api_context, handle);
    block_size = handle != NULL
                     ? graph->api->mb_get_size(graph->api_context, handle)
                     : 0U;
    result = 0;
    if (handle == NULL || address == NULL || bit_width != 1U ||
        sound_mode != 1U ||
        (sample_rate != 0U && sample_rate != RK_AUDIO_SAMPLE_RATE) ||
        data_size == 0U || total % (RK_AUDIO_CHANNELS * 2U) != 0U ||
        total > block_size ||
        (write_sample &&
         ngcd_mp4_write_pcm_s16le(
             graph->record_writer, address, total, pts_us,
             RK_AUDIO_CHANNELS, RK_AUDIO_SAMPLE_RATE) != 0))
        result = -1;
    if (result != 0)
        fprintf(stderr,
                "ngcd: audio frame rejected: handle=%p address=%p "
                "bit_width=%u sound_mode=%u length=%u block=%zu rate=%u\n",
                handle, address, bit_width, sound_mode, data_size,
                block_size, sample_rate);
    if (graph->api->ai_release_frame(
            graph->api_context, RK_AI_DEVICE, RK_AI_CHANNEL, &frame,
            NULL) != 0)
        result = -1;
    if (result != 0 && write_sample) {
        graph->recording_failed = true;
        ngcd_rk_graph_record_abort(graph);
    }
    return result == 0 ? 1 : -1;
}

static int drain_audio(struct ngcd_rk_graph *graph, bool write_sample,
                       unsigned int limit)
{
    unsigned int count;
    for (count = 0U; count < limit; ++count) {
        int result = audio_tick(graph, write_sample);
        if (result <= 0)
            return result;
    }
    return 0;
}

static int discard_record_stream(struct ngcd_rk_graph *graph)
{
    _Alignas(16) unsigned char
        pack[NGCD_RK_VENC_PACK_SIZE * NGCD_RK_VENC_PACK_COUNT];
    _Alignas(16) unsigned char stream[NGCD_RK_VENC_STREAM_SIZE];
    int result;

    memset(pack, 0, sizeof(pack));
    memset(stream, 0, sizeof(stream));
    put_pointer(stream, 0U, pack);
    result = graph->api->venc_get_stream(graph->api_context, 0, stream, 0);
    if (result != 0)
        return 0;
    if (graph->api->venc_release_stream(graph->api_context, 0, stream) != 0) {
        stop_record_hardware(graph);
        return -1;
    }
    return 1;
}

static int drain_record_stream(struct ngcd_rk_graph *graph,
                               unsigned int limit)
{
    unsigned int count;
    for (count = 0U; count < limit; ++count) {
        int result = discard_record_stream(graph);
        if (result <= 0)
            return result;
    }
    return 0;
}

int ngcd_rk_graph_validate_encoder(
    struct ngcd_rk_graph *graph,
    const struct ngcd_encoder_state *encoder)
{
    struct ngcd_rk_venc_chn_attr attribute;
    struct ngcd_rk_venc_rc_param rate_control;
    if (graph == NULL || graph->api == NULL || encoder == NULL ||
        !graph->avs_group_started || !graph->avs_channel_started ||
        graph->recording ||
        ngcd_rk_encoder_attributes(encoder, &attribute, &rate_control) != 0)
        return -1;
    /* Channel zero may be kept warm between captures.  An encoder update owns
     * the same hardware channel, so retire the idle instance before starting
     * the transactional first-frame validation. */
    stop_record_hardware(graph);
    stop_validation_encoder(graph);
    graph->validation_encoder = *encoder;
    graph->validation_poll_count = 0U;
    graph->validation_complete = false;
    graph->validation_failed = false;
    graph->validation_pending = true;
    return 0;
}

int ngcd_rk_graph_tick(struct ngcd_rk_graph *graph)
{
    _Alignas(16) unsigned char
        pack[NGCD_RK_VENC_PACK_SIZE * NGCD_RK_VENC_PACK_COUNT];
    _Alignas(16) unsigned char stream[NGCD_RK_VENC_STREAM_SIZE];
    uint32_t pack_count;
    uint32_t packet_size;
    int result;

    if (graph == NULL || graph->api == NULL)
        return 0;
    if (graph->recording) {
        if (record_tick(graph) != 0)
            return -1;
        return graph->recording
                   ? drain_audio(graph, !graph->record_wait_keyframe, 8U)
                   : -1;
    }
    if (!graph->validation_pending) {
        if (graph->record_channel_started &&
            drain_record_stream(graph, 8U) != 0)
            return -1;
        if (graph->audio_device_started)
            return drain_audio(graph, false, 8U);
        return 0;
    }
    if (!graph->validation_started && start_validation_encoder(graph) != 0)
        return -1;
    memset(pack, 0, sizeof(pack));
    memset(stream, 0, sizeof(stream));
    put_pointer(stream, 0U, pack);
    result = graph->api->venc_get_stream(graph->api_context, 0, stream, 0);
    if (result != 0) {
        ++graph->validation_poll_count;
        if (graph->validation_poll_count >= VENC_VALIDATION_POLLS)
            return fail_validation_encoder(graph, "first-frame timeout");
        return 0;
    }
    memcpy(&pack_count, stream + 16U, sizeof(pack_count));
    memcpy(&packet_size, pack + 8U, sizeof(packet_size));
    result = graph->api->venc_release_stream(graph->api_context, 0, stream);
    if (result != 0)
        return fail_validation_encoder(graph, "stream release");
    if (pack_count == 0U || packet_size == 0U) {
        ++graph->validation_poll_count;
        if (graph->validation_poll_count >= VENC_VALIDATION_POLLS)
            return fail_validation_encoder(graph, "empty stream");
        return 0;
    }
    stop_validation_encoder(graph);
    graph->validation_pending = false;
    graph->validation_complete = true;
    fprintf(stderr, "ngcd: stitched encoder produced its first packet (%u "
                    "bytes)\n", packet_size);
    return 0;
}

static int start_vi(struct ngcd_rk_graph *graph, int device, int channel,
                    int width, int height, int fps, bool compressed)
{
    unsigned char device_attribute[NGCD_RK_VI_DEV_ATTR_SIZE];
    unsigned char pipe_binding[NGCD_RK_VI_BIND_PIPE_SIZE];
    unsigned char channel_attribute[NGCD_RK_VI_CHN_ATTR_SIZE];
    const struct ngcd_rk_api *api = graph->api;
    unsigned int device_bit = 1U << (unsigned int)device;
    unsigned int channel_bit =
        1U << (unsigned int)(device * 4 + channel);
    int result;

    memset(device_attribute, 0, sizeof(device_attribute));
    result = api->vi_get_dev_attr(graph->api_context, device,
                                  device_attribute);
    if ((uint32_t)result == RK_VI_NOT_CONFIG) {
        if (api->vi_set_dev_attr(graph->api_context, device,
                                 device_attribute) != 0)
            return -1;
    } else if (result != 0) {
        return -1;
    }

    if (api->vi_get_dev_enabled(graph->api_context, device) != 0) {
        if (api->vi_enable_dev(graph->api_context, device) != 0)
            return -1;
        graph->vi_device_mask |= device_bit;
        memset(pipe_binding, 0, sizeof(pipe_binding));
        put_u32(pipe_binding, 0, 1);
        put_u32(pipe_binding, 4, (uint32_t)device);
        if (api->vi_bind_pipe(graph->api_context, device, pipe_binding) != 0)
            return -1;
    }

    memset(channel_attribute, 0, sizeof(channel_attribute));
    put_u32(channel_attribute, 0, (uint32_t)width);
    put_u32(channel_attribute, 4, (uint32_t)height);
    put_u32(channel_attribute, 20, compressed ? 1U : 0U);
    put_u32(channel_attribute, 32, compressed ? 0U : 2U);
    put_u32(channel_attribute, 36, (uint32_t)fps);
    put_u32(channel_attribute, 40, (uint32_t)fps);
    put_u32(channel_attribute, 48, fps > 100 ? 12U : 8U);
    put_u32(channel_attribute, 60, compressed ? 4U : 1U);
    if (api->vi_set_channel_attr(graph->api_context, device, channel,
                                 channel_attribute) != 0 ||
        api->vi_enable_channel(graph->api_context, device, channel) != 0)
        return -1;
    graph->vi_channel_mask |= channel_bit;
    return 0;
}

static int start_vpss_channel(struct ngcd_rk_graph *graph, int group,
                              int channel, int width, int height, int fps,
                              bool compressed)
{
    unsigned char attribute[NGCD_RK_VPSS_CHN_ATTR_SIZE];
    unsigned int bit = 1U << (unsigned int)(group * 8 + channel);
    const struct ngcd_rk_api *api = graph->api;
    memset(attribute, 0, sizeof(attribute));
    put_u32(attribute, 0, 2); /* pass-through */
    put_u32(attribute, 4, (uint32_t)width);
    put_u32(attribute, 8, (uint32_t)height);
    put_u32(attribute, 24, compressed ? 1U : 0U);
    put_u32(attribute, 28, (uint32_t)fps);
    put_u32(attribute, 32, (uint32_t)fps);
    put_u32(attribute, 44, 3); /* Rockit's verified queue depth */
    if (api->vpss_set_channel_attr(graph->api_context, group, channel,
                                   attribute) != 0 ||
        api->vpss_enable_channel(graph->api_context, group, channel) != 0)
        return -1;
    graph->vpss_channel_mask |= bit;
    return 0;
}

static int start_vpss(struct ngcd_rk_graph *graph, int group, int width,
                      int height, int fps, bool compressed)
{
    unsigned char group_attribute[NGCD_RK_VPSS_GRP_ATTR_SIZE];
    const struct ngcd_rk_api *api = graph->api;
    unsigned int bit = 1U << (unsigned int)group;

    memset(group_attribute, 0, sizeof(group_attribute));
    put_u32(group_attribute, 0, (uint32_t)width);
    put_u32(group_attribute, 4, (uint32_t)height);
    put_u32(group_attribute, 16, (uint32_t)fps);
    put_u32(group_attribute, 20, (uint32_t)fps);
    put_u32(group_attribute, 24, compressed ? 1U : 0U);
    if (api->vpss_create_group(graph->api_context, group, group_attribute) != 0)
        return -1;
    graph->vpss_group_mask |= bit;
    if (api->vpss_set_device(graph->api_context, group, 1) != 0 ||
        api->vpss_enable_backup(graph->api_context, group) != 0 ||
        api->vpss_start_group(graph->api_context, group) != 0)
        return -1;

    return start_vpss_channel(graph, group, 0, width, height, fps,
                              compressed);
}

static int bind_channels(struct ngcd_rk_graph *graph,
                         int source_module, int source_device,
                         int source_channel, int destination_module,
                         int destination_device, int destination_channel)
{
    struct ngcd_rk_channel source;
    struct ngcd_rk_channel destination;
    source.module = source_module;
    source.device = source_device;
    source.channel = source_channel;
    destination.module = destination_module;
    destination.device = destination_device;
    destination.channel = destination_channel;
    return graph->api->bind(graph->api_context, &source, &destination);
}

static int start_avs(struct ngcd_rk_graph *graph,
                     const struct ngcd_profile *profile, int fps)
{
    unsigned char group_attribute[NGCD_RK_AVS_GRP_ATTR_SIZE];
    unsigned char channel_attribute[NGCD_RK_AVS_CHN_ATTR_SIZE];
    const struct ngcd_rk_api *api = graph->api;
    int count;

    if (profile->stitch.width <= 0 || profile->stitch.height <= 0)
        return -1;
    memset(group_attribute, 0, sizeof(group_attribute));
    if (strcmp(profile->stitch_mode, "VR180") == 0) {
        count = snprintf(graph->avs_project, sizeof(graph->avs_project),
                         "/local/vr180/vr180_%dx%d.pto",
                         profile->stitch.width, profile->stitch.height);
        if (count < 0 || (size_t)count >= sizeof(graph->avs_project))
            return -1;
        count = snprintf(graph->avs_working_directory,
                         sizeof(graph->avs_working_directory),
                         "/tmp/vr180_%dx%d/", profile->stitch.width,
                         profile->stitch.height);
        if (count < 0 ||
            (size_t)count >= sizeof(graph->avs_working_directory))
            return -1;
        if (api->prepare_directory(graph->api_context,
                                   graph->avs_working_directory) != 0)
            return -1;
        put_u32(group_attribute, 0, 0); /* blended calibration */
        put_u32(group_attribute, 8, 1); /* synchronized pipes */
        put_u32(group_attribute, 16, 1);
        put_pointer(group_attribute, 24, graph->avs_project);
        put_pointer(group_attribute, 32, graph->avs_working_directory);
        put_u32(group_attribute, 212,
                (uint32_t)(profile->stitch.width / 2));
        put_u32(group_attribute, 216,
                (uint32_t)(profile->stitch.height / 2));
        put_u32(group_attribute, 220, (uint32_t)profile->stitch_fov_x);
        put_u32(group_attribute, 224, (uint32_t)profile->stitch_fov_y);
    } else if (strcmp(profile->stitch_mode, "SBS") == 0 ||
               strcmp(profile->stitch_mode, "3D") == 0) {
        put_u32(group_attribute, 0, 1); /* horizontal no-blend */
        put_u32(group_attribute, 8, 1);
    } else {
        return -1;
    }
    put_u32(group_attribute, 4, 2); /* two input pipes */
    put_u32(group_attribute, 376, (uint32_t)fps);
    put_u32(group_attribute, 380, (uint32_t)fps);

    if (api->avs_set_working_set(graph->api_context, 0x200018000U) != 0 ||
        api->avs_create_group(graph->api_context, 0, group_attribute) != 0)
        return -1;
    graph->avs_group_started = true;
    if (api->avs_start_group(graph->api_context, 0) != 0)
        return -1;

    memset(channel_attribute, 0, sizeof(channel_attribute));
    put_u32(channel_attribute, 0, (uint32_t)profile->stitch.width);
    put_u32(channel_attribute, 4, (uint32_t)profile->stitch.height);
    put_u32(channel_attribute, 8, profile->isp_mode == 1 ? 1U : 0U);
    put_u32(channel_attribute, 16, profile->isp_mode == 1 ? 0U : 2U);
    put_u32(channel_attribute, 20, (uint32_t)fps);
    put_u32(channel_attribute, 24, (uint32_t)fps);
    put_u32(channel_attribute, 28, profile->isp_mode == 1 ? 6U : 8U);
    if (api->avs_set_channel_attr(graph->api_context, 0, 0,
                                  channel_attribute) != 0 ||
        api->avs_enable_channel(graph->api_context, 0, 0) != 0)
        return -1;
    graph->avs_channel_started = true;
    return 0;
}

static int validate_preview_api(const struct ngcd_rk_api *api)
{
    return api->vo_bind_layer != NULL && api->vo_unbind_layer != NULL &&
           api->vo_set_layer_buffer_length != NULL &&
           api->vo_set_layer_attr != NULL &&
           api->vo_set_layer_splice_mode != NULL &&
           api->vo_enable_layer != NULL && api->vo_disable_layer != NULL &&
           api->vo_set_channel_attr != NULL &&
           api->vo_enable_channel != NULL &&
           api->vo_disable_channel != NULL;
}

static int start_video_layer(struct ngcd_rk_graph *graph)
{
    unsigned char attribute[NGCD_RK_VO_LAYER_ATTR_SIZE];
    const struct ngcd_rk_api *api = graph->api;
    if (api->vo_bind_layer(graph->api_context, RK_VO_VIDEO_LAYER,
                           RK_VO_DEVICE_LCD, 2) != 0)
        return -1;
    graph->vo_video_layer_bound = true;
    if (api->vo_set_layer_buffer_length(graph->api_context,
                                         RK_VO_VIDEO_LAYER, 4) != 0)
        return -1;
    memset(attribute, 0, sizeof(attribute));
    put_u32(attribute, 8, 480);
    put_u32(attribute, 12, 800);
    put_u32(attribute, 16, 480);
    put_u32(attribute, 20, 800);
    put_u32(attribute, 24, 25);
    put_u32(attribute, 28, 0x10001);
    if (api->vo_set_layer_attr(graph->api_context, RK_VO_VIDEO_LAYER,
                               attribute) != 0 ||
        api->vo_set_layer_splice_mode(graph->api_context,
                                      RK_VO_VIDEO_LAYER, 1) != 0 ||
        api->vo_enable_layer(graph->api_context, RK_VO_VIDEO_LAYER) != 0)
        return -1;
    graph->vo_video_layer_started = true;
    return 0;
}

static int start_video_channel(struct ngcd_rk_graph *graph, int channel,
                               int x, int y, int width, int height)
{
    unsigned char attribute[NGCD_RK_VO_CHN_ATTR_SIZE];
    const struct ngcd_rk_api *api = graph->api;
    memset(attribute, 0, sizeof(attribute));
    put_u32(attribute, 0, RK_VO_VIDEO_LAYER);
    put_u32(attribute, 4, (uint32_t)x);
    put_u32(attribute, 8, (uint32_t)y);
    put_u32(attribute, 12, (uint32_t)width);
    put_u32(attribute, 16, (uint32_t)height);
    put_u32(attribute, 44, 1); /* rotate physical portrait LCD to landscape */
    if (api->vo_set_channel_attr(graph->api_context, RK_VO_VIDEO_LAYER,
                                 channel, attribute) != 0 ||
        api->vo_enable_channel(graph->api_context, RK_VO_VIDEO_LAYER,
                               channel) != 0)
        return -1;
    graph->vo_channel_mask |= 1U << (unsigned int)channel;
    return 0;
}

static int start_preview(struct ngcd_rk_graph *graph,
                         const struct ngcd_profile *profile,
                         const struct ngcd_rk_display *display,
                         unsigned int sensor_count)
{
    unsigned int index;
    bool stitched = is_stitched(profile) != 0;
    if (display == NULL || !display->device_started ||
        !validate_preview_api(graph->api))
        return -1;
    for (index = 0; index < sensor_count; ++index) {
        int width = stitched ? 400 : 800;
        int height = stitched ? 400 : 440;
        if (start_vpss_channel(graph, (int)index, 1, width, height, 25,
                               true) != 0)
            return -1;
    }
    if (start_video_layer(graph) != 0)
        return -1;
    if (stitched) {
        for (index = 0; index < sensor_count; ++index) {
            if (start_video_channel(graph, (int)index, 20,
                                    (int)index * 400, 440, 400) != 0 ||
                bind_channels(graph, RK_MODULE_VPSS, (int)index, 1,
                              RK_MODULE_VO, RK_VO_VIDEO_LAYER,
                              (int)index) != 0)
                return -1;
            graph->vpss_vo_bind_mask |= 1U << index;
        }
    } else {
        if (start_video_channel(graph, 1, 20, 0, 440, 800) != 0 ||
            bind_channels(graph, RK_MODULE_VPSS, 0, 1, RK_MODULE_VO,
                          RK_VO_VIDEO_LAYER, 1) != 0)
            return -1;
        graph->vpss_vo_bind_mask |= 1U << 1;
    }
    return 0;
}

void ngcd_rk_graph_stop(struct ngcd_rk_graph *graph)
{
    const struct ngcd_rk_api *api;
    int index;
    if (graph == NULL || graph->api == NULL)
        return;
    api = graph->api;

    discard_stacked_snapshot(graph);
    stop_snapshot_encoder(graph);
    ngcd_rk_graph_record_abort(graph);
    stop_audio_hardware(graph);
    stop_record_hardware(graph);
    stop_validation_encoder(graph);
    graph->validation_pending = false;
    graph->validation_complete = false;
    graph->validation_failed = false;
    graph->validation_poll_count = 0U;

    for (index = 1; index >= 0; --index) {
        unsigned int bit = 1U << (unsigned int)index;
        if ((graph->vpss_vo_bind_mask & bit) != 0U) {
            int group = graph->sensor_count == 1U ? 0 : index;
            struct ngcd_rk_channel source = {RK_MODULE_VPSS, group, 1};
            struct ngcd_rk_channel destination = {
                RK_MODULE_VO, RK_VO_VIDEO_LAYER, index
            };
            (void)api->unbind(graph->api_context, &source, &destination);
        }
        if ((graph->vo_channel_mask & bit) != 0U)
            (void)api->vo_disable_channel(graph->api_context,
                                          RK_VO_VIDEO_LAYER, index);
    }
    graph->vpss_vo_bind_mask = 0;
    graph->vo_channel_mask = 0;
    if (graph->vo_video_layer_started)
        (void)api->vo_disable_layer(graph->api_context, RK_VO_VIDEO_LAYER);
    if (graph->vo_video_layer_bound)
        (void)api->vo_unbind_layer(graph->api_context, RK_VO_VIDEO_LAYER,
                                   RK_VO_DEVICE_LCD);
    graph->vo_video_layer_started = false;
    graph->vo_video_layer_bound = false;

    for (index = 1; index >= 0; --index) {
        unsigned int bit = 1U << (unsigned int)index;
        if ((graph->vpss_avs_bind_mask & bit) != 0U) {
            struct ngcd_rk_channel source = {RK_MODULE_VPSS, index, 0};
            struct ngcd_rk_channel destination = {RK_MODULE_AVS, 0, index};
            (void)api->unbind(graph->api_context, &source, &destination);
        }
    }
    graph->vpss_avs_bind_mask = 0;
    if (graph->avs_channel_started)
        (void)api->avs_disable_channel(graph->api_context, 0, 0);
    if (graph->avs_group_started) {
        (void)api->avs_stop_group(graph->api_context, 0);
        (void)api->avs_destroy_group(graph->api_context, 0);
    }
    graph->avs_channel_started = false;
    graph->avs_group_started = false;

    for (index = 1; index >= 0; --index) {
        unsigned int bit = 1U << (unsigned int)index;
        int channel;
        if ((graph->vi_vpss_bind_mask & bit) != 0U) {
            int device = graph->vi_device_for_vpss[index];
            struct ngcd_rk_channel source;
            struct ngcd_rk_channel destination = {RK_MODULE_VPSS, index, 0};
            source.module = RK_MODULE_VI;
            source.device = device;
            source.channel = graph->vi_channel_for_vpss[index];
            (void)api->unbind(graph->api_context, &source, &destination);
        }
        for (channel = 7; channel >= 0; --channel) {
            unsigned int channel_bit =
                1U << (unsigned int)(index * 8 + channel);
            if ((graph->vpss_channel_mask & channel_bit) != 0U)
                (void)api->vpss_disable_channel(graph->api_context, index,
                                                channel);
        }
        if ((graph->vpss_group_mask & bit) != 0U) {
            (void)api->vpss_stop_group(graph->api_context, index);
            (void)api->vpss_destroy_group(graph->api_context, index);
        }
    }
    graph->vi_vpss_bind_mask = 0;
    graph->vpss_channel_mask = 0;
    graph->vpss_group_mask = 0;

    for (index = 1; index >= 0; --index) {
        unsigned int bit = 1U << (unsigned int)index;
        int channel;
        for (channel = 3; channel >= 0; --channel) {
            unsigned int channel_bit =
                1U << (unsigned int)(index * 4 + channel);
            if ((graph->vi_channel_mask & channel_bit) != 0U)
                (void)api->vi_disable_channel(graph->api_context, index,
                                              channel);
        }
        if ((graph->vi_device_mask & bit) != 0U)
            (void)api->vi_disable_dev(graph->api_context, index);
        if ((graph->sensor_mask & bit) != 0U)
            api->sensor_stop(graph->api_context, graph->sensor_handle[index]);
    }
    graph->vi_channel_mask = 0;
    graph->vi_device_mask = 0;
    graph->sensor_mask = 0;
    graph->sensor_count = 0;
    if (graph->system_started)
        (void)api->system_exit(graph->api_context);
    graph->system_started = false;
}

static int graph_start(struct ngcd_rk_graph *graph,
                       const struct ngcd_rk_api *api, void *api_context,
                       const struct ngcd_profile *profile,
                       struct ngcd_rk_display *display, bool manage_system)
{
    unsigned int count;
    unsigned int index;
    int base;
    int stitched;
    int compressed;
    int capture_channel;

    if (graph == NULL || profile == NULL || !validate_api(api))
        return -1;
    memset(graph, 0, sizeof(*graph));
    graph->api = api;
    graph->api_context = api_context;
    count = graph_sensor_count(profile);
    base = graph_sensor_base(profile);
    stitched = is_stitched(profile);
    compressed = profile->isp_mode == 1;
    capture_channel = compressed ? 2 : 0;
    if (profile->sensor_count == 0 || profile->capture_count == 0 ||
        count > 2U || base + (int)count > 2)
        return -1;

    if (manage_system) {
        if (api->system_init(api_context) != 0) {
            fprintf(stderr, "ngcd: Rockchip SYS initialization failed\n");
            goto fail;
        }
        graph->system_started = true;
    }
    graph->sensor_count = count;
    /* Rockchip's grouped capture path must see the external clock follower
     * streaming before the internal XVS master.  Starting AIQ in input order
     * leaves the two CIF pipelines in independent timestamp domains. */
    for (index = 0; index < count; ++index) {
        unsigned int input = stitched && count == 2U ? 1U - index : index;
        const struct ngcd_video_geometry *sensor = geometry_at(
            profile->sensor, profile->sensor_count, input);
        const struct ngcd_video_geometry *capture = geometry_at(
            profile->capture, profile->capture_count, input);
        int device = base + (int)input;
        enum ngcd_rk_sensor_sync_mode sync_mode = NGCD_RK_SENSOR_NO_SYNC;
        if (sensor == NULL || capture == NULL) {
            fprintf(stderr, "ngcd: missing sensor geometry for input %u\n",
                    input);
            goto fail;
        }
        if (stitched && count == 2U)
            sync_mode = input == 0U ? NGCD_RK_SENSOR_INTERNAL_MASTER :
                                      NGCD_RK_SENSOR_EXTERNAL_MASTER;
        if (api->sensor_start(api_context, device, sensor->width,
                              sensor->height, sensor->fps, capture->width,
                              capture->height, sync_mode,
                              &graph->sensor_handle[device]) != 0) {
            fprintf(stderr, "ngcd: sensor %d initialization failed\n", device);
            goto fail;
        }
        graph->sensor_mask |= 1U << (unsigned int)device;
    }
    for (index = 0; index < count; ++index) {
        unsigned int input = stitched && count == 2U ? 1U - index : index;
        const struct ngcd_video_geometry *capture = geometry_at(
            profile->capture, profile->capture_count, input);
        int device = base + (int)input;
        graph->vi_device_for_vpss[input] = device;
        graph->vi_channel_for_vpss[input] = capture_channel;
        if (capture == NULL) {
            fprintf(stderr, "ngcd: missing sensor geometry for input %u\n",
                    input);
            goto fail;
        }
        if (start_vi(graph, device, capture_channel, capture->width,
                     capture->height, capture->fps, compressed != 0) != 0) {
            fprintf(stderr, "ngcd: VI initialization failed for input %u\n",
                    input);
            goto fail;
        }
        if (start_vpss(graph, (int)input, capture->width, capture->height,
                       capture->fps, compressed != 0) != 0) {
            fprintf(stderr, "ngcd: VPSS initialization failed for input %u\n",
                    input);
            goto fail;
        }
        if (bind_channels(graph, RK_MODULE_VI, device, capture_channel,
                          RK_MODULE_VPSS, (int)input, 0) != 0) {
            fprintf(stderr, "ngcd: VI-to-VPSS bind failed for input %u\n",
                    input);
            goto fail;
        }
        graph->vi_vpss_bind_mask |= 1U << input;
    }
    /* In stereo sync mode neither side is guaranteed to emit until both VI
     * devices are enabled.  Waiting inside the setup loop deadlocks startup. */
    for (index = 0; index < count; ++index) {
        if (api->wait_output(api_context, false, (int)index, 0, 5000) != 0) {
            fprintf(stderr, "ngcd: VPSS output timed out for input %u\n",
                    index);
            goto fail;
        }
    }

    if (stitched) {
        const struct ngcd_video_geometry *capture = geometry_at(
            profile->capture, profile->capture_count, 0);
        if (count == 2U && api->sensor_synchronize(api_context) != 0) {
            fprintf(stderr, "ngcd: stereo sensor synchronization failed\n");
            goto fail;
        }
        if (capture == NULL || start_avs(graph, profile, capture->fps) != 0) {
            fprintf(stderr, "ngcd: AVS initialization failed\n");
            goto fail;
        }
        for (index = 0; index < count; ++index) {
            if (bind_channels(graph, RK_MODULE_VPSS, (int)index, 0,
                              RK_MODULE_AVS, 0, (int)index) != 0) {
                fprintf(stderr,
                        "ngcd: VPSS-to-AVS bind failed for input %u\n",
                        index);
                goto fail;
            }
            graph->vpss_avs_bind_mask |= 1U << index;
        }
    }
    if (profile->preview && start_preview(graph, profile, display, count) != 0) {
        fprintf(stderr, "ngcd: LCD video preview initialization failed\n");
        goto fail;
    }
    if (stitched) {
        graph->snapshot_width = profile->stitch.width;
        graph->snapshot_height = profile->stitch.height;
    }
    return 0;

fail:
    ngcd_rk_graph_stop(graph);
    return -1;
}

int ngcd_rk_graph_start(struct ngcd_rk_graph *graph,
                        const struct ngcd_rk_api *api, void *api_context,
                        const struct ngcd_profile *profile)
{
    return graph_start(graph, api, api_context, profile, NULL, true);
}

int ngcd_rk_graph_start_in_system(struct ngcd_rk_graph *graph,
                                  const struct ngcd_rk_api *api,
                                  void *api_context,
                                  const struct ngcd_profile *profile,
                                  struct ngcd_rk_display *display)
{
    return graph_start(graph, api, api_context, profile, display, false);
}

int ngcd_rk_graph_start_offline_in_system(
    struct ngcd_rk_graph *graph, const struct ngcd_rk_api *api,
    void *api_context, const struct ngcd_profile *profile,
    const uint32_t format[2],
    const struct ngcd_rk_aiq_wb_gain white_balance[2])
{
    unsigned int input;
    int capture_channel = 0;
    if (graph == NULL || profile == NULL || format == NULL ||
        white_balance == NULL || !validate_api(api) ||
        api->offline_sensor_start == NULL ||
        api->offline_sensor_run == NULL ||
        api->offline_sensor_enqueue == NULL ||
        !is_stitched(profile) || graph_sensor_count(profile) != 2U)
        return -1;
    memset(graph, 0, sizeof(*graph));
    graph->api = api;
    graph->api_context = api_context;
    graph->sensor_count = 2U;
    for (input = 0U; input < 2U; ++input) {
        const struct ngcd_video_geometry *capture = geometry_at(
            profile->capture, profile->capture_count, input);
        if (capture == NULL || capture->width <= 0 || capture->height <= 0 ||
            api->offline_sensor_start(
                api_context, (int)input, capture->width, capture->height,
                format[input], &white_balance[input],
                &graph->sensor_handle[input]) != 0) {
            fprintf(stderr, "ngcd: offline AIQ sensor %u failed\n", input);
            goto fail;
        }
        graph->sensor_mask |= 1U << input;
    }
    if (start_offline_sensors(graph) != 0) {
        fprintf(stderr, "ngcd: synchronized offline AIQ start failed\n");
        goto fail;
    }
    /* Match Rockchip's offline demo ordering: prepare and start FakeCamHw,
     * then start capture on the ISP mainpaths that consume raw readback. */
    for (input = 0U; input < 2U; ++input) {
        const struct ngcd_video_geometry *capture = geometry_at(
            profile->capture, profile->capture_count, input);
        if (capture == NULL)
            goto fail;
        graph->vi_device_for_vpss[input] = (int)input;
        graph->vi_channel_for_vpss[input] = capture_channel;
        if (start_vi(graph, (int)input, capture_channel, capture->width,
                     capture->height, capture->fps, false) != 0 ||
            start_vpss(graph, (int)input, capture->width, capture->height,
                       capture->fps, false) != 0 ||
            bind_channels(graph, RK_MODULE_VI, (int)input, capture_channel,
                          RK_MODULE_VPSS, (int)input, 0) != 0) {
            fprintf(stderr, "ngcd: offline VI/VPSS input %u failed\n", input);
            goto fail;
        }
        graph->vi_vpss_bind_mask |= 1U << input;
    }
    {
        const struct ngcd_video_geometry *capture = geometry_at(
            profile->capture, profile->capture_count, 0U);
        if (capture == NULL || start_avs(graph, profile, capture->fps) != 0)
            goto fail;
    }
    for (input = 0U; input < 2U; ++input) {
        if (bind_channels(graph, RK_MODULE_VPSS, (int)input, 0,
                          RK_MODULE_AVS, 0, (int)input) != 0)
            goto fail;
        graph->vpss_avs_bind_mask |= 1U << input;
    }
    graph->snapshot_width = profile->stitch.width;
    graph->snapshot_height = profile->stitch.height;
    return 0;
fail:
    ngcd_rk_graph_stop(graph);
    return -1;
}
