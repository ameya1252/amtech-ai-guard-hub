#include "camera_detection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AMTECH_CAMERA_PATH_MAX 128
#define AMTECH_CAMERA_CAPTURE_TIMEOUT_SECONDS "15"
#define AMTECH_CAMERA_CLAHE_TIMEOUT_SECONDS 5
#define AMTECH_CAMERA_ENCODE_TIMEOUT_SECONDS 5
#define AMTECH_CAMERA_DEMO_TIMEOUT_SECONDS 30
#define AMTECH_CAMERA_CLAHE_BIN "/root/amtech_clahe_ppm"
#define AMTECH_CAMERA_DEMO_BIN "/root/rknn_yolov5_demo_export/rknn_yolov5_demo"
#define AMTECH_CAMERA_DEMO_WORKDIR "/root/rknn_yolov5_demo_export"
#define AMTECH_CAMERA_MODEL_PATH "/root/rknn_yolov5_demo_export/model/yolov5.rknn"
#define AMTECH_CAMERA_ALARM_PERSON_PREFIX "Alarm: person detected confidence="

static void fill_result_identity(camera_detection_result_t *result,
                                 const char *source,
                                 const char *event_type)
{
    if (result == NULL)
    {
        return;
    }

    snprintf(result->source,
             sizeof(result->source),
             "%s",
             source != NULL && source[0] != '\0' ? source : "front");
    snprintf(result->event_type,
             sizeof(result->event_type),
             "%s",
             event_type != NULL && event_type[0] != '\0' ? event_type : "intrusion");
}

#ifdef SIMULATE_CAMERA
#include <unistd.h>

typedef struct
{
    char source[AMTECH_CAMERA_SOURCE_MAX];
    int person_detected;
    float confidence;
    int box_valid;
    int x1;
    int y1;
    int x2;
    int y2;
} simulated_camera_result_t;

static simulated_camera_result_t simulated_results[2] = {
    {"front", 0, 0.0f, 0, 0, 0, 0, 0},
    {"parking", 0, 0.0f, 0, 0, 0, 0, 0}};
static unsigned int simulated_delay_us = 0;
static int simulated_active_inference = 0;
static int simulated_max_concurrent_inference = 0;
static int simulated_run_count = 0;

static simulated_camera_result_t *find_simulated_result(const char *source)
{
    int i;

    for (i = 0; i < 2; i++)
    {
        if (strcmp(simulated_results[i].source, source) == 0)
        {
            return &simulated_results[i];
        }
    }

    return &simulated_results[0];
}

void camera_detection_set_simulated_result(int person_detected, float confidence)
{
    camera_detection_set_simulated_result_for_source("front", person_detected, confidence);
}

void camera_detection_set_simulated_result_for_source(const char *source,
                                                      int person_detected,
                                                      float confidence)
{
    camera_detection_set_simulated_result_box_for_source(source,
                                                         person_detected,
                                                         confidence,
                                                         100,
                                                         100,
                                                         220,
                                                         360);
}

void camera_detection_set_simulated_result_box_for_source(const char *source,
                                                          int person_detected,
                                                          float confidence,
                                                          int x1,
                                                          int y1,
                                                          int x2,
                                                          int y2)
{
    simulated_camera_result_t *simulated_result;

    if (source == NULL || source[0] == '\0')
    {
        source = "front";
    }

    simulated_result = find_simulated_result(source);
    simulated_result->person_detected = person_detected ? 1 : 0;
    simulated_result->confidence = confidence;
    simulated_result->box_valid = person_detected ? 1 : 0;
    simulated_result->x1 = x1;
    simulated_result->y1 = y1;
    simulated_result->x2 = x2;
    simulated_result->y2 = y2;
}

void camera_detection_set_simulated_delay_us(unsigned int delay_us)
{
    simulated_delay_us = delay_us;
}

void camera_detection_reset_simulated_metrics(void)
{
    simulated_active_inference = 0;
    simulated_max_concurrent_inference = 0;
    simulated_run_count = 0;
    simulated_delay_us = 0;
}

