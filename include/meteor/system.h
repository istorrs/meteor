#ifndef METEOR_SYSTEM_H
#define METEOR_SYSTEM_H

#include <imp/imp_system.h>

/* Initialize the IMP system. Must be called first. */
int meteor_system_init(void);

/* Shut down the IMP system. Call after all modules are torn down. */
int meteor_system_exit(void);

/* Bind a source cell output to a destination cell input. */
int meteor_system_bind(IMPCell *src, IMPCell *dst);

/* Unbind a previously bound source/destination pair. */
int meteor_system_unbind(IMPCell *src, IMPCell *dst);

#endif /* METEOR_SYSTEM_H */
