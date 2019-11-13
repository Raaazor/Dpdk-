#include <unistd.h>
#include <inttypes.h>
#include <rte_mbuf.h>
#include <rte_rawdev.h>
#include <rte_ioat_rawdev.h>
#include <rte_timer.h>
#include <rte_memory.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_pci.h>
#include <rte_bus_pci.h>
#include <rte_bus.h>
#include <rte_rawdev_pmd.h>
#include <string.h>
#include <stdio.h>
static struct rte_mempool *pool;
char err[100] = "error here";
extern struct rte_pci_bus rte_pci_bus;
static int 
enqueue_copies(int dev_id)
{
	const unsigned int length = 1024;
	unsigned int i;

	/* test doing multiple copies */
	do {
		struct rte_mbuf *srcs[32], *dsts[32];
		struct rte_mbuf *completed_src[64];
		struct rte_mbuf *completed_dst[64];
		unsigned int j;
	        uint64_t tsc_start, tsc_end;

		for (i = 0; i < RTE_DIM(srcs); i++) {
			char *src_data;

			srcs[i] = rte_pktmbuf_alloc(pool);
			dsts[i] = rte_pktmbuf_alloc(pool);
			srcs[i]->data_len = srcs[i]->pkt_len = length;
			dsts[i]->data_len = dsts[i]->pkt_len = length;
			src_data = rte_pktmbuf_mtod(srcs[i], char *);

			for (j = 0; j < length; j++)
				src_data[j] = rand() & 0xFF;

			if (rte_ioat_enqueue_copy(dev_id,
					srcs[i]->buf_iova + srcs[i]->data_off,
					dsts[i]->buf_iova + dsts[i]->data_off,
					length,
					(uintptr_t)srcs[i],
					(uintptr_t)dsts[i],
					0 /* nofence */) != 1) {
				printf("Error with rte_ioat_enqueue_copy for buffer %u\n",
						i);
				return -1;
			}
		}

 	tsc_start = rte_rdtsc_precise();
		rte_ioat_do_copies(dev_id);
    	tsc_end = rte_rdtsc_precise();

    	printf("Test copy time: TSC = %"PRIu64"\n",
			tsc_end-tsc_start);
		usleep(100);

		if (rte_ioat_completed_copies(dev_id, 64, (void *)completed_src,
				(void *)completed_dst) != RTE_DIM(srcs)) {
			printf("Error with rte_ioat_completed_copies\n");
			return -1;
		}
		for (i = 0; i < RTE_DIM(srcs); i++) {
			char *src_data, *dst_data;

			if (completed_src[i] != srcs[i]) {
				printf("Error with source pointer %u\n", i);
				return -1;
			}
			if (completed_dst[i] != dsts[i]) {
				printf("Error with dest pointer %u\n", i);
				return -1;
			}

			src_data = rte_pktmbuf_mtod(srcs[i], char *);
			dst_data = rte_pktmbuf_mtod(dsts[i], char *);
			for (j = 0; j < length; j++)
				if (src_data[j] != dst_data[j]) {
					printf("Error with copy of packet %u, byte %u\n",
							i, j);
					return -1;
				}
			rte_pktmbuf_free(srcs[i]);
			rte_pktmbuf_free(dsts[i]);
		}

	} while (0);

	return 0;
}


