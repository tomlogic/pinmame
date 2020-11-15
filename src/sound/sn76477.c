/*****************************************************************************
  SN76477 pins and assigned interface variables/functions

								SN76477_envelope_w()
							   /					\
					   [ 1] ENV SEL 1			ENV SEL 2 [28]
					   [ 2] GND 				  MIXER C [27] \
 SN76477_noise_clock_w [ 3] NOISE EXT OSC		  MIXER A [26]	> SN76477_mixer_w()
		  noise_res    [ 4] RES NOISE OSC		  MIXER B [25] /
		 filter_res    [ 5] NOISE FILTER RES	  O/S RES [24] oneshot_res
		 filter_cap    [ 6] NOISE FILTER CAP	  O/S CAP [23] oneshot_cap
		  decay_res    [ 7] DECAY RES			  VCO SEL [22] SN76477_vco_w()
   attack_decay_cap    [ 8] A/D CAP 			  SLF CAP [21] slf_cap
 SN76477_enable_w()    [ 9] ENABLE				  SLF RES [20] slf_res
		 attack_res    [10] ATTACK RES				PITCH [19] pitch_voltage
	  amplitude_res    [11] AMP 				  VCO RES [18] vco_res
	   feedback_res    [12] FEEDBACK			  VCO CAP [17] vco_cap
					   [13] OUTPUT			 VCO EXT CONT [16] vco_voltage
					   [14] Vcc 			  +5V REG OUT [15]

	All resistor values in Ohms.
	All capacitor values in Farads.
	Use RES_K, RES_M and CAP_U, CAP_N, CAP_P macros to convert
	magnitudes, eg. 220k = RES_K(220), 47nF = CAP_N(47)

 *****************************************************************************/

#include "driver.h"

#define VERBOSE 0

#if (VERBOSE >= 0)
#define LOG(n,x) if( VERBOSE >= (n) ) logerror x
#else
#define LOG(n,x)
#endif

#ifdef MAME_DEBUG
#define CHECK_CHIP_NUM						\
	if( chip >= intf->num ) 				\
	{										\
		LOG(0,("SN76477 #%d: fatal, only %d chips defined in interface!\n", chip, intf->num)); \
		return; 							\
	}

