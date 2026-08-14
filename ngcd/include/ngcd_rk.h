#ifndef CALF_NGCD_RK_H
#define CALF_NGCD_RK_H

#include "ngcd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ngcd_mp4_writer;

/*
 * The Rockit ABI used by the camera predates the public RV1106 headers in a
 * few places.  Keep vendor-owned structures opaque here and construct only
 * the fields verified in the ViewPT 2.2.1 ngcd disassembly.  The sizes below
 * are part of the ABI and are covered by unit tests.
 */
#define NGCD_RK_VI_DEV_ATTR_SIZE 28U
#define NGCD_RK_VI_BIND_PIPE_SIZE 68U
#define NGCD_RK_VI_CHN_ATTR_SIZE 108U
#define NGCD_RK_VPSS_GRP_ATTR_SIZE 28U
#define NGCD_RK_VPSS_CHN_ATTR_SIZE 76U
#define NGCD_RK_AVS_GRP_ATTR_SIZE 384U
#define NGCD_RK_AVS_CHN_ATTR_SIZE 32U
#define NGCD_RK_VO_PUB_ATTR_SIZE 16U
#define NGCD_RK_VO_LAYER_ATTR_SIZE 44U
#define NGCD_RK_VO_CHN_ATTR_SIZE 48U
/* VIDEO_FRAME_INFO_S is 96 bytes in the camera's 2023 Rockit ABI. The first
 * 48 bytes are the nested VIDEO_FRAME_S fields used for direct VO submission;
 * VPSS_GetChnFrame writes the complete 96-byte wrapper. */
#define NGCD_RK_VIDEO_FRAME_SIZE 96U
#define NGCD_RK_VENC_CHN_ATTR_SIZE 120U
#define NGCD_RK_VENC_RC_PARAM_SIZE 32U
#define NGCD_RK_VENC_PACK_SIZE 144U
#define NGCD_RK_VENC_PACK_COUNT 8U
/* Stock venc_module::run_once reserves 0x190 bytes for VENC_STREAM_S. */
#define NGCD_RK_VENC_STREAM_SIZE 400U
#define NGCD_RK_VENC_VUI_SIZE 32U
#define NGCD_RK_VENC_JPEG_PARAM_SIZE 200U
#define NGCD_RK_VDEC_CHN_ATTR_SIZE 56U
#define NGCD_RK_VDEC_CHN_PARAM_SIZE 32U
#define NGCD_RK_VDEC_STREAM_SIZE 40U
#define NGCD_RK_VDEC_STATUS_SIZE 80U
#define NGCD_RK_AIO_ATTR_SIZE 104U
#define NGCD_RK_AUDIO_FRAME_SIZE 40U
#define NGCD_RK_AIQ_ACP_ATTR_SIZE 12U
#define NGCD_RK_AIQ_EXP_SW_ATTR_SIZE 1264U
#define NGCD_RK_AIQ_LIN_EXP_ATTR_SIZE 312U
#define NGCD_RK_AIQ_EXP_QUERY_INFO_SIZE 2472U
#define NGCD_RK_AIQ_EFFECT_ATTR_SIZE 12U

enum ngcd_rk_acp_control {
    NGCD_RK_ACP_BRIGHTNESS,
    NGCD_RK_ACP_CONTRAST,
    NGCD_RK_ACP_SATURATION,
    NGCD_RK_ACP_HUE,
};

enum ngcd_rk_flicker_control {
    NGCD_RK_FLICKER_OFF,
    NGCD_RK_FLICKER_AUTO,
    NGCD_RK_FLICKER_50HZ,
    NGCD_RK_FLICKER_60HZ,
};

struct ngcd_rk_aiq_acp_attr {
    unsigned char bytes[NGCD_RK_AIQ_ACP_ATTR_SIZE];
};

struct ngcd_rk_aiq_exp_sw_attr {
    unsigned char bytes[NGCD_RK_AIQ_EXP_SW_ATTR_SIZE];
};

struct ngcd_rk_aiq_lin_exp_attr {
    unsigned char bytes[NGCD_RK_AIQ_LIN_EXP_ATTR_SIZE];
};

struct ngcd_rk_aiq_exp_query_info {
    unsigned char bytes[NGCD_RK_AIQ_EXP_QUERY_INFO_SIZE];
};

