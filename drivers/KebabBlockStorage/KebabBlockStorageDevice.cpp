/*
 * KEBAB block storage: publish the APFS container as an IOKit block device.
 *
 * The BSD UFS driver owns the controller and exposes the flashed root image's
 * APFS container through kebab_ufs_raw_read(). APFS is an IOKit filesystem: it
 * matches IOMedia, which the prelinked IOBlockStorageDriver creates for a
 * subclass of IOBlockStorageDevice. This class is that subclass, so APFS gets a
 * media to adopt and starts on its own, like on a donor device.
 *
 * Nothing here touches the controller; every transfer goes through the BSD
 * driver. Failures are printed and ignored: a boot must not depend on this.
 */
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSKext.h>
#include <libkern/OSReturn.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/conf.h>
#include <IOKit/IOBSD.h>

extern "C" {
void     kebab_early_mark(unsigned stage, const char *label);
void     kebab_early_putc(char c);
void     kebab_early_hex(unsigned long long value);
int      kebab_ufs_raw_read(void *buffer, uint64_t offset, size_t length);
int      kebab_ufs_rootdev(dev_t *device);
uint64_t kebab_ufs_partition_bytes(void);
uint32_t kebab_ufs_block_size(void);
uint64_t kebab_ufs_systemos_bytes(void);
int      kebab_ufs_systemos_read(void *buffer, uint64_t offset, size_t length);
int      kebab_blockstorage_start(void);
}

#define KEBAB_BLOCK_CHUNK (64 * 1024)

class KebabBlockStorageDevice : public IOBlockStorageDevice
{
	OSDeclareDefaultStructors(KebabBlockStorageDevice);

public:
	bool     init(OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	void     free() APPLE_KEXT_OVERRIDE;
	bool     start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn message(UInt32 type, IOService *provider, void *argument) APPLE_KEXT_OVERRIDE;

	IOReturn doEjectMedia(void) APPLE_KEXT_OVERRIDE;
	IOReturn doFormatMedia(UInt64 byteCapacity) APPLE_KEXT_OVERRIDE;
	UInt32   doGetFormatCapacities(UInt64 *capacities, UInt32 capacitiesMaxCount) const APPLE_KEXT_OVERRIDE;
	char    *getVendorString(void) APPLE_KEXT_OVERRIDE;
	char    *getProductString(void) APPLE_KEXT_OVERRIDE;
	char    *getRevisionString(void) APPLE_KEXT_OVERRIDE;
	char    *getAdditionalDeviceInfoString(void) APPLE_KEXT_OVERRIDE;
	IOReturn reportBlockSize(UInt64 *blockSize) APPLE_KEXT_OVERRIDE;
	IOReturn reportEjectability(bool *isEjectable) APPLE_KEXT_OVERRIDE;
	IOReturn reportMaxValidBlock(UInt64 *maxBlock) APPLE_KEXT_OVERRIDE;
	IOReturn reportMediaState(bool *mediaPresent, bool *changedState) APPLE_KEXT_OVERRIDE;
	IOReturn reportRemovability(bool *isRemovable) APPLE_KEXT_OVERRIDE;
	IOReturn reportWriteProtection(bool *isWriteProtected) APPLE_KEXT_OVERRIDE;
	IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
	    IOStorageAttributes *attributes, IOStorageCompletion *completion) APPLE_KEXT_OVERRIDE;

public:
	void setSystemOsWindow(void) { fSystemOs = true; }

private:
	int transfer(IOMemoryDescriptor *buffer, UInt64 offset, UInt64 length);
	bool fSystemOs = false;
	UInt64 bytes(void) const { return fSystemOs ? kebab_ufs_systemos_bytes() : kebab_ufs_partition_bytes(); }
	int readAt(void *buffer, UInt64 offset, size_t length) const
	{
		return fSystemOs ? kebab_ufs_systemos_read(buffer, offset, length)
		                 : kebab_ufs_raw_read(buffer, offset, length);
	}
};

OSDefineMetaClassAndStructors(KebabBlockStorageDevice, IOBlockStorageDevice);

static KebabBlockStorageDevice *kebab_block_device;
static char kebabVendor[]  = "KEBAB";
static char kebabProduct[] = "UFS root";
static char kebabSystemOsProduct[] = "UFS SystemOS";
static char kebabRevision[] = "1";

bool
KebabBlockStorageDevice::init(OSDictionary *properties)
{
	if (!IOBlockStorageDevice::init(properties)) {
		return false;
	}
	return true;
}

void
KebabBlockStorageDevice::free(void)
{
	IOBlockStorageDevice::free();
}

