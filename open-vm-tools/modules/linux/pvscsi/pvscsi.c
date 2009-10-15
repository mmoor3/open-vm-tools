/*********************************************************
 * Copyright (C) 2008 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 *********************************************************/

/*
 * pvscsi.c --
 *
 *      This is a driver for the VMware PVSCSI HBA adapter.
 */

#include "driver-config.h"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>

#include "compat_scsi.h"
#include "compat_pci.h"
#include "compat_interrupt.h"
#include "compat_workqueue.h"

#include "pvscsi_defs.h"
#include "pvscsi_version.h"
#include "scsi_defs.h"
#include "vm_device_version.h"
#include "vm_assert.h"

#define PVSCSI_LINUX_DRIVER_DESC "VMware PVSCSI driver"

MODULE_DESCRIPTION(PVSCSI_LINUX_DRIVER_DESC);
MODULE_AUTHOR("VMware, Inc.");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(PVSCSI_DRIVER_VERSION_STRING);

/*
 * Starting with SLE10sp2, Novell requires that IHVs sign a support agreement
 * with them and mark their kernel modules as externally supported via a
 * change to the module header. If this isn't done, the module will not load
 * by default (i.e., neither mkinitrd nor modprobe will accept it).
 */
MODULE_INFO(supported, "external");

#define PVSCSI_DEFAULT_NUM_PAGES_PER_RING	8
#define PVSCSI_DEFAULT_NUM_PAGES_MSG_RING	1
#define PVSCSI_DEFAULT_QUEUE_DEPTH		64

/* MSI has horrible performance in < 2.6.13 due to needless mask frotzing */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 13)
#define PVSCSI_DISABLE_MSI	0
#else
#define PVSCSI_DISABLE_MSI	1
#endif

/* MSI-X has horrible performance in < 2.6.19 due to needless mask frobbing */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 19)
#define PVSCSI_DISABLE_MSIX	0
#else
#define PVSCSI_DISABLE_MSIX	1
#endif

struct pvscsi_sg_list {
	PVSCSISGElement sge[PVSCSI_MAX_NUM_SG_ENTRIES_PER_SEGMENT];
};

struct pvscsi_ctx {
	/*
	 * The index of the context in cmd_map serves as the context ID for a
	 * 1-to-1 mapping completions back to requests.
	 */
	struct scsi_cmnd	*cmd;
	struct pvscsi_sg_list	*sgl;
	struct list_head	list;
	dma_addr_t		dataPA;
	dma_addr_t		sensePA;
	dma_addr_t		sglPA;
};

struct pvscsi_adapter {
	char			*mmioBase;
	unsigned int		irq;
	u8			rev;
	char			use_msi;
	char			use_msix;
	char			use_msg;

	spinlock_t		hw_lock;

	struct workqueue_struct	*workqueue;
	struct work_struct	work;

	PVSCSIRingReqDesc	*req_ring;
	unsigned		req_pages;
	unsigned		req_depth;
	dma_addr_t		reqRingPA;

	PVSCSIRingCmpDesc	*cmp_ring;
	unsigned		cmp_pages;
	dma_addr_t		cmpRingPA;

	PVSCSIRingMsgDesc	*msg_ring;
	unsigned		msg_pages;
	dma_addr_t		msgRingPA;

	PVSCSIRingsState	*rings_state;
	dma_addr_t		ringStatePA;

	struct pci_dev		*dev;
	struct Scsi_Host	*host;

	struct list_head	cmd_pool;
	struct pvscsi_ctx	*cmd_map;
};

#define HOST_ADAPTER(host) ((struct pvscsi_adapter *)(host)->hostdata)

#define LOG(level, fmt, args...)				\
do {								\
	if (pvscsi_debug_level > level)				\
		printk(KERN_DEBUG "pvscsi: " fmt, args);	\
} while (0)


/* Command line parameters */
static int pvscsi_debug_level;
static int pvscsi_ring_pages     = PVSCSI_DEFAULT_NUM_PAGES_PER_RING;
static int pvscsi_msg_ring_pages = PVSCSI_DEFAULT_NUM_PAGES_MSG_RING;
static int pvscsi_cmd_per_lun    = PVSCSI_DEFAULT_QUEUE_DEPTH;
static int pvscsi_disable_msi    = PVSCSI_DISABLE_MSI;
static int pvscsi_disable_msix   = PVSCSI_DISABLE_MSIX;
static int pvscsi_use_msg        = TRUE;

#define PVSCSI_RW (S_IRUSR | S_IWUSR)

module_param_named(debug_level, pvscsi_debug_level, int, PVSCSI_RW);
MODULE_PARM_DESC(debug_level, "Debug logging level - (default=0)");

module_param_named(ring_pages, pvscsi_ring_pages, int, PVSCSI_RW);
MODULE_PARM_DESC(ring_pages, "Number of pages per req/cmp ring - (default="
		 __stringify(PVSCSI_DEFAULT_NUM_PAGES_PER_RING) ")");

module_param_named(msg_ring_pages, pvscsi_msg_ring_pages, int, PVSCSI_RW);
MODULE_PARM_DESC(msg_ring_pages, "Number of pages for the msg ring - (default="
		 __stringify(PVSCSI_DEFAULT_NUM_PAGES_MSG_RING) ")");

module_param_named(cmd_per_lun, pvscsi_cmd_per_lun, int, PVSCSI_RW);
MODULE_PARM_DESC(cmd_per_lun, "Maximum commands per lun - (default="
		 __stringify(PVSCSI_MAX_REQ_QUEUE_DEPTH) ")");

module_param_named(disable_msi, pvscsi_disable_msi, bool, PVSCSI_RW);
MODULE_PARM_DESC(disable_msi, "Disable MSI use in driver - (default="
		 __stringify(PVSCSI_DISABLE_MSI) ")");

module_param_named(disable_msix, pvscsi_disable_msix, bool, PVSCSI_RW);
MODULE_PARM_DESC(disable_msix, "Disable MSI-X use in driver - (default="
		 __stringify(PVSCSI_DISABLE_MSIX) ")");

module_param_named(use_msg, pvscsi_use_msg, bool, PVSCSI_RW);
MODULE_PARM_DESC(use_msg, "Use msg ring when available - (default=1)");

static const struct pci_device_id pvscsi_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_VMWARE, PCI_DEVICE_ID_VMWARE_PVSCSI) },
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, pvscsi_pci_tbl);

