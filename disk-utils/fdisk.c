/*
 * Copyright (C) 1992  A. V. Le Blanc (LeBlanc@mcc.ac.uk)
 * Copyright (C) 2012  Davidlohr Bueso <dave@gnu.org>
 *
 * Copyright (C) 2007-2013 Karel Zak <kzak@redhat.com>
 *
 * This program is free software.  You can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation: either version 1 or
 * (at your option) any later version.
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <limits.h>
#include <libsmartcols.h>

#include "c.h"
#include "xalloc.h"
#include "all-io.h"
#include "nls.h"
#include "rpmatch.h"
#include "blkdev.h"
#include "mbsalign.h"
#include "pathnames.h"
#include "canonicalize.h"
#include "strutils.h"
#include "closestream.h"
#include "sysfs.h"

#include "fdisk.h"

#include "pt-sun.h"		/* to toggle flags */

#ifdef HAVE_LINUX_COMPILER_H
# include <linux/compiler.h>
#endif
#ifdef HAVE_LINUX_BLKPG_H
# include <linux/blkpg.h>
#endif

/*
 * fdisk debug stuff (see fdisk.h and include/debug.h)
 */
UL_DEBUG_DEFINE_MASK(fdisk);
UL_DEBUG_DEFINE_MASKANEMS(fdisk) = UL_DEBUG_EMPTY_MASKNAMES;

static void fdiskprog_init_debug(void)
{
	__UL_INIT_DEBUG(fdisk, FDISKPROG_DEBUG_, 0, FDISK_DEBUG);
}

int get_user_reply(struct fdisk_context *cxt, const char *prompt,
			  char *buf, size_t bufsz)
{
	char *p;
	size_t sz;

	do {
	        fputs(prompt, stdout);
		fflush(stdout);

		if (!fgets(buf, bufsz, stdin)) {
			if (fdisk_label_is_changed(fdisk_get_label(cxt, NULL))) {
				fprintf(stderr, _("\nDo you really want to quit? "));

				if (fgets(buf, bufsz, stdin) && !rpmatch(buf))
					continue;
			}
			fdisk_unref_context(cxt);
			exit(EXIT_FAILURE);
		} else
			break;
	} while (1);

	for (p = buf; *p && !isgraph(*p); p++);	/* get first non-blank */

	if (p > buf)
		memmove(buf, p, p - buf);		/* remove blank space */
	sz = strlen(buf);
	if (sz && *(buf + sz - 1) == '\n')
		*(buf + sz - 1) = '\0';

	DBG(ASK, ul_debug("user's reply: >>>%s<<<", buf));
	return 0;
}

static int ask_menu(struct fdisk_context *cxt, struct fdisk_ask *ask,
		    char *buf, size_t bufsz)

{
	const char *q = fdisk_ask_get_query(ask);
	int dft = fdisk_ask_menu_get_default(ask);

	if (q) {
		fputs(q, stdout);		/* print header */
		fputc('\n', stdout);
	}

	do {
		char prompt[128];
		int key, c;
		const char *name, *desc;
		size_t i = 0;

		/* print menu items */
		while (fdisk_ask_menu_get_item(ask, i++, &key, &name, &desc) == 0)
			fprintf(stdout, "   %c   %s (%s)\n", key, name, desc);

		/* ask for key */
		snprintf(prompt, sizeof(prompt), _("Select (default %c): "), dft);
		get_user_reply(cxt, prompt, buf, bufsz);
		if (!*buf) {
			fdisk_info(cxt, _("Using default response %c."), dft);
			c = dft;
		} else
			c = tolower(buf[0]);

		/* check result */
		i = 0;
		while (fdisk_ask_menu_get_item(ask, i++, &key, NULL, NULL) == 0) {
			if (c == key) {
				fdisk_ask_menu_set_result(ask, c);
				return 0;	/* success */
			}
		}
		fdisk_warnx(cxt, _("Value out of range."));
	} while (1);

	return -EINVAL;
}