#define CHECK_CHIP_NUM_AND_RANGE(BITS,FUNC) \
	CHECK_CHIP_NUM; 						\
	if( data != (data & BITS) ) 			\
		LOG(0,("SN76477 #%d: warning %s called with data = $%02X!\n", chip, #FUNC, data)); \
	data &= BITS;
#else
#define CHECK_CHIP_NUM
#define CHECK_CHIP_NUM_AND_RANGE(BITS,FUNC)
#endif


#define VMIN	0x0000
#define VMAX	0x7fff

struct SN76477 {
	int channel;			/* returned by stream_init() */

	int samplerate; 		/* from Machine->sample_rate */
	INT32 vol;				/* current volume (attack/decay) */
	INT32 vol_count;		/* volume adjustment counter */
	INT32 vol_rate;			/* volume adjustment rate - dervied from attack/decay */
	INT32 vol_step;			/* volume adjustment step */

	double slf_count;		/* SLF emulation */
	double slf_freq;		/* frequency - derived */
	double slf_level;		/* triangular wave level */
	UINT32 slf_dir;			/* triangular wave direction */
	UINT32 slf_out;			/* rectangular output signal state */

	double vco_count;		/* VCO emulation */
	double vco_freq;		/* frequency - derived */
	double vco_step;		/* modulated frequency - derived */
	UINT32 vco_out;			/* rectangular output signal state */

	UINT32 noise_count;		/* NOISE emulation */
	UINT32 noise_clock;		/* external clock signal */
	UINT32 noise_freq; 		/* filter frequency - derived */
	UINT32 noise_poly; 		/* polynome */
	UINT32 noise_out;		/* rectangular output signal state */

	void *envelope_timer;	/* ENVELOPE timer */
	UINT32 envelope_state; 	/* attack / decay toggle */

	double attack_time; 	/* ATTACK time (time until vol reaches 100%) */
	double decay_time;		/* DECAY time (time until vol reaches 0%) */
	double oneshot_time;	/* ONE-SHOT time */
	void *oneshot_timer;	/* ONE-SHOT timer */

	UINT32 envelope;		/* pin  1, pin 28 */
	double noise_res;		/* pin  4 */
	double filter_res;		/* pin  5 */
	double filter_cap;		/* pin  6 */
	double decay_res;		/* pin  7 */
	double attack_decay_cap;/* pin  8 */
	UINT32 enable; 			/* pin  9 */
	double attack_res;		/* pin 10 */
	double amplitude_res;	/* pin 11 */
	double feedback_res;	/* pin 12 */
	double vco_voltage; 	/* pin 16 */
	double vco_cap; 		/* pin 17 */
	double vco_res; 		/* pin 18 */
	double pitch_voltage;	/* pin 19 */
	double slf_res; 		/* pin 20 */
	double slf_cap; 		/* pin 21 */
	UINT32 vco_select; 		/* pin 22 */
	double oneshot_cap; 	/* pin 23 */
	double oneshot_res; 	/* pin 24 */
	UINT32 mixer;			/* pin 25,26,27 */

	INT16 vol_lookup[VMAX+1-VMIN];	/* volume lookup table */
};

static struct SN76477interface *intf;
static struct SN76477 *sn76477[MAX_SN76477];

static void attack_decay(int param)
{
	struct SN76477 *sn = sn76477[param];
	sn->envelope_state ^= 1;
	if( sn->envelope_state )
	{
		/* start ATTACK */
		sn->vol_rate = ( sn->attack_time > 0 ) ? (INT32)(VMAX / sn->attack_time) : VMAX;
		sn->vol_step = +1;
		LOG(2,("SN76477 #%d: ATTACK rate %d/%d = %d/sec\n", param, sn->vol_rate, sn->samplerate, sn->vol_rate/sn->samplerate));
    }
	else
	{
		/* start DECAY */
		sn->vol = VMAX; /* just in case... */
		sn->vol_rate = ( sn->decay_time > 0 ) ? (INT32)(VMAX / sn->decay_time) : VMAX;
		sn->vol_step = -1;
		LOG(2,("SN76477 #%d: DECAY rate %d/%d = %d/sec\n", param, sn->vol_rate, sn->samplerate, sn->vol_rate/sn->samplerate));
    }
}

static void vco_envelope_cb(int param)
{
	attack_decay(param);
}

static void oneshot_envelope_cb(int param)
{
	attack_decay(param);
}

#if VERBOSE
static const char *mixer_mode[8] = {
	"VCO",
	"SLF",
	"Noise",
	"VCO/Noise",
	"SLF/Noise",
	"SLF/VCO/Noise",
	"SLF/VCO",
	"Inhibit"
};
#endif

/*****************************************************************************
 * set MIXER select inputs
 *****************************************************************************/
void SN76477_mixer_w(int chip, UINT32 data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM_AND_RANGE(7,SN76477_mixer_w);

	if( data == sn->mixer )
		return;
	stream_update(sn->channel, 0);
	sn->mixer = data;
#if VERBOSE
	LOG(1,("SN76477 #%d: MIXER mode %d [%s]\n", chip, sn->mixer, mixer_mode[sn->mixer]));
#endif
}

void SN76477_mixer_a_w(int chip, UINT32 data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM_AND_RANGE(1,SN76477_mixer_a_w);

	data = data ? 1 : 0;
    if( data == (sn->mixer & 1) )
		return;
	stream_update(sn->channel, 0);
	sn->mixer = (sn->mixer & ~1) | data;
#if VERBOSE
	LOG(1,("SN76477 #%d: MIXER mode %d [%s]\n", chip, sn->mixer, mixer_mode[sn->mixer]));
#endif
}

void SN76477_mixer_b_w(int chip, UINT32 data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM_AND_RANGE(1,SN76477_mixer_b_w);

	data = data ? 2 : 0;
    if( data == (sn->mixer & 2) )
		return;
	stream_update(sn->channel, 0);
	sn->mixer = (sn->mixer & ~2) | data;
#if VERBOSE
	LOG(1,("SN76477 #%d: MIXER mode %d [%s]\n", chip, sn->mixer, mixer_mode[sn->mixer]));
#endif
}

void SN76477_mixer_c_w(int chip, UINT32 data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM_AND_RANGE(1,SN76477_mixer_c_w);

	data = data ? 4 : 0;
    if( data == (sn->mixer & 4) )
		return;
	stream_update(sn->channel, 0);
	sn->mixer = (sn->mixer & ~4) | data;
#if VERBOSE
	LOG(1,("SN76477 #%d: MIXER mode %d [%s]\n", chip, sn->mixer, mixer_mode[sn->mixer]));
#endif
}

#if VERBOSE
static const char *envelope_mode[4] = {
	"VCO",
	"One-Shot",
	"Mixer only",
	"VCO with alternating Polarity"
};
#endif

/*****************************************************************************
 * set ENVELOPE select inputs
 *****************************************************************************/
void SN76477_envelope_w(int chip, UINT32 data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM_AND_RANGE(3,SN76477_envelope_w);

	if( data == sn->envelope )
		return;
	stream_update(sn->channel, 0);
	sn->envelope = data;
#if VERBOSE
	LOG(1,("SN76477 #%d: ENVELOPE mode %d [%s]\n", chip, sn->envelope, envelope_mode[sn->envelope]));
#endif
}

void SN76477_envelope_1_w(int chip, UINT32 data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM_AND_RANGE(1,SN76477_envelope_1_w);

	if( data == (sn->envelope & 1) )
		return;
	stream_update(sn->channel, 0);
	sn->envelope = (sn->envelope & ~1) | data;
#if VERBOSE
	LOG(1,("SN76477 #%d: ENVELOPE mode %d [%s]\n", chip, sn->envelope, envelope_mode[sn->envelope]));
#endif
}

void SN76477_envelope_2_w(int chip, UINT32 data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM_AND_RANGE(1,SN76477_envelope_2_w);

	data <<= 1;

	if( data == (sn->envelope & 2) )
		return;
	stream_update(sn->channel, 0);
	sn->envelope = (sn->envelope & ~2) | data;
#if VERBOSE
	LOG(1,("SN76477 #%d: ENVELOPE mode %d [%s]\n", chip, sn->envelope, envelope_mode[sn->envelope]));
#endif
}

/*****************************************************************************
 * set VCO external/SLF input
 *****************************************************************************/
void SN76477_vco_w(int chip, UINT32 data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM_AND_RANGE(1,SN76477_vco_w);

	if( data == sn->vco_select )
		return;
	stream_update(sn->channel, 0);
	sn->vco_select = data;
	LOG(1,("SN76477 #%d: VCO select %d [%s]\n", chip, sn->vco_select, sn->vco_select ? "Internal (SLF)" : "External (Pin 16)"));
}

/*****************************************************************************
 * set VCO enable input
 *****************************************************************************/
void SN76477_enable_w(int chip, UINT32 data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM_AND_RANGE(1,SN76477_enable_w);

	if( data == sn->enable )
		return;
	stream_update(sn->channel, 0);
	sn->enable = data;
	sn->envelope_state = data;

	timer_adjust(sn->envelope_timer, TIME_NEVER, chip, 0);
	timer_adjust(sn->oneshot_timer, TIME_NEVER, chip, 0);

	if( sn->enable == 0 )
	{
		switch( sn->envelope )
		{
		case 0: /* VCO */
			if( sn->vco_res > 0 && sn->vco_cap > 0 )
				timer_adjust(sn->envelope_timer, TIME_IN_HZ(0.64/(sn->vco_res * sn->vco_cap)), chip, TIME_IN_HZ(0.64/(sn->vco_res * sn->vco_cap)));
			else
				oneshot_envelope_cb(chip);
			break;
		case 1: /* One-Shot */
			sn->vol = 0;
			oneshot_envelope_cb(chip);
			if (sn->oneshot_time > 0)
				timer_adjust(sn->oneshot_timer, sn->oneshot_time, chip, 0);
			break;
		case 2: /* MIXER only */
			sn->vol = VMAX;
			break;
		default:  /* VCO with alternating polariy */
			/* huh? */
			if( sn->vco_res > 0 && sn->vco_cap > 0 )
				timer_adjust(sn->envelope_timer, TIME_IN_HZ(0.64/(sn->vco_res * sn->vco_cap)/2), chip, TIME_IN_HZ(0.64/(sn->vco_res * sn->vco_cap)/2));
			else
				oneshot_envelope_cb(chip);
			break;
		}
	}
	else
	{
		switch( sn->envelope )
		{
		case 0: /* VCO */
			if( sn->vco_res > 0 && sn->vco_cap > 0 )
				timer_adjust(sn->envelope_timer, TIME_IN_HZ(0.64/(sn->vco_res * sn->vco_cap)), chip, TIME_IN_HZ(0.64/(sn->vco_res * sn->vco_cap)));
			else
				oneshot_envelope_cb(chip);
			break;
		case 1: /* One-Shot */
			oneshot_envelope_cb(chip);
			break;
		case 2: /* MIXER only */
			sn->vol = VMIN;
			break;
		default:  /* VCO with alternating polariy */
			/* huh? */
			if( sn->vco_res > 0 && sn->vco_cap > 0 )
				timer_adjust(sn->envelope_timer, TIME_IN_HZ(0.64/(sn->vco_res * sn->vco_cap)/2), chip, TIME_IN_HZ(0.64/(sn->vco_res * sn->vco_cap)/2));
			else
				oneshot_envelope_cb(chip);
			break;
		}
#ifdef PINMAME /* We need to reset the SLF value to 5 Volts if disabled! */
		sn->slf_level = 5.0;
#endif /* PINMAME */
	}
	LOG(1,("SN76477 #%d: ENABLE line %d [%s]\n", chip, sn->enable, sn->enable ? "Inhibited" : "Enabled" ));
}

/*****************************************************************************
 * set NOISE external signal (pin 3)
 *****************************************************************************/
void SN76477_noise_clock_w(int chip, UINT32 data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM_AND_RANGE(1,SN76477_noise_clock_w);

	if( data == sn->noise_clock )
		return;
	stream_update(sn->channel, 0);
	sn->noise_clock = data;
	/* on the rising edge shift the polynome */
	if( sn->noise_clock )
		sn->noise_poly = ((sn->noise_poly << 7) + (sn->noise_poly >> 10) + 0x18000) & 0x1ffff;
}

/*****************************************************************************
 * set NOISE resistor (pin 4)
 *****************************************************************************/
void SN76477_set_noise_res(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	stream_update(sn->channel, 0);
	sn->noise_res = data;
}

/*****************************************************************************
 * set NOISE FILTER resistor (pin 5)
 *****************************************************************************/
void SN76477_set_filter_res(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->filter_res )
		return;
	stream_update(sn->channel, 0);
	sn->filter_res = data;
	if( sn->filter_res > 0 && sn->filter_cap > 0 )
	{
		sn->noise_freq = (UINT32)(1.28 / (sn->filter_res * sn->filter_cap));
		LOG(1,("SN76477 #%d: NOISE FILTER freqency %d\n", chip, sn->noise_freq));
	}
	else
		sn->noise_freq = sn->samplerate;
}

/*****************************************************************************
 * set NOISE FILTER capacitor (pin 6)
 *****************************************************************************/
void SN76477_set_filter_cap(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->filter_cap )
		return;
	stream_update(sn->channel, 0);
	sn->filter_cap = data;
	if( sn->filter_res > 0 && sn->filter_cap > 0 )
	{
		sn->noise_freq = (UINT32)(1.28 / (sn->filter_res * sn->filter_cap));
		LOG(1,("SN76477 #%d: NOISE FILTER freqency %d\n", chip, sn->noise_freq));
	}
	else
		sn->noise_freq = sn->samplerate;
}

