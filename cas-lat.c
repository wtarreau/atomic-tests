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
#define WAITL0		2    // l0=2^WAITL0 loops min

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
	const void *wait_ptr; /* this is the element we loop on */
	unsigned long tid;
	unsigned long tid_bit;
	unsigned long next;
	unsigned long next_mask; // mask reporting next and above
	pthread_t thr;
	unsigned long long totf, tots;
	struct {
		unsigned int f, s; // failure, success
	} stats[MAXTHREADS]; // per remote thread: keep it below 8B per thread for optimal asm
	unsigned long loops[16]; // >=4, >=16, >=64, >=256 ...
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
#define cpu_relax_tiny() ({ asm volatile("xchg %rax,%rdx; xchg %rax,%rdx\n"); 1; })
//#define cpu_relax_short() ({ asm volatile("xchg %rax,%rcx; xchg %rcx,%rdx; xchg %rax,%rdx\n"); 1; })  // 6 bytes
//#define cpu_relax_short() ({ asm volatile("xchg %rax,%rdx; xchg %rax,%rdx; xchg %rax,%rdx; xchg %rax,%rdx\n"); 1; })  // 8 bytes
#define cpu_relax_short() ({ asm volatile("xchg %rax,%rcx; xchg %rcx,%rdx; xchg %rcx,%rdx; xchg %rcx,%rdx; xchg %rax,%rdx\n"); 1; })  // 10 bytes opt:102ns, 15% hit
//#define cpu_relax_short() ({ asm volatile("xchg %rax,%rcx; xchg %rcx,%rdx; xchg %rax,%rdx;xchg %rax,%rcx; xchg %rcx,%rdx; xchg %rax,%rdx\n"); 1; })  // 12 bytes 111ns+l0
//#define cpu_relax_short() ({ asm volatile("xchg %rax,%rcx; xchg %rcx,%rdx; xchg %rcx,%rdx; xchg %rcx,%rdx; xchg %rax,%rdx; xchg %rax,%rdx; xchg %rax,%rdx\n"); 1; })  // 14 bytes
//#define cpu_relax_short() ({ asm volatile("cmc; cmc;cmc;cmc\n"); 1; }) // 4 bytes, slow on amd
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
	unsigned int a, d;
	asm volatile("rdtsc" : "=a" (a), "=d" (d));
	cycles = a + ((unsigned long long)d << 32);
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

/* mask of all waiting threads. A bit there indicates that a thread must check
 * its own wait_ptr before touching the shared area.
 */
static unsigned long waiters;

/* wait for our turn if there are already other waiters. Returns zero if nobody
 * is waiting, non-zero if we need to loop again to attempt the operation. The
 * idea is to build loops like this:
 *
 *     do {
 *        prepare_some_condition();
 *     } while (atomic_wait() || (!atomic_wrap(CAS())));
 *
 * i.e. it's pointless to call the CAS after contention as its input condition
 * changed and it *will* fail.
 */
static inline int atomic_wait(const void *ptr, struct thread_ctx *ctx)
{
	unsigned long others;

	/* not possible, we don't have the guarantee there's still someone to
	 * release us.
	 */
	//if (__atomic_load_n(&ctx->wait_ptr, __ATOMIC_ACQUIRE)) {
	//	/* we already came there and must not disturb others */
	//	do {
	//		cpu_relax_short();
	//	} while (__atomic_load_n(&ctx->wait_ptr, __ATOMIC_ACQUIRE));
	//	return 1;
	//}

	others = __atomic_load_n(&waiters, __ATOMIC_ACQUIRE);
	if (!others)
		return 0;

	if (!(others & ctx->tid_bit)) {
		/* first pass here. We can't wait as we don't know if others
		 * saw us, so we just add ourselves to the list of participants
		 * and go back into the loop. We need to try at least once again
		 * to avoid a TOCTOU issue.
		 */
		__atomic_store_n(&ctx->wait_ptr, ptr, __ATOMIC_RELEASE);
		__atomic_or_fetch(&waiters, ctx->tid_bit, __ATOMIC_RELEASE);
		return 0;
	}
	else {
		/* we already tried and failed previously. If we come here
		 * because we were woken up to try again, let's go on with
		 * the CAS.
		 */
		if (!__atomic_load_n(&ctx->wait_ptr, __ATOMIC_ACQUIRE))
			return 0;

		/* Otherwise let's wait for someone else to wake us up and go
		 * back to the loop without touching the shared areas.
		 */
		do {
			cpu_relax_short();
		} while (__atomic_load_n(&ctx->wait_ptr, __ATOMIC_ACQUIRE));

		/* do not attempt the CAS, conditions have changed */
		return 1;
	}
}

/* wraps an atomic op which returns <result> (0=failure, 1=success) on pointer
 * <ptr> for thread <ctx>. Returns <result>.
 */
