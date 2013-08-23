/*
 * random.c -- A strong random number generator
 *
 * Copyright Matt Mackall <mpm@selenic.com>, 2003, 2004, 2005
 *
 * Copyright Theodore Ts'o, 1994, 1995, 1996, 1997, 1998, 1999.  All
 * rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/*
 * (now, with legal B.S. out of the way.....)
 *
 * This routine gathers environmental noise from device drivers, etc.,
 * and returns good random numbers, suitable for cryptographic use.
 * Besides the obvious cryptographic uses, these numbers are also good
 * for seeding TCP sequence numbers, and other places where it is
 * desirable to have numbers which are not only random, but hard to
 * predict by an attacker.
 *
 * Theory of operation
 * ===================
 *
 * Computers are very predictable devices.  Hence it is extremely hard
 * to produce truly random numbers on a computer --- as opposed to
 * pseudo-random numbers, which can easily generated by using a
 * algorithm.  Unfortunately, it is very easy for attackers to guess
 * the sequence of pseudo-random number generators, and for some
 * applications this is not acceptable.  So instead, we must try to
 * gather "environmental noise" from the computer's environment, which
 * must be hard for outside attackers to observe, and use that to
 * generate random numbers.  In a Unix environment, this is best done
 * from inside the kernel.
 *
 * Sources of randomness from the environment include inter-keyboard
 * timings, inter-interrupt timings from some interrupts, and other
 * events which are both (a) non-deterministic and (b) hard for an
 * outside observer to measure.  Randomness from these sources are
 * added to an "entropy pool", which is mixed using a CRC-like function.
 * This is not cryptographically strong, but it is adequate assuming
 * the randomness is not chosen maliciously, and it is fast enough that
 * the overhead of doing it on every interrupt is very reasonable.
 * As random bytes are mixed into the entropy pool, the routines keep
 * an *estimate* of how many bits of randomness have been stored into
 * the random number generator's internal state.
 *
 * When random bytes are desired, they are obtained by taking the SHA
 * hash of the contents of the "entropy pool".  The SHA hash avoids
 * exposing the internal state of the entropy pool.  It is believed to
 * be computationally infeasible to derive any useful information
 * about the input of SHA from its output.  Even if it is possible to
 * analyze SHA in some clever way, as long as the amount of data
 * returned from the generator is less than the inherent entropy in
 * the pool, the output data is totally unpredictable.  For this
 * reason, the routine decreases its internal estimate of how many
 * bits of "true randomness" are contained in the entropy pool as it
 * outputs random numbers.
 *
 * If this estimate goes to zero, the routine can still generate
 * random numbers; however, an attacker may (at least in theory) be
 * able to infer the future output of the generator from prior
 * outputs.  This requires successful cryptanalysis of SHA, which is
 * not believed to be feasible, but there is a remote possibility.
 * Nonetheless, these numbers should be useful for the vast majority
 * of purposes.
 *
 * Exported interfaces ---- output
 * ===============================
 *
 * There are three exported interfaces; the first is one designed to
 * be used from within the kernel:
 *
 * 	void get_random_bytes(void *buf, int nbytes);
 *
 * This interface will return the requested number of random bytes,
 * and place it in the requested buffer.
 *
 * The two other interfaces are two character devices /dev/random and
 * /dev/urandom.  /dev/random is suitable for use when very high
 * quality randomness is desired (for example, for key generation or
 * one-time pads), as it will only return a maximum of the number of
 * bits of randomness (as estimated by the random number generator)
 * contained in the entropy pool.
 *
 * The /dev/urandom device does not have this limit, and will return
 * as many bytes as are requested.  As more and more random bytes are
 * requested without giving time for the entropy pool to recharge,
 * this will result in random numbers that are merely cryptographically
 * strong.  For many applications, however, this is acceptable.
 *
 * Exported interfaces ---- input
 * ==============================
 *
 * The current exported interfaces for gathering environmental noise
 * from the devices are:
 *
 * 	void add_input_randomness(unsigned int type, unsigned int code,
 *                                unsigned int value);
 * 	void add_interrupt_randomness(int irq);
 * 	void add_disk_randomness(struct gendisk *disk);
 *
 * add_input_randomness() uses the input layer interrupt timing, as well as
 * the event type information from the hardware.
 *
 * add_interrupt_randomness() uses the inter-interrupt timing as random
 * inputs to the entropy pool.  Note that not all interrupts are good
 * sources of randomness!  For example, the timer interrupts is not a
 * good choice, because the periodicity of the interrupts is too
 * regular, and hence predictable to an attacker.  Network Interface
 * Controller interrupts are a better measure, since the timing of the
 * NIC interrupts are more unpredictable.
 *
 * add_disk_randomness() uses what amounts to the seek time of block
 * layer request events, on a per-disk_devt basis, as input to the
 * entropy pool. Note that high-speed solid state drives with very low
 * seek times do not make for good sources of entropy, as their seek
 * times are usually fairly consistent.
 *
 * All of these routines try to estimate how many bits of randomness a
 * particular randomness source.  They do this by keeping track of the
 * first and second order deltas of the event timings.
 *
 * Ensuring unpredictability at system startup
 * ============================================
 *
 * When any operating system starts up, it will go through a sequence
 * of actions that are fairly predictable by an adversary, especially
 * if the start-up does not involve interaction with a human operator.
 * This reduces the actual number of bits of unpredictability in the
 * entropy pool below the value in entropy_count.  In order to
 * counteract this effect, it helps to carry information in the
 * entropy pool across shut-downs and start-ups.  To do this, put the
 * following lines an appropriate script which is run during the boot
 * sequence:
 *
 *	echo "Initializing random number generator..."
 *	random_seed=/var/run/random-seed
 *	# Carry a random seed from start-up to start-up
 *	# Load and then save the whole entropy pool
 *	if [ -f $random_seed ]; then
 *		cat $random_seed >/dev/urandom
 *	else
 *		touch $random_seed
 *	fi
 *	chmod 600 $random_seed
 *	dd if=/dev/urandom of=$random_seed count=1 bs=512
 *
 * and the following lines in an appropriate script which is run as
 * the system is shutdown:
 *
 *	# Carry a random seed from shut-down to start-up
 *	# Save the whole entropy pool
 *	echo "Saving random seed..."
 *	random_seed=/var/run/random-seed
 *	touch $random_seed
 *	chmod 600 $random_seed
 *	dd if=/dev/urandom of=$random_seed count=1 bs=512
 *
 * For example, on most modern systems using the System V init
 * scripts, such code fragments would be found in
 * /etc/rc.d/init.d/random.  On older Linux systems, the correct script
 * location might be in /etc/rcb.d/rc.local or /etc/rc.d/rc.0.
 *
 * Effectively, these commands cause the contents of the entropy pool
 * to be saved at shut-down time and reloaded into the entropy pool at
 * start-up.  (The 'dd' in the addition to the bootup script is to
 * make sure that /etc/random-seed is different for every start-up,
 * even if the system crashes without executing rc.0.)  Even with
 * complete knowledge of the start-up activities, predicting the state
 * of the entropy pool requires knowledge of the previous history of
 * the system.
 *
 * Configuring the /dev/random driver under Linux
 * ==============================================
 *
 * The /dev/random driver under Linux uses minor numbers 8 and 9 of
 * the /dev/mem major number (#1).  So if your system does not have
 * /dev/random and /dev/urandom created already, they can be created
 * by using the commands:
 *
 * 	mknod /dev/random c 1 8
 * 	mknod /dev/urandom c 1 9
 *
 * Acknowledgements:
 * =================
 *
 * Ideas for constructing this random number generator were derived
 * from Pretty Good Privacy's random number generator, and from private
 * discussions with Phil Karn.  Colin Plumb provided a faster random
 * number generator, which speed up the mixing function of the entropy
 * pool, taken from PGPfone.  Dale Worley has also contributed many
 * useful ideas and suggestions to improve this driver.
 *
 * Any flaws in the design are solely my responsibility, and should
 * not be attributed to the Phil, Colin, or any of authors of PGP.
 *
 * Further background information on this topic may be obtained from
 * RFC 1750, "Randomness Recommendations for Security", by Donald
 * Eastlake, Steve Crocker, and Jeff Schiller.
 */

