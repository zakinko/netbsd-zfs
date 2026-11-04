/*
 * Open and close the nodes of one unit from many threads at once, as the
 * import scan of zpool(8) does to everything under /dev.
 *
 *   race SECONDS NODE...
 */
#include <err.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static time_t end;

static void *
run(void *arg)
{
	const char *path = arg;
	unsigned long n = 0;
	int fd;

	while ((n++ & 1023) != 0 || time(NULL) < end) {
		if ((fd = open(path, O_RDONLY | O_NONBLOCK)) >= 0)
			close(fd);
	}
	return (void *)n;
}

int
main(int argc, char **argv)
{
	pthread_t t[256];
	unsigned long total = 0;
	void *r;
	int i, n = 0, j;

	if (argc < 3)
		errx(1, "usage");
	end = time(NULL) + atoi(argv[1]);
	for (j = 0; j < 8; j++)
		for (i = 2; i < argc && n < 256; i++)
			if (pthread_create(&t[n], NULL, run, argv[i]) == 0)
				n++;
	for (i = 0; i < n; i++) {
		pthread_join(t[i], &r);
		total += (unsigned long)r;
	}
	printf("%d threads, %lu opens\n", n, total);
	return 0;
}
