// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2002
 * Rich Ireland, Enterasys Networks, rireland@enterasys.com.
 */

#define LOG_CATEGORY UCLASS_FPGA

#include <config.h>		/* core U-Boot definitions */
#include <log.h>
#include <spartan2.h>		/* Spartan-II device family */
#include <time.h>

/* Note: The assumption is that we cannot possibly run fast enough to
 * overrun the device (the Slave Parallel mode can free run at 50MHz).
 * If there is a need to operate slower, define CFG_FPGA_DELAY in
 * the board config file to slow things down.
 */
#ifndef CFG_FPGA_DELAY
#define CFG_FPGA_DELAY()
#endif

#ifndef CFG_SYS_FPGA_WAIT
#define CFG_SYS_FPGA_WAIT CONFIG_SYS_HZ/100	/* 10 ms */
#endif

static int spartan2_sp_load(xilinx_desc *desc, const void *buf, size_t bsize);
static int spartan2_sp_dump(xilinx_desc *desc, const void *buf, size_t bsize);
/* static int spartan2_sp_info(xilinx_desc *desc ); */

static int spartan2_ss_load(xilinx_desc *desc, const void *buf, size_t bsize);
static int spartan2_ss_dump(xilinx_desc *desc, const void *buf, size_t bsize);
/* static int spartan2_ss_info(xilinx_desc *desc ); */

/* ------------------------------------------------------------------------- */
/* Spartan-II Generic Implementation */
static int spartan2_load(xilinx_desc *desc, const void *buf, size_t bsize,
			 bitstream_type bstype, int flags)
{
	int ret_val = FPGA_FAIL;

	switch (desc->iface) {
	case slave_serial:
		log_debug("Launching Slave Serial Load\n");
		ret_val = spartan2_ss_load(desc, buf, bsize);
		break;

	case slave_parallel:
		log_debug("Launching Slave Parallel Load\n");
		ret_val = spartan2_sp_load(desc, buf, bsize);
		break;

	default:
		printf ("%s: Unsupported interface type, %d\n",
				__FUNCTION__, desc->iface);
	}

	return ret_val;
}

static int spartan2_dump(xilinx_desc *desc, const void *buf, size_t bsize)
{
	int ret_val = FPGA_FAIL;

	switch (desc->iface) {
	case slave_serial:
		log_debug("Launching Slave Serial Dump\n");
		ret_val = spartan2_ss_dump(desc, buf, bsize);
		break;

	case slave_parallel:
		log_debug("Launching Slave Parallel Dump\n");
		ret_val = spartan2_sp_dump(desc, buf, bsize);
		break;

	default:
		printf ("%s: Unsupported interface type, %d\n",
				__FUNCTION__, desc->iface);
	}

	return ret_val;
}

