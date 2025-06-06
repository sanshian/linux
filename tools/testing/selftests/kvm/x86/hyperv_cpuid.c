// SPDX-License-Identifier: GPL-2.0
/*
 * Test for x86 KVM_CAP_HYPERV_CPUID
 *
 * Copyright (C) 2018, Red Hat, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 *
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"
#include "vmx.h"

static void guest_code(void)
{
}

static void test_hv_cpuid(struct kvm_vcpu *vcpu, bool evmcs_expected)
{
	const bool has_irqchip = !vcpu || vcpu->vm->has_irqchip;
	const struct kvm_cpuid2 *hv_cpuid_entries;
	int i;
	int nent_expected = 10;
	u32 test_val;

	if (vcpu)
		hv_cpuid_entries = vcpu_get_supported_hv_cpuid(vcpu);
	else
		hv_cpuid_entries = kvm_get_supported_hv_cpuid();

	TEST_ASSERT(hv_cpuid_entries->nent == nent_expected,
		    "KVM_GET_SUPPORTED_HV_CPUID should return %d entries"
		    " (returned %d)",
		    nent_expected, hv_cpuid_entries->nent);

	for (i = 0; i < hv_cpuid_entries->nent; i++) {
		const struct kvm_cpuid_entry2 *entry = &hv_cpuid_entries->entries[i];

		TEST_ASSERT((entry->function >= 0x40000000) &&
			    (entry->function <= 0x40000082),
			    "function %x is our of supported range",
			    entry->function);

		TEST_ASSERT(entry->index == 0,
			    ".index field should be zero");

		TEST_ASSERT(entry->flags == 0,
			    ".flags field should be zero");

		TEST_ASSERT(!entry->padding[0] && !entry->padding[1] &&
			    !entry->padding[2], "padding should be zero");

		switch (entry->function) {
		case 0x40000000:
			test_val = 0x40000082;

			TEST_ASSERT(entry->eax == test_val,
				    "Wrong max leaf report in 0x40000000.EAX: %x"
				    " (evmcs=%d)",
				    entry->eax, evmcs_expected
				);
			break;
		case 0x40000003:
			TEST_ASSERT(has_irqchip || !(entry->edx & BIT(19)),
				    "\"Direct\" Synthetic Timers should require in-kernel APIC");
			break;
		case 0x40000004:
			test_val = entry->eax & (1UL << 18);

			TEST_ASSERT(!!test_val == !is_smt_possible(),
				    "NoNonArchitecturalCoreSharing bit"
				    " doesn't reflect SMT setting");

			TEST_ASSERT(has_irqchip || !(entry->eax & BIT(10)),
				    "Cluster IPI (i.e. SEND_IPI) should require in-kernel APIC");
			break;
		case 0x4000000A:
			TEST_ASSERT(entry->eax & (1UL << 19),
				    "Enlightened MSR-Bitmap should always be supported"
				    " 0x40000000.EAX: %x", entry->eax);
			if (evmcs_expected)
				TEST_ASSERT((entry->eax & 0xffff) == 0x101,
				    "Supported Enlightened VMCS version range is supposed to be 1:1"
				    " 0x40000000.EAX: %x", entry->eax);

			break;
		default:
			break;

		}
		/*
		 * If needed for debug:
		 * fprintf(stdout,
		 *	"CPUID%lx EAX=0x%lx EBX=0x%lx ECX=0x%lx EDX=0x%lx\n",
		 *	entry->function, entry->eax, entry->ebx, entry->ecx,
		 *	entry->edx);
		 */
	}

	/*
	 * Note, the CPUID array returned by the system-scoped helper is a one-
	 * time allocation, i.e. must not be freed.
	 */
	if (vcpu)
		free((void *)hv_cpuid_entries);
}

static void test_hv_cpuid_e2big(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
	static struct kvm_cpuid2 cpuid = {.nent = 0};
	int ret;

	if (vcpu)
		ret = __vcpu_ioctl(vcpu, KVM_GET_SUPPORTED_HV_CPUID, &cpuid);
	else
		ret = __kvm_ioctl(vm->kvm_fd, KVM_GET_SUPPORTED_HV_CPUID, &cpuid);

	TEST_ASSERT(ret == -1 && errno == E2BIG,
		    "%s KVM_GET_SUPPORTED_HV_CPUID didn't fail with -E2BIG when"
		    " it should have: %d %d", !vcpu ? "KVM" : "vCPU", ret, errno);
}

int main(int argc, char *argv[])
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_HYPERV_CPUID));

	/* Test the vCPU ioctl without an in-kernel local APIC. */
	vm = vm_create_barebones();
	vcpu = __vm_vcpu_add(vm, 0);
	test_hv_cpuid(vcpu, false);
	kvm_vm_free(vm);

	/* Test vCPU ioctl version */
	vm = vm_create_with_one_vcpu(&vcpu, guest_code);
	test_hv_cpuid_e2big(vm, vcpu);
	test_hv_cpuid(vcpu, false);

	if (!kvm_cpu_has(X86_FEATURE_VMX) ||
	    !kvm_has_cap(KVM_CAP_HYPERV_ENLIGHTENED_VMCS)) {
		print_skip("Enlightened VMCS is unsupported");
		goto do_sys;
	}
	vcpu_enable_evmcs(vcpu);
	test_hv_cpuid(vcpu, true);

do_sys:
	/* Test system ioctl version */
	if (!kvm_has_cap(KVM_CAP_SYS_HYPERV_CPUID)) {
		print_skip("KVM_CAP_SYS_HYPERV_CPUID not supported");
		goto out;
	}

	test_hv_cpuid_e2big(vm, NULL);
	test_hv_cpuid(NULL, kvm_cpu_has(X86_FEATURE_VMX));

out:
	kvm_vm_free(vm);

	return 0;
}
