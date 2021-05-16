/*
 * atomic access latency tests - Willy Tarreau - 2021/04/26
 *
 * Compilation :
 *  $ gcc -pthread -O3 -o stress stress.c
 *
 * Execution :
 *  $ ./stress -t 16 -r 1
 */

#define _GNU_SOURCE // for sched + cpu_set
#include <sys/time.h>
#include <ctype.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>

#define MAXTHREADS	64

static volatile unsigned int step __attribute__((aligned(64)));
static struct timeval start, stop;
static volatile unsigned int actthreads;

static cpu_set_t cpu_mask;
static cpu_set_t aff[MAXTHREADS];

/* the flag array all threads compete on. Each one tries to flip its own bit
 * as often as possible.
 */
static unsigned long flag __attribute__((aligned(64)));
static struct { unsigned long tid __attribute__((aligned(64))); } runners[MAXTHREADS];

/* per-thread stats */
static struct {
	unsigned long ready;
	unsigned long done;
} stats[MAXTHREADS] __attribute__((aligned(64)));

pthread_t thr[MAXTHREADS];
unsigned int nbthreads;
unsigned int arg_run = 1;
unsigned int arg_relax = 0;
unsigned long arg_tgmask = 0UL;

volatile static unsigned long total __attribute__((aligned(64))) = 0;

#if defined(__x86_64__)
//#define cpu_relax() ({ asm volatile("rep;nop\n"); 1; })
#define cpu_relax() ({ asm volatile("xchg %rax,%rdx; xchg %rax,%rdx;xchg %rax,%rdx; xchg %rax,%rdx\n"); 1; })
#elif defined(__aarch64__)
#define cpu_relax() ({ asm volatile("isb"); 1; })
#else
#warning "No cpu_relax() implemented on this platform"
#define cpu_relax() do { } while (0)
#endif

/* display the message and exit with the code */
__attribute__((noreturn)) void die(int code, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	exit(code);
}

#define rdtscll(val) \
     __asm__ __volatile__("rdtsc" : "=A" (val))

static inline struct timeval *tv_now(struct timeval *tv)
{
        gettimeofday(tv, NULL);
        return tv;
}

static inline unsigned long tv_ms_elapsed(const struct timeval *tv1, const struct timeval *tv2)
{
        unsigned long ret;

        ret  = ((signed long)(tv2->tv_sec  - tv1->tv_sec))  * 1000;
        ret += ((signed long)(tv2->tv_usec - tv1->tv_usec)) / 1000;
        return ret;
}

unsigned int rnd32()
{
        static unsigned int y = 1;

        y ^= y << 13;
        y ^= y >> 17;
        y ^= y << 5;
        return y;
}

unsigned int rnd32range(unsigned int range)
{
        unsigned long long res = rnd32();

        res *= (range + 1);
        return res >> 32;
}