static struct pvscsi_ctx *
pvscsi_find_context(const struct pvscsi_adapter *adapter, struct scsi_cmnd *cmd)
{
	struct pvscsi_ctx *ctx, *end;

	end = &adapter->cmd_map[adapter->req_depth];
	for (ctx = adapter->cmd_map; ctx < end; ctx++)
		if (ctx->cmd == cmd)
			return ctx;

	return NULL;
}

static struct pvscsi_ctx *
pvscsi_acquire_context(struct pvscsi_adapter *adapter, struct scsi_cmnd *cmd)
{
	struct pvscsi_ctx *ctx;

	if (list_empty(&adapter->cmd_pool))
		return NULL;

	ctx = list_entry(adapter->cmd_pool.next, struct pvscsi_ctx, list);
	ctx->cmd = cmd;
	list_del(&ctx->list);

	return ctx;
}

static void pvscsi_release_context(struct pvscsi_adapter *adapter,
				   struct pvscsi_ctx *ctx)
{
	ctx->cmd = NULL;
	list_add(&ctx->list, &adapter->cmd_pool);
}

/*
 * Map a pvscsi_ctx struct to a context ID field value; we map to a simple
 * non-zero integer.
 */
static u64 pvscsi_map_context(const struct pvscsi_adapter *adapter,
			      const struct pvscsi_ctx *ctx)
{
	return ctx - adapter->cmd_map + 1;
}

static struct pvscsi_ctx *
pvscsi_get_context(const struct pvscsi_adapter *adapter, u64 context)
{
	return &adapter->cmd_map[context - 1];
}

/**************************************************************
 *
 *   VMWARE PVSCSI Hypervisor Communication Implementation
 *
 *   This code is largely independent of any Linux internals.
 *
 **************************************************************/

static void pvscsi_reg_write(const struct pvscsi_adapter *adapter,
			     u32 offset, u32 val)
{
	writel(val, adapter->mmioBase + offset);
}

static u32 pvscsi_reg_read(const struct pvscsi_adapter *adapter, u32 offset)
{
	return readl(adapter->mmioBase + offset);
}

static u32 pvscsi_read_intr_status(const struct pvscsi_adapter *adapter)
{
	return pvscsi_reg_read(adapter, PVSCSI_REG_OFFSET_INTR_STATUS);
}

static void pvscsi_write_intr_status(const struct pvscsi_adapter *adapter,
				     u32 val)
{
	pvscsi_reg_write(adapter, PVSCSI_REG_OFFSET_INTR_STATUS, val);
}

static void pvscsi_unmask_intr(const struct pvscsi_adapter *adapter)
{
	u32 intr_bits;

	intr_bits = PVSCSI_INTR_CMPL_MASK;
	if (adapter->use_msg) {
		intr_bits |= PVSCSI_INTR_MSG_MASK;
	}
	pvscsi_reg_write(adapter, PVSCSI_REG_OFFSET_INTR_MASK, intr_bits);
}

static void pvscsi_mask_intr(const struct pvscsi_adapter *adapter)
{
	pvscsi_reg_write(adapter, PVSCSI_REG_OFFSET_INTR_MASK, 0);
}

static void pvscsi_write_cmd_desc(const struct pvscsi_adapter *adapter,
				  u32 cmd, const void *desc, size_t len)
{
	u32 *ptr = (u32 *)desc;
	unsigned i;

	len /= sizeof(u32);
	pvscsi_reg_write(adapter, PVSCSI_REG_OFFSET_COMMAND, cmd);
	for (i = 0; i < len; i++)
		pvscsi_reg_write(adapter,
				 PVSCSI_REG_OFFSET_COMMAND_DATA, ptr[i]);
}

static void pvscsi_abort_cmd(const struct pvscsi_adapter *adapter,
			     const struct pvscsi_ctx *ctx)
{
	PVSCSICmdDescAbortCmd cmd = { 0 };

	cmd.target = ctx->cmd->device->id;
	cmd.context = pvscsi_map_context(adapter, ctx);

	pvscsi_write_cmd_desc(adapter, PVSCSI_CMD_ABORT_CMD, &cmd, sizeof cmd);
}

static void pvscsi_kick_rw_io(const struct pvscsi_adapter *adapter)
{
	pvscsi_reg_write(adapter, PVSCSI_REG_OFFSET_KICK_RW_IO, 0);
}

static void pvscsi_process_request_ring(const struct pvscsi_adapter *adapter)
{
	pvscsi_reg_write(adapter, PVSCSI_REG_OFFSET_KICK_NON_RW_IO, 0);
}

static int scsi_is_rw(unsigned char op)
{
	return op == READ_6  || op == WRITE_6 ||
	       op == READ_10 || op == WRITE_10 ||
	       op == READ_12 || op == WRITE_12 ||
	       op == READ_16 || op == WRITE_16;
}

static void pvscsi_kick_io(const struct pvscsi_adapter *adapter,
			   unsigned char op)
{
	if (scsi_is_rw(op))
		pvscsi_kick_rw_io(adapter);
	else
		pvscsi_process_request_ring(adapter);
}

static void ll_adapter_reset(const struct pvscsi_adapter *adapter)
{
	LOG(0, "Adapter Reset on %p\n", adapter);

	pvscsi_write_cmd_desc(adapter, PVSCSI_CMD_ADAPTER_RESET, NULL, 0);
}

static void ll_bus_reset(const struct pvscsi_adapter *adapter)
{
	LOG(0, "Reseting bus on %p\n", adapter);

	pvscsi_write_cmd_desc(adapter, PVSCSI_CMD_RESET_BUS, NULL, 0);
}

static void ll_device_reset(const struct pvscsi_adapter *adapter, u32 target)
{
	PVSCSICmdDescResetDevice cmd = { 0 };

	LOG(0, "Reseting device: target=%u\n", target);

	cmd.target = target;

	pvscsi_write_cmd_desc(adapter, PVSCSI_CMD_RESET_DEVICE,
			      &cmd, sizeof cmd);
}


/**************************************************************
 *
 *   VMWARE Hypervisor ring / SCSI mid-layer interactions
 *
 *   Functions which have to deal with both ring semantics
 *   and Linux SCSI internals are placed here.
 *
 **************************************************************/

static void pvscsi_create_sg(struct pvscsi_ctx *ctx,
			     struct scatterlist *sg, unsigned count)
{
	unsigned i;
	struct PVSCSISGElement *sge;

	BUG_ON(count > PVSCSI_MAX_NUM_SG_ENTRIES_PER_SEGMENT);