/*****************************************************************************
 * set DECAY resistor (pin 7)
 *****************************************************************************/
void SN76477_set_decay_res(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->decay_res )
		return;
	stream_update(sn->channel, 0);
	sn->decay_res = data;
	sn->decay_time = sn->decay_res * sn->attack_decay_cap;
	LOG(1,("SN76477 #%d: DECAY time is %fs\n", chip, sn->decay_time));
}

/*****************************************************************************
 * set ATTACK/DECAY capacitor (pin 8)
 *****************************************************************************/
void SN76477_set_attack_decay_cap(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->attack_decay_cap )
		return;
	stream_update(sn->channel, 0);
	sn->attack_decay_cap = data;
	sn->decay_time = sn->decay_res * sn->attack_decay_cap;
	sn->attack_time = sn->attack_res * sn->attack_decay_cap;
	LOG(1,("SN76477 #%d: ATTACK time is %fs\n", chip, sn->attack_time));
	LOG(1,("SN76477 #%d: DECAY time is %fs\n", chip, sn->decay_time));
}

/*****************************************************************************
 * set ATTACK resistor (pin 10)
 *****************************************************************************/
void SN76477_set_attack_res(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->attack_res )
		return;
	stream_update(sn->channel, 0);
	sn->attack_res = data;
	sn->attack_time = sn->attack_res * sn->attack_decay_cap;
	LOG(1,("SN76477 #%d: ATTACK time is %fs\n", chip, sn->attack_time));
}

