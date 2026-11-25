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
#include <vm/vm_kern.h>
#include <libkern/crypto/sha2.h>

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

#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
#include "verified_root.h"
extern "C" const struct vr_crypto vr_xnu_crypto;
extern "C" const uint8_t verified_root_manifest[];
extern "C" const uint64_t verified_root_manifest_bytes;
extern "C" const uint8_t verified_root_key[];
extern "C" const uint8_t verified_root_manifest_systemos[];
extern "C" const uint64_t verified_root_manifest_systemos_bytes;
extern "C" uint64_t kebab_ufs_object_bytes(int which);
extern "C" int kebab_ufs_object_read(int which, void *buffer, uint64_t offset, size_t length);
extern "C" int kebab_verified_root_configured(void);
extern "C" boolean_t PE_parse_boot_argn(const char *, void *, int);
#endif

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
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
public:
	bool authenticatesVolume(const char *uuid, uint8_t *digest) const;
	bool manifestDigest(const char *uuid, uint8_t *digest) const;
	bool loadTable(void);
	bool signedUuid(char expected[37]) const;
private:
	bool configureVerification(void);
	int  verifiedTransfer(IOMemoryDescriptor *buffer, UInt64 offset, UInt64 length);
	bool        fVerified = false;
	bool        fHeaderOnly = false;
	uint64_t    fEnvelopeBytes = 0;
	uint64_t    fTableBytes = 0;
	bool        fTablePending = false;
	struct vr_view fView = {};
	vm_offset_t fStore = 0;
	vm_size_t   fStoreBytes = 0;
	uint8_t    *fSnapshot = nullptr;
#endif
};

OSDefineMetaClassAndStructors(KebabBlockStorageDevice, IOBlockStorageDevice);

static KebabBlockStorageDevice *kebab_block_device;
static KebabBlockStorageDevice *kebab_verified_windows[2];
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
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
	if (fSnapshot != nullptr) {
		IOFree(fSnapshot, KEBAB_BLOCK_CHUNK);
		fSnapshot = nullptr;
	}
	if (fStore != 0) {
		kmem_free(kernel_map, fStore, fStoreBytes);
		fStore = 0;
		fVerified = false;
	}
#endif
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
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
	if (fVerified && !fHeaderOnly) {
		return verifiedTransfer(buffer, offset, length);
	}
#endif
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
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
	if (configureVerification()) {
		kebab_verified_windows[fSystemOs ? 1 : 0] = this;
		kebab_early_mark(297, "vroot: window verified");
		kebab_early_hex(fView.block_count);
	} else {
		kebab_early_mark(298, "vroot: window unverified");
		kebab_early_hex(bytes());
	}
#endif
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

#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
/*
 * Owner-verified root, ported from the reference provider
 * (drivers/VirtioMmioBlock/NativeVirtioBlock.cpp): the manifest is matched to
 * this window by its signed image_bytes, copied with the trusted key into
 * read-only kernel memory, and every 4096-byte block read through this device
 * is checked against the signed hash table before it reaches the caller.
 */