	sge = &ctx->sgl->sge[0];
	for (i = 0; i < count; i++, sg++) {
		sge[i].addr   = sg_dma_address(sg);
		sge[i].length = sg_dma_len(sg);
		sge[i].flags  = 0;
	}
}

/*
 * Map all data buffers for a command into PCI space and
 * setup the scatter/gather list if needed.
 */
static void pvscsi_map_buffers(struct pvscsi_adapter *adapter,
			       struct pvscsi_ctx *ctx,
			       struct scsi_cmnd *cmd, PVSCSIRingReqDesc *e)
{
	unsigned count;
	unsigned bufflen = scsi_bufflen(cmd);

	e->dataLen = bufflen;
	e->dataAddr = 0;
	if (bufflen == 0)
		return;

	count = scsi_sg_count(cmd);
	if (count != 0) {
		struct scatterlist *sg = scsi_sglist(cmd);
		int segs = pci_map_sg(adapter->dev, sg, count,
				      cmd->sc_data_direction);
		if (segs > 1) {
			pvscsi_create_sg(ctx, sg, segs);

			e->flags |= PVSCSI_FLAG_CMD_WITH_SG_LIST;
			e->dataAddr = ctx->sglPA;
		} else
			e->dataAddr = sg_dma_address(sg);
	} else {
		ctx->dataPA = pci_map_single(adapter->dev,
					     scsi_request_buffer(cmd), bufflen,
					     cmd->sc_data_direction);
		e->dataAddr = ctx->dataPA;
	}
}

static void pvscsi_unmap_buffers(const struct pvscsi_adapter *adapter,
				 const struct pvscsi_ctx *ctx)
{
	struct scsi_cmnd *cmd;
	unsigned bufflen;

	cmd = ctx->cmd;
	bufflen = scsi_bufflen(cmd);

	if (bufflen != 0) {
		unsigned count = scsi_sg_count(cmd);

		if (count != 0)
			pci_unmap_sg(adapter->dev, scsi_sglist(cmd), count,
				     cmd->sc_data_direction);
		else
			pci_unmap_single(adapter->dev, ctx->dataPA, bufflen,
					 cmd->sc_data_direction);
	}
	if (cmd->sense_buffer)
		pci_unmap_single(adapter->dev, ctx->sensePA,
				 SCSI_SENSE_BUFFERSIZE, PCI_DMA_FROMDEVICE);
}

static int __devinit pvscsi_allocate_rings(struct pvscsi_adapter *adapter)
{
	adapter->rings_state = pci_alloc_consistent(adapter->dev, PAGE_SIZE,
						    &adapter->ringStatePA);
	if (!adapter->rings_state)
		return -ENOMEM;

	adapter->req_pages = min(PVSCSI_MAX_NUM_PAGES_REQ_RING,
				 pvscsi_ring_pages);
	adapter->req_depth = adapter->req_pages
					* PVSCSI_MAX_NUM_REQ_ENTRIES_PER_PAGE;
	adapter->req_ring = pci_alloc_consistent(adapter->dev,
						 adapter->req_pages * PAGE_SIZE,
						 &adapter->reqRingPA);
	if (!adapter->req_ring)
		return -ENOMEM;

	adapter->cmp_pages = min(PVSCSI_MAX_NUM_PAGES_CMP_RING,
				 pvscsi_ring_pages);
	adapter->cmp_ring = pci_alloc_consistent(adapter->dev,
						 adapter->cmp_pages * PAGE_SIZE,
						 &adapter->cmpRingPA);
	if (!adapter->cmp_ring)
		return -ENOMEM;

	BUG_ON(adapter->ringStatePA & ~PAGE_MASK);
	BUG_ON(adapter->reqRingPA   & ~PAGE_MASK);
	BUG_ON(adapter->cmpRingPA   & ~PAGE_MASK);

	if (!adapter->use_msg)
		return 0;

	adapter->msg_pages = min(PVSCSI_MAX_NUM_PAGES_MSG_RING,
				 pvscsi_msg_ring_pages);
	adapter->msg_ring = pci_alloc_consistent(adapter->dev,
						 adapter->msg_pages * PAGE_SIZE,
						 &adapter->msgRingPA);
	if (!adapter->msg_ring)
		return -ENOMEM;
	BUG_ON(adapter->msgRingPA & ~PAGE_MASK);

	return 0;
}

static void pvscsi_setup_all_rings(const struct pvscsi_adapter *adapter)
{
	PVSCSICmdDescSetupRings cmd = { 0 };
	dma_addr_t base;
	unsigned i;

	cmd.ringsStatePPN   = adapter->ringStatePA >> PAGE_SHIFT;
	cmd.reqRingNumPages = adapter->req_pages;
	cmd.cmpRingNumPages = adapter->cmp_pages;

	base = adapter->reqRingPA;
	for (i = 0; i < adapter->req_pages; i++) {
		cmd.reqRingPPNs[i] = base >> PAGE_SHIFT;
		base += PAGE_SIZE;
	}

	base = adapter->cmpRingPA;
	for (i = 0; i < adapter->cmp_pages; i++) {
		cmd.cmpRingPPNs[i] = base >> PAGE_SHIFT;
		base += PAGE_SIZE;
	}

	memset(adapter->rings_state, 0, PAGE_SIZE);
	memset(adapter->req_ring, 0, adapter->req_pages * PAGE_SIZE);
	memset(adapter->cmp_ring, 0, adapter->cmp_pages * PAGE_SIZE);

	pvscsi_write_cmd_desc(adapter, PVSCSI_CMD_SETUP_RINGS,
			      &cmd, sizeof cmd);

	if (adapter->use_msg) {
		PVSCSICmdDescSetupMsgRing cmd_msg = { 0 };

		cmd_msg.numPages = adapter->msg_pages;

		base = adapter->msgRingPA;
		for (i = 0; i < adapter->msg_pages; i++) {
			cmd_msg.ringPPNs[i] = base >> PAGE_SHIFT;
			base += PAGE_SIZE;
		}
		memset(adapter->msg_ring, 0, adapter->msg_pages * PAGE_SIZE);

		pvscsi_write_cmd_desc(adapter, PVSCSI_CMD_SETUP_MSG_RING,
				      &cmd_msg, sizeof cmd_msg);
	}
}

/*
 * Pull a completion descriptor off and pass the completion back
 * to the SCSI mid layer.
 */