struct ngcd_rk_aiq_effect_attr {
    unsigned char bytes[NGCD_RK_AIQ_EFFECT_ATTR_SIZE];
};

struct ngcd_rk_aiq_wb_gain {
    float red;
    float green_red;
    float green_blue;
    float blue;
};

struct ngcd_rk_raw_prop {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t buffer_type;
};

struct ngcd_rk_venc_chn_attr {
    unsigned char bytes[NGCD_RK_VENC_CHN_ATTR_SIZE];
};

struct ngcd_rk_venc_rc_param {
    unsigned char bytes[NGCD_RK_VENC_RC_PARAM_SIZE];
};

struct ngcd_rk_aio_attr {
    unsigned char bytes[NGCD_RK_AIO_ATTR_SIZE];
};

struct ngcd_rk_audio_frame {
    unsigned char bytes[NGCD_RK_AUDIO_FRAME_SIZE];
};

struct ngcd_rk_exif_metadata {
    char datetime[20];
    uint32_t iso;
    uint32_t exposure_numerator;
    uint32_t exposure_denominator;
};

struct ngcd_rk_image_readback {
    bool exposure_automatic;
    float exposure_seconds;
    bool iso_automatic;
    unsigned int iso;
    bool white_balance_automatic;
    unsigned int white_balance_kelvin;
    int exposure_compensation;
    int brightness;
    int contrast;
    int saturation;
    int hue;
    int sharpness;
    int noise_reduction;
    enum ngcd_rk_flicker_control flicker;
    unsigned int effect;
};

struct ngcd_rk_channel {
    int32_t module;
    int32_t device;
    int32_t channel;
};

enum ngcd_rk_sensor_sync_mode {
    NGCD_RK_SENSOR_NO_SYNC = 0,
    NGCD_RK_SENSOR_EXTERNAL_MASTER = 1,
    NGCD_RK_SENSOR_INTERNAL_MASTER = 2,
};

struct ngcd_rk_api {
    int (*system_init)(void *context);
    int (*system_exit)(void *context);
    int (*bind)(void *context, const struct ngcd_rk_channel *source,
                const struct ngcd_rk_channel *destination);
    int (*unbind)(void *context, const struct ngcd_rk_channel *source,
                  const struct ngcd_rk_channel *destination);

