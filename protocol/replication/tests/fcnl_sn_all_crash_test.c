//----------------------------------------------------------------------------
// ZetaScale
// Copyright (c) 2016, SanDisk Corp. and/or all its affiliates.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License version 2.1 as published by the Free
// Software Foundation;
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License v2.1 for more details.
//
// A copy of the GNU Lesser General Public License v2.1 is provided with this package and
// can also be found at: http://opensource.org/licenses/LGPL-2.1
// You should have received a copy of the GNU Lesser General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 59 Temple
// Place, Suite 330, Boston, MA 02111-1307 USA.
//----------------------------------------------------------------------------

/*
 * File: fcnl_sn_all_crash_test.c
 * Author: Zhenwei Lu
 *
 * Scenario:
 * 4 nodes, 0, 1, 2, 3, 3 replicas
 * shard 100 (2, 3, 0) 2 is home node
 * shard 101 (3, 0, 1) 3 is home node
 * shard 102 (1, 2, 3) 1 is home node
 * create shards, write some objects and verify
 * delete some objects from node 2, 3
 * crash node 1, 2, 3 and read objects from node 0
 * restart node 1, 2, 3 and read objects from them
 *
 * Created on Jun 22, 2009, 12:04 AM
 *
 * Copyright Schooner Information Technology, Inc.
 * http://www.schoonerinfotech.com/
 *
 * $Id$
 */

#include "fth/fthOpt.h"
#include "platform/stdio.h"
#include "test_framework.h"

#define RT_USE_COMMON 1
#include "test_common.h"

/*
 * We use a sub-category under test because test implies a huge number
 * of log messages out of simulated networking, flash, etc.
 */
PLAT_LOG_SUBCAT_LOCAL(LOG_CAT, PLAT_LOG_CAT_SDF_PROT_REPLICATION,
                      "test/case");


#define PLAT_OPTS_NAME(name) name ## _sync_test
#include "platform/opts.h"
#include "misc/misc.h"

#define PLAT_OPTS_ITEMS_sync_test() \
    PLAT_OPTS_COMMON_TEST(common_config)

#define SLEEP_US 1000000

struct plat_opts_config_sync_test {
    struct rt_common_test_config common_config;
};

#define NUM_REPLICAS 3
#define NUM_NODES 4

