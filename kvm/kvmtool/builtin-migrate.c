/* `lkvm migrate -n VM --to ADDR:PORT`: move a running machine to a kvmtool started with --incoming. */
#include <kvm/util.h>
#include <kvm/kvm-cmd.h>
#include <kvm/builtin-migrate.h>
#include <kvm/parse-options.h>
#include <kvm/kvm.h>
#include <kvm/kvm-ipc.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *instance_name, *to;

static const char * const migrate_usage[] = {
	"lkvm migrate -n name --to ADDR:PORT",
	NULL
};

static const struct option migrate_options[] = {
	OPT_GROUP("General options:"),
	OPT_STRING('n', "name", &instance_name, "name", "Instance name"),
	OPT_STRING('\0', "to", &to, "ADDR:PORT", "The destination (lkvm run --incoming PORT)"),
	OPT_END()
};

void kvm_migrate_help(void)
{
	usage_with_options(migrate_usage, migrate_options);
}

int kvm_cmd_migrate(int argc, const char **argv, const char *prefix)
{
	char reply[512] = "";
	int fd, r;

	parse_options(argc, argv, migrate_options, migrate_usage, 0);
	if (!instance_name || !to)
		kvm_migrate_help();
	fd = kvm__get_sock_by_instance(instance_name);
	if (fd < 0)
		die("no instance %s", instance_name);
	r = kvm_ipc__send_msg(fd, KVM_IPC_MIGRATE, strlen(to) + 1, (u8 *)to);
	if (r < 0)
		die("migrate: no answer from %s", instance_name);
	r = read(fd, reply, sizeof(reply) - 1);
	close(fd);
	printf("%s\n", r > 0 ? reply : "migrate: no reply");
	return strncmp(reply, "migrate: sent", 13) ? 1 : 0;
}