    int (*sensor_start)(void *context, int sensor, int width, int height,
                        int fps, int crop_width, int crop_height,
                        enum ngcd_rk_sensor_sync_mode sync_mode,
                        void **handle);
    int (*offline_sensor_start)(void *context, int sensor, int width,
                                int height, uint32_t format,
                                const struct ngcd_rk_aiq_wb_gain *white_balance,
                                void **handle);
    int (*offline_sensor_run)(void *context, void *handle);
    int (*offline_sensor_enqueue)(void *context, void *handle, void *raw_data);
    void (*sensor_stop)(void *context, void *handle);
    int (*sensor_synchronize)(void *context);
    int (*aiq_get_acp)(void *context, void *sensor_context,
                       struct ngcd_rk_aiq_acp_attr *attribute);
    int (*aiq_set_acp)(void *context, void *sensor_context,
                       const struct ngcd_rk_aiq_acp_attr *attribute);
    int (*aiq_get_sharpness)(void *context, void *sensor_context,
                             unsigned int *strength);
    int (*aiq_set_sharpness)(void *context, void *sensor_context,
                             unsigned int strength);
    int (*aiq_get_anr)(void *context, void *sensor_context,
                       unsigned int *strength);
    int (*aiq_set_anr)(void *context, void *sensor_context,
                       unsigned int strength);
    int (*aiq_get_spatial_nr)(void *context, void *sensor_context,
                              unsigned int *strength);
    int (*aiq_set_spatial_nr)(void *context, void *sensor_context,
                              unsigned int strength);
    int (*aiq_get_temporal_nr)(void *context, void *sensor_context,
                               unsigned int *strength);
    int (*aiq_set_temporal_nr)(void *context, void *sensor_context,
                               unsigned int strength);
    int (*aiq_get_exposure)(void *context, void *sensor_context,
                            struct ngcd_rk_aiq_exp_sw_attr *attribute);
    int (*aiq_set_exposure)(void *context, void *sensor_context,
                            const struct ngcd_rk_aiq_exp_sw_attr *attribute);
    int (*aiq_get_linear_exposure)(
        void *context, void *sensor_context,
        struct ngcd_rk_aiq_lin_exp_attr *attribute);
    int (*aiq_set_linear_exposure)(
        void *context, void *sensor_context,
        const struct ngcd_rk_aiq_lin_exp_attr *attribute);
    int (*aiq_query_exposure)(
        void *context, void *sensor_context,
        struct ngcd_rk_aiq_exp_query_info *information);
    int (*aiq_get_white_balance_mode)(void *context, void *sensor_context,
                                      unsigned int *mode);
    int (*aiq_set_white_balance_mode)(void *context, void *sensor_context,
                                      unsigned int mode);
    int (*aiq_get_white_balance_ct)(void *context, void *sensor_context,
                                    unsigned int *kelvin);
    int (*aiq_set_white_balance_ct)(void *context, void *sensor_context,
                                    unsigned int kelvin);
    int (*aiq_get_white_balance_gain)(
        void *context, void *sensor_context,
        struct ngcd_rk_aiq_wb_gain *gain);
    int (*aiq_get_flicker_enabled)(void *context, void *sensor_context,
                                   unsigned char *enabled);
    int (*aiq_set_flicker_enabled)(void *context, void *sensor_context,
                                   unsigned char enabled);
    int (*aiq_get_flicker_mode)(void *context, void *sensor_context,
                                unsigned int *mode);
    int (*aiq_set_flicker_mode)(void *context, void *sensor_context,
                                unsigned int mode);
    int (*aiq_get_power_line_frequency)(void *context, void *sensor_context,
                                        unsigned int *frequency);
    int (*aiq_set_power_line_frequency)(void *context, void *sensor_context,
                                        unsigned int frequency);
    int (*aiq_get_effect)(void *context, void *sensor_context,
                          struct ngcd_rk_aiq_effect_attr *attribute);
    int (*aiq_set_effect)(void *context, void *sensor_context,
                          const struct ngcd_rk_aiq_effect_attr *attribute);
    int (*aiq_capture_raw)(void *context, void *sensor_context, int count,
                           const char *capture_directory,
                           char *output_directory);
    int (*prepare_directory)(void *context, const char *path);
    int (*wait_output)(void *context, bool stitched, int device,
                       int channel, int timeout_ms);

    int (*vi_get_dev_attr)(void *context, int device, void *attribute);
    int (*vi_set_dev_attr)(void *context, int device, const void *attribute);
    int (*vi_get_dev_enabled)(void *context, int device);
    int (*vi_enable_dev)(void *context, int device);
    int (*vi_disable_dev)(void *context, int device);
    int (*vi_bind_pipe)(void *context, int device, const void *binding);
    int (*vi_set_channel_attr)(void *context, int device, int channel,
                               const void *attribute);
    int (*vi_enable_channel)(void *context, int device, int channel);
    int (*vi_disable_channel)(void *context, int device, int channel);

    int (*vpss_create_group)(void *context, int group,
                             const void *attribute);
    int (*vpss_destroy_group)(void *context, int group);
    int (*vpss_set_device)(void *context, int group, int device);
    int (*vpss_enable_backup)(void *context, int group);
    int (*vpss_start_group)(void *context, int group);
    int (*vpss_stop_group)(void *context, int group);
    int (*vpss_set_channel_attr)(void *context, int group, int channel,
                                 const void *attribute);
    int (*vpss_enable_channel)(void *context, int group, int channel);
    int (*vpss_disable_channel)(void *context, int group, int channel);
    int (*vpss_get_channel_frame)(void *context, int group, int channel,
                                  void *frame, int timeout_ms);
    int (*vpss_release_channel_frame)(void *context, int group, int channel,
                                      const void *frame);

    int (*avs_set_working_set)(void *context, uint64_t bytes);
    int (*avs_create_group)(void *context, int group, const void *attribute);
    int (*avs_destroy_group)(void *context, int group);
    int (*avs_start_group)(void *context, int group);
    int (*avs_stop_group)(void *context, int group);
    int (*avs_set_channel_attr)(void *context, int group, int channel,
                                const void *attribute);
    int (*avs_enable_channel)(void *context, int group, int channel);
    int (*avs_disable_channel)(void *context, int group, int channel);
    int (*avs_get_channel_frame)(void *context, int group, int channel,
                                 void *frame, int timeout_ms);
    int (*avs_release_channel_frame)(void *context, int group, int channel,
                                     const void *frame);

