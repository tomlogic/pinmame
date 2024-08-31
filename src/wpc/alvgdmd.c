#include "driver.h"
#include "cpu/i8051/i8051.h"
#include "core.h"
#include "sndbrd.h"
#include "alvg.h"
#include "alvgdmd.h"

#ifdef VERBOSE
#define LOG(x)	logerror x
#else
#define LOG(x)
#endif

/*---------------------------*/
/*ALVIN G 128x32 DMD Handling*/
/*---------------------------*/
#define DMD32_BANK0    1

/*static vars*/
static UINT8  *dmd32RAM;

static struct {
  struct sndbrdData brdData;
  int cmd, plans_enable, disenable;
  int selsync; // SELSYNC signal on PCA020A board (Mystery Castle, Pistol Poker). Enable/Disable rasterization VCLOCK. Not present on PCA020 board (Al's Garage Band).
  int rowstart, colstart;
  int vid_page;
  int last;
  core_tDMDPWMState pwm_state;
} dmdlocals;

// Identify PCA020 (Al's Garage Band) vs PCA020A board (Pistol Poker & Mystery Castle)
#define IS_PCA020 (core_gameData->hw.gameSpecific1 == 0)

/*declarations*/
static WRITE_HANDLER(dmd32_bank_w);
static READ_HANDLER(dmd32_latch_r);
static INTERRUPT_GEN(dmd32_firq);
static WRITE_HANDLER(dmd32_data_w);
static void dmd32_init(struct sndbrdData *brdData);
static void dmd32_exit(int boardNo);
static WRITE_HANDLER(dmd32_ctrl_w);
static WRITE_HANDLER(control_w);
static READ_HANDLER(control_r);
static WRITE_HANDLER(port_w);
static READ_HANDLER(port_r);

/*Interface*/
const struct sndbrdIntf alvgdmdIntf = {
  NULL, dmd32_init, dmd32_exit, NULL,NULL,
  dmd32_data_w, NULL, dmd32_ctrl_w, NULL, SNDBRD_NOTSOUND
};

static WRITE_HANDLER(control_w)
{
	UINT16 tmpoffset = offset;
	tmpoffset += 0x8000;	//Adjust the offset so we can work with the "actual address" as it really is (just makes it easier, it's not necessary to do this)
	tmpoffset &= 0xf000;	//Remove bits 0-11 (in other words, treat 0x8fff the same as 0x8000!)
	switch (tmpoffset)
	{
		//Read Main CPU DMD Command [PORTIN signal]
		case 0x8000:
			LOG(("WARNING! Writing to address 0x8000 - DMD Latch, data=%x!\n",data));
			dmdlocals.selsync = 1;
			break;

		//Send Data to the Main CPU [PORTOUT signal]
		case 0x9000:
			sndbrd_ctrl_cb(dmdlocals.brdData.boardNo, data);
			dmdlocals.selsync = 1;
			break;

		//ROM Bankswitching [CODEPAGE signal]
		case 0xa000:
			dmd32_bank_w(0,data);
			dmdlocals.selsync = 1;
			break;

		//ROWSTART Line [ROWSTART signal]
		case 0xb000:
			LOG(("rowstart = %x\n",data));
			dmdlocals.selsync = 1;
			if (IS_PCA020)
				dmdlocals.rowstart = data & 0x7F; // PCA020: GPLAN0 and GPLAN1 can be freely defined
			else
				dmdlocals.rowstart = (data & 0x1F) | 0x20; // PCA020A: GPLAN0 is hardwired to 1 and GPLAN1 is hardwired to 0
			break;

		//COLSTART Line [COLSTART signal]
		case 0xc000:
			LOG(("colstart = %x\n",data));
			dmdlocals.selsync = 1;
			dmdlocals.colstart = data;
			break;

		//NC
		case 0xd000:
		case 0xe000:
			LOG(("writing to not connected address: %x data=%x\n",offset,data));
			dmdlocals.selsync = 1;
			break;

		//SELSYNC Line [SELSYNC signal on PCA020A, not wired on PCA020]
		//SelSync suspend rasterization (stops rasterization clock) if written to, only the address bus is used, so writing to any other address in 0x8000 - 0xE000 will reset it
		//it doesn't seem to be used (didn't find a write to it on Pistol Poker & Mystery Castle)
		// FIXME remove for hardware without it
		case 0xf000:
			LOG(("setsync=%x\n", data));
			dmdlocals.selsync = 0;
			printf("%8.5f selsync off\n", timer_get_time());
			break;

		default:
			LOG(("WARNING! Reading invalid control address %x\n", offset));
			break;
	}
}

