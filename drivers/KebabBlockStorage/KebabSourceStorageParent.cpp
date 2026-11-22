/*
 * KEBAB: resolve IOStorageFamily's IOBlockStorageDevice metaclass at run time.
 *
 * The storage family lives in the prelinked kernel collection, not in this
 * static link, so referring to its gMetaClass symbol would fail at link time.
 * The source-compiled IOBlockStorageDevice therefore takes its superclass from
 * this lookup; it is the same class the prelinked IOBlockStorageDriver expects.
 */
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSKext.h>
#include <libkern/OSReturn.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
}

extern "C" const OSMetaClass *
native_source_storage_parent(void)
{
	const OSSymbol    *name = OSSymbol::withCStringNoCopy("IOBlockStorageDevice");
	const OSMetaClass *meta = NULL;

	if (name != NULL) {
		meta = OSMetaClass::getMetaClassWithName(name);
		name->release();
	}
	if (meta == NULL) {
		IOLog("KEBAB STORAGE: IOBlockStorageDevice metaclass not found\n");
	}
	return meta;
}


/*
 * Deferred class registration.
 *
 * The kernel runs the archive's C++ static constructors from
 * __mod_init_func during OSRuntimeInitializeCPP(), i.e. before any prelinked
 * kext class exists. getMetaClassWithName() then returns NULL and our
 * IOBlockStorageDevice would register as an orphan root class, so a match for
 * IOBlockStorageDevice would never reach it and no IOMedia (and no APFS) would
 * appear. The build strips __mod_init_func from the two objects and this
 * function runs their constructors later, once the storage family is loaded.
 */
/* clang names these after the source file, dot included, so they need asm
 * labels; the object files are compiled with those symbols globalized. */
extern "C" void kebab_base_class_ctors(void) __asm("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void kebab_device_class_ctors(void) __asm("__GLOBAL__sub_I_KebabBlockStorageDevice.cpp");

extern "C" int
kebab_storage_register_classes(void)
{
	kern_return_t      loaded;
	const OSMetaClass *parent;
	void              *handle;

	/* Both kexts are prelinked but not started; their classes have to exist. */
	loaded = OSKext::loadKextWithIdentifier("com.apple.iokit.IOStorageFamily",
	    false, false, kOSKextExcludeNone, kOSKextExcludeNone);
	if (loaded != kOSReturnSuccess) {
		return 0x100 | (int)loaded;
	}
	loaded = OSKext::loadKextWithIdentifier("com.apple.filesystems.apfs",
	    false, false, kOSKextExcludeNone, kOSKextExcludeNone);
	/* kOSReturnAlreadyLoaded is not exported by this XNU; success covers it. */
	if (loaded != kOSReturnSuccess) {
		return 0x200 | (int)loaded;
	}
	parent = native_source_storage_parent();
	if (parent == NULL || parent->getClassSize() == 0) {
		return 0x300;
	}
	handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
	if (handle == NULL) {
		return 0x400;
	}
	kebab_base_class_ctors();
	kebab_device_class_ctors();
	loaded = OSMetaClass::postModLoad(handle);
	if (loaded != kOSReturnSuccess) {
		return 0x500 | (int)loaded;
	}
	return 0;
}