bool
KebabBlockStorageDevice::configureVerification(void)
{
	const uint8_t *manifest = fSystemOs ? verified_root_manifest_systemos : verified_root_manifest;
	uint64_t manifestBytes = fSystemOs ? verified_root_manifest_systemos_bytes :
	    verified_root_manifest_bytes;
	uint64_t capacity = bytes();
	uint64_t image_bytes = 0;
	uint64_t blockCount = 0;
	uint64_t tableBytes = 0;
	uint64_t envelopeBytes;
	bool headerOnly;
	int  opened;

	if (capacity == 0 || manifestBytes == 0) {
		return false;
	}
	for (unsigned i = 0; i < 8; i++) {
		image_bytes |= (uint64_t)manifest[24 + i] << (8 * i);
		blockCount |= (uint64_t)manifest[32 + i] << (8 * i);
	}
	if (image_bytes != capacity) {
		kebab_early_mark(297, "vroot: capacity mismatch");
		kebab_early_hex(image_bytes);
		kebab_early_hex(capacity);
		return false;
	}
	/*
	 * The hash table is 32 bytes per 4096-byte block and does not fit the boot
	 * payload, so unixnu userdata carries it as a v2 data object. When the
	 * header is linked alone, read the table from that object and build the full
	 * signed envelope in kernel memory; without the object the header still
	 * authorises the window (no per-block verification).
	 */
	headerOnly = (manifestBytes == (uint64_t)(VR_HEADER_BYTES + VR_SIGNATURE_BYTES));
	envelopeBytes = manifestBytes;
	if (headerOnly) {
		uint64_t objectBytes = kebab_ufs_object_bytes(fSystemOs ? 1 : 0);
		tableBytes = blockCount * 32;
		if (objectBytes == tableBytes && tableBytes != 0) {
			envelopeBytes = manifestBytes + tableBytes;
		}
	}
	vm_size_t total = round_page((vm_size_t)envelopeBytes) + PAGE_SIZE;
	if (kmem_alloc(kernel_map, &fStore, total,
	    static_cast<kma_flags_t>(KMA_KOBJECT | KMA_DATA),
	    VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) {
		return false;
	}
	fStoreBytes = total;
	memcpy((void *)fStore, manifest, (size_t)manifestBytes);
	fTableBytes = tableBytes;
	if (envelopeBytes > manifestBytes) {
		if (kebab_ufs_object_read(fSystemOs ? 1 : 0, (void *)(fStore + manifestBytes),
		    0, (size_t)tableBytes) != 0) {
			kebab_early_mark(297, "vroot: table read failed");
			return false;
		}
		headerOnly = false;
	}
	memcpy((void *)(fStore + round_page((vm_size_t)envelopeBytes)), verified_root_key, VR_KEY_BYTES);
	(void)vm_map_protect(kernel_map, fStore, fStore + total, VM_PROT_READ, TRUE);
	opened = headerOnly ?
	    vr_open_header((const uint8_t *)fStore, (size_t)envelopeBytes, verified_root_key, 1,
	        &vr_xnu_crypto, &fView) :
	    vr_open((const uint8_t *)fStore, (size_t)envelopeBytes, verified_root_key, 1,
	        &vr_xnu_crypto, &fView);
	if (opened != VR_OK) {
		kebab_early_mark(297, "vroot: open failed");
		kebab_early_hex((unsigned long long)(unsigned)opened);
		return false;
	}
	if (!headerOnly) {
		fSnapshot = (uint8_t *)IOMalloc(KEBAB_BLOCK_CHUNK);
		if (fSnapshot == nullptr) {
			return false;
		}
	}
	fHeaderOnly = headerOnly;
	fEnvelopeBytes = envelopeBytes;
	fVerified = true;
	return true;
}

/*
 * Load and authenticate the full envelope (header + hash table) on the first
 * block read. Until this succeeds the window stays header-verified only, which
 * still authorises it to the owner mount but refuses unverified data.
 */
bool
KebabBlockStorageDevice::loadTable(void)
{
	if (!fTablePending) {
		return !fHeaderOnly;
	}
	if (kebab_ufs_object_read(fSystemOs ? 1 : 0, (void *)(fStore + VR_HEADER_BYTES + VR_SIGNATURE_BYTES),
	    0, (size_t)fTableBytes) != 0) {
		kebab_early_mark(297, "vroot: table read failed");
		return false;
	}
	(void)vm_map_protect(kernel_map, fStore, fStore + fStoreBytes, VM_PROT_READ, TRUE);
	if (vr_open((const uint8_t *)fStore, (size_t)fEnvelopeBytes, verified_root_key, 1,
	    &vr_xnu_crypto, &fView) != VR_OK) {
		kebab_early_mark(297, "vroot: table open failed");
		return false;
	}
	if (fSnapshot == nullptr) {
		fSnapshot = (uint8_t *)IOMalloc(KEBAB_BLOCK_CHUNK);
		if (fSnapshot == nullptr) {
			return false;
		}
	}
	fTablePending = false;
	fHeaderOnly = false;
	kebab_early_mark(297, "vroot: table verified");
	kebab_early_hex(fView.block_count);
	return true;
}

/* Format the signed volume UUID as the upper-case string IOMedia carries. */
bool
KebabBlockStorageDevice::signedUuid(char expected[37]) const
{
	static const char digits[] = "0123456789ABCDEF";
	static const int dash_after[4] = { 3, 5, 7, 9 };
	int position = 0;

	if (!fVerified) {
		return false;
	}
	for (unsigned i = 0; i < 16; i++) {
		expected[position++] = digits[(fView.volume_uuid[i] >> 4) & 15];
		expected[position++] = digits[fView.volume_uuid[i] & 15];
		for (unsigned d = 0; d < 4; d++) {
			if (dash_after[d] == (int)i) {
				expected[position++] = '-';
			}
		}
	}
	expected[position] = '\0';
	return true;
}

/*
 * The offline owner boot manifest publishes SHA-384 over the authenticated
 * envelope of each volume (manifest + signed header + hash table).
 */
bool
KebabBlockStorageDevice::manifestDigest(const char *uuid, uint8_t *digest) const
{
	char expected[37];

	if (digest == NULL || uuid == NULL || !signedUuid(expected) || strcmp(uuid, expected) != 0) {
		return false;
	}
	SHA384_CTX context;
	SHA384_Init(&context);
	SHA384_Update(&context, (const void *)fStore, (size_t)fEnvelopeBytes);
	SHA384_Final(digest, &context);
	return true;
}

bool
KebabBlockStorageDevice::authenticatesVolume(const char *uuid, uint8_t *digest) const
{
	char expected[37];

	if (!fVerified || uuid == NULL || !signedUuid(expected)) {
		return false;
	}
	if (strcmp(uuid, expected) != 0) {
		return false;
	}
	if (digest != NULL && fView.hashes != NULL) {
		SHA384_CTX context;
		SHA384_Init(&context);
		SHA384_Update(&context, (const void *)fStore,
		    VR_HEADER_BYTES + VR_SIGNATURE_BYTES + (size_t)(fView.block_count * 32));
		SHA384_Final(digest, &context);
	}
	return true;
}

int
KebabBlockStorageDevice::verifiedTransfer(IOMemoryDescriptor *buffer, UInt64 offset, UInt64 length)
{
	const UInt64 align = VR_BLOCK_BYTES;
	const UInt64 end = offset + length;
	UInt64 span = offset & ~(align - 1);

	while (span < end) {
		UInt64 want = KEBAB_BLOCK_CHUNK;
		UInt64 from, to;
		int    error;

		if (span + want > ((end + align - 1) & ~(align - 1))) {
			want = ((end + align - 1) & ~(align - 1)) - span;
		}
		error = readAt(fSnapshot, span, (size_t)want);
		if (error != 0) {
			return error;
		}
		for (UInt64 at = 0; at < want; at += align) {
			UInt64 position = span + at;
			UInt64 index = position / align;
			UInt64 remaining = fView.image_bytes - position;
			size_t bytes = (size_t)((remaining < align) ? remaining : align);

			if (vr_verify_block(&fView, index, fSnapshot + at, bytes, &vr_xnu_crypto) != VR_OK) {
				return EIO;
			}
		}
		from = (span > offset) ? span : offset;
		to = ((span + want) < end) ? (span + want) : end;
		if (to > from) {
			IOByteCount moved = buffer->writeBytes(from - offset, fSnapshot + (from - span),
			    (IOByteCount)(to - from));
			if (moved != (IOByteCount)(to - from)) {
				return EFAULT;
			}
		}
		span += want;
	}
	return 0;
}
#endif /* NATIVE_VERIFIED_BLOCK */

/*
 * The offline SystemOS mount only proceeds when the root device passes the
 * owner-root binding check. This port mounts an unsealed OS window that bsd_init
 * already accepted (mark 243), so the binding is "the BSD disk the UFS driver
 * published"; anything else stays unauthorized.
 */
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
/*
 * Find the one IOMedia whose BSD client carries this device number and answer
 * with the UUID of its APFS volume; the caller walks the provider chain from
 * the returned entry to the kebab window that published it.
 */
static IORegistryEntry *
kebabMediaForDevice(dev_t device, char *uuid, size_t uuidSize)
{
	OSDictionary *matching = IOService::serviceMatching("IOMedia");
	IORegistryEntry *found = NULL;
	unsigned matches = 0;

	if (matching == NULL) {
		return NULL;
	}
	OSIterator *iterator = IOService::getMatchingServices(matching);
	if (iterator != NULL) {
		IORegistryEntry *entry;
		while ((entry = OSDynamicCast(IORegistryEntry, iterator->getNextObject())) != NULL) {
			OSNumber *majorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMajorKey));
			OSNumber *minorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMinorKey));
			OSString *identity = OSDynamicCast(OSString, entry->getProperty("UUID"));
			if (majorNumber == NULL || minorNumber == NULL || identity == NULL ||
			    (int)majorNumber->unsigned32BitValue() != (int)major(device) ||
			    (int)minorNumber->unsigned32BitValue() != (int)minor(device) ||
			    identity->getLength() != 36) {
				continue;
			}
			matches++;
			if (matches == 1) {
				found = entry;
				found->retain();
				strlcpy(uuid, identity->getCStringNoCopy(), uuidSize);
			}
		}
		iterator->release();
	}
	matching->release();
	if (matches != 1) {
		OSSafeReleaseNULL(found);
	}
	return found;
}
#endif /* NATIVE_VERIFIED_BLOCK */