#include <linux/utsname.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/cryptohash.h>
#include <linux/fips.h>

#ifdef CONFIG_GENERIC_HARDIRQS
# include <linux/irq.h>
#endif

#include <asm/processor.h>
#include <asm/uaccess.h>
#include <asm/irq.h>
#include <asm/io.h>

/*
 * Configuration information
 */
#define INPUT_POOL_WORDS 128
#define OUTPUT_POOL_WORDS 32
#define SEC_XFER_SIZE 512
#define EXTRACT_SIZE 10

/*
 * The minimum number of bits of entropy before we wake up a read on
 * /dev/random.  Should be enough to do a significant reseed.
 */
static int random_read_wakeup_thresh = 1376;

/*
 * If the entropy count falls under this number of bits, then we
 * should wake up processes which are selecting or polling on write
 * access to /dev/random.
 */
static int random_write_wakeup_thresh = 128;

/*
 * When the input pool goes over trickle_thresh, start dropping most
 * samples to avoid wasting CPU time and reduce lock contention.
 */

static int trickle_thresh __read_mostly = INPUT_POOL_WORDS * 28;

static DEFINE_PER_CPU(int, trickle_count);

/*
 * A pool of size .poolwords is stirred with a primitive polynomial
 * of degree .poolwords over GF(2).  The taps for various sizes are
 * defined below.  They are chosen to be evenly spaced (minimum RMS
 * distance from evenly spaced; the numbers in the comments are a
 * scaled squared error sum) except for the last tap, which is 1 to
 * get the twisting happening as fast as possible.
 */
