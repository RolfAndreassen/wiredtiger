/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "format.h"
#include "config.h"

static void	   config_checksum(void);
static void	   config_compression(void);
static const char *config_file_type(u_int);
static CONFIG	  *config_find(const char *, size_t);
static int	   config_find_is_perm(const char *, size_t);
static void	   config_map_checksum(const char *, u_int *);
static void	   config_map_compression(const char *, u_int *);
static void	   config_map_file_type(const char *, u_int *);

/*
 * config_setup --
 *	Initialize configuration for a run.
 */
void
config_setup(void)
{
	CONFIG *cp;

	/* Clear any temporary values. */
	config_clear();

	/*
	 * Choose a data source type and a file type: they're interrelated (LSM
	 * trees are only compatible with row-store) and other items depend on
	 * them.
	 */
	if (!config_find_is_perm("data_source", strlen("data_source")))
		switch (MMRAND(1, 3)) {
		case 1:
			config_single("data_source=file", 0);
			break;
		case 2:
#if 0
			config_single("data_source=lsm", 0);
			break;
#endif
		case 3:
			config_single("data_source=table", 0);
			break;
		}

	if (!config_find_is_perm("file_type", strlen("file_type")))
		switch (MMRAND(1, 3)) {
		case 1:
			if (!DATASOURCE("lsm")) {
				config_single("file_type=fix", 0);
				break;
			}
			/* FALLTHROUGH */
		case 2:
			if (!DATASOURCE("lsm")) {
				config_single("file_type=var", 0);
				break;
			}
			/* FALLTHROUGH */
		case 3:
			config_single("file_type=row", 0);
			break;
		}
	config_map_file_type(g.c_file_type, &g.type);

	/*
	 * If data_source and file_type were both "permanent", we may still
	 * have a mismatch.
	 */
	if (DATASOURCE("lsm") && g.type != ROW) {
		fprintf(stderr,
	    "%s: lsm data_source is only compatible with row file_type\n",
		    g.progname);
		exit(EXIT_FAILURE);
	}

	/*
	 * Build the top-level object name: we're overloading data_source in
	 * our configuration, LSM or KVS devices are "tables", but files are
	 * tested as well.
	 */
	if ((g.uri = malloc(256)) == NULL)
		syserr("malloc");
	strcpy(g.uri, DATASOURCE("file") ? "file:" : "table:");
	if (DATASOURCE("memrata"))
		strcat(g.uri, "dev1/");
	strcat(g.uri, WT_NAME);

	/* Default single-threaded 10% of the time. */
	cp = config_find("threads", strlen("threads"));
	if (!(cp->flags & C_PERM))
		*cp->v = MMRAND(1, 100) < 10 ? 1: CONF_RAND(cp);

	/* Fill in random values for the rest of the run. */
	for (cp = c; cp->name != NULL; ++cp) {
		if (cp->flags & (C_IGNORE | C_PERM | C_TEMP))
			continue;

		/*
		 * Boolean flags are 0 or 1, but only set N in 100 where the
		 * variable's min value is N.  Set the flag if we rolled >=
		 * the min, 0 otherwise.
		 */
		if (cp->flags & C_BOOL)
			*cp->v = MMRAND(1, 100) <= cp->min ? 1 : 0;
		else
			*cp->v = CONF_RAND(cp);
	}

	/* KVS requires shared libraries. */
	if (DATASOURCE("kvsbdb") && access(KVS_BDB_PATH, R_OK) != 0)
		die(errno, "kvsbdb shared library: %s", KVS_BDB_PATH);
	if (DATASOURCE("memrata") && access(MEMRATA_PATH, R_OK) != 0)
		die(errno, "memrata shared library: %s", MEMRATA_PATH);

	/* KVS doesn't support user-specified collations. */
	if (DATASOURCE("kvsbdb") || DATASOURCE("memrata"))
		g.c_reverse = 0;

	config_checksum();
	config_compression();

	/* Clear operations values if the whole run is read-only. */
	if (g.c_ops == 0)
		for (cp = c; cp->name != NULL; ++cp)
			if (cp->flags & C_OPS)
				*cp->v = 0;

	/*
	 * Periodically, set the delete percentage to 0 so salvage gets run,
	 * as long as the delete percentage isn't nailed down.
	 */
	if (!g.replay && g.run_cnt % 10 == 0) {
		cp = config_find("delete_pct", strlen("delete_pct"));
		if (cp->name != NULL &&
		    !(cp->flags & (C_IGNORE | C_PERM | C_TEMP)))
			g.c_delete_pct = 0;
	}

	/* If it's an LSM run, crank up the insert percentage. */
	if (DATASOURCE("lsm")) {
		cp = config_find("insert_pct", strlen("insert_pct"));
		if (cp->name != NULL &&
		    !(cp->flags & (C_IGNORE | C_PERM | C_TEMP)))
			g.c_insert_pct = MMRAND(50, 85);
	}

	/* Reset the key count. */
	g.key_cnt = 0;
}

