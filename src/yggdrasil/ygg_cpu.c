
#if defined(__GNUC__)
	#if defined(__aarch64__)
		#define ygg_cpu_clobber_all_registers() asm volatile ("":::"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", 	"x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21", "x22", 	"x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30", "cc")

		typedef struct Ygg_CPU_State {
			void* reg[4];
		} Ygg_CPU_State;
		#define ygg_cpu_state_store(state) \
			ygg_cpu_clobber_all_registers();\
			asm volatile ("mov x0, sp\n"\
						  "str x0, %0\n" /* store sp */\
						  "mov x0, lr\n"\
						  "str x0, %1\n" /* store lr */\
						  "mov x0, fp\n"\
						  "str x0, %3\n" /* store frame pointer */\
						  "adr x0, #0\n"\
						  "str x0, %2\n" /* store pc */\
						  : "=m"(state.reg[0]), "=m"(state.reg[1]), "=m"(state.reg[2]), "=m"(state.reg[3])\
						  :\
						  : "x0", "memory"\
						  )\
		
		#define ygg_cpu_state_restore(state) \
			asm volatile ("ldr x0, %0\n"\
						  "mov sp, x0\n" /* restore sp */\
						  "ldr x0, %1\n"\
						  "mov lr, x0\n" /* restore lr */\
						  "ldr x0, %3\n"\
						  "mov fp, x0\n" /* restore frame pointer */\
						  "ldr x0, %2\n"\
						  "br x0\n" /* restore pc */\
						  :\
						  : "m"(state.reg[0]), "m"(state.reg[1]), "m"(state.reg[2]), "m"(state.reg[3])\
						  : "x0"\
						  )\
		
		#define ygg_fiber_boot(stack_ptr, func, data) \
			ygg_cpu_clobber_all_registers();\
			asm volatile(\
				/* Set sp and push current sp to the new stack for restoring when fiber ends. */\
				"mov x1, sp\n"\
				"mov sp, %0\n"\
				"sub sp, sp, #16\n"\
				"str x1, [sp]\n"\
				/* Call func */\
				"mov x0, %2\n"\
				"blr %1\n"\
				/* Restore original sp */\
				"ldr x1, [sp]\n"\
				"mov sp, x1\n"\
				:\
				: "r"(stack_ptr), "r"(func), "r"(data) \
				:\
			)
	#else
		#error "unsupported architecture"
	#endif
#else
	#error "unsupported compiler"
#endif