static struct poolinfo {
	int poolwords;
	int tap1, tap2, tap3, tap4, tap5;
} poolinfo_table[] = {
	/* x^128 + x^103 + x^76 + x^51 +x^25 + x + 1 -- 105 */
	{ 128,	103,	76,	51,	25,	1 },
	/* x^32 + x^26 + x^20 + x^14 + x^7 + x + 1 -- 15 */
	{ 32,	26,	20,	14,	7,	1 },
#if 0
	/* x^2048 + x^1638 + x^1231 + x^819 + x^411 + x + 1  -- 115 */
	{ 2048,	1638,	1231,	819,	411,	1 },

	/* x^1024 + x^817 + x^615 + x^412 + x^204 + x + 1 -- 290 */
	{ 1024,	817,	615,	412,	204,	1 },

	/* x^1024 + x^819 + x^616 + x^410 + x^207 + x^2 + 1 -- 115 */
	{ 1024,	819,	616,	410,	207,	2 },

	/* x^512 + x^411 + x^308 + x^208 + x^104 + x + 1 -- 225 */
	{ 512,	411,	308,	208,	104,	1 },

	/* x^512 + x^409 + x^307 + x^206 + x^102 + x^2 + 1 -- 95 */
	{ 512,	409,	307,	206,	102,	2 },
	/* x^512 + x^409 + x^309 + x^205 + x^103 + x^2 + 1 -- 95 */
	{ 512,	409,	309,	205,	103,	2 },

	/* x^256 + x^205 + x^155 + x^101 + x^52 + x + 1 -- 125 */
	{ 256,	205,	155,	101,	52,	1 },

	/* x^128 + x^103 + x^78 + x^51 + x^27 + x^2 + 1 -- 70 */
	{ 128,	103,	78,	51,	27,	2 },

	/* x^64 + x^52 + x^39 + x^26 + x^14 + x + 1 -- 15 */
	{ 64,	52,	39,	26,	14,	1 },
#endif
};

#define POOLBITS	poolwords*32
#define POOLBYTES	poolwords*4

/*
 * For the purposes of better mixing, we use the CRC-32 polynomial as
 * well to make a twisted Generalized Feedback Shift Reigster
 *
 * (See M. Matsumoto & Y. Kurita, 1992.  Twisted GFSR generators.  ACM
 * Transactions on Modeling and Computer Simulation 2(3):179-194.
 * Also see M. Matsumoto & Y. Kurita, 1994.  Twisted GFSR generators
 * II.  ACM Transactions on Mdeling and Computer Simulation 4:254-266)
 *
 * Thanks to Colin Plumb for suggesting this.
 *
 * We have not analyzed the resultant polynomial to prove it primitive;
 * in fact it almost certainly isn't.  Nonetheless, the irreducible factors
 * of a random large-degree polynomial over GF(2) are more than large enough
 * that periodicity is not a concern.
 *
 * The input hash is much less sensitive than the output hash.  All
 * that we want of it is that it be a good non-cryptographic hash;
 * i.e. it not produce collisions when fed "random" data of the sort
 * we expect to see.  As long as the pool state differs for different
 * inputs, we have preserved the input entropy and done a good job.
 * The fact that an intelligent attacker can construct inputs that
 * will produce controlled alterations to the pool's state is not
 * important because we don't consider such inputs to contribute any
 * randomness.  The only property we need with respect to them is that
 * the attacker can't increase his/her knowledge of the pool's state.
 * Since all additions are reversible (knowing the final state and the
 * input, you can reconstruct the initial state), if an attacker has
 * any uncertainty about the initial state, he/she can only shuffle
 * that uncertainty about, but never cause any collisions (which would
 * decrease the uncertainty).
 *
 * The chosen system lets the state of the pool be (essentially) the input
 * modulo the generator polymnomial.  Now, for random primitive polynomials,
 * this is a universal class of hash functions, meaning that the chance
 * of a collision is limited by the attacker's knowledge of the generator
 * polynomail, so if it is chosen at random, an attacker can never force
 * a collision.  Here, we use a fixed polynomial, but we *can* assume that
 * ###--> it is unknown to the processes generating the input entropy. <-###
 * Because of this important property, this is a good, collision-resistant
 * hash; hash collisions will occur no more often than chance.
 */

/*
 * Static global variables
 */
static DECLARE_WAIT_QUEUE_HEAD(random_read_wait);
static DECLARE_WAIT_QUEUE_HEAD(random_write_wait);
static struct fasync_struct *fasync;

