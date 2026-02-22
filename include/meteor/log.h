#ifndef METEOR_LOG_H
#define METEOR_LOG_H

#include <stdio.h>

#define METEOR_TAG "METEOR"

void meteor_log_init(void);

#define METEOR_LOG_INFO(fmt, ...) \
    (void)fprintf(stdout, "[INFO ] " METEOR_TAG ": " fmt "\n", ##__VA_ARGS__)

#define METEOR_LOG_WARN(fmt, ...) \
    (void)fprintf(stderr, "[WARN ] " METEOR_TAG ": " fmt "\n", ##__VA_ARGS__)

#define METEOR_LOG_ERR(fmt, ...) \
    (void)fprintf(stderr, "[ERROR] " METEOR_TAG ": " fmt "\n", ##__VA_ARGS__)

#define METEOR_LOG_DBG(fmt, ...) \
    (void)fprintf(stdout, "[DEBUG] " METEOR_TAG ": " fmt "\n", ##__VA_ARGS__)

#endif /* METEOR_LOG_H */
