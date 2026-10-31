/*
 * mtest (2026-10-29) - find where `sort` corrupts data on the FPGA board.
 *
 * Board: `seq 1 20000 | sort -r` returns 4 consecutive line pointers each
 * replaced by the next one (one lost, one duplicated) ~3 runs in 10, while
 * md5sum/cat/pipes are clean.  sort = glibc qsort (msort_with_tmp, merge
 * into a malloc'd buffer, memcpy back).  Usage:
 *   mtest q [rounds]   qsort of 20000 pointers, check order + permutation
 *   mtest m [rounds]   memcpy of random sizes/offsets, check each copy
 *   mtest c [rounds]   plain C word copy loop (no glibc), check each copy
 *   mtest q N [rounds] qsort of N pointers (N <= 20000; small N fits the D$)
 *   mtest s [rounds]   own merge sort of 20000 pointers, inline compare (no
 *                      calls through a function pointer), same data sizes
 *   mtest M [rounds]   memcpy of random sizes between two 256 KB buffers
 *   mtest r [rounds]   random word writes over 256 KB, then read back
 *   mtest S [rounds]   own merge sort, but the tmp buffer is malloc'd/freed
 *                      every round like glibc qsort does (80 KB -> mmap)
 *   mtest a [rounds]   malloc 80 KB, fill, make syscalls, check, free (x50)
 *   mtest P [rounds]   own merge sort, compare through a function pointer
 *                      to the same cmp_desc qsort uses (glibc call shape)
 *   mtest Q [rounds]   glibc qsort with cmp_flat: no pointer chasing in the
 *                      comparator (keys compared by address order)
 *   mtest L [rounds]   glibc qsort with a logging comparator: every call's
 *                      (element a, element b, result) is recorded, and a bad
 *                      round is diffed against the first clean round to show
 *                      the first call where the sequence diverges
 *   mtest b [rounds]   glibc memcpy of 4/8/12 bytes (its byte loop) from a
 *                      freshly sw-written source to a destination in the
 *                      SAME D$ set (16 KB apart): every byte ping-pongs the
 *                      line (dirty writeback, then refill of the other)
 *   mtest n [rounds]   the same, destination in a different D$ set
 */
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define N 20000

static void out(const char *s) { write(1, s, strlen(s)); }
static void hex(unsigned long v)
{
	char b[9];
	for (int i = 7; i >= 0; i--, v >>= 4)
		b[i] = "0123456789abcdef"[v & 15];
	b[8] = ' ';
	write(1, b, 9);
}
static void dec(unsigned long v)
{
	char b[12];
	int i = 11;
	b[i] = ' ';
	do { b[--i] = '0' + v % 10; v /= 10; } while (v);
	write(1, b + i, 12 - i);
}

static unsigned keys[N];
static unsigned *ptrs[N];
static unsigned char seen[N];

static int cmp_desc(const void *a, const void *b)
{
	unsigned x = **(unsigned *const *)a, y = **(unsigned *const *)b;
	return x < y ? 1 : x > y ? -1 : 0;
}

/* same order as cmp_desc, but no load-to-load chain: keys[i] is a
 * permutation, so compare the key values via one plain load each */
static int cmp_flat(const void *a, const void *b)
{
	const unsigned *pa = *(unsigned *const *)a, *pb = *(unsigned *const *)b;
	unsigned x = keys[pa - keys], y = keys[pb - keys];
	return (x < y) - (x > y);
}

static int (*volatile cmpf)(const void *, const void *) = cmp_desc;

static unsigned rnd_state = 12345;
static unsigned rnd(void)
{
	rnd_state = rnd_state * 1103515245u + 12345u;
	return rnd_state >> 8;
}

static int nq = N;
static int qsort_flat;

static int test_qsort(int round)
{
	int bad = 0;
	int N_ = nq;

	for (int i = 0; i < N_; i++) {
		keys[i] = (unsigned)(i * 7919u) % N_;	/* a permutation (N_ odd or 7919-coprime) */
		ptrs[i] = &keys[i];
	}
	qsort(ptrs, N_, sizeof(ptrs[0]), qsort_flat ? cmp_flat : cmp_desc);
	memset(seen, 0, sizeof(seen));
	for (int i = 0; i < N_; i++) {
		unsigned *p = ptrs[i];
		unsigned idx = p - keys;
		if (idx >= (unsigned)N_ || *p != (unsigned)(N_ - 1 - i) || seen[idx]++) {
			if (bad < 8) {
				out("Q round "); dec(round); out("i "); dec(i);
				out("ptr "); hex((unsigned long)p);
				out("key "); dec(idx < N ? *p : 0xffffffffu);
				out("want "); dec(N_ - 1 - i);
				out("ptrs@"); hex((unsigned long)&ptrs[i]);
				out("\n");
			}
			bad++;
		}
	}
	return bad;
}