#if 0
static int debug;
module_param(debug, bool, 0644);
#define DEBUG_ENT(fmt, arg...) do { \
	if (debug) \
		printk(KERN_DEBUG "random %04d %04d %04d: " \
		fmt,\
		input_pool.entropy_count,\
		blocking_pool.entropy_count,\
		nonblocking_pool.entropy_count,\
		## arg); } while (0)
#else
#define DEBUG_ENT(fmt, arg...) do {} while (0)
#endif

/**********************************************************************
 *
 * OS independent entropy store.   Here are the functions which handle
 * storing entropy in an entropy pool.
 *
 **********************************************************************/

struct entropy_store;
struct entropy_store {
	/* read-only data: */
	struct poolinfo *poolinfo;
	__u32 *pool;
	const char *name;
	struct entropy_store *pull;
	int limit;

	/* read-write data: */
	spinlock_t lock;
	unsigned add_ptr;
	int entropy_count;
	int input_rotate;
	__u8 last_data[EXTRACT_SIZE];
};

static __u32 input_pool_data[INPUT_POOL_WORDS];
static __u32 blocking_pool_data[OUTPUT_POOL_WORDS];
static __u32 nonblocking_pool_data[OUTPUT_POOL_WORDS];

static struct entropy_store input_pool = {
	.poolinfo = &poolinfo_table[0],
	.name = "input",
	.limit = 1,
	.lock = __SPIN_LOCK_UNLOCKED(&input_pool.lock),
	.pool = input_pool_data
};

static struct entropy_store blocking_pool = {
	.poolinfo = &poolinfo_table[1],
	.name = "blocking",
	.limit = 1,
	.pull = &input_pool,
	.lock = __SPIN_LOCK_UNLOCKED(&blocking_pool.lock),
	.pool = blocking_pool_data
};

static struct entropy_store nonblocking_pool = {
	.poolinfo = &poolinfo_table[1],
	.name = "nonblocking",
	.pull = &input_pool,
	.lock = __SPIN_LOCK_UNLOCKED(&nonblocking_pool.lock),
	.pool = nonblocking_pool_data
};

/*
 * This function adds bytes into the entropy "pool".  It does not
 * update the entropy estimate.  The caller should call
 * credit_entropy_bits if this is appropriate.
 *
 * The pool is stirred with a primitive polynomial of the appropriate
 * degree, and then twisted.  We twist by three bits at a time because
 * it's cheap to do so and helps slightly in the expected case where
 * the entropy is concentrated in the low-order bits.
 */
static void mix_pool_bytes_extract(struct entropy_store *r, const void *in,
				   int nbytes, __u8 out[64])
{
	static __u32 const twist_table[8] = {
		0x00000000, 0x3b6e20c8, 0x76dc4190, 0x4db26158,
		0xedb88320, 0xd6d6a3e8, 0x9b64c2b0, 0xa00ae278 };
	unsigned long i, j, tap1, tap2, tap3, tap4, tap5;
	int input_rotate;
	int wordmask = r->poolinfo->poolwords - 1;
	const char *bytes = in;
	__u32 w;
	unsigned long flags;

	/* Taps are constant, so we can load them without holding r->lock.  */
	tap1 = r->poolinfo->tap1;
	tap2 = r->poolinfo->tap2;
	tap3 = r->poolinfo->tap3;
	tap4 = r->poolinfo->tap4;
	tap5 = r->poolinfo->tap5;

	spin_lock_irqsave(&r->lock, flags);
	input_rotate = r->input_rotate;
	i = r->add_ptr;

	/* mix one byte at a time to simplify size handling and churn faster */
	while (nbytes--) {
		w = rol32(*bytes++, input_rotate & 31);
		i = (i - 1) & wordmask;

		/* XOR in the various taps */
		w ^= r->pool[i];
		w ^= r->pool[(i + tap1) & wordmask];
		w ^= r->pool[(i + tap2) & wordmask];
		w ^= r->pool[(i + tap3) & wordmask];
		w ^= r->pool[(i + tap4) & wordmask];
		w ^= r->pool[(i + tap5) & wordmask];

		/* Mix the result back in with a twist */
		r->pool[i] = (w >> 3) ^ twist_table[w & 7];

		/*
		 * Normally, we add 7 bits of rotation to the pool.
		 * At the beginning of the pool, add an extra 7 bits
		 * rotation, so that successive passes spread the
		 * input bits across the pool evenly.
		 */
		input_rotate += i ? 7 : 14;
	}

	r->input_rotate = input_rotate;
	r->add_ptr = i;

	if (out)
		for (j = 0; j < 16; j++)
			((__u32 *)out)[j] = r->pool[(i - j) & wordmask];

	spin_unlock_irqrestore(&r->lock, flags);
}

static void mix_pool_bytes(struct entropy_store *r, const void *in, int bytes)
{
       mix_pool_bytes_extract(r, in, bytes, NULL);
}

/*
 * Credit (or debit) the entropy store with n bits of entropy
 */
static void credit_entropy_bits(struct entropy_store *r, int nbits)
{
	unsigned long flags;
	int entropy_count;

	if (!nbits)
		return;

	spin_lock_irqsave(&r->lock, flags);

	DEBUG_ENT("added %d entropy credits to %s\n", nbits, r->name);
	entropy_count = r->entropy_count;
	entropy_count += nbits;
	if (entropy_count < 0) {
		DEBUG_ENT("negative entropy/overflow\n");
		entropy_count = 0;
	} else if (entropy_count > r->poolinfo->POOLBITS)
		entropy_count = r->poolinfo->POOLBITS;
	r->entropy_count = entropy_count;

	/* should we wake readers? */
	if (r == &input_pool && entropy_count >= random_read_wakeup_thresh) {
		wake_up_interruptible(&random_read_wait);
		kill_fasync(&fasync, SIGIO, POLL_IN);
	}
	spin_unlock_irqrestore(&r->lock, flags);
}

/*********************************************************************
 *
 * Entropy input management
 *
 *********************************************************************/

/* There is one of these per entropy source */
struct timer_rand_state {
	cycles_t last_time;
	long last_delta, last_delta2;
	unsigned dont_count_entropy:1;
};

#ifndef CONFIG_GENERIC_HARDIRQS

static struct timer_rand_state *irq_timer_state[NR_IRQS];

static struct timer_rand_state *get_timer_rand_state(unsigned int irq)
{
	return irq_timer_state[irq];
}

static void set_timer_rand_state(unsigned int irq,
				 struct timer_rand_state *state)
{
	irq_timer_state[irq] = state;
}

#else

static struct timer_rand_state *get_timer_rand_state(unsigned int irq)
{
	struct irq_desc *desc;

	desc = irq_to_desc(irq);

	return desc->timer_rand_state;
}

static void set_timer_rand_state(unsigned int irq,
				 struct timer_rand_state *state)
{
	struct irq_desc *desc;

	desc = irq_to_desc(irq);

	desc->timer_rand_state = state;
}
#endif

static struct timer_rand_state input_timer_state;

/*
 * This function adds entropy to the entropy "pool" by using timing
 * delays.  It uses the timer_rand_state structure to make an estimate
 * of how many bits of entropy this call has added to the pool.
 *
 * The number "num" is also added to the pool - it should somehow describe
 * the type of event which just happened.  This is currently 0-255 for
 * keyboard scan codes, and 256 upwards for interrupts.
 *
 */
static void add_timer_randomness(struct timer_rand_state *state, unsigned num)
{
	struct {
		cycles_t cycles;
		long jiffies;
		unsigned num;
	} sample;
	long delta, delta2, delta3;

	preempt_disable();
	/* if over the trickle threshold, use only 1 in 4096 samples */
	if (input_pool.entropy_count > trickle_thresh &&
	    ((__this_cpu_inc_return(trickle_count) - 1) & 0xfff))
		goto out;

	sample.jiffies = jiffies;
	sample.cycles = get_cycles();
	sample.num = num;
	mix_pool_bytes(&input_pool, &sample, sizeof(sample));

	/*
	 * Calculate number of bits of randomness we probably added.
	 * We take into account the first, second and third-order deltas
	 * in order to make our estimate.
	 */

	if (!state->dont_count_entropy) {
		delta = sample.jiffies - state->last_time;
		state->last_time = sample.jiffies;

		delta2 = delta - state->last_delta;
		state->last_delta = delta;

		delta3 = delta2 - state->last_delta2;
		state->last_delta2 = delta2;

		if (delta < 0)
			delta = -delta;
		if (delta2 < 0)
			delta2 = -delta2;
		if (delta3 < 0)
			delta3 = -delta3;
		if (delta > delta2)
			delta = delta2;
		if (delta > delta3)
			delta = delta3;

		/*
		 * delta is now minimum absolute delta.
		 * Round down by 1 bit on general principles,
		 * and limit entropy entimate to 12 bits.
		 */
		credit_entropy_bits(&input_pool,
				    min_t(int, fls(delta>>1), 11));
	}
out:
	preempt_enable();
}

void add_input_randomness(unsigned int type, unsigned int code,
				 unsigned int value)
{
	static unsigned char last_value;

	/* ignore autorepeat and the like */
	if (value == last_value)
		return;

	DEBUG_ENT("input event\n");
	last_value = value;
	add_timer_randomness(&input_timer_state,
			     (type << 4) ^ code ^ (code >> 4) ^ value);
}
EXPORT_SYMBOL_GPL(add_input_randomness);

void add_interrupt_randomness(int irq)
{
	struct timer_rand_state *state;

	state = get_timer_rand_state(irq);

	if (state == NULL)
		return;

	DEBUG_ENT("irq event %d\n", irq);
	add_timer_randomness(state, 0x100 + irq);
}

#ifdef CONFIG_BLOCK
void add_disk_randomness(struct gendisk *disk)
{
	if (!disk || !disk->random)
		return;
	/* first major is 1, so we get >= 0x200 here */
	DEBUG_ENT("disk event %d:%d\n",
		  MAJOR(disk_devt(disk)), MINOR(disk_devt(disk)));

	add_timer_randomness(disk->random, 0x100 + disk_devt(disk));
}
#endif

/*********************************************************************
 *
 * Entropy extraction routines
 *
 *********************************************************************/

static ssize_t extract_entropy(struct entropy_store *r, void *buf,
			       size_t nbytes, int min, int rsvd);

/*
 * This utility inline function is responsible for transferring entropy
 * from the primary pool to the secondary extraction pool. We make
 * sure we pull enough for a 'catastrophic reseed'.
 */
static void xfer_secondary_pool(struct entropy_store *r, size_t nbytes)
{
	__u32 tmp[OUTPUT_POOL_WORDS];

	if (r->pull && r->entropy_count < nbytes * 8 &&
	    r->entropy_count < r->poolinfo->POOLBITS) {
		/* If we're limited, always leave two wakeup worth's BITS */
		int rsvd = r->limit ? 0 : random_read_wakeup_thresh/4;
		int bytes = nbytes;

		/* pull at least as many as BYTES as wakeup BITS */
		bytes = max_t(int, bytes, random_read_wakeup_thresh / 8);
		/* but never more than the buffer size */
		bytes = min_t(int, bytes, sizeof(tmp));

		DEBUG_ENT("going to reseed %s with %d bits "
			  "(%d of %d requested)\n",
			  r->name, bytes * 8, nbytes * 8, r->entropy_count);

		bytes = extract_entropy(r->pull, tmp, bytes,
					random_read_wakeup_thresh / 8, rsvd);
		mix_pool_bytes(r, tmp, bytes);
		credit_entropy_bits(r, bytes*8);
	}
}

/*
 * These functions extracts randomness from the "entropy pool", and
 * returns it in a buffer.
 *
 * The min parameter specifies the minimum amount we can pull before
 * failing to avoid races that defeat catastrophic reseeding while the
 * reserved parameter indicates how much entropy we must leave in the
 * pool after each pull to avoid starving other readers.
 *
 * Note: extract_entropy() assumes that .poolwords is a multiple of 16 words.
 */

static size_t account(struct entropy_store *r, size_t nbytes, int min,
		      int reserved)
{
	unsigned long flags;

	/* Hold lock while accounting */
	spin_lock_irqsave(&r->lock, flags);

	BUG_ON(r->entropy_count > r->poolinfo->POOLBITS);
	DEBUG_ENT("trying to extract %d bits from %s\n",
		  nbytes * 8, r->name);

	/* Can we pull enough? */
	if (r->entropy_count / 8 < min + reserved) {
		nbytes = 0;
	} else {
		/* If limited, never pull more than available */
		if (r->limit && nbytes + reserved >= r->entropy_count / 8)
			nbytes = r->entropy_count/8 - reserved;

		if (r->entropy_count / 8 >= nbytes + reserved)
			r->entropy_count -= nbytes*8;
		else
			r->entropy_count = reserved;

		if (r->entropy_count < random_write_wakeup_thresh) {
			wake_up_interruptible(&random_write_wait);
			kill_fasync(&fasync, SIGIO, POLL_OUT);
		}
	}

	DEBUG_ENT("debiting %d entropy credits from %s%s\n",
		  nbytes * 8, r->name, r->limit ? "" : " (unlimited)");

	spin_unlock_irqrestore(&r->lock, flags);

	return nbytes;
}

static void extract_buf(struct entropy_store *r, __u8 *out)
{
	int i;
	__u32 hash[5], workspace[SHA_WORKSPACE_WORDS];
	__u8 extract[64];

	/* Generate a hash across the pool, 16 words (512 bits) at a time */
	sha_init(hash);
	for (i = 0; i < r->poolinfo->poolwords; i += 16)
		sha_transform(hash, (__u8 *)(r->pool + i), workspace);

	/*
	 * We mix the hash back into the pool to prevent backtracking
	 * attacks (where the attacker knows the state of the pool
	 * plus the current outputs, and attempts to find previous
	 * ouputs), unless the hash function can be inverted. By
	 * mixing at least a SHA1 worth of hash data back, we make
	 * brute-forcing the feedback as hard as brute-forcing the
	 * hash.
	 */
	mix_pool_bytes_extract(r, hash, sizeof(hash), extract);

	/*
	 * To avoid duplicates, we atomically extract a portion of the
	 * pool while mixing, and hash one final time.
	 */
	sha_transform(hash, extract, workspace);
	memset(extract, 0, sizeof(extract));
	memset(workspace, 0, sizeof(workspace));

	/*
	 * In case the hash function has some recognizable output
	 * pattern, we fold it in half. Thus, we always feed back
	 * twice as much data as we output.
	 */
	hash[0] ^= hash[3];
	hash[1] ^= hash[4];
	hash[2] ^= rol32(hash[2], 16);
	memcpy(out, hash, EXTRACT_SIZE);
	memset(hash, 0, sizeof(hash));
}

static ssize_t extract_entropy(struct entropy_store *r, void *buf,
			       size_t nbytes, int min, int reserved)
{
	ssize_t ret = 0, i;
	__u8 tmp[EXTRACT_SIZE];
	unsigned long flags;

	xfer_secondary_pool(r, nbytes);
	nbytes = account(r, nbytes, min, reserved);

	while (nbytes) {
		extract_buf(r, tmp);

		if (fips_enabled) {
			spin_lock_irqsave(&r->lock, flags);
			if (!memcmp(tmp, r->last_data, EXTRACT_SIZE))
				panic("Hardware RNG duplicated output!\n");
			memcpy(r->last_data, tmp, EXTRACT_SIZE);
			spin_unlock_irqrestore(&r->lock, flags);
		}
		i = min_t(int, nbytes, EXTRACT_SIZE);
		memcpy(buf, tmp, i);
		nbytes -= i;
		buf += i;
		ret += i;
	}

	/* Wipe data just returned from memory */
	memset(tmp, 0, sizeof(tmp));

	return ret;
}

static ssize_t extract_entropy_user(struct entropy_store *r, void __user *buf,
				    size_t nbytes)
{
	ssize_t ret = 0, i;
	__u8 tmp[EXTRACT_SIZE];

	xfer_secondary_pool(r, nbytes);
	nbytes = account(r, nbytes, 0, 0);

	while (nbytes) {
		if (need_resched()) {
			if (signal_pending(current)) {
				if (ret == 0)
					ret = -ERESTARTSYS;
				break;
			}
			schedule();
		}

		extract_buf(r, tmp);
		i = min_t(int, nbytes, EXTRACT_SIZE);
		if (copy_to_user(buf, tmp, i)) {
			ret = -EFAULT;
			break;
		}

		nbytes -= i;
		buf += i;
		ret += i;
	}

	/* Wipe data just returned from memory */
	memset(tmp, 0, sizeof(tmp));

	return ret;
}

/*
 * This function is the exported kernel interface.  It returns some
 * number of good random numbers, suitable for seeding TCP sequence
 * numbers, etc.
 */
void get_random_bytes(void *buf, int nbytes)
{
	extract_entropy(&nonblocking_pool, buf, nbytes, 0, 0);
}
EXPORT_SYMBOL(get_random_bytes);

/*
 * init_std_data - initialize pool with system data
 *
 * @r: pool to initialize
 *
 * This function clears the pool's entropy count and mixes some system
 * data into the pool to prepare it for use. The pool is not cleared
 * as that can only decrease the entropy in the pool.
 */
static void init_std_data(struct entropy_store *r)
{
	ktime_t now;
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	r->entropy_count = 0;
	spin_unlock_irqrestore(&r->lock, flags);

	now = ktime_get_real();
	mix_pool_bytes(r, &now, sizeof(now));
	mix_pool_bytes(r, utsname(), sizeof(*(utsname())));
}

static int rand_initialize(void)
{
	init_std_data(&input_pool);
	init_std_data(&blocking_pool);
	init_std_data(&nonblocking_pool);
	return 0;
}
module_init(rand_initialize);

void rand_initialize_irq(int irq)
{
	struct timer_rand_state *state;

	state = get_timer_rand_state(irq);

	if (state)
		return;

	/*
	 * If kzalloc returns null, we just won't use that entropy
	 * source.
	 */
	state = kzalloc(sizeof(struct timer_rand_state), GFP_KERNEL);
	if (state)
		set_timer_rand_state(irq, state);
}

#ifdef CONFIG_BLOCK
void rand_initialize_disk(struct gendisk *disk)
{
	struct timer_rand_state *state;

	/*
	 * If kzalloc returns null, we just won't use that entropy
	 * source.
	 */
	state = kzalloc(sizeof(struct timer_rand_state), GFP_KERNEL);
	if (state)
		disk->random = state;
}
#endif

static ssize_t
random_read(struct file *file, char __user *buf, size_t nbytes, loff_t *ppos)
{
	return extract_entropy_user(&nonblocking_pool, buf, nbytes);
}

static ssize_t
urandom_read(struct file *file, char __user *buf, size_t nbytes, loff_t *ppos)
{
	return extract_entropy_user(&nonblocking_pool, buf, nbytes);
}

static unsigned int
random_poll(struct file *file, poll_table * wait)
{
	unsigned int mask;

	poll_wait(file, &random_read_wait, wait);
	poll_wait(file, &random_write_wait, wait);
	mask = 0;
	if (input_pool.entropy_count >= random_read_wakeup_thresh)
		mask |= POLLIN | POLLRDNORM;
	if (input_pool.entropy_count < random_write_wakeup_thresh)
		mask |= POLLOUT | POLLWRNORM;
	return mask;
}

static int
write_pool(struct entropy_store *r, const char __user *buffer, size_t count)
{
	size_t bytes;
	__u32 buf[16];
	const char __user *p = buffer;

	while (count > 0) {
		bytes = min(count, sizeof(buf));
		if (copy_from_user(&buf, p, bytes))
			return -EFAULT;

		count -= bytes;
		p += bytes;

		mix_pool_bytes(r, buf, bytes);
		cond_resched();
	}

	return 0;
}

static ssize_t random_write(struct file *file, const char __user *buffer,
			    size_t count, loff_t *ppos)
{
	size_t ret;

	ret = write_pool(&blocking_pool, buffer, count);
	if (ret)
		return ret;
	ret = write_pool(&nonblocking_pool, buffer, count);
	if (ret)
		return ret;

	return (ssize_t)count;
}

static long random_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	int size, ent_count;
	int __user *p = (int __user *)arg;
	int retval;

	switch (cmd) {
	case RNDGETENTCNT:
		/* inherently racy, no point locking */
		if (put_user(input_pool.entropy_count, p))
			return -EFAULT;
		return 0;
	case RNDADDTOENTCNT:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(ent_count, p))
			return -EFAULT;
		credit_entropy_bits(&input_pool, ent_count);
		return 0;
	case RNDADDENTROPY:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(ent_count, p++))
			return -EFAULT;
		if (ent_count < 0)
			return -EINVAL;
		if (get_user(size, p++))
			return -EFAULT;
		retval = write_pool(&input_pool, (const char __user *)p,
				    size);
		if (retval < 0)
			return retval;
		credit_entropy_bits(&input_pool, ent_count);
		return 0;
	case RNDZAPENTCNT:
	case RNDCLEARPOOL:
		/* Clear the entropy pool counters. */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		rand_initialize();
		return 0;
	default:
		return -EINVAL;
	}
}

