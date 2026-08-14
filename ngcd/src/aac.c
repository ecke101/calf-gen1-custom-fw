#include "ngcd_aac.h"

#include "cmnMemory.h"
#include "voAAC.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

struct ngcd_aac_encoder {
    VO_AUDIO_CODECAPI api;
    VO_HANDLE handle;
    VO_MEM_OPERATOR memory;
    unsigned int channels;
};

int ngcd_aac_encoder_open(struct ngcd_aac_encoder **output,
                          unsigned int channels, unsigned int sample_rate,
                          unsigned int bitrate)
{
    struct ngcd_aac_encoder *encoder;
    VO_CODEC_INIT_USERDATA user_data;
    AACENC_PARAM parameter;
    if (output == NULL || (channels != 1U && channels != 2U) ||
        sample_rate != 48000U || bitrate < 16000U || bitrate > 320000U)
        return -1;
    *output = NULL;
    encoder = calloc(1U, sizeof(*encoder));
    if (encoder == NULL)
        return -1;
    memset(&encoder->memory, 0, sizeof(encoder->memory));
    encoder->memory.Alloc = cmnMemAlloc;
    encoder->memory.Copy = cmnMemCopy;
    encoder->memory.Free = cmnMemFree;
    encoder->memory.Set = cmnMemSet;
    encoder->memory.Check = cmnMemCheck;
    memset(&user_data, 0, sizeof(user_data));
    user_data.memflag = VO_IMF_USERMEMOPERATOR;
    user_data.memData = &encoder->memory;
    memset(&parameter, 0, sizeof(parameter));
    parameter.sampleRate = (int)sample_rate;
    parameter.bitRate = (int)bitrate;
    parameter.nChannels = (short)channels;
    parameter.adtsUsed = 0;
    if (voGetAACEncAPI(&encoder->api) != VO_ERR_NONE ||
        encoder->api.Init(&encoder->handle, VO_AUDIO_CodingAAC,
                          &user_data) != VO_ERR_NONE ||
        encoder->api.SetParam(encoder->handle, VO_PID_AAC_ENCPARAM,
                              &parameter) != VO_ERR_NONE) {
        ngcd_aac_encoder_close(encoder);
        return -1;
    }
    encoder->channels = channels;
    *output = encoder;
    return 0;
}

int ngcd_aac_encoder_encode(struct ngcd_aac_encoder *encoder,
                            const unsigned char *pcm, size_t pcm_size,
                            unsigned char *output, size_t output_capacity,
                            size_t *output_size)
{
    VO_CODECBUFFER input;
    VO_CODECBUFFER encoded;
    VO_AUDIO_OUTPUTINFO information;
    size_t expected;
    if (encoder == NULL || encoder->handle == NULL || pcm == NULL ||
        output == NULL || output_size == NULL ||
        output_capacity > (size_t)UINT32_MAX)
        return -1;
    expected = NGCD_AAC_FRAME_SAMPLES * encoder->channels * 2U;
    if (pcm_size != expected || output_capacity < NGCD_AAC_MAX_OUTPUT_SIZE)
        return -1;
    memset(&input, 0, sizeof(input));
    input.Buffer = (VO_PBYTE)pcm;
    input.Length = (VO_U32)pcm_size;
    memset(&encoded, 0, sizeof(encoded));
    encoded.Buffer = output;
    encoded.Length = (VO_U32)output_capacity;
    memset(&information, 0, sizeof(information));
    if (encoder->api.SetInputData(encoder->handle, &input) != VO_ERR_NONE ||
        encoder->api.GetOutputData(encoder->handle, &encoded,
                                   &information) != VO_ERR_NONE ||
        information.InputUsed != pcm_size || encoded.Length == 0U ||
        encoded.Length > output_capacity)
        return -1;
    *output_size = encoded.Length;
    return 0;
}

void ngcd_aac_encoder_close(struct ngcd_aac_encoder *encoder)
{
    if (encoder == NULL)
        return;
    if (encoder->handle != NULL && encoder->api.Uninit != NULL)
        (void)encoder->api.Uninit(encoder->handle);
    free(encoder);
}
