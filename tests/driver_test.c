// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the driver logic against a modeled HDA bridge. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

typedef uint8_t u8;
typedef uint16_t u16;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define module_param_named(...)
#define MODULE_PARM_DESC(...)
#define AC_VERB_SET_COEF_INDEX 0x500
#define AC_VERB_SET_PROC_COEF 0x400
#define AC_VERB_GET_PROC_COEF 0xc00
#define AC_VERB_GET_COEF_INDEX 0xd00
#define HDA_GEN_PCM_ACT_PREPARE 2
#define HDA_FIXUP_ACT_PRE_PROBE 1
#define HDA_FIXUP_ACT_PROBE 2
#define HDA_FIXUP_ACT_INIT 3
#define GFP_KERNEL 0

struct hda_pcm_stream {
};

struct snd_pcm_runtime {
	unsigned int rate;
};

struct snd_pcm_substream {
	struct snd_pcm_runtime *runtime;
};

struct hda_fixup {
};

struct hda_codec;
struct alc274_rt1305;
struct alc_spec {
	struct {
		void (*pcm_playback_hook)(struct hda_pcm_stream *hinfo,
					  struct hda_codec *codec,
					  struct snd_pcm_substream *substream, int action);
	} gen;
	struct alc274_rt1305 *rt1305;
	int coef_mutex;
};

struct test_bus {
	bool shutdown;
};

struct hda_codec {
	struct {
		unsigned int vendor_id, subsystem_id;
		int dev;
	} core;
	struct alc_spec *spec;
	struct test_bus *bus;
};

static u16 coefs[256], amp[256], before_amp[256], selected;
static unsigned int writes, locks, polls, pending, busy_left;
static bool fail_control_read, volatile_controls;
static void (*on_lock)(struct hda_codec *codec);
static struct {
	unsigned int reg, mask, value, remaining;
	bool timeout;
} fault;

#define lockdep_assert_held(mutex) do { (void)(mutex); assert(locks == 1); } while (0)
static void log_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void log_message(const char *fmt, ...)
{
	(void)fmt;
}

#define codec_err(c, ...) do { (void)(c); log_message(__VA_ARGS__); } while (0)
#define codec_warn(c, ...) codec_err(c, __VA_ARGS__)
#define codec_dbg(c, ...) codec_err(c, __VA_ARGS__)

static void *devm_kzalloc(void *dev, size_t n, int flags)
{
	(void)dev;
	(void)flags;
	return calloc(1, n);
}

static void coef_mutex_lock(struct hda_codec *c)
{
	void (*callback)(struct hda_codec *codec) = on_lock;

	assert(!locks);
	locks++;
	on_lock = NULL;
	if (callback)
		callback(c);
}

static void coef_mutex_unlock(struct hda_codec *c)
{
	(void)c;
	assert(locks == 1);
	locks--;
}

static void usleep_range(unsigned int a, unsigned int b)
{
	(void)a;
	(void)b;
}

static bool allowed(unsigned int r)
{
	return (r >= 0xc0 && r <= 0xfb) || r == 0x14 || r == 0x40 || r == 0x46 || r == 0x4b ||
	       r == 0x4c;
}

static void complete(void)
{
	unsigned int r = coefs[0x26];

	if (!pending)
		return;
	assert(coefs[0x24] == 0x10);
	if (pending == 0xb034) {
		coefs[0x2b] = amp[r];
		if (volatile_controls && (r == 0x40 || r == 0x4c))
			coefs[0x2b] ^= 0x8000;
	} else {
		assert(pending == 0xb024 && allowed(r));
		amp[r] = coefs[0x28];
		writes++;
	}
	pending = 0;
}

static unsigned int snd_hda_codec_read(struct hda_codec *c, unsigned int n, int f,
				       unsigned int v, unsigned int p)
{
	(void)c;
	(void)n;
	(void)f;
	(void)p;
	assert(locks);
	if (v == AC_VERB_GET_COEF_INDEX)
		return selected;
	assert(v == AC_VERB_GET_PROC_COEF);
	if (selected == 0x29) {
		polls++;
		if (fail_control_read) {
			fail_control_read = false;
			return ~0U;
		}
		if (busy_left) {
			busy_left--;
			return 0x4000;
		}
		complete();
		return 0;
	}
	return coefs[selected];
}