static int
ioat_test(uint16_t dev_id)
{
#define IOAT_TEST_RINGSIZE 512
	struct rte_ioat_rawdev_config p = { .ring_size = -1 };
	struct rte_rawdev_info info = { .dev_private = &p };
	struct rte_rawdev_xstats_name *snames = NULL;
	uint64_t *stats = NULL;
	unsigned int *ids = NULL;
	unsigned int nb_xstats;
	unsigned int i;

	rte_rawdev_info_get(dev_id, &info);
	if (p.ring_size != 0) {
		printf("Error, initial ring size is non-zero (%d)\n",
				(int)p.ring_size);
		return -1;
	}

	p.ring_size = IOAT_TEST_RINGSIZE;
	if (rte_rawdev_configure(dev_id, &info) != 0) {		printf("Error with rte_rawdev_configure()\n");
		return -1;
	}
	rte_rawdev_info_get(dev_id, &info);
	if (p.ring_size != IOAT_TEST_RINGSIZE) {
		printf("Error, ring size is not %d (%d)\n",
				IOAT_TEST_RINGSIZE, (int)p.ring_size);
		return -1;
	}

	if (rte_rawdev_start(dev_id) != 0) {
		printf("Error with rte_rawdev_start()\n");
		return -1;
	}

	pool = rte_pktmbuf_pool_create("TEST_IOAT_POOL",
			256, /* n == num elements */
			32,  /* cache size */
			0,   /* priv size */
			2048, /* data room size */
			info.socket_id);
	if (pool == NULL) {
		printf("Error with mempool creation\n");
		return -1;
	}

	/* allocate memory for xstats names and values */
	nb_xstats = rte_rawdev_xstats_names_get(dev_id, NULL, 0);

	snames = malloc(sizeof(*snames) * nb_xstats);
	if (snames == NULL) {
		printf("Error allocating xstat names memory\n");
		goto err;
	}
	rte_rawdev_xstats_names_get(dev_id, snames, nb_xstats);

	ids = malloc(sizeof(*ids) * nb_xstats);
	if (ids == NULL) {
		printf("Error allocating xstat ids memory\n");
		goto err;
	}
	for (i = 0; i < nb_xstats; i++)
		ids[i] = i;

	stats = malloc(sizeof(*stats) * nb_xstats);
	if (stats == NULL) {
		printf("Error allocating xstat memory\n");
		goto err;
	}

	/* run the test cases */
	for (i = 0; i < 100; i++) {
		unsigned int j;

		if (enqueue_copies(dev_id) != 0)
			goto err;

		rte_rawdev_xstats_get(dev_id, ids, stats, nb_xstats);
		for (j = 0; j < nb_xstats; j++)
			printf("%s: %"PRIu64"   ", snames[j].name, stats[j]);
		printf("\r");
	}
	printf("\n");

	rte_mempool_free(pool);
	free(snames);
	free(stats);
	free(ids);
	return 0;

err:
	rte_mempool_free(pool);
	free(snames);
	free(stats);
	free(ids);
	return -1;
}



int ioat_pmd_logtype;

static struct rte_pci_driver ioat_pmd_drv;

#define IOAT_VENDOR_ID		0x8086
#define IOAT_DEVICE_ID_SKX	0x2021
#define IOAT_DEVICE_ID_BDX0	0x6f20
#define IOAT_DEVICE_ID_BDX1	0x6f21
#define IOAT_DEVICE_ID_BDX2	0x6f22
#define IOAT_DEVICE_ID_BDX3	0x6f23
#define IOAT_DEVICE_ID_BDX4	0x6f24
#define IOAT_DEVICE_ID_BDX5	0x6f25
#define IOAT_DEVICE_ID_BDX6	0x6f26
#define IOAT_DEVICE_ID_BDX7	0x6f27
#define IOAT_DEVICE_ID_BDXE	0x6f2E
#define IOAT_DEVICE_ID_BDXF	0x6f2F

#define IOAT_PMD_LOG(level, fmt, args...) rte_log(RTE_LOG_ ## level, \
	ioat_pmd_logtype, "%s(): " fmt "\n", __func__, ##args)

#define IOAT_PMD_DEBUG(fmt, args...)  IOAT_PMD_LOG(DEBUG, fmt, ## args)
#define IOAT_PMD_INFO(fmt, args...)   IOAT_PMD_LOG(INFO, fmt, ## args)
#define IOAT_PMD_ERR(fmt, args...)    IOAT_PMD_LOG(ERR, fmt, ## args)
#define IOAT_PMD_WARN(fmt, args...)   IOAT_PMD_LOG(WARNING, fmt, ## args)

#define DESC_SZ sizeof(struct rte_ioat_generic_hw_desc)
#define COMPLETION_SZ sizeof(__m128i)