#define tochar(num)	((int) ('a' + num - 1))
static int ask_number(struct fdisk_context *cxt,
		      struct fdisk_ask *ask,
		      char *buf, size_t bufsz)
{
	char prompt[128] = { '\0' };
	const char *q = fdisk_ask_get_query(ask);
	const char *range = fdisk_ask_number_get_range(ask);

	uint64_t dflt = fdisk_ask_number_get_default(ask),
		 low = fdisk_ask_number_get_low(ask),
		 high = fdisk_ask_number_get_high(ask);
	int inchar = fdisk_ask_number_inchars(ask);

	assert(q);

	DBG(ASK, ul_debug("asking for number "
			"['%s', <%ju,%ju>, default=%ju, range: %s]",
			q, low, high, dflt, range));

	if (range && dflt >= low && dflt <= high) {
		if (inchar)
			snprintf(prompt, sizeof(prompt), _("%s (%s, default %c): "),
					q, range, tochar(dflt));
		else
			snprintf(prompt, sizeof(prompt), _("%s (%s, default %ju): "),
					q, range, dflt);

	} else if (dflt >= low && dflt <= high) {
		if (inchar)
			snprintf(prompt, sizeof(prompt), _("%s (%c-%c, default %c): "),
					q, tochar(low), tochar(high), tochar(dflt));
		else
			snprintf(prompt, sizeof(prompt), _("%s (%ju-%ju, default %ju): "),
					q, low, high, dflt);
	} else if (inchar)
		snprintf(prompt, sizeof(prompt), _("%s (%c-%c): "),
				q, tochar(low), tochar(high));
	else
		snprintf(prompt, sizeof(prompt), _("%s (%ju-%ju): "),
				q, low, high);

	do {
		int rc = get_user_reply(cxt, prompt, buf, bufsz);
		uint64_t num;

		if (rc)
			return rc;
		if (!*buf && dflt >= low && dflt <= high)
			return fdisk_ask_number_set_result(ask, dflt);

		if (isdigit_string(buf)) {
			char *end;

			errno = 0;
			num = strtoumax(buf, &end, 10);
			if (errno || buf == end || (end && *end))
				continue;
		} else if (inchar && isalpha(*buf)) {
			num = tolower(*buf) - 'a' + 1;
		} else
			rc = -EINVAL;

		if (rc == 0 && num >= low && num <= high)
			return fdisk_ask_number_set_result(ask, num);

		fdisk_warnx(cxt, _("Value out of range."));
	} while (1);

	return -1;
}

static int ask_offset(struct fdisk_context *cxt,
		      struct fdisk_ask *ask,
		      char *buf, size_t bufsz)
{
	char prompt[128] = { '\0' };
	const char *q = fdisk_ask_get_query(ask);
	const char *range = fdisk_ask_number_get_range(ask);

	uint64_t dflt = fdisk_ask_number_get_default(ask),
		 low = fdisk_ask_number_get_low(ask),
		 high = fdisk_ask_number_get_high(ask),
		 base = fdisk_ask_number_get_base(ask);

	assert(q);

	DBG(ASK, ul_debug("asking for offset ['%s', <%ju,%ju>, base=%ju, default=%ju, range: %s]",
				q, low, high, base, dflt, range));

	if (range && dflt >= low && dflt <= high)
		snprintf(prompt, sizeof(prompt), _("%s (%s, default %ju): "), q, range, dflt);
	else if (dflt >= low && dflt <= high)
		snprintf(prompt, sizeof(prompt), _("%s (%ju-%ju, default %ju): "), q, low, high, dflt);
	else
		snprintf(prompt, sizeof(prompt), _("%s (%ju-%ju): "), q, low, high);

	do {
		uint64_t num = 0;
		char sig = 0, *p;
		int pwr = 0;

		int rc = get_user_reply(cxt, prompt, buf, bufsz);
		if (rc)
			return rc;
		if (!*buf && dflt >= low && dflt <= high)
			return fdisk_ask_number_set_result(ask, dflt);

		p = buf;
		if (*p == '+' || *p == '-') {
			sig = *buf;
			p++;
		}

		rc = parse_size(p, &num, &pwr);
		if (rc)
			continue;
		DBG(ASK, ul_debug("parsed size: %ju", num));
		if (sig && pwr) {
			/* +{size}{K,M,...} specified, the "num" is in bytes */
			uint64_t unit = fdisk_ask_number_get_unit(ask);
			num += unit/2;	/* round */
			num /= unit;
		}
		if (sig == '+')
			num += base;
		else if (sig == '-')
			num = base - num;

		DBG(ASK, ul_debug("final offset: %ju [sig: %c, power: %d, %s]",
				num, sig, pwr,
				sig ? "relative" : "absolute"));
		if (num >= low && num <= high) {
			if (sig && pwr)
				fdisk_ask_number_set_relative(ask, 1);
			return fdisk_ask_number_set_result(ask, num);
		}
		fdisk_warnx(cxt, _("Value out of range."));
	} while (1);

	return -1;
}