/* ---- mode L: log every comparison ---- */
#define LOGMAX 300000
static unsigned lg_ab[LOGMAX], ref_ab[LOGMAX];	/* idx(a) << 17 | idx(b) << 2 | (r + 1) */
static unsigned lg_n, ref_n, lg_glitch;
static int have_ref;

static int cmp_log(const void *a, const void *b)
{
	const unsigned *pa = *(unsigned *const *)a, *pb = *(unsigned *const *)b;
	unsigned x = *pa, y = *pb;
	int r = x < y ? 1 : x > y ? -1 : 0;
	unsigned ia = pa - keys, ib = pb - keys;
	/* re-read the keys through a different path: catches a bad load above */
	if (ia >= N || ib >= N || ((volatile unsigned *)keys)[ia] != x ||
	    ((volatile unsigned *)keys)[ib] != y)
		lg_glitch++;
	if (lg_n < LOGMAX)
		lg_ab[lg_n] = ia << 17 | (ib & 0x7fff) << 2 | (r + 1);
	lg_n++;
	return r;
}

static void show_call(const char *tag, unsigned k, unsigned ab)
{
	unsigned ia = ab >> 17, ib = ab >> 2 & 0x7fff;
	int r = (int)(ab & 3) - 1;
	out(tag); dec(k);
	out("a "); dec(ia); out("(key "); dec(ia < N ? keys[ia] : 0); out(") b ");
	dec(ib); out("(key "); dec(ib < N ? keys[ib] : 0); out(") r ");
	out(r > 0 ? "1\n" : r < 0 ? "-1\n" : "0\n");
}

static int test_qsort_log(int round)
{
	lg_n = 0;
	lg_glitch = 0;
	int bad = (qsort_flat = 0, nq = N, 0);
	for (int i = 0; i < N; i++) {
		keys[i] = (unsigned)(i * 7919u) % N;
		ptrs[i] = &keys[i];
	}
	qsort(ptrs, N, sizeof(ptrs[0]), cmp_log);
	memset(seen, 0, sizeof(seen));
	for (int i = 0; i < N; i++) {
		unsigned *p = ptrs[i];
		unsigned idx = p - keys;
		if (idx >= N || *p != (unsigned)(N - 1 - i) || seen[idx]++)
			bad++;
	}
	if (lg_glitch) { out("L round "); dec(round); out("cmp load glitches "); dec(lg_glitch); out("\n"); }
	if (!bad && !have_ref) {
		memcpy(ref_ab, lg_ab, sizeof(ref_ab));
		ref_n = lg_n;
		have_ref = 1;
		out("L reference: "); dec(lg_n); out("calls\n");
	} else if (have_ref && (bad || lg_n != ref_n)) {
		unsigned k = 0, n = lg_n < ref_n ? lg_n : ref_n;
		if (n > LOGMAX) n = LOGMAX;
		while (k < n && lg_ab[k] == ref_ab[k])
			k++;
		out("L round "); dec(round); out("bad "); dec(bad); out("calls "); dec(lg_n);
		out("ref "); dec(ref_n); out("first diff at call "); dec(k); out("\n");
		for (unsigned j = k >= 3 ? k - 3 : 0; j < k + 3 && j < n; j++) {
			show_call("  ref ", j, ref_ab[j]);
			show_call("  got ", j, lg_ab[j]);
		}
	} else if (bad)
		out("L bad round before a reference exists\n");
	return bad;
}

static unsigned srcbuf[4096 + 16], dstbuf[4096 + 16];
static int check_copy(const char *tag, int round, unsigned *d, unsigned *s, int words);
static unsigned *tmpbuf[N];

static int own_sort_fptr;

/* own top-down merge sort, descending by key, compare inlined */
static void msort_own(unsigned **a, unsigned **t, int n)
{
	if (n < 2)
		return;
	int n1 = n / 2, n2 = n - n1;
	msort_own(a, t, n1);
	msort_own(a + n1, t, n2);
	unsigned **b1 = a, **b2 = a + n1, **o = t;
	while (n1 > 0 && n2 > 0) {
		if (own_sort_fptr ? cmpf(b1, b2) <= 0 : **b1 >= **b2) { *o++ = *b1++; n1--; }
		else { *o++ = *b2++; n2--; }
	}
	while (n1 > 0) { *o++ = *b1++; n1--; }
	for (int i = 0; i < o - t; i++)
		a[i] = t[i];
}