static int
ioat_dev_configure(const struct rte_rawdev *dev, rte_rawdev_obj_t config)
{
	struct rte_ioat_rawdev_config *params = config;
	struct rte_ioat_rawdev *ioat = dev->dev_private;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	unsigned short i;

	if (dev->started)
		return -EBUSY;

	if (params == NULL)
		return -EINVAL;

	if (params->ring_size > 4096 || params->ring_size < 64 ||
			!rte_is_power_of_2(params->ring_size))
		return -EINVAL;

	ioat->ring_size = params->ring_size;
	if (ioat->desc_ring != NULL) {
		rte_memzone_free(ioat->desc_mz);
		ioat->desc_ring = NULL;
		ioat->desc_mz = NULL;
	}

	/* allocate one block of memory for both descriptors
	 * and completion handles.
	 */
	snprintf(mz_name, sizeof(mz_name), "rawdev%u_desc_ring", dev->dev_id);
	ioat->desc_mz = rte_memzone_reserve(mz_name,
			(DESC_SZ + COMPLETION_SZ) * ioat->ring_size,
			dev->device->numa_node, RTE_MEMZONE_IOVA_CONTIG);
	if (ioat->desc_mz == NULL)
		return -ENOMEM;
	ioat->desc_ring = ioat->desc_mz->addr;
	ioat->hdls = (void *)&ioat->desc_ring[ioat->ring_size];

	ioat->ring_addr = ioat->desc_mz->iova;

	/* configure descriptor ring - each one points to next */
	for (i = 0; i < ioat->ring_size; i++) {
		ioat->desc_ring[i].next = ioat->ring_addr +
				(((i + 1) % ioat->ring_size) * DESC_SZ);
	}

	return 0;
}

static int
ioat_dev_start(struct rte_rawdev *dev)
{
	struct rte_ioat_rawdev *ioat = dev->dev_private;

	if (ioat->ring_size == 0 || ioat->desc_ring == NULL)
		return -EBUSY;

	/* inform hardware of where the descriptor ring is */
	ioat->regs->chainaddr = ioat->ring_addr;
	/* inform hardware of where to write the status/completions */
	ioat->regs->chancmp = ioat->status_addr;

	/* prime the status register to be set to the last element */
	ioat->status =  ioat->ring_addr + ((ioat->ring_size - 1) * DESC_SZ);
	return 0;
}

static void
ioat_dev_stop(struct rte_rawdev *dev)
{
	RTE_SET_USED(dev);
}

static void
ioat_dev_info_get(struct rte_rawdev *dev, rte_rawdev_obj_t dev_info)
{
	struct rte_ioat_rawdev_config *cfg = dev_info;
	struct rte_ioat_rawdev *ioat = dev->dev_private;

	if (cfg != NULL)
		cfg->ring_size = ioat->ring_size;
}

static const char * const xstat_names[] = {
		"failed_enqueues", "successful_enqueues",
		"copies_started", "copies_completed"
};

static int
ioat_xstats_get(const struct rte_rawdev *dev, const unsigned int ids[],
		uint64_t values[], unsigned int n)
{
	const struct rte_ioat_rawdev *ioat = dev->dev_private;
	unsigned int i;

	for (i = 0; i < n; i++) {
		switch (ids[i]) {
		case 0: values[i] = ioat->enqueue_failed; break;
		case 1: values[i] = ioat->enqueued; break;
		case 2: values[i] = ioat->started; break;
		case 3: values[i] = ioat->completed; break;
		default: values[i] = 0; break;
		}
	}
	return n;
}

static int
ioat_xstats_get_names(const struct rte_rawdev *dev,
		struct rte_rawdev_xstats_name *names,
		unsigned int size)
{
	unsigned int i;

	RTE_SET_USED(dev);
	if (size < RTE_DIM(xstat_names))
		return RTE_DIM(xstat_names);

	for (i = 0; i < RTE_DIM(xstat_names); i++)
		strncpy(names[i].name, xstat_names[i], sizeof(names[i]));

	return RTE_DIM(xstat_names);
}