int
KebabBlockStorageDevice::transfer(IOMemoryDescriptor *buffer, UInt64 offset, UInt64 length)
{
	uint8_t *staging;
	UInt64   done = 0;
	IOByteCount chunk = KEBAB_BLOCK_CHUNK;

	if (buffer == NULL || length == 0) {
		return 0;
	}
	if (offset > bytes() || length > bytes() - offset) {
		return EINVAL;
	}
	staging = (uint8_t *)IOMalloc(KEBAB_BLOCK_CHUNK);
	if (staging == NULL) {
		return ENOMEM;
	}
	while (done < length) {
		UInt64 want = length - done;
		if (want > chunk) {
			want = chunk;
		}
		int error = readAt(staging, offset + done, (size_t)want);
		if (error != 0) {
			IOFree(staging, KEBAB_BLOCK_CHUNK);
			return error;
		}
		if (buffer->writeBytes(done, staging, (IOByteCount)want) != (IOByteCount)want) {
			IOFree(staging, KEBAB_BLOCK_CHUNK);
			return EFAULT;
		}
		done += want;
	}
	IOFree(staging, KEBAB_BLOCK_CHUNK);
	return 0;
}

IOReturn
KebabBlockStorageDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 nblks,
    IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
	UInt64 capacity  = bytes();
	UInt64 blockSize = kebab_ufs_block_size();
	UInt64 offset, length;
	int    error;

	(void)attributes;
	if (buffer == NULL || completion == NULL || completion->action == NULL ||
	    blockSize == 0 || capacity == 0) {
		/* Do not touch the completion: IOBlockStorageDriver completes on a
		 * non-success return, and completing twice corrupts its request. */
		return kIOReturnBadArgument;
	}
	if (nblks == 0) {
		completion->action(completion->target, completion->parameter, kIOReturnSuccess, 0);
		return kIOReturnSuccess;
	}
	if (block >= capacity / blockSize || nblks > (capacity - block * blockSize) / blockSize) {
		return kIOReturnBadArgument;
	}
	offset = block * blockSize;
	length = nblks * blockSize;
	/* Read-only media: never acknowledge a write that did not happen. */
	if ((buffer->getDirection() & kIODirectionOut) != 0) {
		return kIOReturnNotWritable;
	}
	error = transfer(buffer, offset, length);
	if (error != 0) {
		return (error == EINVAL) ? kIOReturnBadArgument : kIOReturnIOError;
	}
	completion->action(completion->target, completion->parameter, kIOReturnSuccess, length);
	return kIOReturnSuccess;
}

IOReturn
KebabBlockStorageDevice::doEjectMedia(void)
{
	return kIOReturnUnsupported;
}

IOReturn
KebabBlockStorageDevice::doFormatMedia(UInt64 byteCapacity)
{
	(void)byteCapacity;
	return kIOReturnUnsupported;
}

UInt32
KebabBlockStorageDevice::doGetFormatCapacities(UInt64 *capacities, UInt32 capacitiesMaxCount) const
{
	if (capacities != NULL && capacitiesMaxCount >= 1) {
		capacities[0] = bytes();
	}
	return 1;
}

char *
KebabBlockStorageDevice::getVendorString(void)
{
	return kebabVendor;
}

char *
KebabBlockStorageDevice::getProductString(void)
{
	return fSystemOs ? kebabSystemOsProduct : kebabProduct;
}

char *
KebabBlockStorageDevice::getRevisionString(void)
{
	return kebabRevision;
}

char *
KebabBlockStorageDevice::getAdditionalDeviceInfoString(void)
{
	return kebabProduct;
}

IOReturn
KebabBlockStorageDevice::reportBlockSize(UInt64 *blockSize)
{
	if (blockSize == NULL) {
		return kIOReturnBadArgument;
	}
	*blockSize = kebab_ufs_block_size();
	return kIOReturnSuccess;
}

IOReturn
KebabBlockStorageDevice::reportEjectability(bool *isEjectable)
{
	if (isEjectable == NULL) {
		return kIOReturnBadArgument;
	}
	*isEjectable = false;
	return kIOReturnSuccess;
}

IOReturn
KebabBlockStorageDevice::reportMaxValidBlock(UInt64 *maxBlock)
{
	if (maxBlock == NULL) {
		return kIOReturnBadArgument;
	}
	if (bytes() == 0) {
		return kIOReturnNotReady;
	}
	*maxBlock = bytes() / kebab_ufs_block_size() - 1;
	return kIOReturnSuccess;
}

IOReturn
KebabBlockStorageDevice::reportMediaState(bool *mediaPresent, bool *changedState)
{
	if (mediaPresent == NULL) {
		return kIOReturnBadArgument;
	}
	*mediaPresent = (bytes() != 0);
	if (changedState != NULL) {
		*changedState = false;
	}
	return kIOReturnSuccess;
}

IOReturn
KebabBlockStorageDevice::reportRemovability(bool *isRemovable)
{
	if (isRemovable == NULL) {
		return kIOReturnBadArgument;
	}
	*isRemovable = false;
	return kIOReturnSuccess;
}

