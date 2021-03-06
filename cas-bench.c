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
#define WAITL0		2  // l0=2^WAITL0 loops min

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
	unsigned long long tot_done;
	unsigned long long tot_wait;
	unsigned long long max_wait;
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

/* SMT CPU relaxation, yields the control to the sibling thread, or does
 * nothing if no SMT available.
 */
#if defined(__x86_64__)
#define cpu_relax_smt() ({ asm volatile("rep;nop\n"); 1; })
#else
#define cpu_relax_smt() do { } while (0)
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
	unsigned int a, d;
	asm volatile("rdtsc" : "=a" (a), "=d" (d));
	cycles = a + ((unsigned long long)d << 32);
#elif defined(__aarch64__)
	asm volatile("mrs %0, cntvct_el0" : "=r"(cycles));
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
	case 0:  return 0;            // does not cost
	case 1:  return now_cycles();
	case 2:  return now_ns();
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
	else if (arg_relax == 3) {
		/* atomic_wrap() without relax */
		do {
			prev = now();
			old = shared.counter;
			do {
				new = old + 1;
			} while (atomic_wait(&shared.counter, ctx) ||
				 !atomic_wrap(__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED), &shared.counter, ctx));
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
	else if (arg_relax == 3) {
		/* atomic_wrap() without relax */
		do {
			prev = now();
			old = shared.rnd;
			do {
				new = old;
				new ^= new << 13;
				new ^= new >> 17;
				new ^= new << 5;
			} while (atomic_wait(&shared.counter, ctx) ||
				 !atomic_wrap(__atomic_compare_exchange_n(&shared.rnd, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED), &shared.rnd, ctx));
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

/* counter which waits for its turn using a CAS */
void operation2(struct thread_ctx *ctx)
{
	unsigned long old, new, prev_ctr;
	unsigned long long prev, curr;
	unsigned long long tot_wait, tot_done, max_wait;

	tot_wait = tot_done = max_wait = 0;
	prev_ctr = ctx->tid;
	if (arg_relax == 0) {
		/* no relax at all */
		do {
			prev = now();
			old = shared.counter;
			do {
				old = prev_ctr;
				new = old + 1;
			} while (!__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && step == 2);
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
			prev_ctr += nbthreads;
		} while (step == 2);
	}
	else if (arg_relax == 1) {
		/* cpu_relax_short() */
		do {
			prev = now();
			old = shared.counter;
			do {
				old = prev_ctr;
				new = old + 1;
			} while (!__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && ({ cpu_relax_short(); 1; }) && step == 2);
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
			prev_ctr += nbthreads;
		} while (step == 2);
	}
	else if (arg_relax == 2) {
		/* cpu_relax_long() */
		do {
			prev = now();
			old = shared.counter;
			do {
				old = prev_ctr;
				new = old + 1;
			} while (!__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && ({ cpu_relax_long(); 1; }) && step == 2);
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
			prev_ctr += nbthreads;
		} while (step == 2);
	}
	else if (arg_relax == 3) {
		/* atomic_wrap() without relax */
		do {
			prev = now();
			old = shared.counter;
			do {
				old = prev_ctr;
				new = old + 1;
			} while ((atomic_wait(&shared.counter, ctx) ||
				  !atomic_wrap(__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED), &shared.counter, ctx)) && step == 2);
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
			prev_ctr += nbthreads;
		} while (step == 2);
	}

	ctx->tot_done = tot_done;
	ctx->tot_wait = tot_wait;
	ctx->max_wait = max_wait;
}

/* counter which waits for its turn using load then a CAS which must always
 * succeed here (for simplicity)
 */
void operation3(struct thread_ctx *ctx)
{
	unsigned long old, new, prev_ctr;
	unsigned long long prev, curr;
	unsigned long long tot_wait, tot_done, max_wait;

	tot_wait = tot_done = max_wait = old = 0;
	prev_ctr = ctx->tid;
	if (arg_relax == 0) {
		/* no relax at all */
		do {
			prev = now();

			while ((old = __atomic_load_n(&shared.counter, __ATOMIC_RELAXED)) != prev_ctr && step == 2)
				;

			new = old + 1;
			if (!__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && step == 2)
				abort();
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
			prev_ctr += nbthreads;
		} while (step == 2);
	}
	else if (arg_relax == 1) {
		/* cpu_relax_short() */
		do {
			prev = now();

			while ((old = __atomic_load_n(&shared.counter, __ATOMIC_RELAXED)) != prev_ctr && ({ cpu_relax_short(); 1; }) && step == 2)
				;

			new = old + 1;
			if (!__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && step == 2)
				abort();
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
			prev_ctr += nbthreads;
		} while (step == 2);
	}
	else if (arg_relax == 2) {
		/* cpu_relax_long() */
		do {
			prev = now();

			while ((old = __atomic_load_n(&shared.counter, __ATOMIC_RELAXED)) != prev_ctr && ({ cpu_relax_long(); 1; }) && step == 2)
				;

			new = old + 1;
			if (!__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && step == 2)
				abort();
			curr = now();
			tot_done++;
			if (curr > prev) {
				tot_wait += curr - prev;
				if (curr - prev > max_wait)
					max_wait = curr - prev;
			}
			prev_ctr += nbthreads;
		} while (step == 2);
	}
	// not possible since we expect exact threads ordering that does not
	// exactly match the atomic_wrap()'s ordering.
	//else if (arg_relax == 3) {
	//	/* atomic_wrap() without relax */
	//	do {
	//		prev = now();
	//
	//		while ((atomic_wait(&shared.counter, ctx) ||
	//			!atomic_wrap((old = __atomic_load_n(&shared.counter, __ATOMIC_RELAXED)) == prev_ctr, &shared.counter, ctx)) && step == 2)
	//			;
	//
	//		new = old + 1;
	//		if (!__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && step == 2)
	//			abort();
	//		curr = now();
	//		tot_done++;
	//		if (curr > prev) {
	//			tot_wait += curr - prev;
	//			if (curr - prev > max_wait)
	//				max_wait = curr - prev;
	//		}
	//		prev_ctr += nbthreads;
	//	} while (step == 2);
	//}

	ctx->tot_done = tot_done;
	ctx->tot_wait = tot_wait;
	ctx->max_wait = max_wait;
}

static unsigned int avg_wait __attribute__((aligned(64)));
static volatile unsigned int wait_line __attribute__((aligned(64)));

/* simple per-thread counter increment using a CAS, and tickets for congestion. */
void operation4(struct thread_ctx *ctx)
{
	unsigned long tid = ctx->tid; // thread id
	unsigned long old, new;
	unsigned long counter;
	unsigned long failcnt, prevcnt, loopcnt;
	unsigned long long tot_done, tot_wait, max_wait;

	old = 0;
	counter = 0;
	tot_done = tot_wait = max_wait = 0;
	if (arg_relax == 0) {
		/* no relax at all */
		do {
			int faillog = -1; // faillog=0 for cnt=1

			prevcnt = failcnt = 0;
			new = counter + tid;
			counter += 65536;

			while (1) {
				old = __atomic_load_n(&shared.counter, __ATOMIC_ACQUIRE);
				if (__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
					break;

				failcnt++;
				if (!(prevcnt & failcnt)) {
					prevcnt = failcnt;
					faillog++;
				}
			}

			if (faillog >= WAITL0) {
				faillog -= WAITL0;
				ctx->loops[faillog >> 1]++;
			}

			tot_done++;
			tot_wait += failcnt;
			if (failcnt > max_wait)
				max_wait = failcnt;
		} while (step == 2);
	} else if (arg_relax == 1) {
		/* relax based on avg wait time. Each thread waits at least as
		 * long as the avg wait time others are experiencing before
		 * starting to disturb others. On success, the avg waittime is
		 * lowered.
		 */
		do {
			unsigned long avg_curr;
			int faillog = -1; // faillog=0 for cnt=1

			prevcnt = failcnt = loopcnt = 0;
			new = counter + tid;
			counter += 65536;

			avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
			while (1) {
				if (__builtin_expect(loopcnt++ > 16 || avg_curr > 2, 0)) {
					/* we know it will happen often but it's on the slow path so
					 * better keep it marked unlikely so that the code remains out
					 * of the fast path. It saves 4ns on average.
					 */
					do {
						cpu_relax_smt();

						if ((loopcnt & 31) == 6)
							avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
						else if (loopcnt > 2*avg_curr) {
							avg_curr = __atomic_exchange_n(&avg_wait, loopcnt, __ATOMIC_RELAXED);
						}
					} while (loopcnt++ <= avg_curr + 1);
				}

				/* perform the atomic op */
				old = __atomic_load_n(&shared.counter, __ATOMIC_ACQUIRE);
				if (__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
					/* wake other threads and give them a chance to pass */
					if (avg_curr)
						__atomic_compare_exchange_n(&avg_wait, &avg_curr, avg_curr*3/4, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
					break;
				}

				/* measure number of failures for reporting */
				failcnt++;
				if (!(prevcnt & failcnt)) {
					prevcnt = failcnt;
					faillog++;
				}
			}


			if (faillog >= WAITL0) {
				faillog -= WAITL0;
				ctx->loops[faillog >> 1]++;
			}

			tot_done++;
			tot_wait += failcnt;
			if (failcnt > max_wait)
				max_wait = failcnt;
		} while (step == 2);
	} else if (arg_relax == 2) {
		/* relax based on avg wait time. Each thread waits at least as
		 * long as the avg wait time others are experiencing before
		 * starting to disturb others. On success, the avg waittime is
		 * lowered.
		 */
		do {
			unsigned long avg_curr;
			int faillog = -1; // faillog=0 for cnt=1

			prevcnt = failcnt = loopcnt = 0;
			new = counter + tid;
			counter += 65536;

			avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
			while (1) {
				if (1) {
					/* Measured values: for loopcnt*9/16 on finish:
					 *  loopcnt         Ryzen                   i7
					 *  >=48/16   74ns,maxw~60,l4~10000   68ns,maxw~13,l2~85k
					 *  >=40/16   73ns,maxw~40,l4~3000    69ns,maxw~15,l2~70k
					 *  >=32/16   74ns,maxw~35,l4~3000    71ns,maxw~13,l2~45k
					 *  >=24/16   70ns,maxw~29,l4~500     82ns,maxw~10,l2~11k
					 *
					 * Measured values: for loopcnt*5/8 on finish:
					 *  loopcnt         Ryzen                   i7
					 *   >32/16:  74ns,maxw~35,l4~4000    68ns,maxw~12,l2~40k
					 *  >=32/16:  74ns,maxw~35,l4~4000    69ns,maxw~12,l2~40k
					 *  >=24/16:  69ns,maxw~30,l4~1000    81ns,maxw~10,l2~10k
					 *   >20/16:  67ns,maxw~23,l4~300     89ns,maxw~8,l2~4000
					 *  >=21/16:  67ns,maxw~24,l4~80      90ns,maxw~8,l2~3300
					 *  >=20/16:  68ns,maxw~21,l4~40      94ns,maxw~7,l2~1500
					 *  >=19/16:  67ns,maxw~18,l4~10     107ns,maxw~6,l2~600
					 *  >=18/16   71ns,maxw~15,l4~2      148ns,maxw~5,l2~45
					 *  >=17/16:  99ns,maxw~12,l2~10k    234ns,maxw~4,l2~1
					 *  >=33/32  133ns,maxw~8,l2~1300    423ns,maxw~3,l2~0.4
					 *
					 * Measured values for loopcnt*7/8 on finish:
					 *  loopcnt         Ryzen                   i7
					 *   >32/16:  47ns,maxw~28,l4~1400   128ns,maxw~5,l2~30
					 *  >=32/16:  47ns,maxw~27,l4~1200   128ns,maxw~6,l2~35
					 *  >=24/16:  51ns,maxw~27,l4~100    128ns,maxw~6,l2~25
					 *  >=21/16:  50ns,maxw~24,l4~60     128ns,maxw~5,l2~22
					 *  >=20/16:  49ns,maxw~20,l4~10     128ns,maxw~5,l2~15
					 *  >=19/16:  53ns,maxw~17,l4~5      142ns,maxw~6,l2~350 <- big change for i7
					 *  >=18/16   57ns,maxw~14,l2~15k    185ns,maxw~6,l2~250
					 *  >=17/16:  82ns,maxw~11,l2~6.8k   271ns,maxw~4,l2~7
					 */
					do {
						if ((loopcnt & 31) == 16) // even faster with &15==8
							avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
						else if (loopcnt >= 3*avg_curr/2)
							avg_curr = __atomic_exchange_n(&avg_wait, loopcnt, __ATOMIC_RELAXED);
						else
							cpu_relax_smt();
					} while (loopcnt++ <= avg_curr + 1);
				}

				/* perform the atomic op */
				old = __atomic_load_n(&shared.counter, __ATOMIC_ACQUIRE);
				if (__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
					/* wake other threads and give them a chance to pass
					 *
					 * Measures for >4/5 above:
					 *  loopcnt          Ryzen                   i7
					 *   *15/16     68ns,maxw~22,l4~25     249ns,maxw~6,l2~150
					 *   *14/16     52ns,maxw~25,l4~40     128ns,maxw~5,l2~80
					 *   *13/16     54ns,maxw~22,l4~120     94ns,maxw~6,l2~780
					 *   *12/16     58ns,maxw~20,l4~150     92ns,maxw~7,l2~2300
					 *   *11/16     63ns,maxw~20,l4~200     91ns,maxw~7,l2~3300
					 *   *10/16     67ns,maxw~23,l4~300     89ns,maxw~8,l2~4000
					 */
					if (avg_curr)
						__atomic_compare_exchange_n(&avg_wait, &avg_curr, loopcnt*12/16, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
					break;
				}

				/* measure number of failures for reporting */
				failcnt++;
				if (!(prevcnt & failcnt)) {
					prevcnt = failcnt;
					faillog++;
				}
			}


			if (faillog >= WAITL0) {
				faillog -= WAITL0;
				ctx->loops[faillog >> 1]++;
			}

			tot_done++;
			tot_wait += failcnt;
			if (failcnt > max_wait)
				max_wait = failcnt;
		} while (step == 2);
	} else if (arg_relax == 3) {
		/* relax based on avg wait time. Each thread waits at least as
		 * long as the avg wait time others are experiencing before
		 * starting to disturb others. On success, the avg waittime is
		 * lowered.
		 */
		do {
			unsigned long avg_curr;
			int faillog = -1; // faillog=0 for cnt=1

			prevcnt = failcnt = loopcnt = 0;
			new = counter + tid;
			counter += 65536;

			avg_curr = 0;//__atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
			while (1) {
				/* perform the atomic op */
				old = __atomic_load_n(&shared.counter, __ATOMIC_ACQUIRE);
				if (__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
					if (avg_curr)
						__atomic_compare_exchange_n(&avg_wait, &avg_curr, (loopcnt+1)*5/8, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
					break;
				}

				/* measure number of failures for reporting */
				failcnt++;
				if (!(prevcnt & failcnt)) {
					prevcnt = failcnt;
					faillog++;
				}

				if (failcnt > 0) {
					/* much faster on i7, slower on ryzen, faster on armada but extremely unfair */
					avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
					do {
						/* not beautiful but much faster on all machines, including armada,
						 * with a huge unfairness there. It goes down from 110 to 14 ns avg
						 * on i7 with 1/10 of the l4 values, unchanged time on ryzen with
						 * l4/100 and no more l6 nor l8.
						 */
						if (loopcnt >= 9*avg_curr/8) {
							unsigned long old_avg = avg_curr;
							__atomic_compare_exchange_n(&avg_wait, &avg_curr, loopcnt, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
							if (avg_curr < old_avg)
								break;
						}
						else if ((loopcnt & 127) == 0) {
							unsigned long old_avg = avg_curr;
							avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
							if (avg_curr < old_avg)
								break;
						}
						else {
							//cpu_relax_long();
							cpu_relax_smt();
						}
						loopcnt++;
					} while (loopcnt < avg_curr);
				}
			}


			if (faillog >= WAITL0) {
				faillog -= WAITL0;
				ctx->loops[faillog >> 1]++;
			}

			tot_done++;
			tot_wait += failcnt;
			if (failcnt > max_wait)
				max_wait = failcnt;
		} while (step == 2);
	}
	ctx->tot_done = tot_done;
	ctx->tot_wait = tot_wait;
	ctx->max_wait = max_wait;
}

/* simple per-thread counter increment using a CAS but with load-exclusive
 * made of xadd(0), and tickets for congestion.
 */
void operation5(struct thread_ctx *ctx)
{
	unsigned long tid = ctx->tid; // thread id
	unsigned long old, new;
	unsigned long counter;
	unsigned long failcnt, prevcnt, loopcnt;
	unsigned long long tot_done, tot_wait, max_wait;

	old = 0;
	counter = 0;
	tot_done = tot_wait = max_wait = 0;
	if (arg_relax == 0) {
		/* no relax at all */
		do {
			int faillog = -1; // faillog=0 for cnt=1

			prevcnt = failcnt = 0;
			new = counter + tid;
			counter += 65536;

			while (1) {
				old = __atomic_fetch_add(&shared.counter, 0, __ATOMIC_RELAXED);
				if (__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
					break;

				failcnt++;
				if (!(prevcnt & failcnt)) {
					prevcnt = failcnt;
					faillog++;
				}
			}

			if (faillog >= WAITL0) {
				faillog -= WAITL0;
				ctx->loops[faillog >> 1]++;
			}

			tot_done++;
			tot_wait += failcnt;
			if (failcnt > max_wait)
				max_wait = failcnt;
		} while (step == 2);
	} else if (arg_relax == 1) {
		/* relax based on avg wait time. Each thread waits at least as
		 * long as the avg wait time others are experiencing before
		 * starting to disturb others. On success, the avg waittime is
		 * lowered.
		 */
		do {
			unsigned long avg_curr;
			int faillog = -1; // faillog=0 for cnt=1

			prevcnt = failcnt = loopcnt = 0;
			new = counter + tid;
			counter += 65536;

			avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
			while (1) {
				if (__builtin_expect(loopcnt++ > 16 || avg_curr > 2, 0)) {
					/* we know it will happen often but it's on the slow path so
					 * better keep it marked unlikely so that the code remains out
					 * of the fast path. It saves 4ns on average.
					 */
					do {
						cpu_relax_smt();

						if ((loopcnt & 31) == 6)
							avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
						else if (loopcnt > 2*avg_curr) {
							avg_curr = __atomic_exchange_n(&avg_wait, loopcnt, __ATOMIC_RELAXED);
						}
					} while (loopcnt++ <= avg_curr + 1);
				}

				/* perform the atomic op */
				old = __atomic_fetch_add(&shared.counter, 0, __ATOMIC_RELAXED);
				if (__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
					/* wake other threads and give them a chance to pass */
					if (avg_curr)
						__atomic_compare_exchange_n(&avg_wait, &avg_curr, avg_curr*3/4, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
					break;
				}

				/* measure number of failures for reporting */
				failcnt++;
				if (!(prevcnt & failcnt)) {
					prevcnt = failcnt;
					faillog++;
				}
			}


			if (faillog >= WAITL0) {
				faillog -= WAITL0;
				ctx->loops[faillog >> 1]++;
			}

			tot_done++;
			tot_wait += failcnt;
			if (failcnt > max_wait)
				max_wait = failcnt;
		} while (step == 2);
	} else if (arg_relax == 2) {
		/* relax based on avg wait time. Each thread waits at least as
		 * long as the avg wait time others are experiencing before
		 * starting to disturb others. On success, the avg waittime is
		 * lowered.
		 */
		do {
			unsigned long avg_curr;
			int faillog = -1; // faillog=0 for cnt=1

			prevcnt = failcnt = loopcnt = 0;
			new = counter + tid;
			counter += 65536;

			avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
			while (1) {
				if (1) {
					do {
						if ((loopcnt & 31) == 16)
							avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
						else if (loopcnt > 2*avg_curr)
							avg_curr = __atomic_exchange_n(&avg_wait, loopcnt, __ATOMIC_RELAXED);
						else
							cpu_relax_smt();
					} while (loopcnt++ <= avg_curr + 1);
				}

				/* perform the atomic op */
				old = __atomic_fetch_add(&shared.counter, 0, __ATOMIC_RELAXED);
				if (__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
					/* wake other threads and give them a chance to pass */
					if (avg_curr)
						__atomic_compare_exchange_n(&avg_wait, &avg_curr, loopcnt*14/16, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
					break;
				}

				/* measure number of failures for reporting */
				failcnt++;
				if (!(prevcnt & failcnt)) {
					prevcnt = failcnt;
					faillog++;
				}
			}


			if (faillog >= WAITL0) {
				faillog -= WAITL0;
				ctx->loops[faillog >> 1]++;
			}

			tot_done++;
			tot_wait += failcnt;
			if (failcnt > max_wait)
				max_wait = failcnt;
		} while (step == 2);
	}
	ctx->tot_done = tot_done;
	ctx->tot_wait = tot_wait;
	ctx->max_wait = max_wait;
}

/* simple per-thread counter increment using a CAS but with load-exclusive
 * made of a failed cmpxchg, and tickets for congestion.
 */
void operation6(struct thread_ctx *ctx)
{
	unsigned long tid = ctx->tid; // thread id
	unsigned long old, new;
	unsigned long counter;
	unsigned long failcnt, prevcnt, loopcnt;
	unsigned long long tot_done, tot_wait, max_wait;

	old = 0;
	counter = 0;
	tot_done = tot_wait = max_wait = 0;
	if (arg_relax == 0) {
		/* no relax at all */
		do {
			int faillog = -1; // faillog=0 for cnt=1

			prevcnt = failcnt = 0;
			new = counter + tid;
			counter += 65536;

			while (1) {
				old = 0xBADC0FFEE; // just use a value unlikely to match
				__atomic_compare_exchange_n(&shared.counter, &old, old, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
				/* now <old> contains the previous value and the line was loaded as exclusive */

				if (__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
					break;

				failcnt++;
				if (!(prevcnt & failcnt)) {
					prevcnt = failcnt;
					faillog++;
				}
			}

			if (faillog >= WAITL0) {
				faillog -= WAITL0;
				ctx->loops[faillog >> 1]++;
			}

			tot_done++;
			tot_wait += failcnt;
			if (failcnt > max_wait)
				max_wait = failcnt;
		} while (step == 2);
	} else if (arg_relax == 1) {
		/* relax based on avg wait time. Each thread waits at least as
		 * long as the avg wait time others are experiencing before
		 * starting to disturb others. On success, the avg waittime is
		 * lowered.
		 */
		do {
			unsigned long avg_curr;
			int faillog = -1; // faillog=0 for cnt=1

			prevcnt = failcnt = loopcnt = 0;
			new = counter + tid;
			counter += 65536;

			avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
			while (1) {
				if (__builtin_expect(loopcnt++ > 16 || avg_curr > 2, 0)) {
					/* we know it will happen often but it's on the slow path so
					 * better keep it marked unlikely so that the code remains out
					 * of the fast path. It saves 4ns on average.
					 */
					do {
						cpu_relax_smt();

						if ((loopcnt & 31) == 6)
							avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
						else if (loopcnt > 2*avg_curr) {
							avg_curr = __atomic_exchange_n(&avg_wait, loopcnt, __ATOMIC_RELAXED);
						}
					} while (loopcnt++ <= avg_curr + 1);
				}

				/* perform the atomic op */
				old = 0xBADC0FFEE; // just use a value unlikely to match
				__atomic_compare_exchange_n(&shared.counter, &old, old, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
				/* now <old> contains the previous value and the line was loaded as exclusive */

				if (__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
					/* wake other threads and give them a chance to pass */
					if (avg_curr)
						__atomic_compare_exchange_n(&avg_wait, &avg_curr, avg_curr*3/4, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
					break;
				}

				/* measure number of failures for reporting */
				failcnt++;
				if (!(prevcnt & failcnt)) {
					prevcnt = failcnt;
					faillog++;
				}
			}


			if (faillog >= WAITL0) {
				faillog -= WAITL0;
				ctx->loops[faillog >> 1]++;
			}

			tot_done++;
			tot_wait += failcnt;
			if (failcnt > max_wait)
				max_wait = failcnt;
		} while (step == 2);
	} else if (arg_relax == 2) {
		/* relax based on avg wait time. Each thread waits at least as
		 * long as the avg wait time others are experiencing before
		 * starting to disturb others. On success, the avg waittime is
		 * lowered.
		 */
		do {
			unsigned long avg_curr;
			int faillog = -1; // faillog=0 for cnt=1

			prevcnt = failcnt = loopcnt = 0;
			new = counter + tid;
			counter += 65536;

			avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
			while (1) {
				if (1) {
					do {
						if ((loopcnt & 31) == 16)
							avg_curr = __atomic_load_n(&avg_wait, __ATOMIC_ACQUIRE);
						else if (loopcnt > 2*avg_curr)
							avg_curr = __atomic_exchange_n(&avg_wait, loopcnt, __ATOMIC_RELAXED);
						else
							cpu_relax_smt();
					} while (loopcnt++ <= avg_curr + 1);
				}

				/* perform the atomic op */
				old = 0xBADC0FFEE; // just use a value unlikely to match
				__atomic_compare_exchange_n(&shared.counter, &old, old, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
				/* now <old> contains the previous value and the line was loaded as exclusive */

				if (__atomic_compare_exchange_n(&shared.counter, &old, new, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
					/* wake other threads and give them a chance to pass */
					if (avg_curr)
						__atomic_compare_exchange_n(&avg_wait, &avg_curr, loopcnt*14/16, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
					break;
				}

				/* measure number of failures for reporting */
				failcnt++;
				if (!(prevcnt & failcnt)) {
					prevcnt = failcnt;
					faillog++;
				}
			}


			if (faillog >= WAITL0) {
				faillog -= WAITL0;
				ctx->loops[faillog >> 1]++;
			}

			tot_done++;
			tot_wait += failcnt;
			if (failcnt > max_wait)
				max_wait = failcnt;
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
	else if (arg_op == 2)
		operation2(&runners[tid]);
	else if (arg_op == 3)
		operation3(&runners[tid]);
	else if (arg_op == 4)
		operation4(&runners[tid]);
	else if (arg_op == 5)
		operation5(&runners[tid]);
	else if (arg_op == 6)
		operation6(&runners[tid]);
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
	    "  -o operation  use this atomic op test (0..4)\n"
	    "  -r relax_meth use this cpu_relax method (0..2)\n"
	    "  -m measure    time measurement method (0=none,1=tsc,2=ns,3=us)\n"
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
	for (u = 0; u < nbthreads; u++) {
		int v;

		done += runners[u].tot_done;
		if (runners[u].tot_done)
			printf("Thread %3d: tot_wait %12llu, tot_done %10llu (%3u%%), max_wait %3llu",
			       u, runners[u].tot_wait, runners[u].tot_done, (unsigned int)(runners[u].tot_done * 100ULL / (runners[u].tot_done + runners[u].tot_wait)), runners[u].max_wait);
		for (v = 0; v < 16; v++)
			if (runners[u].loops[v])
				printf(" l%u=%lu", WAITL0+2*v, runners[u].loops[v]);
		printf("\n");
	}

	printf("threads: %d done: %llu time(ms): %llu rate: %lld/s ns: %lld\n",
	       nbthreads, done, stop / 1000ULL, done * 1000000ULL / stop, stop * 1000ULL / (done?done:1));

	/* All the work has ended */
	exit(0);
}