static unsigned int info_count;

static void fputs_info(struct fdisk_ask *ask, FILE *out)
{
	const char *msg;
	assert(ask);

	msg = fdisk_ask_print_get_mesg(ask);
	if (!msg)
		return;
	if (info_count == 1)
		fputc('\n', out);

	fputs(msg, out);
	fputc('\n', out);
}

int ask_callback(struct fdisk_context *cxt, struct fdisk_ask *ask,
		    void *data __attribute__((__unused__)))
{
	int rc = 0;
	char buf[BUFSIZ];

	assert(cxt);
	assert(ask);

	if (fdisk_ask_get_type(ask) != FDISK_ASKTYPE_INFO)
		info_count = 0;

	switch(fdisk_ask_get_type(ask)) {
	case FDISK_ASKTYPE_MENU:
		return ask_menu(cxt, ask, buf, sizeof(buf));
	case FDISK_ASKTYPE_NUMBER:
		return ask_number(cxt, ask, buf, sizeof(buf));
	case FDISK_ASKTYPE_OFFSET:
		return ask_offset(cxt, ask, buf, sizeof(buf));
	case FDISK_ASKTYPE_INFO:
		if (!fdisk_is_listonly(cxt))
			info_count++;
		fputs_info(ask, stdout);
		break;
	case FDISK_ASKTYPE_WARNX:
		color_scheme_fenable("warn", UL_COLOR_RED, stderr);
		fputs(fdisk_ask_print_get_mesg(ask), stderr);
		color_fdisable(stderr);
		fputc('\n', stderr);
		break;
	case FDISK_ASKTYPE_WARN:
		color_scheme_fenable("warn", UL_COLOR_RED, stderr);
		fputs(fdisk_ask_print_get_mesg(ask), stderr);
		errno = fdisk_ask_print_get_errno(ask);
		fprintf(stderr, ": %m\n");
		color_fdisable(stderr);
		break;
	case FDISK_ASKTYPE_YESNO:
		fputc('\n', stdout);
		do {
			int x;
			fputs(fdisk_ask_get_query(ask), stdout);
			rc = get_user_reply(cxt, _(" [Y]es/[N]o: "), buf, sizeof(buf));
			if (rc)
				break;
			x = rpmatch(buf);
			if (x == 1 || x == 0) {
				fdisk_ask_yesno_set_result(ask, x);
				break;
			}
		} while(1);
		DBG(ASK, ul_debug("yes-no ask: reply '%s' [rc=%d]", buf, rc));
		break;
	case FDISK_ASKTYPE_STRING:
	{
		char prmt[BUFSIZ];
		snprintf(prmt, sizeof(prmt), "%s: ", fdisk_ask_get_query(ask));
		fputc('\n', stdout);
		rc = get_user_reply(cxt, prmt, buf, sizeof(buf));
		if (rc == 0)
			fdisk_ask_string_set_result(ask, xstrdup(buf));
		DBG(ASK, ul_debug("string ask: reply '%s' [rc=%d]", buf, rc));
		break;
	}
	default:
		warnx(_("internal error: unsupported dialog type %d"), fdisk_ask_get_type(ask));
		return -EINVAL;
	}
	return rc;
}