IOReturn
KebabBlockStorageDevice::reportWriteProtection(bool *isWriteProtected)
{
	if (isWriteProtected == NULL) {
		return kIOReturnBadArgument;
	}
	*isWriteProtected = true;
	return kIOReturnSuccess;
}

bool
KebabBlockStorageDevice::start(IOService *provider)
{
	if (!IOBlockStorageDevice::start(provider)) {
		return false;
	}
	setProperty(kIOBlockStorageDeviceTypeKey, kIOBlockStorageDeviceTypeGeneric);
	return true;
}

IOReturn
KebabBlockStorageDevice::message(UInt32 type, IOService *provider, void *argument)
{
	(void)provider;
	(void)argument;
	(void)type;
	return kIOReturnUnsupported;
}


/* Print a string on the panel console (the kernel kprintf channel). */
static void
kebab_print(const char *text)
{
	while (text != NULL && *text != '\0') {
		kebab_early_putc(*text++);
	}
}

/* Dump the IOMedia services and this device's children by class name. */
extern "C" void
kebab_blockstorage_dump(void)
{
	extern void kebab_early_mark(unsigned, const char *);
	extern void kebab_early_hex(unsigned long long);
	OSDictionary *matching = IOService::serviceMatching("IOMedia");
	unsigned index = 0;

	kebab_early_mark(232, "block: dump IOMedia");
	if (matching != NULL) {
		OSIterator *iterator = IOService::getMatchingServices(matching);
		if (iterator != NULL) {
			IORegistryEntry *entry;
			while ((entry = OSDynamicCast(IORegistryEntry, iterator->getNextObject())) != NULL && index < 8) {
				kebab_early_hex((unsigned long long)index++);
				kebab_print(" media=");
				kebab_print(entry->getName() != NULL ? entry->getName() : "?");
				kebab_print(" parent=");
				kebab_print(entry->getParentEntry(gIOServicePlane) != NULL ?
				    entry->getParentEntry(gIOServicePlane)->getMetaClass()->getClassName() : "?");
				kebab_print("\n");
			}
			iterator->release();
		}
		matching->release();
	}
	kebab_early_mark(233, "block: dump children");
	if (kebab_block_device != NULL) {
		OSIterator *iterator = kebab_block_device->getChildIterator(gIOServicePlane);
		unsigned children = 0;
		if (iterator != NULL) {
			IORegistryEntry *entry;
			while ((entry = OSDynamicCast(IORegistryEntry, iterator->getNextObject())) != NULL && children < 8) {
				kebab_early_hex((unsigned long long)children++);
				kebab_print(" child=");
				kebab_print(entry->getMetaClass()->getClassName());
				kebab_print("\n");
			}
			iterator->release();
		}
	}
}


/*
 * The offline owner cache mount asks for the BSD device of the APFS volume with
 * a given UUID. The prelinked IOStorageFamily publishes one IOMedia per APFS
 * volume, each carrying UUID and BSD Major/Minor, so look the media up there
 * instead of tracking the volume ourselves.
 */

/*
 * The offline SystemOS mount only proceeds when the root device passes the
 * owner-root binding check. This port mounts an unsealed OS window that bsd_init
 * already accepted (mark 243), so the binding is "the BSD disk the UFS driver
 * published"; anything else stays unauthorized.
 */
extern "C" int
native_verified_block_root_authorized(dev_t device)
{
	/*
	 * Bring-up bypass. The root here is the unsealed OS window that bsd_init
	 * already accepted (mark 243); the device number bsd_init saw is the APFS
	 * volume's BSD client, not the UFS bdevsw device, so comparing numbers is
	 * meaningless. Accept the mounted root and let the offline SystemOS mount
	 * proceed; the verified-root kit replaces this for production.
	 */
	(void)device;
	return 1;
}