static inline int atomic_wrap(int result, const void *ptr, struct thread_ctx *ctx)
{
	if (!result) {
		/* The atomic op failed and there was no registered waiters. It
		 * means we were the second participant hence first contender.
		 * We have to add our bit to the waiters list.
		 */
		//__atomic_store_n(&ctx->wait_ptr, ptr, __ATOMIC_RELEASE);
		if (__atomic_fetch_or(&waiters, ctx->tid_bit, __ATOMIC_RELEASE)) {
			//while (__atomic_load_n(&ctx->wait_ptr, __ATOMIC_ACQUIRE)) {
			//	cpu_relax_short();
			//}
		}
		return result;
	} else {
		/* the atomic test succeeded, let's see if others are waiting */
		unsigned long others;
		unsigned long bit;

		others = __atomic_load_n(&waiters, __ATOMIC_ACQUIRE);
		if (!others) {
			/* we weren't even there, let's not start a bus write cycle */
			return result;
		}

		/* OK let's refine and possibly unsubscribe if we were there */
		others = __atomic_and_fetch(&waiters, ~ctx->tid_bit, __ATOMIC_RELEASE);
		//if (__atomic_load_n(&ctx->wait_ptr, __ATOMIC_ACQUIRE))
		//	printf("bug @%d\n", __LINE__);
		if (!others)
			return result;

		/* search first one after our bit */
		bit = __builtin_ffsl(others & ctx->next_mask);
		if (bit) {
			/* thread <bit>-1 is waiting. Note: we could
			 * also find its address from <ctx>.
			 */
			__atomic_store_n(&runners[bit-1].wait_ptr, 0, __ATOMIC_RELEASE);
			return result;
		}

		/* no bit set above, so there's at least one below */
		bit = __builtin_ffsl(others);
		__atomic_store_n(&runners[bit-1].wait_ptr, 0, __ATOMIC_RELEASE);
		return result;
	}
}

/* a queue to regulate access to a shared resource using tickets.
 * The tickets have the following format:
 *    [ next | curr ]
 *      16b    16b
 * The <next> part is incremented upon assignment and automatically wraps so
 * that the caller doesn't have to care. The <curr> is incremented upon release
 * so that it involves a check on the slow path, and it is cleared after the
 * last one leaves so that requesters can simply check for 0 over the whole
 * word to figure that tickets are not needed. It's not a problem since
 * contended accesses leave curr != next, hence 0 == uncontended.
 */
static unsigned int queue;
static unsigned int tot_tickets;

/* Assign a ticket for a queue from the dispenser and return it. */
static unsigned short get_ticket(unsigned int *queue)
{
	__atomic_fetch_add(&tot_tickets, 1, __ATOMIC_RELAXED);
	return (__atomic_fetch_add(queue, 0x10000, __ATOMIC_RELAXED) >> 16) + 1;
}

/* wait for being called */
static void wait_turn(unsigned int *queue, unsigned short ticket)
{
	while (__atomic_load_n(queue, __ATOMIC_ACQUIRE) >> 16 != ticket)
		cpu_relax_short();
}

