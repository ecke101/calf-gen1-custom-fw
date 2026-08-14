#ifndef CALF_NGCD_AAC_H
#define CALF_NGCD_AAC_H

#include <stddef.h>

#define NGCD_AAC_FRAME_SAMPLES 1024U
#define NGCD_AAC_MAX_OUTPUT_SIZE 20480U

struct ngcd_aac_encoder;

int ngcd_aac_encoder_open(struct ngcd_aac_encoder **encoder,
                          unsigned int channels, unsigned int sample_rate,
                          unsigned int bitrate);
int ngcd_aac_encoder_encode(struct ngcd_aac_encoder *encoder,
                            const unsigned char *pcm, size_t pcm_size,
                            unsigned char *output, size_t output_capacity,
                            size_t *output_size);
void ngcd_aac_encoder_close(struct ngcd_aac_encoder *encoder);

#endif
