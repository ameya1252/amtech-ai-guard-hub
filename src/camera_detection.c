#include "camera_detection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AMTECH_CAMERA_FRAME_PATH "/tmp/amtech_live_frame.jpg"
#define AMTECH_CAMERA_OUTPUT_PATH "/tmp/amtech_camera_detection_output.txt"
#define AMTECH_CAMERA_CAPTURE_TIMEOUT_SECONDS "15"
#define AMTECH_CAMERA_DEMO_TIMEOUT_SECONDS 30
#define AMTECH_CAMERA_DEMO_BIN "/root/rknn_yolov5_demo_export/rknn_yolov5_demo"
#define AMTECH_CAMERA_MODEL_PATH "/root/rknn_yolov5_demo_export/model/yolov5.rknn"

#ifdef SIMULATE_CAMERA
static int simulated_person_detected = 0;
static float simulated_confidence = 0.0f;

void camera_detection_set_simulated_result(int person_detected, float confidence)
{
    simulated_person_detected = person_detected ? 1 : 0;
    simulated_confidence = confidence;
}

int camera_detection_run_once(const char *rtsp_url, camera_detection_result_t *result)
{
    (void)rtsp_url;

    if (result == NULL)
    {
        return -1;
    }

    result->person_detected = simulated_person_detected;
    result->max_confidence = simulated_confidence;
    printf("Camera: SIMULATE_CAMERA result person=%d confidence=%.3f\n",
           result->person_detected,
           result->max_confidence);
    return 0;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

static int run_command(char *const argv[], const char *stdout_path, int timeout_seconds)
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

static int capture_frame(const char *rtsp_url)
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
        "scale=640:640",
        "-q:v",
        "2",
        AMTECH_CAMERA_FRAME_PATH,
        NULL};

    unlink(AMTECH_CAMERA_FRAME_PATH);
    return run_command(argv, NULL, atoi(AMTECH_CAMERA_CAPTURE_TIMEOUT_SECONDS));
}

static int run_detection_demo(void)
{
    char *const argv[] = {
        AMTECH_CAMERA_DEMO_BIN,
        AMTECH_CAMERA_MODEL_PATH,
        AMTECH_CAMERA_FRAME_PATH,
        NULL};

    return run_command(argv, AMTECH_CAMERA_OUTPUT_PATH, AMTECH_CAMERA_DEMO_TIMEOUT_SECONDS);
}

static int parse_detection_output(camera_detection_result_t *result)
{
    char line[512];
    FILE *fp;

    fp = fopen(AMTECH_CAMERA_OUTPUT_PATH, "r");
    if (fp == NULL)
    {
        printf("Camera: failed to open detection output %s: %s\n",
               AMTECH_CAMERA_OUTPUT_PATH,
               strerror(errno));
        return -1;
    }

    result->person_detected = 0;
    result->max_confidence = 0.0f;

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        char *cursor;
        char *token;
        float confidence;

        if (strncmp(line, "person @", 8) != 0)
        {
            continue;
        }

        cursor = line;
        token = strtok(cursor, " \t\r\n");
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

        result->person_detected = 1;
    }

    fclose(fp);
    printf("Camera: detection result person=%d confidence=%.3f\n",
           result->person_detected,
           result->max_confidence);
    return 0;
}

int camera_detection_run_once(const char *rtsp_url, camera_detection_result_t *result)
{
    if (rtsp_url == NULL || rtsp_url[0] == '\0' || result == NULL)
    {
        printf("Camera: RTSP URL and result buffer are required\n");
        return -1;
    }

    if (capture_frame(rtsp_url) != 0)
    {
        return -1;
    }

    if (run_detection_demo() != 0)
    {
        return -1;
    }

    return parse_detection_output(result);
}
#endif
