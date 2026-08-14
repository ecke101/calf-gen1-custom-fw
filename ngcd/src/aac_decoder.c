#include "ngcd_aac_decoder.h"

#include "ixheaac_type_def.h"
#include "ixheaacd_aac_config.h"
#include "ixheaacd_apicmd_standards.h"
#include "ixheaacd_error_standards.h"
#include "ixheaacd_memory_standards.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NGCD_XAAC_MAX_ALLOCATIONS 16U

extern IA_ERRORCODE ixheaacd_dec_api(pVOID module, WORD32 command,
                                     WORD32 index, pVOID value);

struct ngcd_aac_decoder {
    void *api;
    void *allocations[NGCD_XAAC_MAX_ALLOCATIONS];
    size_t allocation_count;
    unsigned char *input;
    size_t input_capacity;
    unsigned char *pcm;
    size_t pcm_capacity;
    unsigned int channels;
    unsigned int sample_rate;
    unsigned int audio_object_type;
    bool initialized;
};

static int api_call(struct ngcd_aac_decoder *decoder, int command,
                    int index, void *value)
{
    IA_ERRORCODE error = ixheaacd_dec_api(
        decoder != NULL ? decoder->api : NULL, command, index, value);
    if (error != IA_NO_ERROR) {
        fprintf(stderr,
                "ngcd: libxaac command 0x%x/0x%x failed (0x%08x)\n",
                command, index, (unsigned int)error);
        return -1;
    }
    return 0;
}

static void release_decoder(struct ngcd_aac_decoder *decoder)
{
    size_t index;
    for (index = 0U; index < decoder->allocation_count; ++index)
        free(decoder->allocations[index]);
    memset(decoder->allocations, 0, sizeof(decoder->allocations));
    decoder->allocation_count = 0U;
    decoder->api = NULL;
    decoder->input = NULL;
    decoder->input_capacity = 0U;
    decoder->pcm = NULL;
    decoder->pcm_capacity = 0U;
    decoder->initialized = false;
}

static void *allocate_aligned(struct ngcd_aac_decoder *decoder, size_t size,
                              size_t alignment)
{
    unsigned char *allocation;
    uintptr_t address;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    if ((alignment & (alignment - 1U)) != 0U || size == 0U ||
        size > SIZE_MAX - alignment ||
        decoder->allocation_count >= NGCD_XAAC_MAX_ALLOCATIONS)
        return NULL;
    allocation = calloc(1U, size + alignment - 1U);
    if (allocation == NULL)
        return NULL;
    decoder->allocations[decoder->allocation_count++] = allocation;
    address = ((uintptr_t)allocation + alignment - 1U) &
              ~((uintptr_t)alignment - 1U);
    return (void *)address;
}

static int sampling_frequency_index(unsigned int sample_rate)
{
    static const unsigned int rates[] = {
        96000U, 88200U, 64000U, 48000U, 44100U, 32000U, 24000U,
        22050U, 16000U, 12000U, 11025U, 8000U, 7350U,
    };
    size_t index;
    for (index = 0U; index < sizeof(rates) / sizeof(rates[0]); ++index) {
        if (rates[index] == sample_rate)
            return (int)index;
    }
    return -1;
}