int camera_detection_get_simulated_max_concurrent_inference(void)
{
    return simulated_max_concurrent_inference;
}

int camera_detection_get_simulated_run_count(void)
{
    return simulated_run_count;
}

static void simulated_inference_enter(void)
{
    simulated_active_inference++;
    if (simulated_active_inference > simulated_max_concurrent_inference)
    {
        simulated_max_concurrent_inference = simulated_active_inference;
    }
}

static void simulated_inference_exit(void)
{
    if (simulated_active_inference > 0)
    {
        simulated_active_inference--;
    }
}

int camera_detection_run_once(const char *rtsp_url, camera_detection_result_t *result)
{
    return camera_detection_run_once_for_source("front", "intrusion", rtsp_url, NULL, result);
}

int camera_detection_run_once_for_source(const char *source,
                                         const char *event_type,
                                         const char *rtsp_url,
                                         pthread_mutex_t *inference_mutex,
                                         camera_detection_result_t *result)
{
    (void)rtsp_url;

    if (result == NULL)
    {
        return -1;
    }

    if (source == NULL || source[0] == '\0')
    {
        source = "front";
    }

    fill_result_identity(result, source, event_type);
    simulated_run_count++;

    if (inference_mutex != NULL)
    {
        pthread_mutex_lock(inference_mutex);
    }
    simulated_inference_enter();
    if (simulated_delay_us > 0)
    {
        usleep(simulated_delay_us);
    }
    simulated_inference_exit();
    if (inference_mutex != NULL)
    {
        pthread_mutex_unlock(inference_mutex);
    }

    result->person_detected = find_simulated_result(source)->person_detected;
    result->max_confidence = find_simulated_result(source)->confidence;
    result->person_box_valid = find_simulated_result(source)->box_valid;
    result->person_x1 = find_simulated_result(source)->x1;
    result->person_y1 = find_simulated_result(source)->y1;
    result->person_x2 = find_simulated_result(source)->x2;
    result->person_y2 = find_simulated_result(source)->y2;
    printf("Camera: SIMULATE_CAMERA source=%s event=%s result person=%d confidence=%.3f box_valid=%d box=(%d,%d,%d,%d)\n",
           result->source,
           result->event_type,
           result->person_detected,
           result->max_confidence,
           result->person_box_valid,
           result->person_x1,
           result->person_y1,
           result->person_x2,
           result->person_y2);
    return 0;
}
#else
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct
{
    char raw_frame_path[AMTECH_CAMERA_PATH_MAX];
    char clahe_frame_path[AMTECH_CAMERA_PATH_MAX];
    char frame_path[AMTECH_CAMERA_PATH_MAX];
    char output_path[AMTECH_CAMERA_PATH_MAX];
} camera_detection_paths_t;

static void build_path_component(const char *source, char *buffer, size_t buffer_size)
{
    size_t i;
    size_t out = 0;

    if (buffer_size == 0)
    {
        return;
    }

    if (source == NULL || source[0] == '\0')
    {
        source = "front";
    }

    for (i = 0; source[i] != '\0' && out + 1 < buffer_size; i++)
    {
        unsigned char ch = (unsigned char)source[i];
        buffer[out++] = (isalnum(ch) || ch == '-' || ch == '_') ? (char)ch : '_';
    }
    buffer[out] = '\0';

    if (buffer[0] == '\0')
    {
        snprintf(buffer, buffer_size, "%s", "front");
    }
}

static void build_camera_paths(const char *source, camera_detection_paths_t *paths)
{
    char component[AMTECH_CAMERA_SOURCE_MAX];

    build_path_component(source, component, sizeof(component));
    snprintf(paths->raw_frame_path, sizeof(paths->raw_frame_path), "/tmp/amtech_%s_live_frame.ppm", component);
    snprintf(paths->clahe_frame_path, sizeof(paths->clahe_frame_path), "/tmp/amtech_%s_live_frame_clahe.ppm", component);
    snprintf(paths->frame_path, sizeof(paths->frame_path), "/tmp/amtech_%s_live_frame.jpg", component);
    snprintf(paths->output_path, sizeof(paths->output_path), "/tmp/amtech_%s_camera_detection_output.txt", component);
}