static int snd_hda_codec_write(struct hda_codec *c, unsigned int n, int f,
			      unsigned int v, unsigned int p)
{
	(void)c;
	(void)n;
	(void)f;
	assert(locks);
	if (v == AC_VERB_SET_COEF_INDEX) {
		selected = p;
		return 0;
	}
	assert(v == AC_VERB_SET_PROC_COEF);
	coefs[selected] = p;
	if (selected == 0x29) {
		assert(!pending);
		pending = p;
		busy_left = 1;
		if (p == 0xb024 && fault.remaining && coefs[0x26] == fault.reg &&
		    (coefs[0x28] & fault.mask) == fault.value) {
			if (fault.remaining != UINT_MAX)
				fault.remaining--;
			/* Model a committed write followed by an error or timeout. */
			complete();
			busy_left = fault.timeout ? 50 : 0;
			return fault.timeout ? 0 : -EIO;
		}
	}
	return 0;
}

/* The parent Realtek translation unit declares this before its free hook. */
static void alc274_rt1305_restore(struct hda_codec *codec);
#include "../kernel/alc274_rt1305.c"
static struct alc_spec spec;
static struct test_bus bus;
static struct hda_codec codec = {
	.core = { .vendor_id = 0x10ec0274, .subsystem_id = 0x1d051115 },
	.spec = &spec,
	.bus = &bus,
};

static void reset(void)
{
	free(spec.rt1305);
	memset(&spec, 0, sizeof(spec));
	memset(&bus, 0, sizeof(bus));
	memset(coefs, 0, sizeof(coefs));
	memset(amp, 0, sizeof(amp));
	for (unsigned int r = 0; r < 256; r++)
		amp[r] = (r * 97 + 17) & 0xffff;
	amp[0x7c] = 0x10ec;
	amp[0x7e] = 0x6251;
	amp[0x14] = 0x123;
	for (unsigned int r = 0x24; r <= 0x28; r++)
		coefs[r] = r;
	selected = 0x71;
	writes = 0;
	locks = 0;
	polls = 0;
	pending = 0;
	busy_left = 0;
	fail_control_read = false;
	volatile_controls = false;
	memset(&fault, 0, sizeof(fault));
	on_lock = NULL;
	codec.core.vendor_id = 0x10ec0274;
	codec.core.subsystem_id = 0x1d051115;
	alc274_rt1305_enable = true;
	memcpy(before_amp, amp, sizeof(amp));
	alc274_fixup_rt1305(&codec, NULL, HDA_FIXUP_ACT_PRE_PROBE);
	alc274_fixup_rt1305(&codec, NULL, HDA_FIXUP_ACT_PROBE);
}

static void init(void)
{
	alc274_fixup_rt1305(&codec, NULL, HDA_FIXUP_ACT_INIT);
	assert(!locks);
}

static void original(void)
{
	assert(!memcmp(amp, before_amp, sizeof(amp)));
}

static void staging_restored(void)
{
	for (unsigned int r = 0x24; r <= 0x28; r++)
		assert(coefs[r] == r);
	assert(selected == 0x71);
}

static void test_apply(void)
{
	init();
	assert(spec.rt1305->applied && !spec.rt1305->failed);
	assert(writes == 66);
	staging_restored();
	for (unsigned int i = 0; i < RT1305_EQ_COUNT; i++)
		assert(amp[alc274_rt1305_eq[i].reg] == alc274_rt1305_eq[i].value);
	assert(amp[0x14] == before_amp[0x14]);
	assert(amp[0x50] == before_amp[0x50]);
	assert(amp[0x62] == before_amp[0x62]);
}

static void test_reinit(void)
{
	unsigned int count;

	init();
	count = writes;
	init();
	assert(writes == count + 4);
	assert(spec.rt1305->applied && !spec.rt1305->failed);
	staging_restored();
}

static void test_unload_restore(void)
{
	init();
	alc274_rt1305_restore(&codec);
	original();
	staging_restored();
}

static void test_wrong_amplifier(void)
{
	amp[0x7c] = 0;
	init();
	init();
	init();
	assert(!writes && spec.rt1305->failed);
	staging_restored();
}

static void test_partial_write_rollback(void)
{
	unsigned int count;

	fault.reg = 0xc0;
	fault.remaining = 1;
	init();
	assert(spec.rt1305->failed && !spec.rt1305->applied && !spec.rt1305->dirty);
	original();
	staging_restored();
	count = writes;
	init();
	assert(writes == count);
}

