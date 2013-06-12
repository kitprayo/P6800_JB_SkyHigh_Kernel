/*
 * Copyright (C) 2010-2012 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/**
 * @file mali_platform_dvfs.c
 * Platform specific Mali driver dvfs functions
 */

#include "mali_kernel_common.h"
#include "mali_osk.h"
#include "mali_platform.h"

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>

#include <asm/io.h>

#ifdef CONFIG_CPU_FREQ
#include <mach/asv.h>
#include <mach/regs-pmu.h>
#define EXYNOS4_ASV_ENABLED
#endif

#include "mali_device_pause_resume.h"
#include <linux/workqueue.h>

#define MALI_DVFS_WATING 10 // msec

static int bMaliDvfsRun=0;

#if MALI_GPU_BOTTOM_LOCK
static _mali_osk_atomic_t bottomlock_status;
#endif

typedef struct mali_dvfs_tableTag{
	unsigned int clock;
	unsigned int freq;
	unsigned int vol;
}mali_dvfs_table;

typedef struct mali_dvfs_statusTag{
	unsigned int currentStep;
	mali_dvfs_table * pCurrentDvfs;

}mali_dvfs_currentstatus;

typedef struct mali_dvfs_thresholdTag{
	unsigned int downthreshold;
	unsigned int upthreshold;
}mali_dvfs_threshold_table;

typedef struct mali_dvfs_staycount{
	unsigned int staycount;
}mali_dvfs_staycount_table;

mali_dvfs_staycount_table mali_dvfs_staycount[MALI_DVFS_STEPS]={
	/*step 0*/{0}, 
	/*step 1*/{1},
	/*step 2*/{1},
	/*step 3*/{1} }; 

/*dvfs threshold*/
mali_dvfs_threshold_table mali_dvfs_threshold[MALI_DVFS_STEPS]={
	/*step 0*/{((int)((255*0)/100))   ,((int)((255*60)/100))}, 
	/*step 1*/{((int)((255*80)/100))  ,((int)((255*75)/100))},
	/*step 2*/{((int)((255*80)/100))  ,((int)((255*90)/100))},
	/*step 3*/{((int)((255*80)/100))  ,((int)((255*100)/100))} };

/*dvfs status*/
mali_dvfs_currentstatus maliDvfsStatus;
int mali_dvfs_control=0;

/*dvfs table*/
mali_dvfs_table mali_dvfs[MALI_DVFS_STEPS]={
	/*step 0*/{100,	 1000000,  900000},
	/*step 1*/{160,  1000000,  900000},
	/*step 2*/{267,  1000000,  950000},
	/*step 3*/{400,  1000000, 1050000} };

#ifdef EXYNOS4_ASV_ENABLED

#define ASV_8_LEVEL	8
#define ASV_5_LEVEL	5

static unsigned int asv_3d_volt_5_table[ASV_5_LEVEL][MALI_DVFS_STEPS] = {
	/* L4 (100MHz), L3(160MHz), L2(267MHz), L1(400MHz) */
	{ 900000, 900000, 950000, 1050000},  /* S */
	{ 900000, 900000, 950000, 1050000},  /* A */
	{ 900000, 900000, 950000, 1050000},  /* B */
	{ 900000, 900000, 950000, 1050000},  /* C */
	{ 900000, 900000, 950000, 1050000},  /* D */
};

static unsigned int asv_3d_volt_8_table[ASV_8_LEVEL][MALI_DVFS_STEPS] = {
	/* L4 (100MHz), L3(160MHz), L2(267MHz), L1(400MHz) */
	{ 900000, 900000, 950000, 1050000},  /* SS */
	{ 900000, 900000, 950000, 1050000},  /* A1 */
	{ 900000, 900000, 950000, 1050000},  /* A2 */
	{ 900000, 900000, 950000, 1050000},  /* B1 */
	{ 900000, 900000, 950000, 1050000},  /* B2 */
	{ 900000, 900000, 950000, 1050000},  /* C1 */
	{ 900000, 900000, 950000, 1050000},  /* C2 */
	{ 900000, 900000, 950000, 1050000},  /* D1 */ 
};
#endif

static u32 mali_dvfs_utilization = 255;

static void mali_dvfs_work_handler(struct work_struct *w);

static struct workqueue_struct *mali_dvfs_wq = 0;
extern mali_io_address clk_register_map;

#if MALI_GPU_BOTTOM_LOCK
extern _mali_osk_lock_t *mali_dvfs_lock;
#endif

static DECLARE_WORK(mali_dvfs_work, mali_dvfs_work_handler);