    int (*venc_create_channel)(void *context, int channel,
                               const void *attribute);
    int (*venc_destroy_channel)(void *context, int channel);
    int (*venc_set_rc_param)(void *context, int channel,
                             const void *parameter);
    int (*venc_start_receive)(void *context, int channel,
                              const void *parameter);
    int (*venc_stop_receive)(void *context, int channel);
    int (*venc_get_stream)(void *context, int channel, void *stream,
                           int timeout_ms);
    int (*venc_release_stream)(void *context, int channel,
                               const void *stream);
    int (*venc_request_idr)(void *context, int channel, bool instant);
    int (*venc_get_h264_vui)(void *context, int channel, void *vui);
    int (*venc_set_h264_vui)(void *context, int channel, const void *vui);
    int (*venc_get_h265_vui)(void *context, int channel, void *vui);
    int (*venc_set_h265_vui)(void *context, int channel, const void *vui);
    int (*venc_get_jpeg_param)(void *context, int channel, void *parameter);
    int (*venc_set_jpeg_param)(void *context, int channel,
                               const void *parameter);
    int (*venc_send_frame)(void *context, int channel, const void *frame,
                           int timeout_ms);

    int (*vdec_create_channel)(void *context, int channel,
                               const void *attribute);
    int (*vdec_destroy_channel)(void *context, int channel);
    int (*vdec_start_receive)(void *context, int channel);
    int (*vdec_stop_receive)(void *context, int channel);
    int (*vdec_reset_channel)(void *context, int channel);
    int (*vdec_send_stream)(void *context, int channel,
                            const void *stream, int timeout_ms);
    int (*vdec_query_status)(void *context, int channel, void *status);
    int (*vdec_set_channel_param)(void *context, int channel,
                                  const void *parameter);
    int (*vdec_set_display_mode)(void *context, int channel, int mode);

    int (*ai_set_pub_attr)(void *context, int device,
                           const void *attribute);
    int (*ai_enable)(void *context, int device);
    int (*ai_disable)(void *context, int device);
    int (*ai_enable_channel)(void *context, int device, int channel);
    int (*ai_disable_channel)(void *context, int device, int channel);
    int (*ai_set_channel_param)(void *context, int device, int channel,
                                const void *parameter);
    int (*ai_enable_resample)(void *context, int device, int channel,
                              int sample_rate);
    int (*ai_disable_resample)(void *context, int device, int channel);
    int (*ai_get_frame)(void *context, int device, int channel, void *frame,
                        void *aec_frame, int timeout_ms);
    int (*ai_release_frame)(void *context, int device, int channel,
                            const void *frame, const void *aec_frame);

    int (*ao_clear_pub_attr)(void *context, int device);
    int (*ao_set_pub_attr)(void *context, int device,
                           const void *attribute);
    int (*ao_enable)(void *context, int device);
    int (*ao_disable)(void *context, int device);
    int (*ao_enable_channel)(void *context, int device, int channel);
    int (*ao_disable_channel)(void *context, int device, int channel);
    int (*ao_set_channel_param)(void *context, int device, int channel,
                                const void *parameter);
    int (*ao_enable_resample)(void *context, int device, int channel,
                              int sample_rate);
    int (*ao_disable_resample)(void *context, int device, int channel);
    int (*ao_send_frame)(void *context, int device, int channel,
                         const void *frame, int timeout_ms);
    int (*ao_wait_eos)(void *context, int device, int channel,
                       int timeout_ms);
    int (*ao_set_volume)(void *context, int device, int volume);
    int (*ao_get_volume)(void *context, int device, int *volume);

    int (*vo_set_pub_attr)(void *context, int device, const void *attribute);
    int (*vo_enable)(void *context, int device);
    int (*vo_disable)(void *context, int device);
    int (*vo_bind_layer)(void *context, int layer, int device, int mode);
    int (*vo_unbind_layer)(void *context, int layer, int device);
    int (*vo_set_layer_buffer_length)(void *context, int layer, int length);
    int (*vo_set_layer_attr)(void *context, int layer,
                             const void *attribute);
    int (*vo_set_layer_splice_mode)(void *context, int layer, int mode);
    int (*vo_enable_layer)(void *context, int layer);
    int (*vo_disable_layer)(void *context, int layer);
    int (*vo_set_channel_attr)(void *context, int layer, int channel,
                               const void *attribute);
    int (*vo_enable_channel)(void *context, int layer, int channel);
    int (*vo_disable_channel)(void *context, int layer, int channel);
    int (*vo_send_frame)(void *context, int layer, int channel,
                         const void *frame, int timeout_ms);
    int (*vo_set_wbc_source)(void *context, int wbc, const void *source);
    int (*vo_set_wbc_attr)(void *context, int wbc, const void *attribute);
    int (*vo_enable_wbc)(void *context, int wbc);
    int (*vo_disable_wbc)(void *context, int wbc);