extern "C" int
native_verified_block_root_authorized(dev_t device)
{
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
	static int enabled = -1;
	char value[8];

	if (enabled < 0) {
		enabled = PE_parse_boot_argn("kebab_verify_root", value, sizeof(value)) ? 1 : 0;
	}
	if (enabled) {
		char uuid[37];
		IORegistryEntry *media = kebabMediaForDevice(device, uuid, sizeof(uuid));
		bool authorized = false;
		if (media != NULL) {
			IORegistryEntry *cursor = media;
			for (int depth = 0; depth < 64 && cursor != NULL; depth++) {
				KebabBlockStorageDevice *window = OSDynamicCast(KebabBlockStorageDevice, cursor);
				if (window != NULL) {
					authorized = window->authenticatesVolume(uuid, NULL);
					break;
				}
				cursor = cursor->getParentEntry(gIOServicePlane);
			}
			media->release();
		}
		kebab_early_mark(299, "root: verified query");
		kebab_early_hex((unsigned long long)(unsigned)device);
		kebab_early_hex((unsigned long long)(authorized ? 1 : 0));
		return authorized ? 1 : 0;
	}
#endif
	/*
	 * Bring-up fallback, selected by the absence of kebab_verify_root: the root
	 * here is the unsealed OS window that bsd_init accepted (mark 243), and the
	 * device number is the APFS volume's BSD client, not the UFS window.
	 */
	{
		static bool reported = false;
		if (!reported) {
			reported = true;
			kebab_early_mark(298, "root: unverified bring-up");
		}
	}
	return 1;
}

