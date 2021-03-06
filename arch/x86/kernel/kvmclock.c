/*  KVM paravirtual clock driver. A clocksource implementation
    Copyright (C) 2008 Glauber de Oliveira Costa, Red Hat Inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <linux/clocksource.h>
#include <linux/kvm_para.h>
#include <asm/pvclock.h>
#include <asm/msr.h>
#include <asm/apic.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/memblock.h>
#include <linux/sched.h>

#include <asm/x86_init.h>
#include <asm/reboot.h>

static int kvmclock = 1;
static int msr_kvm_system_time = MSR_KVM_SYSTEM_TIME;
static int msr_kvm_wall_clock = MSR_KVM_WALL_CLOCK;
static cycle_t kvm_sched_clock_offset;

static int parse_no_kvmclock(char *arg)
{
	kvmclock = 0;
	return 0;
}
early_param("no-kvmclock", parse_no_kvmclock);

/* The hypervisor will put information about time periodically here */
static struct pvclock_vsyscall_time_info *hv_clock;
static struct pvclock_wall_clock wall_clock;

struct pvclock_vsyscall_time_info *pvclock_pvti_cpu0_va(void)
{
	return hv_clock;
}

/*
 * The wallclock is the time of day when we booted. Since then, some time may
 * have elapsed since the hypervisor wrote the data. So we try to account for
 * that with system time
 */
static void kvm_get_wallclock(struct timespec *now)
{
	struct pvclock_vcpu_time_info *vcpu_time;
	int low, high;
	int cpu;

	low = (int)__pa_symbol(&wall_clock);
	high = ((u64)__pa_symbol(&wall_clock) >> 32);

	// 将wall_clock的物理地址通过MSR注册到kvm中
	native_write_msr(msr_kvm_wall_clock, low, high);

	cpu = get_cpu();
	vcpu_time = &hv_clock[cpu].pvti;
	// 将kvm提供的guest在boot时的wall clock加上guest的boot time，
	// 得到当前的wall clock，写入now中
	pvclock_read_wallclock(&wall_clock, vcpu_time, now);

	put_cpu();
}

static int kvm_set_wallclock(const struct timespec *now)
{
	return -1;
}

// 读取当前的ns数，读到的值总是单调增的，即使pvclock是unstable的。
// 如果pvclock是unstable的，则会用一个last_value记录上次读到的值，
// 如果此次读到的值比last_value还小（时间跳回了过去），则返回last_value
static cycle_t kvm_clock_read(void)
{
	struct pvclock_vcpu_time_info *src;
	cycle_t ret;
	int cpu;

	preempt_disable_notrace();
	cpu = smp_processor_id();
	src = &hv_clock[cpu].pvti;
	ret = pvclock_clocksource_read(src);
	preempt_enable_notrace();
	return ret;
}

static cycle_t kvm_clock_get_cycles(struct clocksource *cs)
{
	return kvm_clock_read();
}

static cycle_t kvm_sched_clock_read(void)
{
	return kvm_clock_read() - kvm_sched_clock_offset;
}

// 将guest的sched_clock替换成kvmclock（默认是TSC）
static inline void kvm_sched_clock_init(bool stable)
{
	if (!stable) {
		pv_time_ops.sched_clock = kvm_clock_read;
		return;
	}

	// 让sched_clock从零开始计时，从而保证printk timestamps从零开始
	// 这淘汰了旧的PVCLOCK_COUNTS_FROM_ZERO特性
	kvm_sched_clock_offset = kvm_clock_read();
	pv_time_ops.sched_clock = kvm_sched_clock_read;
	set_sched_clock_stable();

	printk(KERN_INFO "kvm-clock: using sched offset of %llu cycles\n",
			kvm_sched_clock_offset);

	BUILD_BUG_ON(sizeof(kvm_sched_clock_offset) >
	         sizeof(((struct pvclock_vcpu_time_info *)NULL)->system_time));
}

