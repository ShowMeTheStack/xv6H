/* `lkvm revise --name VM <fact> [value]`: revise a fact of a running machine. */
#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-revise.h>
#include <kvm/parse-options.h>
#include <kvm/kvm.h>
#include <kvm/kvm-ipc.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *instance_name;

static const char * const revise_usage[] = {
	"lkvm revise [--all] [-n name] <fact> [value]",
	NULL
};

static const struct option revise_options[] = {
	OPT_GROUP("General options:"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
	OPT_END()
};

void kvm_revise_help(void)
{
	usage_with_options(revise_usage, revise_options);
}

int kvm_cmd_revise(int argc, const char **argv, const char *prefix)
{
	char req[256] = "", reply[256] = "";
	int fd, r;

	argc = parse_options(argc, argv, revise_options, revise_usage, PARSE_OPT_STOP_AT_NON_OPTION);
	if (argc < 1 || !instance_name)
		kvm_revise_help();
	snprintf(req, sizeof(req), "%s %s", argv[0], argc > 1 ? argv[1] : "");
	fd = kvm__get_sock_by_instance(instance_name);
	if (fd < 0)
		die("revise: no instance %s", instance_name);
	r = kvm_ipc__send_msg(fd, KVM_IPC_REVISE, strlen(req) + 1, (u8 *)req);
	if (r < 0)
		die("revise: cannot send");
	r = read(fd, reply, sizeof(reply) - 1);
	if (r > 0)
		printf("%s\n", reply);
	close(fd);
	return 0;
}
