#include "cache.h"
#include "archive.h"
#include "run-command.h"

struct tar_filter *tar_filters;
static struct tar_filter **tar_filters_tail = &tar_filters;

static struct tar_filter *tar_filter_new(const char *name, int namelen)
{
	struct tar_filter *tf;
	tf = xcalloc(1, sizeof(*tf));
	tf->name = xmemdupz(name, namelen);
	tf->extensions.strdup_strings = 1;
	*tar_filters_tail = tf;
	tar_filters_tail = &tf->next;
	return tf;
}

static void tar_filter_free(struct tar_filter *tf)
{
	string_list_clear(&tf->extensions, 0);
	free(tf->name);
	free(tf->command);
	free(tf);
}

static struct tar_filter *tar_filter_by_namelen(const char *name,
						int len)
{
	struct tar_filter *p;
	for (p = tar_filters; p; p = p->next)
		if (!strncmp(p->name, name, len) && !p->name[len])
			return p;
	return NULL;
}

struct tar_filter *tar_filter_by_name(const char *name)
{
	return tar_filter_by_namelen(name, strlen(name));
}

static int match_extension(const char *filename, const char *ext)
{
	int prefixlen = strlen(filename) - strlen(ext);

	/*
	 * We need 1 character for the '.', and 1 character to ensure that the
	 * prefix is non-empty (i.e., we don't match ".tar.gz" with no actual
	 * filename).
	 */
	if (prefixlen < 2 || filename[prefixlen-1] != '.')
		return 0;
	return !strcmp(filename + prefixlen, ext);
}

struct tar_filter *tar_filter_by_extension(const char *filename)
{
	struct tar_filter *p;

	for (p = tar_filters; p; p = p->next) {
		int i;
		for (i = 0; i < p->extensions.nr; i++) {
			const char *ext = p->extensions.items[i].string;
			if (match_extension(filename, ext))
				return p;
		}
	}
	return NULL;
}

static int tar_filter_config(const char *var, const char *value, void *data)
{
	struct tar_filter *tf;
	const char *dot;
	const char *name;
	const char *type;
	int namelen;

	if (prefixcmp(var, "tarfilter."))
		return 0;
	dot = strrchr(var, '.');
	if (dot == var + 9)
		return 0;

	name = var + 10;
	namelen = dot - name;
	type = dot + 1;

	tf = tar_filter_by_namelen(name, namelen);
	if (!tf)
		tf = tar_filter_new(name, namelen);

	if (!strcmp(type, "command")) {
		if (!value)
			return config_error_nonbool(var);
		tf->command = xstrdup(value);
		return 0;
	}
	else if (!strcmp(type, "extension")) {
		if (!value)
			return config_error_nonbool(var);
		string_list_append(&tf->extensions, value);
		return 0;
	}
	else if (!strcmp(type, "compressionlevels")) {
		tf->use_compression = git_config_bool(var, value);
		return 0;
	}

	return 0;
}

static void remove_filters_without_command(void)
{
	struct tar_filter *p = tar_filters;
	struct tar_filter **last = &tar_filters;

	while (p) {
		if (p->command && *p->command)
			last = &p->next;
		else {
			*last = p->next;
			tar_filter_free(p);
		}
		p = *last;
	}
}

static void load_builtin_filters(void)
{
	struct tar_filter *tf;

	tf = tar_filter_new("tgz", strlen("tgz"));
	tf->command = xstrdup("gzip -n");
	string_list_append(&tf->extensions, "tgz");
	string_list_append(&tf->extensions, "tar.gz");
	tf->use_compression = 1;
}

/*
 * We don't want to load twice, since some of our
 * values actually append rather than overwrite.
 */
static int tar_filter_config_loaded;
extern void tar_filter_load_config(void)
{
	if (tar_filter_config_loaded)
		return;
	tar_filter_config_loaded = 1;

	load_builtin_filters();
	git_config(tar_filter_config, NULL);
	remove_filters_without_command();
}

static int write_tar_to_filter(struct archiver_args *args, const char *cmd)
{
	struct child_process filter;
	const char *argv[2];
	int r;

	memset(&filter, 0, sizeof(filter));
	argv[0] = cmd;
	argv[1] = NULL;
	filter.argv = argv;
	filter.use_shell = 1;
	filter.in = -1;

	if (start_command(&filter) < 0)
		die_errno("unable to start '%s' filter", argv[0]);
	close(1);
	if (dup2(filter.in, 1) < 0)
		die_errno("unable to redirect descriptor");
	close(filter.in);

	r = write_tar_archive(args);

	close(1);
	if (finish_command(&filter) != 0)
		die("'%s' filter reported error", argv[0]);

	return r;
}

int write_tar_filter_archive(struct archiver_args *args)
{
	struct strbuf cmd = STRBUF_INIT;
	int r;

	if (!args->tar_filter)
		die("BUG: tar-filter archiver called with no filter defined");

	strbuf_addstr(&cmd, args->tar_filter->command);
	if (args->tar_filter->use_compression && args->compression_level >= 0)
		strbuf_addf(&cmd, " -%d", args->compression_level);

	r = write_tar_to_filter(args, cmd.buf);

	strbuf_release(&cmd);
	return r;
}