/*
 * If we don't do that, there is the possibility that the guest
 * will calibrate under heavy load - thus, getting a lower lpj -
 * and execute the delays themselves without load. This is wrong,
 * because no delay loop can finish beforehand.
 * Any heuristics is subject to fail, because ultimately, a large
 * poll of guests can be running and trouble each other. So we preset
 * lpj here
 *
 * 直接通过pvclock获取guest的TSC频率，而不是让guest进行calibrate（那样不准确），公式如下
 * tsc_khz = ((10^6 << 32) / mult) >> shift
 */
static unsigned long kvm_get_tsc_khz(void)
{
	struct pvclock_vcpu_time_info *src;
	int cpu;
	unsigned long tsc_khz;

	cpu = get_cpu();
	src = &hv_clock[cpu].pvti;
	tsc_khz = pvclock_tsc_khz(src);
	put_cpu();
	return tsc_khz;
}

// 预设loops_per_jiffiy，免去guest进行calibrate的必要
static void kvm_get_preset_lpj(void)
{
	unsigned long khz;
	u64 lpj;

	khz = kvm_get_tsc_khz();

	lpj = ((u64)khz * 1000);
	do_div(lpj, HZ);
	preset_lpj = lpj;
}

bool kvm_check_and_clear_guest_paused(void)
{
	bool ret = false;
	struct pvclock_vcpu_time_info *src;
	int cpu = smp_processor_id();

	if (!hv_clock)
		return ret;

	src = &hv_clock[cpu].pvti;
	if ((src->flags & PVCLOCK_GUEST_STOPPED) != 0) {
		src->flags &= ~PVCLOCK_GUEST_STOPPED;
		pvclock_touch_watchdogs();
		ret = true;
	}

	return ret;
}

