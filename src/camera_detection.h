#ifndef AMTECH_CAMERA_DETECTION_H
#define AMTECH_CAMERA_DETECTION_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int person_detected;
    float max_confidence;
} camera_detection_result_t;

int camera_detection_run_once(const char *rtsp_url, camera_detection_result_t *result);

#ifdef SIMULATE_CAMERA
void camera_detection_set_simulated_result(int person_detected, float confidence);
#endif

#ifdef __cplusplus
}
#endif

#endif
