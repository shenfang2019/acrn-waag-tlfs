#include "processor.h"
#include "libcflat.h"
#include "vm.h"
#include "alloc_page.h"
#include "smp.h"
#include "desc.h"
#include "isr.h"
#include "apic.h"

/* Hyper-V MSR numbers */
#define HV_X64_MSR_GUEST_OS_ID		0x40000000U
#define HV_X64_MSR_HYPERCALL		0x40000001U
#define HV_X64_MSR_VP_INDEX		0x40000002U

#define HV_X64_MSR_TIME_REF_COUNT	0x40000020U
#define HV_X64_MSR_REFERENCE_TSC	0x40000021U
#define HV_X64_MSR_TSC_FREQUENCY	0x40000022U

/* Partition Reference Counter (HV_X64_MSR_TIME_REF_COUNT) */
#define CPUID3A_TIME_REF_COUNT_MSR	(1U << 1U)
/* Hypercall MSRs (HV_X64_MSR_GUEST_OS_ID and HV_X64_MSR_HYPERCALL) */
#define CPUID3A_HYPERCALL_MSR		(1U << 5U)
/* Access virtual processor index MSR (HV_X64_MSR_VP_INDEX) */
#define CPUID3A_VP_INDEX_MSR		(1U << 6U)
/* Partition reference TSC MSR (HV_X64_MSR_REFERENCE_TSC) */
#define CPUID3A_REFERENCE_TSC_MSR	(1U << 9U)

/**/
/********************************************/
/*          timer calibration  */
/********************************************/
uint64_t tsc_hz;
uint64_t apic_timer_hz;
uint64_t cpu_hz;
uint64_t bus_hz;
struct hv_reference_tsc_page {
	volatile u32 tsc_sequence;
	u32 res1;
	volatile uint64_t tsc_scale;
	volatile int64_t tsc_offset;
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

struct hv_reference_tsc_page *hv_clock;
static void *hypercall_page;

#define HV_HYPERCALL_FAST			(1u << 16)
#define HV_X64_MSR_HYPERCALL_ENABLE 0x1
#define TICKS_PER_SEC_RTSC_COUNT (1000000000 / 100)

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
	u32 tmp;
	u64 ctl = code;
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
#ifdef __x86_64__
	asm volatile("mov %%edx,%0":"=r"(tmp)::"edx");
	if((arg & 0xffffffff) != tmp)
		ret = -1;
#endif
	return ret;
}

#define HV_STATUS_INVALID_HYPERCALL_CODE 0x2
void hvcall_test()
{
#define HVCALL_SIGNAL_EVENT                     0x5d
	u64 ret;
	setup_hypercall();
	ret = do_hypercall(HVCALL_SIGNAL_EVENT,2,1);
/*
*up to now ,we do not support any hypercall,
*just return HV_STATUS_INVALID_HYPERCALL_CODE
*/
	report("hypercall test", ret == HV_STATUS_INVALID_HYPERCALL_CODE);
	teardown_hypercall();
}

/*A hypervisor conformant with the Microsoft hypervisor interface
	will set CPUID.1:ECX [bit 31] = 1
	to indicate its presence to software*/