static void test_rollback_retry(void)
{
	fault.reg = 0xc0;
	fault.remaining = UINT_MAX;
	init();
	assert(spec.rt1305->failed && spec.rt1305->dirty);
	assert(amp[0x14] & RT1305_MUTE);
	fault.remaining = 0;
	alc274_rt1305_restore(&codec);
	/* Unload retries a recovered transport and restores the saved mute state. */
	for (unsigned int i = 0; i < RT1305_EQ_COUNT; i++)
		assert(amp[alc274_rt1305_eq[i].reg] == before_amp[alc274_rt1305_eq[i].reg]);
	original();
}

static void test_bridge_read_failure(void)
{
	fail_control_read = true;
	init();
	assert(!writes && !spec.rt1305->baseline_valid);
	staging_restored();
}

static void test_volatile_controls(void)
{
	volatile_controls = true;
	init();
	assert(spec.rt1305->applied && !spec.rt1305->failed);
}

static void test_resume_eq(void)
{
	unsigned int count;

	init();
	for (unsigned int i = 0; i < RT1305_EQ_COUNT; i++)
		amp[alc274_rt1305_eq[i].reg] = before_amp[alc274_rt1305_eq[i].reg];
	count = writes;
	init();
	assert(spec.rt1305->applied && writes > count);
}

static void test_resume_controls(void)
{
	unsigned int count;

	init();
	amp[0x40] ^= 0x8000;
	amp[0x4c] ^= 0x8000;
	count = writes;
	init();
	assert(writes == count + 4);
	assert(amp[0x40] == 0xc008 && amp[0x4c] == 0x0300);
}

static void test_polling_limit(void)
{
	unsigned int count;

	coef_mutex_lock(&codec);
	busy_left = 100;
	count = polls;
	assert(rt1305_wait_idle(&codec) == -ETIMEDOUT);
	assert(polls - count == 50);
	coef_mutex_unlock(&codec);
	assert(!writes);
}

static void test_disabled(void)
{
	free(spec.rt1305);
	spec.rt1305 = NULL;
	alc274_rt1305_enable = false;
	init();
	assert(!writes && !spec.rt1305);
}

static void test_other_subsystem(void)
{
	free(spec.rt1305);
	spec.rt1305 = NULL;
	codec.core.subsystem_id = 0x1d051119;
	alc274_fixup_rt1305(&codec, NULL, HDA_FIXUP_ACT_PRE_PROBE);
	assert(!spec.rt1305 && !writes);
}

static void test_sample_rate_switch(void)
{
	struct snd_pcm_runtime runtime = { .rate = 44100 };
	struct snd_pcm_substream stream = { .runtime = &runtime };

	init();
	alc274_rt1305_playback(NULL, &codec, &stream, HDA_GEN_PCM_ACT_PREPARE);
	original();
	assert(!spec.rt1305->applied);
	runtime.rate = 48000;
	alc274_rt1305_playback(NULL, &codec, &stream, HDA_GEN_PCM_ACT_PREPARE);
	assert(spec.rt1305->applied);
}

static void assert_recovered(void)
{
	assert(!spec.rt1305->applied && !spec.rt1305->dirty);
	assert(!spec.rt1305->mute_restore_needed);
	original();
	staging_restored();
}

static void test_restore_write_retry(void)
{
	init();
	fault.reg = 0xc0;
	fault.remaining = 1;
	alc274_rt1305_restore(&codec);
	assert(spec.rt1305->failed && spec.rt1305->dirty);
	assert(spec.rt1305->mute_restore_needed);
	assert(spec.rt1305->saved_mute == before_amp[0x14]);
	assert(amp[0x14] == (before_amp[0x14] | 0xc000));
	alc274_rt1305_restore(&codec);
	assert_recovered();
}

static void test_restore_timeout_retry(void)
{
	init();
	fault.reg = 0xc0;
	fault.remaining = 1;
	fault.timeout = true;
	alc274_rt1305_restore(&codec);
	assert(spec.rt1305->failed && spec.rt1305->dirty);
	assert(spec.rt1305->mute_restore_needed);
	alc274_rt1305_restore(&codec);
	assert_recovered();
}