static int own_sort_malloc;

static int test_own_sort(int round)
{
	int bad = 0;
	unsigned **t = tmpbuf;
	if (own_sort_malloc) {
		t = malloc(N * sizeof(*t));
		if (!t) { out("malloc failed\n"); return 1; }
		if (round < 3) { out("tmp@"); hex((unsigned long)t); out("\n"); }
	}
	for (int i = 0; i < N; i++) {
		keys[i] = (unsigned)(i * 7919u) % N;
		ptrs[i] = &keys[i];
	}
	msort_own(ptrs, t, N);
	if (own_sort_malloc)
		free(t);
	memset(seen, 0, sizeof(seen));
	for (int i = 0; i < N; i++) {
		unsigned *p = ptrs[i];
		unsigned idx = p - keys;
		if (idx >= N || *p != (unsigned)(N - 1 - i) || seen[idx]++) {
			if (bad < 8) {
				out("S round "); dec(round); out("i "); dec(i);
				out("key "); dec(idx < N ? *p : 0xffffffffu);
				out("want "); dec(N - 1 - i);
				out("ptrs@"); hex((unsigned long)&ptrs[i]); out("\n");
			}
			bad++;
		}
	}
	return bad;
}

#define BIG (1 << 16)		/* words = 256 KB, far beyond the 16 KB D$ */
static unsigned bigsrc[BIG + 64], bigdst[BIG + 64];

static int test_bigcopy(int round)
{
	int bad = 0;
	for (int i = 0; i < BIG; i++)
		bigsrc[i] = i ^ (round << 24) ^ 0x5a000000u;
	for (int k = 0; k < 400; k++) {
		int words = 4 + rnd() % 1024;	/* up to 4 KB */
		int so = rnd() % (BIG - 1100), dof = rnd() % (BIG - 1100);
		unsigned *s = bigsrc + 8 + so, *d = bigdst + 8 + dof;
		for (int i = -2; i < words + 2; i++)
			d[i] = 0x6b6b6b6bu;
		memcpy(d, s, words * 4);
		bad += check_copy("B", round, d, s, words);
	}
	return bad;
}

static int test_random(int round)
{
	int bad = 0;
	unsigned seed = 777u + round;
	for (int i = 0; i < BIG; i++)
		bigdst[i] = 0;
	rnd_state = seed;
	for (int k = 0; k < 200000; k++) {
		unsigned idx = rnd() % BIG;
		bigdst[idx] += idx ^ 0x01010101u;
	}
	/* replay to compute the expected contents in bigsrc */
	for (int i = 0; i < BIG; i++)
		bigsrc[i] = 0;
	rnd_state = seed;
	for (int k = 0; k < 200000; k++) {
		unsigned idx = rnd() % BIG;
		bigsrc[idx] += idx ^ 0x01010101u;
	}
	for (int i = 0; i < BIG; i++)
		if (bigdst[i] != bigsrc[i]) {
			if (bad < 8) {
				out("R round "); dec(round); out("idx "); dec(i);
				out("got "); hex(bigdst[i]); out("want "); hex(bigsrc[i]);
				out("@"); hex((unsigned long)&bigdst[i]); out("\n");
			}
			bad++;
		}
	return bad;
}

static int check_copy(const char *tag, int round, unsigned *d, unsigned *s, int words)
{
	int bad = 0;
	for (int i = -2; i < words + 2; i++) {
		unsigned want = (i < 0 || i >= words) ? 0x6b6b6b6bu : s[i];
		if (d[i] != want) {
			if (bad < 4) {
				out(tag); out(" round "); dec(round); out("n "); dec(words);
				out("i "); dec(i); out("got "); hex(d[i]); out("want "); hex(want);
				out("dst@"); hex((unsigned long)&d[i]); out("\n");
			}
			bad++;
		}
	}
	return bad;
}

static void wordcopy_c(unsigned *d, const unsigned *s, int n)
{
	for (int i = 0; i < n; i++)
		((volatile unsigned *)d)[i] = s[i];
}