static void check_presence_hv()
{
	report("TC_TLFS_MinimalSet_001 detect the Presence of a Hypervisor", cpuid(1).c & (1 << 31));
}
static void check_cpuid_range()
{
	u32 a;
	/*The maximum input value for hypervisor CPUID information.
		On Microsoft hypervisors, this will be at least 0x40000005.*/
	a = cpuid(0x40000000).a;
	report("TC_TLFS_MinimalSet_002 check Hypervisor CPUID Leaf Range",	\
		(a > 0x40000005)||(a == 0x40000005));
	printf("cpuid0x40000000.a=0x%x\n\r",a);//someone else ask me add here.
}
static void check_hv_identity()
{
	/*0x40000001 EAX 0x31237648---Hv#1
	Hypervisors conforming to the Hv#1 interface*/
	report("TC_TLFS_MinimalSet_003 check Hypervisor Vendor-Neutral Interface Identification",	\
		cpuid(0x40000001).a == 0x31237648);
}
static void check_hv_systemID()
{
	/*This value will be zero until the OS identity MSR is set
	(see section 2.6); after that, it has the following definitions:*/
	struct cpuid cpuid401;

	cpuid401 = cpuid(0x40000002);
	report("TC_TLFS_MinimalSet_004 check check Hypervisor System Identity",	\
		(cpuid401.a == 0) && (cpuid401.b == 0) \
		&& (cpuid401.c == 0) && (cpuid401.d == 0));
	printf("cpuid0x40000002.a=0x%x b=%x c=%x d=%x\n\r",	\
		cpuid401.a,cpuid401.b,cpuid401.c,cpuid401.d);//someone else ask me add here.
}
static void check_hv_feature()
{
	/*CPUID.40000003:EAX and EBX indicate partition privileges and access to virtual MSRs.
	Conforming hypervisors must implement EAX and EBX as defined below. A
	conforming hypervisor returning any non-zero value in 0x40000003.EAX or EBX must
	support the corresponding functionality as defined in the TLFS.*/
	struct cpuid cpuid03;
	u32 supported_hv;

	cpuid03 = cpuid(0x40000003);
	supported_hv = CPUID3A_TIME_REF_COUNT_MSR + CPUID3A_HYPERCALL_MSR +	\
		 CPUID3A_VP_INDEX_MSR + CPUID3A_REFERENCE_TSC_MSR;

	report("TC_TLFS_MinimalSet_005 check Hypervisor feature identification", \
		(cpuid03.a == supported_hv) && (cpuid03.b == 0)	\
		&& (cpuid03.c == 0 ) && (cpuid03.d == 0));
	printf("cpuid0x40000003.a=0x%x b=%x c=%x d=%x\n\r",	\
		cpuid03.a,cpuid03.b,cpuid03.c,cpuid03.d);//someone else ask me add here.
}
static void check_recommend_implement()
{
	struct cpuid cpuid04;
	cpuid04 = cpuid(0x40000004);
	/*These are enlightenment bits which indicate to the guest OS kernel
	which hypercalls are recommended, in addition to other information.*/
	report("TC_TLFS_MinimalSet_006 check Recommended Implementation", \
		(cpuid04.a == 0) && (cpuid04.b == 0) \
		&& (cpuid04.c == 0) && (cpuid04.d == 0));
	printf("cpuid0x40000004.a=0x%x b=%x c=%x d=%x\n\r",	\
		cpuid04.a,cpuid04.b,cpuid04.c,cpuid04.d);//someone else ask me add here.
}
static void check_implement_limit()
{
	struct cpuid cpuid05;

	/*EAX:The maximum number of virtual processors supported
	*EBX:The maximum number of logical processors supported
	*maybe,it will change in future
	*/
	cpuid05 = cpuid(0x40000005);
	report("TC_TLFS_MinimalSet_007 check Implementation limits", \
		(cpuid05.a == 8) && (cpuid05.b == 0));
	printf("cpuid0x40000005.a=0x%x b=%x\n\r",	\
		cpuid05.a,cpuid05.b);//someone else ask me add here.
}
static void check_hw_feature()
{
	/*untile now, we do not implement hardware feature
	*
	*/
	report("TC_TLFS_MinimalSet_008 check Implementation hardware features",	\
		cpuid(0x40000006).a == 0);
	printf("cpuid0x40000006.a=0x%x \n\r",cpuid(0x40000006).a);//someone else ask me add here.
}
static void x64_msr_gust_osid()
{
	u64 guestid = (0x8f00ull << 48);
	report("TC_TLFS_MinimalSet_009 check HV_X64_MSR_GUEST_OS_ID initial value",	\
		rdmsr(HV_X64_MSR_GUEST_OS_ID) == 0);

	wrmsr(HV_X64_MSR_GUEST_OS_ID, guestid);

	report("TC_TLFS_MinimalSet_010 check writing MSR HV_X64_MSR_GUEST_OS_ID", \
		rdmsr(HV_X64_MSR_GUEST_OS_ID) == guestid);
	/*recover the msr value */
	wrmsr(HV_X64_MSR_GUEST_OS_ID, 0);
}
static void x64_msr_hvcall()
{
	/*refer Hypervisor Top Level Functional Specification
	2.6 Reporting the Guest OS Identity*/
	u64 guestid = (0x8f00ull << 48);

	report("TC_TLFS_MinimalSet_011 check HV_X64_MSR_HYPERCALL initial value", \
		rdmsr(HV_X64_MSR_HYPERCALL) == 0);

	hypercall_page = alloc_page();
	if (!hypercall_page)
		report_abort("failed to allocate hypercall page");
	memset(hypercall_page, 0, PAGE_SIZE);

	wrmsr(HV_X64_MSR_GUEST_OS_ID, 0);
	wrmsr(HV_X64_MSR_HYPERCALL,
	      (u64)virt_to_phys(hypercall_page) | HV_X64_MSR_HYPERCALL_ENABLE);

	report("TC_TLFS_MinimalSet_012 check writing HV_X64_MSR_HYPERCALL MSR \
while HV_X64_MSR_GUEST_OS_ID MSR is 0", rdmsr(HV_X64_MSR_HYPERCALL) == 0);

	wrmsr(HV_X64_MSR_GUEST_OS_ID, guestid);
	wrmsr(HV_X64_MSR_HYPERCALL,
	      (u64)virt_to_phys(hypercall_page) | HV_X64_MSR_HYPERCALL_ENABLE);

	report("TC_TLFS_MinimalSet_013 check writing HV_X64_MSR_HYPERCALL MSR \
while HV_X64_MSR_GUEST_OS_ID MSR is set",
		rdmsr(HV_X64_MSR_HYPERCALL) == \
		((u64)virt_to_phys(hypercall_page) | HV_X64_MSR_HYPERCALL_ENABLE));

	wrmsr(HV_X64_MSR_GUEST_OS_ID, 0);
	report("TC_TLFS_MinimalSet_014 check clear HV_X64_MSR_GUEST_OS_ID to disable hypercall page", \
		(rdmsr(HV_X64_MSR_HYPERCALL) & HV_X64_MSR_HYPERCALL_ENABLE) == 0);

	teardown_hypercall();

}
static void check_hvcall()
{

	/*refer Hypervisor Top Level Functional Specification
	chap 3. Hypercall Interface*/
#define HVCALL_SIGNAL_EVENT                     0x5d
	u64 ret;

	setup_hypercall();
	ret = do_hypercall(HVCALL_SIGNAL_EVENT,2,1);
	report("TC_TLFS_MinimalSet_015 test Hypercall interface ",	\
		ret == HV_STATUS_INVALID_HYPERCALL_CODE);

	teardown_hypercall();
}
static void check_vp_index()
{
	/*unitl now, we just support one cpu for waag, maybe ,
	this will change in future, if we support more than one cpu,
	we need rdmsr for each cpu*/
	u64 vp_index;
	unsigned char vector = 0;

	vp_index = rdmsr(HV_X64_MSR_VP_INDEX);
	report("TC_TLFS_MinimalSet_016 check HV_X64_MSR_VP_INDEX MSR", \
		vp_index == 0);

	asm volatile(ASM_TRY("1f")
		"wrmsr \n\t" "1:"
		::"a"(vp_index+1),"d"(0),"c"(HV_X64_MSR_VP_INDEX):"memory");

	vector = exception_vector();

	report("TC_TLFS_MinimalSet_017 check wirting HV_X64_MSR_VP_INDEX", \
		vector == GP_VECTOR);
}
static void check_iTSC_support()
{
	/*A partition reference time enlightenment,
	based on the host platform’s support for
	an Invariant Time Stamp Counter (iTSC). TLFS chap 12.1*/
	report("TC_TLFS_TSC_WaaG_001 check iTSC support", \
		cpuid(0x80000007).d & (1 << 8));

}
/*
*
*this function need be called as early as posible
*we suppose the boot time for acrn-unit-test is less than 2s,
*
*/
static void ref_count_msr_init()
{
#define MAX_TIME_REF_COUNT_INIT (2*1000*1000*10) /*2s*/
	u64 value;
	value = rdmsr(HV_X64_MSR_TIME_REF_COUNT);

	/*Bits Description Attributes
	63:0 Count—Partition’s reference counter value in 100 nanosecond units
	Read-only
	When a partition is created, the value of the TIME_REF_COUNT MSR is set to 0x0000000000000000.
	This value cannot be modified by a virtual processor. Any attempt to write to it results in a #GP fault.
	*/
	report("TC_TLFS_TSC_WaaG_002 check HV_X64_MSR_TIME_REF_COUNT MSR initial value", \
		value < MAX_TIME_REF_COUNT_INIT);
	report("TC_TLFS_TSC_WaaG_006 check HV_X64_MSR_TIME_REF_COUNT reset after UOS reboot", \
		value < MAX_TIME_REF_COUNT_INIT);
}
/*simple sleep for xxx ns*/
static void sleep_ns(u64 ns)
{
#define TSC_TICKS_PER_NS (tsc_hz / 1000000000)
	u64 tsc;

	tsc = rdtsc();
	while(1){
		asm volatile("nop");
		if ((tsc + ns * TSC_TICKS_PER_NS) < rdtsc())
			break;
	}
}

