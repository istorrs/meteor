#ifndef METEOR_ISP_H
#define METEOR_ISP_H

/* Initialize the ISP: open, add GC2053 sensor, enable sensor, enable tuning. */
int meteor_isp_init(void);

/* Tear down the ISP in reverse order. */
int meteor_isp_exit(void);

/* Set ISP running mode (day / night). */
int meteor_isp_set_running_mode(int night);

#endif /* METEOR_ISP_H */