struct fdisk_parttype *ask_partition_type(struct fdisk_context *cxt)
{
	const char *q;
	struct fdisk_label *lb;

	assert(cxt);
	lb = fdisk_get_label(cxt, NULL);

	if (!lb)
		return NULL;

        q = fdisk_label_has_code_parttypes(lb) ?
		_("Partition type (type L to list all types): ") :
		_("Hex code (type L to list all codes): ");
	do {
		char buf[256];
		int rc = get_user_reply(cxt, q, buf, sizeof(buf));

		if (rc)
			break;

		if (buf[1] == '\0' && toupper(*buf) == 'L')
			list_partition_types(cxt);
		else if (*buf)
			return fdisk_label_parse_parttype(lb, buf);
        } while (1);

	return NULL;
}

void list_partition_types(struct fdisk_context *cxt)
{
	size_t ntypes = 0;
	struct fdisk_label *lb = fdisk_get_label(cxt, NULL);

	assert(cxt);
	lb = fdisk_get_label(cxt, NULL);
	if (!lb)
		return;
	ntypes = fdisk_label_get_nparttypes(lb);
	if (!ntypes)
		return;

	if (fdisk_label_has_code_parttypes(lb)) {
		/*
		 * Prints in 4 columns in format <hex> <name>
		 */
		size_t last[4], done = 0, next = 0, size;
		int i;

		size = ntypes;

		for (i = 3; i >= 0; i--)
			last[3 - i] = done += (size + i - done) / (i + 1);
		i = done = 0;

		do {
			#define NAME_WIDTH 15
			char name[NAME_WIDTH * MB_LEN_MAX];
			size_t width = NAME_WIDTH;
			const struct fdisk_parttype *t = fdisk_label_get_parttype(lb, next);
			size_t ret;

			if (fdisk_parttype_get_name(t)) {
				printf("%c%2x  ", i ? ' ' : '\n',
						fdisk_parttype_get_code(t));
				ret = mbsalign(_(fdisk_parttype_get_name(t)),
						name, sizeof(name),
						&width, MBS_ALIGN_LEFT, 0);

				if (ret == (size_t)-1 || ret >= sizeof(name))
					printf("%-15.15s",
						_(fdisk_parttype_get_name(t)));
				else
					fputs(name, stdout);
			}

			next = last[i++] + done;
			if (i > 3 || next >= last[i]) {
				i = 0;
				next = ++done;
			}
		} while (done < last[0]);

	} else {
		/*
		 * Prints 1 column in format <idx> <name> <typestr>
		 */
		size_t i;

		for (i = 0; i < ntypes; i++) {
			const struct fdisk_parttype *t = fdisk_label_get_parttype(lb, i);
			printf("%3zu %-30s %s\n", i + 1,
					fdisk_parttype_get_name(t),
					fdisk_parttype_get_string(t));
		}
	}
	putchar('\n');
}

void toggle_dos_compatibility_flag(struct fdisk_context *cxt)
{
	struct fdisk_label *lb = fdisk_get_label(cxt, "dos");
	int flag;

	if (!lb)
		return;

	flag = !fdisk_dos_is_compatible(lb);
	fdisk_info(cxt, flag ?
			_("DOS Compatibility flag is set (DEPRECATED!)") :
			_("DOS Compatibility flag is not set"));

	fdisk_dos_enable_compatible(lb, flag);

	if (fdisk_is_label(cxt, DOS))
		fdisk_reset_alignment(cxt);	/* reset the current label */
}

