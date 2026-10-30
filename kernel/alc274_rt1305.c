// SPDX-License-Identifier: GPL-2.0-only
/* Included by patch_realtek.c to share its coefficient mutex and PM scope. */
#include "alc274_rt1305_eq.h"

#define RT1305_EQ_COUNT ARRAY_SIZE(alc274_rt1305_eq)
#define RT1305_ENDPOINT 0x10
#define RT1305_BUSY 0x4000
#define RT1305_READ 0xb034
#define RT1305_WRITE 0xb024
#define RT1305_MUTE 0xc000

struct alc274_rt1305 {
	u16 baseline[RT1305_EQ_COUNT];
	bool baseline_valid;
	bool applied;
	bool dirty;
	bool mute_restore_needed;
	u16 saved_mute;
	bool check_needed;
	bool failed;
	unsigned int probe_failures;
	void (*previous_hook)(struct hda_pcm_stream *hinfo, struct hda_codec *codec,
			      struct snd_pcm_substream *substream, int action);
};

struct rt1305_bridge_session {
	u16 staging[5];
	u16 index;
	bool index_valid;
	bool saved;
};

static bool alc274_rt1305_enable = true;
module_param_named(rt1305_eq, alc274_rt1305_enable, bool, 0444);
MODULE_PARM_DESC(rt1305_eq, "Enable GM7ZG8M RT1305-family speaker EQ");

/* The caller holds alc_spec::coef_mutex and a codec power reference. */
static int rt1305_coef_write(struct hda_codec *codec, u16 index, u16 value)
{
	int err;

	err = snd_hda_codec_write(codec, 0x20, 0, AC_VERB_SET_COEF_INDEX, index);
	if (err < 0)
		return err;
	return snd_hda_codec_write(codec, 0x20, 0, AC_VERB_SET_PROC_COEF, value);
}

static int rt1305_coef_read(struct hda_codec *codec, u16 index, u16 *value)
{
	unsigned int response;
	int err;

	err = snd_hda_codec_write(codec, 0x20, 0, AC_VERB_SET_COEF_INDEX, index);
	if (err < 0)
		return err;
	response = snd_hda_codec_read(codec, 0x20, 0, AC_VERB_GET_PROC_COEF, 0);
	if (response == ~0U)
		return -EIO;
	*value = response;
	return 0;
}

static int rt1305_wait_idle(struct hda_codec *codec)
{
	unsigned int i;
	u16 status;
	int err;

	for (i = 0; i < 50; i++) {
		err = rt1305_coef_read(codec, 0x29, &status);
		if (err < 0)
			return err;
		if (!(status & RT1305_BUSY))
			return 0;
		usleep_range(100, 200);
	}
	return -ETIMEDOUT;
}

static int rt1305_amp_read(struct hda_codec *codec, u8 reg, u16 *value)
{
	int err;

	err = rt1305_wait_idle(codec);
	if (err < 0)
		return err;
	err = rt1305_coef_write(codec, 0x24, RT1305_ENDPOINT);
	if (err < 0)
		return err;
	err = rt1305_coef_write(codec, 0x26, reg);
	if (err < 0)
		return err;
	err = rt1305_coef_write(codec, 0x29, RT1305_READ);
	if (err < 0)
		return err;
	err = rt1305_wait_idle(codec);
	if (err < 0)
		return err;
	return rt1305_coef_read(codec, 0x2b, value);
}

static int rt1305_amp_write(struct hda_codec *codec, u8 reg, u16 value)
{
	int err;

	err = rt1305_wait_idle(codec);
	if (err < 0)
		return err;
	err = rt1305_coef_write(codec, 0x24, RT1305_ENDPOINT);
	if (err < 0)
		return err;
	err = rt1305_coef_write(codec, 0x26, reg);
	if (err < 0)
		return err;
	err = rt1305_coef_write(codec, 0x27, 0);
	if (err < 0)
		return err;
	err = rt1305_coef_write(codec, 0x28, value);
	if (err < 0)
		return err;
	err = rt1305_coef_write(codec, 0x29, RT1305_WRITE);
	if (err < 0)
		return err;
	return rt1305_wait_idle(codec);
}

static int rt1305_session_begin(struct hda_codec *codec,
				struct rt1305_bridge_session *session)
{
	unsigned int index, i;
	int err;

	index = snd_hda_codec_read(codec, 0x20, 0, AC_VERB_GET_COEF_INDEX, 0);
	if (index == ~0U)
		return -EIO;
	session->index = index;
	session->index_valid = true;
	for (i = 0; i < ARRAY_SIZE(session->staging); i++) {
		err = rt1305_coef_read(codec, 0x24 + i, &session->staging[i]);
		if (err < 0)
			return err;
	}
	session->saved = true;
	return rt1305_wait_idle(codec);
}