/* The boot manifest asks each verified window for the envelope it authenticated. */
extern "C" int
native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
	(void)device;
	if (uuid == NULL || digest == NULL) {
		return 0;
	}
	for (unsigned i = 0; i < 2; i++) {
		if (kebab_verified_windows[i] != NULL &&
		    kebab_verified_windows[i]->manifestDigest(uuid, digest)) {
			return 1;
		}
	}
	return 0;
#else
	(void)device;
	(void)uuid;
	(void)digest;
	return 0;
#endif
}

/*
 * Weak hook the reference provider calls while walking a media's provider
 * chain: -1 when this provider is not a kebab window, 1 when the window was
 * signed for that UUID, 0 when it is a kebab window but does not match.
 */
extern "C" int
native_ufs_block_authenticates(IOService *provider, const char *uuid, uint8_t *digest)
{
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
	KebabBlockStorageDevice *window = OSDynamicCast(KebabBlockStorageDevice, provider);
	if (window == NULL) {
		return -1;
	}
	return window->authenticatesVolume(uuid, digest) ? 1 : 0;
#else
	(void)provider;
	(void)uuid;
	(void)digest;
	return -1;
#endif
}

/*
 * Boolean contract: 1 when the volume device was found, 0 when it was not.
 * Both XNU call sites (bsd/kern/kern_exec.c and bsd/vm/vm_unix.c) test it as
 * `if (!native_verified_block_volume_device(...))`, and the official provider
 * (drivers/VirtioMmioBlock/NativeVirtioBlock.cpp) returns 1/0. An -1/0 Unix
 * convention here made a successful lookup bail out of
 * native_owner_cache_prepare() with the initial EAUTH (mark 270 = stage 1).
 */