static void pvscsi_complete_request(struct pvscsi_adapter *adapter,
				    const PVSCSIRingCmpDesc *e)
{
	struct pvscsi_ctx *ctx;
	struct scsi_cmnd *cmd;
	u32 btstat = e->hostStatus;
	u32 sdstat = e->scsiStatus;

	ctx = pvscsi_get_context(adapter, e->context);
	cmd = ctx->cmd;
	pvscsi_unmap_buffers(adapter, ctx);
	pvscsi_release_context(adapter, ctx);
	cmd->result = 0;

	if (sdstat != SAM_STAT_GOOD &&
	    (btstat == BTSTAT_SUCCESS ||
	     btstat == BTSTAT_LINKED_COMMAND_COMPLETED ||
	     btstat == BTSTAT_LINKED_COMMAND_COMPLETED_WITH_FLAG)) {
		cmd->result = (DID_OK << 16) | sdstat;
		if (sdstat == SAM_STAT_CHECK_CONDITION && cmd->sense_buffer)
			cmd->result |= (DRIVER_SENSE << 24);
	} else
		switch (btstat) {
		case BTSTAT_SUCCESS:
		case BTSTAT_LINKED_COMMAND_COMPLETED:
		case BTSTAT_LINKED_COMMAND_COMPLETED_WITH_FLAG:
			/* If everything went fine, let's move on..  */
			cmd->result = (DID_OK << 16);
			break;

		case BTSTAT_DATARUN:
		case BTSTAT_DATA_UNDERRUN:
			/* Report residual data in underruns */
			scsi_set_resid(cmd, scsi_bufflen(cmd) - e->dataLen);
			cmd->result = (DID_ERROR << 16);
			break;

		case BTSTAT_SELTIMEO:
			/* Our emulation returns this for non-connected devs */
			cmd->result = (DID_BAD_TARGET << 16);
			break;

		case BTSTAT_LUNMISMATCH:
		case BTSTAT_TAGREJECT:
		case BTSTAT_BADMSG:
			cmd->result = (DRIVER_INVALID << 24);
			/* fall through */

		case BTSTAT_HAHARDWARE:
		case BTSTAT_INVPHASE:
		case BTSTAT_HATIMEOUT:
		case BTSTAT_NORESPONSE:
		case BTSTAT_DISCONNECT:
		case BTSTAT_HASOFTWARE:
		case BTSTAT_BUSFREE:
		case BTSTAT_SENSFAILED:
			cmd->result |= (DID_ERROR << 16);
			break;

		case BTSTAT_SENTRST:
		case BTSTAT_RECVRST:
		case BTSTAT_BUSRESET:
			cmd->result = (DID_RESET << 16);
			break;

		case BTSTAT_ABORTQUEUE:
			cmd->result = (DID_ABORT << 16);
			break;

		case BTSTAT_SCSIPARITY:
			cmd->result = (DID_PARITY << 16);
			break;

		default:
			cmd->result = (DID_ERROR << 16);
			LOG(0, "Unknown completion status: 0x%x\n", btstat);
	}

	LOG(3, "cmd=%p %x ctx=%p result=0x%x status=0x%x,%x\n",
		cmd, cmd->cmnd[0], ctx, cmd->result, btstat, sdstat);
	printk(KERN_ERR "pvscsi: cmd=%p %x ctx=%p result=0x%x status=0x%x,%x\n",
		cmd, cmd->cmnd[0], ctx, cmd->result, btstat, sdstat);

	cmd->scsi_done(cmd);
}

/*
 * barrier usage : Since the PVSCSI device is emulated, there could be cases
 * where we may want to serialize some accesses between the driver and the
 * emulation layer. We use compiler barriers instead of the more expensive
 * memory barriers because PVSCSI is only supported on X86 which has strong
 * memory access ordering.
 */
static void pvscsi_process_completion_ring(struct pvscsi_adapter *adapter)
{
	PVSCSIRingsState *s = adapter->rings_state;
	PVSCSIRingCmpDesc *ring = adapter->cmp_ring;
	u32 cmp_entries = s->cmpNumEntriesLog2;

	while (s->cmpConsIdx != s->cmpProdIdx) {
		PVSCSIRingCmpDesc *e = ring + (s->cmpConsIdx &
					       MASK(cmp_entries));
		/*
		 * This barrier() ensures that *e is not dereferenced while
		 * the device emulation still writes data into the slot.
		 * Since the device emulation advances s->cmpProdIdx only after
		 * updating the slot we want to check it first.
		 */
		barrier();
		pvscsi_complete_request(adapter, e);
		/*
		 * This barrier() ensures that compiler doesn't reorder write
		 * to s->cmpConsIdx before the read of (*e) inside
		 * pvscsi_complete_request. Otherwise, device emulation may
		 * overwrite *e before we had a chance to read it.
		 */
		barrier();
		s->cmpConsIdx++;
	}
}

/*
 * Translate a Linux SCSI request into a request ring entry.
 */
static int pvscsi_queue_ring(struct pvscsi_adapter *adapter,
			     struct pvscsi_ctx *ctx, struct scsi_cmnd *cmd)
{
	PVSCSIRingsState *s;
	PVSCSIRingReqDesc *e;
	struct scsi_device *sdev;
	u32 req_entries;

	s = adapter->rings_state;
	sdev = cmd->device;
	req_entries = s->reqNumEntriesLog2;

	/*
	 * If this condition holds, we might have room on the request ring, but
	 * we might not have room on the completion ring for the response.
	 * However, we have already ruled out this possibility - we would not
	 * have successfully allocated a context if it were true, since we only
	 * have one context per request entry.  Check for it anyway, since it
	 * would be a serious bug.
	 */
	if (s->reqProdIdx - s->cmpConsIdx >= 1 << req_entries) {
		printk(KERN_ERR "pvscsi: ring full: reqProdIdx=%d cmpConsIdx=%d\n",
			s->reqProdIdx, s->cmpConsIdx);
		return -1;
	}

	e = adapter->req_ring + (s->reqProdIdx & MASK(req_entries));

	e->bus    = sdev->channel;
	e->target = sdev->id;
	memset(e->lun, 0, sizeof e->lun);
	e->lun[1] = sdev->lun;

	if (cmd->sense_buffer) {
		ctx->sensePA = pci_map_single(adapter->dev, cmd->sense_buffer,
					      SCSI_SENSE_BUFFERSIZE,
					      PCI_DMA_FROMDEVICE);
		e->senseAddr = ctx->sensePA;
		e->senseLen = SCSI_SENSE_BUFFERSIZE;
	} else {
		e->senseLen  = 0;
		e->senseAddr = 0;
	}
	e->cdbLen   = cmd->cmd_len;
	e->vcpuHint = smp_processor_id();
	memcpy(e->cdb, cmd->cmnd, e->cdbLen);