/*
 * config_checksum --
 *	Checksum configuration.
 */
static void
config_checksum(void)
{
	CONFIG *cp;

	/* Choose a checksum mode if nothing was specified. */
	cp = config_find("checksum", strlen("checksum"));
	if (!(cp->flags & C_PERM))
		switch (MMRAND(1, 10)) {
		case 1:					/* 10% */
			config_single("checksum=on", 0);
			break;
		case 2:					/* 10% */
			config_single("checksum=off", 0);
			break;
		default:				/* 80% */
			config_single("checksum=uncompressed", 0);
			break;
		}
}

/*
 * config_compression --
 *	Compression configuration.
 */
static void
config_compression(void)
{
	CONFIG *cp;
	const char *cstr;

	/*
	 * Compression: choose something if compression wasn't specified,
	 * otherwise confirm the appropriate shared library is available.
	 * We don't include LZO in the test compression choices, we don't
	 * yet have an LZO module of our own.
	 */
	cp = config_find("compression", strlen("compression"));
	if (!(cp->flags & C_PERM)) {
		cstr = "compression=none";
		switch (MMRAND(1, 10)) {
		case 1:					/* 10% */
			break;
		case 2: case 3: case 4: case 5:		/* 40% */
			if (access(BZIP_PATH, R_OK) == 0)
				cstr = "compression=bzip";
			break;
		case 6:					/* 10% */
			if (access(BZIP_PATH, R_OK) == 0)
				cstr = "compression=raw";
			break;
		case 7: case 8: case 9: case 10:	/* 40% */
			if (access(SNAPPY_PATH, R_OK) == 0)
				cstr = "compression=snappy";
			break;
		}
		config_single(cstr, 0);
	}

	switch (g.c_compression_flag) {
	case COMPRESS_BZIP:
	case COMPRESS_RAW:
		if (access(BZIP_PATH, R_OK) != 0)
			die(0, "bzip library not found or not readable");
		break;
	case COMPRESS_LZO:
		if (access(LZO_PATH, R_OK) != 0)
			die(0, "LZO library not found or not readable");
		break;
	case COMPRESS_SNAPPY:
		if (access(SNAPPY_PATH, R_OK) != 0)
			die(0, "snappy library not found or not readable");
	}
}

/*
 * config_error --
 *	Display configuration information on error.
 */
void
config_error(void)
{
	CONFIG *cp;

	/* Display configuration names. */
	fprintf(stderr, "\n");
	fprintf(stderr, "Configuration names:\n");
	for (cp = c; cp->name != NULL; ++cp)
		if (strlen(cp->name) > 17)
			fprintf(stderr,
			    "%s\n%17s: %s\n", cp->name, " ", cp->desc);
		else
			fprintf(stderr, "%17s: %s\n", cp->name, cp->desc);
}

/*
 * config_print --
 *	Print configuration information.
 */
