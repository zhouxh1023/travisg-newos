#include <vcpu.h>
#include <vcpu_struct.h>
#include <stage2.h>
#include <serial.h>
#include <string.h>
#include <sh4.h>

#define CHATTY_TLB 0

extern vcpu_struct kernel_struct;

unsigned int next_utlb_ent = 0;

unsigned int vector_base();
unsigned int boot_stack[256] = { 0, };

void vcpu_clear_all_itlb_entries();
void vcpu_clear_all_utlb_entries();
void vcpu_dump_utlb_entry(int ent);

static int default_vector(unsigned int ex_code, unsigned int pc, unsigned int trap)
{
	dprintf("default_vector: ex_code 0x%x, pc 0x%x, trap 0x%x\r\n", ex_code, pc, trap);
	dprintf("spinning forever\r\n");
	for(;;);
	return 0;
}

unsigned int get_sr();
void set_sr(unsigned int sr);
unsigned int get_vbr();
void set_vbr(unsigned int vbr);
asm("
.globl _get_sr,_set_sr
.globl _get_vbr,_set_vbr

_get_sr:
        stc     sr,r0
        rts
        nop

_set_sr:
        ldc     r4,sr
        rts
        nop

_get_vbr:
        stc     vbr,r0
        rts
        nop

_set_vbr:
        ldc     r4,vbr
        rts
        nop
");

int vcpu_init(kernel_args *ka)
{
	int i;
	unsigned int sr;
	unsigned int vbr;
	
	dprintf("vcpu_init: entry\r\n");

	memset(&kernel_struct, 0, sizeof(kernel_struct));
	for(i=0; i<256; i++) {
		kernel_struct.vt[i].func = &default_vector;
	}
	kernel_struct.kstack = (unsigned int *)((int)boot_stack + sizeof(boot_stack) - 4);

	// set the vbr
	vbr = (unsigned int)&vector_base;
	set_vbr(vbr);
	dprintf("vbr = 0x%x\n", get_vbr());

	// disable exceptions
	sr = get_sr();
	sr |= 0x10000000;
	set_sr(sr);

	if((sr & 0x20000000) != 0) {
		// we're using register bank 1 now
		dprintf("using bank 1, switching register banks\r\n");
		// this switches in the bottom 8 registers.
		// dont have to do anything more, since the bottom 8 are
		// not saved in the call.
		set_sr(sr & 0xdfffffff);
	}

	// enable exceptions
	sr = get_sr();
	sr &= 0xefffffff;
	set_sr(sr);
	
	ka->vcpu = &kernel_struct;

	// enable the mmu
	vcpu_clear_all_itlb_entries();
	vcpu_clear_all_utlb_entries();
	*(int *)PTEH = 0;
	*(int *)MMUCR = 0x00000105;

	return 0;
}

static struct ptent *get_ptent(struct pdent *pd, unsigned int fault_address)
{
	struct ptent *pt;

#if CHATTY_TLB
	dprintf("get_ptent: fault_address 0x%x\r\n", fault_address);
#endif

	if((unsigned int)pd < P1_PHYS_MEM_START || (unsigned int)pd >= P1_PHYS_MEM_END) {
#if CHATTY_TLB
		dprintf("get_ptent: bad pdent 0x%x\r\n", pd);
#endif
		return 0;
	}

	if(pd[fault_address >> 22].v != 0) {
		pt = (struct ptent *)PHYS_ADDR_TO_P1(pd[fault_address >> 22].ppn << 12);
	} else {
		pt = 0;
	}
#if CHATTY_TLB
	dprintf("get_ptent: found ptent 0x%x\r\n", pt);
#endif

	return &pt[(fault_address >> 12) & 0x00000fff];
}

static void tlb_map(unsigned int vpn, struct ptent *ptent, unsigned int tlb_ent, unsigned int asid)
{
	union {
		struct utlb_data data;
		unsigned int n[3];
	} u;

	ptent->tlb_ent = tlb_ent;
	
	u.n[0] = 0;
	u.data.a.asid = asid;
	u.data.a.vpn = vpn << 2;
	u.data.a.dirty = ptent->d;
	u.data.a.valid = 1;
	
	u.n[1] = 0;
	u.data.da1.ppn = ptent->ppn << 2;
	u.data.da1.valid = 1;
	u.data.da1.psize1 = (ptent->sz & 0x2) ? 1 : 0;
	u.data.da1.prot_key = ptent->pr;
	u.data.da1.psize0 = ptent->sz & 0x1;
	u.data.da1.cacheability = ptent->c;
	u.data.da1.dirty = ptent->d;
	u.data.da1.sh = ptent->sh;
	u.data.da1.wt = ptent->wt;
	
	u.n[2] = 0;

	*((unsigned int *)(UTLB | (next_utlb_ent << UTLB_ADDR_SHIFT))) = u.n[0];
	*((unsigned int *)(UTLB1 | (next_utlb_ent << UTLB_ADDR_SHIFT))) = u.n[1];
	*((unsigned int *)(UTLB2 | (next_utlb_ent << UTLB_ADDR_SHIFT))) = u.n[2];
}

unsigned int tlb_miss(unsigned int excode, unsigned int pc)
{
	struct pdent *pd;
	struct ptent *ent;
	unsigned int fault_addr = *(unsigned int *)TEA; 
	unsigned int shifted_fault_addr;
	unsigned int asid;

#if CHATTY_TLB
	dprintf("tlb_miss: excode 0x%x, pc 0x%x, fault_address 0x%x\r\n", excode, pc, fault_addr);
#endif
	
	if(fault_addr >= P1_AREA) {
		pd = (struct pdent *)kernel_struct.kernel_pgdir;
		asid = kernel_struct.kernel_asid;
		shifted_fault_addr = fault_addr & 0x7fffffff;
	} else {
		pd = (struct pdent *)kernel_struct.user_pgdir;
		asid = kernel_struct.user_asid;
		shifted_fault_addr = fault_addr;
	}	

	ent = get_ptent(pd, shifted_fault_addr);
	if(ent == NULL || ent->v == 0) {
		return EXCEPTION_PAGE_FAULT;
	}	

#if CHATTY_TLB
	dprintf("found entry. vaddr 0x%x maps to paddr 0x%x\r\n",
		fault_addr, ent->ppn << 12);
#endif

	if(excode == 0x3) {
		// this is a tlb miss because of a write, so 
		// go ahead and mark it dirty
		ent->d = 1;
	}

	tlb_map(fault_addr >> 12, ent, next_utlb_ent, asid);
#if CHATTY_TLB
	vcpu_dump_utlb_entry(next_utlb_ent);
#endif
	next_utlb_ent++;
	if(next_utlb_ent >= UTLB_COUNT)
		next_utlb_ent = 0;
	
	return excode;
}

unsigned int tlb_initial_page_write(unsigned int excode, unsigned int pc)
{
	struct pdent *pd;
	struct ptent *ent;
	unsigned int fault_addr = *(unsigned int *)TEA; 
	unsigned int shifted_fault_addr;
	unsigned int asid;

#if CHATTY_TLB
	dprintf("tlb_initial_page_write: excode 0x%x, pc 0x%x, fault_address 0x%x\r\n", 
		excode, pc, fault_addr);
#endif

	if(fault_addr >= P1_AREA) {
		pd = (struct pdent *)kernel_struct.kernel_pgdir;
		asid = kernel_struct.kernel_asid;
		shifted_fault_addr = fault_addr & 0x7fffffff;
	} else {
		pd = (struct pdent *)kernel_struct.user_pgdir;
		asid = kernel_struct.user_asid;
		shifted_fault_addr = fault_addr;
	}	

	ent = get_ptent(pd, shifted_fault_addr);
	if(ent == NULL || ent->v == 0) {
		// if we're here, the page table is
		// out of sync with the tlb cache. 
		// time to die.
		dprintf("tlb_ipw exception called but no page table ent exists!\r\n");
		for(;;);
	}	

	{
		struct utlb_addr_array   *a;
		struct utlb_data_array_1 *da1;

		a = (struct utlb_addr_array *)(UTLB | (ent->tlb_ent << UTLB_ADDR_SHIFT));
		da1 = (struct utlb_data_array_1 *)(UTLB1 | (ent->tlb_ent << UTLB_ADDR_SHIFT));

		// inspect this tlb entry to make sure it's the right one
		if(asid != a->asid || (ent->ppn << 2) != da1->ppn || ((fault_addr >> 12) << 2) != a->vpn) {
			dprintf("tlb_ipw exception found that the page table out of sync with tlb\r\n");
			dprintf("page_table entry: 0x%x\r\n", *(unsigned int *)ent);
			vcpu_dump_utlb_entry(ent->tlb_ent);
			for(;;);
		}
	 	a->dirty = 1;
		ent->d = 1;
	}

	return excode;
}

void vcpu_dump_itlb_entry(int ent)
{
	struct itlb_data data;

	*(int *)&data.a = *((int *)(ITLB | (ent << ITLB_ADDR_SHIFT)));
	*(int *)&data.da1 = *((int *)(ITLB1 | (ent << ITLB_ADDR_SHIFT)));
	*(int *)&data.da2 = *((int *)(ITLB2 | (ent << ITLB_ADDR_SHIFT)));
	
	dprintf("itlb[%d] = \r\n", ent);
	dprintf(" asid = %d\r\n", data.a.asid);
	dprintf(" valid = %d\r\n", data.a.valid);
	dprintf(" vpn = 0x%x\r\n", data.a.vpn << 10);
	dprintf(" ppn = 0x%x\r\n", data.da1.ppn << 10);
}

void vcpu_clear_all_itlb_entries()
{	
	int i;
	for(i=0; i<4; i++) {
		*((int *)(ITLB | (i << ITLB_ADDR_SHIFT))) = 0;
		*((int *)(ITLB1 | (i << ITLB_ADDR_SHIFT))) = 0;
		*((int *)(ITLB2 | (i << ITLB_ADDR_SHIFT))) = 0;
	}
}

void vcpu_dump_all_itlb_entries()
{
	int i;

	for(i=0; i<4; i++) {
		vcpu_dump_itlb_entry(i);
	}
}

void vcpu_dump_utlb_entry(int ent)
{
	struct utlb_data data;

	*(int *)&data.a = *((int *)(UTLB | (ent << UTLB_ADDR_SHIFT)));
	*(int *)&data.da1 = *((int *)(UTLB1 | (ent << UTLB_ADDR_SHIFT)));
	*(int *)&data.da2 = *((int *)(UTLB2 | (ent << UTLB_ADDR_SHIFT)));
	
	dprintf("utlb[%d] = \r\n", ent);
	dprintf(" asid = %d\r\n", data.a.asid);
	dprintf(" valid = %d\r\n", data.a.valid);
	dprintf(" dirty = %d\r\n", data.a.dirty);
	dprintf(" vpn = 0x%x\r\n", data.a.vpn << 10);
	dprintf(" ppn = 0x%x\r\n", data.da1.ppn << 10);
}

void vcpu_clear_all_utlb_entries()
{	
	int i;
	for(i=0; i<64; i++) {
		*((int *)(UTLB | (i << UTLB_ADDR_SHIFT))) = 0;
		*((int *)(UTLB1 | (i << UTLB_ADDR_SHIFT))) = 0;
		*((int *)(UTLB2 | (i << UTLB_ADDR_SHIFT))) = 0;
	}
}
		
void vcpu_dump_all_utlb_entries()
{
	int i;

	for(i=0; i<4; i++) {
		vcpu_dump_utlb_entry(i);
	}
}