static int spartan2_info(xilinx_desc *desc)
{
	return FPGA_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Spartan-II Slave Parallel Generic Implementation */

static int spartan2_sp_load(xilinx_desc *desc, const void *buf, size_t bsize)
{
	int ret_val = FPGA_FAIL;	/* assume the worst */
	xilinx_spartan2_slave_parallel_fns *fn = desc->iface_fns;

	log_debug("start with interface functions @ 0x%p\n", fn);

	if (fn) {
		size_t bytecount = 0;
		unsigned char *data = (unsigned char *) buf;
		int cookie = desc->cookie;	/* make a local copy */
		unsigned long ts;		/* timestamp */

		log_debug("Function Table:\n"
			  "ptr:\t0x%p\n"
			  "struct: 0x%p\n"
			  "pre: 0x%p\n"
			  "pgm:\t0x%p\n"
			  "init:\t0x%p\n"
			  "err:\t0x%p\n"
			  "clk:\t0x%p\n"
			  "cs:\t0x%p\n"
			  "wr:\t0x%p\n"
			  "read data:\t0x%p\n"
			  "write data:\t0x%p\n"
			  "busy:\t0x%p\n"
			  "abort:\t0x%p\n"
			  "post:\t0x%p\n\n",
			  &fn, fn, fn->pre, fn->pgm, fn->init, fn->err,
			  fn->clk, fn->cs, fn->wr, fn->rdata, fn->wdata, fn->busy,
			  fn->abort, fn->post);

		/*
		 * This code is designed to emulate the "Express Style"
		 * Continuous Data Loading in Slave Parallel Mode for
		 * the Spartan-II Family.
		 */
#ifdef CONFIG_SYS_FPGA_PROG_FEEDBACK
		printf ("Loading FPGA Device %d...\n", cookie);
#endif
		/*
		 * Run the pre configuration function if there is one.
		 */
		if (*fn->pre) {
			(*fn->pre) (cookie);
		}

		/* Establish the initial state */
		(*fn->pgm) (true, true, cookie);	/* Assert the program, commit */

		/* Get ready for the burn */
		CFG_FPGA_DELAY ();
		(*fn->pgm) (false, true, cookie);	/* Deassert the program, commit */

		ts = get_timer (0);		/* get current time */
		/* Now wait for INIT and BUSY to go high */
		do {
			CFG_FPGA_DELAY ();
			if (get_timer (ts) > CFG_SYS_FPGA_WAIT) {	/* check the time */
				puts ("** Timeout waiting for INIT to clear.\n");
				(*fn->abort) (cookie);	/* abort the burn */
				return FPGA_FAIL;
			}
		} while ((*fn->init) (cookie) && (*fn->busy) (cookie));

		(*fn->wr) (true, true, cookie); /* Assert write, commit */
		(*fn->cs) (true, true, cookie); /* Assert chip select, commit */
		(*fn->clk) (true, true, cookie);	/* Assert the clock pin */

		/* Load the data */
		while (bytecount < bsize) {
			/* XXX - do we check for an Ctrl-C press in here ??? */
			/* XXX - Check the error bit? */

			(*fn->wdata) (data[bytecount++], true, cookie); /* write the data */
			CFG_FPGA_DELAY ();
			(*fn->clk) (false, true, cookie);	/* Deassert the clock pin */
			CFG_FPGA_DELAY ();
			(*fn->clk) (true, true, cookie);	/* Assert the clock pin */

#ifdef CONFIG_SYS_FPGA_CHECK_BUSY
			ts = get_timer (0);	/* get current time */
			while ((*fn->busy) (cookie)) {
				/* XXX - we should have a check in here somewhere to
				 * make sure we aren't busy forever... */

				CFG_FPGA_DELAY ();
				(*fn->clk) (false, true, cookie);	/* Deassert the clock pin */
				CFG_FPGA_DELAY ();
				(*fn->clk) (true, true, cookie);	/* Assert the clock pin */

				if (get_timer (ts) > CFG_SYS_FPGA_WAIT) {	/* check the time */
					puts ("** Timeout waiting for BUSY to clear.\n");
					(*fn->abort) (cookie);	/* abort the burn */
					return FPGA_FAIL;
				}
			}
#endif

#ifdef CONFIG_SYS_FPGA_PROG_FEEDBACK
			if (bytecount % (bsize / 40) == 0)
				putc ('.');		/* let them know we are alive */
#endif
		}

		CFG_FPGA_DELAY ();
		(*fn->cs) (false, true, cookie);	/* Deassert the chip select */
		(*fn->wr) (false, true, cookie);	/* Deassert the write pin */

#ifdef CONFIG_SYS_FPGA_PROG_FEEDBACK
		putc ('\n');			/* terminate the dotted line */
#endif

		/* now check for done signal */
		ts = get_timer (0);		/* get current time */
		ret_val = FPGA_SUCCESS;
		while ((*fn->done) (cookie) == FPGA_FAIL) {

			CFG_FPGA_DELAY ();
			(*fn->clk) (false, true, cookie);	/* Deassert the clock pin */
			CFG_FPGA_DELAY ();
			(*fn->clk) (true, true, cookie);	/* Assert the clock pin */

			if (get_timer (ts) > CFG_SYS_FPGA_WAIT) {	/* check the time */
				puts ("** Timeout waiting for DONE to clear.\n");
				(*fn->abort) (cookie);	/* abort the burn */
				ret_val = FPGA_FAIL;
				break;
			}
		}

		/*
		 * Run the post configuration function if there is one.
		 */
		if (*fn->post)
			(*fn->post) (cookie);

#ifdef CONFIG_SYS_FPGA_PROG_FEEDBACK
		if (ret_val == FPGA_SUCCESS)
			puts ("Done.\n");
		else
			puts ("Fail.\n");
#endif

	} else {
		printf ("%s: NULL Interface function table!\n", __FUNCTION__);
	}

	return ret_val;
}

static int spartan2_sp_dump(xilinx_desc *desc, const void *buf, size_t bsize)
{
	int ret_val = FPGA_FAIL;	/* assume the worst */
	xilinx_spartan2_slave_parallel_fns *fn = desc->iface_fns;

	if (fn) {
		unsigned char *data = (unsigned char *) buf;
		size_t bytecount = 0;
		int cookie = desc->cookie;	/* make a local copy */

		printf ("Starting Dump of FPGA Device %d...\n", cookie);

		(*fn->cs) (true, true, cookie); /* Assert chip select, commit */
		(*fn->clk) (true, true, cookie);	/* Assert the clock pin */

		/* dump the data */
		while (bytecount < bsize) {
			/* XXX - do we check for an Ctrl-C press in here ??? */

			(*fn->clk) (false, true, cookie);	/* Deassert the clock pin */
			(*fn->clk) (true, true, cookie);	/* Assert the clock pin */
			(*fn->rdata) (&(data[bytecount++]), cookie);	/* read the data */
#ifdef CONFIG_SYS_FPGA_PROG_FEEDBACK
			if (bytecount % (bsize / 40) == 0)
				putc ('.');		/* let them know we are alive */
#endif
		}

		(*fn->cs) (false, false, cookie);	/* Deassert the chip select */
		(*fn->clk) (false, true, cookie);	/* Deassert the clock pin */
		(*fn->clk) (true, true, cookie);	/* Assert the clock pin */

#ifdef CONFIG_SYS_FPGA_PROG_FEEDBACK
		putc ('\n');			/* terminate the dotted line */
#endif
		puts ("Done.\n");

		/* XXX - checksum the data? */
	} else {
		printf ("%s: NULL Interface function table!\n", __FUNCTION__);
	}

	return ret_val;
}

/* ------------------------------------------------------------------------- */

static int spartan2_ss_load(xilinx_desc *desc, const void *buf, size_t bsize)
{
	int ret_val = FPGA_FAIL;	/* assume the worst */
	xilinx_spartan2_slave_serial_fns *fn = desc->iface_fns;
	int i;
	unsigned char val;

	log_debug("start with interface functions @ 0x%p\n", fn);

	if (fn) {
		size_t bytecount = 0;
		unsigned char *data = (unsigned char *) buf;
		int cookie = desc->cookie;	/* make a local copy */
		unsigned long ts;		/* timestamp */

		log_debug("Function Table:\n"
			  "ptr:\t0x%p\n"
			  "struct: 0x%p\n"
			  "pgm:\t0x%p\n"
			  "init:\t0x%p\n"
			  "clk:\t0x%p\n"
			  "wr:\t0x%p\n"
			  "done:\t0x%p\n\n",
			  &fn, fn, fn->pgm, fn->init,
			  fn->clk, fn->wr, fn->done);
#ifdef CONFIG_SYS_FPGA_PROG_FEEDBACK
		printf ("Loading FPGA Device %d...\n", cookie);
#endif

		/*
		 * Run the pre configuration function if there is one.
		 */
		if (*fn->pre) {
			(*fn->pre) (cookie);
		}

		/* Establish the initial state */
		(*fn->pgm) (true, true, cookie);	/* Assert the program, commit */

		/* Wait for INIT state (init low)                            */
		ts = get_timer (0);		/* get current time */
		do {
			CFG_FPGA_DELAY ();
			if (get_timer (ts) > CFG_SYS_FPGA_WAIT) {	/* check the time */
				puts ("** Timeout waiting for INIT to start.\n");
				return FPGA_FAIL;
			}
		} while (!(*fn->init) (cookie));

		/* Get ready for the burn */
		CFG_FPGA_DELAY ();
		(*fn->pgm) (false, true, cookie);	/* Deassert the program, commit */

		ts = get_timer (0);		/* get current time */
		/* Now wait for INIT to go high */
		do {
			CFG_FPGA_DELAY ();
			if (get_timer (ts) > CFG_SYS_FPGA_WAIT) {	/* check the time */
				puts ("** Timeout waiting for INIT to clear.\n");
				return FPGA_FAIL;
			}
		} while ((*fn->init) (cookie));

		/* Load the data */
		while (bytecount < bsize) {

			/* Xilinx detects an error if INIT goes low (active)
			   while DONE is low (inactive) */
			if ((*fn->done) (cookie) == 0 && (*fn->init) (cookie)) {
				puts ("** CRC error during FPGA load.\n");
				return (FPGA_FAIL);
			}
			val = data [bytecount ++];
			i = 8;
			do {
				/* Deassert the clock */
				(*fn->clk) (false, true, cookie);
				CFG_FPGA_DELAY ();
				/* Write data */
				(*fn->wr) ((val & 0x80), true, cookie);
				CFG_FPGA_DELAY ();
				/* Assert the clock */
				(*fn->clk) (true, true, cookie);
				CFG_FPGA_DELAY ();
				val <<= 1;
				i --;
			} while (i > 0);

#ifdef CONFIG_SYS_FPGA_PROG_FEEDBACK
			if (bytecount % (bsize / 40) == 0)
				putc ('.');		/* let them know we are alive */
#endif
		}

		CFG_FPGA_DELAY ();

#ifdef CONFIG_SYS_FPGA_PROG_FEEDBACK
		putc ('\n');			/* terminate the dotted line */
#endif

		/* now check for done signal */
		ts = get_timer (0);		/* get current time */
		ret_val = FPGA_SUCCESS;
		(*fn->wr) (true, true, cookie);

		while (! (*fn->done) (cookie)) {

			CFG_FPGA_DELAY ();
			(*fn->clk) (false, true, cookie);	/* Deassert the clock pin */
			CFG_FPGA_DELAY ();
			(*fn->clk) (true, true, cookie);	/* Assert the clock pin */

			putc ('*');

			if (get_timer (ts) > CFG_SYS_FPGA_WAIT) {	/* check the time */
				puts ("** Timeout waiting for DONE to clear.\n");
				ret_val = FPGA_FAIL;
				break;
			}
		}
		putc ('\n');			/* terminate the dotted line */

		/*
		 * Run the post configuration function if there is one.
		 */
		if (*fn->post)
			(*fn->post) (cookie);

#ifdef CONFIG_SYS_FPGA_PROG_FEEDBACK
		if (ret_val == FPGA_SUCCESS)
			puts ("Done.\n");
		else
			puts ("Fail.\n");
#endif

	} else {
		printf ("%s: NULL Interface function table!\n", __FUNCTION__);
	}

	return ret_val;
}

static int spartan2_ss_dump(xilinx_desc *desc, const void *buf, size_t bsize)
{
	/* Readback is only available through the Slave Parallel and         */
	/* boundary-scan interfaces.                                         */
	printf ("%s: Slave Serial Dumping is unavailable\n",
			__FUNCTION__);
	return FPGA_FAIL;
}

struct xilinx_fpga_op spartan2_op = {
	.load = spartan2_load,
	.dump = spartan2_dump,
	.info = spartan2_info,
};