/*****************************************************************************
 * set AMP resistor (pin 11)
 *****************************************************************************/
void SN76477_set_amplitude_res(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];
	INT32 i;

	CHECK_CHIP_NUM;

	if( data == sn->amplitude_res )
		return;
	stream_update(sn->channel, 0);
	sn->amplitude_res = data;
	if( sn->amplitude_res > 0 )
	{
#if VERBOSE
		INT32 clip = 0;
#endif
		for( i = 0; i < VMAX+1; i++ )
		{
			INT32 vol = (INT32)((sn->feedback_res / sn->amplitude_res) * 32767 * i / (VMAX+1));
#if VERBOSE
			if( vol > 32767 && !clip )
				clip = i;
			LOG(3,("%d\n", vol));
#endif
			if( vol > 32767 ) vol = 32767;
			sn->vol_lookup[i] = vol * intf->mixing_level[chip] / 100;
		}
#if VERBOSE
		LOG(1,("SN76477 #%d: volume range from -%d to +%d (clip at %d%%)\n", chip, sn->vol_lookup[VMAX-VMIN], sn->vol_lookup[VMAX-VMIN], clip * 100 / 32767));
#endif
	}
	else
	{
		memset(sn->vol_lookup, 0, sizeof(sn->vol_lookup));
	}
}