static READ_HANDLER(control_r)
{
	UINT16 tmpoffset = offset;
	tmpoffset += 0x8000;	//Adjust the offset so we can work with the "actual address" as it really is (just makes it easier, it's not necessary to do this)
	tmpoffset &= 0xf000;	//Remove bits 0-11 (in other words, treat 0x8fff the same as 0x8000!)
	switch (tmpoffset)
	{
		//Read Main CPU DMD Command
		case 0x8000: {
			int data = dmd32_latch_r(0);
			dmdlocals.selsync = 1;
			return data;
		}

		case 0x9000:
		case 0xa000:
		case 0xb000:
		case 0xc000:
		case 0xd000:
		case 0xe000:
			// undefined behavior as data is not set
			dmdlocals.selsync = 1;
			break;

		// see control_w (data is unused so it has the same effect)
		case 0xf000:
			dmdlocals.selsync = 0;
			break;

		default:
			LOG(("WARNING! Reading invalid control address %x\n", offset));
	}
	return 0;
}


static READ_HANDLER(port_r)
{
	//Port 1 is read in the wait for interrupt loop.. Not sure why this is done.
	if(offset == 1)
		return dmdlocals.vid_page;
	else
		LOG(("port read @ %x\n",offset));
	return 0;
}

static WRITE_HANDLER(port_w)
{
	switch(offset) {
		case 0:
			LOG(("writing to port %x data = %x\n",offset,data));
			break;
		case 1:
			dmdlocals.vid_page = data & 0x0f;
			if(dmdlocals.last != data) {
				dmdlocals.last = data;
				LOG(("port write @ %x, data=%x\n",offset,data));
			}
			break;
		case 2:
			LOG(("writing to port %x data = %x\n",offset,data));
			break;
		/*PORT 3:
			P3.0 = PLANS ENABLE
			P3.1 = LED
			P3.2 = INT0 (Not used as output)
			P3.3 = INT1 (Not used as output)
			P3.4 = TO   (Not used as output)
			P3.5 = DISENABLE (Display Enable)
			P3.6 = /WR  (Used Internally only?)
			P3.7 = /RD  (Used Internally only?) */
		case 3:
			dmdlocals.plans_enable = data & 0x01;
			alvg_UpdateSoundLEDS(1,(data&0x02)>>1);
			dmdlocals.disenable = ((data & 0x20) >> 5);
			break;
		default:
			LOG(("writing to port %x data = %x\n",offset,data));
	}
}

static WRITE_HANDLER(dmd32_bank_w)
{
	LOG(("setting dmd32_bank to: %x\n",data));
	cpu_setbank(DMD32_BANK0, dmdlocals.brdData.romRegion + ((data & 0x1f)*0x8000));
}


//The MC51 cpu's can all access up to 64K ROM & 64K RAM in the SAME ADDRESS SPACE
//It uses separate commands to distinguish which area it's reading/writing to (RAM or ROM).
//So to handle this, the cpu core automatically adjusts all external memory access to the follwing setup..
//0-FFFF is for ROM, 10000 - 1FFFF is for RAM
static MEMORY_READ_START(alvgdmd_readmem)
	{ 0x000000, 0x007fff, MRA_ROM },
	{ 0x008000, 0x00ffff, MRA_BANK1 },
	{ 0x010000, 0x017fff, MRA_RAM },
	{ 0x018000, 0x01ffff, control_r },
MEMORY_END

static MEMORY_WRITE_START(alvgdmd_writemem)
	{ 0x000000, 0x00ffff, MWA_ROM },		//This area can never really be accessed by the cpu core but we'll put this here anyway
	{ 0x010000, 0x017fff, MWA_RAM, &dmd32RAM },
	{ 0x018000, 0x01ffff, control_w },