static int initialize_stream(struct ngcd_aac_decoder *decoder)
{
    unsigned char config[2];
    unsigned int input_bytes = sizeof(config);
    unsigned int initialized = 0U;
    unsigned int output_rate = 0U;
    unsigned int output_channels = 0U;
    int rate_index = sampling_frequency_index(decoder->sample_rate);
    if (rate_index < 0 || decoder->input_capacity < sizeof(config))
        return -1;
    config[0] = (unsigned char)((decoder->audio_object_type << 3U) |
                               ((unsigned int)rate_index >> 1U));
    config[1] = (unsigned char)(((unsigned int)rate_index << 7U) |
                               (decoder->channels << 3U));
    memcpy(decoder->input, config, sizeof(config));
    if (api_call(decoder, IA_API_CMD_SET_INPUT_BYTES, 0, &input_bytes) != 0 ||
        api_call(decoder, IA_API_CMD_INIT,
                 IA_CMD_TYPE_INIT_PROCESS, NULL) != 0 ||
        api_call(decoder, IA_API_CMD_INIT,
                 IA_CMD_TYPE_INIT_DONE_QUERY, &initialized) != 0)
        return -1;
    if (api_call(decoder, IA_API_CMD_GET_CONFIG_PARAM,
                 IA_ENHAACPLUS_DEC_CONFIG_PARAM_SAMP_FREQ,
                 &output_rate) != 0 ||
        api_call(decoder, IA_API_CMD_GET_CONFIG_PARAM,
                 IA_ENHAACPLUS_DEC_CONFIG_PARAM_NUM_CHANNELS,
                 &output_channels) != 0)
        return -1;
    if (output_rate != decoder->sample_rate ||
        output_channels != decoder->channels) {
        fprintf(stderr, "ngcd: libxaac configured unexpected %u Hz/%u ch\n",
                output_rate, output_channels);
        return -1;
    }
    decoder->initialized = initialized == 1U;
    return 0;
}

static int initialize_decoder(struct ngcd_aac_decoder *decoder)
{
    unsigned int api_size = 0U;
    unsigned int memtabs_size = 0U;
    unsigned int memory_count = 0U;
    unsigned int value;
    unsigned int index;
    if (ixheaacd_dec_api(NULL, IA_API_CMD_GET_API_SIZE, 0,
                         &api_size) != IA_NO_ERROR || api_size == 0U)
        return -1;
    decoder->api = allocate_aligned(decoder, api_size, 8U);
    if (decoder->api == NULL ||
        api_call(decoder, IA_API_CMD_INIT,
                 IA_CMD_TYPE_INIT_API_PRE_CONFIG_PARAMS, NULL) != 0)
        return -1;

    value = 16U;
    if (api_call(decoder, IA_API_CMD_SET_CONFIG_PARAM,
                 IA_ENHAACPLUS_DEC_CONFIG_PARAM_PCM_WDSZ, &value) != 0)
        return -1;
    value = 1U;
    if (api_call(decoder, IA_API_CMD_SET_CONFIG_PARAM,
                 IA_ENHAACPLUS_DEC_CONFIG_PARAM_ISMP4, &value) != 0)
        return -1;
    value = decoder->channels;
    if (api_call(decoder, IA_API_CMD_SET_CONFIG_PARAM,
                 IA_ENHAACPLUS_DEC_CONFIG_PARAM_MAX_CHANNEL, &value) != 0)
        return -1;

    if (api_call(decoder, IA_API_CMD_GET_MEMTABS_SIZE, 0,
                 &memtabs_size) != 0 || memtabs_size == 0U)
        return -1;
    value = (unsigned int)sizeof(void *);
    {
        void *memtabs = allocate_aligned(decoder, memtabs_size, value);
        if (memtabs == NULL ||
            api_call(decoder, IA_API_CMD_SET_MEMTABS_PTR, 0, memtabs) != 0)
            return -1;
    }
    if (api_call(decoder, IA_API_CMD_INIT,
                 IA_CMD_TYPE_INIT_API_POST_CONFIG_PARAMS, NULL) != 0 ||
        api_call(decoder, IA_API_CMD_GET_N_MEMTABS, 0,
                 &memory_count) != 0 || memory_count == 0U ||
        memory_count > NGCD_XAAC_MAX_ALLOCATIONS -
                           decoder->allocation_count)
        return -1;
    for (index = 0U; index < memory_count; ++index) {
        unsigned int size = 0U;
        unsigned int alignment = 0U;
        unsigned int type = 0U;
        void *memory;
        if (api_call(decoder, IA_API_CMD_GET_MEM_INFO_SIZE,
                     (int)index, &size) != 0 ||
            api_call(decoder, IA_API_CMD_GET_MEM_INFO_ALIGNMENT,
                     (int)index, &alignment) != 0 ||
            api_call(decoder, IA_API_CMD_GET_MEM_INFO_TYPE,
                     (int)index, &type) != 0)
            return -1;
        memory = allocate_aligned(decoder, size, alignment);
        if (memory == NULL ||
            api_call(decoder, IA_API_CMD_SET_MEM_PTR,
                     (int)index, memory) != 0)
            return -1;
        if (type == IA_MEMTYPE_INPUT) {
            decoder->input = memory;
            decoder->input_capacity = size;
        } else if (type == IA_MEMTYPE_OUTPUT) {
            decoder->pcm = memory;
            decoder->pcm_capacity = size;
        }
    }
    if (decoder->input == NULL || decoder->pcm == NULL)
        return -1;
    return initialize_stream(decoder);
}

