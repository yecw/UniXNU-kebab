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

extern "C" {
void     kebab_early_mark(unsigned stage, const char *label);
void     kebab_early_hex(unsigned long long value);
int      kebab_ufs_raw_read(void *buffer, uint64_t offset, size_t length);
uint64_t kebab_ufs_partition_bytes(void);
uint32_t kebab_ufs_block_size(void);
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

private:
	int transfer(IOMemoryDescriptor *buffer, UInt64 offset, UInt64 length);
};

OSDefineMetaClassAndStructors(KebabBlockStorageDevice, IOBlockStorageDevice);

static KebabBlockStorageDevice *kebab_block_device;
static char kebabVendor[]  = "KEBAB";
static char kebabProduct[] = "UFS root";
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
	if (offset > kebab_ufs_partition_bytes() || length > kebab_ufs_partition_bytes() - offset) {
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
		int error = kebab_ufs_raw_read(staging, offset + done, (size_t)want);
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
	IOReturn status;
	UInt64   offset;
	UInt64   length;

	(void)attributes;
	if (completion == NULL || buffer == NULL) {
		return kIOReturnBadArgument;
	}
	offset = block * kebab_ufs_block_size();
	length = nblks * kebab_ufs_block_size();
	if (buffer->getDirection() == kIODirectionOut) {
		status = kIOReturnSuccess;      /* read-only media, nothing to write */
	} else {
		int error = transfer(buffer, offset, length);
		status = error ? kIOReturnIOError : kIOReturnSuccess;
	}
	/* IOStorage::complete lives in the prelinked family; call the completion
	 * action directly instead of linking its symbol. */
	if (completion->action != NULL) {
		completion->action(completion->target, completion->parameter, status, length);
	}
	return status;
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
		capacities[0] = kebab_ufs_partition_bytes();
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
	return kebabProduct;
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
	if (kebab_ufs_partition_bytes() == 0) {
		return kIOReturnNotReady;
	}
	*maxBlock = kebab_ufs_partition_bytes() / kebab_ufs_block_size() - 1;
	return kIOReturnSuccess;
}

IOReturn
KebabBlockStorageDevice::reportMediaState(bool *mediaPresent, bool *changedState)
{
	if (mediaPresent == NULL) {
		return kIOReturnBadArgument;
	}
	*mediaPresent = (kebab_ufs_partition_bytes() != 0);
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
	IOSleep(300);
	{
		OSDictionary *matching = IOService::serviceMatching("IOMedia");
		unsigned media = 0;
		if (matching != NULL) {
			OSIterator *iterator = IOService::getMatchingServices(matching);
			if (iterator != NULL) {
				while (iterator->getNextObject() != NULL && media < 64) {
					media++;
				}
				iterator->release();
			}
			matching->release();
		}
		kebab_early_mark(230, "block: IOMedia after delay");
		kebab_early_hex((unsigned long long)media);
	}
	{
		OSDictionary *matching = IOService::serviceMatching("IOMedia");
		if (matching != NULL) {
			OSIterator *iterator = IOService::getMatchingServices(matching);
			unsigned media = 0;
			if (iterator != NULL) {
				while (iterator->getNextObject() != NULL && media < 64) {
					media++;
				}
				iterator->release();
			}
			IOLog("KEBAB BLOCK: IOMedia services after registration: %u\n", media);
			kebab_early_mark(228, "block: IOMedia count");
			kebab_early_hex((unsigned long long)media);
			matching->release();
		}
	}
	IOLog("KEBAB BLOCK: registered %llu MiB, %u-byte blocks\n",
	    kebab_ufs_partition_bytes() >> 20, kebab_ufs_block_size());
	return 0;
}
