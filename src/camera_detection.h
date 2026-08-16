#ifndef AMTECH_CAMERA_DETECTION_H
#define AMTECH_CAMERA_DETECTION_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMTECH_CAMERA_SOURCE_MAX 16
#define AMTECH_CAMERA_EVENT_TYPE_MAX 32

typedef struct
{
    char source[AMTECH_CAMERA_SOURCE_MAX];
    char event_type[AMTECH_CAMERA_EVENT_TYPE_MAX];
    int person_detected;
    float max_confidence;
    int person_box_valid;
    int person_x1;
    int person_y1;
    int person_x2;
    int person_y2;
} camera_detection_result_t;

int camera_detection_run_once(const char *rtsp_url, camera_detection_result_t *result);
int camera_detection_run_once_for_source(const char *source,
                                         const char *event_type,
                                         const char *rtsp_url,
                                         pthread_mutex_t *inference_mutex,
                                         camera_detection_result_t *result);

#ifdef SIMULATE_CAMERA
void camera_detection_set_simulated_result(int person_detected, float confidence);
void camera_detection_set_simulated_result_for_source(const char *source,
                                                      int person_detected,
                                                      float confidence);
void camera_detection_set_simulated_result_box_for_source(const char *source,
                                                          int person_detected,
                                                          float confidence,
                                                          int x1,
                                                          int y1,
                                                          int x2,
                                                          int y2);
void camera_detection_set_simulated_delay_us(unsigned int delay_us);
void camera_detection_reset_simulated_metrics(void);
int camera_detection_get_simulated_max_concurrent_inference(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
