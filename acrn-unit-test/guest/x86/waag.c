#include "processor.h"
#include "libcflat.h"
#include "vm.h"
#include "alloc_page.h"
#include "smp.h"

/* Hyper-V MSR numbers */
#define HV_X64_MSR_GUEST_OS_ID		0x40000000U
#define HV_X64_MSR_HYPERCALL		0x40000001U
#define HV_X64_MSR_VP_INDEX		0x40000002U

#define HV_X64_MSR_TIME_REF_COUNT	0x40000020U
#define HV_X64_MSR_REFERENCE_TSC	0x40000021U

#define HV_X64_MSR_TSC_FREQUENCY	0x40000022U
/**/
/********************************************/
/*          timer calibration  */
/********************************************/
uint64_t tsc_hz;
uint64_t apic_timer_hz;
uint64_t cpu_hz;
uint64_t bus_hz;
struct hv_reference_tsc_page {
        u32 tsc_sequence;
        u32 res1;
        uint64_t tsc_scale;
        int64_t tsc_offset;
};

static void tsc_calibrate(void)
{
	u32 eax_denominator, ebx_numerator, ecx_crystal_hz, reserved;
	u32 eax_base_mhz = 0, ebx_max_mhz = 0, ecx_bus_mhz = 0, edx;

	__asm volatile("cpuid"
	    :"=a"(eax_denominator), "=b"(ebx_numerator), "=c"(ecx_crystal_hz), "=d"(reserved)
	    : "0" (0x15), "2" (0)
	    : "memory");

	printf("crystal_hz:%u\n\r",ecx_crystal_hz);
	
	if (ecx_crystal_hz != 0) {
		tsc_hz = ((uint64_t) ecx_crystal_hz *
		        ebx_numerator) / eax_denominator;
		apic_timer_hz = ecx_crystal_hz;
	} else {

		__asm volatile("cpuid"
		    :"=a"(eax_base_mhz), "=b"(ebx_max_mhz), "=c"(ecx_bus_mhz), "=d"(edx)
		    : "0" (0x16), "2" (0)
		    : "memory");

		tsc_hz = (uint64_t) eax_base_mhz * 1000000U;
		apic_timer_hz = tsc_hz * eax_denominator / ebx_numerator;
	}

	cpu_hz = eax_base_mhz * 1000000U;
	bus_hz = ecx_bus_mhz * 1000000U;
	printf("apic_timer_hz: %lu\n", apic_timer_hz);
	printf("tsc_hz: %lu\n", tsc_hz);
	printf("cpu_hz: %lu\n", cpu_hz);
	printf("bus_hz: %lu\n", bus_hz);

	/* apic_timer_hz: 23863636; 16C 2154H
	 * tsc_hz: 2100000000
	 * cpu_hz: 2100000000
	 * bus_hz: 100000000
	 */
}

bool smp_done = false;
u64 vp_index = 0;
int vp_id = 0;
struct hv_reference_tsc_page *hv_clock;
static void *hypercall_page;
#define HV_HYPERCALL_FAST			(1u << 16)
#define HV_X64_MSR_HYPERCALL_ENABLE 0x1

static void setup_hypercall(void)
{
	u64 guestid = (0x8f00ull << 48);

	hypercall_page = alloc_page();
	if (!hypercall_page)
		report_abort("failed to allocate hypercall page");
	memset(hypercall_page, 0, PAGE_SIZE);

	wrmsr(HV_X64_MSR_GUEST_OS_ID, guestid);

	wrmsr(HV_X64_MSR_HYPERCALL,
	      (u64)virt_to_phys(hypercall_page) | HV_X64_MSR_HYPERCALL_ENABLE);
}

static void teardown_hypercall(void)
{
	wrmsr(HV_X64_MSR_HYPERCALL, 0);
	wrmsr(HV_X64_MSR_GUEST_OS_ID, 0);
	free_page(hypercall_page);
}


