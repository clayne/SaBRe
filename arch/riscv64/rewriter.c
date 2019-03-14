#include "config.h"

#include "loader/global_vars.h"
#include "loader/rewriter.h"

#include "handle_rdtsc.h"
#include "handle_syscall.h"
#include "handle_syscall_loader.h"
#include "handle_vdso.h"
#include "rewriter_tools.h"

#include "riscv_utils.h"
#include "riscv_decoder.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char *stub =
		"\xff\x01\x01\x13" // addi sp, sp, -16
		"\x00\x51\x30\x23" // sd t0, 0(sp)
		"\x00\x11\x34\x23" // sd ra, 8(sp)
		"\x00\x00\x02\x93" // li t0, 0
		"\x00\x82\x92\x93" // slli t0, t0, 0x8
		"\x00\x02\x82\x93" // addi t0, t0, ...
		"\x00\x82\x92\x93" // slli t0, t0, 0x8
		"\x00\x02\x82\x93" // addi t0, t0, ...
		"\x00\x82\x92\x93" // slli t0, t0, 0x8
		"\x00\x02\x82\x93" // addi t0, t0, ...
		"\x00\x82\x92\x93" // slli t0, t0, 0x8
		"\x00\x02\x82\x93" // addi t0, t0, ...
		"\x00\x82\x92\x93" // slli t0, t0, 0x8
		"\x00\x02\x82\x93" // addi t0, t0, ...
		"\x00\x82\x92\x93" // slli t0, t0, 0x8
		"\x00\x02\x82\x93" // addi t0, t0, ...
		"\x00\x82\x92\x93" // slli t0, t0, 0x8
		"\x00\x02\x82\x93" // addi t0, t0, ...
		"\x00\x02\x80\xe7" // jalr t0
		"\x00\x01\x32\x83" // ld t0, 0(sp)
		"\x00\x81\x30\x83" // ld ra, 8(sp)
		"\x01\x01\x01\x13" // addi sp, sp, 16
		;

static const size_t stub_n = 22;
static const uint32_t needed_scratch_space = 22 * 4 + 4;

static char patched_stub_loader[120];
static char patched_stub[120];

static void prepare_patched_stub(char *patched_stub, uint64_t handle_syscall_addr) {
	  memcpy(patched_stub, stub, stub_n * 4);

	  char *start = patched_stub + 12;
	  uint64_t mask = 0xff00000000000000;
	  for (int i = 0; i < 8; i++) {
		uint8_t val = (handle_syscall_addr & mask) >> ((7 - i)*2*4);
		char b1 = (val & 0xf0) >> 4;
		char b2 = val & 0xf;
		b2 <<= 4;
		b2 |= *(start+1);
		*start = b1;
		*(start + 1) = b2;
		mask >>= 8;
		start += 8;
	  }
}

//static void debug_print_block2(uint16_t *start, uint16_t *end, bool debug) {
//  if (debug) {
//	printf("----------------------------------------\n");
//	int i = 0;
//	for (; i < 8 && start <= end; i++) {
//	  printf("0x%04x ", __bswap_16(*start));
//	  if (__bswap_16(*start) == 0xaa87) {
//		printf("ADDRESS IS 0x%016lx", start);
//	  }
//	  start++;
//	  if (i + 1 >= 8) {
//		i = -1;
//		printf("\n");
//	  }
//	}
//	printf("\n");
//	printf("----------------------------------------\n");
//  }
//}
//
//
//static void debug_print_block(uint16_t *start, uint16_t *end) {
//  if (1) {
//	printf("----------------------------------------\n");
//	int i = 0;
//	for (; i < 8 && start <= end; i++) {
//	  printf("0x%04x ", __bswap_16(*start));
//	  if (__bswap_16(*start) == 0xaa87) {
//		printf("ADDRESS IS 0x%016lx", start);
//	  }
//	  start++;
//	  if (i + 1 >= 8) {
//		i = -1;
//		printf("\n");
//	  }
//	}
//	printf("\n");
//	printf("----------------------------------------\n");
//  }
//}