static int wait_for_child(pid_t pid, const char *name, int timeout_seconds)
{
    int status;
    int elapsed_ms = 0;

    for (;;)
    {
        pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == pid)
        {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            {
                printf("Camera: command %s failed status=%d\n", name, status);
                return -1;
            }
            return 0;
        }

        if (result < 0)
        {
            printf("Camera: waitpid failed for %s: %s\n", name, strerror(errno));
            return -1;
        }

        if (timeout_seconds > 0 && elapsed_ms >= timeout_seconds * 1000)
        {
            printf("Camera: command %s timed out after %d seconds\n", name, timeout_seconds);
            kill(pid, SIGTERM);
            usleep(200 * 1000);
            if (waitpid(pid, &status, WNOHANG) == 0)
            {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            return -1;
        }

        usleep(100 * 1000);
        elapsed_ms += 100;
    }
}

static int run_command_in_dir(char *const argv[],
                              const char *stdout_path,
                              int timeout_seconds,
                              const char *working_dir)
{
    pid_t pid;

    pid = fork();
    if (pid < 0)
    {
        printf("Camera: fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        if (working_dir != NULL && chdir(working_dir) != 0)
        {
            _exit(125);
        }

        if (stdout_path != NULL)
        {
            int fd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0)
            {
                _exit(126);
            }
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

        execvp(argv[0], argv);
        _exit(127);
    }

    return wait_for_child(pid, argv[0], timeout_seconds);
}

static int run_command(char *const argv[], const char *stdout_path, int timeout_seconds)
{
    return run_command_in_dir(argv, stdout_path, timeout_seconds, NULL);
}

static int capture_frame(const char *rtsp_url, const camera_detection_paths_t *paths)
{
    char *const argv[] = {
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-rtsp_transport",
        "tcp",
        "-analyzeduration",
        "1000000",
        "-probesize",
        "32768",
        "-y",
        "-i",
        (char *)rtsp_url,
        "-frames:v",
        "1",
        "-vf",
        "scale=480:480:force_original_aspect_ratio=decrease,pad=480:480:(ow-iw)/2:(oh-ih)/2",
        "-pix_fmt",
        "rgb24",
        (char *)paths->raw_frame_path,
        NULL};

    unlink(paths->raw_frame_path);
    return run_command(argv, NULL, atoi(AMTECH_CAMERA_CAPTURE_TIMEOUT_SECONDS));
}

static int apply_clahe(const camera_detection_paths_t *paths)
{
    char *const argv[] = {
        AMTECH_CAMERA_CLAHE_BIN,
        (char *)paths->raw_frame_path,
        (char *)paths->clahe_frame_path,
        "2.0",
        "8",
        "8",
        NULL};

    unlink(paths->clahe_frame_path);
    return run_command(argv, NULL, AMTECH_CAMERA_CLAHE_TIMEOUT_SECONDS);
}

static int encode_clahe_frame(const camera_detection_paths_t *paths)
{
    char *const argv[] = {
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        (char *)paths->clahe_frame_path,
        "-frames:v",
        "1",
        (char *)paths->frame_path,
        NULL};

    unlink(paths->frame_path);
    return run_command(argv, NULL, AMTECH_CAMERA_ENCODE_TIMEOUT_SECONDS);
}

static int run_detection_demo(const camera_detection_paths_t *paths)
{
    char *const argv[] = {
        AMTECH_CAMERA_DEMO_BIN,
        AMTECH_CAMERA_MODEL_PATH,
        (char *)paths->frame_path,
        NULL};

    return run_command_in_dir(argv,
                              paths->output_path,
                              AMTECH_CAMERA_DEMO_TIMEOUT_SECONDS,
                              AMTECH_CAMERA_DEMO_WORKDIR);
}

static int parse_detection_output(const camera_detection_paths_t *paths, camera_detection_result_t *result)
{
    char line[512];
    FILE *fp;

    fp = fopen(paths->output_path, "r");
    if (fp == NULL)
    {
        printf("Camera: failed to open detection output %s: %s\n",
               paths->output_path,
               strerror(errno));
        return -1;
    }

    result->person_detected = 0;
    result->max_confidence = 0.0f;
    result->person_box_valid = 0;
    result->person_x1 = 0;
    result->person_y1 = 0;
    result->person_x2 = 0;
    result->person_y2 = 0;

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        float confidence;

        if (strncmp(line,
                    AMTECH_CAMERA_ALARM_PERSON_PREFIX,
                    strlen(AMTECH_CAMERA_ALARM_PERSON_PREFIX)) == 0)
        {
            char *end = NULL;
            const char *confidence_text = line + strlen(AMTECH_CAMERA_ALARM_PERSON_PREFIX);

            confidence = strtof(confidence_text, &end);
            if (end != confidence_text)
            {
                result->person_detected = 1;
                if (confidence > result->max_confidence)
                {
                    result->max_confidence = confidence;
                }
            }
            continue;
        }

        if (strncmp(line, "person @", 8) != 0)
        {
            continue;
        }

        {
            int x1;
            int y1;
            int x2;
            int y2;
            int parsed;

            parsed = sscanf(line, "person @ (%d %d %d %d) %f", &x1, &y1, &x2, &y2, &confidence);
            if (parsed == 5)
            {
                if (confidence >= result->max_confidence)
                {
                    result->max_confidence = confidence;
                    result->person_box_valid = 1;
                    result->person_x1 = x1;
                    result->person_y1 = y1;
                    result->person_x2 = x2;
                    result->person_y2 = y2;
                }
                result->person_detected = 1;
                continue;
            }
        }

        {
            char *cursor = line;
            char *token = strtok(cursor, " \t\r\n");
            while (token != NULL)
            {
                char *next = strtok(NULL, " \t\r\n");
                if (next == NULL)
                {
                    char *end = NULL;
                    confidence = strtof(token, &end);
                    if (end != token && confidence > result->max_confidence)
                    {
                        result->max_confidence = confidence;
                    }
                    break;
                }
                token = next;
            }
        }

        result->person_detected = 1;
    }

    fclose(fp);
    printf("Camera: detection result source=%s event=%s person=%d confidence=%.3f box_valid=%d box=(%d,%d,%d,%d)\n",
           result->source,
           result->event_type,
           result->person_detected,
           result->max_confidence,
           result->person_box_valid,
           result->person_x1,
           result->person_y1,
           result->person_x2,
           result->person_y2);
    return 0;
}