/* lock/unlock CPU freq by Mali */
#include <linux/types.h>
#include <mach/cpufreq.h>

atomic_t mali_cpufreq_lock;

int cpufreq_lock_by_mali(unsigned int freq)
{
#ifdef CONFIG_EXYNOS4_CPUFREQ
	/* #if defined(CONFIG_CPU_FREQ) && defined(CONFIG_ARCH_EXYNOS4) */
	unsigned int level;

	if (atomic_read(&mali_cpufreq_lock) == 0) {
		if (exynos_cpufreq_get_level(freq * 1000, &level)) {
			printk(KERN_ERR
				"Mali: failed to get cpufreq level for %dMHz",
				freq);
			return -EINVAL;
		}

		if (exynos_cpufreq_lock(DVFS_LOCK_ID_G3D, level)) {
			printk(KERN_ERR
				"Mali: failed to cpufreq lock for L%d", level);
			return -EINVAL;
		}

		atomic_set(&mali_cpufreq_lock, 1);
		printk(KERN_DEBUG "Mali: cpufreq locked on <%d>%dMHz\n", level,
                 	                  freq);
	}
#endif
   return 0;
}

void cpufreq_unlock_by_mali(void)
{
#ifdef CONFIG_EXYNOS4_CPUFREQ
/* #if defined(CONFIG_CPU_FREQ) && defined(CONFIG_ARCH_EXYNOS4) */
	if (atomic_read(&mali_cpufreq_lock) == 1) {
		exynos_cpufreq_lock_free(DVFS_LOCK_ID_G3D);
		atomic_set(&mali_cpufreq_lock, 0);
		printk(KERN_DEBUG "Mali: cpufreq locked off\n");
	}
#endif
}

static unsigned int get_mali_dvfs_status(void)
{
	return maliDvfsStatus.currentStep;
}

#if MALI_GPU_BOTTOM_LOCK
#if MALI_PMM_RUNTIME_JOB_CONTROL_ON
int get_mali_dvfs_control_status(void)
{
	return mali_dvfs_control;
}

mali_bool set_mali_dvfs_current_step(unsigned int step)
{
	_mali_osk_lock_wait(mali_dvfs_lock, _MALI_OSK_LOCKMODE_RW);
	maliDvfsStatus.currentStep = step;
	_mali_osk_lock_signal(mali_dvfs_lock, _MALI_OSK_LOCKMODE_RW);
	return MALI_TRUE;
}
#endif
#endif

static mali_bool set_mali_dvfs_status(u32 step,mali_bool boostup)
{
	u32 validatedStep=step;
	int err;
#ifdef CONFIG_REGULATOR
	if (mali_regulator_get_usecount()==0) {
		MALI_DEBUG_PRINT(1, ("regulator use_count is 0 \n"));
		return MALI_FALSE;
	}
#endif

	if (boostup) {
#ifdef CONFIG_REGULATOR
		/*change the voltage*/
		mali_regulator_set_voltage(mali_dvfs[step].vol, mali_dvfs[step].vol);
#endif
		/*change the clock*/
		mali_clk_set_rate(mali_dvfs[step].clock, mali_dvfs[step].freq);
	} else {
		/*change the clock*/
		mali_clk_set_rate(mali_dvfs[step].clock, mali_dvfs[step].freq);
#ifdef CONFIG_REGULATOR
		/*change the voltage*/
		mali_regulator_set_voltage(mali_dvfs[step].vol, mali_dvfs[step].vol);
#endif
	}

	maliDvfsStatus.currentStep = validatedStep;
	/*for future use*/
	maliDvfsStatus.pCurrentDvfs = &mali_dvfs[validatedStep];

	/* lock/unlock CPU freq by Mali */
	if (mali_dvfs[step].clock >= 266)
		err = cpufreq_lock_by_mali(500);
	else
		cpufreq_unlock_by_mali();

	return MALI_TRUE;
}

static void mali_platform_wating(u32 msec)
{
	/*sample wating
	change this in the future with proper check routine.
	*/
	unsigned int read_val;
	while(1) {
		read_val = _mali_osk_mem_ioread32(clk_register_map, 0x00);
		if ((read_val & 0x8000)==0x0000) break;

		_mali_osk_time_ubusydelay(100); // 1000 -> 100 : 20101218
	}
	/* _mali_osk_time_ubusydelay(msec*1000);*/
}