// if nearest, return the first depreated_reg that is found
// else return after certain range
static uint32_t forward_search_deprecated_reg(char *curr, 
	char *end, 
	int range,
	uint32_t depre_mask,
	uint32_t depen_mask,
	bool nearest) {
  struct rb_root branch_targets = RB_ROOT;

  int depre_reg = 0;

  uint32_t inst = 0;
  char * addr = 0;
  int len;

  char *ptr = curr;
  for (int i = 0; ptr < end && i < range; i++) {
	addr = ptr;
	inst = next_inst_riscv(&ptr);
	int len = (inst & 0x3) == 0x3 ? 4 : 2;
	//printf("current inst : 0x%08x\n", inst);
	struct branch_target *target =
	  rb_upper_bound_target(&branch_targets, addr);
	if (target && target->addr < ptr) {
	  break;
	}

	depre_mask |= deprecated_reg(inst);
	depen_mask |= dependency_reg(inst);

	if (depre_mask ^ depen_mask == depen_mask) {
	  if (nearest) {
		return depre_mask;
	  }
	} 

	depre_mask = depre_mask & (~depen_mask);
	if (is_control_flow_inst(inst)) {
	  return depre_mask;
	}
  }
  return 0;
}


void print_regs(uint32_t regs) {
  int reg = 0;
  for (int i = 0; i < 32; i++) {
	if ((regs & 0x1) == 1) {
	  printf("reg : %d\n", reg);
	}
	reg++;
	regs >>= 1;
  }
}

