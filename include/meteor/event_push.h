/*
 * event_push.h — Lightweight HTTP client for pushing detection events and
 * FF binary files to the N100 receiver over a trusted LAN connection.
 *
 * Uses raw POSIX BSD sockets with no external dependencies.
 * HTTP/1.0 is used for simplicity (no chunked encoding required).
 */
#ifndef METEOR_EVENT_PUSH_H
#define METEOR_EVENT_PUSH_H

#include <stddef.h>

/* Connection parameters for the N100 HTTP receiver. */
typedef struct {
	char     server_ip[64]; /* flawfinder: ignore */
	int      server_port;
	int      timeout_ms;
} PushConfig;

/*
 * POST a NUL-terminated JSON string to /event on the receiver.
 * Returns 0 on success, -1 on network or HTTP error.
 */
int event_push_json(const PushConfig *cfg, const char *json_payload);

/*
 * POST any local file to an arbitrary endpoint on the receiver.
 *   endpoint     : URL path, e.g. "/ff" or "/stack"
 *   content_type : MIME type, e.g. "application/octet-stream" or "image/jpeg"
 *   filepath     : local path to the file (must exist)
 *   filename     : basename sent in X-Filename header
 * Returns 0 on success, -1 on error.
 */
int event_push_file(const PushConfig *cfg, const char *endpoint,
		    const char *content_type, const char *filepath,
		    const char *filename);

/* Convenience wrapper: POST an FF binary to /ff. */
int event_push_ff(const PushConfig *cfg, const char *ff_path,
		  const char *filename);

/* Convenience wrapper: POST a JPEG stack frame to /stack. */
int event_push_stack(const PushConfig *cfg, const char *jpeg_path,
		     const char *filename);

#endif /* METEOR_EVENT_PUSH_H */
