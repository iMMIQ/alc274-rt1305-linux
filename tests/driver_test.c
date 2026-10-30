// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the actual driver logic with a modeled HDA bridge, not real hardware. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
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
		void (*pcm_playback_hook)(struct hda_pcm_stream *, struct hda_codec *,
					  struct snd_pcm_substream *, int);
	} gen;
	struct alc274_rt1305 *rt1305;
};
struct test_bus {
	bool shutdown;
};
struct hda_codec {
	struct {
		unsigned vendor_id, subsystem_id;
		int dev;
	} core;
	struct alc_spec *spec;
	struct test_bus *bus;
};
static u16 coefs[256], amp[256], before_amp[256], selected;
static unsigned writes, locks, polls, pending, busy_left;
static bool fail_once, fail_always, fail_control_read, volatile_controls;
static void log_message(const char *fmt, ...) { (void)fmt; }
#define codec_err(c, ...) log_message(__VA_ARGS__)
#define codec_warn(c, ...) log_message(__VA_ARGS__)
#define codec_dbg(c, ...) log_message(__VA_ARGS__)
static void *devm_kzalloc(void *dev, size_t n, int flags)
{
	(void)dev;
	(void)flags;
	return calloc(1, n);
}
static void coef_mutex_lock(struct hda_codec *c)
{
	(void)c;
	assert(!locks);
	locks++;
}
static void coef_mutex_unlock(struct hda_codec *c)
{
	(void)c;
	assert(locks == 1);
	locks--;
}
static void usleep_range(unsigned a, unsigned b)
{
	(void)a;
	(void)b;
}
static bool allowed(unsigned r)
{
	return (r >= 0xc0 && r <= 0xfb) || r == 0x14 || r == 0x40 || r == 0x46 || r == 0x4b ||
	       r == 0x4c;
}
static void complete(void)
{
	if (!pending)
		return;
	unsigned r = coefs[0x26];
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
static unsigned snd_hda_codec_read(struct hda_codec *c, unsigned n, int f, unsigned v, unsigned p)
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
static int snd_hda_codec_write(struct hda_codec *c, unsigned n, int f, unsigned v, unsigned p)
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
		if (p == 0xb024 && coefs[0x26] == 0xc0 && (fail_once || fail_always)) {
			complete();
			fail_once = false;
			return -EIO;
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
    .core = {.vendor_id = 0x10ec0274, .subsystem_id = 0x1d051115}, .spec = &spec, .bus = &bus};
static void reset(void)
{
	free(spec.rt1305);
	memset(&spec, 0, sizeof(spec));
	memset(&bus, 0, sizeof(bus));
	memset(coefs, 0, sizeof(coefs));
	memset(amp, 0, sizeof(amp));
	for (unsigned r = 0; r < 256; r++)
		amp[r] = (r * 97 + 17) & 0xffff;
	amp[0x7c] = 0x10ec;
	amp[0x7e] = 0x6251;
	amp[0x14] = 0x123;
	for (unsigned r = 0x24; r <= 0x28; r++)
		coefs[r] = r;
	selected = 0x71;
	writes = locks = polls = pending = busy_left = 0;
	fail_once = fail_always = fail_control_read = volatile_controls = false;
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
static void original(void) { assert(!memcmp(amp, before_amp, sizeof(amp))); }
static void staging_restored(void)
{
	for (unsigned r = 0x24; r <= 0x28; r++)
		assert(coefs[r] == r);
	assert(selected == 0x71);
}
int main(void)
{
	unsigned count;
	reset();
	init();
	assert(spec.rt1305->applied && !spec.rt1305->failed);
	assert(writes == 66);
	staging_restored();
	for (unsigned i = 0; i < RT1305_EQ_COUNT; i++)
		assert(amp[alc274_rt1305_eq[i].reg] == alc274_rt1305_eq[i].value);
	assert(amp[0x14] == before_amp[0x14] && amp[0x50] == before_amp[0x50] &&
	       amp[0x62] == before_amp[0x62]);
	count = writes;
	init();
	assert(writes == count + 4);
	alc274_rt1305_restore(&codec);
	original();
	staging_restored();
	puts("PASS apply, idempotent reinit, protected registers, mute and unload restoration");
	reset();
	amp[0x7c] = 0;
	init();
	init();
	init();
	assert(!writes && spec.rt1305->failed);
	staging_restored();
	puts("PASS wrong amplifier causes no configuration writes and bounded retries");
	reset();
	fail_once = true;
	init();
	assert(spec.rt1305->failed && !spec.rt1305->applied && !spec.rt1305->dirty);
	original();
	staging_restored();
	count = writes;
	init();
	assert(writes == count);
	puts("PASS partial committed write rolls back and fault prevents retry");
	reset();
	fail_always = true;
	init();
	assert(spec.rt1305->failed && spec.rt1305->dirty);
	assert(amp[0x14] & RT1305_MUTE);
	fail_always = false;
	alc274_rt1305_restore(&codec);
	/* Unload retries a recovered transport and restores the saved mute state. */
	for (unsigned i = 0; i < RT1305_EQ_COUNT; i++)
		assert(amp[alc274_rt1305_eq[i].reg] == before_amp[alc274_rt1305_eq[i].reg]);
	original();
	puts("PASS failed rollback keeps mute until unload restores EQ and saved mute");
	reset();
	fail_control_read = true;
	init();
	assert(!writes && !spec.rt1305->baseline_valid);
	staging_restored();
	puts("PASS bridge read error before mutation preserves configuration and staging");
	reset();
	volatile_controls = true;
	init();
	assert(spec.rt1305->applied && !spec.rt1305->failed);
	puts("PASS public volatile controls are excluded from strict data verification");
	reset();
	init();
	for (unsigned i = 0; i < RT1305_EQ_COUNT; i++)
		amp[alc274_rt1305_eq[i].reg] = before_amp[alc274_rt1305_eq[i].reg];
	count = writes;
	init();
	assert(spec.rt1305->applied && writes > count);
	puts("PASS codec resume reinitializes lost EQ");
	reset();
	init();
	amp[0x40] ^= 0x8000;
	amp[0x4c] ^= 0x8000;
	count = writes;
	init();
	assert(writes == count + 4);
	assert(amp[0x40] == 0xc008 && amp[0x4c] == 0x0300);
	puts("PASS resume refreshes volatile controls even when data already matches");
	reset();
	coef_mutex_lock(&codec);
	busy_left = 100;
	count = polls;
	assert(rt1305_wait_idle(&codec) == -ETIMEDOUT);
	assert(polls - count == 50);
	coef_mutex_unlock(&codec);
	assert(!writes);
	puts("PASS stuck busy terminates after 50 polls without configuration writes");
	reset();
	free(spec.rt1305);
	spec.rt1305 = NULL;
	alc274_rt1305_enable = false;
	init();
	assert(!writes && !spec.rt1305);
	puts("PASS disabled driver leaves existing output untouched");
	reset();
	free(spec.rt1305);
	spec.rt1305 = NULL;
	codec.core.subsystem_id = 0x1d051119;
	alc274_fixup_rt1305(&codec, NULL, HDA_FIXUP_ACT_PRE_PROBE);
	assert(!spec.rt1305 && !writes);
	puts("PASS another subsystem is not attached");
	reset();
	init();
	struct snd_pcm_runtime runtime = {.rate = 44100};
	struct snd_pcm_substream stream = {.runtime = &runtime};
	alc274_rt1305_playback(NULL, &codec, &stream, HDA_GEN_PCM_ACT_PREPARE);
	original();
	assert(!spec.rt1305->applied);
	runtime.rate = 48000;
	alc274_rt1305_playback(NULL, &codec, &stream, HDA_GEN_PCM_ACT_PREPARE);
	assert(spec.rt1305->applied);
	puts("PASS non-48-kHz playback restores the baseline and 48-kHz playback reapplies EQ");
	free(spec.rt1305);
	return 0;
}