static void library_patch_syscalls_in_func(struct library *lib,
  	                                       const struct maps *maps  __attribute__ ((unused)),
	                                       int vsys_offset,
	                                       char *start,
	                                       char *end,
	                                       char **extra_space,
	                                       int *extra_len,
	                                       bool loader) {
  printf("pathc func start\n");
  struct rb_root branch_targets = RB_ROOT;

  // Count how many targets we'll need
  unsigned long total = 0;
  for (char *ptr = start; ptr < end;) {
	if (is_control_flow_inst(next_inst_riscv(&ptr)))
	  total++;
  }

  _nx_debug_printf("total number of jump instructions found %d\n", total);

  // Allocate all the memory we'll need in one go
  struct branch_target *target = malloc(total*sizeof(*target));

  // Lookup branch targets dynamically
  for (char *ptr = start; ptr < end;) {
	char *addr = ptr;
	uint32_t insn = next_inst_riscv(&ptr);

	if (is_control_flow_inst(insn))
	  addr += inst_offset64(insn);
	else 
	  continue;

	target->addr = addr;
	rb_insert_target(&branch_targets, addr, &target->rb_target);
	target += 1;
  }

  struct code {
	char *addr;
	uint32_t insn;
	uint32_t len;
  } code[9] = {{0}};

  int i = 0;

  for (char *ptr = start; ptr < end;) {
	// Keep a ring-buffer of the last few instructions in order to find the correct place to patch the code
	code[i].addr = ptr;
	code[i].insn = next_inst_riscv(&ptr);
	code[i].len = ((code[i].insn & 0x3) == 0x3) ? 4 : 2;

	if (code[i].insn == 0x73) { // system call

	  char *dest = maps_alloc_near_range(maps,
		                                 code[i].addr + 4,
		                                 needed_scratch_space,
		                                 PROT_EXEC | PROT_READ | PROT_WRITE,
		                                 true,
		                                 0xfffff); /* alloc within range of 20 bit */

	  size_t patch_length = 4;

	  if (dest == NULL) {
		dest = maps_alloc_near_range(maps,
			                         code[i].addr + 4,
		                             needed_scratch_space,
			                         PROT_EXEC | PROT_READ | PROT_WRITE,
			                         true,
			                         0xffffffff); /* alloc within range of 32 bit */ 
		if (dest == NULL) {
		  *((uint32_t *)code[i].addr) = 0;
		  goto replaced;
		}
		patch_length = 16;
	  }

	  char *scratch_start = dest;

	  if (patch_length == 4) {
		{
		  char *copy_start = loader ? patched_stub_loader : patched_stub;
		  for (int i = 0; i < stub_n; i++) {
			*((uint32_t *)dest) = __bswap_32(*((uint32_t *)copy_start));
			copy_start += 4;
			dest += 4;
		  }
		}
		// calculate the return address of the next inst after ecall
		// patch the jal to the next inst after ecall to the end of stub
  
		char *return_jal_addr = scratch_start + needed_scratch_space - 4;
		int64_t offset = code[i].addr + 4 - return_jal_addr;
		*((uint32_t *)return_jal_addr) = get_patch_jal(offset/2, 0);

		// patch the system call

		offset = scratch_start - code[i].addr;
		*((uint32_t *)code[i].addr) = get_patch_jal(offset/2, 0);

	  } else {
		int start_idx = i;
		int length = code[i].len;
		int preamble_depre_reg = 0, p_postamble_depre_reg = 0; 
		uint32_t depre_mask = 0, depen_mask = 0;

		// Find preamble and find deprecated register
		for (int j = i; (j = (j + (sizeof(code) / sizeof(struct code)) - 1) %
			  (sizeof(code) / sizeof(struct code))) != i;) {
		  struct branch_target *target =
			rb_upper_bound_target(&branch_targets, code[j].addr);
		  if (target && target->addr < ptr) {
			break;
		  }

		  if (code[j].addr && is_safe_insn(code[j].insn)) {
			// update closest deprecated register
			// we can use to jump to stub
			depre_mask |= deprecated_reg(code[j].insn);
			depen_mask |= dependency_reg(code[j].insn);
			depre_mask = depre_mask & (~depen_mask);

			start_idx = j;
			length = ptr - code[start_idx].addr;
			if (length >= 12 && depre_mask != 0 && depre_mask != 1) {
				break;
			}
		  } else {
			break;
		  }
		}

		preamble_depre_reg = demask_ignore(depre_mask, 1);
		if (preamble_depre_reg) patch_length -= 4;

		// for searching deprecated register after postamble
		char *depre_search_start = NULL;
		int limit = preamble_depre_reg ? 8 : 12;
		if (length >= limit) {
			depre_search_start = code[i].addr + 4;
		}

		// Find postamble and place to start searching for
		// deprecated register
		char *next = ptr;
		for (int j = i;
			next < end && (j = (j + 1) % (sizeof(code) / sizeof(struct code))) !=
			start_idx && length < patch_length;) {
		  struct branch_target *target =
			rb_lower_bound_target(&branch_targets, next);
		  if (target && target->addr == next) {
			break;
		  }

		  code[j].addr = next;
		  code[j].insn = next_inst_riscv(&next);
		  code[j].len = ((code[j].insn & 0x3) == 0x3) ? 4 : 2;

		  if (is_safe_insn(code[j].insn)) {
			length = next - code[start_idx].addr;
			if (!depre_search_start) {
			  int limit = preamble_depre_reg ? 8 : 12;
			  if (length >= limit) {
				depre_search_start = next;
			  }
			}
		  } else {
			break;
		  }
		}

		if (depre_search_start == NULL) {
		  depre_search_start = next;
		}

		p_postamble_depre_reg = 
		  demask_ignore(forward_search_deprecated_reg(depre_search_start, end, 100, 0, 0, true), 10);

		if (p_postamble_depre_reg) {
		  length = depre_search_start - code[start_idx].addr;
		} 

		if (p_postamble_depre_reg) patch_length -= 4;		

		if (length < patch_length) {
		  *((uint32_t *)code[i].addr) = 0;
		  goto replaced;
		}

		// patch nop
		char *patch_nop_start = code[start_idx].addr;
		for (int copy_length = length; copy_length > 0;) {
		  *((uint16_t *) patch_nop_start) = 0x1;
		  copy_length -= 2; 
		  patch_nop_start += 2;
		}

		int copy_length = length;

		// copy preamble
		for (int j = start_idx; j != i;
			j = (j + 1) % (sizeof(code) / sizeof(struct code))) {
		  if (code[j].len == 4) {
			*((uint32_t *)dest) = code[j].insn;
		  } else {
			*((uint16_t *)dest) = code[j].insn;
		  }
		  dest += code[j].len;
		  copy_length -= code[j].len;
		}

		// load the skeleton stub to scratch space
		{
		  char *copy_start = loader ? patched_stub_loader : patched_stub;
		  for (int i = 0; i < stub_n; i++) {
			uint32_t insn = __bswap_32(*((uint32_t *)copy_start));
			*((uint32_t *)dest) = insn;
			copy_start += 4;
			dest += 4;
		  }
		}

		copy_length -= 4;

		// copy_postamble
		for (int j = (i + 1) % (sizeof(code) / sizeof(struct code));
			copy_length > 0;
			j = (j + 1) % (sizeof(code) / sizeof(struct code))) {
		  if (code[j].len == 4) {
			*((uint32_t *) dest) = code[j].insn;
		  } else {
			*((uint16_t *) dest) = code[j].insn;
		  }
		  dest += code[j].len;
		  copy_length -= code[j].len;
		}
		char *scratch_end = dest;

		// patch at the original position
		char *patch_start = code[start_idx].addr;
		if (!preamble_depre_reg) {
		  // patch the store addr
		  *((uint32_t *)patch_start) = get_patch_store();
		  // by default ra is used to store the jump offset
		  patch_start += 4;
		}

		struct inst_param auipc_encode, jalr_encode;
		int64_t offset;
		offset = scratch_start - patch_start;
		auipc_encode.imm = (offset + 0x800) >> 12;
		auipc_encode.rd = preamble_depre_reg ? preamble_depre_reg : 1;

		jalr_encode.imm = offset - (auipc_encode.imm << 12);
		jalr_encode.rs1 = auipc_encode.rd;
		jalr_encode.rd = 0;

		*((uint32_t *)patch_start) = encode_auipc32(&auipc_encode);
		*((uint32_t *)(patch_start + 4)) = encode_jalr32(&jalr_encode);
		patch_start += 8;

		if (!p_postamble_depre_reg) {
		  struct inst_param ld_encode;
		  ld_encode.imm = -8;
		  ld_encode.rs1 = 2; // sp
		  ld_encode.rd = 10;
		  *((uint32_t *)patch_start) = encode_ld32(&ld_encode);
		  patch_start += 4;
		}

		// patch at scratch end to return
		offset = patch_start - scratch_end;
		auipc_encode.imm = (offset + 0x800) >> 12;
		auipc_encode.rd = p_postamble_depre_reg ?
		  p_postamble_depre_reg : 10;

		jalr_encode.imm = offset - (auipc_encode.imm << 12);
		jalr_encode.rs1 = auipc_encode.rd;
		jalr_encode.rd = 0;

		*((uint32_t *) scratch_end) = encode_auipc32(&auipc_encode);
		*((uint32_t *) (scratch_end + 4)) = encode_jalr32(&jalr_encode);
		ptr = patch_start + 4;
	  }
	}
replaced:
	i = (i + 1) % (sizeof(code) / sizeof(struct code));
  }
  printf("patch_func end\n");
}

