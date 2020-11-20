// SPDX-License-Identifier: GPL-2.0

#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <asm/sgx.h>
#include <uapi/asm/sgx.h>

#include "encls.h"
#include "sgx.h"
#include "virt.h"

struct sgx_virt_epc {
	struct xarray page_array;
	struct rw_semaphore lock;
	struct mm_struct *mm;
};

static struct mutex virt_epc_lock;
static struct list_head virt_epc_zombie_pages;

static inline unsigned long sgx_virt_epc_calc_index(struct vm_area_struct *vma,
						    unsigned long addr)
{
	return vma->vm_pgoff + PFN_DOWN(addr - vma->vm_start);
}

static int __sgx_virt_epc_fault(struct sgx_virt_epc *epc,
				struct vm_area_struct *vma, unsigned long addr)
{
	struct sgx_epc_page *epc_page;
	unsigned long index, pfn;
	int ret;

	index = sgx_virt_epc_calc_index(vma, addr);

	epc_page = xa_load(&epc->page_array, index);
	if (epc_page)
		return 0;

	epc_page = sgx_alloc_epc_page(&epc, false);
	if (IS_ERR(epc_page))
		return PTR_ERR(epc_page);

	ret = xa_err(xa_store(&epc->page_array, index, epc_page, GFP_KERNEL));
	if (unlikely(ret))
		goto err_free;

	pfn = PFN_DOWN(sgx_get_epc_phys_addr(epc_page));

	ret = vmf_insert_pfn(vma, addr, pfn);
	if (unlikely(ret != VM_FAULT_NOPAGE)) {
		ret = -EFAULT;
		goto err_delete;
	}

	return 0;

err_delete:
	xa_erase(&epc->page_array, index);
err_free:
	sgx_free_epc_page(epc_page);
	return ret;
}

static vm_fault_t sgx_virt_epc_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct sgx_virt_epc *epc = vma->vm_private_data;
	int ret;

	down_write(&epc->lock);
	ret = __sgx_virt_epc_fault(epc, vma, vmf->address);
	up_write(&epc->lock);

	if (!ret || signal_pending(current))
		return VM_FAULT_NOPAGE;

	if (ret == -EBUSY && (vmf->flags & FAULT_FLAG_ALLOW_RETRY)) {
		mmap_read_unlock(vma->vm_mm);
		return VM_FAULT_RETRY;
	}

	return VM_FAULT_SIGBUS;
}

static int sgx_virt_epc_access(struct vm_area_struct *vma, unsigned long start,
			       void *buf, int len, int write)
{
	/* EDBG{RD,WR} are naturally sized, i.e. always 8-byte on 64-bit. */
	unsigned char data[sizeof(unsigned long)];
	struct sgx_epc_page *epc_page;
	struct sgx_virt_epc *epc;
	unsigned long addr, index;
	int offset, cnt, i;
	int ret = 0;
	void *p;

	epc = vma->vm_private_data;

	for (i = 0; i < len && !ret; i += cnt) {
		addr = start + i;
		if (i == 0 || PFN_DOWN(addr) != PFN_DOWN(addr - cnt))
			index = sgx_virt_epc_calc_index(vma, addr);

		down_write(&epc->lock);
		epc_page = xa_load(&epc->page_array, index);

		/*
		 * EDBG{RD,WR} require an active enclave and virtual EPC does
		 * not support reclaim.  A non-existent entry means the guest
		 * hasn't accessed the page and therefore can't possibility
		 * have added the page to an enclave.
		 */
		if (!epc_page) {
			up_write(&epc->lock);
			return -EIO;
		}

		offset = addr & (sizeof(unsigned long) - 1);
		addr = ALIGN_DOWN(addr, sizeof(unsigned long));
		cnt = min((int)sizeof(unsigned long) - offset, len - i);

		p = sgx_get_epc_virt_addr(epc_page) + (addr & ~PAGE_MASK);

		/* EDBGRD for read, or to do RMW for a partial write. */
		if (!write || cnt != sizeof(unsigned long))
			ret = __edbgrd(p, (void *)data);

		if (!ret) {
			if (write) {
				memcpy(data + offset, buf + i, cnt);
				ret = __edbgwr(p, (void *)data);
			} else {
				memcpy(buf + i, data + offset, cnt);
			}
		}
		up_write(&epc->lock);
	}

	if (ret)
		return -EIO;
	return i;
}

