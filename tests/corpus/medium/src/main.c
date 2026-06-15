extern int medium_core(void);
extern int medium_variant(void);
extern int medium_product(void);
extern int medium_module_start(void);
extern int medium_module_cache(void);
extern int medium_module_runner(void);
extern int medium_plugin_stream(void);
extern int medium_plugin_timer(void);
extern int medium_plugin_storage(void);
extern int medium_runtime_clock(void);
extern int medium_runtime_power(void);
extern int medium_service_network(void);

int main(void) {
  return medium_core() + medium_variant() + medium_product() +
         medium_module_start() + medium_module_cache() +
         medium_module_runner() + medium_plugin_stream() +
         medium_plugin_timer() + medium_plugin_storage() +
         medium_runtime_clock() + medium_runtime_power() +
         medium_service_network();
}