void
config_print(int error_display)
{
	CONFIG *cp;
	FILE *fp;

	if (error_display)
		fp = stdout;
	else
		if ((fp = fopen(g.home_run, "w")) == NULL)
			die(errno, "fopen: %s", g.home_run);

	fprintf(fp, "############################################\n");
	fprintf(fp, "#  RUN PARAMETERS\n");
	fprintf(fp, "############################################\n");

	/* Display configuration values. */
	for (cp = c; cp->name != NULL; ++cp)
		if ((cp->type_mask != 0 &&
		    ((g.type == FIX && !(cp->type_mask & C_FIX)) ||
		    (g.type == ROW && !(cp->type_mask & C_ROW)) ||
		    (g.type == VAR && !(cp->type_mask & C_VAR)))) ||
		    (cp->flags & C_STRING && *(cp->vstr) == NULL))
			fprintf(fp,
			    "# %s not applicable to this run\n", cp->name);
		else if (cp->flags & C_STRING)
			fprintf(fp, "%s=%s\n", cp->name, *cp->vstr);
		else
			fprintf(fp, "%s=%" PRIu32 "\n", cp->name, *cp->v);

	fprintf(fp, "############################################\n");
	if (fp != stdout)
		(void)fclose(fp);
}

/*
 * config_file --
 *	Read configuration values from a file.
 */
void
config_file(const char *name)
{
	FILE *fp;
	char *p, buf[256];

	if ((fp = fopen(name, "r")) == NULL)
		die(errno, "fopen: %s", name);
	while (fgets(buf, sizeof(buf), fp) != NULL) {
		for (p = buf; *p != '\0' && *p != '\n'; ++p)
			;
		*p = '\0';
		if (buf[0] == '\0' || buf[0] == '#')
			continue;
		config_single(buf, 1);
	}
	(void)fclose(fp);
}

/*
 * config_clear --
 *	Clear per-run values.
 */
void
config_clear(void)
{
	CONFIG *cp;

	/* Clear configuration data. */
	for (cp = c; cp->name != NULL; ++cp) {
		cp->flags &= ~(uint32_t)C_TEMP;
		if (!(cp->flags & C_PERM) &&
		    cp->flags & C_STRING && cp->vstr != NULL) {
			free(*cp->vstr);
			*cp->vstr = NULL;
		}
	}
	free(g.uri);
	g.uri = NULL;
}

/*
 * config_single --
 *	Set a single configuration structure value.
 */
void
config_single(const char *s, int perm)
{
	CONFIG *cp;
	int v;
	const char *ep;

	if ((ep = strchr(s, '=')) == NULL) {
		fprintf(stderr,
		    "%s: %s: illegal configuration value\n", g.progname, s);
		exit(EXIT_FAILURE);
	}

	cp = config_find(s, (size_t)(ep - s));
	cp->flags |= perm ? C_PERM : C_TEMP;
	++ep;

	if (cp->flags & C_STRING) {
		if (strncmp(s, "data_source", strlen("data_source")) == 0 &&
		    strncmp("file", ep, strlen("file")) != 0 &&
		    strncmp("kvsbdb", ep, strlen("kvsbdb")) != 0 &&
		    strncmp("lsm", ep, strlen("lsm")) != 0 &&
		    strncmp("memrata", ep, strlen("memrata")) != 0 &&
		    strncmp("table", ep, strlen("table")) != 0) {
			    fprintf(stderr,
				"Invalid data source option: %s\n", ep);
			    exit(EXIT_FAILURE);
		}

		if (strncmp(s, "checksum", strlen("checksum")) == 0) {
			config_map_checksum(ep, &g.c_checksum_flag);
			*cp->vstr = strdup(ep);
		} else if (strncmp(
		    s, "compression", strlen("compression")) == 0) {
			config_map_compression(ep, &g.c_compression_flag);
			*cp->vstr = strdup(ep);
		} else if (strncmp(s, "file_type", strlen("file_type")) == 0) {
			config_map_file_type(ep, &g.type);
			*cp->vstr = strdup(config_file_type(g.type));
		} else
			*cp->vstr = strdup(ep);
		if (*cp->vstr == NULL)
			syserr("malloc");

		return;
	}

	v = atoi(ep);
	if (cp->flags & C_BOOL) {
		if (v != 0 && v != 1) {
			fprintf(stderr, "%s: %s: value of boolean not 0 or 1\n",
			    g.progname, s);
			exit(EXIT_FAILURE);
		}
	} else if (v < 0 || (u_int)v < cp->min || (u_int)v > cp->maxset) {
		fprintf(stderr, "%s: %s: value of %" PRIu32
		    " outside min/max values of %" PRIu32 "-%" PRIu32 "\n",
		    g.progname, s, *cp->v, cp->min, cp->maxset);
		exit(EXIT_FAILURE);
	}
	*cp->v = (u_int)v;
}