static void test_restore_mute_retry(void)
{
	init();
	fault.reg = 0x14;
	fault.mask = 0xc000;
	fault.value = 0xc000;
	fault.remaining = 1;
	alc274_rt1305_restore(&codec);
	assert(spec.rt1305->failed && spec.rt1305->mute_restore_needed);
	assert(spec.rt1305->saved_mute == before_amp[0x14]);
	alc274_rt1305_restore(&codec);
	assert_recovered();
}

static void test_restore_unmute_retry(void)
{
	init();
	fault.reg = 0x14;
	fault.mask = 0xc000;
	fault.remaining = 1;
	alc274_rt1305_restore(&codec);
	assert(spec.rt1305->failed && spec.rt1305->mute_restore_needed);
	assert(!spec.rt1305->applied && !spec.rt1305->dirty);
	alc274_rt1305_restore(&codec);
	assert_recovered();
}

static void test_apply_mute_failure(void)
{
	fault.reg = 0x14;
	fault.mask = 0xc000;
	fault.value = 0xc000;
	fault.remaining = 1;
	init();
	assert(spec.rt1305->failed);
	assert_recovered();
}

static void test_apply_unmute_retry(void)
{
	fault.reg = 0x14;
	fault.mask = 0xc000;
	fault.remaining = 1;
	init();
	assert(spec.rt1305->failed && spec.rt1305->applied);
	assert(spec.rt1305->mute_restore_needed);
	alc274_rt1305_restore(&codec);
	assert_recovered();
}

static void test_apply_timeout_rollback(void)
{
	fault.reg = 0xc0;
	fault.remaining = 1;
	fault.timeout = true;
	init();
	assert(spec.rt1305->failed);
	assert_recovered();
}

static void publish_failed_state(struct hda_codec *c)
{
	c->spec->rt1305->failed = true;
}

static void test_apply_locked_state(void)
{
	/* Model state published by a preceding lock holder. */
	on_lock = publish_failed_state;
	init();
	assert(!writes && !polls);
	assert(!spec.rt1305->baseline_valid);
}

static void publish_restored_state(struct hda_codec *c)
{
	c->spec->rt1305->applied = false;
	c->spec->rt1305->dirty = false;
	c->spec->rt1305->mute_restore_needed = false;
}

static void test_restore_locked_state(void)
{
	unsigned int count;

	init();
	count = polls;
	on_lock = publish_restored_state;
	alc274_rt1305_restore(&codec);
	assert(polls == count && !locks);
}

static void test_probe_counter_reset(void)
{
	fail_control_read = true;
	init();
	assert(spec.rt1305->probe_failures == 1);
	init();
	assert(spec.rt1305->applied && spec.rt1305->probe_failures == 0);
	fail_control_read = true;
	init();
	assert(spec.rt1305->probe_failures == 1 && !spec.rt1305->failed);
	init();
	assert(spec.rt1305->probe_failures == 0 && !spec.rt1305->failed);
}

int main(void)
{
	static const struct {
		const char *name;
		void (*run)(void);
	} tests[] = {
		{ "apply", test_apply },
		{ "reinit", test_reinit },
		{ "unload_restore", test_unload_restore },
		{ "wrong_amplifier", test_wrong_amplifier },
		{ "partial_write_rollback", test_partial_write_rollback },
		{ "rollback_retry", test_rollback_retry },
		{ "bridge_read_failure", test_bridge_read_failure },
		{ "volatile_controls", test_volatile_controls },
		{ "resume_eq", test_resume_eq },
		{ "resume_controls", test_resume_controls },
		{ "polling_limit", test_polling_limit },
		{ "disabled", test_disabled },
		{ "other_subsystem", test_other_subsystem },
		{ "sample_rate_switch", test_sample_rate_switch },
		{ "restore_write_retry", test_restore_write_retry },
		{ "restore_timeout_retry", test_restore_timeout_retry },
		{ "restore_mute_retry", test_restore_mute_retry },
		{ "restore_unmute_retry", test_restore_unmute_retry },
		{ "apply_mute_failure", test_apply_mute_failure },
		{ "apply_unmute_retry", test_apply_unmute_retry },
		{ "apply_timeout_rollback", test_apply_timeout_rollback },
		{ "apply_locked_state", test_apply_locked_state },
		{ "restore_locked_state", test_restore_locked_state },
		{ "probe_counter_reset", test_probe_counter_reset },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		reset();
		tests[i].run();
		assert(!locks);
		printf("PASS %s\n", tests[i].name);
	}
	free(spec.rt1305);
	return 0;
}