int ngcd_aac_decoder_open(struct ngcd_aac_decoder **output,
                          unsigned int channels,
                          unsigned int sample_rate,
                          unsigned int audio_object_type)
{
    struct ngcd_aac_decoder *decoder;
    if (output == NULL || channels == 0U || channels > 2U ||
        sampling_frequency_index(sample_rate) < 0 ||
        audio_object_type != 2U)
        return -1;
    *output = NULL;
    decoder = calloc(1U, sizeof(*decoder));
    if (decoder == NULL)
        return -1;
    decoder->channels = channels;
    decoder->sample_rate = sample_rate;
    decoder->audio_object_type = audio_object_type;
    if (initialize_decoder(decoder) != 0) {
        ngcd_aac_decoder_close(decoder);
        return -1;
    }
    *output = decoder;
    return 0;
}

void ngcd_aac_decoder_close(struct ngcd_aac_decoder *decoder)
{
    if (decoder == NULL)
        return;
    release_decoder(decoder);
    free(decoder);
}

int ngcd_aac_decoder_reset(struct ngcd_aac_decoder *decoder)
{
    if (decoder == NULL)
        return -1;
    release_decoder(decoder);
    if (initialize_decoder(decoder) != 0) {
        release_decoder(decoder);
        return -1;
    }
    return 0;
}

int ngcd_aac_decoder_decode(struct ngcd_aac_decoder *decoder,
                            const unsigned char *input, size_t input_size,
                            short *output, size_t output_samples,
                            size_t *written_samples)
{
    unsigned int bytes;
    unsigned int output_bytes = 0U;
    unsigned int consumed = 0U;
    if (decoder == NULL || input == NULL || input_size == 0U ||
        input_size > decoder->input_capacity || input_size > UINT32_MAX ||
        output == NULL || written_samples == NULL)
        return -1;
    *written_samples = 0U;
    memcpy(decoder->input, input, input_size);
    bytes = (unsigned int)input_size;
    if (!decoder->initialized) {
        unsigned int initialized = 0U;
        if (api_call(decoder, IA_API_CMD_SET_INPUT_BYTES, 0, &bytes) != 0 ||
            api_call(decoder, IA_API_CMD_INIT,
                     IA_CMD_TYPE_INIT_PROCESS, NULL) != 0 ||
            api_call(decoder, IA_API_CMD_INIT,
                     IA_CMD_TYPE_INIT_DONE_QUERY, &initialized) != 0 ||
            initialized != 1U) {
            fprintf(stderr, "ngcd: libxaac stream initialization failed\n");
            return -1;
        }
        decoder->initialized = true;
    }
    if (api_call(decoder, IA_API_CMD_SET_INPUT_BYTES, 0, &bytes) != 0 ||
        api_call(decoder, IA_API_CMD_EXECUTE,
                 IA_CMD_TYPE_DO_EXECUTE, NULL) != 0 ||
        api_call(decoder, IA_API_CMD_GET_CURIDX_INPUT_BUF,
                 0, &consumed) != 0 || consumed != bytes ||
        api_call(decoder, IA_API_CMD_GET_OUTPUT_BYTES,
                 0, &output_bytes) != 0 || output_bytes == 0U ||
        output_bytes > decoder->pcm_capacity ||
        (output_bytes & 1U) != 0U || output_bytes / 2U > output_samples)
        return -1;
    memcpy(output, decoder->pcm, output_bytes);
    *written_samples = output_bytes / 2U;
    return 0;
}
