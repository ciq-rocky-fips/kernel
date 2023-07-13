// SPDX-License-Identifier: GPL-2.0-or-later

extern void *jent_zalloc(unsigned int len);
extern void jent_zfree(void *ptr);
extern int jent_fips_enabled(void);
extern void jent_panic(char *s);
extern void jent_memcpy(void *dest, const void *src, unsigned int n);
extern void jent_get_nstime(__u64 *out);

struct rand_data;

enum fips_failprobe {
	FIPS_NOFAIL = 0,
	FIPS_FAIL_STUCK = 1,
	FIPS_FAIL_REPETITION_COUNT = 2,
	FIPS_FAIL_ADAPTIVE_PROPORTION = 3
};

extern int jent_entropy_init(enum fips_failprobe fips_fail_probe);
extern int jent_read_entropy(struct rand_data *ec, unsigned char *data,
			     unsigned int len);

extern struct rand_data *jent_entropy_collector_alloc(unsigned int osr,
						      unsigned int flags);
extern void jent_entropy_collector_free(struct rand_data *entropy_collector);

/* -- error codes for init function -- */
#define JENT_ENOTIME            1 /* Timer service not available */
#define JENT_ECOARSETIME        2 /* Timer too coarse for RNG */
#define JENT_ENOMONOTONIC       3 /* Timer is not monotonic increasing */
#define JENT_EVARVAR            5 /* Timer does not produce variations of
                                   * variations (2nd derivation of time is
                                   * zero). */
#define JENT_ESTUCK             8 /* Too many stuck results during init. */
#define JENT_EHEALTH            9 /* Health test failed during initialization */
#define JENT_ERCT               10 /* RCT failed during initialization */