/*****************************************************************************
 * set FEEDBACK resistor (pin 12)
 *****************************************************************************/
void SN76477_set_feedback_res(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];
	INT32 i;

	CHECK_CHIP_NUM;

	if( data == sn->feedback_res )
		return;
	stream_update(sn->channel, 0);
	sn->feedback_res = data;
	if( sn->amplitude_res > 0 )
	{
#if VERBOSE
		INT32 clip = 0;
#endif
		for( i = 0; i < VMAX+1; i++ )
		{
			INT32 vol = (INT32)((sn->feedback_res / sn->amplitude_res) * 32767 * i / (VMAX+1));
#if VERBOSE
			if( vol > 32767 && !clip ) clip = i;
#endif
			if( vol > 32767 ) vol = 32767;
			sn->vol_lookup[i] = vol * intf->mixing_level[chip] / 100;
		}
#if VERBOSE
		LOG(1,("SN76477 #%d: volume range from -%d to +%d (clip at %d%%)\n", chip, sn->vol_lookup[VMAX-VMIN], sn->vol_lookup[VMAX-VMIN], clip * 100 / 32767));
#endif
	}
	else
	{
		memset(sn->vol_lookup, 0, sizeof(sn->vol_lookup));
	}
}

/*****************************************************************************
 * set PITCH voltage (pin 19)
 * TODO: fill with live...
 *****************************************************************************/
void SN76477_set_pitch_voltage(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->pitch_voltage )
		return;
	stream_update(sn->channel, 0);
	sn->pitch_voltage = data;
	LOG(1,("SN76477 #%d: VCO pitch voltage %f (%d%% duty cycle)\n", chip, sn->pitch_voltage, 0));
}

/*****************************************************************************
 * set VCO resistor (pin 18)
 *****************************************************************************/
void SN76477_set_vco_res(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->vco_res )
		return;
	stream_update(sn->channel, 0);
	sn->vco_res = data;
	if( sn->vco_res > 0 && sn->vco_cap > 0 )
	{
		sn->vco_freq = 0.64 / (sn->vco_res * sn->vco_cap);
		LOG(1,("SN76477 #%d: VCO freqency %f\n", chip, sn->vco_freq));
	}
	else
		sn->vco_freq = 0;
}

/*****************************************************************************
 * set VCO capacitor (pin 17)
 *****************************************************************************/
void SN76477_set_vco_cap(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->vco_cap )
		return;
	stream_update(sn->channel, 0);
	sn->vco_cap = data;
	if( sn->vco_res > 0 && sn->vco_cap > 0 )
	{
		sn->vco_freq = 0.64 / (sn->vco_res * sn->vco_cap);
		LOG(1,("SN76477 #%d: VCO freqency %f\n", chip, sn->vco_freq));
	}
	else
		sn->vco_freq = 0;
}

/*****************************************************************************
 * set VCO voltage (pin 16)
 *****************************************************************************/
void SN76477_set_vco_voltage(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->vco_voltage )
		return;
	stream_update(sn->channel, 0);
	sn->vco_voltage = data;
	LOG(1,("SN76477 #%d: VCO ext. voltage %f (%f * %f = %f Hz)\n", chip,
		sn->vco_voltage,
		sn->vco_freq,
		10.0 * (5.0 - sn->vco_voltage) / 5.0,
		sn->vco_freq * 10.0 * (5.0 - sn->vco_voltage) / 5.0));
}