void patch_syscalls_in_range(struct library *lib,
	char *start,
	char *stop,
	char **extra_space,
	int *extra_len,
	bool loader) {
  _nx_debug_printf("patch syscalls in range %p-%p\n", start, stop);
  printf("patch syscalls in range =\n");
  char *func = start;
  int nopcount = 0;
  bool has_syscall = false;

  // scan procedure for riscv
  uint16_t *ptr_riscv = (uint16_t *)start;
  uint16_t *stop_riscv = (uint16_t *)stop;

  bool prev_ecall_ehi = false; // if the front immediate is the higher part of system call

  for (; ptr_riscv < stop_riscv; ptr_riscv++) {
	if (__bswap_16(*ptr_riscv) == 0x7300) { // swap because riscv is little endian
	  prev_ecall_ehi = true;
	} else {
	  if (__bswap_16(*ptr_riscv) == 0 && prev_ecall_ehi) {
		has_syscall = true;
		//printf("find system call\n");
	  }
	  prev_ecall_ehi = true;
	}
  }
  printf("finish patch syscall in range\n");

  _nx_debug_printf("has syscall? %u\n", has_syscall);
  if (has_syscall) {
    // Ensure trampoline stubs are patched
    if (loader) {
      static bool stub_loader_is_patched = false;
      if (!stub_loader_is_patched) {
        prepare_patched_stub(patched_stub_loader, (void *)&handle_syscall_loader);
        stub_loader_is_patched = true;
      }
    }
    else {
      static bool stub_is_patched = false;
      if (!stub_is_patched) {
        prepare_patched_stub(patched_stub, (void *)&handle_syscall);
        stub_is_patched = true;
      }
    }
	// Patch any remaining system calls that were in the last function before
	// the loop terminated.
	library_patch_syscalls_in_func(lib,
		func,
		stop,
		extra_space,
		extra_len,
		loader);
  }
  _nx_debug_printf("patched syscalls in range\n");
}