void run(void *arg)
{
	const int tid = (long)arg;
	const int next = (tid + 1) % nbthreads;
	unsigned long loops = 0;
	const unsigned long bit = 1UL << tid;
	const unsigned long tgmask = ~arg_tgmask;
	unsigned long old;

	sched_setaffinity(0, sizeof(aff[tid]), &aff[tid]);

	/* step 0: create all threads */
	while (step == 0) {
		/* don't disturb pthread_create() */
		usleep(10000);
	}

	/* step 1 : wait for signal to start */
	__sync_fetch_and_add(&actthreads, 1);
	while (step == 1)
		;

	/* step 2 : run */
	if (arg_relax == 0) {
		/* no CPU relax in the loop */
		do {
			do {
				old = tid; // only leave when another thread placed our ID there
			} while (!__atomic_compare_exchange_n(&runners[tid & tgmask].tid, &old, -1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
			loops++;
			__atomic_store_n(&runners[next & tgmask].tid, next, __ATOMIC_RELEASE);
		} while (step == 2);
	}
	else if (arg_relax == 1) {
		/* CPU relax in the loop */
		do {
			do {
				old = tid; // only leave when another thread placed our ID there
			} while (!__atomic_compare_exchange_n(&runners[tid & tgmask].tid, &old, -1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && ({ cpu_relax(); 1; }));
			loops++;
			__atomic_store_n(&runners[next & tgmask].tid, next, __ATOMIC_RELEASE);
		} while (step == 2);
	}
	else if (arg_relax == 2) {
		/* no CPU relax in the loop, use xchg */
		do {
			while (__atomic_exchange_n(&runners[tid & tgmask].tid, -1, __ATOMIC_RELAXED) != tid)
				;
			loops++;
			__atomic_store_n(&runners[next & tgmask].tid, next, __ATOMIC_RELEASE);
		} while (step == 2);
	}
	else if (arg_relax == 3) {
		/* CPU relax in the loop, use xchg */
		do {
			while (__atomic_exchange_n(&runners[tid & tgmask].tid, -1, __ATOMIC_RELAXED) != tid)
				cpu_relax();
			loops++;
			__atomic_store_n(&runners[next & tgmask].tid, next, __ATOMIC_RELEASE);
		} while (step == 2);
	}

	fprintf(stderr, "thread %2d quitting after %lu loops\n", tid, loops);
	stats[tid].done = loops;

	/* step 3 : stop */
	__sync_fetch_and_sub(&actthreads, 1);
	pthread_exit(0);
}

/* stops all threads upon SIG_ALRM */
void alarm_handler(int sig)
{
	fprintf(stderr, "received signal %d\n", sig);
	step = 3;
}

void usage(int ret)
{
	die(ret, "usage: stress [-h] [-t threads] [-g tgmask] [ -r relax ] [-d run_time] [thr:cpu]*\n");
}

int main(int argc, char **argv)
{
	int i, err, cnt;
	unsigned int u;
	unsigned long done, done_min, done_max;
	unsigned long loops, loops_min, loops_max;
	unsigned long min, max;

	sched_getaffinity(0, sizeof(cpu_mask), &cpu_mask);

	/* look for the Nth CPU bound in the process' mask and use this one to
	 * bind the current thread.
	 */
	for (i = cnt = 0; i < CPU_SETSIZE; i++) {
		if (cnt < MAXTHREADS && CPU_ISSET(i, &cpu_mask)) {
			CPU_ZERO(&aff[cnt]);
			CPU_SET(i, &aff[cnt]);
			cnt++;
		}
	}

	nbthreads = CPU_COUNT(&cpu_mask);

	argc--; argv++;
	while (argc > 0) {
		if (!strcmp(*argv, "-t")) {
			if (--argc < 0)
				usage(1);
			nbthreads = atol(*++argv);
		}
		else if (!strcmp(*argv, "-g")) {
			/* thread group mask */
			if (--argc < 0)
				usage(1);
			arg_tgmask = atol(*++argv);
		}
		else if (!strcmp(*argv, "-d")) {
			if (--argc < 0)
				usage(1);
			arg_run = atol(*++argv);
		}
		else if (!strcmp(*argv, "-r")) {
			if (--argc < 0)
				usage(1);
			arg_relax = atol(*++argv);
		}
		else if (!strcmp(*argv, "-h"))
			usage(0);
		else if (isdigit((unsigned char)**argv)) {
			unsigned int thr, cpu;
			char *sep = strchr(*argv, ':');
			if (!sep)
				usage(1);
			sep++;
			thr = atoi(*argv);
			cpu = atoi(sep);
			if (thr < MAXTHREADS && cpu < CPU_SETSIZE) {
				CPU_ZERO(&aff[thr]);
				CPU_SET(cpu, &aff[thr]);
			}
			else
				usage(1);
		}
		else
			usage(1);
		argc--; argv++;
	}

	if (nbthreads >= MAXTHREADS)
		nbthreads = MAXTHREADS;

	actthreads = 0;	step = 0;

	if (arg_run <= 0)
		arg_run = 1;

	printf("Starting %d thread%c\n", nbthreads, (nbthreads > 1)?'s':' ');

	for (u = 0; u < nbthreads; u++) {
		err = pthread_create(&thr[u], NULL, (void *)&run, (void *)(long)u);
		if (err)
			die(1, "pthread_create(): %s\n", strerror(err));
	}

	__sync_fetch_and_add(&step, 1);  /* let the threads warm up and get ready to start */

	while (actthreads != nbthreads)
		;

	signal(SIGALRM, alarm_handler);
	alarm(arg_run);

	gettimeofday(&start, NULL);
	__sync_fetch_and_add(&step, 1); /* fire ! */
	__atomic_add_fetch(&stats[0].ready, 1, __ATOMIC_RELEASE);

	/* and wait for all threads to die */

	done = loops = min = max = 0;
	for (u = 0; u < nbthreads; u++) {
		pthread_join(thr[u], NULL);
		done += stats[u].done;
	}

	gettimeofday(&stop, NULL);

	i = (stop.tv_usec - start.tv_usec);
	while (i < 0) {
		i += 1000000;
		start.tv_sec++;
	}

	total = done;
	i = i / 1000 + (int)(stop.tv_sec - start.tv_sec) * 1000;
	printf("threads: %d done: %lu time(ms): %u rate: %lld/s ns: %lld\n",
	       nbthreads, total, i, total * 1000ULL / (unsigned)i, (unsigned)i * 1000000ULL / total);

	/* All the work has ended */

	exit(0);
}