extern int ioat_rawdev_test(uint16_t dev_id);

static int
ioat_rawdev_create(const char *name, struct rte_pci_device *dev)
{
	static const struct rte_rawdev_ops ioat_rawdev_ops = {
			.dev_configure = ioat_dev_configure,
			.dev_start = ioat_dev_start,
			.dev_stop = ioat_dev_stop,
			.dev_info_get = ioat_dev_info_get,
			.xstats_get = ioat_xstats_get,
			.xstats_get_names = ioat_xstats_get_names,
			.dev_selftest = ioat_rawdev_test,
	};	
	printf("%s\n",err);
	struct rte_rawdev *rawdev = NULL;
	struct rte_ioat_rawdev *ioat = NULL;
	const struct rte_memzone *mz = NULL;
	char mz_name[RTE_MEMZONE_NAMESIZE];
	int ret = 0;
	int retry = 0;
;
	if (!name) {
		IOAT_PMD_ERR("Invalid name of the device!");
		ret = -EINVAL;
		goto cleanup;
	}
	printf("%s\n", err);
	/* Allocate device structure */
	rawdev = rte_rawdev_pmd_allocate(name, sizeof(struct rte_ioat_rawdev),
					 dev->device.numa_node);
	if (rawdev == NULL) {
		IOAT_PMD_ERR("Unable to allocate raw device");
		ret = -ENOMEM;
		goto cleanup;
	}

	snprintf(mz_name, sizeof(mz_name), "rawdev%u_private", rawdev->dev_id);
	mz = rte_memzone_reserve(mz_name, sizeof(struct rte_ioat_rawdev),
			dev->device.numa_node, RTE_MEMZONE_IOVA_CONTIG);
	if (mz == NULL) {
		IOAT_PMD_ERR("Unable to reserve memzone for private data\n");
		ret = -ENOMEM;
		goto cleanup;
	}
	char addr[100] = "void point error";
	if(dev->mem_resource[0].addr == NULL)
	printf("%s\n", addr);
	printf("%s\n",err);
	rawdev->dev_private = mz->addr;
	rawdev->dev_ops = &ioat_rawdev_ops;
	rawdev->device = &dev->device;
	rawdev->driver_name = dev->device.driver->name;

	ioat = rawdev->dev_private;
	ioat->rawdev = rawdev;
	ioat->mz = mz;
	ioat->regs =(struct rte_ioat_registers *)dev->mem_resource[0].addr;
	ioat->ring_size = 0;
	ioat->desc_ring = NULL;
	ioat->status_addr = ioat->mz->iova +
			offsetof(struct rte_ioat_rawdev, status);

	;
	/* do device initialization - reset and set error behaviour */
	if (ioat->regs->chancnt != 1)
		IOAT_PMD_ERR("%s: Channel count == %d\n", __func__,
				ioat->regs->chancnt);
	printf("%s\n",err);
	if (ioat->regs->chanctrl & 0x100) { /* locked by someone else */
		IOAT_PMD_WARN("%s: Channel appears locked\n", __func__);
		ioat->regs->chanctrl = 0;
	}
 	printf("%s\n", err);
	ioat->regs->chancmd = RTE_IOAT_CHANCMD_SUSPEND;
	rte_delay_ms(1);
	ioat->regs->chancmd = RTE_IOAT_CHANCMD_RESET;
	rte_delay_ms(1);
	while (ioat->regs->chancmd & RTE_IOAT_CHANCMD_RESET) {
		ioat->regs->chainaddr = 0;
		rte_delay_ms(1);
		if (++retry >= 200) {
			IOAT_PMD_ERR("%s: cannot reset device. CHANCMD=0x%"PRIx8", CHANSTS=0x%"PRIx64", CHANERR=0x%"PRIx32"\n",
					__func__,
					ioat->regs->chancmd,
					ioat->regs->chansts,
					ioat->regs->chanerr);
			ret = -EIO;
		}
	}
	ioat->regs->chanctrl = RTE_IOAT_CHANCTRL_ANY_ERR_ABORT_EN |
			RTE_IOAT_CHANCTRL_ERR_COMPLETION_EN;

	return 0;

cleanup:
	if (rawdev)
		rte_rawdev_pmd_release(rawdev);

	return ret;
}