static u64 do_hypercall(u16 code, u64 arg, bool fast)
{
	u64 ret;
	u64 tmp;
	u64 ctl = code;
	printf("arg:%lx ",arg);
	if (fast)
		ctl |= HV_HYPERCALL_FAST;

	asm volatile ("call *%[hcall_page]"
#ifdef __x86_64__
		      "\n mov $0,%%r8"
		      : "=a"(ret)
		      : "c"(ctl), "d"(arg),
#else
		      : "=A"(ret)
		      : "A"(ctl),
			"b" ((u32)(arg >> 32)), "c" ((u32)arg),
			"D"(0), "S"(0),
#endif
		      [hcall_page] "m" (hypercall_page)
#ifdef __x86_64__
		      : "r8"
#endif
		     );
	asm volatile("mov %%rdx,%0":"=r"(tmp)::"rdx"); 
	printf("tmp:0x%lx,arg:0x%lx \n\r",tmp,arg);
	return ret;
}

void get_vp_index()
{
	vp_index = rdmsr(HV_X64_MSR_VP_INDEX);
	vp_id = smp_id();
	smp_done = true;
}
#define HV_STATUS_INVALID_HYPERCALL_CODE 0x2
void hvcall_test()
{
#define HVCALL_SIGNAL_EVENT                     0x5d
	u64 ret;
	//u64 tmp;
	setup_hypercall();
	//asm volatile ("mov %rdx,%0":=(tmp)::"rdx");
	ret = do_hypercall(HVCALL_SIGNAL_EVENT,2,1);
	printf("do_hypercall ret:0x%lx\n\r",ret);
	report("hypercall test", ret == HV_STATUS_INVALID_HYPERCALL_CODE);
	teardown_hypercall();
}