    int (*mmz_alloc)(void *context, void **handle, size_t bytes);
    int (*mmz_free)(void *context, void *handle);
    int (*mb_create)(void *context, void **handle, void *address,
                     size_t bytes);
    int (*mb_release)(void *context, void *handle);
    int (*mb_handle_to_id)(void *context, void *handle);
    void *(*mb_handle_to_address)(void *context, void *handle);
    size_t (*mb_get_size)(void *context, void *handle);
    int (*vo_create_graphics_buffer)(void *context, int width, int height,
                                     int format, void **handle);
    int (*vo_destroy_graphics_buffer)(void *context, void *handle);
};

struct ngcd_rk_display {
    const struct ngcd_rk_api *api;
    void *api_context;
    void *control_handle;
    void *pixel_handle;
    volatile uint32_t *control;
    int control_id;
    uint32_t last_generation;
    bool device_started;
    bool graphics_layer_bound;
    bool graphics_layer_started;
    bool graphics_channel_started;
    bool histogram_wbc_started;
    bool histogram_vpss_group_created;
    bool histogram_vpss_group_started;
    bool histogram_vpss_channel_started;
    bool histogram_wbc_vpss_bound;
    bool histogram_video_source;
    unsigned char *screenshot_bmp;
    size_t screenshot_bmp_capacity;
};

struct ngcd_rk_graph {
    const struct ngcd_rk_api *api;
    void *api_context;
    void *sensor_handle[2];
    int vi_device_for_vpss[2];
    int vi_channel_for_vpss[2];
    char avs_project[NGCD_PATH_MAX];
    char avs_working_directory[NGCD_PATH_MAX];
    unsigned int sensor_count;
    unsigned int sensor_mask;
    unsigned int vi_device_mask;
    unsigned int vi_channel_mask;
    unsigned int vpss_group_mask;
    unsigned int vpss_channel_mask;
    unsigned int vi_vpss_bind_mask;
    unsigned int vpss_avs_bind_mask;
    unsigned int vpss_vo_bind_mask;
    unsigned int vo_channel_mask;
    struct ngcd_encoder_state validation_encoder;
    struct ngcd_encoder_state record_encoder;
    struct ngcd_mp4_writer *record_writer;
    unsigned char *record_buffer;
    size_t record_buffer_capacity;
    void *stacked_snapshot_handle;
    uint32_t stacked_snapshot_width;
    uint32_t stacked_snapshot_height;
    uint32_t stacked_snapshot_stride;
    uint32_t stacked_snapshot_virtual_height;
    unsigned int validation_poll_count;
    int snapshot_width;
    int snapshot_height;
    bool system_started;
    bool avs_group_started;
    bool avs_channel_started;
    bool vo_video_layer_bound;
    bool vo_video_layer_started;
    bool validation_pending;
    bool validation_started;
    bool validation_bound;
    bool validation_receiving;
    bool validation_complete;
    bool validation_failed;
    bool snapshot_channel_started;
    bool record_channel_started;
    bool record_bound;
    bool record_receiving;
    bool recording;
    bool recording_failed;
    bool record_wait_keyframe;
    bool audio_device_started;
    bool audio_channel_started;
    bool audio_resample_started;
    int audio_input;
};

struct ngcd_rk_audio_output {
    const struct ngcd_rk_api *api;
    void *api_context;
    bool device_started;
    bool channel_started;
    bool resample_started;
};

int ngcd_rk_audio_output_start(struct ngcd_rk_audio_output *output,
                               const struct ngcd_rk_api *api,
                               void *api_context);
void ngcd_rk_audio_output_stop(struct ngcd_rk_audio_output *output);
int ngcd_rk_audio_output_set_volume(struct ngcd_rk_audio_output *output,
                                    int volume, int *readback);
