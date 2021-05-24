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
#include <inttypes.h>
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

/* shared data all threads are working on */
struct shared_data {
	unsigned long counter;
	unsigned int rnd;
} __attribute__((aligned(64)));

/* one thread context */
struct thread_ctx {
	unsigned long tid;
	unsigned long tid_bit;
	unsigned long next;
	pthread_t thr;
	unsigned long long tot_done;
	unsigned long long tot_wait;
	unsigned long long max_wait;
} __attribute__((aligned(64)));

static struct thread_ctx runners[MAXTHREADS];
static struct shared_data shared = { .counter = 0, .rnd = 1 };

unsigned int nbthreads;
unsigned int arg_op = 0;
unsigned int arg_run = 1;
unsigned int arg_meas = 0;
unsigned int arg_relax = 0;
unsigned long arg_tgmask = 0UL;

volatile static unsigned long total __attribute__((aligned(64))) = 0;


/* long CPU relaxation, yields the control to the sibling thread */
#if defined(__x86_64__)
#define cpu_relax_long() ({ asm volatile("rep;nop\n"); 1; })
#elif defined(__aarch64__)
#define cpu_relax_long() ({ asm volatile("isb"); 1; })
#else
#warning "No cpu_relax_long() implemented on this platform"
#define cpu_relax_long() do { } while (0)
#endif

/* short CPU relaxation, only avoid using the ALU for a few cycles */
#if defined(__x86_64__)
#define cpu_relax_short() ({ asm volatile("xchg %rax,%rdx; xchg %rax,%rdx;xchg %rax,%rdx; xchg %rax,%rdx\n"); 1; })
#elif defined(__aarch64__)
#define cpu_relax_short() ({ asm volatile("isb"); 1; })
#else
#warning "No cpu_relax_short() implemented on this platform"
#define cpu_relax_short() do { } while (0)
#endif

/* returns a monotonic cycle counter */
static uint64_t now_cycles()
{
	uint64_t cycles = 0;
#if defined(__x86_64__)
	__asm__ __volatile__("rdtsc" : "=A" (cycles));
#endif
	return cycles;
}

/* returns a monotonic ns counter */
static uint64_t now_ns()
{
#if _POSIX_TIMERS > 0
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#else
	return 0;
#endif
}

/* returns a more-or-less monotonic us counter */
static uint64_t now_us()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

/* returns <now> as using the preferred method */
static inline uint64_t now()
{
	switch (arg_meas) {
	case 0:  return now_cycles();
	case 1:  return now_ns();
	default: return now_us();
	}
}