int camera_detection_run_once(const char *rtsp_url, camera_detection_result_t *result)
{
    return camera_detection_run_once_for_source("front", "intrusion", rtsp_url, NULL, result);
}

int camera_detection_run_once_for_source(const char *source,
                                         const char *event_type,
                                         const char *rtsp_url,
                                         pthread_mutex_t *inference_mutex,
                                         camera_detection_result_t *result)
{
    camera_detection_paths_t paths;
    int rc;

    if (rtsp_url == NULL || rtsp_url[0] == '\0' || result == NULL)
    {
        printf("Camera: RTSP URL and result buffer are required\n");
        return -1;
    }

    fill_result_identity(result, source, event_type);
    build_camera_paths(result->source, &paths);

    if (capture_frame(rtsp_url, &paths) != 0)
    {
        return -1;
    }

    if (apply_clahe(&paths) != 0)
    {
        return -1;
    }

    if (encode_clahe_frame(&paths) != 0)
    {
        return -1;
    }

    if (inference_mutex != NULL)
    {
        pthread_mutex_lock(inference_mutex);
    }
    rc = run_detection_demo(&paths);
    if (inference_mutex != NULL)
    {
        pthread_mutex_unlock(inference_mutex);
    }

    if (rc != 0)
    {
        return -1;
    }

    return parse_detection_output(&paths, result);
}
#endif