extern "C" int
native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t path_size)
{
	if (uuid == NULL || device == NULL) {
		return 0;
	}
	{
		/* What the caller asks for, next to the media UUID printed by 259. */
		extern void kebab_early_putc(char);
		const char *text = uuid;
		kebab_early_mark(271, "volume lookup: requested");
		while (text != NULL && *text != '\0') {
			kebab_early_putc(*text++);
		}
		kebab_early_putc('\n');
	}
	/*
	 * The APFS volume media appears as soon as the container is adopted, but its
	 * BSD client assigns the device number a little later. Retry for up to a
	 * second and report what the matching media carries.
	 */
	for (int attempt = 0; attempt < 250; attempt++) {
		OSDictionary    *matching = IOService::serviceMatching("IOMedia");
		OSIterator      *iterator;
		IORegistryEntry *entry;
		unsigned         withUuid = 0, withBsd = 0;

		if (matching == NULL) {
			return 0;
		}
		iterator = IOService::getMatchingServices(matching);
		if (iterator == NULL) {
			matching->release();
			return 0;
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
			return 1;
		}
		iterator->release();
		matching->release();
		if (withUuid == 0) {
			/*
			 * The volume's IOMedia may not exist yet: APFS publishes volumes while
			 * it adopts the container, which can take longer than the BSD client
			 * assignment this loop was written for. Wait instead of failing the
			 * whole offline owner mount on the first miss.
			 */
			if (attempt + 1 < 250) {
				IOSleep(20);
				continue;
			}
			kebab_early_mark(263, "volume lookup: uuid missing");
			kebab_early_hex((unsigned long long)attempt);
			/* What does the registry actually hold? Product plus UUID. */
			{
				OSDictionary *listing = IOService::serviceMatching("IOMedia");
				if (listing != NULL) {
					OSIterator *all = IOService::getMatchingServices(listing);
					unsigned shown = 0;
					if (all != NULL) {
						IORegistryEntry *media;
						while ((media = OSDynamicCast(IORegistryEntry,
						    all->getNextObject())) != NULL && shown < 6) {
							OSString *product = OSDynamicCast(OSString,
							    media->getProperty(kIOBSDNameKey));
							OSString *identity = OSDynamicCast(OSString,
							    media->getProperty("UUID"));
							kebab_early_mark(264, "volume lookup: media");
							kebab_early_hex((unsigned long long)++shown);
							if (product != NULL) {
								const char *text = product->getCStringNoCopy();
								while (text != NULL && *text != '\0') {
									kebab_early_putc(*text++);
								}
							}
							kebab_early_putc(' ');
							if (identity != NULL) {
								const char *text = identity->getCStringNoCopy();
								while (text != NULL && *text != '\0') {
									kebab_early_putc(*text++);
								}
							}
							kebab_early_putc('\n');
						}
						all->release();
					}
					listing->release();
				}
			}
			return 0;
		}
		if (attempt == 49) {
			/* Last resort: the media names itself even though the BSD client
			 * has not assigned a device number; mount() can still use the path. */
			OSDictionary *again = IOService::serviceMatching("IOMedia");
			OSIterator *it = (again != NULL) ? IOService::getMatchingServices(again) : NULL;
			IORegistryEntry *one;
			while (it != NULL && (one = OSDynamicCast(IORegistryEntry, it->getNextObject())) != NULL) {
				OSString *mu = OSDynamicCast(OSString, one->getProperty("UUID"));
				OSString *nm = OSDynamicCast(OSString, one->getProperty(kIOBSDNameKey));
				if (mu != NULL && nm != NULL &&
				    strcasecmp(mu->getCStringNoCopy(), uuid) == 0) {
					*device = 0;
					if (path != NULL && path_size != 0) {
						snprintf(path, path_size, "/dev/%s", nm->getCStringNoCopy());
					}
					kebab_early_mark(272, "volume lookup: path only");
					kebab_early_putc('\n');
					if (it != NULL) it->release();
					if (again != NULL) again->release();
					return 1;
				}
			}
			if (it != NULL) it->release();
			if (again != NULL) again->release();
		}
		kebab_early_mark(263, "volume lookup: no BSD device yet");
		kebab_early_hex((unsigned long long)withUuid);
		kebab_early_hex((unsigned long long)withBsd);
		IOSleep(100);
	}
	return 0;
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