void main()
{
	//u32 a,b,c,d;
	struct cpuid cpuid1 = {0,0,0,0};
	struct cpuid cpuid40000000 = {0,0,0,0};
	struct cpuid cpuid40000001 = {0,0,0,0};
	struct cpuid cpuid02 = {0,0,0,0};
	struct cpuid cpuid03 = {0,0,0,0};
	struct cpuid cpuid04 = {0,0,0,0};
	struct cpuid cpuid05 = {0,0,0,0};
	struct cpuid cpuid06 = {0,0,0,0};
	struct cpuid cpuid8007 = {0,0,0,0};
	
	u64 guest_os_id = 0;
	u64 msr_hypercall = 0;
	u64 msr_vpindex = 0;
	u64 msr_time_ref_cnt = 0;
	u64 msr_ref_tsc = 0;
	
	//u64 tsc_freguency = 0;
	msr_time_ref_cnt = rdmsr(HV_X64_MSR_TIME_REF_COUNT);
	
	setup_vm();
	
	cpuid1 = cpuid(1);
	cpuid40000001 = cpuid(0x40000001);
	cpuid40000000 = cpuid(0x40000000);
	cpuid02 = cpuid(0x40000002);
	cpuid03 = cpuid(0x40000003);
	cpuid04 = cpuid(0x40000004);
	cpuid05 = cpuid(0x40000005);
	cpuid06 = cpuid(0x40000006);
	cpuid8007 = cpuid(0x80000007);
	
	printf("\n\rcpuid 1 a:%x b:%x c:%x d:%x\n\r",cpuid1.a,cpuid1.b,cpuid1.c,cpuid1.d);
	printf("cpuid 40000000 a:%x b:%x c:%x d:%x\n\r",cpuid40000000.a,cpuid40000000.b,cpuid40000000.c,cpuid40000000.d);
	printf("cpuid 40000001 a:%x b:%x c:%x d:%x\n\r",cpuid40000001.a,cpuid40000001.b,cpuid40000001.c,cpuid40000001.d);
	printf("cpuid 40000002 a:%x b:%x c:%x d:%x\n\r",cpuid02.a,cpuid02.b,cpuid02.c,cpuid02.d);
	printf("cpuid 40000003 a:%x b:%x c:%x d:%x\n\r",cpuid03.a,cpuid03.b,cpuid03.c,cpuid03.d);
	printf("cpuid 40000004 a:%x b:%x c:%x d:%x\n\r",cpuid04.a,cpuid04.b,cpuid04.c,cpuid04.d);
	printf("cpuid 40000005 a:%x b:%x c:%x d:%x\n\r",cpuid05.a,cpuid05.b,cpuid05.c,cpuid05.d);
	printf("cpuid 40000006 a:%x b:%x c:%x d:%x\n\r",cpuid06.a,cpuid06.b,cpuid06.c,cpuid06.d);
	printf("cpuid 80000007 a:%x b:%x c:%x d:%x\n\r",cpuid8007.a,cpuid8007.b,cpuid8007.c,cpuid8007.d);
	
	guest_os_id = rdmsr(HV_X64_MSR_GUEST_OS_ID);
	msr_hypercall = rdmsr(HV_X64_MSR_HYPERCALL);
	msr_vpindex = rdmsr(HV_X64_MSR_VP_INDEX);
	//msr_time_ref_cnt = rdmsr(HV_X64_MSR_TIME_REF_COUNT);
	msr_ref_tsc = rdmsr(HV_X64_MSR_REFERENCE_TSC);
	//tsc_freguency = rdmsr(HV_X64_MSR_TSC_FREQUENCY);
	
	printf("read HV_X64_MSR_GUEST_OS_ID :0x%lx\n\r",guest_os_id);
	printf("read HV_X64_MSR_HYPERCALL :0x%lx\n\r",msr_hypercall);
	printf("read HV_X64_MSR_VP_INDEX :0x%lx\n\r",msr_vpindex);
	printf("read HV_X64_MSR_TIME_REF_COUNT :0x%lx\n\r",msr_time_ref_cnt);
	printf("read HV_X64_MSR_REFERENCE_TSC :0x%lx\n\r",msr_ref_tsc);
	
	//printf("HV_X64_MSR_TSC_FREQUENCY :0x%lx \n\r",tsc_freguency);
	printf("write 0xff to HV_X64_MSR_REFERENCE_TSC");
	wrmsr(HV_X64_MSR_REFERENCE_TSC,0xff);
	msr_ref_tsc = rdmsr(HV_X64_MSR_REFERENCE_TSC);
	printf("read HV_X64_MSR_REFERENCE_TSC :0x%lx\n\r",msr_ref_tsc);
	
	msr_time_ref_cnt = rdmsr(HV_X64_MSR_TIME_REF_COUNT);
	printf("write 0 to HV_X64_MSR_TIME_REF_COUNT\n\r");
	wrmsr(HV_X64_MSR_TIME_REF_COUNT,0);
	msr_time_ref_cnt = rdmsr(HV_X64_MSR_TIME_REF_COUNT);
	printf("read HV_X64_MSR_TIME_REF_COUNT :0x%lx\n\r",msr_time_ref_cnt);
	
	u32 ecx = 0;
	printf("tsc: 0x%llx, tscp: 0x%llx\n\r",rdtsc(),rdtscp(&ecx));

	tsc_calibrate();

	hv_clock = alloc_page();
	
	//struct hv_reference_tsc_page shadow;
	wrmsr(HV_X64_MSR_REFERENCE_TSC, (u64)(uintptr_t)hv_clock | 1);
	report("MSR value after enabling",
	       rdmsr(HV_X64_MSR_REFERENCE_TSC) == ((u64)(uintptr_t)hv_clock | 1));

	//hvclock_get_time_values(&shadow, hv_clock);
	if (hv_clock->tsc_sequence == 0 || hv_clock->tsc_sequence == 0xFFFFFFFF) {
		printf("Reference TSC page not available\n");
		exit(1);
	}

	printf("scale: %" PRIx64" offset: %" PRId64"\n", hv_clock->tsc_scale, hv_clock->tsc_offset);
	printf("smp_id:%d,vp_index :0x%lx\n\r",vp_id,vp_index);
	hvcall_test();
	//asm volatile ("hlt");
	
}