static int random_fasync(int fd, struct file *filp, int on)
{
	return fasync_helper(fd, filp, on, &fasync);
}

const struct file_operations random_fops = {
	.read  = random_read,
	.write = random_write,
	.poll  = random_poll,
	.unlocked_ioctl = random_ioctl,
	.fasync = random_fasync,
	.llseek = noop_llseek,
};

const struct file_operations urandom_fops = {
	.read  = urandom_read,
	.write = random_write,
	.unlocked_ioctl = random_ioctl,
	.fasync = random_fasync,
	.llseek = noop_llseek,
};

/***************************************************************
 * Random UUID interface
 *
 * Used here for a Boot ID, but can be useful for other kernel
 * drivers.
 ***************************************************************/

/*
 * Generate random UUID
 */
void generate_random_uuid(unsigned char uuid_out[16])
{
	get_random_bytes(uuid_out, 16);
	/* Set UUID version to 4 --- truly random generation */
	uuid_out[6] = (uuid_out[6] & 0x0F) | 0x40;
	/* Set the UUID variant to DCE */
	uuid_out[8] = (uuid_out[8] & 0x3F) | 0x80;
}
EXPORT_SYMBOL(generate_random_uuid);

/********************************************************************
 *
 * Sysctl interface
 *
 ********************************************************************/