static int test_copy(int round, int use_glibc)
{
	int bad = 0;
	for (int k = 0; k < 2000; k++) {
		int words = 4 + rnd() % 64;
		int so = rnd() % 2048, dof = rnd() % 2048;
		unsigned *s = srcbuf + 8 + so, *d = dstbuf + 8 + dof;
		for (int i = 0; i < words; i++)
			s[i] = (round << 20) ^ (k << 8) ^ i ^ 0xa5000000u;
		for (int i = -2; i < words + 2; i++)
			d[i] = 0x6b6b6b6bu;
		if (use_glibc)
			memcpy(d, s, words * 4);
		else
			wordcopy_c(d, s, words);
		bad += check_copy(use_glibc ? "M" : "C", round, d, s, words);
	}
	return bad;
}

/* through a pointer so gcc cannot inline or specialise the small copy */
static void *(*volatile memcpy_p)(void *, const void *, size_t) = memcpy;

static int test_small(int round, int alias)
{
	int bad = 0;
	for (int k = 0; k < 20000; k++) {
		int words = 1 + k % 3;
		unsigned off = (unsigned)(k * 68 + round * 4) % (BIG * 4 - 256) & ~3u;
		unsigned *s = (unsigned *)((char *)bigsrc + off);
		unsigned delta = ((unsigned long)s - (unsigned long)bigdst) & 16383;
		if (!alias)
			delta = (delta + 2048) & 16383;
		unsigned *d = (unsigned *)((char *)bigdst + delta + (k & 7) * 16384);
		unsigned guard = d[words];
		for (int i = 0; i < words; i++)
			((volatile unsigned *)s)[i] = (round << 24) ^ (k << 6) ^ (i << 2) ^ 0x00800000u;
		memcpy_p(d, s, words * 4);
		for (int i = 0; i <= words; i++) {
			unsigned want = i < words ? s[i] : guard;
			if (d[i] != want) {
				if (bad < 8) {
					out(alias ? "b" : "n"); out(" round "); dec(round); out("k "); dec(k);
					out("i "); dec(i); out("got "); hex(d[i]); out("want "); hex(want);
					out("dst@"); hex((unsigned long)&d[i]); out("src@"); hex((unsigned long)s);
					out("\n");
				}
				bad++;
			}
		}
	}
	return bad;
}

static int test_alloc(int round)
{
	int bad = 0;
	for (int k = 0; k < 50; k++) {
		unsigned n = 20000, tag = (round << 16) ^ (k << 8) ^ 0xc3000000u;
		unsigned *b = malloc(n * 4);
		if (!b) { out("malloc failed\n"); return 1; }
		for (unsigned i = 0; i < n; i++)
			b[i] = tag ^ i;
		for (int j = 0; j < 200; j++)
			(void)getppid();
		for (unsigned i = 0; i < n; i++)
			if (b[i] != (tag ^ i)) {
				if (bad < 8) {
					out("A round "); dec(round); out("k "); dec(k);
					out("i "); dec(i); out("got "); hex(b[i]);
					out("want "); hex(tag ^ i); out("@"); hex((unsigned long)&b[i]);
					out("\n");
				}
				bad++;
			}
		free(b);
	}
	return bad;
}

int main(int argc, char **argv)
{
	char mode = argc > 1 ? argv[1][0] : 'q';
	int argi = 2;
	if ((mode == 'q' || mode == 'Q') && argc > 3) {
		nq = atoi(argv[2]);
		if (nq < 2 || nq > N)
			nq = N;
		argi = 3;
	}
	int rounds = argc > argi ? atoi(argv[argi]) : 20;
	int total = 0;
	char tag[2] = { mode, 0 };

	for (int r = 0; r < rounds; r++) {
		int bad = mode == 'L' ? test_qsort_log(r) :
			  mode == 'b' || mode == 'n' ? test_small(r, mode == 'b') :
			  mode == 'q' || mode == 'Q' ? (qsort_flat = mode == 'Q', test_qsort(r)) :
			  mode == 'P' ? (own_sort_fptr = 1, own_sort_malloc = 0, test_own_sort(r)) :
			  mode == 's' || mode == 'S' ? (own_sort_malloc = mode == 'S', test_own_sort(r)) :
			  mode == 'a' ? test_alloc(r) :
			  mode == 'M' ? test_bigcopy(r) :
			  mode == 'r' ? test_random(r) : test_copy(r, mode == 'm');
		total += bad;
		out(tag);
		out(" round "); dec(r); out(bad ? "BAD " : "ok "); dec(bad); out("\n");
	}
	out("total errors "); dec(total); out("\n");
	return total != 0;
}
