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




int
main(int argc, char ** argv)
{
	int i=0;
	int ret = rte_eal_init(argc, argv);
	struct rte_pci_driver * drv = rte_pci_bus.driver_list.tqh_first; 
	if (ret < 0)
		rte_panic("Cannot init EAL\n");

	struct rte_rawdev_info rdev_info = { 0 };
    	printf("%d\n", rte_rawdev_count());
	int j = 0;    
	int rdev_id = 0;
            rte_rawdev_info_get(rdev_id, &rdev_info);
	 printf("%s\n", rdev_info.driver_name);

    	ioat_test(rdev_id);
    	return 0;
}
