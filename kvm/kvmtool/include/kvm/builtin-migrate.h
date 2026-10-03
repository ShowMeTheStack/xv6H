#ifndef KVM__MIGRATE_BUILTIN_H
#define KVM__MIGRATE_BUILTIN_H

int kvm_cmd_migrate(int argc, const char **argv, const char *prefix);
void kvm_migrate_help(void) NORETURN;

#endif