static int
ioat_rawdev_destroy(const char *name)
{
	int ret;
	struct rte_rawdev *rdev;

	if (!name) {
		IOAT_PMD_ERR("Invalid device name");
		return -EINVAL;
	}

	rdev = rte_rawdev_pmd_get_named_dev(name);
	if (!rdev) {
		IOAT_PMD_ERR("Invalid device name (%s)", name);
		return -EINVAL;
	}

	if (rdev->dev_private != NULL) {
		struct rte_ioat_rawdev *ioat = rdev->dev_private;
		rdev->dev_private = NULL;
		rte_memzone_free(ioat->desc_mz);
		rte_memzone_free(ioat->mz);
	}

	/* rte_rawdev_close is called by pmd_release */
	ret = rte_rawdev_pmd_release(rdev);
	if (ret)
		IOAT_PMD_DEBUG("Device cleanup failed");

	return 0;
}

static int
ioat_rawdev_probe(struct rte_pci_driver *drv, struct rte_pci_device *dev)
{
	char name[32];
	int ret = 0;

	rte_pci_device_name(&dev->addr, name, sizeof(name));
	IOAT_PMD_INFO("Init %s on NUMA node %d", name, dev->device.numa_node);
        printf("The address of driver is %d\n ", &drv->driver); 
	printf("error with dev? %s\n", dev->device.name);
	dev->device.driver = &(drv->driver);
	printf("\n");
	ret = ioat_rawdev_create(name, dev);
	return ret;
}

static int
ioat_rawdev_remove(struct rte_pci_device *dev)
{
	char name[32];
	int ret;

	rte_pci_device_name(&dev->addr, name, sizeof(name));

	IOAT_PMD_INFO("Closing %s on NUMA node %d",
			name, dev->device.numa_node);

	ret = ioat_rawdev_destroy(name);
	return ret;
}



int
main(int argc, char ** argv)
{
	int i=0;
	int ret = rte_eal_init(argc, argv);
	struct rte_pci_driver * drv = rte_pci_bus.driver_list.tqh_first; 
	if (ret < 0)
		rte_panic("Cannot init EAL\n");

     	printf("%s\n", drv->next.tqe_next->next.tqe_next->driver.name);
	struct rte_pci_device * dev = rte_pci_bus.device_list.tqh_first->next.tqe_next;
 //   printf("%s\n", rte_pci_bus.device_list.tqh_first->next.tqe_next->device.name);
    	for(i = 0 ; strcmp(drv->driver.name, "rawdev_ioat") != 0;  i++)
    	{	drv = drv->next.tqe_next;
	}
//	drv->dma_map(dev, &dev->mem_resource[0].addr, NULL, dev->mem_resource[0].len);
	drv->probe(drv, rte_pci_bus.device_list.tqh_first->next.tqe_next);
  	ioat_rawdev_probe(drv, rte_pci_bus.device_list.tqh_first->next.tqe_next); 
//   	rte_pci_bus.device_list.tqh_first->next.tqe_next->device.driver = &drv->driver;
//  	rte_rawdev_pmd_allocate("0000:00:04.0", sizeof(struct rte_ioat_rawdev), 0);    
	struct rte_rawdev_info rdev_info = { 0 };
    	printf("%d\n", rte_rawdev_count());
	printf("%d\n", i);
	int j = 0;    
	int rdev_id = 0;
//      do {
	 printf("%d\n", i);	
            rte_rawdev_info_get(rdev_id, &rdev_info);
	 printf("%s\n", rdev_info.driver_name);
//	 } while (strcmp(rdev_info.driver_name,
//          IOAT_PMD_RAWDEV_NAME_STR) != 0);
    
//      if(rdev_id == rte_rawdev_count())	return -1;
    	ioat_test(rdev_id);
    	return 0;
}
