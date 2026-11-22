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