	e->tag = SIMPLE_QUEUE_TAG;
	if (sdev->tagged_supported &&
	    (cmd->tag == HEAD_OF_QUEUE_TAG ||
	     cmd->tag == ORDERED_QUEUE_TAG))
		e->tag = cmd->tag;

	if (cmd->sc_data_direction == DMA_FROM_DEVICE)
		e->flags = PVSCSI_FLAG_CMD_DIR_TOHOST;
	else if (cmd->sc_data_direction == DMA_TO_DEVICE)
		e->flags = PVSCSI_FLAG_CMD_DIR_TODEVICE;
	else if (cmd->sc_data_direction == DMA_NONE)
		e->flags = PVSCSI_FLAG_CMD_DIR_NONE;
	else
		e->flags = 0;
        { static int i = 0;
           if (++i%2)
              //e->flags |= PVSCSI_FLAG_CMD_OUT_OF_BAND_CDB;
              e->flags |= (1 << 5);
        }

	pvscsi_map_buffers(adapter, ctx, cmd, e);

	e->context = pvscsi_map_context(adapter, ctx);

	barrier();

	s->reqProdIdx++;

	return 0;
}

static int pvscsi_queue(struct scsi_cmnd *cmd, void (*done)(struct scsi_cmnd *))
{
	struct Scsi_Host *host = cmd->device->host;
	struct pvscsi_adapter *adapter = HOST_ADAPTER(host);
	struct pvscsi_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&adapter->hw_lock, flags);

	ctx = pvscsi_acquire_context(adapter, cmd);
	if (!ctx || pvscsi_queue_ring(adapter, ctx, cmd) != 0) {
		if (ctx)
			pvscsi_release_context(adapter, ctx);
		spin_unlock_irqrestore(&adapter->hw_lock, flags);
		return SCSI_MLQUEUE_HOST_BUSY;
	}

	cmd->scsi_done = done;

	LOG(3, "queued cmd %p, ctx %p, op=%x\n", cmd, ctx, cmd->cmnd[0]);

	spin_unlock_irqrestore(&adapter->hw_lock, flags);

	pvscsi_kick_io(adapter, cmd->cmnd[0]);

	return 0;
}

static int pvscsi_abort(struct scsi_cmnd *cmd)
{
	struct pvscsi_adapter *adapter = HOST_ADAPTER(cmd->device->host);
	struct pvscsi_ctx *ctx;
	unsigned long flags;

	printk(KERN_DEBUG "pvscsi: task abort on host %u, %p\n",
	       adapter->host->host_no, cmd);

	spin_lock_irqsave(&adapter->hw_lock, flags);

	/*
	 * Poll the completion ring first - we might be trying to abort
	 * a command that is waiting to be dispatched in the completion ring.
	 */
	pvscsi_process_completion_ring(adapter);

	/*
	 * If there is no context for the command, it either already succeeded
	 * or else was never properly issued.  Not our problem.
	 */
	ctx = pvscsi_find_context(adapter, cmd);
	if (!ctx) {
		LOG(1, "Failed to abort cmd %p\n", cmd);
		goto out;
	}

	pvscsi_abort_cmd(adapter, ctx);

	pvscsi_process_completion_ring(adapter);

out:
	spin_unlock_irqrestore(&adapter->hw_lock, flags);
	return SUCCESS;
}

/*
 * Abort all outstanding requests.  This is only safe to use if the completion
 * ring will never be walked again or the device has been reset, because it
 * destroys the 1-1 mapping between context field passed to emulation and our
 * request structure.
 */
static void pvscsi_reset_all(struct pvscsi_adapter *adapter)
{
	unsigned i;

	for (i = 0; i < adapter->req_depth; i++) {
		struct pvscsi_ctx *ctx = &adapter->cmd_map[i];
		struct scsi_cmnd *cmd = ctx->cmd;
		if (cmd) {
			printk(KERN_ERR "pvscsi: forced reset on cmd %p\n", cmd);
			pvscsi_unmap_buffers(adapter, ctx);
			pvscsi_release_context(adapter, ctx);
			cmd->result = (DID_RESET << 16);
			cmd->scsi_done(cmd);
		}
	}
}

static int pvscsi_host_reset(struct scsi_cmnd *cmd)
{
	struct Scsi_Host *host = cmd->device->host;
	struct pvscsi_adapter *adapter = HOST_ADAPTER(host);
	unsigned long flags;
	char use_msg;

	printk(KERN_INFO "pvscsi: host reset on host %u\n", host->host_no);

	spin_lock_irqsave(&adapter->hw_lock, flags);

	use_msg = adapter->use_msg;

	if (use_msg) {
		adapter->use_msg = 0;
		spin_unlock_irqrestore(&adapter->hw_lock, flags);

		/*
		 * Now that we know that the ISR won't add more work on the
		 * workqueue we can safely flush any outstanding work.
		 */
		flush_workqueue(adapter->workqueue);
		spin_lock_irqsave(&adapter->hw_lock, flags);
	}

	/*
	 * We're going to tear down the entire ring structure and set it back
	 * up, so stalling new requests until all completions are flushed and
	 * the rings are back in place.
	 */

	pvscsi_process_request_ring(adapter);

	ll_adapter_reset(adapter);

	/*
	 * Now process any completions.  Note we do this AFTER adapter reset,
	 * which is strange, but stops races where completions get posted
	 * between processing the ring and issuing the reset.  The backend will
	 * not touch the ring memory after reset, so the immediately pre-reset
	 * completion ring state is still valid.
	 */
	pvscsi_process_completion_ring(adapter);

	pvscsi_reset_all(adapter);
	adapter->use_msg = use_msg;
	pvscsi_setup_all_rings(adapter);
	pvscsi_unmask_intr(adapter);

	spin_unlock_irqrestore(&adapter->hw_lock, flags);

	return SUCCESS;
}

static int pvscsi_bus_reset(struct scsi_cmnd *cmd)
{
	struct Scsi_Host *host = cmd->device->host;
	struct pvscsi_adapter *adapter = HOST_ADAPTER(host);
	unsigned long flags;

	printk(KERN_INFO "pvscsi: bus reset on host %u\n", host->host_no);

	/*
	 * We don't want to queue new requests for this bus after
	 * flushing all pending requests to emulation, since new
	 * requests could then sneak in during this bus reset phase,
	 * so take the lock now.
	 */
	spin_lock_irqsave(&adapter->hw_lock, flags);

	pvscsi_process_request_ring(adapter);
	ll_bus_reset(adapter);
	pvscsi_process_completion_ring(adapter);

	spin_unlock_irqrestore(&adapter->hw_lock, flags);

	return SUCCESS;
}