/*****************************************************************************
 * set SLF resistor (pin 20)
 *****************************************************************************/
void SN76477_set_slf_res(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->slf_res )
		return;
	stream_update(sn->channel, 0);
	sn->slf_res = data;
	if( sn->slf_res > 0 && sn->slf_cap > 0 )
	{
		sn->slf_freq = 0.64 / (sn->slf_res * sn->slf_cap);
		LOG(1,("SN76477 #%d: SLF freqency %f\n", chip, sn->slf_freq));
	}
	else
		sn->slf_freq = 0;
}

/*****************************************************************************
 * set SLF capacitor (pin 21)
 *****************************************************************************/
void SN76477_set_slf_cap(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->slf_cap )
		return;
	stream_update(sn->channel, 0);
	sn->slf_cap = data;
	if( sn->slf_res > 0 && sn->slf_cap > 0 )
	{
		sn->slf_freq = 0.64 / (sn->slf_res * sn->slf_cap);
		LOG(1,("SN76477 #%d: SLF freqency %f\n", chip, sn->slf_freq));
	}
	else
		sn->slf_freq = 0;
}

/*****************************************************************************
 * set ONESHOT resistor (pin 24)
 *****************************************************************************/
void SN76477_set_oneshot_res(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;
	if( data == sn->oneshot_res )
		return;
	sn->oneshot_res = data;
	sn->oneshot_time = 0.8 * sn->oneshot_res * sn->oneshot_cap;
	LOG(1,("SN76477 #%d: ONE-SHOT time %fs\n", chip, sn->oneshot_time));
}

/*****************************************************************************
 * set ONESHOT capacitor (pin 23)
 *****************************************************************************/
void SN76477_set_oneshot_cap(int chip, double data)
{
	struct SN76477 *sn = sn76477[chip];

	CHECK_CHIP_NUM;

	if( data == sn->oneshot_cap )
        return;
    sn->oneshot_cap = data;
	sn->oneshot_time = 0.8 * sn->oneshot_res * sn->oneshot_cap;
	LOG(1,("SN76477 #%d: ONE-SHOT time %fs\n", chip, sn->oneshot_time));
}

#define UPDATE_SLF															\
	/*************************************                                  \
     * SLF super low frequency oscillator                                   \
     * frequency = 0.64 / (r_slf * c_slf)                                   \
     *************************************/ 								\
	sn->slf_count -= sn->slf_freq;											\
	while( sn->slf_count <= 0 ) 											\
	{																		\
		sn->slf_count += sn->samplerate;									\
		sn->slf_out ^= 1;													\
	}

#define UPDATE_VCO															\
	/************************************                                   \
     * VCO voltage controlled oscilator                                     \
     * min. freq = 0.64 / (r_vco * c_vco)                                   \
     * freq. range is approx. 10:1                                          \
     ************************************/									\
	if( sn->vco_select )													\
	{																		\
		/* VCO is controlled by SLF */										\
		if( sn->slf_dir == 0 )												\
		{																	\
			sn->slf_level -= sn->slf_freq * 2 * 5.0 / sn->samplerate;		\
			if( sn->slf_level <= 0.0 )										\
			{																\
                sn->slf_level = 0.0;                                        \
				sn->slf_dir = 1;											\
			}																\
		}																	\
		else																\
		if( sn->slf_dir == 1 )												\
		{																	\
			sn->slf_level += sn->slf_freq * 2 * 5.0 / sn->samplerate;		\
			if( sn->slf_level >= 5.0 )										\
			{																\
				sn->slf_level = 5.0;										\
				sn->slf_dir = 0;											\
            }                                                               \
        }                                                                   \
		sn->vco_step = sn->vco_freq * sn->slf_level;						\
	}																		\
	else																	\
	{																		\
		/* VCO is controlled by external voltage */ 						\
		sn->vco_step = sn->vco_freq * sn->vco_voltage;						\
	}																		\
	sn->vco_count -= sn->vco_step;											\
	while( sn->vco_count <= 0 ) 											\
	{																		\
		sn->vco_count += sn->samplerate;									\
		sn->vco_out ^= 1;													\
	}

