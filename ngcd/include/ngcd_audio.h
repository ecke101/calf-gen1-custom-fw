#ifndef CALF_NGCD_AUDIO_H
#define CALF_NGCD_AUDIO_H

struct ngcd_audio_control;

int ngcd_audio_control_open(struct ngcd_audio_control **control);
void ngcd_audio_control_close(struct ngcd_audio_control *control);
int ngcd_audio_control_detect_input(struct ngcd_audio_control *control,
                                    int *input);
int ngcd_audio_control_apply(struct ngcd_audio_control *control, int input,
                             int volume);

#endif