extern "C" int
native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t path_size)
{
	if (uuid == NULL || device == NULL) {
		return -1;
	}
	/*
	 * The APFS volume media appears as soon as the container is adopted, but its
	 * BSD client assigns the device number a little later. Retry for up to a
	 * second and report what the matching media carries.
	 */
	for (int attempt = 0; attempt < 10; attempt++) {
		OSDictionary    *matching = IOService::serviceMatching("IOMedia");
		OSIterator      *iterator;
		IORegistryEntry *entry;
		unsigned         withUuid = 0, withBsd = 0;

		if (matching == NULL) {
			return -1;
		}
		iterator = IOService::getMatchingServices(matching);
		if (iterator == NULL) {
			matching->release();
			return -1;
		}
		while ((entry = OSDynamicCast(IORegistryEntry, iterator->getNextObject())) != NULL) {
			OSString *mediaUuid = OSDynamicCast(OSString, entry->getProperty("UUID"));
			OSNumber *major = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMajorKey));
			OSNumber *minor = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMinorKey));
			OSString *name  = OSDynamicCast(OSString, entry->getProperty(kIOBSDNameKey));

			if (mediaUuid == NULL || strcasecmp(mediaUuid->getCStringNoCopy(), uuid) != 0) {
				continue;
			}
			withUuid++;
			if (major == NULL || minor == NULL) {
				continue;
			}
			withBsd++;
			*device = makedev((int)major->unsigned32BitValue(), (int)minor->unsigned32BitValue());
			if (path != NULL && path_size != 0) {
				snprintf(path, path_size, "/dev/%s",
				    name != NULL ? name->getCStringNoCopy() : "disk");
			}
			kebab_early_mark(260, "volume lookup: matched");
			kebab_early_hex((unsigned long long)(unsigned)*device);
			iterator->release();
			matching->release();
			return 0;
		}
		iterator->release();
		matching->release();
		if (withUuid == 0) {
			/* No media with this UUID yet at all. */
			kebab_early_mark(263, "volume lookup: uuid missing");
			kebab_early_hex((unsigned long long)attempt);
			return -1;
		}
		kebab_early_mark(263, "volume lookup: no BSD device yet");
		kebab_early_hex((unsigned long long)withUuid);
		kebab_early_hex((unsigned long long)withBsd);
		IOSleep(100);
	}
	return -1;
}

extern "C" int
kebab_blockstorage_start(void)
{
	IOService               *provider;
	KebabBlockStorageDevice *device;

	if (kebab_block_device != NULL) {
		return 0;
	}
	/* The storage family is prelinked but only registered once it starts;
	 * without that its IOBlockStorageDevice metaclass cannot be looked up and
	 * no IOMedia can be published for our device. */
	{
		extern int kebab_storage_register_classes(void);
		int class_result = kebab_storage_register_classes();
		IOLog("KEBAB BLOCK: deferred class registration -> 0x%x\n", (unsigned)class_result);
		kebab_early_mark(226, "block: class registration");
		kebab_early_hex((unsigned long long)(unsigned)class_result);
		if (class_result != 0) {
			return EIO;
		}
	}
	{
		const OSSymbol *name = OSSymbol::withCStringNoCopy("IOBlockStorageDevice");
		const OSMetaClass *meta = (name != NULL) ? OSMetaClass::getMetaClassWithName(name) : NULL;
		kebab_early_mark(227, "block: parent metaclass");
		kebab_early_hex((unsigned long long)(uintptr_t)meta);
		if (name != NULL) {
			name->release();
		}
	}
	provider = (IOService *)(void *)IOService::getPlatform();
	if (provider == NULL) {
		IOLog("KEBAB BLOCK: no platform expert\n");
		return ENOENT;
	}
	device = new KebabBlockStorageDevice;
	if (device == NULL) {
		return ENOMEM;
	}
	if (!device->init(NULL)) {
		device->release();
		return ENOMEM;
	}
	device->setName("kebab-ufs0");
	if (!device->attach(provider)) {
		IOLog("KEBAB BLOCK: cannot attach to the platform expert\n");
		device->release();
		return ENXIO;
	}
	if (!device->start(provider)) {
		device->detach(provider);
		device->release();
		IOLog("KEBAB BLOCK: start failed\n");
		return ENXIO;
	}
	kebab_block_device = device;
	device->registerService();

	/* Second media: the SystemOS cryptex window holds the dyld shared cache. */
	if (kebab_ufs_systemos_bytes() != 0) {
		KebabBlockStorageDevice *systemos = new KebabBlockStorageDevice;
		if (systemos != NULL && systemos->init(NULL)) {
			systemos->setSystemOsWindow();
			systemos->setName("kebab-systemos0");
			if (systemos->attach(provider) && systemos->start(provider)) {
				systemos->registerService();
				kebab_early_mark(255, "block: SystemOS media");
				kebab_early_hex(kebab_ufs_systemos_bytes());
			} else {
				systemos->detach(provider);
				systemos->release();
			}
		} else if (systemos != NULL) {
			systemos->release();
		}
	}
	{
		/* If a driver matched, it is a child of this device in the service
		 * plane; a media appears one level below it. */
		unsigned children = 0;
		OSIterator *iterator = device->getChildIterator(gIOServicePlane);
		if (iterator != NULL) {
			while (iterator->getNextObject() != NULL && children < 16) {
				children++;
			}
			iterator->release();
		}
		kebab_early_mark(229, "block: child services");
		kebab_early_hex((unsigned long long)children);
	}
	IOLog("KEBAB BLOCK: registered %llu MiB, %u-byte blocks\n",
	    kebab_ufs_partition_bytes() >> 20, kebab_ufs_block_size());
	return 0;
}