MEMORY_END

static PORT_READ_START( alvgdmd_readport )
	{ 0x00,0xff, port_r },
PORT_END

static PORT_WRITE_START( alvgdmd_writeport )
	{ 0x00,0xff, port_w },
PORT_END

// Al's Garage Band Goes On A World Tour
// CPU Clock, directly drives VCLOCK, then divided by 640 (ROWCLOCK = 512 for DOTCLOCK [U26/U21] + 128 for COLLATCH [U23]), then by 256 for INT1 [U22])
MACHINE_DRIVER_START(alvgdmd1)
  MDRV_CPU_ADD(I8051, 12000000)	/*12 Mhz*/
  MDRV_CPU_MEMORY(alvgdmd_readmem, alvgdmd_writemem)
  MDRV_CPU_PORTS(alvgdmd_readport, alvgdmd_writeport)
  MDRV_CPU_PERIODIC_INT(dmd32_firq, 12000000./(640.*256.))
  MDRV_INTERLEAVE(50)
MACHINE_DRIVER_END

// Pistol Poker & Mystery Castle
// CPU Clock, divided by 4 (VCLOCK), then by 640 (ROWCLOCK = 512 for DOTCLOCK [U26/U21] + 128 for COLLATCH [U23]), then by 256 for INT1 [U22])
// This leads to fairly slow animation, but seems ot match the real pin (f.e. see https://www.youtube.com/watch?v=KY_spGpQC-Q)
MACHINE_DRIVER_START(alvgdmd2)
  MDRV_CPU_ADD(I8051, 12000000)	/*12 Mhz*/
  MDRV_CPU_MEMORY(alvgdmd_readmem, alvgdmd_writemem)
  MDRV_CPU_PORTS(alvgdmd_readport, alvgdmd_writeport)
  //MDRV_CPU_PERIODIC_INT(dmd32_firq, 12000000./(4.*640.*256.))
  MDRV_CPU_PERIODIC_INT(dmd32_firq, 12000000./(640.*256.)) // HACK: U36 divide main clock by 4 but this leads to slow animation, so skipped here
  MDRV_INTERLEAVE(50)
MACHINE_DRIVER_END
MACHINE_DRIVER_START(alvgdmd3)
  MDRV_CPU_ADD(I8051, 12000000)	/*12 Mhz*/
  MDRV_CPU_MEMORY(alvgdmd_readmem, alvgdmd_writemem)
  MDRV_CPU_PORTS(alvgdmd_readport, alvgdmd_writeport)
  //MDRV_CPU_PERIODIC_INT(dmd32_firq, 12000000./(4.*640.*256.))
  MDRV_CPU_PERIODIC_INT(dmd32_firq, 12000000./(640.*256.)) // HACK: U36 divide main clock by 4 but this leads to slow animation, so skipped here
  MDRV_INTERLEAVE(50)
MACHINE_DRIVER_END


//Use only for testing the 8031 core emulation
#ifdef MAME_DEBUG
MACHINE_DRIVER_START(test8031)
  MDRV_CPU_ADD(I8051, 12000000)	/*12 Mhz*/
  MDRV_CPU_MEMORY(alvgdmd_readmem, alvgdmd_writemem)
  MDRV_CPU_PORTS(alvgdmd_readport, alvgdmd_writeport)
  MDRV_CPU_PERIODIC_INT(dmd32_firq, 12000000. / (640. * 256.))
MACHINE_DRIVER_END
#endif

static void dmd32_init(struct sndbrdData *brdData) {
  memset(&dmdlocals, 0, sizeof(dmdlocals));
  dmdlocals.brdData = *brdData;
  dmd32_bank_w(0,0);			//Set DMD Bank to 0
  dmdlocals.selsync = 1;		//Start Sync @ 1
  // Init PWM shading
  core_dmd_pwm_init(&dmdlocals.pwm_state, 128, 32, CORE_DMD_PWM_FILTER_ALVG);
}

static void dmd32_exit(int boardNo) {
  core_dmd_pwm_exit(&dmdlocals.pwm_state);
}

// Main CPU sends command to DMD
static WRITE_HANDLER(dmd32_data_w) {
	dmdlocals.cmd = data;
}