static void x64_msr_ref_count()
{
	unsigned char vector = 0;
	u64 msr_value = 0;
	u64 msr_last = 0;
	bool ok = true;

	asm volatile(ASM_TRY("1f")
		"wrmsr \n\t" "1:"
		::"a"(1),"d"(0),"c"(HV_X64_MSR_TIME_REF_COUNT):"memory");

	vector = exception_vector();
	report("TC_TLFS_TSC_WaaG_003 writing HV_X64_MSR_TIME_REF_COUNT MSR expect #GP", \
		vector == GP_VECTOR);

	msr_last = msr_value = rdmsr(HV_X64_MSR_TIME_REF_COUNT);
	for (char i = 0; i < 20; i++){
		sleep_ns(100);
		msr_value = rdmsr(HV_X64_MSR_TIME_REF_COUNT);
		if (!(msr_value > msr_last))
		 	ok = false;
		msr_last = msr_value;
	}
	report("TC_TLFS_TSC_WaaG_004 reference count is monotonically increasing", ok);
}

#define TSC_DEADLINE_TIMER_VECTOR 0xef
bool volatile tdt_isr = false;

static void tsc_deadline_timer_isr(isr_regs_t *regs)
{
    tdt_isr = true;
    eoi();
}

static bool __test_tsc_deadline_timer(void)
{
	u64 ticks1,ticks2,tsc;
	/*
	*the delta should be less than 0.01ms according to the test case design;
	*0.01ms need 100 time_ref_count's ticks
	*/
	u64 delta_ticks = 100;
	bool ret = true;

	/*
	*we start 1s tsc deadline timer to calculate time reference count MSR
	*/
	tsc = rdmsr(MSR_IA32_TSC);
	tsc += tsc_hz;
	ticks1 = rdmsr(HV_X64_MSR_TIME_REF_COUNT);
	wrmsr(MSR_IA32_TSCDEADLINE, tsc);

	while(!tdt_isr);

	ticks2 = rdmsr(HV_X64_MSR_TIME_REF_COUNT);
	tdt_isr = false;

	if ((TICKS_PER_SEC_RTSC_COUNT + delta_ticks) < (ticks2 - ticks1)
		|| (TICKS_PER_SEC_RTSC_COUNT - delta_ticks) > (ticks2 -ticks1))
		ret = false;

	return ret;
}