static int pvscsi_device_reset(struct scsi_cmnd *cmd)
{
	struct Scsi_Host *host = cmd->device->host;
	struct pvscsi_adapter *adapter = HOST_ADAPTER(host);
	unsigned long flags;

	printk(KERN_INFO "pvscsi: device reset on scsi%u:%u\n",
	       host->host_no, cmd->device->id);

	/*
	 * We don't want to queue new requests for this device after flushing
	 * all pending requests to emulation, since new requests could then
	 * sneak in during this device reset phase, so take the lock now.
	 */
	spin_lock_irqsave(&adapter->hw_lock, flags);

	pvscsi_process_request_ring(adapter);
	ll_device_reset(adapter, cmd->device->id);
	pvscsi_process_completion_ring(adapter);

	spin_unlock_irqrestore(&adapter->hw_lock, flags);

	return SUCCESS;
}

static struct scsi_host_template pvscsi_template;

static const char *pvscsi_info(struct Scsi_Host *host)
{
	struct pvscsi_adapter *adapter = HOST_ADAPTER(host);
	static char buf[512];

	sprintf(buf, "VMware PVSCSI storage adapter rev %d, req/cmp/msg rings: "
		"%u/%u/%u pages, cmd_per_lun=%u", adapter->rev,
		adapter->req_pages, adapter->cmp_pages, adapter->msg_pages,
		pvscsi_template.cmd_per_lun);

	return buf;
}

static struct scsi_host_template pvscsi_template = {
	.module				= THIS_MODULE,
	.name				= "VMware PVSCSI Host Adapter",
	.proc_name			= "pvscsi",
	.info				= pvscsi_info,
	.queuecommand			= pvscsi_queue,
	.this_id			= -1,
	.sg_tablesize			= PVSCSI_MAX_NUM_SG_ENTRIES_PER_SEGMENT,
	.dma_boundary			= UINT_MAX,
	.max_sectors			= 0xffff,
	.use_clustering			= ENABLE_CLUSTERING,
	.eh_abort_handler		= pvscsi_abort,
	.eh_device_reset_handler	= pvscsi_device_reset,
	.eh_bus_reset_handler		= pvscsi_bus_reset,
	.eh_host_reset_handler		= pvscsi_host_reset,
};

static void pvscsi_process_msg(const struct pvscsi_adapter *adapter,
			       const PVSCSIRingMsgDesc *e)
{
	PVSCSIRingsState *s = adapter->rings_state;
	struct Scsi_Host *host = adapter->host;
	struct scsi_device *sdev;

	printk(KERN_INFO "pvscsi: msg type: 0x%x - MSG RING: %u/%u (%u) \n",
	       e->type, s->msgProdIdx, s->msgConsIdx, s->msgNumEntriesLog2);

	ASSERT_ON_COMPILE(PVSCSI_MSG_LAST == 2);

	if (e->type == PVSCSI_MSG_DEV_ADDED) {
		PVSCSIMsgDescDevStatusChanged *desc;
		desc = (PVSCSIMsgDescDevStatusChanged *)e;

		printk(KERN_INFO "pvscsi: msg: device added at scsi%u:%u:%u\n",
		       desc->bus, desc->target, desc->lun[1]);

		if (!scsi_host_get(host))
			return;

		sdev = scsi_device_lookup(host, desc->bus, desc->target,
					  desc->lun[1]);
		if (sdev) {
			printk(KERN_INFO "pvscsi: device already exists\n");
			scsi_device_put(sdev);
		} else
			scsi_add_device(adapter->host, desc->bus,
					desc->target, desc->lun[1]);

		scsi_host_put(host);
	} else if (e->type == PVSCSI_MSG_DEV_REMOVED) {
		PVSCSIMsgDescDevStatusChanged *desc;
		desc = (PVSCSIMsgDescDevStatusChanged *)e;

		printk(KERN_INFO "pvscsi: msg: device removed at scsi%u:%u:%u\n",
		       desc->bus, desc->target, desc->lun[1]);

		if (!scsi_host_get(host))
			return;

		sdev = scsi_device_lookup(host, desc->bus, desc->target,
					  desc->lun[1]);
		if (sdev) {
			scsi_remove_device(sdev);
			scsi_device_put(sdev);
		} else
			printk(KERN_INFO "pvscsi: failed to lookup scsi%u:%u:%u\n",
			       desc->bus, desc->target, desc->lun[1]);

		scsi_host_put(host);
	}
}

static int pvscsi_msg_pending(const struct pvscsi_adapter *adapter)
{
	PVSCSIRingsState *s = adapter->rings_state;

	return s->msgProdIdx != s->msgConsIdx;
}

static void pvscsi_process_msg_ring(const struct pvscsi_adapter *adapter)
{
	PVSCSIRingsState *s = adapter->rings_state;
	PVSCSIRingMsgDesc *ring = adapter->msg_ring;
	u32 msg_entries = s->msgNumEntriesLog2;

	while (pvscsi_msg_pending(adapter)) {
		PVSCSIRingMsgDesc *e = ring + (s->msgConsIdx &
					       MASK(msg_entries));

		barrier();
		pvscsi_process_msg(adapter, e);
		barrier();
		s->msgConsIdx++;
	}
}

static void pvscsi_msg_workqueue_handler(compat_work_arg data)
{
	struct pvscsi_adapter *adapter;

	adapter = COMPAT_WORK_GET_DATA(data, struct pvscsi_adapter, work);

	pvscsi_process_msg_ring(adapter);
}

static int pvscsi_setup_msg_workqueue(struct pvscsi_adapter *adapter)
{
	char name[32];

	if (!pvscsi_use_msg)
		return 0;

	pvscsi_reg_write(adapter, PVSCSI_REG_OFFSET_COMMAND,
			 PVSCSI_CMD_SETUP_MSG_RING);

	if (pvscsi_reg_read(adapter, PVSCSI_REG_OFFSET_COMMAND_STATUS) == -1)
		return 0;

	snprintf(name, sizeof name, "pvscsi_wq_%u", adapter->host->host_no);

	adapter->workqueue = create_singlethread_workqueue(name);
	if (!adapter->workqueue) {
		printk(KERN_ERR "pvscsi: failed to create work queue\n");
		return 0;
	}
	COMPAT_INIT_WORK(&adapter->work, pvscsi_msg_workqueue_handler, adapter);
	return 1;
}