static int rt1305_session_end(struct hda_codec *codec,
			      const struct rt1305_bridge_session *session)
{
	unsigned int i;
	int err, first = 0;

	if (!session->saved)
		goto restore_index;
	err = rt1305_wait_idle(codec);
	if (err < 0)
		first = err;
	else
		for (i = 0; i < ARRAY_SIZE(session->staging); i++) {
			err = rt1305_coef_write(codec, 0x24 + i, session->staging[i]);
			if (err < 0 && !first)
				first = err;
		}
	/* Never restore 0x29: writing an old control word issues a new command. */
restore_index:
	if (!session->index_valid)
		return first;
	err = snd_hda_codec_write(codec, 0x20, 0, AC_VERB_SET_COEF_INDEX, session->index);
	return first ? first : err;
}

static int rt1305_identify(struct hda_codec *codec)
{
	u16 vendor, device;
	int err;

	err = rt1305_amp_read(codec, 0x7c, &vendor);
	if (err < 0)
		return err;
	if (vendor != 0x10ec)
		return -ENODEV;
	err = rt1305_amp_read(codec, 0x7e, &device);
	if (err < 0)
		return err;
	return device == 0x6251 ? 0 : -ENODEV;
}

static int rt1305_eq_snapshot(struct hda_codec *codec, u16 *values)
{
	unsigned int i;
	int err;

	for (i = 0; i < RT1305_EQ_COUNT; i++) {
		err = rt1305_amp_read(codec, alc274_rt1305_eq[i].reg, &values[i]);
		if (err < 0)
			return err;
	}
	return 0;
}

static int rt1305_eq_verify(struct hda_codec *codec, const u16 *baseline)
{
	unsigned int i;
	u16 value, expected;
	int err;

	for (i = 0; i < RT1305_EQ_COUNT; i++) {
		/* These two controls are volatile in the upstream RT1305 regmap. */
		if (alc274_rt1305_eq[i].reg == 0x40 ||
		    alc274_rt1305_eq[i].reg == 0x4c)
			continue;
		expected = baseline ? baseline[i] : alc274_rt1305_eq[i].value;
		err = rt1305_amp_read(codec, alc274_rt1305_eq[i].reg, &value);
		if (err < 0)
			return err;
		if (value != expected)
			return -EUCLEAN;
	}
	return 0;
}

static int rt1305_eq_write(struct hda_codec *codec, const u16 *baseline)
{
	unsigned int i;
	int err;

	for (i = 0; i < RT1305_EQ_COUNT; i++) {
		err = rt1305_amp_write(codec, alc274_rt1305_eq[i].reg,
				       baseline ? baseline[i] : alc274_rt1305_eq[i].value);
		if (err < 0)
			return err;
	}
	return rt1305_eq_verify(codec, baseline);
}

static int rt1305_eq_refresh_controls(struct hda_codec *codec)
{
	unsigned int i;
	int err;

	for (i = 0; i < RT1305_EQ_COUNT; i++) {
		if (alc274_rt1305_eq[i].reg != 0x40 &&
		    alc274_rt1305_eq[i].reg != 0x4c)
			continue;
		err = rt1305_amp_write(codec, alc274_rt1305_eq[i].reg,
				       alc274_rt1305_eq[i].value);
		if (err < 0)
			return err;
	}
	return 0;
}