#define UPDATE_NOISE														\
	/*************************************                                  \
     * NOISE pseudo rand number generator                                   \
     *************************************/ 								\
	if( sn->noise_res > 0 ) 												\
		sn->noise_poly = ( (sn->noise_poly << 7) +							\
						   (sn->noise_poly >> 10) + 						\
						   0x18000 ) & 0x1ffff; 							\
																			\
	/* low pass filter: sample every noise_freq pseudo random value */		\
	if (sn->noise_count > sn->noise_freq)									\
	{																		\
		sn->noise_count -= sn->noise_freq;									\
	}																		\
	else																	\
	{																		\
		sn->noise_count = sn->samplerate;									\
		sn->noise_out = sn->noise_poly & 1; 								\
	}

#define UPDATE_VOLUME														\
	/*************************************                                  \
     * VOLUME adjust for attack/decay                                       \
     *************************************/ 								\
	sn->vol_count -= sn->vol_rate;											\
	if( sn->vol_count <= 0 )												\
	{																		\
		INT32 n = - sn->vol_count / sn->samplerate + 1; /* number of steps */ \
		sn->vol_count += n * sn->samplerate;								\
		sn->vol += n * sn->vol_step;										\
		if( sn->vol < VMIN ) sn->vol = VMIN;								\
		if( sn->vol > VMAX ) sn->vol = VMAX;								\
		LOG(3,("SN76477 #%d: vol = $%04X\n", chip, sn->vol));      \
	}


/*****************************************************************************
 * mixer select 0 0 0 : VCO
 *****************************************************************************/
static void SN76477_update_0(int chip, INT16 *buffer, int length)
{
	struct SN76477 *sn = sn76477[chip];
	while( length-- )
	{
		UPDATE_VCO;
		UPDATE_VOLUME;
		*buffer++ = sn->vco_out ? sn->vol_lookup[sn->vol-VMIN] : -sn->vol_lookup[sn->vol-VMIN];
	}
}

/*****************************************************************************
 * mixer select 0 0 1 : SLF
 *****************************************************************************/
static void SN76477_update_1(int chip, INT16 *buffer, int length)
{
	struct SN76477 *sn = sn76477[chip];
	while( length-- )
	{
		UPDATE_SLF;
		UPDATE_VOLUME;
		*buffer++ = sn->slf_out ? sn->vol_lookup[sn->vol-VMIN] : -sn->vol_lookup[sn->vol-VMIN];
	}
}

/*****************************************************************************
 * mixer select 0 1 0 : NOISE
 *****************************************************************************/
static void SN76477_update_2(int chip, INT16 *buffer, int length)
{
	struct SN76477 *sn = sn76477[chip];
	while( length-- )
	{
		UPDATE_NOISE;
		UPDATE_VOLUME;
		*buffer++ = sn->noise_out ? sn->vol_lookup[sn->vol-VMIN] : -sn->vol_lookup[sn->vol-VMIN];
	}
}

/*****************************************************************************
 * mixer select 0 1 1 : VCO and NOISE
 *****************************************************************************/
static void SN76477_update_3(int chip, INT16 *buffer, int length)
{
	struct SN76477 *sn = sn76477[chip];
	while( length-- )
	{
		UPDATE_VCO;
		UPDATE_NOISE;
		UPDATE_VOLUME;
		*buffer++ = (sn->vco_out & sn->noise_out) ? sn->vol_lookup[sn->vol-VMIN] : -sn->vol_lookup[sn->vol-VMIN];
	}
}

/*****************************************************************************
 * mixer select 1 0 0 : SLF and NOISE
 *****************************************************************************/
static void SN76477_update_4(int chip, INT16 *buffer, int length)
{
	struct SN76477 *sn = sn76477[chip];
	while( length-- )
	{
		UPDATE_SLF;
		UPDATE_NOISE;
		UPDATE_VOLUME;
		*buffer++ = (sn->slf_out & sn->noise_out) ? sn->vol_lookup[sn->vol-VMIN] : -sn->vol_lookup[sn->vol-VMIN];
	}
}

/*****************************************************************************
 * mixer select 1 0 1 : VCO, SLF and NOISE
 *****************************************************************************/
static void SN76477_update_5(int chip, INT16 *buffer, int length)
{
	struct SN76477 *sn = sn76477[chip];
	while( length-- )
	{
		UPDATE_SLF;
		UPDATE_VCO;
		UPDATE_NOISE;
		UPDATE_VOLUME;
		*buffer++ = (sn->vco_out & sn->slf_out & sn->noise_out) ? sn->vol_lookup[sn->vol-VMIN] : -sn->vol_lookup[sn->vol-VMIN];
	}
}