static irqreturn_t pvscsi_isr COMPAT_IRQ_HANDLER_ARGS(irq, devp)
{
	struct pvscsi_adapter *adapter = devp;
	int handled;

	if (adapter->use_msi || adapter->use_msix)
		handled = TRUE;
	else {
		u32 val = pvscsi_read_intr_status(adapter);
		handled = (val & PVSCSI_INTR_ALL_SUPPORTED) != 0;
		if (handled)
			pvscsi_write_intr_status(devp, val);
	}

	if (handled) {
		unsigned long flags;

		spin_lock_irqsave(&adapter->hw_lock, flags);

		pvscsi_process_completion_ring(adapter);
		if (adapter->use_msg && pvscsi_msg_pending(adapter))
			queue_work(adapter->workqueue, &adapter->work);

		spin_unlock_irqrestore(&adapter->hw_lock, flags);
	}

	return IRQ_RETVAL(handled);
}

static void pvscsi_free_sgls(const struct pvscsi_adapter *adapter)
{
	struct pvscsi_ctx *ctx = adapter->cmd_map;
	unsigned i;

	for (i = 0; i < adapter->req_depth; ++i, ++ctx)
		pci_free_consistent(adapter->dev, PAGE_SIZE, ctx->sgl,
				    ctx->sglPA);
}

static int pvscsi_setup_msix(const struct pvscsi_adapter *adapter, int *irq)
{
#ifdef CONFIG_PCI_MSI
	struct msix_entry entry = { 0, PVSCSI_VECTOR_COMPLETION };
	int ret;

	ret = pci_enable_msix(adapter->dev, &entry, 1);
	if (ret)
		return ret;

	*irq = entry.vector;

	return 0;
#else
	return -1;
#endif
}

static void pvscsi_shutdown_intr(struct pvscsi_adapter *adapter)
{
	if (adapter->irq) {
		free_irq(adapter->irq, adapter);
		adapter->irq = 0;
	}
#ifdef CONFIG_PCI_MSI
	if (adapter->use_msi) {
		pci_disable_msi(adapter->dev);
		adapter->use_msi = 0;
	}

	if (adapter->use_msix) {
		pci_disable_msix(adapter->dev);
		adapter->use_msix = 0;
	}
#endif
}

static void pvscsi_release_resources(struct pvscsi_adapter *adapter)
{
	pvscsi_shutdown_intr(adapter);

	if (adapter->workqueue)
		destroy_workqueue(adapter->workqueue);

	if (adapter->mmioBase)
		iounmap(adapter->mmioBase);

	pci_release_regions(adapter->dev);

	if (adapter->cmd_map) {
		pvscsi_free_sgls(adapter);
		kfree(adapter->cmd_map);
	}

	if (adapter->rings_state)
		pci_free_consistent(adapter->dev, PAGE_SIZE,
				    adapter->rings_state, adapter->ringStatePA);

	if (adapter->req_ring)
		pci_free_consistent(adapter->dev,
				    adapter->req_pages * PAGE_SIZE,
				    adapter->req_ring, adapter->reqRingPA);

	if (adapter->cmp_ring)
		pci_free_consistent(adapter->dev,
				    adapter->cmp_pages * PAGE_SIZE,
				    adapter->cmp_ring, adapter->cmpRingPA);

	if (adapter->msg_ring)
		pci_free_consistent(adapter->dev,
				    adapter->msg_pages * PAGE_SIZE,
				    adapter->msg_ring, adapter->msgRingPA);
}

/*
 * Allocate scatter gather lists.
 *
 * These are statically allocated.  Trying to be clever was not worth it.
 *
 * Dynamic allocation can fail, and we can't go deeep into the memory
 * allocator, since we're a SCSI driver, and trying too hard to allocate
 * memory might generate disk I/O.  We also don't want to fail disk I/O
 * in that case because we can't get an allocation - the I/O could be
 * trying to swap out data to free memory.  Since that is pathological,
 * just use a statically allocated scatter list.
 *
 */
static int __devinit pvscsi_allocate_sg(struct pvscsi_adapter *adapter)
{
	struct pvscsi_ctx *ctx;
	int i;

	ctx = adapter->cmd_map;
	ASSERT_ON_COMPILE(sizeof(struct pvscsi_sg_list) <= PAGE_SIZE);

	for (i = 0; i < adapter->req_depth; ++i, ++ctx) {
		ctx->sgl = pci_alloc_consistent(adapter->dev, PAGE_SIZE,
						&ctx->sglPA);
		BUG_ON(ctx->sglPA & ~PAGE_MASK);
		if (!ctx->sgl) {
			for (; i >= 0; --i, --ctx) {
				pci_free_consistent(adapter->dev, PAGE_SIZE,
						    ctx->sgl, ctx->sglPA);
				ctx->sgl = NULL;
				ctx->sglPA = 0;
			}
			return -ENOMEM;
		}
	}

	return 0;
}