#ifdef CONFIG_SYSCTL

#include <linux/sysctl.h>

static int min_read_thresh = 8, min_write_thresh;
static int max_read_thresh = INPUT_POOL_WORDS * 32;
static int max_write_thresh = INPUT_POOL_WORDS * 32;
static char sysctl_bootid[16];

/*
 * These functions is used to return both the bootid UUID, and random
 * UUID.  The difference is in whether table->data is NULL; if it is,
 * then a new UUID is generated and returned to the user.
 *
 * If the user accesses this via the proc interface, it will be returned
 * as an ASCII string in the standard UUID format.  If accesses via the
 * sysctl system call, it is returned as 16 bytes of binary data.
 */
static int proc_do_uuid(ctl_table *table, int write,
			void __user *buffer, size_t *lenp, loff_t *ppos)
{
	ctl_table fake_table;
	unsigned char buf[64], tmp_uuid[16], *uuid;

	uuid = table->data;
	if (!uuid) {
		uuid = tmp_uuid;
		uuid[8] = 0;
	}
	if (uuid[8] == 0)
		generate_random_uuid(uuid);

	sprintf(buf, "%pU", uuid);

	fake_table.data = buf;
	fake_table.maxlen = sizeof(buf);

	return proc_dostring(&fake_table, write, buffer, lenp, ppos);
}

