/* $Id: utils.c,v 1.37 2003-03-13 21:34:38 rjkaes Exp $
 *
 * Misc. routines which are used by the various functions to handle strings
 * and memory allocation and pretty much anything else we can think of. Also,
 * the load cutoff routine is in here. Could not think of a better place for
 * it, so it's in here.
 *
 * Copyright (C) 1998       Steven Young
 * Copyright (C) 1999,2001  Robert James Kaes (rjkaes@flarenet.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include "tinyproxy.h"

#include "buffer.h"
#include "conns.h"
#include "filter.h"
#include "heap.h"
#include "log.h"
#include "network.h"
#include "sock.h"
#include "utils.h"


#define HEADER_SIZE (1024 * 8)
/*
 * Build the data for a complete HTTP & HTML message for the client.
 */
int
send_http_message(struct conn_s *connptr, int http_code,
		  const char *error_title, const char *message)
{
	static char *headers = \
		"HTTP/1.0 %d %s\r\n" \
		"Server: %s/%s\r\n" \
		"Date: %s\r\n" \
		"Content-Type: text/html\r\n" \
		"Content-Length: %d\r\n" \
		"Connection: close\r\n" \
		"\r\n";

	char timebuf[30];
	time_t global_time;
	size_t message_len = strlen(message);

	global_time = time(NULL);
	strftime(timebuf, sizeof(timebuf), "%a, %d %b %Y %H:%M:%S GMT",
		 gmtime(&global_time));

	if (write_message(connptr->client_fd,
			  headers,
			  http_code, error_title, PACKAGE, VERSION,
			  timebuf, message_len) < 0)
		return -1;

	return safe_write(connptr->client_fd, message, message_len);
}

/*
 * Safely creates filename and returns the low-level file descriptor.
 */
int
create_file_safely(const char *filename, unsigned int truncate_file)
{
	struct stat lstatinfo;
	int fildes;

	/*
	 * lstat() the file. If it doesn't exist, create it with O_EXCL.
	 * If it does exist, open it for writing and perform the fstat()
	 * check.
	 */
	if (lstat(filename, &lstatinfo) < 0) {
		/*
		 * If lstat() failed for any reason other than "file not
		 * existing", exit.
		 */
		if (errno != ENOENT) {
			fprintf(stderr,
				"%s: Error checking file %s: %s\n",
				PACKAGE, filename, strerror(errno));
			return -EACCES;
		}

		/*
		 * The file doesn't exist, so create it with O_EXCL to make
		 * sure an attacker can't slip in a file between the lstat()
		 * and open()
		 */
		if ((fildes =
		     open(filename, O_RDWR | O_CREAT | O_EXCL, 0600)) < 0) {
			fprintf(stderr,
				"%s: Could not create file %s: %s\n",
				PACKAGE, filename, strerror(errno));
			return fildes;
		}
	} else {
		struct stat fstatinfo;
		int flags;

		flags = O_RDWR;
		if (!truncate_file)
			flags |= O_APPEND;

		/*
		 * Open an existing file.
		 */
		if ((fildes = open(filename, flags)) < 0) {
			fprintf(stderr,
				"%s: Could not open file %s: %s\n",
				PACKAGE, filename, strerror(errno));
			return fildes;
		}

		/*
		 * fstat() the opened file and check that the file mode bits,
		 * inode, and device match.
		 */
		if (fstat(fildes, &fstatinfo) < 0
		    || lstatinfo.st_mode != fstatinfo.st_mode
		    || lstatinfo.st_ino != fstatinfo.st_ino
		    || lstatinfo.st_dev != fstatinfo.st_dev) {
			fprintf(stderr,
				"%s: The file %s has been changed before it could be opened\n",
				PACKAGE, filename);
			close(fildes);
			return -EIO;
		}

		/*
		 * If the above check was passed, we know that the lstat()
		 * and fstat() were done on the same file. Now we check that
		 * there's only one link, and that it's a normal file (this
		 * isn't strictly necessary because the fstat() vs lstat()
		 * st_mode check would also find this)
		 */
		if (fstatinfo.st_nlink > 1 || !S_ISREG(lstatinfo.st_mode)) {
			fprintf(stderr,
				"%s: The file %s has too many links, or is not a regular file: %s\n",
				PACKAGE, filename, strerror(errno));
			close(fildes);
			return -EMLINK;
		}

		/*
		 * Just return the file descriptor if we _don't_ want the file
		 * truncated.
		 */
		if (!truncate_file)
			return fildes;

		/*
		 * On systems which don't support ftruncate() the best we can
		 * do is to close the file and reopen it in create mode, which
		 * unfortunately leads to a race condition, however "systems
		 * which don't support ftruncate()" is pretty much SCO only,
		 * and if you're using that you deserve what you get.
		 * ("Little sympathy has been extended")
		 */
#ifdef HAVE_FTRUNCATE
		ftruncate(fildes, 0);
#else
		close(fildes);
		if ((fildes =
		     open(filename, O_RDWR | O_CREAT | O_TRUNC, 0600)) < 0) {
			fprintf(stderr,
				"%s: Could not open file %s: %s.",
				PACKAGE, filename, strerror(errno));
			return fildes;
		}
#endif				/* HAVE_FTRUNCATE */
	}

	return fildes;
}

/*
 * Write the PID of the program to the specified file.
 */
int
pidfile_create(const char *filename)
{
	int fildes;
	FILE *fd;

	/*
	 * Create a new file
	 */
	if ((fildes = create_file_safely(filename, TRUE)) < 0)
		return fildes;

	/*
	 * Open a stdio file over the low-level one.
	 */
	if ((fd = fdopen(fildes, "w")) == NULL) {
		fprintf(stderr,
			"%s: Could not write PID file %s: %s.",
			PACKAGE, filename, strerror(errno));
		close(fildes);
		unlink(filename);
		return -EIO;
	}

	fprintf(fd, "%ld\n", (long) getpid());
	fclose(fd);
	return 0;
}