static int enable_tsc_deadline_timer(void)
{
	u32 lvtt;

	if (cpuid(1).c & (1 << 24)) {
		lvtt = APIC_LVT_TIMER_TSCDEADLINE | TSC_DEADLINE_TIMER_VECTOR;
		apic_write(APIC_LVTT, lvtt);
		return 1;
	} else {
		return 0;
	}
}

static void check_rtsc_freq(void)
{
	bool ret = true;

	printf("\tStart TC_TLFS_TSC_WaaG_005,it will take about 10s,pls waiting... ...\n\r");

	if(enable_tsc_deadline_timer()) {
		handle_irq(TSC_DEADLINE_TIMER_VECTOR, tsc_deadline_timer_isr);
		irq_enable();
		for (char i=0; i<10; i++) {
			if ( __test_tsc_deadline_timer() != true) {
				ret = false;
				break;
			}
		}
		report("TC_TLFS_TSC_WaaG_005 reference count tsc increases 1 with 100ns as a unit", ret);
    } else {
        report_skip("skip TC_TLFS_TSC_WaaG_005 reference count increases 1 with 100ns as a unit");
    }
}

static void x64_msr_ref_tsc()
{
	u64 tsc,disc,tsc_scale,tsc_offset,tick;
	struct hv_reference_tsc_page * hv_rtsc_page;

	report("TC_TLFS_TSC_WaaG_009 check HV_X64_MSR_REFERENCE_TSC MSR initial value", \
		rdmsr(HV_X64_MSR_REFERENCE_TSC) == 0);

	hv_rtsc_page = alloc_page();

	if (!hv_rtsc_page)
		report_abort("failed to allocate hypercall page");

	wrmsr(HV_X64_MSR_REFERENCE_TSC, (u64)(uintptr_t)hv_rtsc_page | 1);
	report("TC_TLFS_TSC_WaaG_010 check writing HV_X64_MSR_REFERENCE_TSC MSR",
	       rdmsr(HV_X64_MSR_REFERENCE_TSC) == ((u64)(uintptr_t)hv_rtsc_page | 1));

	if (hv_rtsc_page->tsc_sequence == 0 || hv_rtsc_page->tsc_sequence == 0xFFFFFFFF) {
		printf("Reference TSC page not available\n");
		exit(1);
	}

	tsc_scale = hv_rtsc_page->tsc_scale;
	tsc_offset = hv_rtsc_page->tsc_offset;
	tsc = rdtsc();
	/* ret = ((tsc * tsc_scale) >> 64) + tsc_offset */
	asm volatile ("mulq %3" :
		"=d" (tick), "=a" (disc) :
		"a" (tsc), "r" (tsc_scale));
	tick += tsc_offset;
	disc = rdmsr(HV_X64_MSR_TIME_REF_COUNT);
	/*we suppose the time is small,we just check it will be less than 1us*/
	report("TC_TLFS_TSC_WaaG_011 reference TSC is synchronous with reference count",
		(tick + 10) > disc);
	free_page(hv_rtsc_page);
}
void main()
{
	ref_count_msr_init();/*this case need be called firstly*/

	setup_vm();
	setup_idt();
	tsc_calibrate();

	check_presence_hv();
	check_cpuid_range();
	check_hv_identity();
	check_hv_systemID();
	check_hv_feature();
	check_recommend_implement();
	check_implement_limit();
	check_hw_feature();
	x64_msr_gust_osid();
	x64_msr_hvcall();
	check_hvcall();
	check_vp_index();
	check_iTSC_support();
	x64_msr_ref_count();
	check_rtsc_freq();
	x64_msr_ref_tsc();
	report_summary();
}
