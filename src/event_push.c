#include <meteor/event_push.h>
#include <meteor/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define HTTP_BUF_SZ   4096
#define HTTP_SEND_SZ  8192

/* Open a blocking TCP socket to server and set SO_RCVTIMEO / SO_SNDTIMEO. */
static int open_socket(const PushConfig *cfg)
{
	struct sockaddr_in addr;
	struct timeval tv;
	int fd;
	int ret;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	tv.tv_sec  = (long)(cfg->timeout_ms / 1000);
	tv.tv_usec = (long)((cfg->timeout_ms % 1000) * 1000);
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	(void)memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons((uint16_t)cfg->server_port);

	ret = inet_pton(AF_INET, cfg->server_ip, &addr.sin_addr);
	if (ret != 1) {
		(void)close(fd);
		return -1;
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		(void)close(fd);
		return -1;
	}

	return fd;
}

/*
 * Send all bytes from buf.  Returns 0 on success, -1 on error.
 */
static int send_all(int fd, const char *buf, size_t len)
{
	size_t sent = 0;

	while (sent < len) {
		ssize_t n = send(fd, buf + sent, len - sent, 0);

		if (n <= 0)
			return -1;
		sent += (size_t)n;
	}
	return 0;
}

int event_push_json(const PushConfig *cfg, const char *json_payload)
{
	char   hdr[HTTP_BUF_SZ]; /* flawfinder: ignore */
	size_t body_len;
	int    fd;
	int    rc = 0;

	body_len = strlen(json_payload);

	(void)snprintf(hdr, sizeof(hdr),
		"POST /event HTTP/1.0\r\n"
		"Host: %s:%d\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n"
		"\r\n",
		cfg->server_ip, cfg->server_port, body_len);

	fd = open_socket(cfg);
	if (fd < 0) {
		METEOR_LOG_WARN("event_push: cannot connect to %s:%d",
				cfg->server_ip, cfg->server_port);
		return -1;
	}

	if (send_all(fd, hdr, strlen(hdr)) < 0 ||
	    send_all(fd, json_payload, body_len) < 0) {
		METEOR_LOG_WARN("event_push: send failed");
		rc = -1;
	}

	(void)close(fd);
	return rc;
}

int event_push_file(const PushConfig *cfg, const char *endpoint,
		    const char *content_type, const char *filepath,
		    const char *filename)
{
	struct stat st;
	char        hdr[HTTP_BUF_SZ];      /* flawfinder: ignore */
	char        send_buf[HTTP_SEND_SZ]; /* flawfinder: ignore */
	FILE       *f;
	int         fd;
	int         rc = 0;

	if (stat(filepath, &st) < 0) {
		METEOR_LOG_WARN("event_push_file: cannot stat %s", filepath);
		return -1;
	}

	(void)snprintf(hdr, sizeof(hdr),
		"POST %s HTTP/1.0\r\n"
		"Host: %s:%d\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %lld\r\n"
		"X-Filename: %s\r\n"
		"Connection: close\r\n"
		"\r\n",
		endpoint, cfg->server_ip, cfg->server_port,
		content_type, (long long)st.st_size, filename);

	fd = open_socket(cfg);
	if (fd < 0) {
		METEOR_LOG_WARN("event_push_file: cannot connect to %s:%d",
				cfg->server_ip, cfg->server_port);
		return -1;
	}

	if (send_all(fd, hdr, strlen(hdr)) < 0) {
		METEOR_LOG_WARN("event_push_file: header send failed");
		rc = -1;
		goto done;
	}

	f = fopen(filepath, "rb"); /* flawfinder: ignore */
	if (!f) {
		METEOR_LOG_WARN("event_push_file: cannot open %s", filepath);
		rc = -1;
		goto done;
	}

	{
		size_t nr;

		while (!feof(f) && !ferror(f)) {
			nr = fread(send_buf, 1, sizeof(send_buf), f);
			if (nr == 0)
				break;
			if (send_all(fd, send_buf, nr) < 0) {
				METEOR_LOG_WARN("event_push_file: send failed");
				rc = -1;
				break;
			}
		}
		if (ferror(f)) {
			METEOR_LOG_WARN("event_push_file: read error");
			rc = -1;
		}
	}

	if (fclose(f) != 0)
		rc = -1;

done:
	(void)close(fd);
	return rc;
}

int event_push_ff(const PushConfig *cfg, const char *ff_path,
		  const char *filename)
{
	return event_push_file(cfg, "/ff", "application/octet-stream",
			       ff_path, filename);
}

int event_push_stack(const PushConfig *cfg, const char *jpeg_path,
		     const char *filename)
{
	return event_push_file(cfg, "/stack", "image/jpeg",
			       jpeg_path, filename);
}