/* Called at codec init or PREPARE, before a new stream starts delivering data. */
static void alc274_rt1305_apply(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	struct alc274_rt1305 *amp = spec->rt1305;
	struct rt1305_bridge_session session = {};
	u16 mute = 0;
	bool muted = false;
	bool controls_only;
	int err, restore_err = 0, end_err;

	if (!amp || amp->failed || !amp->check_needed)
		return;
	coef_mutex_lock(codec);
	err = rt1305_session_begin(codec, &session);
	if (err < 0)
		goto out;
	err = rt1305_identify(codec);
	if (err < 0)
		goto out;
	if (!amp->baseline_valid) {
		err = rt1305_eq_snapshot(codec, amp->baseline);
		if (err < 0)
			goto out;
		amp->baseline_valid = true;
	}
	err = rt1305_eq_verify(codec, NULL);
	if (err && err != -EUCLEAN)
		goto out;
	/* A data match does not prove the two volatile controls survived PM. */
	controls_only = !err;
	err = rt1305_amp_read(codec, 0x14, &mute);
	if (err < 0)
		goto out;
	/* A command may reach the AMP even if the completion read fails. */
	amp->saved_mute = mute;
	amp->mute_restore_needed = true;
	muted = true;
	err = rt1305_amp_write(codec, 0x14, mute | RT1305_MUTE);
	if (err < 0) {
		amp->failed = true;
		restore_err = rt1305_amp_write(codec, 0x14, mute);
		if (!restore_err) {
			muted = false;
			amp->mute_restore_needed = false;
		}
		goto out;
	}
	amp->dirty = true;
	err = controls_only ? rt1305_eq_refresh_controls(codec) :
			      rt1305_eq_write(codec, NULL);
	if (err < 0) {
		amp->failed = true;
		restore_err = rt1305_eq_write(codec, amp->baseline);
		amp->applied = false;
		amp->dirty = !!restore_err;
	} else {
		amp->applied = true;
		amp->check_needed = false;
	}
	if (!restore_err) {
		restore_err = rt1305_amp_write(codec, 0x14, mute);
		if (!restore_err) {
			muted = false;
			amp->mute_restore_needed = false;
		}
	}
out:
	end_err = rt1305_session_end(codec, &session);
	if (restore_err || end_err)
		amp->failed = true;
	if (err < 0 && !amp->failed && ++amp->probe_failures >= 3)
		amp->failed = true;
	coef_mutex_unlock(codec);
	if (restore_err || muted)
		codec_err(codec, "RT1305 EQ failed (%d), recovery failed (%d); amplifier may remain muted\n",
			  err, restore_err);
	else if (err < 0 || end_err)
		codec_warn(codec, "RT1305 EQ unavailable: %d (bridge restore: %d)\n",
			   err, end_err);
	else
		codec_dbg(codec, "RT1305 speaker EQ ready\n");
}

static void alc274_rt1305_playback(struct hda_pcm_stream *hinfo,
				   struct hda_codec *codec,
				 struct snd_pcm_substream *substream, int action)
{
	struct alc_spec *spec = codec->spec;
	struct alc274_rt1305 *amp = spec->rt1305;

	if (amp->previous_hook)
		amp->previous_hook(hinfo, codec, substream, action);
	if (action != HDA_GEN_PCM_ACT_PREPARE)
		return;
	if (substream->runtime->rate == 48000)
		alc274_rt1305_apply(codec);
	else
		alc274_rt1305_restore(codec);
}

static void alc274_rt1305_restore(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	struct alc274_rt1305 *amp = spec->rt1305;
	struct rt1305_bridge_session session = {};
	u16 mute, restore_mute;
	int err, end_err;

	if (!amp || !amp->baseline_valid ||
	    (!amp->applied && !amp->dirty && !amp->mute_restore_needed) ||
	    codec->bus->shutdown)
		return;
	coef_mutex_lock(codec);
	err = rt1305_session_begin(codec, &session);
	if (!err)
		err = rt1305_identify(codec);
	if (!err)
		err = rt1305_amp_read(codec, 0x14, &mute);
	if (!err) {
		restore_mute = amp->mute_restore_needed ? amp->saved_mute : mute;
		err = rt1305_amp_write(codec, 0x14, mute | RT1305_MUTE);
	}
	if (!err) {
		err = rt1305_eq_write(codec, amp->baseline);
		if (!err)
			err = rt1305_amp_write(codec, 0x14, restore_mute);
	}
	end_err = rt1305_session_end(codec, &session);
	if (!err && !end_err) {
		amp->applied = false;
		amp->dirty = false;
		amp->mute_restore_needed = false;
		amp->check_needed = true;
	} else {
		amp->failed = true;
	}
	coef_mutex_unlock(codec);
	if (err || end_err)
		codec_err(codec, "RT1305 baseline restore failed: %d/%d\n", err, end_err);
}

static void alc274_fixup_rt1305(struct hda_codec *codec,
				const struct hda_fixup *fix, int action)
{
	struct alc_spec *spec = codec->spec;
	struct alc274_rt1305 *amp;

	if (!alc274_rt1305_enable || codec->core.vendor_id != 0x10ec0274 ||
	    codec->core.subsystem_id != 0x1d051115)
		return;
	switch (action) {
	case HDA_FIXUP_ACT_PRE_PROBE:
		spec->rt1305 = devm_kzalloc(&codec->core.dev, sizeof(*amp), GFP_KERNEL);
		break;
	case HDA_FIXUP_ACT_PROBE:
		amp = spec->rt1305;
		if (amp) {
			amp->previous_hook = spec->gen.pcm_playback_hook;
			spec->gen.pcm_playback_hook = alc274_rt1305_playback;
		}
		break;
	case HDA_FIXUP_ACT_INIT:
		amp = spec->rt1305;
		if (amp) {
			amp->check_needed = true;
			alc274_rt1305_apply(codec);
		}
		break;
	}
}
