#ifndef CALF_NGCD_AAC_DECODER_H
#define CALF_NGCD_AAC_DECODER_H

#include <stddef.h>

struct ngcd_aac_decoder;

int ngcd_aac_decoder_open(struct ngcd_aac_decoder **output,
                          unsigned int channels,
                          unsigned int sample_rate,
                          unsigned int audio_object_type);
void ngcd_aac_decoder_close(struct ngcd_aac_decoder *decoder);
int ngcd_aac_decoder_reset(struct ngcd_aac_decoder *decoder);
int ngcd_aac_decoder_decode(struct ngcd_aac_decoder *decoder,
                            const unsigned char *input, size_t input_size,
                            short *output, size_t output_samples,
                            size_t *written_samples);

#endif