void change_partition_type(struct fdisk_context *cxt)
{
	size_t i;
	struct fdisk_parttype *t = NULL;
	struct fdisk_partition *pa = NULL;
	const char *old = NULL;

	assert(cxt);

	if (fdisk_ask_partnum(cxt, &i, FALSE))
		return;

	if (fdisk_get_partition(cxt, i, &pa)) {
		fdisk_warnx(cxt, _("Partition %zu does not exist yet!"), i + 1);
		return;
	}

	t = (struct fdisk_parttype *) fdisk_partition_get_type(pa);
	old = t ? fdisk_parttype_get_name(t) : _("Unknown");

	do {
		t = ask_partition_type(cxt);
	} while (!t);

	if (fdisk_set_partition_type(cxt, i, t) == 0)
		fdisk_sinfo(cxt, FDISK_INFO_SUCCESS,
			_("Changed type of partition '%s' to '%s'."),
			old, t ? fdisk_parttype_get_name(t) : _("Unknown"));
	else
		fdisk_info(cxt,
			_("Type of partition %zu is unchanged: %s."),
			i + 1, old);

	fdisk_unref_partition(pa);
}

static size_t skip_empty(const unsigned char *buf, size_t i, size_t sz)
{
	size_t next;
	const unsigned char *p0 = buf + i;

	for (next = i + 16; next < sz; next += 16) {
		if (memcmp(p0, buf + next, 16) != 0)
			break;
	}

	return next == i + 16 ? i : next;
}

static void dump_buffer(off_t base, unsigned char *buf, size_t sz, int all)
{
	size_t i, l, next = 0;

	if (!buf)
		return;
	for (i = 0, l = 0; i < sz; i++, l++) {
		if (l == 0) {
			if (all == 0 && !next)
				next = skip_empty(buf, i, sz);
			printf("%08jx ", base + i);
		}
		printf(" %02x", buf[i]);
		if (l == 7)				/* words separator */
			fputs(" ", stdout);
		else if (l == 15) {
			fputc('\n', stdout);		/* next line */
			l = -1;
			if (next > i) {
				printf("*\n");
				i = next - 1;
			}
			next = 0;
		}
	}
	if (l > 0)
		printf("\n");
}

static void dump_blkdev(struct fdisk_context *cxt, const char *name,
			off_t offset, size_t size, int all)
{
	int fd = fdisk_get_devfd(cxt);

	fdisk_info(cxt, _("\n%s: offset = %ju, size = %zu bytes."),
			name, offset, size);

	assert(fd >= 0);

	if (lseek(fd, offset, SEEK_SET) == (off_t) -1)
		fdisk_warn(cxt, _("cannot seek"));
	else {
		unsigned char *buf = xmalloc(size);

		if (read_all(fd, (char *) buf, size) != (ssize_t) size)
			fdisk_warn(cxt, _("cannot read"));
		else
			dump_buffer(offset, buf, size, all);
		free(buf);
	}
}

void dump_firstsector(struct fdisk_context *cxt)
{
	int all = !isatty(STDOUT_FILENO);

	assert(cxt);

	dump_blkdev(cxt, _("First sector"), 0, fdisk_get_sector_size(cxt), all);
}

void dump_disklabel(struct fdisk_context *cxt)
{
	int all = !isatty(STDOUT_FILENO);
	int i = 0;
	const char *name = NULL;
	off_t offset = 0;
	size_t size = 0;

	assert(cxt);

	while (fdisk_locate_disklabel(cxt, i++, &name, &offset, &size) == 0 && size)
		dump_blkdev(cxt, name, offset, size, all);
}

static sector_t get_dev_blocks(char *dev)
{
	int fd, ret;
	sector_t size;

	if ((fd = open(dev, O_RDONLY)) < 0)
		err(EXIT_FAILURE, _("cannot open %s"), dev);
	ret = blkdev_get_sectors(fd, &size);
	close(fd);
	if (ret < 0)
		err(EXIT_FAILURE, _("BLKGETSIZE ioctl failed on %s"), dev);
	return size/2;
}

