/*
 * Kebab bring-up stubs.
 *
 * The UniXNU XNU patches reference device probe entry points that only exist
 * when the matching native driver archive is linked.  The kebab kernel does not
 * build those drivers yet, so this archive provides weak no-op definitions.
 * Weak symbols let a real driver override them later.
 */
#define KSTUB __attribute__((weak))

KSTUB void native_adreno_firmware_probe(void) {}
KSTUB void native_constants_probe(void) {}
KSTUB void native_legacy_display_late_probe(void) {}
KSTUB void native_legacy_display_probe(void) {}
KSTUB void native_mobile_display_provider_probe(void) {}
KSTUB void native_mobile_graphics_client_probe(void) {}
KSTUB void native_offline_gpu_publish(void) {}
KSTUB void native_verified_root_probe(void) {}

KSTUB void native_owner_root_accepted_checkpoint(int a, int b) { (void)a; (void)b; }
KSTUB void native_owner_root_checkpoint(int a, int b, int c) { (void)a; (void)b; (void)c; }
KSTUB int  native_owner_publish_boot_manifest(int a, const char *b, int c, const char *d)
{ (void)a; (void)b; (void)c; (void)d; return 0; }
/* native_verified_block_root_authorized: provided by drivers/KebabBlockStorage */
KSTUB int  native_ram_block_volume_seed(const char *a, int *b, char *c, unsigned long d,
                                        unsigned long *e, unsigned long *f, unsigned char *g)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return 0; }

/* Referenced through __attribute__((weak_import)) by the GPU addon mount patch;
 * a static kernel link still needs a definition when no provider is present. */
KSTUB int  native_ram_block_gpu_addon(const unsigned char **a, unsigned long long *b)
{ (void)a; (void)b; return 0; }

/* NativeRamBlock declares these weak_import; a static kernel link still needs
 * definitions. A zero store mode / no GPT keeps the Data disk volatile, which
 * is what this port wants until a persistent window exists. */
KSTUB int  native_ufs_store_mode(void) { return 0; }
KSTUB int  native_ufs_store_gpt_check(void) { return 0; }
KSTUB int  native_ufs_store_open(unsigned long long a, unsigned long long *b, const char **c)
{ (void)a; (void)b; (void)c; return -1; }
KSTUB int  native_ufs_store_read(unsigned long long a, unsigned int b, void *c)
{ (void)a; (void)b; (void)c; return -1; }
KSTUB int  native_ufs_store_write(unsigned long long a, unsigned int b, const void *c)
{ (void)a; (void)b; (void)c; return -1; }
KSTUB int  native_ufs_store_write_cached(unsigned long long a, unsigned int b, const void *c)
{ (void)a; (void)b; (void)c; return -1; }
KSTUB int  native_ufs_store_sync(void) { return -1; }
