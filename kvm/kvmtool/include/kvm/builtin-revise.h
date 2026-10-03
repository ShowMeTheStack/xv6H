#ifndef KVM__REVISE_H
#define KVM__REVISE_H
#include <kvm/util.h>
int kvm_cmd_revise(int argc, const char **argv, const char *prefix);
void kvm_revise_help(void) NORETURN;
#endif