/*
 * config_map_file_type --
 *	Map a file type configuration to a flag.
 */
static void
config_map_file_type(const char *s, u_int *vp)
{
	if (strcmp(s, "fix") == 0 ||
	    strcmp(s, "fixed-length column-store") == 0)
		*vp = FIX;
	else if (strcmp(s, "var") == 0 ||
	    strcmp(s, "variable-length column-store") == 0)
		*vp = VAR;
	else if (strcmp(s, "row") == 0 ||
	    strcmp(s, "row-store") == 0)
		*vp = ROW;
	else
		die(EINVAL, "illegal file type configuration: %s", s);
}

/*
 * config_map_checksum --
 *	Map a checksum configuration to a flag.
 */
static void
config_map_checksum(const char *s, u_int *vp)
{
	if (strcmp(s, "on") == 0)
		*vp = CHECKSUM_ON;
	else if (strcmp(s, "off") == 0)
		*vp = CHECKSUM_ON;
	else if (strcmp(s, "uncompressed") == 0)
		*vp = CHECKSUM_UNCOMPRESSED;
	else
		die(EINVAL, "illegal checksum configuration: %s", s);
}

/*
 * config_map_compression --
 *	Map a compression configuration to a flag.
 */
static void
config_map_compression(const char *s, u_int *vp)
{
	if (strcmp(s, "none") == 0)
		*vp = COMPRESS_NONE;
	else if (strcmp(s, "bzip") == 0)
		*vp = COMPRESS_BZIP;
	else if (strcmp(s, "lzo") == 0)
		*vp = COMPRESS_LZO;
	else if (strcmp(s, "raw") == 0)
		*vp = COMPRESS_RAW;
	else if (strcmp(s, "snappy") == 0)
		*vp = COMPRESS_SNAPPY;
	else
		die(EINVAL, "illegal compression configuration: %s", s);
}

/*
 * config_find
 *	Find a specific configuration entry.
 */
static CONFIG *
config_find(const char *s, size_t len)
{
	CONFIG *cp;

	for (cp = c; cp->name != NULL; ++cp) 
		if (strncmp(s, cp->name, len) == 0 && cp->name[len] == '\0')
			return (cp);

	fprintf(stderr,
	    "%s: %s: unknown configuration keyword\n", g.progname, s);
	config_error();
	exit(EXIT_FAILURE);
}

/*
 * config_find_is_perm
 *	Return if a specific configuration entry was permanently set.
 */
static int
config_find_is_perm(const char *s, size_t len)
{
	CONFIG *cp;

	cp = config_find(s, len);
	return (cp->flags & C_PERM ? 1 : 0);
}

/*
 * config_file_type --
 *	Return the file type as a string.
 */
static const char *
config_file_type(u_int type)
{
	switch (type) {
	case FIX:
		return ("fixed-length column-store");
	case VAR:
		return ("variable-length column-store");
	case ROW:
		return ("row-store");
	default:
		break;
	}
	return ("error: unknown file type");
}