/* call next waiter in queue */
static void next_one(unsigned int *queue)
{
	unsigned int waiters = __atomic_load_n(queue, __ATOMIC_ACQUIRE);

	if (!waiters) {
		/* pb: race here */
		return;
	}

	/* 3 possibilities here:
	 *    next == curr: nobody's waiting anymore, let's try to reset it
	 *    curr  < 0xffff: atomic_inc is sufficient to unlock
	 *    curr == 0xffff: use -0xffff to avoid wrapping
	 */
	if (((waiters >> 16) == (waiters & 0xffff)) &&
	    __atomic_compare_exchange_n(queue, &waiters, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
		return;
	} else if ((waiters & 0xffff) == 0xffff)
		__atomic_fetch_sub(queue, 0xffff, __ATOMIC_RELAXED);
	else
		__atomic_fetch_add(queue, 1, __ATOMIC_RELAXED);
}

/* simple per-thread counter increment using a CAS, and tickets for congestion.
 *
 * Algorithm:
 *   - if there are no waiters in queue, try the CAS directly, otherwise take
 *     a ticket and wait for our turn
 *   - in case of ticket-less access, there may be conflicts. Repeated failures
 *     to proceed should result in taking a ticket and waiting for our turn.
 *   - in case of ticket-based access, there should theoretically be no conflict
 *     though code doing stuff out of this protection may still cause conflict,
 *     so we must be prepared to retry immediately under the same ticket.
 *   - upon release, the queue must be checked, regardless of the fact that
 *     we got a ticket, because there could be some waiters that were
 *     conflicting during our operations.
 */
void operation0(struct thread_ctx *ctx)
{
	unsigned long old, new;
	unsigned long tid = ctx->tid; // thread id
	unsigned long prev; // prev id
	unsigned long counter = 0;
	unsigned long failcnt, prevcnt, faillog, prevlog;

	old = 0;
	if (arg_relax == 0) {
		/* no relax at all */
		do {
			faillog = prevcnt = failcnt = 0;
			new = counter + tid;
			counter += 65536;

			/* If others have been waiting longer than us, we have
			 * to let them pass first.
			 */
		try_again:
			while (1) {
				prevlog = __atomic_load_n(&queue, __ATOMIC_ACQUIRE);
				if (!prevlog)
					break;
			failed:
				prevcnt = failcnt++;
				if ((prevcnt & failcnt) == 0) {
					faillog++;
					if (faillog >= prevlog) {
						/* let's get out and try to pass */
						if (faillog > prevlog/* && faillog >= 8*/)
							__atomic_store_n(&queue, faillog, __ATOMIC_RELEASE);
						break;
					}
				}
				//cpu_relax_long();
				//cpu_relax_short();
			}

			old = __atomic_load_n(&shared.counter, __ATOMIC_ACQUIRE);
			if (!__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
				prev = old & 255;
				ctx->stats[prev].f++; // failure
				//goto failed;
				//prevcnt = failcnt++;
				//if ((prevcnt & failcnt) == 0)
				//	faillog++;
				//
				//if (faillog > prevlog)
				//	__atomic_store_n(&queue, faillog, __ATOMIC_RELEASE);
				//cpu_relax_long();
				//cpu_relax_short();
				goto try_again;
			}

			/* next one! */
			if (prevlog)
				__atomic_store_n(&queue, prevlog / 4, __ATOMIC_RELEASE);

			prev = old & 255;
			ctx->stats[prev].s++; // success

			/* we consumed our right through the queue, let's release someone else now */
			//next_one(&queue);

			if (faillog >= WAITL0) {
				faillog -= WAITL0;
				ctx->loops[faillog >> 1]++;
			}

		} while (step == 2);
	}

	ctx->tots = ctx->totf = 0;
	for (prev = 0; prev < nbthreads; prev++) {
		ctx->totf += ctx->stats[prev].f++;
		ctx->tots += ctx->stats[prev].s++;
	}
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
	unsigned int u, v;
	unsigned long long done, totf, tots, tot;
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
		runners[u].tid       = u;
		runners[u].tid_bit   = 1UL << u;
		runners[u].next      = (u + 1) % nbthreads;
		runners[u].next_mask = ~((1UL << ((u + 1) % nbthreads)) - 1);

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

	printf("Thr ");
	for (v = 0; v < nbthreads; v++) {
		printf("%8u ", v);
	}
	printf("\n");

	/* compute <done> = tot# of operations */
	done = totf = tots = 0;
	for (u = 0; u < nbthreads; u++) {
		totf += runners[u].totf;
		tots += runners[u].tots;
	}
	done = totf + tots;

	for (u = 0; u < nbthreads; u++) {
		totf = runners[u].totf;
		tots = runners[u].tots;
		tot = totf + tots;
		if (!tot)
			tot = 1;

		printf("%2d: ", u);
		for (v = 0; v < nbthreads; v++) {
			printf("t:%5.1f%% ", (runners[u].stats[v].s + runners[u].stats[v].f)*100.0 / tot);
		}

		printf(" tt=%5.1f%%", tot*100.0/done);
		for (v = 0; v < 16; v++)
			if (runners[u].loops[v])
				printf(" l%u=%lu", v, runners[u].loops[v]);
		printf("\n    ");

		for (v = 0; v < nbthreads; v++) {
			unsigned long t = runners[u].stats[v].s + runners[u].stats[v].f;
			if (!t)
				t = 1;
			printf("h:%5.1f%% ", runners[u].stats[v].s * 100.0 / t);
		}


		//printf("%2d: ", u);
		//for (v = 0; v < nbthreads; v++) {
		//	printf("s:%8u ", runners[u].stats[v].s);
		//}

		//printf(" ts=%u\n    ", tots);
		//for (v = 0; v < nbthreads; v++) {
		//	printf("f:%8u ", runners[u].stats[v].f);
		//}
		printf(" ts=%5.1f%%\n", (tots)*100.0/tot);
	}

	done = totf = tots = 0;
	for (u = 0; u < nbthreads; u++) {
		totf += runners[u].totf;
		tots += runners[u].tots;
	}
	done = totf + tots;

	printf("threads:%d  attempt:%llu  success:%llu(%.2f%%)  time(ms):%llu  rate:%lld/s  ns:%lld  hit:%.2f%%\n",
	       nbthreads, done, tots, (tots * 100.0) / (done?done:1), stop / 1000ULL, tots * 1000000ULL / stop, stop * 1000ULL / (tots?tots:1),
	       (tots*100.0)/done);

	/* All the work has ended */
	exit(0);
}
