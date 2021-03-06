/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011, 2015 by Delphix. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_file.h>
#include <sys/vdev_impl.h>
#include <sys/zio.h>
#include <sys/fs/zfs.h>
#include <sys/fm/fs/zfs.h>
#include <sys/vnode.h>

/*
 * Virtual device vector for files.
 */

static taskq_t *vdev_file_taskq;

static void
vdev_file_hold(vdev_t *vd)
{
	ASSERT(vd->vdev_path != NULL);
}

static void
vdev_file_rele(vdev_t *vd)
{
	ASSERT(vd->vdev_path != NULL);
}

#ifdef _KERNEL
extern int VOP_GETATTR(struct vnode *vp, vattr_t *vap, int flags, void *x3, void *x4);
#endif

static int
vdev_file_open(vdev_t *vd, uint64_t *psize, uint64_t *max_psize,
    uint64_t *ashift)
{
#if _KERNEL
	static vattr_t vattr;
	vdev_file_t *vf;
#endif
	int error = 0;

    dprintf("vdev_file_open %p\n", vd->vdev_tsd);
	/* Rotational optimizations only make sense on block devices */
	vd->vdev_nonrot = B_TRUE;

	/*
	 * We must have a pathname, and it must be absolute.
	 */
	if (vd->vdev_path == NULL || (vd->vdev_path[0] != '/' &&
		vd->vdev_path[0] != '\\')) {
		vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Reopen the device if it's not currently open.  Otherwise,
	 * just update the physical size of the device.
	 */
#ifdef _KERNEL
	if (vd->vdev_tsd != NULL) {
		ASSERT(vd->vdev_reopening);
		vf = vd->vdev_tsd;
		goto skip_open;
	}

	vf = vd->vdev_tsd = kmem_zalloc(sizeof (vdev_file_t), KM_SLEEP);
#endif

	/*
	 * We always open the files from the root of the global zone, even if
	 * we're in a local zone.  If the user has gotten to this point, the
	 * administrator has already decided that the pool should be available
	 * to local zone users, so the underlying devices should be as well.
	 */
	ASSERT(vd->vdev_path != NULL && (
		vd->vdev_path[0] == '/' || vd->vdev_path[0] == '\\'));

    /*
      vn_openat(char *pnamep,
      enum uio_seg seg,
      int filemode,
      int createmode,
      struct vnode **vpp,
      enum create crwhy,
      mode_t umask,
      struct vnode *startvp)
      extern int vn_openat(char *pnamep, enum uio_seg seg, int filemode,
      int createmode, struct vnode **vpp, enum create crwhy,
      mode_t umask, struct vnode *startvp);
    */
	uint8_t *FileName = NULL;
	FileName = vd->vdev_path;

	if (!strncmp("\\\\?\\", FileName, 4)) {
		FileName[1] = '?';
	}

	dprintf("%s: opening '%s'\n", __func__, FileName);

#ifdef _KERNEL

	ANSI_STRING         AnsiFilespec;
	UNICODE_STRING      UnicodeFilespec;
	OBJECT_ATTRIBUTES   ObjectAttributes;

	SHORT                   UnicodeName[PATH_MAX];
	CHAR                    AnsiName[PATH_MAX];
	USHORT                  NameLength = 0;
	NTSTATUS ntstatus;

	memset(UnicodeName, 0, sizeof(SHORT) * PATH_MAX);
	memset(AnsiName, 0, sizeof(UCHAR) * PATH_MAX);

	NameLength = strlen(FileName);
	ASSERT(NameLength < PATH_MAX);

	memmove(AnsiName, FileName, NameLength);

	AnsiFilespec.MaximumLength = AnsiFilespec.Length = NameLength;
	AnsiFilespec.Buffer = AnsiName;

	UnicodeFilespec.MaximumLength = PATH_MAX * 2;
	UnicodeFilespec.Length = 0;
	UnicodeFilespec.Buffer = (PWSTR)UnicodeName;

	RtlAnsiStringToUnicodeString(&UnicodeFilespec, &AnsiFilespec, FALSE);

	ObjectAttributes.Length = sizeof(OBJECT_ATTRIBUTES);
	ObjectAttributes.RootDirectory = NULL;
	ObjectAttributes.Attributes = /*OBJ_CASE_INSENSITIVE |*/ OBJ_KERNEL_HANDLE;
	ObjectAttributes.ObjectName = &UnicodeFilespec;
	ObjectAttributes.SecurityDescriptor = NULL;
	ObjectAttributes.SecurityQualityOfService = NULL;
	IO_STATUS_BLOCK iostatus;

	ntstatus = ZwCreateFile(&vf->vf_handle,
		spa_mode(vd->vdev_spa) == FREAD ? GENERIC_READ | SYNCHRONIZE : GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
		&ObjectAttributes,
		&iostatus,
		0,
		FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_WRITE | FILE_SHARE_READ,
		FILE_OPEN,
		FILE_SYNCHRONOUS_IO_NONALERT | (spa_mode(vd->vdev_spa) == FREAD ? 0 : FILE_NO_INTERMEDIATE_BUFFERING),
		NULL,
		0);

	if (ntstatus == STATUS_SUCCESS) {
		error = 0;
	} else {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		goto failed;
	}


	/*
	* Make sure it's a regular file.
	*/
	FILE_STANDARD_INFORMATION info;
	IO_STATUS_BLOCK iob;

	if ((ZwQueryInformationFile(
		vf->vf_handle,
		&iob,
		&info,
		sizeof(info),
		FileStandardInformation) != STATUS_SUCCESS) ||
		(info.Directory != FALSE)) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		ZwClose(vf->vf_handle);
		error = ENOENT;
		goto failed;
	}

	// Since we will use DeviceObject and FileObject to do ioctl and IO
	// we grab them now and lock them in place.
	// Convert HANDLE to FileObject
	PFILE_OBJECT        FileObject;
	PDEVICE_OBJECT      DeviceObject;
	NTSTATUS status;

	// This adds a reference to FileObject
	status = ObReferenceObjectByHandle(
		vf->vf_handle, 
		0,
		*IoFileObjectType,
		KernelMode,
		&FileObject,
		NULL
	);
	if (status != STATUS_SUCCESS) {
		ZwClose(vf->vf_handle);
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		error = EIO;
		goto failed;
	}

	// Convert FileObject to DeviceObject
	DeviceObject = IoGetRelatedDeviceObject(FileObject);

	// Grab a reference to DeviceObject
	ObReferenceObject(DeviceObject);

	vf->vf_FileObject = FileObject;
	vf->vf_DeviceObject = DeviceObject;

#endif

#if _KERNEL
skip_open:
	/*
	 * Determine the physical size of the file.
	 */
	//vattr.va_mask = AT_SIZE;
    //vn_lock(vf->vf_vnode, LK_SHARED | LK_RETRY);
	//error = VOP_GETATTR(vf->vf_vnode, &vattr, 0, kcred, NULL);
    //VN_UNLOCK(vf->vf_vnode);
#endif

#ifdef _KERNEL
	*max_psize = *psize = info.EndOfFile.QuadPart;
#else
    /* userland's vn_open() will get the device size for us, so we can
     * just look it up - there is argument for a userland VOP_GETATTR to make
     * this function cleaner. */
//	*max_psize = *psize = vp->v_size;
#endif
    *ashift = SPA_MINBLOCKSHIFT;

	return (0);

failed:
#ifdef _KERNEL
	if (vf) {
		if (vf->vf_handle != NULL) {
			vf->vf_handle = NULL;
		}

		kmem_free(vf, sizeof(vdev_file_t));
		vd->vdev_tsd = NULL;
	}
#endif
	return error;
}

