/*
 * Copyright 2016-2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * strace.ebpf.c -- trace syscalls using eBPF linux kernel feature
 */

#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#include <linux/sched.h>
#include <linux/limits.h>

#include <bcc/libbpf.h>
#include <bcc/bpf_common.h>
#include <bcc/perf_reader.h>

#include "strace.ebpf.h"
#include "bpf_ctx.h"
#include "txt.h"
#include "utils.h"
#include "cl_parser.h"
#include "attach_probes.h"
#include "ebpf_syscalls.h"
#include "generate_ebpf.h"
#include "print_event_cb.h"
#include "ebpf/ebpf_file_set.h"

struct cl_options Args;		/* command-line arguments */
FILE *Out_lf;			/* output file */
enum out_lf_fmt Out_lf_fmt;	/* format of output */

int OutputError;		/* I/O error in perf callback occured */
int AbortTracing;		/* terminating signal received */

pid_t PidToBeKilled;		/* PID of started traced process */

/* what are we tracing ? */
enum {
	TRACING_ALL = 0,	/* all syscalls in the system */
	TRACING_CMD = 1,	/* a process given by the command */
	TRACING_PID = 2,	/* a process given by the PID */
};

/*
 * check_args -- check input arguments
 */
int
check_args(struct cl_options const *args)
{
	if (args->command && args->pid > 0) {
		ERROR("command and PID cannot be set together");
		fprint_help(stderr);
		exit(EXIT_FAILURE);
	}

	if (args->pid > 0) {
		/* check if process with given PID exists */
		if (kill(Args.pid, 0) == -1) {
			ERROR("process with PID '%d' does not exist", Args.pid);
			exit(EXIT_FAILURE);
		}
		return TRACING_PID;
	}

	if (args->command) {
		return TRACING_CMD;
	}

	WARNING("will trace all syscalls in the system...");

	return TRACING_ALL;
}

int
main(const int argc, char *const argv[])
{
	struct perf_reader **readers;
	int tracing = TRACING_ALL; /* what are we tracing ? */
	int st_optind;

	Args.pid = -1;
	Args.out_lf_fld_sep_ch = ' ';

	/* XXX set using command-line options */
	Args.pr_arr_max = 1000;

	/* XXX set using command-line options */
	Args.out_buf_size = OUT_BUF_SIZE;

	/* enlarge ring buffers	- XXX set using command-line options */
	Args.strace_reader_page_cnt = STRACE_READER_PAGE_CNT_DEFAULT;

	/* parse command-line options */
	st_optind = cl_parser(&Args, argc, argv);

	/* check input arguments */
	tracing = check_args(&Args);

	setup_out_lf();
	if (NULL == Out_lf) {
		ERROR("failed to set up the output file");
		return EXIT_FAILURE;
	}

	/* check JIT acceleration of BPF */
	check_bpf_jit_status(stderr);

	INFO("Initializing...");
	init_printing_events();
	/* init array of syscalls */
	init_sc_tbl();

	if (Args.ff_mode) { /* only in follow-fork mode */
		/*
		 * Set the "child subreaper" attribute to be able
		 * to wait for all children and grandchildren.
		 */
		if (prctl(PR_SET_CHILD_SUBREAPER, 1) == -1) {
			perror("prctl");
			ERROR("failed to set 'child subreaper' attribute");
			return EXIT_FAILURE;
		}
	}

	attach_signals_handlers();

	if (Args.command) {
		/* run the command */
		Args.pid = start_command(argv + st_optind);
		if (Args.pid == -1) {
			ERROR("failed to start the command");
			return EXIT_FAILURE;
		}

		/* if tracing is aborted, kill the started process */
		PidToBeKilled = Args.pid;
	}

	INFO("Generating eBPF code...");

	/* generate BPF program */
	char *bpf_str = generate_ebpf();
	if (bpf_str == NULL) {
		ERROR("cannot generate eBPF code");
		return EXIT_FAILURE;
	}

	apply_process_attach_code(&bpf_str);

	/* simulate preprocessor, because it's safer */
	apply_trace_h_header(&bpf_str);

	/* print resulting code in debug mode */
	if (Args.debug)
		fprint_ebpf_code_with_debug_marks(stderr, bpf_str);

	/* XXX should be done only by user request */
	save_trace_h();

	/* initialize BPF */
	struct bpf_ctx *b = calloc(1, sizeof(*b));
	if (b == NULL) {
		ERROR("out of memory");
		free(bpf_str);
		return EXIT_FAILURE;
	}

	/* compile generated eBPF code */
	INFO("Compiling generated eBPF code...");
	b->module = bpf_module_create_c_from_string(bpf_str, 0, NULL, 0);
	free(bpf_str);
	if (b->module == NULL) {
		ERROR("cannot compile eBPF code");
		return EXIT_FAILURE;
	}

	b->debug  = Args.debug;

	INFO("Attaching probes...");
	if (!attach_probes(b)) {
		ERROR("no probes were attached");
		goto error_kill;
	}

	INFO("Starting tracing...");

	if (Print_header[Out_lf_fmt](argc, argv)) {
		ERROR("error while printing header");
		goto error_detach;
	}

	/*
	 * Attach callback to perf output. "events" is a name of class declared
	 * with BPF_PERF_OUTPUT() in ebpf/trace_head.c.
	 *
	 * XXX We should use str_replace here.
	 */
#define PERF_OUTPUT_NAME "events"
	int res = attach_callback_to_perf_output(b, PERF_OUTPUT_NAME,
						Print_event_cb[Out_lf_fmt]);
	if (res == 0) {
		if (Args.command) {
			/* let child go */
			kill(Args.pid, SIGCONT);
		}
	} else {
		ERROR("cannot attach callbacks to perf output '%s'",
			PERF_OUTPUT_NAME);
		goto error_detach;
	}

	readers = calloc(b->pr_arr_qty, sizeof(struct perf_reader *));
	if (readers == NULL) {
		ERROR("out of memory");
		goto error_detach;
	}

	for (unsigned i = 0; i < b->pr_arr_qty; i++)
		readers[i] = b->pr_arr[i]->pr;

	int stop_tracing = 0;
	while (!stop_tracing) {

		(void) perf_reader_poll((int)b->pr_arr_qty, readers, -1);

		if (OutputError) {
			ERROR("error while writing to output");
			break;
		}

		if (AbortTracing) {
			INFO("Notice: terminated by signal. Exiting...");
			break;
		}

		switch (tracing) {
		case TRACING_ALL:
			/* wait for a terminating signal */
			break;
		case TRACING_CMD:
			if (Args.ff_mode) {
				/* trace until all children exit */
				if ((waitpid(-1, NULL, WNOHANG) == -1) &&
				    (errno == ECHILD)) {
					INFO("Notice: all children exited");
					stop_tracing = 1;
				}
			} else {
				/* trace until the child exits */
				if (waitpid(-1, NULL, WNOHANG) == Args.pid) {
					INFO("Notice: the child exited");
					stop_tracing = 1;
				}
			}
			break;
		case TRACING_PID:
			/* check if the process traced by PID exists */
			if (kill(Args.pid, 0) == -1) {
				ERROR("traced process with PID '%d' "
					"disappeared", Args.pid);
				stop_tracing = 1;
			}
			break;
		}
	}

	fflush(Out_lf);
	free(readers);
	detach_all(b);
	return EXIT_SUCCESS;

error_detach:
	detach_all(b);
error_kill:
	if (PidToBeKilled) {
		/* kill the started child */
		kill(PidToBeKilled, SIGKILL);
	}
	return EXIT_FAILURE;
}