const struct vm_operations_struct sgx_virt_epc_vm_ops = {
	.fault = sgx_virt_epc_fault,
	.access = sgx_virt_epc_access,
};

static int sgx_virt_epc_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct sgx_virt_epc *epc = file->private_data;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	if (vma->vm_mm != epc->mm)
		return -EINVAL;

	vma->vm_ops = &sgx_virt_epc_vm_ops;
	vma->vm_flags |= VM_PFNMAP | VM_IO | VM_DONTDUMP;
	vma->vm_private_data = file->private_data;

	return 0;
}

static int sgx_virt_epc_free_page(struct sgx_epc_page *epc_page)
{
	int ret;

	if (!epc_page)
		return 0;

	ret = __eremove(sgx_get_epc_virt_addr(epc_page));
	if (ret) {
		WARN_ON_ONCE(ret != SGX_CHILD_PRESENT);
		return ret;
	}

	__sgx_free_epc_page(epc_page);
	return 0;
}

static int sgx_virt_epc_release(struct inode *inode, struct file *file)
{
	struct sgx_virt_epc *epc = file->private_data;
	struct sgx_epc_page *epc_page, *tmp, *entry;
	unsigned long index;

	LIST_HEAD(secs_pages);

	mmdrop(epc->mm);

	xa_for_each(&epc->page_array, index, entry) {
		if (sgx_virt_epc_free_page(entry))
			continue;

		xa_erase(&epc->page_array, index);
	}

	/*
	 * Because we don't track which pages are SECS pages, it's possible
	 * for EREMOVE to fail, e.g. a SECS page can have children if a VM
	 * shutdown unexpectedly.  Retry all failed pages after iterating
	 * through the entire tree, at which point all children should be
	 * removed and the SECS pages can be nuked as well...unless userspace
	 * has exposed multiple instance of virtual EPC to a single VM.
	 */
	xa_for_each(&epc->page_array, index, entry) {
		epc_page = entry;
		if (sgx_virt_epc_free_page(epc_page))
			list_add_tail(&epc_page->list, &secs_pages);

		xa_erase(&epc->page_array, index);
	}

	/*
	 * Third time's a charm.  Try to EREMOVE zombie SECS pages from virtual
	 * EPC instances that were previously released, i.e. free SECS pages
	 * that were in limbo due to having children in *this* EPC instance.
	 */
	mutex_lock(&virt_epc_lock);
	list_for_each_entry_safe(epc_page, tmp, &virt_epc_zombie_pages, list) {
		/*
		 * Speculatively remove the page from the list of zombies, if
		 * the page is successfully EREMOVE it will be added to the
		 * list of free pages.  If EREMOVE fails, throw the page on the
		 * local list, which will be spliced on at the end.
		 */
		list_del(&epc_page->list);

		if (sgx_virt_epc_free_page(epc_page))
			list_add_tail(&epc_page->list, &secs_pages);
	}

	if (!list_empty(&secs_pages))
		list_splice_tail(&secs_pages, &virt_epc_zombie_pages);
	mutex_unlock(&virt_epc_lock);

	kfree(epc);

	return 0;
}

static int sgx_virt_epc_open(struct inode *inode, struct file *file)
{
	struct sgx_virt_epc *epc;

	epc = kzalloc(sizeof(struct sgx_virt_epc), GFP_KERNEL);
	if (!epc)
		return -ENOMEM;

	mmgrab(current->mm);
	epc->mm = current->mm;
	init_rwsem(&epc->lock);
	xa_init(&epc->page_array);

	file->private_data = epc;

	return 0;
}

static const struct file_operations sgx_virt_epc_fops = {
	.owner			= THIS_MODULE,
	.open			= sgx_virt_epc_open,
	.release		= sgx_virt_epc_release,
	.mmap			= sgx_virt_epc_mmap,
};

static struct miscdevice sgx_virt_epc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "sgx_virt_epc",
	.nodename = "sgx_virt_epc",
	.fops = &sgx_virt_epc_fops,
};

int __init sgx_virt_epc_init(void)
{
	INIT_LIST_HEAD(&virt_epc_zombie_pages);
	mutex_init(&virt_epc_lock);

	return misc_register(&sgx_virt_epc_dev);
}
