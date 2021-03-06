/*
 * Copyright (C) 2015 Motorola Mobility LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/cdev.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/irq.h>
#include <linux/switch.h>
#include <linux/time.h>
#include <linux/wakelock.h>
#include <linux/gpio.h>

#include <linux/motosh.h>

/* Max latency needs to allow for kernel irq delay and streaming
   queue depth. Nudging offset at 50 uS per sample allows drift
   tracking up to .25 mS/s at 5 Hz sample rates.
*/
#define MAX_DRIFT_LATENCY 100000000  /* 100 ms in ns */
#define MIN_DRIFT_LATENCY    200000  /*  .2 ms in ns */
#define DRIFT_NUDGE           50000  /* .05 ms */

static int64_t motosh_realtime_delta;
static spinlock_t time_sync_lock;

/*
  read hub time interrupt status register
*/
void motosh_time_sync(void)
{

	struct timespec ts;
	int64_t ap_time1;
	int64_t hub_time;
	int64_t delta;
	unsigned char cmdbuff[1];
	unsigned char readbuff[8];
	int err = 0;
	unsigned long flags;

	/* one time init */
	if (!motosh_realtime_delta)
		spin_lock_init(&time_sync_lock);

	/* ensure line starts low */
	gpio_set_value(motosh_misc_data->pdata->gpio_sh_wake, 0);

	/* get AP time and raise wake line to lock in time at hub */
	spin_lock_irqsave(&time_sync_lock, flags);
	get_monotonic_boottime(&ts);
	gpio_set_value(motosh_misc_data->pdata->gpio_sh_wake, 1);
	spin_unlock_irqrestore(&time_sync_lock, flags);

	ap_time1 = ts.tv_sec*1000000000LL + ts.tv_nsec;

	/* read time from hub saved at int */
	cmdbuff[0] = ELAPSED_RT;
	err = motosh_i2c_write_read(motosh_misc_data, cmdbuff, readbuff, 1, 8);
	if (err < 0) {
		dev_err(&motosh_misc_data->client->dev,
			"Unable to read hub time");
	}

	/* nanoseconds */
	hub_time =
		(((uint64_t)readbuff[0] << 56) |
		 ((uint64_t)readbuff[1] << 48) |
		 ((uint64_t)readbuff[2] << 40) |
		 ((uint64_t)readbuff[3] << 32) |
		 ((uint64_t)readbuff[4] << 24) |
		 ((uint64_t)readbuff[5] << 16) |
		 ((uint64_t)readbuff[6] <<  8) |
		  (uint64_t)readbuff[7]) * 1000;

	/* ap time will always be greater than hub time */
	delta = ap_time1 - hub_time;

	/* update offset */
	dev_dbg(&motosh_misc_data->client->dev,
		"Sync time - sh: %12lld ap: %12lld offs_delta: %12lld",
		hub_time, ap_time1, delta - motosh_realtime_delta);

	motosh_realtime_delta = delta;
}

/*
  hubshort - 3 bytes of time from sensorhub in 16 uSec resolution
  (lsb = 16 uSec, mult by 16 to get uSec)

  curtime  - 8 bytes in nSec
 */
int64_t motosh_time_recover(int32_t hubshort, int64_t cur_time)
{

	int64_t hubtime_estimate;
	int64_t hubtime = -1;
	int32_t short_hubtime_estimate;

	/* convert back to uSec */
	hubshort *= 16;

	hubtime_estimate = div64_s64((cur_time - motosh_realtime_delta), 1000); /* uS */
	short_hubtime_estimate = hubtime_estimate & 0xFFFFFFF;

	/* Determine if a rollover needs to be accounted for */
	if (short_hubtime_estimate - hubshort > 130000000) {
		/* hub time likely rolled, AP estimate has not, roll
		   estimate forward
		*/
		hubtime_estimate += 0x010000000;
		dev_dbg(&motosh_misc_data->client->dev,
			 "roll fwd %X %X", short_hubtime_estimate, hubshort);

	} else if (hubshort - short_hubtime_estimate > 130000000) {
		/* AP estimate likely rolled, hub time did not, roll
		   estimate back
		*/
		hubtime_estimate -= 0x010000000;
		dev_dbg(&motosh_misc_data->client->dev,
			 "roll back %X %X", short_hubtime_estimate, hubshort);
	}

	/* recover AP time based on 28bit usec from Hub */
	hubtime = ((hubtime_estimate & 0xFFFFFFFFF0000000) |
		   (hubshort & 0xFFFFFFF)) * 1000;

	return hubtime + motosh_realtime_delta;

}

/*
  rec_hub - last recovered hub time in AP time base
  cur_time - current AP time
  returns direction of nudge to offset
 */
int motosh_time_drift_comp(int64_t rec_hub, int64_t cur_time, bool streaming)
{
	int64_t offset;
	int nudged;

#ifdef MOTOSH_TIME_DEBUG
	static int count;
#endif
	/* offset should be a positve value indicating that the
	   recovered hub time is in the past.
	*/
	offset = cur_time - rec_hub;

	if (streaming && offset > MAX_DRIFT_LATENCY) {
		/* increase delta, to reduce offset on next sample */
		motosh_realtime_delta += DRIFT_NUDGE;
		nudged = 1;
	} else if (offset < MIN_DRIFT_LATENCY) {
		/* reduce delta, to increase offset on next sample */
		motosh_realtime_delta -= DRIFT_NUDGE;
		nudged = -1;
	} else
		nudged = 0;

#ifdef MOTOSH_TIME_DEBUG
	if (nudged || count > 999) {
		count = 0;
		dev_info(&motosh_misc_data->client->dev,
			 "driftcomp, uS delta: %lld, %d\n",
			 offset/1000,
			 nudged * DRIFT_NUDGE/1000);
	}
	count++;
#endif

	return nudged;
}