void
user_operations_mc_test(uint64_t args) {
    struct replication_test_framework *test_framework =
        (struct replication_test_framework *)args;
    SDF_shardid_t shard_id[3] = {100, 101, 102};
    struct SDF_shard_meta *shard_meta[3];
    /* configuration infomation about shard */
    SDF_replication_props_t *replication_props = NULL;
    SDF_status_t op_ret = SDF_SUCCESS;
    vnode_t node[NUM_REPLICAS] = {1, 2, 3};
    replication_test_framework_read_data_free_cb_t free_cb;
    int failed, i;

    char keys[2][5] = {"key0", "key1"};
    char *key;
    size_t key_len;
    char *data_in[NUM_REPLICAS][2];
    void *data_out;
    size_t data_len_out;
    int data_generation = 0;

    failed = !plat_calloc_struct(&meta);
    plat_assert(!failed);
    replication_test_meta_init(meta);

    /* Assure test_framework is started?! */
    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "start test_framework");
    rtfw_start(test_framework);
    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "test_framework started\n");

    /* Start all nodes */
    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "start nodes");
    rtfw_start_all_nodes(test_framework);
    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "nodes started");


    /* configures test framework accommodate to RT_TYPE_META_STORAGE */
    failed = !(plat_calloc_struct(&replication_props));
    plat_assert(!failed);

    /* initialize replciation properties and create shards */
    rtfw_set_default_replication_props(&test_framework->config,
                                       replication_props);
    for (i = 1; i <= NUM_REPLICAS; i++) {
        int j = i % NUM_REPLICAS;
        shard_meta[j] = rtfw_init_shard_meta(&test_framework->config, node[j] /* first */,
                                             shard_id[i-1]
                                             /* shard_id, in real system generated by generate_shard_ids() */,
                                             replication_props);
        plat_assert(shard_meta[j]);
        
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "create on node %d", node[j]);
        op_ret = rtfw_create_shard_sync(test_framework, node[j], shard_meta[j]);
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "create on node %d complete", node[j]);
        plat_assert(op_ret == SDF_SUCCESS);

        /* write two objects to nodes separately */
        plat_asprintf(&data_in[i-1][0], "data_%s_%d_%s_%d", "node", i, keys[0], data_generation);
        key = keys[0];
        key_len = strlen(key) + 1;
        plat_log_msg(LOG_ID, LOG_CAT, LOG_TRACE,
                     "write on node %d key:%s, key_len:%u, data:%s, data_len:%u",
                     i, key, (int)(strlen(key)), data_in[i-1][0], (int)(strlen(data_in[i-1][0])));
        op_ret = rtfw_write_sync(test_framework, shard_id[i-1] /* shard */, node[j] /* node */,
                                 meta /* test_meta */, key, key_len, data_in[i-1][0],
                                 strlen(data_in[i-1][0])+1);
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "write on node %d complete", node[j]);
        plat_assert(op_ret == SDF_SUCCESS);

        plat_asprintf(&data_in[i-1][1], "data_%s_%d_%s_%d", "node", i, keys[1], data_generation);
        key = keys[1];
        key_len = strlen(key) + 1;
        plat_log_msg(LOG_ID, LOG_CAT, LOG_TRACE,
                     "write on node %d key:%s, key_len:%u, data:%s, data_len:%u",
                     i, key, (int)(strlen(key)), data_in[i-1][1], (int)(strlen(data_in[i-1][1])));
        op_ret = rtfw_write_sync(test_framework, shard_id[i-1] /* shard */, node[j] /* node */,
                                 meta /* test_meta */, key, key_len, data_in[i-1][1],
                                 strlen(data_in[i-1][1])+1);
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "write on node %d complete", i);
        plat_assert(op_ret == SDF_SUCCESS);

        /* get object from nodes */
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "read on node %d", node[j]);
        op_ret = rtfw_read_sync(test_framework, shard_id[i-1], node[j] /* node */, key,
                                key_len, &data_out, &data_len_out, &free_cb);
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "read on node %d complete", i);
        plat_assert(op_ret == SDF_SUCCESS);
        plat_assert(strcmp(data_out, data_in[i-1][1]) == 0);
        plat_closure_apply(replication_test_framework_read_data_free_cb, &free_cb,
                           data_out, data_len_out);
    }

    /* del an object from node1 */
    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "del %s from shard:%d node:%d", key, (int)shard_id[1], (int)node[2]);
    key = keys[0];
    key_len = strlen(key) + 1;
    op_ret = rtfw_delete_sync(test_framework, shard_id[1], node[2], key, key_len);
    plat_assert(op_ret == SDF_SUCCESS);
    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "del %s from shard:%d node:%d complete",
                 key, (int)shard_id[1], (int)node[2]);

    /* crash node1, node2, node3 */
    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "crash node 3");
    op_ret = rtfw_crash_node_sync(test_framework, 3);
    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "crash node 3 complete");
    plat_assert(op_ret == SDF_SUCCESS);

    /* Wait for node switch.  Delay is arbitrary and long */
    rtfw_sleep_usec(test_framework,
                    test_framework->config.replicator_config.lease_usecs * 2);

    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "crash node 2");
    op_ret = rtfw_crash_node_sync(test_framework, 2);
    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "crash node 2 complete");
    plat_assert(op_ret == SDF_SUCCESS);

    /* Wait for node swtich.  Delay is arbitrary and long */
    rtfw_sleep_usec(test_framework,
                    test_framework->config.replicator_config.lease_usecs * 2);

    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "crash node 1");
    op_ret = rtfw_crash_node_sync(test_framework, 1);
    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "crash node 1 complete");
    plat_assert(op_ret == SDF_SUCCESS);

    /* Wait for node swtich.  Delay is arbitrary and long */
    rtfw_sleep_usec(test_framework,
                    test_framework->config.replicator_config.lease_usecs * 2);

    /**
     * read for all available objects on node 0
     * all replica node of shard_id[2] are failed,
     * keys[0] has been deleted from shard_id[1]
     */
    for (i = 0; i < sizeof(shard_id) / sizeof(shard_id[0]) - 1; i++) {
        int j;
        for (j = 0; j < 2; j++) {
            key = keys[j];
            key_len = strlen(key) + 1;
            plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "read on node 0 shard %d", (int)shard_id[i]);
            op_ret = rtfw_read_sync(test_framework, shard_id[i], 0, key, key_len,
                                    &data_out, &data_len_out, &free_cb);
            if (0 == j && i == 1) {
                plat_assert(op_ret != SDF_SUCCESS);
                break;
            } else {
                plat_assert(op_ret == SDF_SUCCESS);
            }
            plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "read on node 0 shard %d complete", (int)shard_id[i]);
            plat_assert(strcmp(data_out, data_in[i][j]) == 0);
            plat_closure_apply(replication_test_framework_read_data_free_cb, &free_cb,
                               data_out, data_len_out);
        }
    }

    void *data_out_temp[NUM_REPLICAS];
    size_t data_len_out_temp[NUM_REPLICAS];
    /* restart node 1, 2, 3, read objects and crash them */
    for (i = 0; i < NUM_REPLICAS; i++) {
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "start node %d", i+1);
        op_ret = rtfw_start_node(test_framework, i+1);
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "start node %d complete", i+1);
        plat_assert(op_ret == SDF_SUCCESS);

        /* Wait for recovery.  Delay is arbitrary and long */
        rtfw_sleep_usec(test_framework,
                        test_framework->config.replicator_config.lease_usecs * 2);

        /* verify values on shard_id 102, key1 since 102 replicate across on (1, 2, 3) */
        key = keys[1];
        key_len = strlen(key) + 1;
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "read on node %d shard %d", i+1, (int)shard_id[2]);
        op_ret = rtfw_read_sync(test_framework, shard_id[2], i+1, key, key_len,
                                &data_out_temp[i], &data_len_out_temp[i], &free_cb);
        /**
         * FIXME: lease expire on my machine so that wait_meta can't require lease
         * #cr_meta_update_cb (rms->rms_shard_meta_cb_t for update shard meta)
         *     |--> #cr_shard_meta_update_external
         *          |--> cr_shard_update_meta (seqno check failure)
         */
        plat_assert(op_ret == SDF_SUCCESS);
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "read on node %d shard %d complete", i+1, (int)shard_id[i]);

        /* crash the node to assure the subsequent started node is home node */
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "crash node %d", i+1);
        op_ret = rtfw_crash_node_sync(test_framework, i+1);
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "crash node %d complete", i+1);
        plat_assert(op_ret == SDF_SUCCESS);

        /* Wait for recovery.  Delay is arbitrary and long */
        rtfw_sleep_usec(test_framework,
                        test_framework->config.replicator_config.lease_usecs * 2);
    }

    /**
     * FIXME: perhaps we need a solution to prevent lease expire so that we can make
     * sure home node switch process are completed and target home node is available
     */
    plat_assert(strcmp(data_out_temp[0], data_out_temp[1]) == 0);
    plat_assert(strcmp(data_out_temp[2], data_out_temp[1]) == 0);
    plat_assert(strcmp(data_in[2][1], data_out_temp[1]) == 0);

    for (i = 0; i < NUM_REPLICAS; i++) {
        plat_closure_apply(replication_test_framework_read_data_free_cb, &free_cb,
                           data_out_temp[i], data_len_out_temp[i]);
    }


    /* start node 1, 2, 3 1 by 1 */
    for (i = 0; i < NUM_REPLICAS; i++) {
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "start node %d", i+1);
        op_ret = rtfw_start_node(test_framework, i+1);
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "start node %d complete", i+1);
        plat_assert(op_ret == SDF_SUCCESS);

        /* Wait for recovery.  Delay is arbitrary and long */
        rtfw_sleep_usec(test_framework,
                        test_framework->config.replicator_config.lease_usecs * 2);
    }

    /* Shutdown test framework */
    plat_log_msg(LOG_ID, LOG_CAT, LOG_TRACE,
                 "\n************************************************************\n"
                 "                  Test framework shutdown                       "
                 "\n************************************************************");
    rtfw_shutdown_sync(test_framework);

    for (i = 0; i < NUM_REPLICAS; i++) {
        plat_free(data_in[i][0]);
        plat_free(data_in[i][1]);
    }
    plat_free(meta);
    plat_free(replication_props);
    plat_free(shard_meta[0]);
    plat_free(shard_meta[1]);
    plat_free(shard_meta[2]);

    /* Terminate scheduler if idle_thread exit */
    while (test_framework->timer_dispatcher) {
        fthYield(-1);
    }
    plat_free(test_framework);

    /* Terminate scheduler */
    fthKill(1);
}