static int __devinit pvscsi_probe(struct pci_dev *pdev,
				  const struct pci_device_id *id)
{
	struct pvscsi_adapter *adapter;
	struct Scsi_Host *host;
	unsigned long base, i;
	int error;

	error = -ENODEV;

	if (pci_enable_device(pdev))
		return error;

	if (pdev->vendor != PCI_VENDOR_ID_VMWARE)
		goto out_disable_device;

	if (pci_set_dma_mask(pdev, DMA_BIT_MASK(64)) == 0 &&
	    pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64)) == 0) {
		printk(KERN_INFO "pvscsi: using 64bit dma\n");
	} else if (pci_set_dma_mask(pdev, DMA_BIT_MASK(32)) == 0 &&
		   pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32)) == 0) {
		printk(KERN_INFO "pvscsi: using 32bit dma\n");
	} else {
		printk(KERN_ERR "pvscsi: failed to set DMA mask\n");
		goto out_disable_device;
	}

	pvscsi_template.can_queue =
		min(PVSCSI_MAX_NUM_PAGES_REQ_RING, pvscsi_ring_pages) *
		PVSCSI_MAX_NUM_REQ_ENTRIES_PER_PAGE;
	pvscsi_template.cmd_per_lun =
		min(pvscsi_template.can_queue, pvscsi_cmd_per_lun);
	host = scsi_host_alloc(&pvscsi_template, sizeof(struct pvscsi_adapter));
	if (!host) {
		printk(KERN_ERR "pvscsi: failed to allocate host\n");
		goto out_disable_device;
	}

	adapter = HOST_ADAPTER(host);
	memset(adapter, 0, sizeof *adapter);
	adapter->dev  = pdev;
	adapter->host = host;

	spin_lock_init(&adapter->hw_lock);

	host->max_channel = 0;
	host->max_id      = 16;
	host->max_lun     = 1;
	host->max_cmd_len = 16;

	pci_read_config_byte(pdev, PCI_CLASS_REVISION, &adapter->rev);

	if (pci_request_regions(pdev, "pvscsi")) {
		printk(KERN_ERR "pvscsi: pci memory selection failed\n");
		goto out_free_host;
	}

	base = 0;
	for (i = 0; i < DEVICE_COUNT_RESOURCE; i++) {
		if ((pci_resource_flags(pdev, i) & PCI_BASE_ADDRESS_SPACE_IO))
			continue;

		if (pci_resource_len(pdev, i) <
					PVSCSI_MEM_SPACE_NUM_PAGES * PAGE_SIZE)
			continue;

		base = pci_resource_start(pdev, i);
		break;
	}

	if (base == 0) {
		printk(KERN_ERR "pvscsi: adapter has no suitable MMIO region\n");
		goto out_release_resources;
	}

	adapter->mmioBase = ioremap(base, PVSCSI_MEM_SPACE_SIZE);
	if (!adapter->mmioBase) {
		printk(KERN_ERR "pvscsi: can't ioremap 0x%lx\n", base);
		goto out_release_resources;
	}

	pci_set_master(pdev);
	pci_set_drvdata(pdev, host);

	ll_adapter_reset(adapter);

	adapter->use_msg = pvscsi_setup_msg_workqueue(adapter);

	error = pvscsi_allocate_rings(adapter);
	if (error) {
		printk(KERN_ERR "pvscsi: unable to allocate ring memory\n");
		goto out_release_resources;
	}

	/*
	 * From this point on we should reset the adapter if anything goes
	 * wrong.
	 */
	pvscsi_setup_all_rings(adapter);

	adapter->cmd_map = kmalloc(adapter->req_depth *
				   sizeof(struct pvscsi_ctx), GFP_KERNEL);
	if (!adapter->cmd_map) {
		printk(KERN_ERR "pvscsi: failed to allocate memory.\n");
		error = -ENOMEM;
		goto out_reset_adapter;
	}
	memset(adapter->cmd_map, 0,
	       adapter->req_depth * sizeof(struct pvscsi_ctx));

	INIT_LIST_HEAD(&adapter->cmd_pool);
	for (i = 0; i < adapter->req_depth; i++) {
		struct pvscsi_ctx *ctx = adapter->cmd_map + i;
		list_add(&ctx->list, &adapter->cmd_pool);
	}

	error = pvscsi_allocate_sg(adapter);
	if (error) {
		printk(KERN_ERR "pvscsi: unable to allocate s/g table\n");
		goto out_reset_adapter;
	}

	if (!pvscsi_disable_msix &&
	    pvscsi_setup_msix(adapter, &adapter->irq) == 0) {
		printk(KERN_INFO "pvscsi: using MSI-X\n");
		adapter->use_msix = 1;
	} else if (!pvscsi_disable_msi && pci_enable_msi(pdev) == 0) {
		printk(KERN_INFO "pvscsi: using MSI\n");
		adapter->use_msi = 1;
		adapter->irq = pdev->irq;
	} else {
		printk(KERN_INFO "pvscsi: using INTx\n");
		adapter->irq = pdev->irq;
	}

	error = request_irq(adapter->irq, pvscsi_isr, COMPAT_IRQF_SHARED,
			    "pvscsi", adapter);
	if (error) {
		printk(KERN_ERR "pvscsi: unable to request IRQ: %d\n", error);
		adapter->irq = 0;
		goto out_reset_adapter;
	}

	error = scsi_add_host(host, &pdev->dev);
	if (error) {
		printk(KERN_ERR "pvscsi: scsi_add_host failed: %d\n", error);
		goto out_reset_adapter;
	}

	printk(KERN_INFO "VMware PVSCSI rev %d on bus:%u slot:%u func:%u host #%u\n",
	       adapter->rev, pdev->bus->number, PCI_SLOT(pdev->devfn),
	       PCI_FUNC(pdev->devfn), host->host_no);

	pvscsi_unmask_intr(adapter);

	scsi_scan_host(host);

	return 0;

out_reset_adapter:
	ll_adapter_reset(adapter);
out_release_resources:
	pvscsi_release_resources(adapter);
out_free_host:
	scsi_host_put(host);
out_disable_device:
	pci_set_drvdata(pdev, NULL);
	pci_disable_device(pdev);

	return error;
}

static void __pvscsi_shutdown(struct pvscsi_adapter *adapter)
{
	pvscsi_mask_intr(adapter);

	if (adapter->workqueue)
		flush_workqueue(adapter->workqueue);

	pvscsi_shutdown_intr(adapter);

	pvscsi_process_request_ring(adapter);
	pvscsi_process_completion_ring(adapter);
	ll_adapter_reset(adapter);
}

static void COMPAT_PCI_DECLARE_SHUTDOWN(pvscsi_shutdown, dev)
{
	struct Scsi_Host *host = pci_get_drvdata(COMPAT_PCI_TO_DEV(dev));
	struct pvscsi_adapter *adapter = HOST_ADAPTER(host);

	__pvscsi_shutdown(adapter);
}

static void pvscsi_remove(struct pci_dev *pdev)
{
	struct Scsi_Host *host = pci_get_drvdata(pdev);
	struct pvscsi_adapter *adapter = HOST_ADAPTER(host);

	scsi_remove_host(host);

	__pvscsi_shutdown(adapter);
	pvscsi_release_resources(adapter);

	scsi_host_put(host);

	pci_set_drvdata(pdev, NULL);
	pci_disable_device(pdev);
}

static struct pci_driver pvscsi_pci_driver = {
	.name		= "pvscsi",
	.id_table	= pvscsi_pci_tbl,
	.probe		= pvscsi_probe,
	.remove		= __devexit_p(pvscsi_remove),
	COMPAT_PCI_SHUTDOWN(pvscsi_shutdown)
};

static int __init pvscsi_init(void)
{
	printk(KERN_DEBUG "%s - version %s\n",
	       PVSCSI_LINUX_DRIVER_DESC, PVSCSI_DRIVER_VERSION_STRING);
	return pci_register_driver(&pvscsi_pci_driver);
}

static void __exit pvscsi_exit(void)
{
	pci_unregister_driver(&pvscsi_pci_driver);
}

module_init(pvscsi_init);
module_exit(pvscsi_exit);
