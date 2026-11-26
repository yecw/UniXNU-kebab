/*
 * Link-time bindings for the IOStorageFamily symbols the kernel-side RAM block
 * driver needs. They live in the prelinked IOStorageFamily kext, so a static
 * kernel cannot resolve them; each is bound to its address in the prelinked
 * image (the donor kernelcache address plus this board's prelink slide of
 * -0x1000000).
 *
 * The addresses were recovered structurally with the method of GFander-kebab's
 * scripts/recover-kext-symbols.py (vtable slots of IOBlockStorageDevice, the
 * class's gMetaClass from the mod_init thunk, and C2/D2 from the constructor
 * store and the destructor tail call); init is the IOService-range override in
 * vtable slot 21. Verified against the built image: __ZTV lands inside
 * __PLK_DATA_CONST and the methods inside __PLK_TEXT_EXEC.
 */

__asm__(".globl __ZN20IOBlockStorageDevice10gMetaClassE\n.set __ZN20IOBlockStorageDevice10gMetaClassE, 0xfffffff006add1d0");
__asm__(".globl __ZN20IOBlockStorageDevice4initEP12OSDictionary\n.set __ZN20IOBlockStorageDevice4initEP12OSDictionary, 0xfffffff00496ee30");
__asm__(".globl __ZN20IOBlockStorageDevice11requestIdleEv\n.set __ZN20IOBlockStorageDevice11requestIdleEv, 0xfffffff00496f050");
__asm__(".globl __ZN20IOBlockStorageDevice13doSetPriorityEP26IOBlockStorageDeviceExtentjh\n.set __ZN20IOBlockStorageDevice13doSetPriorityEP26IOBlockStorageDeviceExtentjh, 0xfffffff00496f068");
__asm__(".globl __ZN20IOBlockStorageDevice13setPropertiesEP8OSObject\n.set __ZN20IOBlockStorageDevice13setPropertiesEP8OSObject, 0xfffffff00496ef38");
__asm__(".globl __ZN20IOBlockStorageDevice20doGetProvisionStatusEyyPjP35IOBlockStorageProvisionDeviceExtenty\n.set __ZN20IOBlockStorageDevice20doGetProvisionStatusEyyPjP35IOBlockStorageProvisionDeviceExtenty, 0xfffffff00496f08c");
__asm__(".globl __ZN20IOBlockStorageDevice7doUnmapEP26IOBlockStorageDeviceExtentjj\n.set __ZN20IOBlockStorageDevice7doUnmapEP26IOBlockStorageDeviceExtentjj, 0xfffffff00496f05c");
__asm__(".globl __ZN20IOBlockStorageDeviceC2EPK11OSMetaClass\n.set __ZN20IOBlockStorageDeviceC2EPK11OSMetaClass, 0xfffffff00496ed98");
__asm__(".globl __ZN20IOBlockStorageDeviceD2Ev\n.set __ZN20IOBlockStorageDeviceD2Ev, 0xfffffff00496edb8");
__asm__(".globl __ZNK20IOBlockStorageDevice11getPropertyEPK8OSSymbol\n.set __ZNK20IOBlockStorageDevice11getPropertyEPK8OSSymbol, 0xfffffff00496ee88");
__asm__(".globl __ZTV20IOBlockStorageDevice\n.set __ZTV20IOBlockStorageDevice, 0xfffffff005cb44c0");