int main(int argc, char **argv) {
    SDF_status_t status;
    struct replication_test_framework *test_framework = NULL;

    struct plat_opts_config_sync_test opts_config;
    memset(&opts_config, 0, sizeof (opts_config));

    rt_common_test_config_init(&opts_config.common_config);
    opts_config.common_config.test_config.nnode = NUM_NODES;
    opts_config.common_config.test_config.num_replicas = NUM_REPLICAS;
    opts_config.common_config.test_config.replication_type =
        SDF_REPLICATION_META_SUPER_NODE;
    opts_config.common_config.test_config.replicator_config.lease_usecs =
        100 * MILLION;


    int opts_status = plat_opts_parse_sync_test(&opts_config, argc, argv);
    if (opts_status) {
        return (1);
    }

    status = rt_sm_init(&opts_config.common_config.shmem_config);
    if (status) {
        return (1);
    }

    /* start fthread library */
    fthInit();

    test_framework =
        replication_test_framework_alloc(&opts_config.common_config.test_config);
    if (test_framework) {
        plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "test_framework %p allocated\n",
                     test_framework);
    }
    XResume(fthSpawn(&user_operations_mc_test, 40960), (uint64_t)test_framework);
    fthSchedulerPthread(0);
    plat_log_msg(LOG_ID, LOG_CAT, LOG_DBG, "JOIN");

    rt_sm_detach();

    rt_common_test_config_destroy(&opts_config.common_config);

    return (0);
}
#include "platform/opts_c.h"
