extern int medium_core(void);
extern int medium_arch(void);
extern int medium_board(void);
extern int medium_kernel_boot(void);
extern int medium_kernel_mm(void);
extern int medium_kernel_sched(void);
extern int medium_driver_serial(void);
extern int medium_driver_timer(void);
extern int medium_driver_storage(void);
extern int medium_platform_clock(void);
extern int medium_platform_power(void);
extern int medium_network_stack(void);

int main(void) {
  return medium_core() + medium_arch() + medium_board() + medium_kernel_boot() +
         medium_kernel_mm() + medium_kernel_sched() + medium_driver_serial() +
         medium_driver_timer() + medium_driver_storage() +
         medium_platform_clock() + medium_platform_power() +
         medium_network_stack();
}