/*****************************************************************************
 * mixer select 1 1 0 : VCO and SLF
 *****************************************************************************/
static void SN76477_update_6(int chip, INT16 *buffer, int length)
{
	struct SN76477 *sn = sn76477[chip];
	while( length-- )
	{
		UPDATE_SLF;
		UPDATE_VCO;
		UPDATE_VOLUME;
		*buffer++ = (sn->vco_out & sn->slf_out) ? sn->vol_lookup[sn->vol-VMIN] : -sn->vol_lookup[sn->vol-VMIN];
	}
}

/*****************************************************************************
 * mixer select 1 1 1 : Inhibit
 *****************************************************************************/
static void SN76477_update_7(int chip, INT16 *buffer, int length)
{
	while( length-- )
		*buffer++ = 0;
}

static void SN76477_sound_update(int param, INT16 *buffer, int length)
{
	struct SN76477 *sn = sn76477[param];
	if( sn->enable )
	{
		SN76477_update_7(param,buffer,length);
	}
	else
	{
		switch( sn->mixer )
		{
		case 0:
			SN76477_update_0(param,buffer,length);
			break;
		case 1:
			SN76477_update_1(param,buffer,length);
			break;
		case 2:
			SN76477_update_2(param,buffer,length);
			break;
		case 3:
			SN76477_update_3(param,buffer,length);
			break;
		case 4:
			SN76477_update_4(param,buffer,length);
			break;
		case 5:
			SN76477_update_5(param,buffer,length);
			break;
		case 6:
			SN76477_update_6(param,buffer,length);
			break;
		default:
			SN76477_update_7(param,buffer,length);
			break;
		}
	}
}

int SN76477_sh_start(const struct MachineSound *msound)
{
	int i;
	intf = msound->sound_interface;

	for( i = 0; i < intf->num; i++ )
	{
		char name[16];

		sn76477[i] = auto_malloc(sizeof(struct SN76477));
		if( !sn76477[i] )
		{
			LOG(0,("%s failed to malloc struct SN76477\n", name));
			return 1;
		}
		memset(sn76477[i], 0, sizeof(struct SN76477));

		sprintf(name, "SN76477 #%d", i);
		sn76477[i]->channel = stream_init(name, intf->mixing_level[i], Machine->sample_rate, i, SN76477_sound_update);
		if( sn76477[i]->channel == -1 )
		{
			LOG(0,("%s stream_init failed\n", name));
			return 1;
		}
		sn76477[i]->samplerate = Machine->sample_rate != 0. ? (int)(Machine->sample_rate+0.5) : 1;
		
		sn76477[i]->envelope_timer = timer_alloc(vco_envelope_cb);
		sn76477[i]->oneshot_timer = timer_alloc(oneshot_envelope_cb);
		
		/* set up interface (default) values */
		SN76477_set_noise_res(i, intf->noise_res[i]);
		SN76477_set_filter_res(i, intf->filter_res[i]);
		SN76477_set_filter_cap(i, intf->filter_cap[i]);
		SN76477_set_decay_res(i, intf->decay_res[i]);
		SN76477_set_attack_decay_cap(i, intf->attack_decay_cap[i]);
		SN76477_set_attack_res(i, intf->attack_res[i]);
		SN76477_set_amplitude_res(i, intf->amplitude_res[i]);
		SN76477_set_feedback_res(i, intf->feedback_res[i]);
		SN76477_set_oneshot_res(i, intf->oneshot_res[i]);
		SN76477_set_oneshot_cap(i, intf->oneshot_cap[i]);
		SN76477_set_pitch_voltage(i, intf->pitch_voltage[i]);
		SN76477_set_slf_res(i, intf->slf_res[i]);
		SN76477_set_slf_cap(i, intf->slf_cap[i]);
		SN76477_set_vco_res(i, intf->vco_res[i]);
		SN76477_set_vco_cap(i, intf->vco_cap[i]);
		SN76477_set_vco_voltage(i, intf->vco_voltage[i]);
		SN76477_mixer_w(i, 0x07);		/* turn off mixing */
		SN76477_envelope_w(i, 0x03);	/* envelope inputs open */
		SN76477_enable_w(i, 0x01);		/* enable input open */
	}
	return 0;
}

void SN76477_sh_stop(void)
{
}

void SN76477_sh_update(void)
{
	int i;
	for( i = 0; i < intf->num; i++ )
		stream_update(i,0);
}