static void __attribute__ ((__noreturn__)) usage(FILE *out)
{
	fputs(USAGE_HEADER, out);

	fprintf(out,
	      _(" %1$s [options] <disk>      change partition table\n"
	        " %1$s [options] -l [<disk>] list partition table(s)\n"),
	       program_invocation_short_name);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -b, --sector-size <size>      physical and logical sector size\n"), out);
	fputs(_(" -c, --compatibility[=<mode>]  mode is 'dos' or 'nondos' (default)\n"), out);
	fputs(_(" -L, --color[=<when>]          colorize output (auto, always or never)\n"), out);
	fputs(_(" -l, --list                    display partitions end exit\n"), out);
	fputs(_(" -t, --type <type>             recognize specified partition table type only\n"), out);
	fputs(_(" -u, --units[=<unit>]          display units: 'cylinders' or 'sectors' (default)\n"), out);
	fputs(_(" -s, --getsz                   display device size in 512-byte sectors [DEPRECATED]\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fputs(_(" -C, --cylinders <number>      specify the number of cylinders\n"), out);
	fputs(_(" -H, --heads <number>          specify the number of heads\n"), out);
	fputs(_(" -S, --sectors <number>        specify the number of sectors per track\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fputs(USAGE_HELP, out);
	fputs(USAGE_VERSION, out);

	list_available_columns(out);

	fprintf(out, USAGE_MAN_TAIL("fdisk(8)"));
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}


enum {
	ACT_FDISK = 0,	/* default */
	ACT_LIST,
	ACT_SHOWSIZE
};

int main(int argc, char **argv)
{
	int rc, i, c, act = ACT_FDISK;
	int colormode = UL_COLORMODE_UNDEF;
	struct fdisk_context *cxt;
	char *outarg = NULL;

	static const struct option longopts[] = {
		{ "color",          optional_argument, NULL, 'L' },
		{ "compatibility",  optional_argument, NULL, 'c' },
		{ "cylinders",      required_argument, NULL, 'C' },
		{ "heads",	    required_argument, NULL, 'H' },
		{ "sectors",        required_argument, NULL, 'S' },
		{ "getsz",          no_argument,       NULL, 's' },
		{ "help",           no_argument,       NULL, 'h' },
		{ "list",           no_argument,       NULL, 'l' },
		{ "sector-size",    required_argument, NULL, 'b' },
		{ "type",           required_argument, NULL, 't' },
		{ "units",          optional_argument, NULL, 'u' },
		{ "version",        no_argument,       NULL, 'V' },
		{ NULL, 0, NULL, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	fdisk_init_debug(0);
	fdiskprog_init_debug();

	cxt = fdisk_new_context();
	if (!cxt)
		err(EXIT_FAILURE, _("failed to allocate libfdisk context"));

	fdisk_set_ask(cxt, ask_callback, NULL);

	while ((c = getopt_long(argc, argv, "b:c::C:hH:lL::o:sS:t:u::vV",
				longopts, NULL)) != -1) {
		switch (c) {
		case 'b':
		{
			size_t sz = strtou32_or_err(optarg,
					_("invalid sector size argument"));
			if (sz != 512 && sz != 1024 && sz != 2048 && sz != 4096)
				usage(stderr);
			fdisk_save_user_sector_size(cxt, sz, sz);
			break;
		}
		case 'C':
			fdisk_save_user_geometry(cxt,
				strtou32_or_err(optarg,
						_("invalid cylinders argument")),
				0, 0);
			break;
		case 'c':
			if (optarg) {
				/* this setting is independent on the current
				 * actively used label
				 */
				char *p = *optarg == '=' ? optarg + 1 : optarg;
				struct fdisk_label *lb = fdisk_get_label(cxt, "dos");

				if (!lb)
					err(EXIT_FAILURE, _("not found DOS label driver"));
				if (strcmp(p, "dos") == 0)
					fdisk_dos_enable_compatible(lb, TRUE);
				else if (strcmp(p, "nondos") == 0)
					fdisk_dos_enable_compatible(lb, FALSE);
				else {
					warnx(_("unknown compatibility mode '%s'"), p);
					usage(stderr);
				}
			}
			/* use default if no optarg specified */
			break;
		case 'H':
			fdisk_save_user_geometry(cxt, 0,
				strtou32_or_err(optarg,
						_("invalid heads argument")),
				0);
			break;
		case 'S':
			fdisk_save_user_geometry(cxt, 0, 0,
				strtou32_or_err(optarg,
					_("invalid sectors argument")));
			break;
		case 'l':
			act = ACT_LIST;
			break;
		case 'L':
			if (optarg)
				colormode = colormode_or_err(optarg,
						_("unsupported color mode"));
			break;
		case 'o':
			outarg = optarg;
			break;
		case 's':
			act = ACT_SHOWSIZE;
			break;
		case 't':
		{
			struct fdisk_label *lb = NULL;

			while (fdisk_next_label(cxt, &lb) == 0)
				fdisk_label_set_disabled(lb, 1);

			lb = fdisk_get_label(cxt, optarg);
			if (!lb)
				errx(EXIT_FAILURE, _("unsupported disklabel: %s"), optarg);
			fdisk_label_set_disabled(lb, 0);
		}
		case 'u':
			if (optarg && *optarg == '=')
				optarg++;
			if (fdisk_set_unit(cxt, optarg) != 0)
				usage(stderr);
			break;
		case 'V': /* preferred for util-linux */
		case 'v': /* for backward compatibility only */
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage(stdout);
		default:
			usage(stderr);
		}
	}

	if (argc-optind != 1 && fdisk_has_user_device_properties(cxt))
		warnx(_("The device properties (sector size and geometry) should"
			" be used with one specified device only."));

	colors_init(colormode, "fdisk");

	switch (act) {
	case ACT_LIST:
		fdisk_enable_listonly(cxt, 1);
		init_fields(cxt, outarg, NULL);

		if (argc > optind) {
			int k;
			for (k = optind; k < argc; k++)
				print_device_pt(cxt, argv[k], 1, 0);
		} else
			print_all_devices_pt(cxt, 0);
		break;

	case ACT_SHOWSIZE:
		/* deprecated */
		if (argc - optind <= 0)
			usage(stderr);

		for (i = optind; i < argc; i++) {
			if (argc - optind == 1)
				printf("%llu\n", get_dev_blocks(argv[i]));
			else
				printf("%s: %llu\n", argv[i], get_dev_blocks(argv[i]));
		}
		break;

	case ACT_FDISK:
		if (argc-optind != 1)
			usage(stderr);

		/* Here starts interactive mode, use fdisk_{warn,info,..} functions */
		color_scheme_enable("welcome", UL_COLOR_GREEN);
		fdisk_info(cxt, _("Welcome to fdisk (%s)."), PACKAGE_STRING);
		color_disable();
		fdisk_info(cxt, _("Changes will remain in memory only, until you decide to write them.\n"
				  "Be careful before using the write command.\n"));

		rc = fdisk_assign_device(cxt, argv[optind], 0);
		if (rc == -EACCES) {
			rc = fdisk_assign_device(cxt, argv[optind], 1);
			if (rc == 0)
				fdisk_warnx(cxt, _("Device open in read-only mode."));
		}
		if (rc)
			err(EXIT_FAILURE, _("cannot open %s"), argv[optind]);

		fflush(stdout);

		if (!fdisk_has_label(cxt)) {
			fdisk_info(cxt, _("Device does not contain a recognized partition table."));
			fdisk_create_disklabel(cxt, NULL);

		} else if (fdisk_is_label(cxt, GPT) && fdisk_gpt_is_hybrid(cxt))
			fdisk_warnx(cxt, _(
				  "A hybrid GPT was detected. You have to sync "
				  "the hybrid MBR manually (expert command 'M')."));

		init_fields(cxt, outarg, NULL);		/* -o <columns> */

		while (1)
			process_fdisk_menu(&cxt);
	}

	fdisk_unref_context(cxt);
	return EXIT_SUCCESS;
}