static mali_bool change_mali_dvfs_status(u32 step, mali_bool boostup )
{

	MALI_DEBUG_PRINT(1, ("> change_mali_dvfs_status: %d, %d \n",step, boostup));

	if (!set_mali_dvfs_status(step, boostup)) {
		MALI_DEBUG_PRINT(1, ("error on set_mali_dvfs_status: %d, %d \n",step, boostup));
		return MALI_FALSE;
	}

	/*wait until clock and voltage is stablized*/
	mali_platform_wating(MALI_DVFS_WATING); /*msec*/

	return MALI_TRUE;
}

static unsigned int decideNextStatus(unsigned int utilization)
{
	unsigned int level=0; // 0:stay, 1:up

	if (!mali_dvfs_control) {
#if MALI_GPU_BOTTOM_LOCK
		if (_mali_osk_atomic_read(&bottomlock_status) > 0)
			level = 1;	/* or bigger */
		else 
#endif
		switch(maliDvfsStatus.currentStep)
		{
			case 0:
				if( utilization > mali_dvfs_threshold[maliDvfsStatus.currentStep].upthreshold)
					level=1;
				else
					level = maliDvfsStatus.currentStep;
				break;
			case 1:
				if( utilization > mali_dvfs_threshold[maliDvfsStatus.currentStep].upthreshold)
					level=2;
				else if( utilization <
					 (mali_dvfs_threshold[maliDvfsStatus.currentStep].downthreshold*mali_dvfs[maliDvfsStatus.currentStep-1].clock)/
					 mali_dvfs[maliDvfsStatus.currentStep].clock)
					level=0;
				else
					level = maliDvfsStatus.currentStep;
				break;
			case 2:
				if( utilization > mali_dvfs_threshold[maliDvfsStatus.currentStep].upthreshold)
					level=3;
				else if( utilization <
					 (mali_dvfs_threshold[maliDvfsStatus.currentStep].downthreshold*mali_dvfs[maliDvfsStatus.currentStep-1].clock)/
					 mali_dvfs[maliDvfsStatus.currentStep].clock)
					level=1;
				else
					level = maliDvfsStatus.currentStep;
				break;
			case 3:
				if( utilization <
				         (mali_dvfs_threshold[maliDvfsStatus.currentStep].downthreshold*mali_dvfs[maliDvfsStatus.currentStep-1].clock)/
					 mali_dvfs[maliDvfsStatus.currentStep].clock)
					level=2;
				else
					level = maliDvfsStatus.currentStep;
				break;
		}
	}
	else
	{
		if((mali_dvfs_control == 1)||(( mali_dvfs_control > 4) && (mali_dvfs_control < mali_dvfs[0].clock+1)))
		{
			level=0;
		}
		else if((mali_dvfs_control == 2)||(( mali_dvfs_control > mali_dvfs[0].clock) && (mali_dvfs_control < mali_dvfs[1].clock+1)))
		{
			level=1;
		}
		else if((mali_dvfs_control == 3)||(( mali_dvfs_control > mali_dvfs[1].clock) && (mali_dvfs_control < mali_dvfs[2].clock+1)))
		{
			level=2;
		}
		else
		{
			level=3;
		}
	}

	return level;
}

#ifdef EXYNOS4_ASV_ENABLED
static mali_bool mali_dvfs_table_update(void)
{
	unsigned int exynos_result_of_asv_group;
	unsigned int target_asv;
	unsigned int i;
	exynos_result_of_asv_group = exynos_result_of_asv & 0xf;
	target_asv = exynos_result_of_asv >> 28;
	MALI_PRINT(("exynos_result_of_asv_group = 0x%x, target_asv = 0x%x\n", exynos_result_of_asv_group, target_asv));

	if (target_asv == 0x8) { //SUPPORT_1400MHZ
		for (i = 0; i < MALI_DVFS_STEPS; i++) {
			mali_dvfs[i].vol = asv_3d_volt_5_table[exynos_result_of_asv_group][i];
			MALI_PRINT(("mali_dvfs[%d].vol = %d\n", i, mali_dvfs[i].vol));
		}
	} else if (target_asv == 0x4){ //SUPPORT_1200MHZ
		for (i = 0; i < MALI_DVFS_STEPS; i++) {
			mali_dvfs[i].vol = asv_3d_volt_8_table[exynos_result_of_asv_group][i];
			MALI_PRINT(("mali_dvfs[%d].vol = %d\n", i, mali_dvfs[i].vol));
		}
	}

	return MALI_TRUE;

}
#endif