/* simple counter increment using a CAS */
void operation0(struct thread_ctx *ctx)
{
	unsigned long old, new;
	unsigned long long prev, curr;
	unsigned long long tot_wait, tot_done, max_wait;

	tot_wait = tot_done = max_wait = 0;
	if (arg_relax == 0) {
		/* no relax at all */
		do {
			prev = now();
			old = shared.counter;
			do {
				new = old + 1;
			} while (!__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
		} while (step == 2);
	}
	else if (arg_relax == 1) {
		/* cpu_relax_short() */
		do {
			prev = now();
			old = shared.counter;
			do {
				new = old + 1;
			} while (!__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && ({ cpu_relax_short(); 1; }));
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
		} while (step == 2);
	}
	else if (arg_relax == 2) {
		/* cpu_relax_long() */
		do {
			prev = now();
			old = shared.counter;
			do {
				new = old + 1;
			} while (!__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && ({ cpu_relax_long(); 1; }));
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
		} while (step == 2);
	}

	ctx->tot_done = tot_done;
	ctx->tot_wait = tot_wait;
	ctx->max_wait = max_wait;
}

/* XorShift random number generator using a CAS */
void operation1(struct thread_ctx *ctx)
{
	unsigned int old, new;
	unsigned long long prev, curr;
	unsigned long long tot_wait, tot_done, max_wait;

	tot_wait = tot_done = max_wait = 0;
	if (arg_relax == 0) {
		/* no relax at all */
		do {
			prev = now();
			old = shared.rnd;
			do {
				new = old;
				new ^= new << 13;
				new ^= new >> 17;
				new ^= new << 5;
			} while (!__atomic_compare_exchange_n(&shared.rnd, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
		} while (step == 2);
	}
	else if (arg_relax == 1) {
		/* cpu_relax_short() */
		do {
			prev = now();
			old = shared.rnd;
			do {
				new = old;
				new ^= new << 13;
				new ^= new >> 17;
				new ^= new << 5;
			} while (!__atomic_compare_exchange_n(&shared.rnd, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && ({ cpu_relax_short(); 1; }));
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
		} while (step == 2);
	}
	else if (arg_relax == 2) {
		/* cpu_relax_long() */
		do {
			prev = now();
			old = shared.rnd;
			do {
				new = old;
				new ^= new << 13;
				new ^= new >> 17;
				new ^= new << 5;
			} while (!__atomic_compare_exchange_n(&shared.rnd, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && ({ cpu_relax_long(); 1; }));
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
		} while (step == 2);
	}

	ctx->tot_done = tot_done;
	ctx->tot_wait = tot_wait;
	ctx->max_wait = max_wait;
}

void run(void *arg)
{
	const int tid = (long)arg;

	sched_setaffinity(0, sizeof(aff[tid]), &aff[tid]);

	/* step 0: create all threads */
	while (step == 0) {
		/* don't disturb pthread_create() */
		usleep(10000);
	}

	/* step 1 : wait for signal to start */
	__sync_fetch_and_add(&actthreads, 1);
	while (step == 1)
		cpu_relax_long();

	/* step 2 : run */
	if (arg_op == 0)
		operation0(&runners[tid]);
	else if (arg_op == 1)
		operation1(&runners[tid]);
	else
		do { } while (step == 2);

	/* step 3 : stop */
	__sync_fetch_and_sub(&actthreads, 1);
	pthread_exit(0);
}

/* stops all threads upon SIG_ALRM */
void alarm_handler(int sig)
{
	//fprintf(stderr, "received signal %d\n", sig);
	step = 3;
}


/* display the message and exit with the code */
void die(int code, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	exit(code);
}

void usage(const char *name, int ret)
{
	die(ret,
	    "usage: %s [options...]* [thr:cpu]*\n"
	    "Arguments:\n"
	    "  -h            display this help\n"
	    "  -t threads    use this number of threads\n"
	    "  -g tgmask     group threads using this mask\n"
	    "  -o operation  use this atomic op test (0..0)\n"
	    "  -r relax_meth use this cpu_relax method (0..2)\n"
	    "  -m measure    time measurement method (0..2)\n"
	    "  -d run_time   stop after this number of seconds\n"
	    "  [thr:cpu]*    bind thread <thr> to CPU <cpu>\n",
	    name);
}

int main(int argc, char **argv)
{
	int i, err, cnt;
	unsigned int u;
	unsigned long long done;
	unsigned long long start, stop;
	const char *progname = argv[0];

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
				usage(progname, 1);
			nbthreads = atol(*++argv);
		}
		else if (!strcmp(*argv, "-g")) {
			/* thread group mask */
			if (--argc < 0)
				usage(progname, 1);
			arg_tgmask = strtol(*++argv, NULL, 0);
		}
		else if (!strcmp(*argv, "-d")) {
			if (--argc < 0)
				usage(progname, 1);
			arg_run = atol(*++argv);
		}
		else if (!strcmp(*argv, "-m")) {
			if (--argc < 0)
				usage(progname, 1);
			arg_meas = atol(*++argv);
		}
		else if (!strcmp(*argv, "-o")) {
			if (--argc < 0)
				usage(progname, 1);
			arg_op = atol(*++argv);
		}
		else if (!strcmp(*argv, "-r")) {
			if (--argc < 0)
				usage(progname, 1);
			arg_relax = atol(*++argv);
		}
		else if (!strcmp(*argv, "-h"))
			usage(progname, 0);
		else if (isdigit((unsigned char)**argv)) {
			unsigned int thr, cpu;
			char *sep = strchr(*argv, ':');
			if (!sep)
				usage(progname, 1);
			sep++;
			thr = atoi(*argv);
			cpu = atoi(sep);
			if (thr < MAXTHREADS && cpu < CPU_SETSIZE) {
				CPU_ZERO(&aff[thr]);
				CPU_SET(cpu, &aff[thr]);
			}
			else
				usage(progname, 1);
		}
		else
			usage(progname, 1);
		argc--; argv++;
	}

	if (nbthreads >= MAXTHREADS)
		nbthreads = MAXTHREADS;

	actthreads = 0;	step = 0;

	if (arg_run <= 0)
		arg_run = 1;

	printf("Starting %d thread%c\n", nbthreads, (nbthreads > 1)?'s':' ');

	for (u = 0; u < nbthreads; u++) {
		runners[u].tid     = u;
		runners[u].tid_bit = 1UL << u;
		runners[u].next    = (u + 1) % nbthreads;

		err = pthread_create(&runners[u].thr, NULL, (void *)&run, (void *)(long)u);
		if (err)
			die(1, "pthread_create(): %s\n", strerror(err));
	}

	__sync_fetch_and_add(&step, 1);  /* let the threads warm up and get ready to start */

	while (actthreads != nbthreads)
		;

	signal(SIGALRM, alarm_handler);
	alarm(arg_run);

	start = now_us();
	__sync_fetch_and_add(&step, 1); /* fire ! */

	/* and wait for all threads to die */
	for (u = 0; u < nbthreads; u++)
		pthread_join(runners[u].thr, NULL);

	stop = now_us() - start;

	done = 0;
	for (u = 0; u < nbthreads; u++) {
		done += runners[u].tot_done;
		printf("Thread %d: tot_done %10llu, tot_wait %12llu, max_wait %llu\n",
		       u, runners[u].tot_done, runners[u].tot_wait, runners[u].max_wait);
	}

	printf("threads: %d done: %llu time(ms): %llu rate: %lld/s ns: %lld\n",
	       nbthreads, done, stop / 1000ULL, done * 1000000ULL / stop, stop * 1000ULL / (done?done:1));

	/* All the work has ended */
	exit(0);
}
