/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
 *
 * The person who associated a work with this file has dedicated the work
 * to the public domain by waiving all of his or her rights to the work
 * worldwide under copyright law, including all related and neighboring
 * rights, to the extent allowed by law.
 *
 * You can copy, modify, distribute, and perform this work, even for
 * commercial purposes, all without asking permission.
 *
 * More info: https://creativecommons.org/publicdomain/zero/1.0/
 */

#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int rand_fd = -1;

static pthread_barrier_t start_barrier;

static size_t *alloc_bins;		// Allocation bins (bytes)
static size_t alloc_bin_count = 0;	// Number of bins

static size_t concurrent_allocs = 0;	// Number of simultaneous allocations
static size_t max_allocs = 0;		// Maximum total number of allocations

typedef struct {
	size_t bin;
	double alloc_elapsed;
	double free_elapsed;
} stats_t;

static inline void *xcalloc(size_t nelemb, size_t size)
{
	void *ptr = calloc(nelemb, size);
	if(ptr == NULL)
	{
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	return ptr;
}

static inline void rand_bytes(uint8_t *data, size_t len)
{
	if(read(rand_fd, data, len) != (ssize_t)len)
	{
		perror("read");
		exit(EXIT_FAILURE);
	}
}

static inline size_t generate_rng_size_range(size_t min, size_t max)
{
	size_t range = max - min + 1;
	size_t x;

	size_t limit = SIZE_MAX - (SIZE_MAX % range);

	do
	{
		rand_bytes((uint8_t *)&x, sizeof(x));
	} while(x >= limit);

	return min + (x % range);
}

static inline int parse_size(const char *s, size_t *out) {
	if(!s || !*s)
	{
		return -1;
	}

	if(s[0] == '+' || s[0] == '-')
	{
		return -1;
	}

	errno = 0;
	char *end = NULL;
	unsigned long long v = strtoull(s, &end, 10);

	if(errno == ERANGE || v > SIZE_MAX)
	{
		return -1;
	}

	if(end == s || *end != '\0')
	{
		return -1;
	}

	if(v == 0)
	{
		return -1;
	}

	*out = (size_t)v;
	return 0;
}

static size_t *parse_csv_sizes(const char *arg, size_t *count_out)
{
	if(!arg)
	{
		return NULL;
	}

	char *buf = strdup(arg);
	if(!buf)
	{
		return NULL;
	}

	size_t cap = 8, count = 0;
	size_t *arr = malloc(cap * sizeof(*arr));
	if(!arr)
	{
		free(buf);
		return NULL;
	}

	char *saveptr = NULL;
	char *token = strtok_r(buf, ",", &saveptr);

	while(token)
	{
		size_t val;
		if(parse_size(token, &val) != 0)
		{
			free(arr);
			free(buf);
			return NULL;
		}

		if(count == cap)
		{
			cap *= 2;
			size_t *tmp = realloc(arr, cap * sizeof *tmp);
			if(!tmp)
			{
				perror("realloc");
				free(arr);
				free(buf);
				return NULL;
			}
			arr = tmp;
		}

		arr[count++] = val;
		token = strtok_r(NULL, ",", &saveptr);
	}

	free(buf);
	*count_out = count;
	return arr;
}

void* allocate_thread(void* arg)
{
	int ret = pthread_barrier_wait(&start_barrier);
	if(ret != 0 && ret != PTHREAD_BARRIER_SERIAL_THREAD)
	{
		perror("pthread_barrier_wait");
		return NULL;
	}

	stats_t *stat = arg;

	size_t allocs = 0;
	while(allocs < max_allocs)
	{
		uint8_t *data[concurrent_allocs];
		struct timespec start, end;

		size_t bin = generate_rng_size_range(0, alloc_bin_count - 1);
		size_t stat_idx = allocs / concurrent_allocs;
		stat[stat_idx].bin = bin;

		clock_gettime(CLOCK_MONOTONIC, &start);
		for(size_t i = 0; i < concurrent_allocs; i++, allocs++)
		{
			data[i] = malloc(alloc_bins[bin] * sizeof(uint8_t));
			if(data[i] == NULL)
			{
				perror("malloc");
				return NULL;
			}

			// Touch data so it's not optimised out
			*(data[i] + alloc_bins[bin] - 1) = 0;
		}
		clock_gettime(CLOCK_MONOTONIC, &end);
		stat[stat_idx].alloc_elapsed =
			(end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

		clock_gettime(CLOCK_MONOTONIC, &start);
		for(size_t i = 0; i < concurrent_allocs; i++)
		{
			free(data[i]);
		}
		clock_gettime(CLOCK_MONOTONIC, &end);
		stat[stat_idx].free_elapsed =
			(end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
	}

	return NULL;
}

[[noreturn]] static void bad_arg(const char *arg0)
{
	fprintf(stderr,
		"Usage: %s [-t THREADS] [-c CONCURRENT_ALLOCS] [-n NUM_CONCURRENT_ALLOCS] [-b BINS]\n",
		arg0);
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	size_t nthreads = 0;

	int c, ret;
	size_t num_bins = 0;
	while((c = getopt(argc, argv, "t:c:n:b:")) != -1)
	{
		switch(c)
		{
		case 't':
			ret = parse_size(optarg, &nthreads);
			if(ret != 0)
			{
				fprintf(stderr, "Number of threads must be an integer > 0 <= %zu\n", SIZE_MAX);
				bad_arg(argv[0]);
				// Not reached
			}
			break;
		case 'c':
			ret = parse_size(optarg, &concurrent_allocs);
			if(ret != 0)
			{
				fprintf(stderr,
					"Number of concurrent allocations must be an integer > 0 <= %zu\n",
					SIZE_MAX);
				bad_arg(argv[0]);
				// Not reached
			}
			break;
		case 'n':
			ret = parse_size(optarg, &num_bins);
			if(ret != 0)
			{
				fprintf(stderr,
					"Number of concurrent allocation batches must be an integer > 0 <= %zu\n",
					SIZE_MAX);
				bad_arg(argv[0]);
				// Not reached
			}
			break;
		case 'b':
			if((alloc_bins = parse_csv_sizes(optarg, &alloc_bin_count)) == NULL)
			{
				fprintf(stderr,
					"Bins must be comma-separated integers > 0 <= %zu\n",
					SIZE_MAX);
				bad_arg(argv[0]);
				// Not reached
			}
			break;
		default:
			bad_arg(argv[0]);
			break;
			// Not reached
		}
	}

	if(nthreads == 0)
	{
		long nproc = sysconf(_SC_NPROCESSORS_ONLN);
		if(nproc < 0)
		{
			perror("sysconf");
			return EXIT_FAILURE;
		}
		else if(nproc == 0)
		{
			fprintf(stderr, "Could not get CPU count");
			return EXIT_FAILURE;
		}

		nthreads = (size_t)nproc;
	}

	if(concurrent_allocs == 0)
	{
		concurrent_allocs = 64;
	}

	if(num_bins == 0)
	{
		num_bins = 16384;
	}

	max_allocs = concurrent_allocs * num_bins;

	if(alloc_bins == NULL || alloc_bin_count == 0)
	{
		alloc_bin_count = 4;
		alloc_bins = malloc(alloc_bin_count * sizeof(*alloc_bins));
		alloc_bins[0] = 32;
		alloc_bins[1] = 1024;
		alloc_bins[2] = 32768;
		alloc_bins[3] = 1048576;
	}

	printf("Threads: %zu\n", nthreads);
	printf("Bins: %zu (", alloc_bin_count);
	for(size_t i = 0; i < alloc_bin_count - 1; i++)
	{
		printf("%zu, ", alloc_bins[i]);
	}
	printf("%zu)\n", alloc_bins[alloc_bin_count - 1]);
	printf("Concurrent allocations: %zu per thread (%zu peak total)\n",
		concurrent_allocs, concurrent_allocs * nthreads);
	printf("Maximum allocations: %zu per thread (%zu total)\n",
		max_allocs, max_allocs * nthreads);

	rand_fd = open("/dev/urandom", O_RDONLY);
	if(rand_fd < 0)
	{
		perror("open");
		return EXIT_FAILURE;
	}

	if(pthread_barrier_init(&start_barrier, NULL, (unsigned int)nthreads) != 0)
	{
		perror("pthread_barrier_init");
		return EXIT_FAILURE;
	}

	const size_t per_thread_count = (max_allocs / concurrent_allocs) + (max_allocs % concurrent_allocs != 0);
	size_t stats_count = nthreads * per_thread_count;
	stats_t *stats = xcalloc(stats_count, sizeof(stats_t));

	pthread_attr_t attr;
	if(pthread_attr_init(&attr) != 0)
	{
		perror("pthread_attr_init");
		return EXIT_FAILURE;
	}

	// 8MB stack size
	if(pthread_attr_setstacksize(&attr, 1048576 * 8) != 0)
	{
		perror("pthread_attr_setstacksize");
		return EXIT_FAILURE;
	}

	pthread_t thread_ids[nthreads];
	for(size_t i = 0; i < nthreads; i++)
	{
		stats_t *stats_ptr = stats + per_thread_count * i;
		if(pthread_create(&thread_ids[i], &attr, allocate_thread, stats_ptr) != 0)
		{
			perror("pthread_create");
			return EXIT_FAILURE;
		}
	}

	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for(size_t i = 0; i < nthreads; i++)
	{
		if(pthread_join(thread_ids[i], NULL) != 0)
		{
			// Don't stop joining for this
			perror("pthread_join");
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	double total_time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

	double *alloc_average = xcalloc(alloc_bin_count, sizeof(*alloc_average));
	double *free_average = xcalloc(alloc_bin_count, sizeof(*free_average));
	double *alloc_M2 = xcalloc(alloc_bin_count, sizeof(*alloc_M2));
	double *free_M2 = xcalloc(alloc_bin_count, sizeof(*free_M2));
	size_t *alloc_count = xcalloc(alloc_bin_count, sizeof(*alloc_count));
	size_t *free_count = xcalloc(alloc_bin_count, sizeof(*free_count));
	size_t *bin_count = xcalloc(alloc_bin_count, sizeof(*bin_count));

	double global_alloc_average = 0.0;
	double global_free_average = 0.0;
	double global_alloc_M2 = 0.0;
	double global_free_M2 = 0.0;

	for(size_t i = 0; i < stats_count; i++)
	{
		// Global stats //

		// alloc
		double delta_alloc = stats[i].alloc_elapsed - global_alloc_average;
		global_alloc_average += delta_alloc / (i + 1);
		global_alloc_M2 += delta_alloc * (stats[i].alloc_elapsed - global_alloc_average);

		// free
		double delta_free = stats[i].free_elapsed - global_free_average;
		global_free_average += delta_free / (i + 1);
		global_free_M2 += delta_free * (stats[i].free_elapsed - global_free_average);

		// Per-bin //
		size_t bin = stats[i].bin;
		bin_count[bin]++;

		// alloc
		double delta_bin_alloc = stats[i].alloc_elapsed - alloc_average[bin];
		alloc_average[bin] += delta_bin_alloc / bin_count[bin];
		alloc_M2[bin] += delta_bin_alloc * (stats[i].alloc_elapsed - alloc_average[bin]);

		// free
		double delta_bin_free = stats[i].free_elapsed - free_average[bin];
		free_average[bin] += delta_bin_free / bin_count[bin];
		free_M2[bin] += delta_bin_free * (stats[i].free_elapsed - free_average[bin]);
	}

	double global_alloc_variance = global_alloc_M2 / (stats_count - 1);
	double global_free_variance = global_free_M2 / (stats_count - 1);

	double global_alloc_stdev = sqrt(global_alloc_variance);
	double global_free_stdev = sqrt(global_free_variance);

	printf("\n");
	printf("Total time: %fs, %fus each avg\n", total_time, (total_time / (max_allocs * nthreads)) * 1000000);
	printf("Alloc average: %fus per %zu, %fus each avg\n",
		global_alloc_average * 1000000, concurrent_allocs, (global_alloc_average / concurrent_allocs) * 1000000);
	printf("Alloc stdev: %fus\n", global_alloc_stdev * 1000000);
	printf("Free average: %fus per %zu, %fus each avg\n",
		global_free_average * 1000000, concurrent_allocs, (global_free_average / concurrent_allocs) * 1000000);
	printf("Free stdev: %fus\n", global_free_stdev * 1000000);
	printf("\n");

	printf("Per-bin stats:\n");
	for(size_t i = 0; i < alloc_bin_count; i++)
	{
		double alloc_variance = alloc_M2[i] / (bin_count[i] - 1);
		double free_variance = free_M2[i] / (bin_count[i] - 1);

		double alloc_stdev = sqrt(alloc_variance);
		double free_stdev = sqrt(free_variance);

		printf("%zu:\n", alloc_bins[i]);
		printf("Alloc average: %fus per %zu, %fus each avg\n",
			alloc_average[i] * 1000000, concurrent_allocs,
			(alloc_average[i] / concurrent_allocs) * 1000000);
		printf("Alloc stdev: %fus\n", alloc_stdev * 1000000);
		printf("Free average: %fus per %zu, %fus each avg\n",
			free_average[i] * 1000000, concurrent_allocs,
			(free_average[i] / concurrent_allocs) * 1000000);
		printf("Free stdev: %fus\n", free_stdev * 1000000);
		printf("\n");
	}

	printf("\n");
	printf("Done.\n");

	if(pthread_barrier_destroy(&start_barrier) != 0)
	{
		// Don't stop the teardown for this
		perror("pthread_barrier_destroy");
	}

	close(rand_fd);
	free(stats);
	free(alloc_bins);
	free(alloc_average);
	free(free_average);
	free(alloc_M2);
	free(free_M2);
	free(alloc_count);
	free(free_count);
	free(bin_count);

	return EXIT_SUCCESS;
}