//Send data from Main CPU to latch - Set's the INT0 Line
static WRITE_HANDLER(dmd32_ctrl_w) {
	LOG(("INT0: Sending DMD Strobe - current command = %x\n",dmdlocals.cmd));
	cpu_set_irq_line(dmdlocals.brdData.cpuNo, I8051_INT0_LINE, HOLD_LINE);
}

//Read data from latch sent by Main CPU - Clear's the INT0 Line
static READ_HANDLER(dmd32_latch_r) {
  LOG(("INT0: reading latch: data = %x\n",dmdlocals.cmd));
  cpu_set_irq_line(dmdlocals.brdData.cpuNo, I8051_INT0_LINE, CLEAR_LINE);
  return dmdlocals.cmd;
}

// PCA020A rasterize 128 rows at once (so 4 frames), with a main VCLOCK of  3MHz (Pistol Poker and Mystery Castle)
static INTERRUPT_GEN(dmd32_firq) {
  if (!IS_PCA020 && (dmdlocals.selsync == 0)) { // VCLOCK is disabled so FIRQ may not happen [not present on Al's Garage Band Hardware]
	LOG(("Skipping INT1\n"));
	return;
  }
  //static double prev; printf("DMD VBlank %8.5fms => %8.5fHz for 4 frames so %8.5fHz\n", timer_get_time() - prev, 1. / (timer_get_time() - prev), 4. / (timer_get_time() - prev)); prev = timer_get_time();
  //Pulse the INT1 Line (wired to /ROWDATA so pulsed when vertical blanking after a sequence of PWM frames)
  LOG(("INT1 Pulse\n"));
  cpu_set_irq_line(dmdlocals.brdData.cpuNo, I8051_INT1_LINE, PULSE_LINE);
  assert((dmdlocals.colstart & 0x07) == 0); // Lowest 3 bits are actually loaded to the shift register, so it is possible to perform a dot shift, but we don't support it
  const UINT8* RAM = (UINT8*)dmd32RAM + (dmdlocals.vid_page << 11) + ((dmdlocals.colstart >> 3) & 0x0F);
  const unsigned int plan_mask = dmdlocals.plans_enable ? 0x7F : 0x1F; // either render 4 different frames or 4 times the same
  if (dmdlocals.pwm_state.legacyColorization) {
    // For backward compatibility regarding colorization, previous implementation:
    // - for Al's Garage submited first frame with a weight of 1, and second (same as first if plans_enable = 0) with a weight of 2 => 4 shades (with unbalanced luminance between frames)
    // - for PP & MC submited a 16 shade frame made up of the 4 frames (if plans_enable = 0, four time the same frame) => 16 shades (with balanced luminance between frames)
	assert(IS_PCA020); // PCA020A doe snot need this and is not supported
	core_dmd_submit_frame(&dmdlocals.pwm_state, RAM + (((dmdlocals.rowstart + 0x00) & plan_mask) << 4));
	core_dmd_submit_frame(&dmdlocals.pwm_state, RAM + (((dmdlocals.rowstart + 0x20) & plan_mask) << 4));
	core_dmd_submit_frame(&dmdlocals.pwm_state, RAM + (((dmdlocals.rowstart + 0x20) & plan_mask) << 4));
  } else {
    core_dmd_submit_frame(&dmdlocals.pwm_state, RAM + (((dmdlocals.rowstart + 0x00) & plan_mask) << 4));
	core_dmd_submit_frame(&dmdlocals.pwm_state, RAM + (((dmdlocals.rowstart + 0x20) & plan_mask) << 4));
	core_dmd_submit_frame(&dmdlocals.pwm_state, RAM + (((dmdlocals.rowstart + 0x40) & plan_mask) << 4));
	core_dmd_submit_frame(&dmdlocals.pwm_state, RAM + (((dmdlocals.rowstart + 0x60) & plan_mask) << 4));
  }
}

PINMAME_VIDEO_UPDATE(alvgdmd_update) {
  core_dmd_update_pwm(&dmdlocals.pwm_state);
  video_update_core_dmd(bitmap, cliprect, layout);
  return 0;
}
