/* Copyright (c) 2010-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/firmware.h>
#include <linux/pm_qos_params.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <mach/clk.h>
#include <mach/msm_memtypes.h>
#include <linux/interrupt.h>
#include <linux/memory_alloc.h>
#include <asm/sizes.h>
#include "vidc.h"
#include "vcd_res_tracker.h"
#include "vidc_init.h"

static unsigned int vidc_clk_table[3] = {
	48000000, 133330000, 200000000
};
static unsigned int restrk_mmu_subsystem[] =	{
		MSM_SUBSYSTEM_VIDEO, MSM_SUBSYSTEM_VIDEO_FWARE};
static struct res_trk_context resource_context;

#define VIDC_FW	"vidc_1080p.fw"
#define VIDC_FW_SIZE SZ_1M

struct res_trk_vidc_mmu_clk {
	char *mmu_clk_name;
	struct clk *mmu_clk;
};

static struct res_trk_vidc_mmu_clk vidc_mmu_clks[] = {
	{"mdp_iommu_clk"}, {"rot_iommu_clk"},
	{"vcodec_iommu0_clk"}, {"vcodec_iommu1_clk"},
	{"smmu_iface_clk"}
};

unsigned char *vidc_video_codec_fw;
u32 vidc_video_codec_fw_size;
static u32 res_trk_get_clk(void);
static void res_trk_put_clk(void);

static void *res_trk_pmem_map
	(struct ddl_buf_addr *addr, size_t sz, u32 alignment)
{
	u32 offset = 0, flags = 0;
	u32 index = 0;
	struct ddl_context *ddl_context;
	struct msm_mapped_buffer *mapped_buffer = NULL;
	ddl_context = ddl_get_context();
	if (!addr->alloced_phys_addr) {
		pr_err(" %s() alloced addres NULL", __func__);
		goto bail_out;
	}
	flags = MSM_SUBSYSTEM_MAP_IOVA | MSM_SUBSYSTEM_MAP_KADDR;
	if (alignment == DDL_KILO_BYTE(128))
			index = 1;
	else if (alignment > SZ_4K)
		flags |= MSM_SUBSYSTEM_ALIGN_IOVA_8K;

	addr->mapped_buffer =
	msm_subsystem_map_buffer((unsigned long)addr->alloced_phys_addr,
	sz, flags, &restrk_mmu_subsystem[index],
	sizeof(restrk_mmu_subsystem[index])/sizeof(unsigned int));
	if (IS_ERR(addr->mapped_buffer)) {
		pr_err(" %s() buffer map failed", __func__);
		goto bail_out;
	}
	mapped_buffer = addr->mapped_buffer;
	if (!mapped_buffer->vaddr || !mapped_buffer->iova[0]) {
		pr_err("%s() map buffers failed\n", __func__);
		goto bail_out;
	}
	addr->physical_base_addr = (u8 *)mapped_buffer->iova[0];
	addr->virtual_base_addr = mapped_buffer->vaddr;
	addr->align_physical_addr = (u8 *) DDL_ALIGN((u32)
		addr->physical_base_addr, alignment);
	offset = (u32)(addr->align_physical_addr -
			addr->physical_base_addr);
	addr->align_virtual_addr = addr->virtual_base_addr + offset;
	addr->buffer_size = sz;
	return addr->virtual_base_addr;
bail_out:
	return NULL;
}

static void *res_trk_pmem_alloc
	(struct ddl_buf_addr *addr, size_t sz, u32 alignment)
{
	u32 alloc_size;
	struct ddl_context *ddl_context;
	int rc = -EINVAL;
	ion_phys_addr_t phyaddr = 0;
	size_t len = 0;
	DBG_PMEM("\n%s() IN: Requested alloc size(%u)", __func__, (u32)sz);
	if (!addr) {
		DDL_MSG_ERROR("\n%s() Invalid Parameters", __func__);
		goto bail_out;
	}
	ddl_context = ddl_get_context();
	res_trk_set_mem_type(addr->mem_type);
	alloc_size = (sz + alignment);
	if (res_trk_get_enable_ion()) {
		if (!ddl_context->video_ion_client)
			ddl_context->video_ion_client =
				res_trk_get_ion_client();
		if (!ddl_context->video_ion_client) {
			DDL_MSG_ERROR("%s() :DDL ION Client Invalid handle\n",
						 __func__);
			goto bail_out;
		}
		addr->alloc_handle = ion_alloc(
		ddl_context->video_ion_client, alloc_size, SZ_4K,
			(1<<res_trk_get_mem_type()));
		if (IS_ERR_OR_NULL(addr->alloc_handle)) {
			DDL_MSG_ERROR("%s() :DDL ION alloc failed\n",
						 __func__);
			goto bail_out;
		}
		rc = ion_phys(ddl_context->video_ion_client,
				addr->alloc_handle, &phyaddr,
				 &len);
		if (rc || !phyaddr) {
			DDL_MSG_ERROR("%s():DDL ION client physical failed\n",
						 __func__);
			goto free_acm_ion_alloc;
		}
		addr->alloced_phys_addr = phyaddr;
	} else {
		addr->alloced_phys_addr = (phys_addr_t)
		allocate_contiguous_memory_nomap(alloc_size,
			res_trk_get_mem_type(), SZ_4K);
		if (!addr->alloced_phys_addr) {
			DDL_MSG_ERROR("%s() : acm alloc failed (%d)\n",
					 __func__, alloc_size);
			goto bail_out;
		}
	}

	addr->buffer_size = sz;
	return (void *)addr->alloced_phys_addr;

free_acm_ion_alloc:
	if (ddl_context->video_ion_client) {
		if (addr->alloc_handle) {
			ion_free(ddl_context->video_ion_client,
				addr->alloc_handle);
			addr->alloc_handle = NULL;
		}
	}
bail_out:
	return NULL;
}

static void res_trk_pmem_unmap(struct ddl_buf_addr *addr)
{
	if (!addr) {
		pr_err("%s() invalid args\n", __func__);
		return;
	}
	if (addr->mapped_buffer)
		msm_subsystem_unmap_buffer(addr->mapped_buffer);
	addr->mapped_buffer = NULL;
}


static u32 res_trk_get_clk()
{
	if (resource_context.vcodec_clk ||
		resource_context.vcodec_pclk) {
		VCDRES_MSG_ERROR("%s() Clock reference exists\n",
						__func__);
		goto bail_out;
	}
	resource_context.vcodec_clk = clk_get(resource_context.device,
		"core_clk");
	if (IS_ERR(resource_context.vcodec_clk)) {
		VCDRES_MSG_ERROR("%s(): core_clk get failed\n",
						__func__);
		goto bail_out;
	}
	 resource_context.vcodec_pclk = clk_get(resource_context.device,
		"iface_clk");
	if (IS_ERR(resource_context.vcodec_pclk)) {
		VCDRES_MSG_ERROR("%s(): iface_clk get failed\n",
						__func__);
		goto release_vcodec_clk;
	}
	if (clk_set_rate(resource_context.vcodec_clk,
		vidc_clk_table[0])) {
		VCDRES_MSG_ERROR("%s(): set rate failed in power up\n",
						__func__);
		goto release_vcodec_pclk;
	}
	return true;
release_vcodec_pclk:
	clk_put(resource_context.vcodec_pclk);
	resource_context.vcodec_pclk = NULL;
release_vcodec_clk:
	clk_put(resource_context.vcodec_clk);
	resource_context.vcodec_clk = NULL;
bail_out:
	return false;
}

static void res_trk_put_clk()
{
	if (resource_context.vcodec_clk)
		clk_put(resource_context.vcodec_clk);
	if (resource_context.vcodec_pclk)
		clk_put(resource_context.vcodec_pclk);
	resource_context.vcodec_clk = NULL;
	resource_context.vcodec_pclk = NULL;
}

static u32 res_trk_shutdown_vidc(void)
{
	mutex_lock(&resource_context.lock);
	if (resource_context.clock_enabled) {
		mutex_unlock(&resource_context.lock);
		VCDRES_MSG_LOW("\n Calling CLK disable in Power Down\n");
		res_trk_disable_clocks();
		mutex_lock(&resource_context.lock);
	}
	res_trk_put_clk();
	if (resource_context.footswitch) {
		if (regulator_disable(resource_context.footswitch))
			VCDRES_MSG_ERROR("Regulator disable failed\n");
		regulator_put(resource_context.footswitch);
		resource_context.footswitch = NULL;
	}
	if (pm_runtime_put(resource_context.device) < 0)
		VCDRES_MSG_ERROR("Error : pm_runtime_put failed");
	mutex_unlock(&resource_context.lock);
	return true;
}

u32 res_trk_enable_clocks(void)
{
	VCDRES_MSG_LOW("\n in res_trk_enable_clocks()");
	mutex_lock(&resource_context.lock);
	if (!resource_context.clock_enabled) {
		VCDRES_MSG_LOW("Enabling IRQ in %s()\n", __func__);
		enable_irq(resource_context.irq_num);
		VCDRES_MSG_LOW("%s(): Enabling the clocks\n", __func__);
		if (resource_context.vcodec_clk &&
			resource_context.vcodec_pclk) {
			if (clk_enable(resource_context.vcodec_pclk)) {
				VCDRES_MSG_ERROR("vidc pclk Enable fail\n");
				goto bail_out;
			}
			if (clk_enable(resource_context.vcodec_clk)) {
				VCDRES_MSG_ERROR("vidc core clk Enable fail\n");
				goto vidc_disable_pclk;
			}

			VCDRES_MSG_LOW("%s(): Clocks enabled!\n", __func__);
		} else {
		   VCDRES_MSG_ERROR("%s(): Clocks enable failed!\n",
			__func__);
		   goto bail_out;
		}
	}
	resource_context.clock_enabled = 1;
	mutex_unlock(&resource_context.lock);
	return true;
vidc_disable_pclk:
	clk_disable(resource_context.vcodec_pclk);
bail_out:
	mutex_unlock(&resource_context.lock);
	return false;
}

static u32 res_trk_sel_clk_rate(unsigned long hclk_rate)
{
	u32 status = true;
	mutex_lock(&resource_context.lock);
	if (clk_set_rate(resource_context.vcodec_clk,
		hclk_rate)) {
		VCDRES_MSG_ERROR("vidc hclk set rate failed\n");
		status = false;
	} else
		resource_context.vcodec_clk_rate = hclk_rate;
	mutex_unlock(&resource_context.lock);
	return status;
}

static u32 res_trk_get_clk_rate(unsigned long *phclk_rate)
{
	u32 status = true;
	mutex_lock(&resource_context.lock);
	if (phclk_rate) {
		*phclk_rate = clk_get_rate(resource_context.vcodec_clk);
		if (!(*phclk_rate)) {
			VCDRES_MSG_ERROR("vidc hclk get rate failed\n");
			status = false;
		}
	} else
		status = false;
	mutex_unlock(&resource_context.lock);
	return status;
}

u32 res_trk_disable_clocks(void)
{
	u32 status = false;
	VCDRES_MSG_LOW("in res_trk_disable_clocks()\n");
	mutex_lock(&resource_context.lock);
	if (resource_context.clock_enabled) {
		VCDRES_MSG_LOW("Disabling IRQ in %s()\n", __func__);
		disable_irq_nosync(resource_context.irq_num);
		VCDRES_MSG_LOW("%s(): Disabling the clocks ...\n", __func__);
		resource_context.clock_enabled = 0;
		if (resource_context.vcodec_clk)
			clk_disable(resource_context.vcodec_clk);
		if (resource_context.vcodec_pclk)
			clk_disable(resource_context.vcodec_pclk);
		status = true;
	}
	mutex_unlock(&resource_context.lock);
	return status;
}

static u32 res_trk_vidc_pwr_up(void)
{
	mutex_lock(&resource_context.lock);

	if (pm_runtime_get(resource_context.device) < 0) {
		VCDRES_MSG_ERROR("Error : pm_runtime_get failed\n");
		goto bail_out;
	}
	resource_context.footswitch = regulator_get(NULL, "fs_ved");
	if (IS_ERR(resource_context.footswitch)) {
		VCDRES_MSG_ERROR("foot switch get failed\n");
		resource_context.footswitch = NULL;
	} else
		regulator_enable(resource_context.footswitch);
	if (!res_trk_get_clk())
		goto rel_vidc_pm_runtime;
	mutex_unlock(&resource_context.lock);
	return true;

rel_vidc_pm_runtime:
	if (pm_runtime_put(resource_context.device) < 0)
		VCDRES_MSG_ERROR("Error : pm_runtime_put failed");
bail_out:
	mutex_unlock(&resource_context.lock);
	return false;
}

static struct ion_client *res_trk_create_ion_client(void){
	struct ion_client *video_client;
	video_client = msm_ion_client_create((1<<ION_HEAP_TYPE_CARVEOUT),
						"video_client");
	return video_client;
}

u32 res_trk_power_up(void)
{
	VCDRES_MSG_LOW("clk_regime_rail_enable");
	VCDRES_MSG_LOW("clk_regime_sel_rail_control");
#ifdef CONFIG_MSM_BUS_SCALING
	resource_context.pcl = 0;
	if (resource_context.vidc_bus_client_pdata) {
		resource_context.pcl = msm_bus_scale_register_client(
			resource_context.vidc_bus_client_pdata);
		VCDRES_MSG_LOW("%s(), resource_context.pcl = %x", __func__,
			 resource_context.pcl);
	}
	if (resource_context.pcl == 0) {
		dev_err(resource_context.device,
			"register bus client returned NULL\n");
		return false;
	}
#endif
	return res_trk_vidc_pwr_up();
}

u32 res_trk_power_down(void)
{
	VCDRES_MSG_LOW("clk_regime_rail_disable");
	res_trk_pmem_unmap(&resource_context.firmware_addr);
#ifdef CONFIG_MSM_BUS_SCALING
	msm_bus_scale_client_update_request(resource_context.pcl, 0);
	msm_bus_scale_unregister_client(resource_context.pcl);
#endif
	VCDRES_MSG_MED("res_trk_power_down():: Calling "
		"res_trk_shutdown_vidc()\n");
	return res_trk_shutdown_vidc();
}

u32 res_trk_get_max_perf_level(u32 *pn_max_perf_lvl)
{
	if (!pn_max_perf_lvl) {
		VCDRES_MSG_ERROR("%s(): pn_max_perf_lvl is NULL\n",
			__func__);
		return false;
	}
	*pn_max_perf_lvl = RESTRK_1080P_MAX_PERF_LEVEL;
	return true;
}

#ifdef CONFIG_MSM_BUS_SCALING
int res_trk_update_bus_perf_level(struct vcd_dev_ctxt *dev_ctxt, u32 perf_level)
{
	struct vcd_clnt_ctxt *cctxt_itr = NULL;
	u32 enc_perf_level = 0, dec_perf_level = 0;
	u32 bus_clk_index, client_type = 0;
	int rc = 0;

	cctxt_itr = dev_ctxt->cctxt_list_head;
	while (cctxt_itr) {
		if (cctxt_itr->decoding)
			dec_perf_level += cctxt_itr->reqd_perf_lvl;
		else
			enc_perf_level += cctxt_itr->reqd_perf_lvl;
		cctxt_itr = cctxt_itr->next;
	}
	if (!enc_perf_level)
		client_type = 1;
	if (perf_level <= RESTRK_1080P_VGA_PERF_LEVEL)
		bus_clk_index = 0;
	else if (perf_level <= RESTRK_1080P_720P_PERF_LEVEL)
		bus_clk_index = 1;
	else
		bus_clk_index = 2;

	if (dev_ctxt->reqd_perf_lvl + dev_ctxt->curr_perf_lvl == 0)
		bus_clk_index = 2;

	bus_clk_index = (bus_clk_index << 1) + (client_type + 1);
	VCDRES_MSG_LOW("%s(), bus_clk_index = %d", __func__, bus_clk_index);
	VCDRES_MSG_LOW("%s(),context.pcl = %x", __func__, resource_context.pcl);
	VCDRES_MSG_LOW("%s(), bus_perf_level = %x", __func__, perf_level);
	rc = msm_bus_scale_client_update_request(resource_context.pcl,
		bus_clk_index);
	return rc;
}
#endif

u32 res_trk_set_perf_level(u32 req_perf_lvl, u32 *pn_set_perf_lvl,
	struct vcd_dev_ctxt *dev_ctxt)
{
	u32 vidc_freq = 0;
	if (!pn_set_perf_lvl || !dev_ctxt) {
		VCDRES_MSG_ERROR("%s(): NULL pointer! dev_ctxt(%p)\n",
			__func__, dev_ctxt);
		return false;
	}
	VCDRES_MSG_LOW("%s(), req_perf_lvl = %d", __func__, req_perf_lvl);
#ifdef CONFIG_MSM_BUS_SCALING
	if (!res_trk_update_bus_perf_level(dev_ctxt, req_perf_lvl) < 0) {
		VCDRES_MSG_ERROR("%s(): update buf perf level failed\n",
			__func__);
		return false;
	}

#endif
	if (dev_ctxt->reqd_perf_lvl + dev_ctxt->curr_perf_lvl == 0)
		req_perf_lvl = RESTRK_1080P_MAX_PERF_LEVEL;

	if (req_perf_lvl <= RESTRK_1080P_VGA_PERF_LEVEL) {
		vidc_freq = vidc_clk_table[0];
		*pn_set_perf_lvl = RESTRK_1080P_VGA_PERF_LEVEL;
	} else if (req_perf_lvl <= RESTRK_1080P_720P_PERF_LEVEL) {
		vidc_freq = vidc_clk_table[1];
		*pn_set_perf_lvl = RESTRK_1080P_720P_PERF_LEVEL;
	} else {
		vidc_freq = vidc_clk_table[2];
		*pn_set_perf_lvl = RESTRK_1080P_MAX_PERF_LEVEL;
	}
	resource_context.perf_level = *pn_set_perf_lvl;
	VCDRES_MSG_MED("VIDC: vidc_freq = %u, req_perf_lvl = %u\n",
		vidc_freq, req_perf_lvl);
#ifdef USE_RES_TRACKER
    if (req_perf_lvl != RESTRK_1080P_MIN_PERF_LEVEL) {
		VCDRES_MSG_MED("%s(): Setting vidc freq to %u\n",
			__func__, vidc_freq);
		if (!res_trk_sel_clk_rate(vidc_freq)) {
			VCDRES_MSG_ERROR("%s(): res_trk_sel_clk_rate FAILED\n",
				__func__);
			*pn_set_perf_lvl = 0;
			return false;
		}
	}
#endif
	VCDRES_MSG_MED("%s() set perl level : %d", __func__, *pn_set_perf_lvl);
	return true;
}

u32 res_trk_get_curr_perf_level(u32 *pn_perf_lvl)
{
	unsigned long freq;

	if (!pn_perf_lvl) {
		VCDRES_MSG_ERROR("%s(): pn_perf_lvl is NULL\n",
			__func__);
		return false;
	}
	VCDRES_MSG_LOW("clk_regime_msm_get_clk_freq_hz");
	if (!res_trk_get_clk_rate(&freq)) {
		VCDRES_MSG_ERROR("%s(): res_trk_get_clk_rate FAILED\n",
			__func__);
		*pn_perf_lvl = 0;
		return false;
	}
	*pn_perf_lvl = resource_context.perf_level;
	VCDRES_MSG_MED("%s(): freq = %lu, *pn_perf_lvl = %u", __func__,
		freq, *pn_perf_lvl);
	return true;
}

u32 res_trk_download_firmware(void)
{
	const struct firmware *fw_video = NULL;
	int rc = 0;
	u32 status = true;

	VCDRES_MSG_HIGH("%s(): Request firmware download\n",
		__func__);
	mutex_lock(&resource_context.lock);
	rc = request_firmware(&fw_video, VIDC_FW,
		resource_context.device);
	if (rc) {
		VCDRES_MSG_ERROR("request_firmware for %s error %d\n",
				VIDC_FW, rc);
		status = false;
		goto bail_out;
	}
	vidc_video_codec_fw = (unsigned char *)fw_video->data;
	vidc_video_codec_fw_size = (u32) fw_video->size;
bail_out:
	mutex_unlock(&resource_context.lock);
	return status;
}

void res_trk_init(struct device *device, u32 irq)
{
	if (resource_context.device || resource_context.irq_num ||
		!device) {
		VCDRES_MSG_ERROR("%s() Resource Tracker Init error\n",
			__func__);
	} else {
		memset(&resource_context, 0, sizeof(resource_context));
		mutex_init(&resource_context.lock);
		resource_context.device = device;
		resource_context.irq_num = irq;
		resource_context.vidc_platform_data =
			(struct msm_vidc_platform_data *) device->platform_data;
		if (resource_context.vidc_platform_data) {
			resource_context.memtype =
			resource_context.vidc_platform_data->memtype;
			if (resource_context.vidc_platform_data->enable_ion) {
				resource_context.res_ion_client =
					res_trk_create_ion_client();
				if (!(resource_context.res_ion_client)) {
					VCDRES_MSG_ERROR("%s()ION createfail\n",
							__func__);
					return;
				}
			}
			resource_context.disable_dmx =
			resource_context.vidc_platform_data->disable_dmx;
			resource_context.disable_fullhd =
			resource_context.vidc_platform_data->disable_fullhd;
#ifdef CONFIG_MSM_BUS_SCALING
			resource_context.vidc_bus_client_pdata =
			resource_context.vidc_platform_data->
				vidc_bus_client_pdata;
#endif
		} else {
			resource_context.memtype = -1;
			resource_context.disable_dmx = 0;
		}
		resource_context.core_type = VCD_CORE_1080P;
		resource_context.firmware_addr.mem_type = DDL_FW_MEM;
		if (!res_trk_pmem_alloc(&resource_context.firmware_addr,
			VIDC_FW_SIZE, DDL_KILO_BYTE(128))) {
			pr_err("%s() Firmware buffer allocation failed",
				   __func__);
			memset(&resource_context.firmware_addr, 0,
				   sizeof(resource_context.firmware_addr));
		}
	}
}

u32 res_trk_get_core_type(void){
	return resource_context.core_type;
}

u32 res_trk_get_firmware_addr(struct ddl_buf_addr *firm_addr)
{
	if (!firm_addr || resource_context.firmware_addr.mapped_buffer) {
		pr_err("%s() invalid params", __func__);
		return -1;
	}
	if (!res_trk_pmem_map(&resource_context.firmware_addr,
		resource_context.firmware_addr.buffer_size, DDL_KILO_BYTE(128))) {
		pr_err("%s() Firmware buffer mapping failed",
			   __func__);
		return -1;
	}
	memcpy(firm_addr, &resource_context.firmware_addr,
		sizeof(struct ddl_buf_addr));
	return 0;
}

u32 res_trk_get_mem_type(void){
	return resource_context.memtype;
}

u32 res_trk_get_enable_ion(void)
{
	if (resource_context.vidc_platform_data->enable_ion)
		return 1;
	else
		return 0;
}

struct ion_client *res_trk_get_ion_client(void)
{
	return resource_context.res_ion_client;
}

u32 res_trk_get_disable_dmx(void){
	return resource_context.disable_dmx;
}

void res_trk_set_mem_type(enum ddl_mem_area mem_type)
{
	resource_context.res_mem_type = mem_type;
	return;
}

u32 res_trk_get_disable_fullhd(void)
{
	return resource_context.disable_fullhd;
}

int res_trk_enable_iommu_clocks(void)
{
	int ret = 0, i;
	if (resource_context.mmu_clks_on) {
		pr_err(" %s: Clocks are already on", __func__);
		return -EINVAL;
	}
	resource_context.mmu_clks_on = 1;
	for (i = 0; i < ARRAY_SIZE(vidc_mmu_clks); i++) {
		vidc_mmu_clks[i].mmu_clk = clk_get(resource_context.device,
			vidc_mmu_clks[i].mmu_clk_name);
		if (IS_ERR(vidc_mmu_clks[i].mmu_clk)) {
			pr_err(" %s: Get failed for clk %s", __func__,
				   vidc_mmu_clks[i].mmu_clk_name);
			ret = PTR_ERR(vidc_mmu_clks[i].mmu_clk);
		}
		if (!ret) {
			ret = clk_enable(vidc_mmu_clks[i].mmu_clk);
			if (ret) {
				clk_put(vidc_mmu_clks[i].mmu_clk);
				vidc_mmu_clks[i].mmu_clk = NULL;
			}
		}
		if (ret) {
			for (i--; i >= 0; i--) {
				clk_disable(vidc_mmu_clks[i].mmu_clk);
				clk_put(vidc_mmu_clks[i].mmu_clk);
				vidc_mmu_clks[i].mmu_clk = NULL;
			}
			resource_context.mmu_clks_on = 0;
			pr_err("%s() clocks enable failed", __func__);
			break;
		}
	}
	return ret;
}

int res_trk_disable_iommu_clocks(void)
{
	int i;
	if (!resource_context.mmu_clks_on) {
		pr_err(" %s: clks are already off", __func__);
		return -EINVAL;
	}
	resource_context.mmu_clks_on = 0;
	for (i = 0; i < ARRAY_SIZE(vidc_mmu_clks); i++) {
		clk_disable(vidc_mmu_clks[i].mmu_clk);
		clk_put(vidc_mmu_clks[i].mmu_clk);
		vidc_mmu_clks[i].mmu_clk = NULL;
	}
	return 0;
}
