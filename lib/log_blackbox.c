/*
 * Copyright (C) 2011 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Angus Salkeld <asalkeld@redhat.com>
 *
 * libqb is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * libqb is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libqb.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "os_base.h"

#include <qb/qbrb.h>
#include "util_int.h"
#include "log_int.h"

static void
_blackbox_reload(int32_t target)
{
	struct qb_log_target *t = qb_log_target_get(target);

	if (t->instance == NULL) {
		return;
	}
	qb_rb_close(t->instance);
	t->instance = qb_rb_open(t->filename, t->size,
				 QB_RB_FLAG_CREATE | QB_RB_FLAG_OVERWRITE, 0);
}

/* <u32> file lineno
 * <u32> function name length
 * <string> function name
 * <u32> buffer length
 * <string> buffer
 */
static void
_blackbox_vlogger(int32_t target,
		  struct qb_log_callsite *cs, time_t timestamp, va_list ap)
{
	size_t max_size;
	size_t actual_size;
	size_t fn_size;
	char *chunk;
	struct qb_log_target *t = qb_log_target_get(target);

	if (t->instance == NULL) {
		return;
	}

	fn_size = strlen(cs->function) + 1;

	actual_size = 2 * sizeof(uint32_t) + fn_size + sizeof(time_t);
	max_size = actual_size + QB_LOG_MAX_LEN;

	chunk = qb_rb_chunk_alloc(t->instance, max_size);

	/* line number */
	memcpy(chunk, &cs->lineno, sizeof(uint32_t));
	chunk += sizeof(uint32_t);

	/* function name */
	memcpy(chunk, &fn_size, sizeof(uint32_t));
	chunk += sizeof(uint32_t);
	memcpy(chunk, cs->function, fn_size);
	chunk += fn_size;

	/* timestamp */
	memcpy(chunk, &timestamp, sizeof(time_t));
	chunk += sizeof(time_t);

	/* log message */
	actual_size += qb_vsprintf_serialize(chunk, cs->format, ap);

	(void)qb_rb_chunk_commit(t->instance, actual_size);
}

static void
_blackbox_close(int32_t target)
{
	struct qb_log_target *t = qb_log_target_get(target);

	if (t->instance) {
		qb_rb_close(t->instance);
		t->instance = NULL;
	}
}

int32_t
qb_log_blackbox_open(struct qb_log_target *t)
{
	if (t->size < 1024) {
		return -EINVAL;
	}
	snprintf(t->filename, PATH_MAX, "%s-blackbox", t->name);

	t->instance = qb_rb_open(t->filename, t->size,
				 QB_RB_FLAG_CREATE | QB_RB_FLAG_OVERWRITE, 0);
	if (t->instance == NULL) {
		return -errno;
	}

	t->logger = NULL;
	t->vlogger = _blackbox_vlogger;
	t->reload = _blackbox_reload;
	t->close = _blackbox_close;
	return 0;
}

ssize_t
qb_log_blackbox_write_to_file(const char *filename)
{
	ssize_t written_size = 0;
	struct qb_log_target *t;
	int fd = open(filename, O_CREAT | O_RDWR, 0700);

	if (fd < 0) {
		return -errno;
	}
	t = qb_log_target_get(QB_LOG_BLACKBOX);
	if (t->instance) {
		written_size = qb_rb_write_to_file(t->instance, fd);
	} else {
		written_size = -ENOENT;
	}
	close(fd);

	return written_size;
}

void
qb_log_blackbox_print_from_file(const char *bb_filename)
{
	qb_ringbuffer_t *instance;
	ssize_t bytes_read;
	char chunk[512];
	int fd;
	char time_buf[64];

	fd = open(bb_filename, 0);
	if (fd < 0) {
		qb_util_perror(LOG_ERR, "qb_log_blackbox_print_from_file");
		return;
	}
	instance = qb_rb_create_from_file(fd, 0);
	close(fd);
	if (instance == NULL) {
		return;
	}

	do {
		char *ptr;
		uint32_t lineno;
		uint32_t fn_size;
		char *function;
		uint32_t len;
		time_t timestamp;
		char message[QB_LOG_MAX_LEN];
		uint32_t u32;

		bytes_read = qb_rb_chunk_read(instance, chunk, 512, 0);
		ptr = chunk;
		if (bytes_read > 0) {
			struct tm *tm;
			/* lineno */
			memcpy(&lineno, ptr, sizeof(uint32_t));
			ptr += sizeof(uint32_t);

			/* function size & name */
			memcpy(&fn_size, ptr, sizeof(uint32_t));
			ptr += sizeof(uint32_t);

			function = ptr;
			ptr += fn_size;

			/* timestamp size & content */
			memcpy(&u32, ptr, sizeof(uint32_t));
			timestamp = u32;
			ptr += sizeof(time_t);
			tm = localtime(&timestamp);
			if (tm) {
				(void)strftime(time_buf,
					       sizeof(time_buf), "%b %d %T",
					       tm);
			} else {
				snprintf(time_buf, sizeof(time_buf), "%ld",
					 (long int)timestamp);
			}
			/* message content */
			len = qb_vsnprintf_deserialize(message, QB_LOG_MAX_LEN, ptr);
			len--;
			while (message[len] == '\n' || message[len] == '\0') {
				message[len] = '\0';
				len--;
			}
			printf("%s %s(%d): %s\n",
			       time_buf, function, lineno, message);
		}
	} while (bytes_read > 0);
}