static mali_bool mali_dvfs_status(u32 utilization)
{
	unsigned int nextStatus = 0;
	unsigned int curStatus = 0;
	mali_bool boostup = MALI_FALSE;
#ifdef EXYNOS4_ASV_ENABLED
	static mali_bool asv_applied = MALI_FALSE;
#endif
	static int stay_count = 5; // to prevent frequent switch

	MALI_DEBUG_PRINT(1, ("> mali_dvfs_status: %d \n",utilization));
#ifdef EXYNOS4_ASV_ENABLED
	if (asv_applied == MALI_FALSE) {
		mali_dvfs_table_update();
		change_mali_dvfs_status(0,0);
		asv_applied = MALI_TRUE;

		return MALI_TRUE;
	}
#endif

	/*decide next step*/
	curStatus = get_mali_dvfs_status();
	nextStatus = decideNextStatus(utilization);

	MALI_DEBUG_PRINT(1, ("= curStatus %d, nextStatus %d, maliDvfsStatus.currentStep %d \n", curStatus, nextStatus, maliDvfsStatus.currentStep));

	/*if next status is same with current status, don't change anything*/
	if(curStatus != nextStatus) {
		/*check if boost up or not*/
		if(maliDvfsStatus.currentStep < nextStatus) {
			boostup = 1;
			stay_count = 5;
		}
		else if (maliDvfsStatus.currentStep > nextStatus){
			stay_count--;
		}
			if( boostup == 1 || stay_count <= 0){
			/*change mali dvfs status*/
			if (!change_mali_dvfs_status(nextStatus,boostup)) {
				MALI_DEBUG_PRINT(1, ("error on change_mali_dvfs_status \n"));
				return MALI_FALSE;
			}
			boostup = 0;
			stay_count = 5;
		}
	}
	else
		stay_count = 5;

	return MALI_TRUE;
}

int mali_dvfs_is_running(void)
{
	return bMaliDvfsRun;
}



void mali_dvfs_late_resume(void)
{
	// set the init clock as low when resume
	set_mali_dvfs_status(0,0);
}


static void mali_dvfs_work_handler(struct work_struct *w)
{
	bMaliDvfsRun=1;

	MALI_DEBUG_PRINT(3, ("=== mali_dvfs_work_handler\n"));

	if (!mali_dvfs_status(mali_dvfs_utilization))
		MALI_DEBUG_PRINT(1,( "error on mali dvfs status in mali_dvfs_work_handler"));

	bMaliDvfsRun=0;
}


mali_bool init_mali_dvfs_status(int step)
{
	/*default status
	add here with the right function to get initilization value.
	*/
	if (!mali_dvfs_wq)
		mali_dvfs_wq = create_singlethread_workqueue("mali_dvfs");

#if MALI_GPU_BOTTOM_LOCK
	_mali_osk_atomic_init(&bottomlock_status, 0);
#endif

	/*add a error handling here*/
	maliDvfsStatus.currentStep = step;

	return MALI_TRUE;
}

void deinit_mali_dvfs_status(void)
{
#if MALI_GPU_BOTTOM_LOCK
	_mali_osk_atomic_term(&bottomlock_status);
#endif

	if (mali_dvfs_wq)
		destroy_workqueue(mali_dvfs_wq);
	mali_dvfs_wq = NULL;
}

mali_bool mali_dvfs_handler(u32 utilization)
{
	mali_dvfs_utilization = utilization;
	queue_work_on(0, mali_dvfs_wq,&mali_dvfs_work);

	/*add error handle here*/
	return MALI_TRUE;
}

void mali_default_step_set(int step, mali_bool boostup)
{
	mali_clk_set_rate(mali_dvfs[step].clock, mali_dvfs[step].freq);

	if (maliDvfsStatus.currentStep == 1)
		set_mali_dvfs_status(step, boostup);
}

#if MALI_GPU_BOTTOM_LOCK
int mali_dvfs_bottom_lock_push(void)
{
	int prev_status = _mali_osk_atomic_read(&bottomlock_status);

	if (prev_status < 0) {
		MALI_PRINT(("gpu bottom lock status is not valid for push"));
		return -1;
	}

	if (prev_status == 0) {
		mali_regulator_set_voltage(mali_dvfs[1].vol, mali_dvfs[1].vol);
		mali_clk_set_rate(mali_dvfs[1].clock, mali_dvfs[1].freq);
		set_mali_dvfs_current_step(1);
	}

	return _mali_osk_atomic_inc_return(&bottomlock_status);
}

int mali_dvfs_bottom_lock_pop(void)
{
	if (_mali_osk_atomic_read(&bottomlock_status) <= 0) {
		MALI_PRINT(("gpu bottom lock status is not valid for pop"));
		return -1;
	}

	return _mali_osk_atomic_dec_return(&bottomlock_status);
}
#endif