static struct clocksource kvm_clock = {
	.name = "kvm-clock",
	.read = kvm_clock_get_cycles,
	.rating = 400, // 将rating设置为400从而比系统中的其他clocksource优先级高，接管系统的时间读取
	.mask = CLOCKSOURCE_MASK(64),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

// 将pvclock_page的物理地址通过MSR注册到kvm中，此后kvm可以通过更新这块内存来告知guest真实时间
int kvm_register_clock(char *txt)
{
	int cpu = smp_processor_id();
	int low, high, ret;
	struct pvclock_vcpu_time_info *src;

	if (!hv_clock)
		return 0;

	src = &hv_clock[cpu].pvti;
	low = (int)slow_virt_to_phys(src) | 1;
	high = ((u64)slow_virt_to_phys(src) >> 32);
	ret = native_write_msr_safe(msr_kvm_system_time, low, high);
	printk(KERN_INFO "kvm-clock: cpu %d, msr %x:%x, %s\n",
	       cpu, high, low, txt);

	return ret;
}

static void kvm_save_sched_clock_state(void)
{
}

static void kvm_restore_sched_clock_state(void)
{
	kvm_register_clock("primary cpu clock, resume");
}

#ifdef CONFIG_X86_LOCAL_APIC
static void kvm_setup_secondary_clock(void)
{
	/*
	 * Now that the first cpu already had this clocksource initialized,
	 * we shouldn't fail.
	 */
	WARN_ON(kvm_register_clock("secondary cpu clock"));
}
#endif

/*
 * After the clock is registered, the host will keep writing to the
 * registered memory location. If the guest happens to shutdown, this memory
 * won't be valid. In cases like kexec, in which you install a new kernel, this
 * means a random memory location will be kept being written. So before any
 * kind of shutdown from our side, we unregister the clock by writing anything
 * that does not have the 'enable' bit set in the msr
 */
#ifdef CONFIG_KEXEC_CORE
static void kvm_crash_shutdown(struct pt_regs *regs)
{
	native_write_msr(msr_kvm_system_time, 0, 0);
	kvm_disable_steal_time();
	native_machine_crash_shutdown(regs);
}
#endif

static void kvm_shutdown(void)
{
	native_write_msr(msr_kvm_system_time, 0, 0);
	kvm_disable_steal_time();
	native_machine_shutdown();
}

void __init kvmclock_init(void)
{
	struct pvclock_vcpu_time_info *vcpu_time;
	unsigned long mem;
	int size, cpu;
	u8 flags;

	size = PAGE_ALIGN(sizeof(struct pvclock_vsyscall_time_info)*NR_CPUS);

	if (!kvm_para_available())
		return;

	// 这两个MSR用于告知kvm是否启用pvclock，以及将pvclock的信息放在guest中的哪个物理页上
	if (kvmclock && kvm_para_has_feature(KVM_FEATURE_CLOCKSOURCE2)) {
		msr_kvm_system_time = MSR_KVM_SYSTEM_TIME_NEW;
		msr_kvm_wall_clock = MSR_KVM_WALL_CLOCK_NEW;
	} else if (!(kvmclock && kvm_para_has_feature(KVM_FEATURE_CLOCKSOURCE)))
		return;

	printk(KERN_INFO "kvm-clock: Using msrs %x and %x",
		msr_kvm_system_time, msr_kvm_wall_clock);

	// 申请pvclock_page的内存，其中存储hv_clock数组，每个CPU有一个struct pvclock_vcpu_time_info
	mem = memblock_alloc(size, PAGE_SIZE);
	if (!mem)
		return;
	hv_clock = __va(mem);
	memset(hv_clock, 0, size);

	// 将pvclock_page的物理地址注册到kvm中，此后kvm可以通过这块内存告知guest时间
	if (kvm_register_clock("primary cpu clock")) {
		hv_clock = NULL;
		memblock_free(mem, size);
		return;
	}

	// Stable TSC指的是各vCPU间的TSC是同步的（这同时也需要Host TSC是同步的）
	if (kvm_para_has_feature(KVM_FEATURE_CLOCKSOURCE_STABLE_BIT))
		pvclock_set_flags(PVCLOCK_TSC_STABLE_BIT);

	cpu = get_cpu();
	vcpu_time = &hv_clock[cpu].pvti;
	flags = pvclock_read_flags(vcpu_time);

	// 注册sched_clock，用kvmclock替代默认的TSC实现
	kvm_sched_clock_init(flags & PVCLOCK_TSC_STABLE_BIT);
	put_cpu();

	x86_platform.calibrate_tsc = kvm_get_tsc_khz;
	x86_platform.calibrate_cpu = kvm_get_tsc_khz;
	x86_platform.get_wallclock = kvm_get_wallclock;
	x86_platform.set_wallclock = kvm_set_wallclock;
#ifdef CONFIG_X86_LOCAL_APIC
	x86_cpuinit.early_percpu_clock_init =
		kvm_setup_secondary_clock;
#endif
	x86_platform.save_sched_clock_state = kvm_save_sched_clock_state;
	x86_platform.restore_sched_clock_state = kvm_restore_sched_clock_state;
	machine_ops.shutdown  = kvm_shutdown;
#ifdef CONFIG_KEXEC_CORE
	machine_ops.crash_shutdown  = kvm_crash_shutdown;
#endif
	kvm_get_preset_lpj();
	// 将kvmclock的频率设置为10^9，这样其读到的cycles其实就是纳秒数
	clocksource_register_hz(&kvm_clock, NSEC_PER_SEC);
	pv_info.name = "KVM";
}

// 在kvmclock_init之后被调用，如果Host的TSC和Guest的virtual TSC都已经同步，
// 则Host会将kvmclock设置位masterclock模式，并通过PVCLOCK_TSC_STABLE_BIT
// 这一flag通知Guest。在masterclock模式下，Guest会将kvmclock的clocksource
// 设置为PVCLOCK模式，使gettimeofday()等系统调用通过vDSO方式调用，
// 直接读取vCPU 0的pvti
int __init kvm_setup_vsyscall_timeinfo(void)
{
#ifdef CONFIG_X86_64
	int cpu;
	u8 flags;
	struct pvclock_vcpu_time_info *vcpu_time;
	unsigned int size;

	if (!hv_clock)
		return 0;

	size = PAGE_ALIGN(sizeof(struct pvclock_vsyscall_time_info)*NR_CPUS);

	cpu = get_cpu();

	vcpu_time = &hv_clock[cpu].pvti;
	flags = pvclock_read_flags(vcpu_time);

	if (!(flags & PVCLOCK_TSC_STABLE_BIT)) {
		put_cpu();
		return 1;
	}

	put_cpu();

	kvm_clock.archdata.vclock_mode = VCLOCK_PVCLOCK;
#endif
	return 0;
}