int ngcd_rk_audio_output_send_pcm(struct ngcd_rk_audio_output *output,
                                  const void *data, size_t bytes,
                                  uint64_t pts_us, int timeout_ms);

int ngcd_rk_graph_start(struct ngcd_rk_graph *graph,
                        const struct ngcd_rk_api *api, void *api_context,
                        const struct ngcd_profile *profile);
int ngcd_rk_graph_start_in_system(struct ngcd_rk_graph *graph,
                                  const struct ngcd_rk_api *api,
                                  void *api_context,
                                  const struct ngcd_profile *profile,
                                  struct ngcd_rk_display *display);
void ngcd_rk_graph_stop(struct ngcd_rk_graph *graph);
int ngcd_rk_graph_tick(struct ngcd_rk_graph *graph);
int ngcd_rk_graph_validate_encoder(
    struct ngcd_rk_graph *graph,
    const struct ngcd_encoder_state *encoder);
int ngcd_rk_graph_activate_encoder(
    struct ngcd_rk_graph *graph,
    const struct ngcd_encoder_state *encoder);
int ngcd_rk_graph_snapshot(
    struct ngcd_rk_graph *graph, const char *path,
    const struct ngcd_rk_exif_metadata *metadata);
int ngcd_rk_graph_stack_snapshot(struct ngcd_rk_graph *graph, int count);
int ngcd_rk_graph_capture_raw(struct ngcd_rk_graph *graph, int count,
                              const char *capture_directory,
                              char output_directory[2][128]);
int ngcd_rk_graph_read_white_balance(
    const struct ngcd_rk_graph *graph,
    struct ngcd_rk_aiq_wb_gain gain[2]);
int ngcd_rk_graph_start_offline_in_system(
    struct ngcd_rk_graph *graph, const struct ngcd_rk_api *api,
    void *api_context, const struct ngcd_profile *profile,
    const uint32_t format[2],
    const struct ngcd_rk_aiq_wb_gain white_balance[2]);
int ngcd_rk_graph_offline_enqueue(
    struct ngcd_rk_graph *graph, void *const raw_data[2], bool discard_output);
int ngcd_rk_graph_record_start(
    struct ngcd_rk_graph *graph, const struct ngcd_encoder_state *encoder,
    const char *temporary_path);
int ngcd_rk_graph_record_stop(struct ngcd_rk_graph *graph);
void ngcd_rk_graph_record_abort(struct ngcd_rk_graph *graph);
int ngcd_rk_graph_record_size(struct ngcd_rk_graph *graph, uint64_t *bytes);
int ngcd_rk_graph_record_duration(struct ngcd_rk_graph *graph,
                                  uint64_t *microseconds);
int ngcd_rk_graph_record_camm_gyro(
    struct ngcd_rk_graph *graph, uint64_t monotonic_ns,
    float x_radians_per_second, float y_radians_per_second,
    float z_radians_per_second);
int ngcd_rk_graph_set_audio_input(struct ngcd_rk_graph *graph, int input);

int ngcd_rk_encoder_attributes(
    const struct ngcd_encoder_state *encoder,
    struct ngcd_rk_venc_chn_attr *attribute,
    struct ngcd_rk_venc_rc_param *rate_control);

int ngcd_rk_display_start(struct ngcd_rk_display *display,
                          const struct ngcd_rk_api *api, void *api_context);
void ngcd_rk_display_stop(struct ngcd_rk_display *display);
int ngcd_rk_display_tick(struct ngcd_rk_display *display);
int ngcd_rk_display_control_id(const struct ngcd_rk_display *display);
int ngcd_rk_display_histogram(
    struct ngcd_rk_display *display,
    uint32_t bins[NGCD_HISTOGRAM_BINS]);
int ngcd_rk_display_screenshot_bmp(
    struct ngcd_rk_display *display,
    const unsigned char **data, size_t *size);
int ngcd_rk_display_histogram_suspend(struct ngcd_rk_display *display);
void ngcd_rk_display_auxiliary_stop(struct ngcd_rk_display *display);
int ngcd_rk_image_set_acp(struct ngcd_rk_graph *graph,
                          enum ngcd_rk_acp_control control, int value,
                          int *readback);
int ngcd_rk_image_set_sharpness(struct ngcd_rk_graph *graph, int value,
                                int *readback);