static int sysctl_poolsize = INPUT_POOL_WORDS * 32;
ctl_table random_table[] = {
	{
		.procname	= "poolsize",
		.data		= &sysctl_poolsize,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "entropy_avail",
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= proc_dointvec,
		.data		= &input_pool.entropy_count,
	},
	{
		.procname	= "read_wakeup_threshold",
		.data		= &random_read_wakeup_thresh,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &min_read_thresh,
		.extra2		= &max_read_thresh,
	},
	{
		.procname	= "write_wakeup_threshold",
		.data		= &random_write_wakeup_thresh,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &min_write_thresh,
		.extra2		= &max_write_thresh,
	},
	{
		.procname	= "boot_id",
		.data		= &sysctl_bootid,
		.maxlen		= 16,
		.mode		= 0444,
		.proc_handler	= proc_do_uuid,
	},
	{
		.procname	= "uuid",
		.maxlen		= 16,
		.mode		= 0444,
		.proc_handler	= proc_do_uuid,
	},
	{ }
};
#endif 	/* CONFIG_SYSCTL */

static u32 random_int_secret[MD5_MESSAGE_BYTES / 4] ____cacheline_aligned;

static int __init random_int_secret_init(void)
{
	get_random_bytes(random_int_secret, sizeof(random_int_secret));
	return 0;
}
late_initcall(random_int_secret_init);

/*
 * Get a random word for internal kernel use only. Similar to urandom but
 * with the goal of minimal entropy pool depletion. As a result, the random
 * value is not cryptographically secure but for several uses the cost of
 * depleting entropy is too high
 */
DEFINE_PER_CPU(__u32 [MD5_DIGEST_WORDS], get_random_int_hash);
unsigned int get_random_int(void)
{
	__u32 *hash = get_cpu_var(get_random_int_hash);
	unsigned int ret;

	hash[0] += current->pid + jiffies + get_cycles();
	md5_transform(hash, random_int_secret);
	ret = hash[0];
	put_cpu_var(get_random_int_hash);

	return ret;
}

/*
 * randomize_range() returns a start address such that
 *
 *    [...... <range> .....]
 *  start                  end
 *
 * a <range> with size "len" starting at the return value is inside in the
 * area defined by [start, end], but is otherwise randomized.
 */
unsigned long
randomize_range(unsigned long start, unsigned long end, unsigned long len)
{
	unsigned long range = end - len - start;

	if (end <= start + len)
		return 0;
	return PAGE_ALIGN(get_random_int() % range + start);
}