static void
vdev_file_close(vdev_t *vd)
{
#ifdef _KERNEL
	vdev_file_t *vf = vd->vdev_tsd;

	if (vd->vdev_reopening || vf == NULL)
		return;

	if (vf->vf_handle != NULL) {

		// Release our holds
		ObDereferenceObject(vf->vf_FileObject);
		ObDereferenceObject(vf->vf_DeviceObject);

		ZwClose(vf->vf_handle);
	}

	vf->vf_FileObject = NULL;
	vf->vf_DeviceObject = NULL;
	vf->vf_handle = NULL;
	vd->vdev_delayed_close = B_FALSE;
	kmem_free(vf, sizeof (vdev_file_t));
	vd->vdev_tsd = NULL;
#endif
}

#ifdef _KERNEL
struct vdev_file_callback_struct {
	KEVENT Event;
	zio_t *zio;
	PIRP irp;
	void *b_data;
};
typedef struct vdev_file_callback_struct vf_callback_t;


static NTSTATUS
vdev_file_io_intrxxx(PDEVICE_OBJECT DeviceObject, PIRP irp, PVOID Context)
{
	KEVENT *kevent = Context;

//	dprintf("%s: event\n", __func__);
	KeSetEvent(kevent, 0, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}


static void
vdev_file_io_intr(void *Context)
{
	vf_callback_t *vb = (vf_callback_t *)Context;
	zio_t *zio = vb->zio;
	PIRP irp = vb->irp;

	// Wait for IoCompletionRoutine to have been called.
	KeWaitForSingleObject(&vb->Event, Executive, KernelMode, FALSE, NULL); // SYNC

//	dprintf("%s: done\n", __func__);

	/*
	* The rest of the zio stack only deals with EIO, ECKSUM, and ENXIO.
	* Rather than teach the rest of the stack about other error
	* possibilities (EFAULT, etc), we normalize the error value here.
	*/
	//	zio->io_error = (geterror(bp) != 0 ? EIO : 0);

	//	if (zio->io_error == 0 && bp->b_resid != 0)
	//		zio->io_error = SET_ERROR(EIO);
	zio->io_error = (irp->IoStatus.Status != 0 ? EIO : 0);
	if (zio->io_type == ZIO_TYPE_READ) {
		if (zio->io_abd->abd_size == zio->io_size) {
			abd_return_buf_copy(zio->io_abd, vb->b_data, zio->io_size);
		} else {
			VERIFY3S(zio->io_abd->abd_size, >= , zio->io_size);
			abd_return_buf_copy_off(zio->io_abd, vb->b_data,
				0, zio->io_size, zio->io_abd->abd_size);
		}
	} else {
		VERIFY3S(zio->io_abd->abd_size, >= , zio->io_size);
		abd_return_buf_off(zio->io_abd, vb->b_data, 0, zio->io_size, zio->io_abd->abd_size);
	}

	if (irp->IoStatus.Information != zio->io_size)
		dprintf("%s: size mismatch 0x%llx != 0x%llx\n",
			irp->IoStatus.Information, zio->io_size);

	// Release irp
	if (irp) {
		while (irp->MdlAddress != NULL) {
			PMDL NextMdl;
			NextMdl = irp->MdlAddress->Next;
			MmUnlockPages(irp->MdlAddress);
			IoFreeMdl(irp->MdlAddress);
			irp->MdlAddress = NextMdl;
		}
		IoFreeIrp(irp);
	}
	irp = NULL;

	kmem_free(vb, sizeof(vf_callback_t));
	vb = NULL;

	zio_delay_interrupt(zio);

	thread_exit();
}
#endif

/*
 * count the number of mismatches of zio->io_size and zio->io_abd->abd_size below
 */
_Atomic uint64_t zfs_vdev_file_size_mismatch_cnt = 0;

static void
vdev_file_io_start(zio_t *zio)
{
    vdev_t *vd = zio->io_vd;
    ssize_t resid = 0;


    if (zio->io_type == ZIO_TYPE_IOCTL) {

        if (!vdev_readable(vd)) {
            zio->io_error = SET_ERROR(ENXIO);
			zio_interrupt(zio);
            return;
        }

        switch (zio->io_cmd) {
        case DKIOCFLUSHWRITECACHE:
#if 0
			if (!vnode_getwithvid(vf->vf_vnode, vf->vf_vid)) {
                zio->io_error = VOP_FSYNC(vf->vf_vnode, FSYNC | FDSYNC,
                                          kcred, NULL);
                vnode_put(vf->vf_vnode);
            }
#endif
			break;
        default:
            zio->io_error = SET_ERROR(ENOTSUP);
        }

		zio_interrupt(zio);
        return;
    }

	ASSERT(zio->io_type == ZIO_TYPE_READ || zio->io_type == ZIO_TYPE_WRITE);
	zio->io_target_timestamp = zio_handle_io_delay(zio);


	ASSERT(zio->io_size != 0);

#ifdef _KERNEL
	vdev_file_t *vf = vd->vdev_tsd;

	NTSTATUS status;
	PIRP irp = NULL;
	PIO_STACK_LOCATION irpStack = NULL;
	//KEVENT completionEvent;

	IO_STATUS_BLOCK IoStatusBlock;
	LARGE_INTEGER offset;

	offset.QuadPart = zio->io_offset + vd->vdev_win_offset;

	vf_callback_t *vb = (vf_callback_t *)kmem_alloc(sizeof(vf_callback_t), KM_SLEEP);
	vb->zio = zio;
	KeInitializeEvent(&vb->Event, NotificationEvent, FALSE);

#ifdef DEBUG
	if (zio->io_abd->abd_size != zio->io_size) {
		zfs_vdev_file_size_mismatch_cnt++;
		// this dprintf can be very noisy
		dprintf("ZFS: %s: trimming zio->io_abd from 0x%x to 0x%llx\n", 
			__func__, zio->io_abd->abd_size, zio->io_size);
	}
#endif

	if (zio->io_type == ZIO_TYPE_READ) {
		ASSERT3S(zio->io_abd->abd_size, >= , zio->io_size);
		vb->b_data =
			abd_borrow_buf(zio->io_abd, zio->io_abd->abd_size);
	} else {
		ASSERT3S(zio->io_abd->abd_size, >= , zio->io_size);
		vb->b_data =
			abd_borrow_buf_copy(zio->io_abd, zio->io_abd->abd_size);
	}

	if (zio->io_type == ZIO_TYPE_READ) {
		irp = IoBuildAsynchronousFsdRequest(IRP_MJ_READ,
			vf->vf_DeviceObject,
			vb->b_data,
			(ULONG)zio->io_size,
			&offset,
			&IoStatusBlock);
	} else {
		irp = IoBuildAsynchronousFsdRequest(IRP_MJ_WRITE,
			vf->vf_DeviceObject,
			vb->b_data,
			(ULONG)zio->io_size,
			&offset,
			&IoStatusBlock);
	}

	if (!irp) {
		kmem_free(vb, sizeof(vf_callback_t));
		zio->io_error = EIO;
		zio_interrupt(zio);
		return;
	}

	vb->irp = irp;

	irpStack = IoGetNextIrpStackLocation(irp);

	irpStack->Flags |= SL_OVERRIDE_VERIFY_VOLUME; // SetFlag(IoStackLocation->Flags, SL_OVERRIDE_VERIFY_VOLUME);
												  //SetFlag(ReadIrp->Flags, IRP_NOCACHE);
	irpStack->FileObject = vf->vf_FileObject;

	IoSetCompletionRoutine(irp,
		vdev_file_io_intrxxx,
		&vb->Event, // "Context" in vdev_disk_io_intr()
		TRUE, // On Success
		TRUE, // On Error
		TRUE);// On Cancel

			  // Start a thread to wait for IO completion, which is signalled
			  // by CompletionRouting setting event.
	(void)thread_create(NULL, 0, vdev_file_io_intr, vb, 0, &p0,
		TS_RUN, minclsyspri);

	status = IoCallDriver(vf->vf_DeviceObject, irp);

#endif

    return;
}


/* ARGSUSED */
static void
vdev_file_io_done(zio_t *zio)
{
}

vdev_ops_t vdev_file_ops = {
	vdev_file_open,
	vdev_file_close,
	vdev_default_asize,
	vdev_file_io_start,
	vdev_file_io_done,
	NULL,
	NULL,
	vdev_file_hold,
	vdev_file_rele,
	NULL,
	vdev_default_xlate,
	VDEV_TYPE_FILE,		/* name of this vdev type */
	B_TRUE			/* leaf vdev */
};

void
vdev_file_init(void)
{
	vdev_file_taskq = taskq_create("vdev_file_taskq", 100, minclsyspri,
	    max_ncpus, INT_MAX, TASKQ_PREPOPULATE | TASKQ_THREADS_CPU_PCT);

	VERIFY(vdev_file_taskq);
}

void
vdev_file_fini(void)
{
	taskq_destroy(vdev_file_taskq);
}

/*
 * From userland we access disks just like files.
 */
#ifndef _KERNEL

vdev_ops_t vdev_disk_ops = {
	vdev_file_open,
	vdev_file_close,
	vdev_default_asize,
	vdev_file_io_start,
	vdev_file_io_done,
	NULL,
	NULL,
	vdev_file_hold,
	vdev_file_rele,
	NULL,
	vdev_default_xlate,
	VDEV_TYPE_DISK,		/* name of this vdev type */
	B_TRUE			/* leaf vdev */
};

#endif