int ngcd_rk_image_set_noise_reduction(struct ngcd_rk_graph *graph, int value,
                                      int *readback);
int ngcd_rk_image_set_iso(struct ngcd_rk_graph *graph, unsigned int iso,
                          unsigned int *readback);
int ngcd_rk_image_set_exposure(struct ngcd_rk_graph *graph, float seconds,
                               bool automatic, float *readback);
int ngcd_rk_image_set_exposure_iso(struct ngcd_rk_graph *graph,
                                   float seconds, bool exposure_automatic,
                                   unsigned int iso,
                                   float *exposure_readback,
                                   unsigned int *iso_readback);
int ngcd_rk_image_query_exposure(const struct ngcd_rk_graph *graph,
                                 float *seconds, unsigned int *iso);
int ngcd_rk_image_query_sensor_registers(
    const struct ngcd_rk_graph *graph, uint32_t exposure_register[2],
    uint32_t gain_register[2]);
int ngcd_rk_image_set_exposure_compensation(struct ngcd_rk_graph *graph,
                                             int value, int *readback);
int ngcd_rk_image_set_white_balance(struct ngcd_rk_graph *graph,
                                    unsigned int kelvin,
                                    unsigned int *readback);
int ngcd_rk_image_set_flicker(struct ngcd_rk_graph *graph,
                              enum ngcd_rk_flicker_control control,
                              enum ngcd_rk_flicker_control *readback);
int ngcd_rk_image_set_effect(struct ngcd_rk_graph *graph,
                             unsigned int effect, unsigned int *readback);
int ngcd_rk_image_read(const struct ngcd_rk_graph *graph,
                       struct ngcd_rk_image_readback *readback);

struct ngcd_rk_target;
struct ngcd_rk_playback;
int ngcd_rk_target_open(struct ngcd_rk_target **target,
                        struct ngcd_rk_api *api);
void ngcd_rk_target_close(struct ngcd_rk_target *target);

int ngcd_rk_playback_open(struct ngcd_rk_playback **playback,
                          const struct ngcd_rk_api *api, void *api_context,
                          struct ngcd_rk_audio_output *audio_output,
                          const char *path);
void ngcd_rk_playback_close(struct ngcd_rk_playback *playback);
int ngcd_rk_playback_tick(struct ngcd_rk_playback *playback,
                          uint64_t monotonic_us);
int ngcd_rk_playback_pause(struct ngcd_rk_playback *playback, bool pause,
                           uint64_t monotonic_us);
int ngcd_rk_playback_seek(struct ngcd_rk_playback *playback, size_t index,
                          uint64_t monotonic_us);
bool ngcd_rk_playback_is_picture(const struct ngcd_rk_playback *playback);
bool ngcd_rk_playback_is_paused(const struct ngcd_rk_playback *playback);
size_t ngcd_rk_playback_sample_index(
    const struct ngcd_rk_playback *playback);
size_t ngcd_rk_playback_sample_count(
    const struct ngcd_rk_playback *playback);
uint32_t ngcd_rk_playback_decoder_received(
    const struct ngcd_rk_playback *playback);
uint32_t ngcd_rk_playback_decoder_decoded(
    const struct ngcd_rk_playback *playback);
uint32_t ngcd_rk_playback_decoder_pending_stream(
    const struct ngcd_rk_playback *playback);
uint32_t ngcd_rk_playback_decoder_pending_pictures(
    const struct ngcd_rk_playback *playback);
uint32_t ngcd_rk_playback_decoder_errors(
    const struct ngcd_rk_playback *playback);
uint32_t ngcd_rk_playback_presented_frames(
    const struct ngcd_rk_playback *playback);
uint32_t ngcd_rk_playback_output_errors(
    const struct ngcd_rk_playback *playback);
uint64_t ngcd_rk_playback_duration_us(
    const struct ngcd_rk_playback *playback);
uint64_t ngcd_rk_playback_file_size(
    const struct ngcd_rk_playback *playback);
uint64_t ngcd_rk_playback_create_time(
    const struct ngcd_rk_playback *playback);
unsigned int ngcd_rk_playback_width(
    const struct ngcd_rk_playback *playback);
unsigned int ngcd_rk_playback_height(
    const struct ngcd_rk_playback *playback);
const char *ngcd_rk_playback_codec_name(
    const struct ngcd_rk_playback *playback);

#endif