void detour_func(struct library *lib,
	char *start,
	char *end,
	int discriminator,
	char **extra_space,
	int *extra_len) {
  struct rb_root *branch_targets;
  branch_targets = lookup_branch_targets(start, end);

  struct code {
	char *addr;
	uint32_t insn;
	uint32_t len;
  } code[6] = {{0}};

  char *ptr = start;
  code[0].addr = ptr;
  code[0].insn = next_inst_riscv(&ptr);
  code[0].len = (code[0].insn & 0x3) == 0x3 ? 4 : 2; 

  char *next = ptr;
  size_t length = code[0].len;


  for (size_t i = 1; next < end; i++) {
	code[i].addr = next;
	code[i].insn = next_inst_riscv(&next);
	code[i].len = (code[i].insn & 0x3) == 0x3 ? 4 : 2;
	struct branch_target *target = rb_lower_bound_target(branch_targets, next);
	if (target && target->addr == next) {
	  break;
	}
	if (is_safe_insn(code[i].insn) || is_store16(code[i].insn)
		|| is_store32(code[i].insn)) {
	  length = next - code[0].addr;
	} else {
	  break;
	}
  }


  // if we have handler, the handle_vdso_call will call
  // the vdso handler, otherwise it will just treat it
  // as a normal system call
  // we need to input the system call


  char *stub = 
	"\xfe\x81\x01\x13" // addi sp, sp, -16
	"\x00\x51\x30\x23" // sd t0, 0(sp)
	"\x00\x11\x34\x23" // sd ra, 8(sp)
	"\x00\x61\x38\x23" // sd t1, 16(sp)
	"\x00\x00\x03\x13" // mv zero, t1
	"\x00\x00\x00\x00" // preserved for putting ecall number
	"\x00\x00\x02\x93" // li t0, 0
	"\x00\x82\x92\x93" // slli t0, t0, 0x8
	"\x00\x02\x82\x93" // addi t0, t0, ...
	"\x00\x82\x92\x93" // slli t0, t0, 0x8
	"\x00\x02\x82\x93" // addi t0, t0, ...
	"\x00\x82\x92\x93" // slli t0, t0, 0x8
	"\x00\x02\x82\x93" // addi t0, t0, ...
	"\x00\x82\x92\x93" // slli t0, t0, 0x8
	"\x00\x02\x82\x93" // addi t0, t0, ...
	"\x00\x82\x92\x93" // slli t0, t0, 0x8
	"\x00\x02\x82\x93" // addi t0, t0, ...
	"\x00\x82\x92\x93" // slli t0, t0, 0x8
	"\x00\x02\x82\x93" // addi t0, t0, ...
	"\x00\x82\x92\x93" // slli t0, t0, 0x8
	"\x00\x02\x82\x93" // addi t0, t0, ...
	"\x00\x02\x80\xe7" // jalr t0
	"\x00\x01\x32\x83" // ld t0, 0(sp)
	"\x00\x81\x30\x83" // ld ra, 8(sp)
	"\x01\x01\x33\x03" // ld t1, 16(sp)
	"\x01\x81\x01\x13" // addi sp, sp, 16
	;

  const size_t stub_n = 26;
  const uint32_t needed_scratch_space = stub_n * 4 + 4 + length + 4;
  // don't add the system call (-4)
  // and count the jal return (+4)


  char *dest = maps_alloc_near_range(lib->maps, code[0].addr, needed_scratch_space, PROT_EXEC | PROT_READ | PROT_WRITE, true, 0xfffff);

  if (dest == NULL) {
	assert(false);
  }

  void *trampoline_addr = dest + stub_n* 4 + 4;
  void *vdso_handler;
  if (vdso_callback) {
	vdso_handler = vdso_callback(discriminator, trampoline_addr);
  }


  struct inst_param addi_encode;
  if (vdso_handler != NULL) {
	stub[17] = 0x10; // t1 has 1
	addi_encode.imm = 0;
	addi_encode.rs1 = 0;
	addi_encode.rd = 0;

  } else {
	// the vdso call usually first put the system call number
	// and then do an ecall
	addi_encode.imm = discriminator;
	addi_encode.rs1 = 0;
	addi_encode.rd = 7;
  }

  // inst added

  char *const scratch_start = dest;

  // load the skeleton stub to scratch space
  char *stub_dest_start = dest;
  {
	char *copy_start = stub;
	for (int i = 0; i < stub_n; i++) {
	  uint32_t insn = __bswap_32(*((uint32_t *)copy_start));
	  *((uint32_t *)dest) = insn;
	  copy_start += 4;
	  dest += 4;
	}
  }
  *((uint32_t *)(stub_dest_start + 20)) = encode_addi32(&addi_encode);

  dest += 4;
  // move the trampoline to somewhere else
  int i;
  for (char *copy_start = dest, i = 0; length > 0; i++) {
	if (code[i].len == 4) {
	  *((uint32_t *) dest) = code[i].insn;
	} else {
	  *((uint16_t *) dest) = code[i].insn;
	}
	dest += code[i].len;
	length -= code[i].len;
  }

  int64_t trampoline_return_offset = code[i].addr - dest;
  *((uint32_t *) dest) = get_patch_jal(trampoline_return_offset, 0);


  // patch the target address
  // TODO: remove duplication
  uint64_t handle_vdso_addr = (uint64_t)handle_vdso;
  char *stub_patch_start = stub_dest_start  + 26;
  uint64_t mask = 0xff00000000000000;
  for (int i = 0; i < 8; i++) {
	uint8_t val = (handle_vdso_addr & mask) >> ((7 - i)*2*4);
	char b1 = (val & 0xf0) >> 4;
	char b2 = val & 0xf;
	b2 <<= 4;
	b2 |= *stub_patch_start;
	*stub_patch_start = b2;
	*(stub_patch_start + 1) = b1;
	mask >>= 8;
	stub_patch_start += 8;
  }

  // calculate the return address of the next inst after ecall
  // patch the jal to the next inst after ecall to the end of stub
  uint64_t return_jalr_addr = (uint64_t)scratch_start + stub_n * 4;
  uint64_t return_jalr_inst = 0x00008067; // ret

  *((uint32_t *)return_jalr_addr) = return_jalr_inst;
  _nx_debug_printf("return jal is 0x%08x\n", return_jal_inst);

  // patch the vdso call
  int32_t offset = scratch_start - code[0].addr;
  uint32_t jal_insn = get_patch_jal(offset/2, 0);

  // patch store and jal at the original location
  *((uint32_t *)code[0].addr) = jal_insn;

}


