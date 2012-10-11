/*
 * File:   apps/memcached/server/memcached-1.2.5-schooner/mcd_osd.c
 * Author: Xiaonan Ma
 *
 * Created on Jan 08, 2009
 *
 * (c) Copyright 2009, Schooner Information Technology, Inc.
 * http://www.schoonerinfotech.com/
 *
 * $Id: mcd_osd.c 16149 2011-02-15 16:07:23Z briano $
 */

#include <stdio.h>
#include <signal.h>
#include <aio.h>
#include "common/sdftypes.h"
#include "common/sdfstats.h"
#include "platform/stdio.h"
#include "platform/assert.h"
#include "platform/errno.h"
#include "platform/logging.h"
#include "platform/shmem.h"
#include "platform/signal.h"
#include "platform/stdlib.h"
#include "platform/string.h"
#include "platform/time.h"
#include "platform/unistd.h"
#include "utils/hash.h"
#include "utils/properties.h"
#include "fth/fthMbox.h"

#include "protocol/protocol_common.h"
#include "protocol/action/action_internal_ctxt.h"
#include "shared/object.h"
#include "shared/open_container_mgr.h"

#include "fth/fth.h"
#include "ssd/ssd_local.h"
#include "ssd/fifo/fifo.h"
#include "ssd/ssd_aio.h"
#include "ssd/ssd_aio_local.h"

//#include "memcached.h"
//#include "command.h"
//#include "mcd_sdf.h"
//#include "mcd_rep.h"
#include "mcd_osd.h"
#include "mcd_aio.h"
#include "mcd_bak.h"
#include "mcd_pfx.h"
#include "mcd_rep.h"
#include "mcd_hash.h"

#include "shared/private.h"
#ifdef SDFAPI
#include "api/sdf.h"
#include "api/sdf_internal.h"
#include "shared/name_service.h"
extern
SDF_cguid_t generate_cguid(
        SDF_internal_ctxt_t     *pai,
        const char              *path,
        uint32_t                 node,
        int64_t                  cntr_id
        );
extern uint32_t
init_get_my_node_id();
#endif

/*
 *    Per fthread aio state.  All fthreads that call aio routines
 *    must have their own instance of aio state.
 */
#define AIO_MAX_CTXTS   2000
static osd_state_t    *Mcd_aio_states[AIO_MAX_CTXTS];
static uint32_t        Mcd_aio_state_next = 0;
static fthLock_t       Mcd_aio_ctxt_lock;

/*
 * tracking aio_context usage by each module
 */
static int Mcd_fth_aio_ctxts[SSD_AIO_CTXT_MAX_COUNT];

#define MCD_OSD_DBG_GET_DIV     10000
#define MCD_OSD_DBG_SET_DIV     10000

#ifdef  MCD_OSD_DEBUGGING
#  define MCD_OSD_LOG_LVL_DEBUG PLAT_LOG_LEVEL_DEBUG
#  define MCD_OSD_LOG_LVL_DIAG  PLAT_LOG_LEVEL_DIAGNOSTIC
#  define MCD_OSD_LOG_LVL_INFO  PLAT_LOG_LEVEL_INFO
#else
#  define MCD_OSD_LOG_LVL_INFO  PLAT_LOG_LEVEL_DEBUG
#  define MCD_OSD_LOG_LVL_DEBUG PLAT_LOG_LEVEL_DEBUG
#  define MCD_OSD_LOG_LVL_DIAG  PLAT_LOG_LEVEL_DEBUG
#endif

#ifndef MCD_ENABLE_FIFO
#  define MCD_ENABLE_SLAB
#endif

/* xxxzzz fix this linkage!
 */

// from container.c
extern struct shard * container_to_shard( SDF_internal_ctxt_t *pai, local_SDF_CONTAINER lc);
// from action_thread.h
extern void SDFClusterStatus(SDF_action_init_t *pai, uint32_t *mynode_id, uint32_t *cluster_size);
// from mcd_rep.c
extern uint64_t rep_seqno_get(struct shard * shard);

#ifdef SDFAPI
// from sdf.c
extern ctnr_map_t CtnrMap[MCD_MAX_NUM_CNTRS];
#endif /* SDFAPI */

/*
 * reserved blocks
 */
#define MCD_OSD_MIN_OFFSET      0

/*
 * minimum hash table size is 4GB
 */
#define MCD_OSD_HASH_SIZE       (512 * 1024 * 1024)

#define MCD_OSD_LOCK_BUCKETS    262144
#define MCD_OSD_LOCKBKT_MINSIZE 256

/*
 * size of the write staging buffers
 */
#define MCD_OSD_WBUF_SIZE       (16 * 1048576)
#define MCD_OSD_WBUF_BLKS       (MCD_OSD_WBUF_SIZE / MCD_OSD_BLK_SIZE)

#define MCD_OSD_MAGIC_NUMBER    MCD_OSD_META_MAGIC


typedef struct mcd_osd_wbuf {
    int                 id;
    uint32_t            ref_count;
    uint32_t            filled;
    uint64_t            blk_offset;
    char              * buf;
    uint32_t            items;
    char              * meta;
} mcd_osd_wbuf_t;

//mcd_container_t;

typedef struct mcd_osd_rbuf {
    int                 container_id;
    mcd_container_t   * container;
    uint64_t            err_offset;
    uint64_t            blk_offset;
    uint64_t            prev_seq;
    uint64_t            curr_seq;
    char              * buf;
} mcd_osd_rbuf_t;

static mcd_osd_rbuf_t *Raw_rbuf = NULL; // used for raw_get's

/*
 * cluster node information (needed for cas_id handling)
 */
uint32_t        Mcd_fth_node_id = 0;
uint32_t        Mcd_fth_num_nodes = 1;

/*
 * next volatile container generation number (for stale connection
 * handling)
 */
uint32_t        Mcd_next_cntr_gen = 1;

mcd_container_t Mcd_containers[MCD_MAX_NUM_CNTRS];

/*
 * saved container properties from SSD
 */
static mcd_container_t Mcd_fth_saved_props[MCD_MAX_NUM_CNTRS];

/*
 * mbox for admin container command usage
 */
static fthMbox_t       Mcd_fth_admin_mbox;
static uint32_t        Mcd_adm_pending_mails = 0;

int			        Mcd_osd_rand_blksize;

/*
 * local globals
 */
static uint64_t                 Mcd_osd_total_size      = 0;
static uint64_t                 Mcd_osd_total_blks      = 0;
static uint64_t                 Mcd_osd_free_blks       = 0;

static fthMbox_t                Mcd_osd_writer_mbox;
static fthMbox_t                Mcd_osd_sleeper_mbox;

/*
 * debugging stats
 */
uint64_t                        Mcd_osd_set_cmds        = 0;
uint64_t                        Mcd_osd_get_cmds        = 0;
uint64_t                        Mcd_osd_get_ram_hits    = 0;
uint64_t                        Mcd_osd_replaces        = 0;
uint64_t                        Mcd_osd_iobuf_total     = 0;
uint64_t                        Mcd_osd_hard_wakeups    = 0;

/*
 * from mcd_aio.c
 */
extern int                      Mcd_aio_num_files;
extern uint64_t                 Mcd_aio_strip_size;
extern uint64_t                 Mcd_aio_total_size;
extern uint64_t                 Mcd_aio_real_size;
extern uint64_t                 Mcd_aio_read_ops;
extern uint64_t                 Mcd_aio_write_ops;

/*
 *  Unique pthread id.
 */
static int                      thread_id = 0;
/*  
 *  This must be initialized to 0 for the startup sequence to work
 *  correctly.
 */
static __thread uint32_t        Mcd_pthread_id = 0;

void mcd_osd_assign_pthread_id()
{
    Mcd_pthread_id = __sync_fetch_and_add( &thread_id, 1 );
    mcd_log_msg( 150001, PLAT_LOG_LEVEL_INFO,
                 "pth_id=%d", Mcd_pthread_id );
}

/*
 * CMC dummy mcd_container
 */
mcd_container_t                 Mcd_osd_cmc_cntr;

/*
 * turns out plat_alloc allocates outside of guma for requests above
 * certain size (currently 64MB)
 */
#define plat_alloc_large        plat_alloc_steal_from_heap
#define plat_free_large         plat_free  // can't be freed, leaks memory

/*
 *   Predeclarations
 */
static void mcd_osd_shard_open_phase2( struct shard * shard, mcd_container_t * cntr );
static inline int mcd_fth_osd_slab_free( void * buf );
static int mcd_fth_do_try_container_internal( void * pai, int index, 
                              mcd_container_t **ppcontainer, bool open_only,
                              int tcp_port, int udp_port,
                              SDF_container_props_t * prop, char * cntr_name,
                              mcd_cntr_props_t * cntr_props );
static SDF_status_t
mcd_osd_delete_expired( osd_state_t *osd_state, mcd_osd_shard_t * shard );

/*
 * copied from mc_init.c
 */
static SDF_status_t
flash_to_sdf_status(int code) {
    SDF_status_t status = SDF_FAILURE;

    switch(code) {

    case FLASH_EOK:
	status = SDF_SUCCESS;
	break;

    case FLASH_ENOENT:
	status = SDF_OBJECT_UNKNOWN;
	break;

    case FLASH_EEXIST:
	status = SDF_OBJECT_EXISTS;
	break;

    case FLASH_EPERM:
    case FLASH_EAGAIN:
    case FLASH_ENOMEM:                       // Out of system memory
    case FLASH_EBUSY:
    case FLASH_EACCES:
    case FLASH_EINVAL:
    case FLASH_EMFILE:
    case FLASH_ENOSPC:
    case FLASH_ENOBUFS:
    case FLASH_EDQUOT:
    default:
        plat_log_msg( 20292, PLAT_LOG_CAT_SDF_AGENT,
                      PLAT_LOG_LEVEL_ERROR, "FLASH ERROR: code is %u", code);
	status = SDF_FAILURE;
	break;
    }

    return (status);
}


inline uint32_t
mcd_osd_blk_to_lba( uint32_t blocks )
{
    uint32_t    temp;

    if ( MCD_OSD_MAX_BLKS_OLD >= blocks ) {
        return blocks;
    }
    else {
        if ( 0 == blocks % MCD_OSD_LBA_MIN_BLKS ) {
            temp = blocks;
        }
        else {
            temp = blocks +
                MCD_OSD_LBA_MIN_BLKS - ( blocks % MCD_OSD_LBA_MIN_BLKS );
        }
        temp = ( temp >> MCD_OSD_LBA_SHIFT_BITS ) | MCD_OSD_LBA_SHIFT_FLAG;
    }

    return temp;
}


inline uint32_t
mcd_osd_lba_to_blk( uint32_t blocks )
{
    uint32_t    temp;

    if ( MCD_OSD_MAX_BLKS_OLD >= blocks ) {
        return blocks;
    }
    else {
        temp = ( blocks & MCD_OSD_LBA_SHIFT_MASK ) << MCD_OSD_LBA_SHIFT_BITS;
    }

    return temp;
}


#define MCD_OSD_BUF_STA_MAGIC   0x6fc74956dc6f7a45ULL
#define MCD_OSD_BUF_DYN_MAGIC   0x169e23ba6b9c7516ULL

typedef struct mcd_osd_buf_hdr {
    uint64_t            magic;
    uint16_t            offset;
    uint64_t            size;
} mcd_osd_buf_hdr_t;


/*
 * returned buffer is Mcd_osd_blk_size-aligned
 */
void * mcd_fth_osd_iobuf_alloc( size_t size, bool is_static )
{
    char                      * buf;
    char                      * temp;
    mcd_osd_buf_hdr_t         * buf_hdr;

    buf = plat_alloc( size + 2 * Mcd_osd_blk_size - 1 );
    if ( NULL == buf ) {
        return NULL;
    }

    temp = (char *)( ( (uint64_t)buf + Mcd_osd_blk_size - 1 )
                     & Mcd_osd_blk_mask );
    buf_hdr = (mcd_osd_buf_hdr_t *)temp;
    buf_hdr->magic = is_static ? MCD_OSD_BUF_STA_MAGIC : MCD_OSD_BUF_DYN_MAGIC;

    temp += Mcd_osd_blk_size;
    buf_hdr->offset = temp - buf;
    buf_hdr->size = size + 2 * Mcd_osd_blk_size - 1;
    (void) __sync_fetch_and_add( &Mcd_osd_iobuf_total, buf_hdr->size );

    mcd_log_msg( 50038, PLAT_LOG_LEVEL_DEBUG,
                 "buf=%p aligned=%p offset=%hu", buf, temp, buf_hdr->offset );
    return temp;
}


void
mcd_fth_osd_iobuf_free( void * buf )
{
    mcd_osd_buf_hdr_t         * buf_hdr;

    buf_hdr = (mcd_osd_buf_hdr_t *)(buf - Mcd_osd_blk_size);
    if ( MCD_OSD_BUF_STA_MAGIC != buf_hdr->magic &&
         MCD_OSD_BUF_DYN_MAGIC != buf_hdr->magic ) {
        mcd_log_msg( 50039, PLAT_LOG_LEVEL_FATAL,
                     "not enough magic in buffer header" );
        plat_abort();
    }

    (void) __sync_fetch_and_sub( &Mcd_osd_iobuf_total, buf_hdr->size );

    mcd_log_msg( 50040, PLAT_LOG_LEVEL_DEBUG,
                 "freeing iobuf, buf=%p orig=%p", buf, buf - buf_hdr->offset );
    plat_free( buf - buf_hdr->offset );
}


static inline int
mcd_fth_osd_slab_free( void * buf )
{
    mcd_osd_buf_hdr_t         * buf_hdr;

    buf_hdr = (mcd_osd_buf_hdr_t *)(buf - Mcd_osd_blk_size);
    if ( MCD_OSD_BUF_STA_MAGIC == buf_hdr->magic ) {
        return 0;
    }
    else if ( MCD_OSD_BUF_DYN_MAGIC == buf_hdr->magic ) {
        mcd_log_msg( 50040, PLAT_LOG_LEVEL_DEBUG,
                     "freeing iobuf, buf=%p orig=%p",
                     buf, buf - buf_hdr->offset );
        (void) __sync_fetch_and_sub( &Mcd_osd_iobuf_total, buf_hdr->size );
        plat_free( buf - buf_hdr->offset );
        return 0;
    }

    mcd_log_msg( 50039, PLAT_LOG_LEVEL_FATAL,
                 "not enough magic in buffer header" );
    plat_abort();
    return -1;
}


int
mcd_osd_release_buf( void * buf )
{
    mcd_log_msg( 20407, PLAT_LOG_LEVEL_DEBUG, "ENTERING, buf=%p", buf );

    return mcd_fth_osd_slab_free( buf );
}


/************************************************************************
 *                                                                      *
 *                      Memcached FIFO SSD subsystem                    *
 *                                                                      *
 ************************************************************************/

/*  Get container handles for all currently open containers.
 *  This is used by an application at initialization time to
 *  get the handles for all containers that were recovered from
 *  flash or statically defined in a property file.
 */
void mcd_osd_get_containers(struct ssdaio_ctxt *pctxt, mcd_container_t **containers, int *pn_containers)
{
    int   i;
    int   n_containers;
    // osd_state_t   *osd_state = (osd_state_t *) pctxt;

    n_containers = 0;
    for (i=0; i<MCD_MAX_NUM_CNTRS; i++) {
        if (Mcd_containers[i].tcp_port != 0) {
	    containers[n_containers] = &(Mcd_containers[i]);
	    n_containers++;
	}
    }

    *pn_containers = n_containers;
}

void mcd_osd_get_containers_cguids(struct ssdaio_ctxt *pctxt, SDF_cguid_t *cguids, uint32_t *n_cguids)
{
    int   i;
    int   n_containers;
    // osd_state_t   *osd_state = (osd_state_t *) pctxt;

    n_containers = 0;
    for (i=0; i<MCD_MAX_NUM_CNTRS; i++) {
        if (Mcd_containers[i].tcp_port != 0) {
            cguids[n_containers] = Mcd_containers[i].cguid;
            n_containers++;
        }
    }

    *n_cguids = n_containers;
}

static void
mcd_fth_get_container_properties( int index, char * cname,
                                  int system_recovery,
                                  mcd_cntr_props_t * cntr_props,
                                  SDF_container_props_t * props,
				  char *ctnr_name)
{
    int                         prop;
    int                         mynode = 0;
    int                         simple_rep = 0;
    char                        pname[MCD_CNTR_NAME_MAXLEN];
    char                      * ips;
    SDF_container_props_t       properties;
    mcd_container_t           * container;
    mcd_cntr_ips_t            * cntr_ips = cntr_props->cntr_ips;

    memset( &properties, 0, sizeof(SDF_container_props_t) );
    properties.container_type.type = SDF_OBJECT_CONTAINER;

    if ( 0 == flash_settings.static_containers &&
         SYS_FLASH_REFORMAT != system_recovery ) 
    {
        /*
         * get the container properties from recovered shard meta data
         */
        container = &Mcd_fth_saved_props[index];

        /*
         * FIXME_CNTR:
         * the following is needed for non-persistent containers,
         * should we also retain certain SDF properties as well?
         */
        properties.container_type.caching_container =
            container->eviction ? SDF_TRUE : SDF_FALSE;
        properties.container_type.persistence =
            container->persistent ? SDF_TRUE : SDF_FALSE;

        properties.cache.writethru = SDF_TRUE;         /* FIXME */
        properties.container_type.async_writes = SDF_FALSE;     /* FIXME */

        properties.container_id.size     = container->size_quota;
        properties.container_id.num_objs = container->obj_quota;
        properties.container_id.container_id = container->container_id;

        strncpy( cname, container->cname, 64 );
        cntr_props->sync_backup  = container->sync_backup;
        cntr_props->sync_updates = container->sync_updates;
        cntr_props->sync_msec    = container->sync_msec;
        cntr_props->cas_id       = container->cas_id;

        for ( int i = 0; i < container->num_ips; i++ ) {
            cntr_ips->ip_addrs[i] = container->ip_addrs[i];
        }
        cntr_ips->num_ips = container->num_ips;

        cntr_props->sasl = container->sasl;
        cntr_props->prefix_delete = container->prefix_delete;

        *props = properties;
        return;
    }

#ifdef  SIMPLE_REPLICATION
    // xxxzzz imported from simple_replication.c
    // TODO: get rid of this dependency
    extern int SDFSimpleReplication;
    simple_rep = SDFSimpleReplication;
    mynode = msg_sdf_myrank();
#endif

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].MASTER", mynode, index);
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].MASTER", index);
    }
    properties.master_vnode = getProperty_Int( pname, 0);

    /*
     * default is to allow eviction, command line option -M
     * overrides container[0] setting
     */
    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].EVICTION",
                 mynode, index );
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].EVICTION", index );
    }
    prop = getProperty_Int( pname, 1 );
    if ( 0 == index && 0 == flash_settings.evict_to_free ) {
        prop = 0;
    }
    properties.container_type.caching_container = prop ? SDF_TRUE : SDF_FALSE;
    properties.cache.writethru = SDF_TRUE;

    /*
     * default is non-persistent
     */
    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].PERSISTENT",
                 mynode, index);
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].PERSISTENT", index );
    }
    prop = getProperty_Int( pname, 0 );
    if ( 0 == index && 1 == flash_settings.sdf_persistence ) {
        prop = 1;
    }
    properties.container_type.persistence = prop ? SDF_TRUE : SDF_FALSE;

    /*
     * default is synchronous writes
     */
    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].ASYNC_WRITES",
                 mynode, index);
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].ASYNC_WRITES", index );
    }
    prop = getProperty_Int( pname, 0 );
    properties.container_type.async_writes = prop ? SDF_TRUE : SDF_FALSE;

    /*
     * for debugging and testing, allows -m to override container sizes
     *
     * if CAPACITY_MB is specified, it overrides CAPACITY_GB
     */
    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].CAPACITY_GB",
                 mynode, index);
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].CAPACITY_GB", index );
    }
    properties.container_id.size = getProperty_Int( pname, 0 );
    properties.container_id.size *= 1048576;

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].CAPACITY_MB",
                 mynode, index);
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].CAPACITY_MB", index );
    }
    properties.container_id.size =
        getProperty_Int( pname, properties.container_id.size / 1024 );
    properties.container_id.size *= 1024;

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].MAX_OBJECTS",
                 mynode, index);
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].MAX_OBJECTS", index );
    }
    properties.container_id.num_objs = getProperty_Int( pname, 0 );

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].CONTAINER_ID",
                 mynode, index);
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].CONTAINER_ID", index );
    }
    prop = getProperty_Int( pname, 0 );
    properties.container_id.container_id = prop;

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].CONTAINER_NAME",
                 mynode, index );
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].CONTAINER_NAME", index );
    }
    strncpy( cname, getProperty_String( pname, ctnr_name ), 64 );

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].SYNC_BACKUP",
                 mynode, index);
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].SYNC_BACKUP", index );
    }
    cntr_props->sync_backup = getProperty_Int( pname, 1 );

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].SYNC_UPDATES",
                 mynode, index);
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].SYNC_UPDATES", index );
    }
    cntr_props->sync_updates = getProperty_Int( pname, 0 );

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].SYNC_MSEC",
                 mynode, index);
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].SYNC_MSEC", index );
    }
    cntr_props->sync_msec = getProperty_Int( pname, 0 );

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].IP_ADDR",
                 mynode, index);
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].IP_ADDR", index );
    }
    ips = (char *)getProperty_String( pname, NULL );
    if ( NULL != ips ) {
        int                     len;
        char                  * ptr1;
        char                  * ptr2;
        struct in_addr          ip;
        bool                    ipaddr_any;

        mcd_log_msg( 50016, PLAT_LOG_LEVEL_DEBUG, "got ips: %s", ips );

        ipaddr_any = false;
        memset( (void *)cntr_ips, 0, sizeof(mcd_cntr_ips_t) );

        if ( NULL == ( ptr1 = plat_strdup( ips ) ) ) {
            mcd_log_msg( 50017, PLAT_LOG_LEVEL_FATAL,
                         "failed to dup container IP list" );
            plat_abort();
        }
        ips = ptr1;
        len = strlen( ips );
        do {
            if ( NULL != ( ptr2 = strchr( ptr1, ',' ) ) ) {
                *ptr2 = '\0';
            }
            if ( 0 == inet_aton( ptr1, &ip ) ) {
                mcd_log_msg( 50018, PLAT_LOG_LEVEL_FATAL,
                             "invalid ip_addr %s encounterd", ptr1 );
                plat_abort();
            }
            if ( 0 == ip.s_addr ) {
                ipaddr_any = true;
            }
            if ( MCD_CNTR_MAX_NUM_IPS == cntr_ips->num_ips ) {
                mcd_log_msg( 50019, PLAT_LOG_LEVEL_FATAL,
                             "too many IPs specified in the property file" );
                plat_abort();
            }
            cntr_ips->ip_addrs[cntr_ips->num_ips++] = ip;
            ptr1 = ptr2 + 1;
        } while ( ptr1 - ips < len && NULL != ptr2 );

        if ( true == ipaddr_any && 1 != cntr_ips->num_ips ) {
            mcd_log_msg( 50020, PLAT_LOG_LEVEL_FATAL,
                         "IPADDR_ANY cannot be specified with any other IPs" );
            plat_abort();
        }
        mcd_log_msg( 50021, PLAT_LOG_LEVEL_INFO,
                     "total number of ips is %d, ips=%s",
                     cntr_ips->num_ips, ips );

        plat_free( ips );
    }
    else {      /* no IP specified, default to IPADDR_ANY */
        cntr_ips->ip_addrs[0].s_addr = 0;
        cntr_ips->num_ips = 1;
    }

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].SASL",
                 mynode, index );
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].SASL", index );
    }
    cntr_props->sasl = getProperty_Int( pname, 0 ) ? true : false;

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].PREFIX_DELETE",
                 mynode, index );
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].PREFIX_DELETE", index );
    }
    cntr_props->prefix_delete = getProperty_Int( pname, 0 ) ? true : false;

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].WRITE_BACK",
                 mynode, index );
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].WRITE_BACK", index );
    }
    prop = getProperty_Int( pname, 0 );
    if ( 0 == prop ) {
        properties.cache.writethru = true;
    }
    else {
        properties.cache.writethru = false;
    }

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].FLUSH_TIME",
                 mynode, index );
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].FLUSH_TIME", index );
    }
    cntr_props->flush_time = getProperty_uLongInt( pname, 0 );

    if ( simple_rep ) {
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].CAS_ID",
                 mynode, index );
    } else {
        sprintf( pname, "MEMCACHED_CONTAINER[%d].CAS_ID", index );
    }
    cntr_props->cas_id = getProperty_uLongInt( pname, 0 );

    /*
     * Get the Group Type
     */
    if(  simple_rep ) {
        properties.replication.type = SDF_REPLICATION_NONE;
        sprintf( pname, "NODE[%d].GROUP_ID", mynode );
        prop = getProperty_Int( pname, -1 );
        if( -1 == prop ) {
            mcd_log_msg( 20118, PLAT_LOG_LEVEL_FATAL,
                         "Unable to find GrpId(NODE[%d].GROUP_ID) for node:%d",
                         mynode, mynode );
            plat_abort();
        }
        sprintf( pname, "SDF_CLUSTER_GROUP[%d].TYPE",prop );
        strncpy( pname, getProperty_String( pname, ctnr_name ), 64 );
        if( strcmp( pname, "MIRRORED" ) == 0 ) {
            properties.replication.type = SDF_REPLICATION_V1_2_WAY;
        }
        else if( strcmp( pname, "N+1" ) == 0 ) {
            properties.replication.type = SDF_REPLICATION_V1_N_PLUS_1;
        }
        /* get the number of replicas */
        sprintf( pname, "MEMCACHED_CONTAINER[%d][%d].NREPLICAS",
                 mynode, index );
        properties.replication.num_replicas = getProperty_Int( pname, 0 );
    }

    *props = properties;

    return;
}


SDF_status_t
mcd_fth_open_container( void * pai, SDF_context_t ctxt,
                        char * cname, SDF_container_props_t props,
                        SDF_CONTAINER * ctnr )
{
    SDF_status_t           status = SDF_FAILURE;
#ifdef SDFAPI
    SDF_cguid_t	cguid = SDF_NULL_CGUID;
#endif /* SDFAPI */

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

    status = SDF_I_NewContext( pai, &ctxt );
    plat_assert_always( SDF_SUCCESS == status );

    uint64_t old_ctxt = ((SDF_action_init_t *)pai)->ctxt;
    ((SDF_action_init_t *)pai)->ctxt = ctxt;
#ifdef SDFAPI
    if (SDF_SUCCESS != name_service_get_cguid(pai, cname, &cguid)) {
        status = SDF_CONTAINER_UNKNOWN;
    } else {
    	status = SDFOpenContainer( (SDF_internal_ctxt_t *)pai,
                               	    cguid,                   // container guid
                                    SDF_READ_WRITE_MODE);    // mode
    }
#else
    status = SDFOpenContainer( (SDF_internal_ctxt_t *)pai,
                               cname,                   // cntr path
                               SDF_READ_WRITE_MODE,     // mode
                               ctnr );
#endif /* SDFAPI */
    if ( SDF_SUCCESS != status ) {
        if ( SDF_CONTAINER_UNKNOWN != status ) {
            mcd_log_msg( 20086, PLAT_LOG_LEVEL_ERROR,
                         "failed to open container" );
        }
    }
    else {
        mcd_log_msg( 20072, PLAT_LOG_LEVEL_DEBUG, "container=%s opened", cname );
    }

    ((SDF_action_init_t *)pai)->ctxt = old_ctxt;

    SDF_I_Delete_Context( pai, ctxt );
    // plat_assert_always( SDF_SUCCESS == status );

    return status;
}


#ifdef SDFAPI
SDF_status_t
mcd_fth_create_open_container( void * pai, SDF_context_t ctxt,
                               char * cname, SDF_container_props_t *props,
                               SDF_cguid_t * cguid)  
#else
SDF_status_t
mcd_fth_create_open_container( void * pai, SDF_context_t ctxt,
                               char * cname, SDF_container_props_t *props,
                               SDF_CONTAINER * ctnr )
#endif /* SDFAPI */
{
    SDF_status_t        status = SDF_FAILURE;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

    status = SDF_I_NewContext( pai, &ctxt );
    plat_assert_always( SDF_SUCCESS == status );

    uint64_t old_ctxt = ((SDF_action_init_t *)pai)->ctxt;
    ((SDF_action_init_t *)pai)->ctxt = ctxt;
#ifdef SDFAPI
    status = SDFCreateContainer( (SDF_internal_ctxt_t *)pai,
                                 cname,                         // cntr path
                                 props,
				 cguid);
#else
    status = SDFCreateContainer( (SDF_internal_ctxt_t *)pai,
                                 cname,                         // cntr path
                                 props,
				 props.container_id.container_id );
#endif
    if ( SDF_SUCCESS != status ) {
        if ( SDF_CONTAINER_EXISTS != status ) {
            mcd_log_msg( 20087, PLAT_LOG_LEVEL_ERROR,
                         "failed to create container %s, status=%s",
                         cname, SDF_Status_Strings[status] );
        }
    }
    else {
#ifdef SDFAPI
        status = SDFOpenContainer( (SDF_internal_ctxt_t *)pai,
                                   *cguid,                      // cntr cguid
                                   SDF_READ_WRITE_MODE);        // mode
#else
        status = SDFOpenContainer( (SDF_internal_ctxt_t *)pai,
                                   cname,                       // cntr path
                                   SDF_READ_WRITE_MODE,         // mode
                                   ctnr );
#endif /* SDFAPI */
        if ( SDF_SUCCESS != status ) {
            mcd_log_msg( 20088, PLAT_LOG_LEVEL_ERROR,
                         "failed to open container %s, status=%s",
                         cname, SDF_Status_Strings[status] );

            SDF_status_t del_rc;
#ifdef SDFAPI
            del_rc = SDFDeleteContainer( (SDF_internal_ctxt_t *)pai, *cguid );
#else
            del_rc = SDFDeleteContainer( (SDF_internal_ctxt_t *)pai, cname );
#endif /* SDFAPI */
            if ( SDF_SUCCESS != del_rc ) {
                mcd_log_msg( 20089, PLAT_LOG_LEVEL_ERROR,
                             "failed to delete container %s, status=%s",
                             cname, SDF_Status_Strings[del_rc] );
            }
        }
        else {
            mcd_log_msg( 20090, PLAT_LOG_LEVEL_DEBUG, "container %s opened", cname );
        }
    }

    ((SDF_action_init_t *)pai)->ctxt = old_ctxt;

    SDF_I_Delete_Context( pai, ctxt );
    // plat_assert_always( SDF_SUCCESS == status );

    return status;
}

int mcd_fth_do_try_container( void * pai, mcd_container_t **ppcontainer, bool open_only,
                              int tcp_port, int udp_port,
                              SDF_container_props_t * prop, char * cntr_name,
                              mcd_cntr_props_t * cntr_props )
{
    return(mcd_fth_do_try_container_internal( pai, -1, ppcontainer, open_only, tcp_port, udp_port, prop, cntr_name, cntr_props));
}

static int mcd_fth_do_try_container_internal( void * pai, int index, 
                              mcd_container_t **ppcontainer, bool open_only,
                              int tcp_port, int udp_port,
                              SDF_container_props_t * prop, char * cntr_name,
                              mcd_cntr_props_t * cntr_props )
{
    int                         rc;
    SDF_status_t                status = SDF_FAILURE;
    SDF_CONTAINER               container = containerNull;
    SDF_container_props_t       properties = *prop;
    char                        cname[MCD_CNTR_NAME_MAXLEN];
    mcd_cntr_ips_t            * cntr_ips = cntr_props->cntr_ips;
    void *                      pfx_deleter = NULL;
#ifdef SDFAPI
    SDF_cguid_t                 cguid = SDF_NULL_CGUID;
#endif /* SDFAPI */

    if ( flash_settings.ips_per_cntr ) {
        strncpy( cname, cntr_name, MCD_CNTR_NAME_MAXLEN );
    }
    else {
        snprintf( cname, MCD_CNTR_NAME_MAXLEN, "%s%5d%.8x", cntr_name,
                  tcp_port, (int)properties.container_id.container_id );
    }

    /*
     * FIXME_PREFIX: no recovery for outstanding prefixes in the current
     * implementation
     */
    if ( cntr_props->prefix_delete ) {
        pfx_deleter = mcd_prefix_init( flash_settings.max_num_prefixes );
        if ( NULL == pfx_deleter ) {
            mcd_log_msg( 50027, PLAT_LOG_LEVEL_ERROR,
                         "failed to initialize prefix-based deleter for "
                         "container %s", cntr_name );
            return -1;
        }
    }

    if ( true == open_only ) {
        status = mcd_fth_open_container( pai,
                                         0,             // ctxt
                                         cname,
                                         properties,
                                         &container );
        if ( SDF_SUCCESS != status && SDF_CONTAINER_UNKNOWN != status ) {
            mcd_log_msg( 20088, PLAT_LOG_LEVEL_ERROR,
                         "failed to open container %s, status=%s",
                         cname, SDF_Status_Strings[status] );
            rc = -1;
            goto out;
        }
    }
    else {
#ifdef SDFAPI
        status = mcd_fth_create_open_container( pai,
                                                0,      // ctxt
                                                cname,
						prop,
						&cguid);
#else
        status = mcd_fth_create_open_container( pai,
                                                0,      // ctxt
                                                cname,
                                                properties,
                                                &container );
#endif /* SDFAPI */
        if ( SDF_SUCCESS != status && SDF_CONTAINER_EXISTS != status ) {
            mcd_log_msg( 20119, PLAT_LOG_LEVEL_ERROR,
                         "failed to create and open container %s, status=%s",
                         cname, SDF_Status_Strings[status] );
            rc = -1;
            goto out;
        }
    }

    if ( SDF_SUCCESS != status ) {
        rc = 1;
    }
    else {
#ifdef SDFAPI
	int map_index = get_ctnr_from_cname(cname);
	if (map_index >= 0) {
	    container = CtnrMap[map_index].sdf_container;
	}
#endif /* SDFAPI */
        local_SDF_CONTAINER lc = getLocalContainer(&lc, container);
        SDF_time_t time;

	/*  
	 *  Use the container structure specified by index if it is valid.
	 *  Otherwise, look for an unused structure.
	 */

        if (index == -1) {
	    for (index=0; index<MCD_MAX_NUM_CNTRS; index++) {
		if (Mcd_containers[index].tcp_port == 0) {
		    break;
		}
	    }
	    plat_assert(index < MCD_MAX_NUM_CNTRS);
	}

	*ppcontainer = &(Mcd_containers[index]);

	// Set Mcd index in container properties
	properties.mcd_index = index;

        Mcd_containers[index].state        = cntr_stopped;
        Mcd_containers[index].generation   =
            __sync_fetch_and_add( &Mcd_next_cntr_gen, 1 );

        /*
         * initialize the mcd_container structure
         */
        status = SDF_I_GetContainerInvalidationTime( pai, lc->cguid, &time );
        if ( SDF_SUCCESS != status ) {
            mcd_log_msg( 20120, PLAT_LOG_LEVEL_ERROR,
                         "failed to obtain invalidation time for container %s",
                         cname );
        }
        Mcd_containers[index].flush_time    = (time_t)time;

#ifdef  SIMPLE_REPLICATION
        Mcd_containers[index].cas_num_nodes = QREP_MAX_REPLICAS;
#else
        Mcd_containers[index].cas_num_nodes = SDF_REPLICATION_MAX_REPLICAS;
#endif
        Mcd_containers[index].cas_node_id   = Mcd_fth_node_id;
        Mcd_containers[index].cas_id        = cntr_props->cas_id;

        Mcd_containers[index].tcp_port      = tcp_port;
        Mcd_containers[index].udp_port      = udp_port;
        Mcd_containers[index].eviction      =
            properties.container_type.caching_container ? 1 : 0;
        Mcd_containers[index].persistent    =
            properties.container_type.persistence ? 1 : 0;
        Mcd_containers[index].container_id  =
            properties.container_id.container_id;

        strncpy( Mcd_containers[index].cname, cntr_name,
                 MCD_CNTR_NAME_MAXLEN );
        strncpy( Mcd_containers[index].cluster_name,
                 getProperty_String( "SDF_CLUSTER_NAME", "Schooner" ), 64 );

        Mcd_containers[index].sync_backup  = cntr_props->sync_backup;
        Mcd_containers[index].sync_updates = cntr_props->sync_updates;
        Mcd_containers[index].sync_msec    = cntr_props->sync_msec;
        Mcd_containers[index].size_quota   = properties.container_id.size;
        Mcd_containers[index].obj_quota    = properties.container_id.num_objs;

#ifdef SDFAPI
        Mcd_containers[index].cguid        = lc->cguid;
#else
        Mcd_containers[index].cguid        = cguid;
#endif /* SDAPI */
        Mcd_containers[index].sdf_container = internal_serverToClientContainer( container );

        Mcd_containers[index].shard = (void *)container_to_shard( pai, lc );
        if ( NULL == Mcd_containers[index].shard ) {
            mcd_log_msg( 20121, PLAT_LOG_LEVEL_ERROR,
                         "failed to obtain shard for container %s", cname );
        }

        if ( NULL != cntr_ips ) {
            Mcd_containers[index].num_ips = cntr_ips->num_ips;
            for ( int i = 0; i < MCD_CNTR_MAX_NUM_IPS; i++ ) {
                Mcd_containers[index].ip_addrs[i] = cntr_ips->ip_addrs[i];
            }
        }

        Mcd_containers[index].sasl = cntr_props->sasl;

        Mcd_containers[index].prefix_delete = cntr_props->prefix_delete;
        if ( Mcd_containers[index].prefix_delete ) {
            Mcd_containers[index].pfx_deleter = pfx_deleter;
        }

        /*
         * open container, part deux
         * 2-phase approach so that we can set up the reverse pointer
         * to mcd_container in shard
         */
        mcd_osd_shard_open_phase2( Mcd_containers[index].shard,
                                   &Mcd_containers[index] );

        mcd_log_msg( 20122, PLAT_LOG_LEVEL_INFO,
                     "container %s opened, id=%ld size=%lluKB num_objs=%d "
                     "eviction=%d persistence=%d",
                     cname, (long) Mcd_containers[index].sdf_container,
                     (unsigned long long)properties.container_id.size,
                     properties.container_id.num_objs,
                     properties.container_type.caching_container,
                     properties.container_type.persistence );
#ifdef SIMPLE_REPLICATION
        SDFRepDataStructAddContainer( pai, properties,lc->cguid );
#endif

        /*
         * FIXME_CNTR: flush_time is not included in SDF_container_props_t
         * so set it explicitly here if necessary.
         */
        if ( false == open_only && 0 != cntr_props->flush_time ) {
            rc = mcd_osd_shard_set_flush_time( Mcd_containers[index].shard,
                                               cntr_props->flush_time );
            if ( 0 != rc ) {
                mcd_log_msg( 50072, PLAT_LOG_LEVEL_ERROR,
                             "failed to set shard flush_time, rc=%d", rc );
                (void) (*flash_settings.delete_container_callback)( pai, &Mcd_containers[index] );
                return -1;
            }
            SDF_status_t sdf_rc =
                SDF_I_InvalidateContainerObjects( pai, lc->cguid,
                                                  (*(flash_settings.pcurrent_time)),
                                                  cntr_props->flush_time );
            if ( SDF_SUCCESS != sdf_rc ) {
                mcd_log_msg( 50073, PLAT_LOG_LEVEL_ERROR,
                             "failed to set sdf flush_time, rc=%d", rc );
                (void) (*flash_settings.delete_container_callback)( pai, &Mcd_containers[index] );
                return -1;
            }
        }

        return 0;       /* SUCCESS */
    }

out:
    if ( pfx_deleter ) {
        mcd_prefix_cleanup( pfx_deleter );
    }

    return rc;
}


static int mcd_fth_try_container( void * pai, int index, int system_recovery, int tcp_port, int udp_port, bool open_only, char *ctnr_name )
{
    int                         rc;
    int                         state;
    SDF_container_props_t       properties;
    char                        prop_cname[MCD_CNTR_NAME_MAXLEN];
    mcd_cntr_ips_t              cntr_ips;
    mcd_container_t           * mcd_cntr;
    mcd_container_t           * container = NULL;
    mcd_cntr_props_t            cntr_props;


    if ( 0 == tcp_port ) {
        return 0;
    }

    memset( (void *)&cntr_ips, 0, sizeof(mcd_cntr_ips_t) );
    memset( (void *)&cntr_props, 0, sizeof(cntr_props) );
    cntr_props.cntr_ips = &cntr_ips;
    mcd_fth_get_container_properties( index, prop_cname, 
                                      system_recovery, &cntr_props,
                                      &properties, ctnr_name );

    rc = mcd_fth_do_try_container_internal( pai, index, &container, open_only,
                                   tcp_port,
                                   udp_port,
                                   &properties, prop_cname,
                                   &cntr_props );
    if ( 0 == rc ) {

        if ( SYS_FLASH_REFORMAT == system_recovery ) {
            state = cntr_running;
        }
        else {
            state = -1;
            for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {

                mcd_cntr = &Mcd_fth_saved_props[i];
                if ( 0 == mcd_cntr->tcp_port ) {
                    continue;
                }
                mcd_log_msg( 20123, PLAT_LOG_LEVEL_DEBUG,
                             "container prop found, port=%d id=%d name=%s",
                             mcd_cntr->tcp_port, mcd_cntr->container_id,
                             mcd_cntr->cname );

                if ( mcd_cntr->tcp_port == container->tcp_port &&
                     mcd_cntr->container_id == container->container_id &&
                     0 == strcmp( mcd_cntr->cname, container->cname ) ) {
                    state = mcd_cntr->state;
                    mcd_log_msg( 20124, PLAT_LOG_LEVEL_DEBUG,
                                 "persisted container state %d", state );
                }
            }
            if ( -1 == state ) {
                /*
                 * FIXME: this should not happen once if dynamic container
                 * support is enabled
                 */
                mcd_log_msg( 20125, PLAT_LOG_LEVEL_WARN,
                             "recovered container state not found" );
                state = cntr_running;
            }
            container->prev_state = state;
        }

        if ( SDF_TRUE != flash_settings.is_node_independent &&
             0 != container->persistent &&
             SYS_FLASH_REFORMAT != system_recovery ) {
            /*
             * if replication is enabled, a persistent container
             * should always start with the stopped state (unless
             * "--reformat" is specified at the command line), and can
             * be activated through one of the following means:
             *
             *   1. this node is a recovering node and at the end of a
             *   successful recovery the replication module will set
             *   the container mode to match that of the surviving node
             *
             *   2. the user manually activates the container
             */
            if ( flash_settings.ips_per_cntr ) {
                rc = mcd_stop_container_byname_internal( pai,
                                                         container->cname );
            }
            else {
                rc = mcd_stop_container_internal( pai, container->tcp_port );
            }
            if ( 0 == rc ) {
                container->need_reinstate = 1;
            }
        }
        else {
            if ( cntr_running == state ) {
                if ( flash_settings.ips_per_cntr ) {
                    rc = mcd_start_container_byname_internal( pai,
                                                       container->cname );
                }
                else {
                    rc = mcd_start_container_internal( pai,
                                                       container->tcp_port );
                }
            }
            else if ( cntr_stopped == state ) {
                container->state = cntr_stopped;
            }
            else {
                mcd_log_msg( 20126, PLAT_LOG_LEVEL_FATAL,
                             "invalid recovered container state %d", state );
                plat_abort();
            }
        }

        if ( 0 == rc ) {
            rc = mcd_osd_shard_set_properties( container->shard, container );
            plat_assert_rc( rc );

            /*
             * FIXME: restore on-SSD state since we stopped the container
             * inexplicitly
             */
            if ( container->need_reinstate ) {
                rc = mcd_osd_shard_set_state( container->shard,
                                              container->prev_state );
                plat_assert_rc( rc );
            }
        }
    }
    else if ( -1 == rc ) {
        mcd_log_msg( 20127, PLAT_LOG_LEVEL_FATAL,
                     "container error during initialization, aborting..." );
        plat_abort();
    }

    return rc;
}

// from mcd_rec.c
extern int recovery_reclaim_space( void );

static SDF_status_t update_container_map(char *cname, SDF_cguid_t cguid) {
    int 	 i;
    SDF_status_t status = SDF_FAILURE;

    for (i=1; i<MCD_MAX_NUM_CNTRS; i++) {
        if (CtnrMap[i].cguid == 0) {
            // this is an unused map entry
            CtnrMap[i].cname = plat_alloc(strlen(cname)+1);
            if (CtnrMap[i].cname == NULL) {
                status = SDF_FAILURE_MEMORY_ALLOC;
		break;
            }
            strcpy(CtnrMap[i].cname, cname);
            CtnrMap[i].cguid         = cguid;
            CtnrMap[i].sdf_container = containerNull;
            status = SDF_SUCCESS;
            break;
        }
    }

    return(status);
}

SDF_status_t mcd_fth_container_init( void * pai, int system_recovery, int tcp_ports[MCD_MAX_NUM_CNTRS], int udp_ports[MCD_MAX_NUM_CNTRS], char *ctnr_name )
{
    int                         i;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

#ifdef  MCD_SDF_NO_OP
    return SDF_SUCCESS;
#endif

#ifdef  SIMPLE_REPLICATION
    /*
     * discover local node_id and number of nodes in the cluster,
     * needed by cas_id updating and should go before container
     * initialization
     */
    SDFClusterStatus( pai, &Mcd_fth_node_id, &Mcd_fth_num_nodes );
    mcd_log_msg( 20128, PLAT_LOG_LEVEL_INFO,
                 "got cluster status, node_id=%u num_nodes=%d",
                 Mcd_fth_node_id, Mcd_fth_num_nodes );
#endif

    /*
     * if MEMCACHED_STATIC_CONTAINERS is set, always read container
     * configurations from the properties file (for backward compatibility)
     */
    if ( 1 == getProperty_Int( "MEMCACHED_STATIC_CONTAINERS", 0 ) ) {
        flash_settings.static_containers = 1;
    }

    /*
     * FIXME: temporary workaround for static container support,
     * save container states since recovery_reclaim_space() will
     * remove all non-persistent containers
     */
    for ( i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
        /*
         * FIXME: skip the 0th entry which is for cmc
         */
        mcd_osd_shard_get_properties( i + 1, &Mcd_fth_saved_props[i] );
        if ( 0 == flash_settings.static_containers &&
             SYS_FLASH_REFORMAT != system_recovery ) {
	    if ( Mcd_fth_saved_props[i].tcp_port != 0 ) {
		char *cname = Mcd_fth_saved_props[i].cname;
                SDF_cguid_t cguid;
		if (SDF_SUCCESS == name_service_get_cguid(pai, cname, &cguid)) {
		    update_container_map(cname, cguid);
		}
	    }
            tcp_ports[i] = Mcd_fth_saved_props[i].tcp_port;
            udp_ports[i] = Mcd_fth_saved_props[i].udp_port;
        }
    }

    /*
     * first try opening all the container
     */
    for ( i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
        mcd_fth_try_container( pai, i, system_recovery, tcp_ports[i], udp_ports[i], true, ctnr_name );
    }

#ifdef MCD_ENABLE_PERSISTENCE
    /*
     * reclaim space left by deleted containers
     */
    int rc = recovery_reclaim_space();
    plat_assert_rc( rc );
#endif

    /*
     * now try create and open
     */
    for ( i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
        if ( 0 != Mcd_containers[i].sdf_container ) {
            continue;
        }
        mcd_fth_try_container( pai, i, system_recovery, tcp_ports[i], udp_ports[i], false, ctnr_name );
    }

    return SDF_SUCCESS;
}


static int
mcd_osd_fifo_shard_init( mcd_osd_shard_t * shard, uint64_t shard_id,
                         int flags, uint64_t quota, unsigned max_nobjs )
{
    int                         i;
    uint64_t                    total_alloc = 0;
    char                      * wbuf = NULL;
    char                      * wbuf_base;

    mcd_log_msg( 20293, PLAT_LOG_LEVEL_INFO,
                 "ENTERING, shard=%p shard_id=%lu flags=0x%x quota=%lu "
                 "max_nobjs=%u",
                 shard, shard_id, flags, quota, max_nobjs );

    shard->id = shard_id;

    if ( FLASH_SHARD_INIT_PERSISTENCE_YES ==
         (flags & FLASH_SHARD_INIT_PERSISTENCE_MASK) ) {
        shard->persistent = 1;
    }

    if ( FLASH_SHARD_INIT_EVICTION_CACHE ==
         (flags & FLASH_SHARD_INIT_EVICTION_MASK) ) {
        shard->evict_to_free = 1;
    }

    if ( FLASH_SHARD_SEQUENCE_EXTERNAL ==
         (flags & FLASH_SHARD_SEQUENCE_MASK) ) {
        shard->replicated = 1;
    }

    shard->use_fifo = 1;

    /*
     * shard total data size should align with total wbuf size
     */
    while ( 0 != ( ( shard->total_segments * Mcd_osd_segment_size ) %
                   ( MCD_OSD_WBUF_SIZE * MCD_OSD_NUM_WBUFS ) ) ) {
        shard->total_segments--;
    }
    shard->total_size = shard->total_segments * Mcd_osd_segment_size;
    shard->total_blks = shard->total_segments * Mcd_osd_segment_blks;

    if ( 4294967296ULL < shard->total_blks ) {
        mcd_log_msg( 20294, PLAT_LOG_LEVEL_ERROR,
                     "shard size greater than 2TB, not supported yet" );
        return FLASH_EINVAL;
    }

    /*
     * initialize the address remapping table
     */
    shard->rand_table = (uint32_t *)plat_alloc(
        (shard->total_blks / Mcd_osd_rand_blksize) * sizeof(uint32_t) );
    if ( NULL == shard->rand_table ) {
        mcd_log_msg( 20295, PLAT_LOG_LEVEL_ERROR, "failed to allocate random table" );
        return FLASH_ENOMEM;
    }
    total_alloc +=
        (shard->total_blks / Mcd_osd_rand_blksize) * sizeof(uint32_t);

    /*
     * exclude metadata segments here since recovery code makes them
     * transparent
     */
    int temp = Mcd_osd_segment_blks / Mcd_osd_rand_blksize;
    uint32_t * tmp_segments = shard->segments +
        shard->data_blk_offset / Mcd_osd_segment_blks;
    for ( i = 0; i < shard->total_blks / Mcd_osd_rand_blksize; i++ ) {
        if ( 0 == flash_settings.mq_ssd_balance ) {
            shard->rand_table[i] = tmp_segments[i / temp]
                + (i % temp) * Mcd_osd_rand_blksize;
        }
        else {
            shard->rand_table[i] = tmp_segments[i / temp]
                + ( (i + i / temp) % temp ) * Mcd_osd_rand_blksize;
            mcd_log_msg( 20296, PLAT_LOG_LEVEL_INFO,
                         "rand_table[%d]=%u, segments[%d]=%u",
                         i, shard->rand_table[i],
                         i / temp, tmp_segments[i / temp] );
        }
    }
#ifdef MCD_DISABLED
    uint32_t temp;
    for ( i = 0; i < shard->total_blks / Mcd_osd_rand_blksize; i++ ) {
        j = (uint32_t) ( (shard->total_blks / Mcd_osd_rand_blksize) *
                         (rand() / (RAND_MAX + 1.0) ) );
        temp = shard->rand_table[i];
        shard->rand_table[i] = shard->rand_table[j];
        shard->rand_table[j] = temp;
    }
#endif
    mcd_log_msg( 20297, PLAT_LOG_LEVEL_INFO,
                 "remapping table initialized, size=%lu",
                 (shard->total_blks / Mcd_osd_rand_blksize) *
                 sizeof(uint32_t) );

    /*
     * hash table will be allocated later
     */
    shard->hash_size = shard->total_size / Mcd_osd_blk_size;
    if ( 0 < max_nobjs && max_nobjs < shard->hash_size ) {
        shard->hash_size = ( max_nobjs + Mcd_osd_segment_blks - 1 )
            / Mcd_osd_segment_blks * Mcd_osd_segment_blks;
    }

    /*
     * allocate the bucket table
     */
    shard->hash_buckets = (mcd_osd_bucket_t *)plat_alloc_large(
        shard->hash_size / Mcd_osd_bucket_size * sizeof(mcd_osd_bucket_t) );
    if ( NULL == shard->hash_buckets ) {
        mcd_log_msg( 20298, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocate hash buckets" );
        return FLASH_ENOMEM;
    }

    memset( (void *)shard->hash_buckets, 0,
        shard->hash_size / Mcd_osd_bucket_size * sizeof(mcd_osd_bucket_t) );
    total_alloc +=
        shard->hash_size / Mcd_osd_bucket_size * sizeof(mcd_osd_bucket_t);
    mcd_log_msg( 20299, PLAT_LOG_LEVEL_INFO,
                 "hash buckets allocated, size=%lu",
                 shard->hash_size /
                 Mcd_osd_bucket_size * sizeof(mcd_osd_bucket_t) );

    /*
     * initialize the lock buckets
     */
    shard->lock_bktsize = MCD_OSD_LOCKBKT_MINSIZE;
    shard->lock_buckets = shard->hash_size / shard->lock_bktsize;
    while ( MCD_OSD_LOCK_BUCKETS < shard->lock_buckets ) {
        shard->lock_bktsize *= 2;
        shard->lock_buckets /= 2;
    }
    while ( 32768 > shard->lock_buckets ) {
        shard->lock_bktsize /= 2;
        shard->lock_buckets *= 2;
    }

    shard->bucket_locks = (fthLock_t *)
        plat_alloc_large( shard->lock_buckets * sizeof(fthLock_t) );
    if ( NULL == shard->bucket_locks ) {
        mcd_log_msg( 20099, PLAT_LOG_LEVEL_ERROR,
                     "failed to alloc bucket locks" );
        return FLASH_ENOMEM;
    }
    total_alloc += shard->lock_buckets * sizeof(fthLock_t);

    for ( i = 0; i < shard->lock_buckets; i++ ) {
        fthLockInit( &shard->bucket_locks[i] );
    }
    mcd_log_msg( 20300, PLAT_LOG_LEVEL_INFO,
                 "lock_buckets=%lu lock_bktsize=%d, total lock size=%lu",
                 shard->lock_buckets, shard->lock_bktsize,
                 shard->lock_buckets * sizeof(fthLock_t) );

    /*
     * initialize hash table
     */
    shard->hash_table = (mcd_osd_hash_t *)
        plat_alloc_large( shard->hash_size * sizeof(mcd_osd_hash_t) );
    if ( NULL == shard->hash_table ) {
        mcd_log_msg( 20301, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocate hash table" );
        return FLASH_ENOMEM;
    }
    plat_assert_always(
        0 == ( (uint64_t)shard->hash_table % sizeof(mcd_osd_hash_t) ) );

    memset( (void *)shard->hash_table, 0,
            shard->hash_size * sizeof(mcd_osd_hash_t) );
    total_alloc += shard->hash_size * sizeof(mcd_osd_hash_t);
    mcd_log_msg( 20302, PLAT_LOG_LEVEL_INFO,
                 "hash table initialized, size=%lu",
                 shard->hash_size * sizeof(mcd_osd_hash_t) );

    /*
     * initialize the address lookup table
     */
    shard->addr_table = (uint32_t *)
        plat_alloc_large( shard->total_blks * sizeof(uint32_t) );
    if ( NULL == shard->addr_table ) {
        mcd_log_msg( 20301, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocate hash table" );
        return FLASH_ENOMEM;
    }
    memset( (void *)shard->addr_table, 0,
            shard->total_blks * sizeof(uint32_t) );
    total_alloc += shard->total_blks * sizeof(uint32_t);
    mcd_log_msg( 20307, PLAT_LOG_LEVEL_INFO,
                 "address table initialized, size=%lu",
                 shard->total_blks * sizeof(uint32_t) );

    /*
     * initialize FIFO related shard data structures
     */
    wbuf = plat_alloc( MCD_OSD_WBUF_SIZE * MCD_OSD_NUM_WBUFS + 4095 );
    if ( NULL == wbuf ) {
        mcd_log_msg( 20308, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocate write buffer" );
        return FLASH_ENOMEM;
    }
    memset( wbuf, 0, MCD_OSD_WBUF_SIZE * MCD_OSD_NUM_WBUFS + 4095 );
    wbuf_base = (char *)( ( (uint64_t)wbuf + 4095 ) & 0xfffffffffffff000ULL );
    total_alloc += MCD_OSD_WBUF_SIZE * MCD_OSD_NUM_WBUFS + 4095;

    for ( int i = 0; i < MCD_OSD_NUM_WBUFS; i++ ) {
        shard->fifo.wbufs[i].id = i;
        shard->fifo.wbufs[i].ref_count = 0;
        shard->fifo.wbufs[i].filled = 0;
        shard->fifo.wbufs[i].blk_offset = i * MCD_OSD_WBUF_BLKS;
        shard->fifo.wbufs[i].buf = wbuf_base + i * MCD_OSD_WBUF_SIZE;
        shard->fifo.wbufs[i].items = 0;
        shard->fifo.wbufs[i].meta = NULL;
    }
    shard->fifo.wbufs[0].filled = MCD_OSD_MIN_OFFSET;

    shard->fifo.wbuf_base = wbuf;
    shard->fifo.fth_count = 0;
    shard->fifo.pending_wmails = 0;
    shard->fifo.free_blks = shard->total_blks;
    fthMboxInit( &shard->fifo.sleeper_mbox );

    mcd_log_msg( 20309, PLAT_LOG_LEVEL_INFO,
                 "shard initialized, total allocated bytes=%lu", total_alloc );
    return 0;   /* SUCCESS */
}


static int mcd_osd_fifo_reclaim( mcd_osd_shard_t * shard, char * rbuf,
                                 uint64_t blk_offset )
{
    int                         i;
    int                         total;
    int                         blocks;
    uint64_t                    syndrome;
    fthLock_t                 * bkt_lock;
    fthWaitEl_t               * wait;
    mcd_osd_meta_t            * meta;
    mcd_osd_hash_t            * hash_entry;
    mcd_osd_hash_t            * bucket_head;
    mcd_osd_bucket_t          * bucket;

    meta = (mcd_osd_meta_t *)rbuf;
    while ( 0 == meta->magic ) {
        meta = (mcd_osd_meta_t *)((char *)meta + Mcd_osd_blk_size);
        if ( rbuf + MCD_OSD_WBUF_SIZE <= (char *)meta ) {
            mcd_log_msg( 20310, PLAT_LOG_LEVEL_WARN, "nothing to reclaim" );
            return 0;
        }
    }

    total = 0;
    do {
        if ( MCD_OSD_MAGIC_NUMBER != meta->magic ) {
            mcd_log_msg( 20311, PLAT_LOG_LEVEL_ERROR,
                         "not enough magic, blk_offset=%lu",
                         ( (char *)meta - rbuf ) / Mcd_osd_blk_size +
                         blk_offset );
            plat_abort();
        }
        mcd_log_msg( 20312, PLAT_LOG_LEVEL_DEBUG, "key_len=%d data_len=%u",
                     meta->key_len, meta->data_len );

        syndrome = hash( (unsigned char *)meta + sizeof(mcd_osd_meta_t),
                         meta->key_len, 0 );

        bkt_lock = shard->bucket_locks +
            ( syndrome % shard->hash_size ) / shard->lock_bktsize;

        bucket = shard->hash_buckets +
            ( ( syndrome % shard->hash_size ) / Mcd_osd_bucket_size );
        bucket_head = shard->hash_table +
            ( ( syndrome % shard->hash_size ) & Mcd_osd_bucket_mask );
        hash_entry = bucket_head;

        wait = fthLock( bkt_lock, 1, NULL );

        for ( i = 0; i < bucket->next_item; i++,hash_entry++ ) {
            if ( 0 == hash_entry->used ) {
                plat_assert_always( 0 == 1 );
            }
            if ( blk_offset + ( (char *)meta - rbuf ) / Mcd_osd_blk_size
                 == hash_entry->address ) {
                mcd_log_msg( 20313, PLAT_LOG_LEVEL_DEBUG,
                             "reclaiming item: syndrome=%lx "
                             "syn=%x addr=%u blocks=%u",
                             syndrome, hash_entry->syndrome,
                             hash_entry->address, hash_entry->blocks );
                total += mcd_osd_lba_to_blk( hash_entry->blocks );
                if ( 0 == bucket->next_item ) {
                    plat_assert_always( 0 == 1 );
                }
                if ( bucket_head + bucket->next_item - 1 != hash_entry ) {
                    *hash_entry = bucket_head[--bucket->next_item];
                    *((uint64_t *)&bucket_head[bucket->next_item]) = 0;
                }
                else {
                    *((uint64_t *)hash_entry) = 0;
                    bucket->next_item--;
                }
                (void) __sync_fetch_and_sub( &shard->num_objects, 1 );
                (void) __sync_fetch_and_add( &shard->num_slab_evictions, 1 );
                break;
            }
        }
        fthUnlock( wait );

        blocks = ( sizeof(mcd_osd_meta_t) + meta->key_len + meta->data_len
                   + ( Mcd_osd_blk_size - 1 ) ) / Mcd_osd_blk_size;
        meta = (mcd_osd_meta_t *)((char *)meta + blocks * Mcd_osd_blk_size);

        if ( 0 == meta->magic ) {
            break;
        }
    } while ( rbuf + MCD_OSD_WBUF_SIZE > (char *)meta );

    (void) __sync_fetch_and_sub( &shard->blk_consumed, total );

    mcd_log_msg( 20314, MCD_OSD_LOG_LVL_INFO,
                 "space reclaimed, blk_offset=%lu blocks=%d",
                 blk_offset, total );

    return 0;   /* SUCCESS */
}


static int mcd_osd_fifo_write( mcd_osd_shard_t * shard, char * rbuf,
                               osd_state_t * osd_state )
{
    int                         rc;
    int                         count;
    int                         cushion;
    int                         total;
    uint64_t                    tmp_offset;
    uint64_t                    blk_offset;
    uint64_t                    offset;
    osd_state_t               * context;
    mcd_osd_fifo_wbuf_t       * wbuf;

    mcd_log_msg( 20316, MCD_OSD_LOG_LVL_INFO,
                 "got writer mail, blk_committed=%lu",
                 shard->fifo.blk_committed );

    cushion = shard->total_size < 1073741824 ? 1 : 2;
    wbuf =
        &shard->fifo.wbufs[ (shard->fifo.blk_committed / MCD_OSD_WBUF_BLKS)
                            % MCD_OSD_NUM_WBUFS ];

    count = 0;
    do {
        if ( shard->fifo.blk_committed != wbuf->blk_offset ) {
            mcd_log_msg( 20317, PLAT_LOG_LEVEL_FATAL,
                         "this should not happen" );
            plat_abort();
        }

        if ( MCD_OSD_WBUF_BLKS != wbuf->filled ) {
            mcd_log_msg( 20318, MCD_OSD_LOG_LVL_INFO,
                         "writer mail arrived out of order" );
            break;
        }

        blk_offset = wbuf->blk_offset % shard->total_blks;
        tmp_offset = shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
            + ( blk_offset % Mcd_osd_rand_blksize );
        offset = tmp_offset * Mcd_osd_blk_size;

        rc = mcd_fth_aio_blk_write( (void *) &osd_state->osd_aio_state,
                                    wbuf->buf,
                                    offset,
                                    MCD_OSD_WBUF_SIZE );
        if ( FLASH_EOK != rc ) {
            mcd_log_msg( 20319, PLAT_LOG_LEVEL_FATAL,
                         "failed to commit buffer, rc=%d", rc );
            plat_abort();
        }

        (void) __sync_fetch_and_add(
            &shard->fifo.blk_committed, MCD_OSD_WBUF_BLKS );

        /*
         * check whether there are still outstanding readers
         */
        while ( 0 != wbuf->ref_count ) {
            mcd_log_msg( 20320, MCD_OSD_LOG_LVL_INFO,
                         "buffer still referenced, count=%u",
                         wbuf->ref_count );
            fthYield( 1 );
        }

        wbuf->filled = 0;
        wbuf->items  = 0;

        (void) __sync_fetch_and_add(
            &wbuf->blk_offset, MCD_OSD_WBUF_BLKS * MCD_OSD_NUM_WBUFS );

        (void) __sync_fetch_and_add(
            &shard->fifo.blk_nextbuf, MCD_OSD_WBUF_BLKS );

        mcd_log_msg( 20321, MCD_OSD_LOG_LVL_INFO,
                     "wbuf %d committed, "
                     "rsvd=%lu alloc=%lu cmtd=%lu next=%lu", wbuf->id,
                     shard->fifo.blk_reserved, shard->fifo.blk_allocated,
                     shard->fifo.blk_committed, shard->fifo.blk_nextbuf );

        /*
         * check for available "free" space
         */
        shard->fifo.free_blks -= MCD_OSD_WBUF_BLKS;

        if ( ( MCD_OSD_NUM_WBUFS * MCD_OSD_WBUF_BLKS ) * cushion
             >= shard->fifo.free_blks ) {

            blk_offset = ( shard->fifo.blk_committed + cushion *
                           ( MCD_OSD_NUM_WBUFS * MCD_OSD_WBUF_BLKS ) )
                % shard->total_blks;

            tmp_offset =
                shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
                + (blk_offset % Mcd_osd_rand_blksize);
            offset = tmp_offset * Mcd_osd_blk_size;

            mcd_log_msg( 20322, MCD_OSD_LOG_LVL_INFO,
                         "rbuf to read, blk_offset=%lu", blk_offset );

            rc = mcd_fth_aio_blk_read( (void *) &osd_state->osd_aio_state,
                                       rbuf,
                                       offset,
                                       MCD_OSD_WBUF_SIZE );
            if ( FLASH_EOK != rc ) {
                mcd_log_msg( 20323, PLAT_LOG_LEVEL_FATAL,
                             "failed to read data, rc=%d", rc );
                plat_abort();
            }

            mcd_osd_fifo_reclaim( shard, rbuf, blk_offset );

            shard->fifo.free_blks += MCD_OSD_WBUF_BLKS;
        }

        /*
         * wake up some sleepers
         */
        wbuf = &shard->fifo.wbufs[
            (shard->fifo.blk_committed / MCD_OSD_WBUF_BLKS) %
            MCD_OSD_NUM_WBUFS ];

        total = 0;
        while ( 1 ) {

            context =
                (osd_state_t *)fthMboxTry( &shard->fifo.sleeper_mbox );
            if ( NULL == context ) {
                break;
            }

            fthMboxPost( (fthMbox_t *)context->osd_mbox, 0 );
            total += context->osd_blocks;
            mcd_log_msg( 20324, MCD_OSD_LOG_LVL_DEBUG,
                         "sleeper waked up, blocks=%d, total=%d",
                         context->osd_blocks, total );

            if ( 1 < shard->fifo.fth_count ||
                 MCD_OSD_WBUF_BLKS == wbuf->filled ) {
                if ( MCD_OSD_WBUF_BLKS < total ) {
                    break;
                }
            }
            else {
                if ( MCD_OSD_WBUF_BLKS * 2 < total ) {
                    break;
                }
            }
        }

        if ( MCD_OSD_WBUF_BLKS != wbuf->filled ) {
            break;
        }

        if ( MCD_OSD_NUM_WBUFS == ++count ) {
            /*
             * time to check the mailbox to avoid starving others
             * FIXME: move to the outside loop or replace with fth_yield()?
             */
            return -EAGAIN;
        }
    } while ( 1 );

    return 0;   /* SUCCESS */
}

void mcd_fth_start_osd_writers()
{
    int            i;
    int            num_writers;
    osd_state_t   *osd_state;

    /*
     * start the writer fthread
     */
    num_writers = 1;
    if ( flash_settings.multi_fifo_writers ) {
        num_writers = flash_settings.num_sched;
    }

    for ( i = 0; i < num_writers; i++ ) {
	osd_state = mcd_fth_init_aio_ctxt(SSD_AIO_CTXT_MCD_WRITER);

        fthResume( fthSpawn( &mcd_fth_osd_writer, 81920),
                   (uint64_t) osd_state );
        mcd_log_msg( 150002, PLAT_LOG_LEVEL_INFO,
                     "writer fthread spawned, use context %p", osd_state );
    }
}


void mcd_fth_osd_writer( uint64_t arg )
{
    int                         rc;
    int                         count;
    int                         cushion;
    uint64_t                    mail;
    osd_state_t               * osd_state = (osd_state_t *)arg;
    int                         total;
    uint64_t                    tmp_offset;
    uint64_t                    blk_offset;
    uint64_t                    offset;
    osd_state_t               * context;
    fthMbox_t                 * mbox = &Mcd_osd_writer_mbox;
    char                      * rbuf;
    mcd_osd_shard_t           * shard;
    mcd_osd_fifo_wbuf_t       * wbuf;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_INFO, "ENTERING" );

    rbuf = plat_alloc( MCD_OSD_WBUF_SIZE + 4095 );
    if ( NULL == rbuf ) {
        mcd_log_msg( 20315, PLAT_LOG_LEVEL_FATAL,
                     "failed to allocate read buffer" );
        plat_abort();
    }
    rbuf = (char *)( ((uint64_t)rbuf + 4095) & 0xfffffffffffff000ULL );

    while ( 1 ) {

        mail = fthMboxWait( mbox );
        if ( (uint64_t)&Mcd_fth_admin_mbox == mail ) {
            /*
             * ok the shard is being stopped
             */
            fthMboxPost( &Mcd_fth_admin_mbox, cntr_stopping );
            continue;
        }
        else {
            shard = (mcd_osd_shard_t *)mail;
        }

        if ( flash_settings.multi_fifo_writers ) {
            do {
                rc = mcd_osd_fifo_write( shard, rbuf, osd_state );
                if ( -EAGAIN == rc ) {
                    fthMboxPost( &Mcd_osd_writer_mbox, (uint64_t)shard );
                    mcd_log_msg( 50041, PLAT_LOG_LEVEL_DEBUG,
                                 "writer resume mail posted, shard=%p",
                                 shard );
                    break;
                }
            } while ( __sync_fetch_and_sub( &shard->fifo.fth_count, 1 ) > 1 );
            continue;
        }

        (void) __sync_fetch_and_sub( &shard->fifo.pending_wmails, 1 );
        mcd_log_msg( 20316, MCD_OSD_LOG_LVL_INFO,
                     "got writer mail, blk_committed=%lu",
                     shard->fifo.blk_committed );

        cushion = shard->total_size < 1073741824 ? 1 : 2;
        wbuf =
            &shard->fifo.wbufs[ (shard->fifo.blk_committed / MCD_OSD_WBUF_BLKS)
                                % MCD_OSD_NUM_WBUFS ];

        count = 0;
        do {
            if ( shard->fifo.blk_committed != wbuf->blk_offset ) {
                mcd_log_msg( 20317, PLAT_LOG_LEVEL_FATAL,
                             "this should not happen" );
                plat_abort();
            }

            if ( MCD_OSD_WBUF_BLKS != wbuf->filled ) {
                mcd_log_msg( 20318, MCD_OSD_LOG_LVL_INFO,
                             "writer mail arrived out of order" );
                break;
            }

            blk_offset = wbuf->blk_offset % shard->total_blks;
            tmp_offset = shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
                + ( blk_offset % Mcd_osd_rand_blksize );
            offset = tmp_offset * Mcd_osd_blk_size;

            rc = mcd_fth_aio_blk_write( (void *) &osd_state->osd_aio_state,
                                        wbuf->buf,
                                        offset,
                                        MCD_OSD_WBUF_SIZE );

            if ( FLASH_EOK != rc ) {
                mcd_log_msg( 20319, PLAT_LOG_LEVEL_FATAL,
                             "failed to commit buffer, rc=%d", rc );
                plat_abort();
            }

            (void) __sync_fetch_and_add(
                &shard->fifo.blk_committed, MCD_OSD_WBUF_BLKS );

            /*
             * check whether there are still outstanding readers
             */
            while ( 0 != wbuf->ref_count ) {
                mcd_log_msg( 20320, MCD_OSD_LOG_LVL_INFO,
                             "buffer still referenced, count=%u",
                             wbuf->ref_count );
                fthYield( 1 );
            }

            wbuf->filled = 0;
            wbuf->items  = 0;

            (void) __sync_fetch_and_add(
                &wbuf->blk_offset, MCD_OSD_WBUF_BLKS * MCD_OSD_NUM_WBUFS );

            (void) __sync_fetch_and_add(
                &shard->fifo.blk_nextbuf, MCD_OSD_WBUF_BLKS );

            mcd_log_msg( 20321, MCD_OSD_LOG_LVL_INFO,
                         "wbuf %d committed, "
                         "rsvd=%lu alloc=%lu cmtd=%lu next=%lu", wbuf->id,
                         shard->fifo.blk_reserved, shard->fifo.blk_allocated,
                         shard->fifo.blk_committed, shard->fifo.blk_nextbuf );

            /*
             * check for available "free" space
             */
            shard->fifo.free_blks -= MCD_OSD_WBUF_BLKS;

            if ( ( MCD_OSD_NUM_WBUFS * MCD_OSD_WBUF_BLKS ) * cushion
                 >= shard->fifo.free_blks ) {

                blk_offset = ( shard->fifo.blk_committed + cushion *
                               ( MCD_OSD_NUM_WBUFS * MCD_OSD_WBUF_BLKS ) )
                    % shard->total_blks;

                tmp_offset =
                    shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
                    + (blk_offset % Mcd_osd_rand_blksize);
                offset = tmp_offset * Mcd_osd_blk_size;

                mcd_log_msg( 20322, MCD_OSD_LOG_LVL_INFO,
                             "rbuf to read, blk_offset=%lu", blk_offset );

                rc = mcd_fth_aio_blk_read( (void *) &osd_state->osd_aio_state,
                                           rbuf,
                                           offset,
                                           MCD_OSD_WBUF_SIZE );
                if ( FLASH_EOK != rc ) {
                    mcd_log_msg( 20323, PLAT_LOG_LEVEL_FATAL,
                                 "failed to read data, rc=%d", rc );
                    plat_abort();
                }

                mcd_osd_fifo_reclaim( shard, rbuf, blk_offset );

                shard->fifo.free_blks += MCD_OSD_WBUF_BLKS;
            }

            /*
             * wake up some sleepers
             */
            total = 0;
            while ( 1 ) {

                context =
                    (osd_state_t *)fthMboxTry( &Mcd_osd_sleeper_mbox );
                if ( NULL == context ) {
                    break;
                }

                fthMboxPost( (fthMbox_t *)context->osd_mbox, 0 );

                total += context->osd_blocks;
                mcd_log_msg( 20324, MCD_OSD_LOG_LVL_DEBUG,
                             "sleeper waked up, blocks=%d, total=%d",
                             context->osd_blocks, total );

                if ( MCD_OSD_WBUF_BLKS < total ) {
                    break;
                }
            }

            if ( MCD_OSD_NUM_WBUFS == ++count ) {
                /*
                 * time to check the mailbox to avoid starving others
                 */
                break;
            }

            wbuf = &shard->fifo.wbufs[
                (shard->fifo.blk_committed / MCD_OSD_WBUF_BLKS) %
                MCD_OSD_NUM_WBUFS ];
            if ( MCD_OSD_WBUF_BLKS != wbuf->filled ) {
                break;
            }
        } while ( 1 );
    }

    return;
}


inline char *
mcd_fifo_find_buffered_obj( mcd_osd_shard_t * shard, char * key, int key_len,
                            uint64_t address, mcd_osd_fifo_wbuf_t ** wbufp,
                            int * committed )
{
    uint32_t                    blk_committed;
    char                      * buf;
    mcd_osd_meta_t            * meta;
    mcd_osd_fifo_wbuf_t       * wbuf;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

retry:
    blk_committed = shard->fifo.blk_committed % shard->total_blks;

    if ( ( address >= blk_committed &&
           MCD_OSD_NUM_WBUFS * MCD_OSD_WBUF_BLKS >
           ( address - blk_committed ) ) ||
         ( address < blk_committed &&
           MCD_OSD_NUM_WBUFS * MCD_OSD_WBUF_BLKS >
           ( shard->total_blks - blk_committed ) &&
           ( blk_committed + MCD_OSD_NUM_WBUFS * MCD_OSD_WBUF_BLKS )
           % shard->total_blks > address ) ) {

        wbuf = &shard->fifo.wbufs[ ( address / MCD_OSD_WBUF_BLKS )
                                   % MCD_OSD_NUM_WBUFS];

        (void) __sync_fetch_and_add( &wbuf->ref_count, 1 );

        if ( blk_committed != shard->fifo.blk_committed % shard->total_blks ) {
            (void) __sync_fetch_and_sub( &wbuf->ref_count, 1 );
            goto retry;
        }
        *committed = 0;

        buf = wbuf->buf + (address % MCD_OSD_WBUF_BLKS) * Mcd_osd_blk_size;

        meta = (mcd_osd_meta_t *)buf;
        mcd_log_msg( 20312, PLAT_LOG_LEVEL_DEBUG, "key_len=%d data_len=%u",
                    meta->key_len, meta->data_len );

        if ( MCD_OSD_MAGIC_NUMBER != meta->magic ) {
            mcd_log_msg( 20325, PLAT_LOG_LEVEL_FATAL, "not enough magic!" );
            plat_abort();
        }

        if ( key && key_len != meta->key_len ) {
            mcd_log_msg( 20326, MCD_OSD_LOG_LVL_DIAG,
                         "key length mismatch, req %d osd %d",
                         key_len, meta->key_len );
            (void) __sync_fetch_and_add( &shard->get_hash_collisions, 1 );
            (void) __sync_fetch_and_sub( &wbuf->ref_count, 1 );
            return NULL;
        }

        if ( key &&
             0 != strncmp( buf + sizeof(mcd_osd_meta_t), key, key_len ) ) {
            mcd_log_msg( 20006, MCD_OSD_LOG_LVL_DIAG,
                         "key mismatch, req %s", key );
            (void) __sync_fetch_and_add( &shard->get_hash_collisions, 1 );
            (void) __sync_fetch_and_sub( &wbuf->ref_count, 1 );
            return NULL;
        }

        *wbufp = wbuf;
        return buf;
    }
    else {
        *committed = 1;
    }

    return NULL;
}


uint32_t mcd_osd_get_lba_minsize( void ) {
    return MCD_OSD_LBA_MIN_BLKS * MCD_OSD_BLK_SIZE;
}


static int
mcd_fth_osd_fifo_get( void * context, mcd_osd_shard_t * shard, char * key,
                      int key_len, void ** ppdata, SDF_size_t * pactual_size,
                      int flags, struct objMetaData * meta_data,
                      uint64_t syndrome )
{
    int                         i, rc;
    int                         committed;
    char                      * buf;
    char                      * data_buf;
    uint64_t                    tmp_offset;
    uint64_t                    blk_offset;
    uint64_t                    offset;
    int                         nbytes;
    int                         nread;
    mcd_osd_meta_t            * meta;
    mcd_osd_hash_t            * hash_entry;
    mcd_osd_bucket_t          * bucket;
    mcd_osd_fifo_wbuf_t       * wbuf;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );
    (void) __sync_fetch_and_add( &shard->num_gets, 1 );

#ifdef  MCD_OSD_DEBUGGING
    (void) __sync_fetch_and_add( &Mcd_osd_get_cmds, 1 );
    if ( 0 == Mcd_osd_get_cmds % MCD_OSD_DBG_GET_DIV ) {
        mcd_log_msg( 20327, PLAT_LOG_LEVEL_INFO,
                     "%lu get cmds", Mcd_osd_get_cmds );
    }
#endif

    if ( NULL == ppdata ) {
        (void) __sync_fetch_and_add( &shard->get_exist_checks, 1 );
    }

    /*
     * look up the hash table entry
     */
    bucket = shard->hash_buckets +
        ( ( syndrome % shard->hash_size ) / Mcd_osd_bucket_size );
    hash_entry = shard->hash_table +
        ( ( syndrome % shard->hash_size ) & Mcd_osd_bucket_mask );

    for ( i = 0; i < bucket->next_item; i++,hash_entry++ ) {

        if ( 0 == hash_entry->used ) {
            plat_assert_always( 0 == 1 );
        }

        if ( (uint16_t)(syndrome >> 48) != hash_entry->syndrome ) {
            continue;
        }

        buf = mcd_fifo_find_buffered_obj( shard, key, key_len,
                                          hash_entry->address, &wbuf,
                                          &committed );
        if ( NULL != buf ) {

            meta = (mcd_osd_meta_t *)buf;
            if ( NULL != meta_data ) {
                meta_data->createTime = meta->create_time;
                meta_data->expTime    = meta->expiry_time;
            }

            if ( NULL == ppdata ) {
                /*
                 * ok only an existence check
                 */
                (void) __sync_fetch_and_sub( &wbuf->ref_count, 1 );
                return FLASH_EOK;
            }

                if ( MCD_OSD_MAX_BLKS_OLD >= hash_entry->blocks ) {
                    data_buf = ((osd_state_t *)context)->osd_buf;
                }
                else {
                    data_buf = mcd_fth_osd_iobuf_alloc(
                        mcd_osd_lba_to_blk( hash_entry->blocks ) *
                        Mcd_osd_blk_size, false );
                    if ( NULL == data_buf ) {
                        (void) __sync_fetch_and_sub( &wbuf->ref_count, 1 );
                        return FLASH_ENOMEM;
                    }
                }

            memcpy( data_buf, buf + sizeof(mcd_osd_meta_t) + key_len,
                    meta->data_len );

            *ppdata = data_buf;
            *pactual_size = meta->data_len;

            (void) __sync_fetch_and_sub( &wbuf->ref_count, 1 );
            return FLASH_EOK;
        }

        if ( 0 == committed ) {
            continue;
        }

        /*
         * ok, read the data from flash
         */
            if ( MCD_OSD_MAX_BLKS_OLD >= hash_entry->blocks ) {
                data_buf = ((osd_state_t *)context)->osd_buf;
            }
            else {
                data_buf = mcd_fth_osd_iobuf_alloc(
                    mcd_osd_lba_to_blk( hash_entry->blocks ) *
                    Mcd_osd_blk_size, false );
                if ( NULL == data_buf ) {
                    return FLASH_ENOMEM;
                }
            }
        buf = (char *)( ( (uint64_t)data_buf + Mcd_osd_blk_size - 1 )
                        & Mcd_osd_blk_mask );

        blk_offset = hash_entry->address;

        tmp_offset = shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
            + (blk_offset % Mcd_osd_rand_blksize);
        offset = tmp_offset * Mcd_osd_blk_size;

        if ( NULL == ppdata ) {
            /*
             * read only one block for existence check
             */
            nbytes = Mcd_osd_blk_size;
        }
        else {
            nbytes =
                mcd_osd_lba_to_blk( hash_entry->blocks ) * Mcd_osd_blk_size;
        }

        /*
         * FIXME_8MB: if the object is large, should we read the metadata
         * only first to verify that it's the right object?
         */
        nread = nbytes;
        rc = mcd_fth_aio_blk_read( context,
                                   buf,
                                   offset,
                                   nread );
        if ( FLASH_EOK != rc ) {
            mcd_log_msg( 20003, PLAT_LOG_LEVEL_ERROR,
                         "failed to read blocks, rc=%d", rc );
            mcd_fth_osd_slab_free( data_buf );
            return rc;
        }

        meta = (mcd_osd_meta_t *)buf;
        mcd_log_msg( 20312, PLAT_LOG_LEVEL_DEBUG, "key_len=%d data_len=%u",
                     meta->key_len, meta->data_len );

        if ( MCD_OSD_MAGIC_NUMBER != meta->magic ) {
            mcd_log_msg( 20325, PLAT_LOG_LEVEL_FATAL, "not enough magic!" );
            continue;
        }

        if ( key_len != meta->key_len ) {
            mcd_log_msg( 20326, MCD_OSD_LOG_LVL_DIAG,
                         "key length mismatch, req %d osd %d",
                         key_len, meta->key_len );
            (void) __sync_fetch_and_add( &shard->get_hash_collisions, 1 );
            mcd_fth_osd_slab_free( data_buf );
            continue;
        }

        if ( 0 != strncmp( buf + sizeof(mcd_osd_meta_t), key, key_len ) ) {
            mcd_log_msg( 20006, MCD_OSD_LOG_LVL_DIAG,
                         "key mismatch, req %s", key );
            (void) __sync_fetch_and_add( &shard->get_hash_collisions, 1 );
            mcd_fth_osd_slab_free( data_buf );
            continue;
        }

        if ( NULL != meta_data ) {
            meta_data->createTime = meta->create_time;
            meta_data->expTime    = meta->expiry_time;
        }

        if ( NULL == ppdata ) {
            /*
             * ok only an existence check
             */
            mcd_fth_osd_slab_free( data_buf );
            return FLASH_EOK;
        }

        *ppdata = data_buf;
        *pactual_size = meta->data_len;

        /*
         * meta can no longer be accessed after this
         */
        memmove( data_buf, buf + sizeof(mcd_osd_meta_t) + meta->key_len,
                 meta->data_len );

        return FLASH_EOK;
    }

    return FLASH_ENOENT;
}


static void
mcd_osd_fifo_notify_writer( mcd_osd_shard_t * shard )
{
    int cur_count = __sync_fetch_and_add( &shard->fifo.fth_count, 1 );
    if ( 0 == cur_count ) {
        fthMboxPost( &Mcd_osd_writer_mbox, (uint64_t)shard );
    }
}


static int
mcd_fth_osd_fifo_set( void * context, mcd_osd_shard_t * shard, char * key,
                      int key_len, void * data, SDF_size_t data_len, int flags,
                      struct objMetaData * meta_data, uint64_t syndrome )
{
    int                         i, n, rc;
    int                         committed;
    bool                        obj_exists = false;
    SDF_size_t                  raw_len;
    uint32_t                    blocks;
    int                         skip;
    char                      * buf;
    char                      * data_buf = NULL;
    uint32_t                    pending;
    uint32_t                    blk_filled;
    uint64_t                    blk_nextbuf;
    uint64_t                    tmp_offset;
    uint64_t                    blk_offset;
    uint64_t                    offset;
    uint64_t                    mail;
    mcd_osd_meta_t            * meta;
    mcd_osd_hash_t              new_entry;
    mcd_osd_hash_t            * hash_entry;
    mcd_osd_hash_t            * bucket_head;
    mcd_osd_bucket_t          * bucket;
    mcd_osd_fifo_wbuf_t       * wbuf;
    osd_state_t               * osd_state = (osd_state_t *)context;

    new_entry.r1 = 0;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );
    (void) __sync_fetch_and_add( &shard->num_puts, 1 );

#ifdef  MCD_OSD_DEBUGGING
    (void) __sync_fetch_and_add( &Mcd_osd_set_cmds, 1 );
    if ( 0 == Mcd_osd_set_cmds % MCD_OSD_DBG_SET_DIV ) {
        mcd_log_msg( 20329, PLAT_LOG_LEVEL_INFO,
                     "%lu sets, b_alloc=%lu overwrites=%lu evictions=%lu "
                     "soft_of=%lu hard_of=%lu p_id=%d ",
                     Mcd_osd_set_cmds,
                     shard->blk_allocated,
                     shard->num_overwrites,
                     shard->num_hash_evictions,
                     shard->num_soft_overflows,
                     shard->num_hard_overflows,
                     Mcd_pthread_id );
    }
#endif

    raw_len = sizeof(mcd_osd_meta_t) + key_len + data_len;
    blocks = ( raw_len + ( Mcd_osd_blk_size - 1 ) ) / Mcd_osd_blk_size;

    if ( MCD_OSD_OBJ_MAX_BLKS <= blocks ) {
        mcd_log_msg( 20330, PLAT_LOG_LEVEL_ERROR,
                     "object size beyond limit, raw_len=%lu", raw_len );
        return FLASH_EINVAL;
    }

    data_buf = ((osd_state_t *)context)->osd_buf;
    buf = (char *)( ( (uint64_t)data_buf + Mcd_osd_blk_size - 1 )
                    & Mcd_osd_blk_mask );

    bucket = shard->hash_buckets +
        ( ( syndrome % shard->hash_size ) / Mcd_osd_bucket_size );
    bucket_head = shard->hash_table +
        ( ( syndrome % shard->hash_size ) & Mcd_osd_bucket_mask );
    hash_entry = bucket_head;

    for ( i = 0; i < bucket->next_item; i++,hash_entry++ ) {

        if ( 0 == hash_entry->used ) {
            plat_assert_always( 0 == 1 );
        }

        if ( (uint16_t)(syndrome >> 48) != hash_entry->syndrome ) {
            continue;
        }

        if ( NULL !=
             mcd_fifo_find_buffered_obj( shard, key, key_len,
                                         hash_entry->address, &wbuf,
                                         &committed ) ) {
            obj_exists = true;
            mcd_log_msg( 20331, PLAT_LOG_LEVEL_DEBUG, "object exists" );

            (void) __sync_fetch_and_sub( &wbuf->ref_count, 1 );

            if ( FLASH_PUT_TEST_EXIST & flags ) {
                rc = FLASH_EEXIST;
                goto out;
            }

            if ( NULL == data ) {
                /*
                 * item found in memory, to be deleted
                 */
                // mcd_fth_osd_remove_entry( shard, hash_entry );  // FIXME
                (void) __sync_fetch_and_sub(
                    &shard->blk_consumed,
                    mcd_osd_lba_to_blk( hash_entry->blocks ) );
                *((uint64_t *)hash_entry) = 0;

                if ( 0 == bucket->next_item ) {
                    plat_assert_always( 0 == 1 );
                }
                if ( bucket_head + bucket->next_item - 1 != hash_entry ) {
                    *hash_entry = bucket_head[--bucket->next_item];
                    *((uint64_t *)&bucket_head[bucket->next_item]) = 0;
                }
                else {
                    bucket->next_item--;
                }

                if ( Mcd_osd_bucket_size - 1 == bucket->next_item ) {
                    (void) __sync_fetch_and_sub( &shard->num_full_buckets, 1 );
                }
                (void) __sync_fetch_and_sub( &shard->num_objects, 1 );
                (void) __sync_fetch_and_add( &shard->num_deletes, 1 );

                rc = FLASH_EOK;
                goto out;
            }

            break;
        }

        if ( 0 == committed ) {
            continue;
        }

        blk_offset = hash_entry->address;

        tmp_offset = shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
            + (blk_offset % Mcd_osd_rand_blksize);
        offset = tmp_offset * Mcd_osd_blk_size;

        rc = mcd_fth_aio_blk_read( context,
                                   buf,
                                   offset,
                                   Mcd_osd_blk_size );
        if ( FLASH_EOK != rc ) {
            mcd_log_msg( 20003, PLAT_LOG_LEVEL_ERROR,
                         "failed to read blocks, rc=%d", rc );
            goto out;
        }

        meta = (mcd_osd_meta_t *)buf;
        mcd_log_msg( 20312, PLAT_LOG_LEVEL_DEBUG, "key_len=%d data_len=%u",
                     meta->key_len, meta->data_len );

        if ( MCD_OSD_MAGIC_NUMBER != meta->magic ) {
            mcd_log_msg( 20325, PLAT_LOG_LEVEL_FATAL, "not enough magic!" );
            continue;
        }

        if ( key_len != meta->key_len ) {
            mcd_log_msg( 20326, MCD_OSD_LOG_LVL_DIAG,
                         "key length mismatch, req %d osd %d",
                         key_len, meta->key_len );
            (void) __sync_fetch_and_add( &shard->get_hash_collisions, 1 );
            continue;
        }

        if ( 0 != strncmp( buf + sizeof(mcd_osd_meta_t), key, key_len ) ) {
            mcd_log_msg( 20006, MCD_OSD_LOG_LVL_DIAG,
                         "key mismatch, req %s", key );
            (void) __sync_fetch_and_add( &shard->get_hash_collisions, 1 );
            continue;
        }

        obj_exists = true;
        mcd_log_msg( 20331, PLAT_LOG_LEVEL_DEBUG, "object exists" );

        if ( FLASH_PUT_TEST_EXIST & flags ) {
            rc = FLASH_EEXIST;
            goto out;
        }

        if ( NULL == data ) {
            /*
             * item found on flash, to be deleted
             */
            // mcd_fth_osd_remove_entry( shard, hash_entry );   // FIXME
            (void) __sync_fetch_and_sub(
                &shard->blk_consumed,
                mcd_osd_lba_to_blk( hash_entry->blocks ) );
            *((uint64_t *)hash_entry) = 0;

            if ( 0 == bucket->next_item ) {
                plat_assert_always( 0 == 1 );
            }
            if ( bucket_head + bucket->next_item - 1 != hash_entry ) {
                *hash_entry = bucket_head[--bucket->next_item];
                *((uint64_t *)&bucket_head[bucket->next_item]) = 0;
            }
            else {
                bucket->next_item--;
            }

            if ( Mcd_osd_bucket_size - 1 == bucket->next_item ) {
                (void) __sync_fetch_and_sub( &shard->num_full_buckets, 1 );
            }
            (void) __sync_fetch_and_sub( &shard->num_objects, 1 );
            (void) __sync_fetch_and_add( &shard->num_deletes, 1 );

            rc = FLASH_EOK;
            goto out;
        }

        break;
    }

    if ( NULL == data ) {
        rc = FLASH_ENOENT;
        goto out;
    }

    if ( false == obj_exists ) {
        if ( FLASH_PUT_TEST_NONEXIST & flags ) {
            rc = FLASH_ENOENT;
            goto out;
        }
        if ( Mcd_osd_bucket_size == bucket->next_item ) {
            hash_entry = bucket_head;
            for ( n = 0; n < Mcd_osd_bucket_size; n++, hash_entry++ ) {
                if ( 0 == hash_entry->deleted ) {
                    break;
                }
            }
            if ( Mcd_osd_bucket_size == n ) {
                rc = FLASH_ENOSPC;
                goto out;
            }
        }
    }
    else {
        /*
         * remove the hash entry first as we may lose control over the
         * hash bucket during allocation
         */
        (void) __sync_fetch_and_sub(
            &shard->blk_consumed,
            mcd_osd_lba_to_blk( hash_entry->blocks ) );
        *((uint64_t *)hash_entry) = 0;

        if ( 0 == bucket->next_item ) {
            plat_assert_always( 0 == 1 );
        }
        if ( bucket_head + bucket->next_item - 1 != hash_entry ) {
            *hash_entry = bucket_head[--bucket->next_item];
            *((uint64_t *)&bucket_head[bucket->next_item]) = 0;
        }
        else {
            bucket->next_item--;
        }

        if ( Mcd_osd_bucket_size - 1 == bucket->next_item ) {
            (void) __sync_fetch_and_sub( &shard->num_full_buckets, 1 );
        }
        (void) __sync_fetch_and_sub( &shard->num_objects, 1 );
        (void) __sync_fetch_and_add( &shard->num_overwrites, 1 );
    }

    /*
     * allocate space on flash
     */
    do {
        blk_offset = __sync_fetch_and_add( &shard->fifo.blk_reserved, blocks );
        if ( blk_offset > 0xfffffffffffff000ULL ) {
            mcd_log_msg( 20332, PLAT_LOG_LEVEL_FATAL,
                         "block offset about to wrap" );
            plat_abort();
        }

        blk_nextbuf = shard->fifo.blk_nextbuf;
        if ( blk_offset + blocks >
             blk_nextbuf + MCD_OSD_NUM_WBUFS * MCD_OSD_WBUF_BLKS ) {
            /*
             * ok we are writing too fast, time to sleep
             */
            (void) __sync_fetch_and_sub( &shard->fifo.blk_reserved, blocks );

            ((osd_state_t *)context)->osd_blocks = blocks;
            if ( flash_settings.multi_fifo_writers ) {
                fthMboxPost( &shard->fifo.sleeper_mbox, (uint64_t)context );
            }
            else {
                fthMboxPost( &Mcd_osd_sleeper_mbox, (uint64_t)context );
            }

            if ( blk_nextbuf != shard->fifo.blk_nextbuf ) {
                /*
                 * FIXME_MWRITER: note that the mail might be consumed
                 * already and also the value would change next time when
                 * a sleeper mail is sent
                 */
                ((osd_state_t *)context)->osd_blocks = 0;
                mcd_dbg_msg( PLAT_LOG_LEVEL_WARN,
                             "mail might arrive too late, don't sleep yet "
                             "old_next=%lu new_next=%lu",
                             blk_nextbuf, shard->fifo.blk_nextbuf );
                continue;
            }
            mcd_log_msg( 20333, PLAT_LOG_LEVEL_DEBUG,
                         "mail posted, ready to sleep" );

            /*
             * unlock the bucket before sleeping
             */
            fthUnlock( osd_state->osd_wait );

            mail = fthMboxWait( ((osd_state_t *)context)->osd_mbox );
            mcd_log_msg( 20334, PLAT_LOG_LEVEL_DEBUG,
                         "waked up, mail=%lu", mail );

            /*
             * relock the bucket
             */
            osd_state->osd_wait = fthLock( osd_state->osd_lock, 1, NULL );

            continue;
        }

        blk_offset =
            __sync_fetch_and_add( &shard->fifo.blk_allocated, blocks );

        if ( blk_offset + blocks > shard->fifo.blk_nextbuf +
             MCD_OSD_NUM_WBUFS * MCD_OSD_WBUF_BLKS ) {
            mcd_log_msg( 20335, PLAT_LOG_LEVEL_FATAL,
                         "over allocation detected, "
                         "rsvd=%lu alloc=%lu next=%lu offset=%lu blks=%d",
                         shard->fifo.blk_reserved, shard->fifo.blk_allocated,
                         shard->fifo.blk_nextbuf, blk_offset, blocks );
            plat_assert_always( 0 == 1 );
        }
        if ( blk_offset / MCD_OSD_WBUF_BLKS
             == ( blk_offset + blocks - 1 ) / MCD_OSD_WBUF_BLKS ) {
            break;
        }

        /*
         * allocation crossed wbuf boundary, try again
         */
        wbuf = &shard->fifo.wbufs[
            ( blk_offset / MCD_OSD_WBUF_BLKS ) % MCD_OSD_NUM_WBUFS];

        skip = MCD_OSD_WBUF_BLKS - (blk_offset % MCD_OSD_WBUF_BLKS);
        memset( wbuf->buf + (MCD_OSD_WBUF_BLKS - skip) * Mcd_osd_blk_size,
                0, skip * Mcd_osd_blk_size );
        blk_filled = __sync_add_and_fetch( &wbuf->filled, skip );

        if ( MCD_OSD_WBUF_BLKS == blk_filled ) {
            if ( false == flash_settings.multi_fifo_writers ) {
                pending =
                    __sync_fetch_and_add( &shard->fifo.pending_wmails, 1 );
                if ( MCD_OSD_NUM_WBUFS <= pending ) {
                    (void) __sync_fetch_and_sub(
                        &shard->fifo.pending_wmails, 1 );
                }
                else {
                    fthMboxPost( &Mcd_osd_writer_mbox, (uint64_t)shard );
                    mcd_log_msg( 20336, MCD_OSD_LOG_LVL_INFO,
                                 "wbuf %d full, writer mail posted off=%lu "
                                 "rsvd=%lu alloc=%lu cmtd=%lu next=%lu",
                                 wbuf->id, blk_offset,
                                 shard->fifo.blk_reserved,
                                 shard->fifo.blk_allocated,
                                 shard->fifo.blk_committed,
                                 shard->fifo.blk_nextbuf );
                }
            }
            else {
                mcd_osd_fifo_notify_writer( shard );
                mcd_log_msg( 50042, MCD_OSD_LOG_LVL_INFO,
                             "wbuf %d full, writer notified off=%lu "
                             "rsvd=%lu alloc=%lu cmtd=%lu next=%lu",
                             wbuf->id, blk_offset,
                             shard->fifo.blk_reserved,
                             shard->fifo.blk_allocated,
                             shard->fifo.blk_committed,
                             shard->fifo.blk_nextbuf );
            }
        }

        wbuf = &shard->fifo.wbufs[
            ( (blk_offset + blocks) / MCD_OSD_WBUF_BLKS ) % MCD_OSD_NUM_WBUFS];

        skip = (blk_offset + blocks) % MCD_OSD_WBUF_BLKS;
        memset( wbuf->buf, 0, skip * Mcd_osd_blk_size );
        blk_filled = __sync_add_and_fetch( &wbuf->filled, skip );

        if ( MCD_OSD_WBUF_BLKS == blk_filled ) {
            if ( false == flash_settings.multi_fifo_writers ) {
                pending =
                    __sync_fetch_and_add( &shard->fifo.pending_wmails, 1 );
                if ( MCD_OSD_NUM_WBUFS <= pending ) {
                    (void) __sync_fetch_and_sub(
                        &shard->fifo.pending_wmails, 1 );
                }
                else {
                    fthMboxPost( &Mcd_osd_writer_mbox, (uint64_t)shard );
                    mcd_log_msg( 20336, MCD_OSD_LOG_LVL_INFO,
                                 "wbuf %d full, writer mail posted off=%lu "
                                 "rsvd=%lu alloc=%lu cmtd=%lu next=%lu",
                                 wbuf->id, blk_offset,
                                 shard->fifo.blk_reserved,
                                 shard->fifo.blk_allocated,
                                 shard->fifo.blk_committed,
                                 shard->fifo.blk_nextbuf );
                }
            }
            else {
                mcd_osd_fifo_notify_writer( shard );
                mcd_log_msg( 50042, MCD_OSD_LOG_LVL_INFO,
                             "wbuf %d full, writer notified off=%lu "
                             "rsvd=%lu alloc=%lu cmtd=%lu next=%lu",
                             wbuf->id, blk_offset,
                             shard->fifo.blk_reserved,
                             shard->fifo.blk_allocated,
                             shard->fifo.blk_committed,
                             shard->fifo.blk_nextbuf );
            }
        }

    } while ( 1 );

    wbuf = &shard->fifo.wbufs[
        ( blk_offset / MCD_OSD_WBUF_BLKS ) % MCD_OSD_NUM_WBUFS ];

    buf = wbuf->buf + (blk_offset % MCD_OSD_WBUF_BLKS) * MCD_OSD_BLK_SIZE;

    meta = (mcd_osd_meta_t *)buf;
    meta->magic    = MCD_OSD_MAGIC_NUMBER;
    meta->key_len  = key_len;
    meta->data_len = data_len;

    if ( NULL != meta_data ) {
        meta->create_time = meta_data->createTime;
        meta->expiry_time = meta_data->expTime;
        meta->seqno = meta_data->sequence;
    }

    memcpy( buf + sizeof(mcd_osd_meta_t), key, key_len );
    memcpy( buf + sizeof(mcd_osd_meta_t) + key_len, data, data_len );

    plat_log_msg( 20337, PLAT_LOG_CAT_SDF_SIMPLE_REPLICATION,
                  PLAT_LOG_LEVEL_DEBUG,
                  "store object [%ld][%d]: syndrome: %lx key_len: %d",
                  (syndrome % shard->hash_size) / Mcd_osd_bucket_size, i,
                  syndrome, key_len );

    /*
     * update the hash table entry
     */
    new_entry.used       = 1;
    new_entry.referenced = 1;
    new_entry.blocks     = mcd_osd_blk_to_lba( blocks );
    new_entry.syndrome   = (uint16_t)(syndrome >> 48);
    new_entry.address    = blk_offset % shard->total_blks;

    new_entry.deleted    = 0;
    if ( &Mcd_osd_cmc_cntr != shard->cntr ) {
        if ((*flash_settings.check_delete_in_future)(data)) {
            new_entry.deleted = 1;
        }
    }

    if ( Mcd_osd_bucket_size == bucket->next_item ) {
        n = 0;
        do {
            hash_entry =
                bucket_head + ( bucket->hand % Mcd_osd_bucket_size );
            if ( 0 == hash_entry->used ) {
                plat_assert_always( 0 == 1 );
            }
            if ( 0 == hash_entry->referenced && 0 == hash_entry->deleted ) {
                break;
            }
            if ( ( 2 * Mcd_osd_bucket_size ) < n ) {
                hash_entry = NULL;
                break;
            }
            hash_entry->referenced = 0;
            bucket->hand++;
        } while ( ++n );

        if ( hash_entry ) {
            // mcd_fth_osd_remove_entry( shard, hash_entry );       // FIXME
            (void) __sync_fetch_and_sub(
                &shard->blk_consumed,
                mcd_osd_lba_to_blk( hash_entry->blocks ) );
            *((uint64_t *)hash_entry) = 0;
            (void) __sync_fetch_and_add( &shard->num_hash_evictions, 1 );
        }
    }
    else {
        hash_entry = bucket_head + bucket->next_item++;
        (void) __sync_fetch_and_add( &shard->num_objects, 1 );
        if ( Mcd_osd_bucket_size == bucket->next_item ) {
            (void) __sync_fetch_and_add( &shard->num_full_buckets, 1 );
        }
    }

    if ( hash_entry ) {
        *hash_entry = new_entry;
        shard->addr_table[new_entry.address] = hash_entry - shard->hash_table;
        (void) __sync_fetch_and_add( &shard->blk_consumed,
                                     mcd_osd_lba_to_blk( new_entry.blocks ) );
        (void) __sync_fetch_and_add( &shard->total_objects, 1 );
    }

    blk_filled = __sync_add_and_fetch( &wbuf->filled, blocks );
    mcd_log_msg( 20338, PLAT_LOG_LEVEL_DEBUG,
                 "blocks allocated, key_len=%d data_len=%lu "
                 "blk_offset=%lu addr=%lu blocks=%d filled=%u",
                 key_len, data_len, blk_offset,
                 blk_offset % shard->total_blks, blocks, blk_filled );

    if ( MCD_OSD_WBUF_BLKS == blk_filled ) {
        if ( false == flash_settings.multi_fifo_writers ) {
            pending = __sync_fetch_and_add( &shard->fifo.pending_wmails, 1 );
            if ( MCD_OSD_NUM_WBUFS <= pending ) {
                (void) __sync_fetch_and_sub( &shard->fifo.pending_wmails, 1 );
            }
            else {
                fthMboxPost( &Mcd_osd_writer_mbox, (uint64_t)shard );
                mcd_log_msg( 20336, MCD_OSD_LOG_LVL_INFO,
                             "wbuf %d full, writer mail posted "
                             "off=%lu rsvd=%lu alloc=%lu cmtd=%lu next=%lu",
                             wbuf->id, blk_offset,
                             shard->fifo.blk_reserved,
                             shard->fifo.blk_allocated,
                             shard->fifo.blk_committed,
                             shard->fifo.blk_nextbuf );
            }
        }
        else {
            mcd_osd_fifo_notify_writer( shard );
            mcd_log_msg( 50042, MCD_OSD_LOG_LVL_INFO,
                         "wbuf %d full, writer notified "
                         "off=%lu rsvd=%lu alloc=%lu cmtd=%lu next=%lu",
                         wbuf->id, blk_offset,
                         shard->fifo.blk_reserved,
                         shard->fifo.blk_allocated,
                         shard->fifo.blk_committed,
                         shard->fifo.blk_nextbuf );
        }
    }

    if ( hash_entry ) {
        return FLASH_EOK;
    }
    else {
        return FLASH_ENOSPC;
    }

out:
    return rc;
}


/************************************************************************
 *                                                                      *
 *                      Memcached SLAB SSD subsystem                    *
 *                                                                      *
 ************************************************************************/


#define MCD_OSD_OVERFLOW_DEPTH  32
#define MCD_OSD_FREELIST_LEN    32768
#define MCD_OSD_FREELIST_MIN    64
#define MCD_OSD_SCAN_THRESHOLD  20
#define MCD_OSD_SCAN_TBSIZE     4093
#define MCD_OSD_CACHE_SIZE      (32ULL * 1073741824)

int                     Mcd_osd_max_nclasses    = MCD_OSD_MAX_NCLASSES;

uint64_t                Mcd_osd_blk_size        = MCD_OSD_BLK_SIZE;
uint64_t                Mcd_osd_blk_mask        = 0xfffffffffffffe00ULL;

uint64_t                Mcd_osd_segment_size    = MCD_OSD_SEGMENT_SIZE;
uint64_t                Mcd_osd_segment_blks    = MCD_OSD_SEGMENT_BLKS;
fthLock_t               Mcd_osd_segment_lock;

uint64_t                Mcd_osd_bucket_size     = MCD_OSD_BUCKET_SIZE;
uint64_t                Mcd_osd_bucket_mask     = MCD_OSD_BUCKET_MASK;

uint64_t                Mcd_osd_overflow_depth  = MCD_OSD_OVERFLOW_DEPTH;

uint64_t                Mcd_osd_free_seg_curr;
uint64_t                Mcd_osd_free_segments[MCD_OSD_MAX_SEGMENTS];

mcd_osd_shard_t       * Mcd_osd_slab_shards[MCD_OSD_MAX_NUM_SHARDS];

uint64_t                Mcd_osd_bitmap_masks[64];
static uint64_t         Mcd_osd_scan_masks[64];
static uint8_t          Mcd_osd_scan_table[MCD_OSD_SCAN_TBSIZE];


static __attribute__((unused)) int mcd_osd_slab_init();


/*
 * FIXME: temporary place holder
 */
static mcd_osd_shard_t  Mcd_osd_slab_shard;


static inline uint64_t *
get_bitmaps( int bitmap_size, int initializer )
{
    uint64_t                  * bitmaps;

    bitmaps = (uint64_t *)plat_alloc_large( bitmap_size );
    if ( NULL != bitmaps ) {
        memset( (void *)bitmaps, initializer, bitmap_size );
    }
    return bitmaps;
}


static int
mcd_osd_slab_shard_init( mcd_osd_shard_t * shard, uint64_t shard_id,
                         int flags, uint64_t quota, unsigned max_nobjs )
{
    int                    i, j;
    int                    blksize;
    int                    max_segments;
    int                    bitmap_size;
    uint64_t               total_alloc = 0;
    uint64_t             * bitmaps[5] = {NULL, NULL, NULL, NULL, NULL};
    mcd_osd_segment_t    * segments = NULL;
    mcd_osd_slab_class_t * class;

    mcd_log_msg( 20293, PLAT_LOG_LEVEL_INFO,
                 "ENTERING, shard=%p shard_id=%lu flags=0x%x quota=%lu "
                 "max_nobjs=%u",
                 shard, shard_id, flags, quota, max_nobjs );

    shard->id = shard_id;

    if ( FLASH_SHARD_INIT_PERSISTENCE_YES ==
         (flags & FLASH_SHARD_INIT_PERSISTENCE_MASK) ) {
        shard->persistent = 1;
    }

    if ( FLASH_SHARD_INIT_EVICTION_CACHE ==
         (flags & FLASH_SHARD_INIT_EVICTION_MASK) ) {
        shard->evict_to_free = 1;
    }

    if ( FLASH_SHARD_SEQUENCE_EXTERNAL ==
         (flags & FLASH_SHARD_SEQUENCE_MASK) ) {
        shard->replicated = 1;
    }

    shard->total_size = shard->total_segments * Mcd_osd_segment_size;
    shard->total_blks = shard->total_segments * Mcd_osd_segment_blks;

    if ( 4294967296ULL < shard->total_blks ) {
        mcd_log_msg( 20294, PLAT_LOG_LEVEL_FATAL,
                     "shard size greater than 2TB, not supported yet" );
        return FLASH_EINVAL;
    }

    /*
     * initialize slab classes and segments
     */
    max_segments = shard->total_size / Mcd_osd_segment_size;

    segments = (mcd_osd_segment_t *)
        plat_alloc( max_segments * sizeof(mcd_osd_segment_t) );
    if ( NULL == segments ) {
        mcd_log_msg( 20339, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocate slab segments" );
        goto out_failed;
    }
    memset( (void *)segments, 0,
            max_segments * sizeof(mcd_osd_segment_t) );
    total_alloc += max_segments * sizeof(mcd_osd_segment_t);
    mcd_log_msg( 40111, PLAT_LOG_LEVEL_DEBUG,
                 "base segment table initialized, size=%lu",
                 max_segments * sizeof(mcd_osd_segment_t) );

    bitmap_size = Mcd_osd_segment_blks / 8;
    for ( j = 0; j < 5; j ++ ) {
        bitmaps[j] = get_bitmaps( max_segments * bitmap_size,
                                  (j == 0 ? 0xff : 0) );
        if ( NULL == bitmaps[j] ) {
            mcd_log_msg( 20340, PLAT_LOG_LEVEL_ERROR,
                         "failed to allocate segment bitmaps[%d]", j );
            goto out_failed;
        }
        total_alloc += max_segments * bitmap_size;
    }
    mcd_log_msg( 40112, PLAT_LOG_LEVEL_DEBUG,
                 "bitmaps initialized, size=%d, bitmap_size=%d, total=%d",
                 bitmap_size, max_segments * bitmap_size,
                 5 * max_segments * bitmap_size );

    for ( j = 0; j < max_segments; j++ ) {
        segments[j].bitmap =
            (uint64_t *)( (char *)bitmaps[0] + ( j * bitmap_size ) );
        segments[j].update_map =
            (uint64_t *)( (char *)bitmaps[1] + ( j * bitmap_size ) );
        segments[j].update_map_s =
            (uint64_t *)( (char *)bitmaps[2] + ( j * bitmap_size ) );
        segments[j].alloc_map =
            (uint64_t *)( (char *)bitmaps[3] + ( j * bitmap_size ) );
        segments[j].alloc_map_s =
            (uint64_t *)( (char *)bitmaps[4] + ( j * bitmap_size ) );
    }

    shard->base_segments = segments;

    for ( i = 0, blksize = 1; i < MCD_OSD_MAX_NCLASSES; i++ ) {

        fthLockInit( &shard->slab_classes[i].lock );
        shard->slab_classes[i].segments = (mcd_osd_segment_t **)
            plat_alloc( max_segments * sizeof(mcd_osd_segment_t *) );
        if ( NULL == shard->slab_classes[i].segments ) {
            mcd_log_msg( 20339, PLAT_LOG_LEVEL_ERROR,
                         "failed to allocate slab segments" );
            return FLASH_ENOMEM;
        }
        memset( (void *)shard->slab_classes[i].segments, 0,
                max_segments * sizeof(mcd_osd_segment_t *) );
        total_alloc += max_segments * sizeof(mcd_osd_segment_t *);

        shard->slab_classes[i].slab_blksize = blksize;
        shard->slab_classes[i].slabs_per_segment =
            Mcd_osd_segment_size / Mcd_osd_blk_size / blksize;

        // make free list proportional to container and class
        class = &shard->slab_classes[i];
        class->freelist_len = shard->total_blks / 32768 / blksize;
        if ( class->freelist_len > MCD_OSD_FREELIST_LEN ) {
            class->freelist_len = MCD_OSD_FREELIST_LEN;
        } else if ( class->freelist_len < MCD_OSD_FREELIST_MIN ) {
            class->freelist_len = MCD_OSD_FREELIST_MIN;
        }

        for ( j = 0; j < MCD_OSD_MAX_PTHREADS; j++ ) {
            class->free_slab_curr[j] = 0;
            class->free_slabs[j] = (uint32_t *)
                plat_alloc( class->freelist_len * sizeof(uint32_t) );
            if ( NULL == class->free_slabs[j] ) {
                mcd_log_msg( 20341, PLAT_LOG_LEVEL_ERROR,
                             "failed to allocate free lists" );
                return FLASH_ENOMEM;
            }
            memset( (void *)class->free_slabs[j], 0,
                    class->freelist_len * sizeof(uint32_t) );
            total_alloc += class->freelist_len * sizeof(uint32_t);
        }
        mcd_log_msg( 40113, PLAT_LOG_LEVEL_DEBUG,
                     "slab class inited, blksize=%d, free_slabs=%lu, "
                     "segments=%lu",
                     blksize, class->freelist_len * sizeof(uint32_t),
                     max_segments * sizeof(mcd_osd_segment_t *) );

        shard->num_classes++;
        if ( blksize > MCD_OSD_OBJ_MAX_BLKS ) {
            mcd_log_msg( 50043, MCD_OSD_LOG_LVL_INFO,
                         "max slab_blksize=%d", blksize );
            break;
        }
        blksize *= 2;
    }

    shard->segment_table = (mcd_osd_segment_t **)
        plat_alloc( max_segments * sizeof(mcd_osd_segment_t *) );
    if ( NULL == shard->segment_table ) {
        mcd_log_msg( 20343, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocate segment lookup table" );
        return FLASH_ENOMEM;
    }
    memset( (void *)shard->segment_table, 0,
            max_segments * sizeof(mcd_osd_segment_t *) );
    total_alloc += max_segments * sizeof(mcd_osd_segment_t *);

    mcd_log_msg( 20344, PLAT_LOG_LEVEL_INFO,
                 "segments initialized, %d segments, %d classes size=%lu",
                 max_segments, i, total_alloc );

    /*
     * hash table will be allocated together with the overflow area later
     */
    shard->hash_size = shard->total_size / Mcd_osd_blk_size;
    shard->hash_size += shard->hash_size / 4;
    if ( 0 < max_nobjs && max_nobjs < shard->hash_size ) {
        shard->hash_size = ( max_nobjs + Mcd_osd_segment_blks - 1 )
            / Mcd_osd_segment_blks * Mcd_osd_segment_blks;
    }

    /*
     * initialize the address lookup table
     */
    shard->addr_table = (uint32_t *)
        plat_alloc_large( shard->total_blks * sizeof(uint32_t) );
    if ( NULL == shard->addr_table ) {
        mcd_log_msg( 150029, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocate hash table: %lu", shard->total_blks * sizeof(uint32_t) );
        return FLASH_ENOMEM;
    }
    memset( (void *)shard->addr_table, 0,
            shard->total_blks * sizeof(uint32_t) );
    total_alloc += shard->total_blks * sizeof(uint32_t);
    mcd_log_msg( 20307, PLAT_LOG_LEVEL_INFO,
                 "address table initialized, size=%lu",
                 shard->total_blks * sizeof(uint32_t) );

    /*
     * initialize the address remapping table
     */
    shard->rand_table = (uint32_t *)plat_alloc(
        (shard->total_blks / Mcd_osd_rand_blksize) * sizeof(uint32_t) );
    if ( NULL == shard->rand_table ) {
        mcd_log_msg( 20295, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocate random table" );
        return FLASH_ENOMEM;
    }
    total_alloc +=
        (shard->total_blks / Mcd_osd_rand_blksize) * sizeof(uint32_t);

    /*
     * exclude metadata segments here since recovery code makes them
     * transparent
     */
    int temp = Mcd_osd_segment_blks / Mcd_osd_rand_blksize;
    uint32_t * tmp_segments = shard->segments +
        shard->data_blk_offset / Mcd_osd_segment_blks;
    for ( i = 0; i < shard->total_blks / Mcd_osd_rand_blksize; i++ ) {
        if ( 0 == flash_settings.mq_ssd_balance ) {
            shard->rand_table[i] = tmp_segments[i / temp]
                + (i % temp) * Mcd_osd_rand_blksize;
        }
        else {
            shard->rand_table[i] = tmp_segments[i / temp]
                + ( (i + i / temp) % temp ) * Mcd_osd_rand_blksize;
            mcd_log_msg( 20296, PLAT_LOG_LEVEL_INFO,
                         "rand_table[%d]=%u, segments[%d]=%u",
                         i, shard->rand_table[i],
                         i / temp, tmp_segments[i / temp] );
        }
    }
#ifdef MCD_DISABLED
    uint32_t temp;
    for ( i = 0; i < shard->total_blks / Mcd_osd_rand_blksize; i++ ) {
        j = (uint32_t) ( (shard->total_blks / Mcd_osd_rand_blksize) *
                         (rand() / (RAND_MAX + 1.0) ) );
        temp = shard->rand_table[i];
        shard->rand_table[i] = shard->rand_table[j];
        shard->rand_table[j] = temp;
    }
#endif
    mcd_log_msg( 20297, PLAT_LOG_LEVEL_INFO,
                 "remapping table initialized, size=%lu",
                 (shard->total_blks / Mcd_osd_rand_blksize) *
                 sizeof(uint32_t) );

    /*
     * allocate the bucket table
     */
    shard->hash_buckets = (mcd_osd_bucket_t *)plat_alloc_large(
        shard->hash_size / Mcd_osd_bucket_size * sizeof(mcd_osd_bucket_t) );
    if ( NULL == shard->hash_buckets ) {
        mcd_log_msg( 20298, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocate hash buckets" );
        return FLASH_ENOMEM;
    }

    memset( (void *)shard->hash_buckets, 0,
        shard->hash_size / Mcd_osd_bucket_size * sizeof(mcd_osd_bucket_t) );
    total_alloc +=
        shard->hash_size / Mcd_osd_bucket_size * sizeof(mcd_osd_bucket_t);
    mcd_log_msg( 20299, PLAT_LOG_LEVEL_INFO,
                 "hash buckets allocated, size=%lu",
                 shard->hash_size /
                 Mcd_osd_bucket_size * sizeof(mcd_osd_bucket_t) );

    /*
     * initialize the lock buckets
     */
    shard->lock_bktsize = MCD_OSD_LOCKBKT_MINSIZE;
    shard->lock_buckets = shard->hash_size / shard->lock_bktsize;
    while ( MCD_OSD_LOCK_BUCKETS < shard->lock_buckets ) {
        shard->lock_bktsize *= 2;
        shard->lock_buckets /= 2;
    }
    while ( 32768 > shard->lock_buckets ) {
        shard->lock_bktsize /= 2;
        shard->lock_buckets *= 2;
    }

    shard->bucket_locks = (fthLock_t *)
        plat_alloc_large( shard->lock_buckets * sizeof(fthLock_t) );
    if ( NULL == shard->bucket_locks ) {
        mcd_log_msg( 20099, PLAT_LOG_LEVEL_ERROR,
                     "failed to alloc bucket locks" );
        return FLASH_ENOMEM;
    }
    total_alloc += shard->lock_buckets * sizeof(fthLock_t);

    for ( i = 0; i < shard->lock_buckets; i++ ) {
        fthLockInit( &shard->bucket_locks[i] );
    }
    mcd_log_msg( 20300, PLAT_LOG_LEVEL_INFO,
                 "lock_buckets=%lu lock_bktsize=%d, total lock size=%lu",
                 shard->lock_buckets, shard->lock_bktsize,
                 shard->lock_buckets * sizeof(fthLock_t) );

    /*
     * initialize hash table and its overflow area
     */
    shard->hash_table = (mcd_osd_hash_t *)
        plat_alloc_large( ( shard->lock_buckets * Mcd_osd_overflow_depth +
                            shard->hash_size ) * sizeof(mcd_osd_hash_t) );
    if ( NULL == shard->hash_table ) {
        mcd_log_msg( 20301, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocate hash table" );
        return FLASH_ENOMEM;
    }
    plat_assert_always(
        0 == ( (uint64_t)shard->hash_table % sizeof(mcd_osd_hash_t) ) );

    memset( (void *)shard->hash_table, 0,
            shard->hash_size * sizeof(mcd_osd_hash_t) );
    total_alloc += shard->hash_size * sizeof(mcd_osd_hash_t);
    mcd_log_msg( 20302, PLAT_LOG_LEVEL_INFO,
                 "hash table initialized, size=%lu",
                 shard->hash_size * sizeof(mcd_osd_hash_t) );

    shard->overflow_table = shard->hash_table + shard->hash_size;
    if ( NULL == shard->overflow_table ) {
        mcd_log_msg( 20303, PLAT_LOG_LEVEL_ERROR,
                     "failed to alloc overflow table" );
        return FLASH_ENOMEM;
    }
    plat_assert_always(
        0 == ( (uint64_t)shard->overflow_table % sizeof(mcd_osd_hash_t) ) );

    memset( (void *)shard->overflow_table, 0, shard->lock_buckets
            * Mcd_osd_overflow_depth * sizeof(mcd_osd_hash_t) );
    total_alloc +=
        shard->lock_buckets * Mcd_osd_overflow_depth * sizeof(mcd_osd_hash_t);
    mcd_log_msg( 20304, PLAT_LOG_LEVEL_INFO,
                 "overflow table initialized, size=%lu",
                 shard->lock_buckets * Mcd_osd_overflow_depth
                 * sizeof(mcd_osd_hash_t) );

    shard->overflow_index = (uint16_t *)
        plat_alloc_large( shard->lock_buckets * Mcd_osd_overflow_depth
                          * sizeof(uint16_t) );
    if ( NULL == shard->overflow_index ) {
        mcd_log_msg( 20305, PLAT_LOG_LEVEL_ERROR,
                     "failed to alloc overflow index" );
        return FLASH_ENOMEM;
    }
    memset( (void *)shard->overflow_index, 0, shard->lock_buckets
            * Mcd_osd_overflow_depth * sizeof(uint16_t) );
    total_alloc +=
        shard->lock_buckets * Mcd_osd_overflow_depth * sizeof(uint16_t);
    mcd_log_msg( 20306, PLAT_LOG_LEVEL_INFO,
                 "overflow index initialized, size=%lu",
                 shard->lock_buckets * Mcd_osd_overflow_depth
                 * sizeof(uint16_t) );

    /*
     * initialize the class table
     */
    for ( i = 0, j = 0; i <= MCD_OSD_OBJ_MAX_BLKS; i++ ) {
        while ( i > shard->slab_classes[j].slab_blksize ) {
            j++;
            plat_assert_always( MCD_OSD_MAX_NCLASSES > j );
        }
        shard->class_table[i] = j;
    }
    mcd_log_msg( 50044, MCD_OSD_LOG_LVL_INFO,
                 "last entry, i=%d blksize=%d",
                 i - 1, shard->slab_classes[j].slab_blksize );

#ifdef  MCD_ENABLE_SLAB_CACHE
    shard->slab_cache = plat_alloc_large( shard->total_size );
    if ( NULL == shard->slab_cache ) {
        mcd_log_msg( 20345, PLAT_LOG_LEVEL_ERROR,
                     "failed to alloc shard cache" );
        plat_abort();
    }
    total_alloc += shard->total_size;
    mcd_log_msg( 20346, PLAT_LOG_LEVEL_INFO,
                 "shard cache allocated, size=%lu miss_interval=%d",
                 shard->total_size, flash_settings.fake_miss_rate  );
#endif

    mcd_log_msg( 20309, PLAT_LOG_LEVEL_INFO,
                 "shard initialized, total allocated bytes=%lu", total_alloc );
    return 0;   /* SUCCESS */

out_failed:

    // clean up memory held by temporary variables
    if ( segments ) {
        plat_free( segments );
    }
    for ( i = 0; i < 5; i++ ) {
        if ( bitmaps[i] ) {
            plat_free( bitmaps[i] );
        }
    }

    return FLASH_ENOMEM;
}


static int mcd_osd_slab_init()
{
    int                 i;
    uint64_t            mask;

    Mcd_osd_total_size = Mcd_aio_total_size;
    Mcd_osd_total_blks = Mcd_osd_total_size / Mcd_osd_blk_size;
    Mcd_osd_free_blks  = Mcd_osd_total_blks;
    mcd_log_msg( 20347, PLAT_LOG_LEVEL_INFO,
                 "ENTERING, total=%lu blks=%lu free=%lu",
                 Mcd_osd_total_size, Mcd_osd_total_blks, Mcd_osd_free_blks );

    plat_assert_always( 8 == sizeof(mcd_osd_hash_t) );

    if ( MCD_OSD_MAX_SSDSIZE < Mcd_osd_total_size ) {
        mcd_log_msg( 20348, PLAT_LOG_LEVEL_FATAL,
                     "maximum storage size supported is 2TB" );
        plat_abort();
    }

    Mcd_osd_rand_blksize = Mcd_osd_segment_blks / Mcd_aio_num_files;
    mcd_log_msg( 20349, PLAT_LOG_LEVEL_INFO, "rand_blksize set to %d",
                 Mcd_osd_rand_blksize );

    /*
     * set AIO strip size, verify that number of aio files is power of 2
     */
    if ( 0 >= Mcd_aio_num_files  ||
         0 != ( Mcd_aio_num_files & (Mcd_aio_num_files - 1) ) ) {
        mcd_log_msg( 20350, PLAT_LOG_LEVEL_FATAL,
                     "number of ssd files needs to be power of 2" );
        plat_abort();
    }
    Mcd_aio_strip_size = Mcd_osd_segment_size / Mcd_aio_num_files;
    mcd_log_msg( 20351, PLAT_LOG_LEVEL_INFO, "aio strip size set to %lu",
                 Mcd_aio_strip_size );

    /*
     * initialize the scan table and masks
     */
    memset( (void *)Mcd_osd_scan_table, 0xff,
            MCD_OSD_SCAN_TBSIZE * sizeof(uint8_t) );

    mask = 1;
    for ( i = 0; i < 64; i++ ) {
        Mcd_osd_bitmap_masks[i] = mask;
        if ( 0xff != Mcd_osd_scan_table[mask % MCD_OSD_SCAN_TBSIZE] ) {
            mcd_log_msg( 20352, PLAT_LOG_LEVEL_INFO,
                         "collision detected, needs a larger table" );
            plat_abort();
        }
        Mcd_osd_scan_table[mask % MCD_OSD_SCAN_TBSIZE] = i;
        mcd_log_msg( 20353, PLAT_LOG_LEVEL_DEBUG,
                     "mask=%.16lx index=%lu value=%u",
                     mask, mask % MCD_OSD_SCAN_TBSIZE, i );
        mask = mask << 1;
    }

    mask = 1;
    for ( i = 0; i < 64; i++ ) {
        Mcd_osd_scan_masks[i] = mask;
        mask = (mask << 1) + 1;
    }

    /*
     * initialize the free segment list
     * FIXME: need to integrate with recovery code
     */
    for ( i = 0; i < Mcd_osd_total_size / Mcd_osd_segment_size; i++ ) {
        Mcd_osd_free_segments[i] =
            ( Mcd_osd_total_size / Mcd_osd_segment_size - i - 1 )
            * Mcd_osd_segment_blks;
    }
    Mcd_osd_free_seg_curr = i;
    mcd_log_msg( 150028, PLAT_LOG_LEVEL_INFO,
                 "segment free list set up, %d segments, %lu bytes/segment", i, Mcd_osd_segment_size );

#ifdef  USING_SDF_SHIM
    mcd_osd_slab_shard_init( &Mcd_osd_slab_shard,
                             12345,                             // shard_id
                             FLASH_SHARD_INIT_EVICTION_CACHE,   // flags
                             0,                                 // quota
                             1024 * 1048576 );                  // max_nobjs
#endif

    return 0;   /* SUCCESS */
}


/*
 * try to allocate a new segment for this slab class
 */
static inline int
mcd_fth_osd_grow_class( mcd_osd_shard_t * shard, mcd_osd_slab_class_t * class )
{
    uint64_t                    blk_offset;
    uint32_t                    num_segments;
    mcd_osd_segment_t         * segment;
    fthWaitEl_t               * wait = NULL;

    // serialize updates to persistent class segment tables
    if ( 1 == shard->persistent ) {
        wait = fthLock( &class->lock, 1, NULL );
        if ( class->num_segments > 0 ) {
            segment = class->segments[class->num_segments - 1];
            if ( segment->next_slab < class->slabs_per_segment ) {
                fthUnlock( wait );
                return 0;
            }
        }
    }

    blk_offset =
        __sync_fetch_and_add( &shard->blk_allocated, Mcd_osd_segment_blks );

    if ( blk_offset + Mcd_osd_segment_blks > shard->total_blks ) {
        /*
         * sorry out of free segments
         */
        (void ) __sync_fetch_and_sub( &shard->blk_allocated,
                                      Mcd_osd_segment_blks );
        if ( 1 == shard->persistent ) {
            fthUnlock( wait );
        }
        mcd_log_msg( 20355, PLAT_LOG_LEVEL_DEBUG, "out of free segments" );
        return -1;
    }

    num_segments = __sync_fetch_and_add( &class->num_segments, 1 );

    segment = &shard->base_segments[blk_offset / Mcd_osd_segment_blks];

    segment->blk_offset = blk_offset;
    segment->class = class;

    (void) __sync_fetch_and_add( &class->total_slabs,
                                 class->slabs_per_segment );

    shard->segment_table[blk_offset / Mcd_osd_segment_blks] = segment;

    class->segments[num_segments] = segment;

    if ( 1 == shard->persistent ) {
        int rc = update_class( shard, class, num_segments );
        fthUnlock( wait );
        if ( 0 != rc ) {
            mcd_log_msg( 20356, PLAT_LOG_LEVEL_ERROR, "error updating class" );
            return rc;
        }
    }

    mcd_log_msg( 20357, MCD_OSD_LOG_LVL_INFO,
                 "segment %d allocated for shard %lu class %ld, "
                 "blk_offset=%lu used_slabs=%lu total_slabs=%lu",
                 num_segments, shard->id, class - shard->slab_classes,
                 blk_offset, class->used_slabs, class->total_slabs );

    return 0;           /* SUCCESS */
}


static inline void
mcd_osd_slab_evage_update( void * context, mcd_osd_shard_t * shard,
                           mcd_osd_hash_t * hash_entry, uint64_t evictions )
{
    int                         rc;
    char                      * buf;
    char                      * data_buf = NULL;
    uint64_t                    tmp_offset;
    uint64_t                    blk_offset;
    uint64_t                    offset;
    uint64_t                    ev_age;
    mcd_osd_meta_t            * meta;

    data_buf = ((osd_state_t *)context)->osd_buf;
    buf = (char *)( ( (uint64_t)data_buf + Mcd_osd_blk_size - 1 )
                    & Mcd_osd_blk_mask );

    blk_offset = hash_entry->address;

    tmp_offset = shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
        + (blk_offset % Mcd_osd_rand_blksize);
    offset = tmp_offset * Mcd_osd_blk_size;

    rc = mcd_fth_aio_blk_read( context,
                               buf,
                               offset,
                               Mcd_osd_blk_size );
    if ( FLASH_EOK != rc ) {
        mcd_log_msg( 20003, PLAT_LOG_LEVEL_ERROR,
                    "failed to read blocks, rc=%d", rc );
        return;
    }

    meta = (mcd_osd_meta_t *)buf;
    if ( MCD_OSD_MAGIC_NUMBER != meta->magic ) {
        mcd_log_msg( 20325, PLAT_LOG_LEVEL_ERROR,
                    "not enough magic!" );
        return;
    }

    if ( meta->create_time > (*(flash_settings.pcurrent_time)) ) {
        return;
    }
    else if ( meta->create_time == (*(flash_settings.pcurrent_time)) ) {
        ev_age = 1;
    }
    else {
        ev_age = (*(flash_settings.pcurrent_time)) - meta->create_time;
    }

    shard->slab.eviction_ages[ ( evictions / MCD_OSD_EVAGE_FEQUENCY ) %
                               MCD_OSD_EVAGE_SAMPLES ] = ev_age;
}


void mcd_osd_eviction_age_stats( mcd_osd_shard_t *shard, char ** ppos, int * lenp )
{
    int                         i, n;
    uint64_t                    total = 0;

    if ( shard->use_fifo ) {
        return;
    }

    for ( i = 0, n = 0; i < MCD_OSD_EVAGE_SAMPLES; i++ ) {
        if ( 0 == shard->slab.eviction_ages[i] ) {
            continue;
        }
        total += shard->slab.eviction_ages[i];
        n++;
    }

    if ( n ) {
        plat_snprintfcat( ppos, lenp, "STAT flash_avg_eviction_age %lu\r\n",
                          total / n );
    }

    /*
     * FIXME
     */
    plat_snprintfcat( ppos, lenp, "STAT flash_get_size_overrides %lu\r\n",
                      shard->get_size_overrides );
}


static inline int
mcd_fth_osd_get_slab( void * context, mcd_osd_shard_t * shard,
                      mcd_osd_slab_class_t * class, int blocks,
                      uint64_t * blk_offset )
{
    int                         i;
    uint32_t                    hand;
    uint32_t                    slabs_per_pth;
    int                         map_offset;
    int                         tmp_offset;
    uint64_t                    map_value;
    uint64_t                    tmp_value;
    uint64_t                    temp;
    uint64_t                    evictions;
    uint32_t                    hash_index;
    mcd_osd_segment_t         * segment;
    mcd_osd_hash_t            * hash_entry;
    mcd_osd_hash_t            * bucket_head;
    mcd_osd_bucket_t          * bucket;
    osd_state_t               * osd_state = (osd_state_t *)context;
    mcd_logrec_object_t         log_rec;

    while ( 0 < class->free_slab_curr[Mcd_pthread_id] ) {

        *blk_offset = *( class->free_slabs[Mcd_pthread_id] +
                         (--class->free_slab_curr[Mcd_pthread_id]) );

        segment =
            shard->segment_table[*blk_offset / Mcd_osd_segment_blks];

        map_offset =
            (*blk_offset - segment->blk_offset) / class->slab_blksize;
        map_value =
            __sync_fetch_and_or( &segment->bitmap[map_offset / 64],
                                 Mcd_osd_bitmap_masks[map_offset % 64] );

        if ( 0 != ( map_value & Mcd_osd_bitmap_masks[map_offset % 64] ) ) {
            (void) __sync_fetch_and_add( &shard->num_stolen_slabs, 1 );
            continue;
        }

        (void) __sync_fetch_and_add( &segment->class->used_slabs, 1 );

        return 0;       /* SUCCESS */
    }

    /*
     * currently not much we can do if the class is empty
     */
    if ( 0 == class->num_segments ) {
        return FLASH_ENOSPC;
    }

    /*
     * FIXME: use the whole map if map size is small enough?
     */
    slabs_per_pth = class->slabs_per_segment / (flash_settings.num_sdf_threads*flash_settings.num_cores);

    /*
     * if the slab class is not nearly full, scan the bitmaps
     * FIXME: be more aggressive if store mode?
     * FIXME: add scan distance check and stats
     */
    if ( 1 >= slabs_per_pth  ) { /* FIXME_8MB */

        plat_assert_always( sizeof(uint64_t) * 8 >= class->slabs_per_segment );
        hand = class->scan_hand[0];

        for ( i = 0; i < class->num_segments; i++ ) {

            segment = class->segments[hand % class->num_segments];
            if ( NULL == segment ) {
                continue;
            }

            map_value = segment->bitmap[0];
            if ( 0 != ~map_value ) {

                temp = (map_value + 1) & ~map_value;
                tmp_offset = Mcd_osd_scan_table[temp % MCD_OSD_SCAN_TBSIZE];

                if ( tmp_offset < class->slabs_per_segment ) {

                    tmp_value = __sync_fetch_and_or(
                        &segment->bitmap[0], Mcd_osd_bitmap_masks[tmp_offset]);

                    if ( 0 == ( tmp_value &
                                Mcd_osd_bitmap_masks[tmp_offset % 64] ) ) {
                        /*
                         * ok we found an available slab
                         */
                        *blk_offset = segment->blk_offset +
                            tmp_offset * class->slab_blksize;
                        (void) __sync_fetch_and_add(
                            &segment->class->used_slabs, 1 );
                        return 0;       /* SUCCESS */
                    }
                }
            }

            hand = __sync_add_and_fetch( &class->scan_hand[0], 1 );
        }
    }
    else if ( class->used_slabs + class->total_slabs / MCD_OSD_SCAN_THRESHOLD
              < class->total_slabs ) {

        hand = class->scan_hand[Mcd_pthread_id];
        for ( i = 0; i < class->num_segments; i++ ) {

            segment =
                class->segments[(hand / slabs_per_pth) % class->num_segments];
            if ( NULL == segment ) {
                continue;
            }
            map_offset = hand % slabs_per_pth + Mcd_pthread_id * slabs_per_pth;

            while ( map_offset < (Mcd_pthread_id + 1) * slabs_per_pth ) {

                map_value = segment->bitmap[map_offset / 64];
                if ( 0 != map_offset % 64 ) {
                    map_value |= Mcd_osd_scan_masks[(map_offset % 64) - 1];
                }

                if ( 0 != ~map_value ) {

                    temp = (map_value + 1) & ~map_value;
                    tmp_offset = (map_offset / 64) * 64
                        + Mcd_osd_scan_table[temp % MCD_OSD_SCAN_TBSIZE];

                    if ( tmp_offset < (Mcd_pthread_id + 1) * slabs_per_pth ) {

                        tmp_value = __sync_fetch_and_or(
                            &segment->bitmap[tmp_offset / 64],
                            Mcd_osd_bitmap_masks[tmp_offset % 64] );

                        if ( 0 == ( tmp_value &
                                    Mcd_osd_bitmap_masks[tmp_offset % 64] ) ) {
                            /*
                             * ok we found an available slab
                             */
                            *blk_offset = segment->blk_offset +
                                tmp_offset * class->slab_blksize;

                            (void) __sync_fetch_and_add(
                                &segment->class->used_slabs, 1 );

                            class->scan_hand[Mcd_pthread_id] =
                                tmp_offset - Mcd_pthread_id * slabs_per_pth
                                + hand - (hand % slabs_per_pth);

                            mcd_log_msg( 20359, MCD_OSD_LOG_LVL_INFO,
                                         "free slab, map_off=%d blk_off=%lu",
                                         tmp_offset, *blk_offset );

                            return 0;       /* SUCCESS */
                        }
                        else {
                            (void) __sync_fetch_and_add(
                                &shard->num_stolen_slabs, 1 );
                        }
                    }
                }

                map_offset += 64 - (map_offset % 64);
            }

            hand += slabs_per_pth - (hand % slabs_per_pth);
        }

        class->scan_hand[Mcd_pthread_id] = hand;
    }

    if ( 0 == shard->evict_to_free ) {
        return FLASH_ENOSPC;
    }

#ifdef  MCD_ENABLE_SLAB_CLOCK
    // return mcd_osd_slab_clock_evict();
#endif

    /*
     * ok, need to evict an object
     */
    uint32_t loop_cnt = 0;
    do {
        if ( 1 >= slabs_per_pth ) { /* FIXME_8MB */
            hand = __sync_fetch_and_add( &(class->clock_hand[0]), 1 );
            segment = class->segments[ ( hand / class->slabs_per_segment )
                                       % class->num_segments];
            map_offset = hand % class->slabs_per_segment;
        }
        else {
            hand = class->clock_hand[Mcd_pthread_id]++;
            segment =
                class->segments[(hand / slabs_per_pth) % class->num_segments];
            map_offset = hand % slabs_per_pth + Mcd_pthread_id * slabs_per_pth;
        }
        if ( NULL == segment ) {
            continue;
        }

        *blk_offset = segment->blk_offset + map_offset * class->slab_blksize;

        /*
         * if slab is not in use, simply grab it
         */
        if ( 0 == ( segment->bitmap[map_offset / 64] &
                    Mcd_osd_bitmap_masks[map_offset % 64] ) ) {

            mcd_log_msg( 20360, PLAT_LOG_LEVEL_DEBUG,
                         "unused slab found, seg_off=%lu map_off=%u",
                         segment->blk_offset, map_offset );

            map_value =
                __sync_fetch_and_or( &segment->bitmap[map_offset / 64],
                                     Mcd_osd_bitmap_masks[map_offset % 64] );

            if ( 0 == ( map_value & Mcd_osd_bitmap_masks[map_offset % 64] ) ) {
                if ( NULL == osd_state->osd_wait ) {
                    osd_state->osd_wait =
                        fthLock( osd_state->osd_lock, 1, NULL );
                }
                (void) __sync_fetch_and_add( &segment->class->used_slabs, 1 );
                return 0;   /* SUCCESS */
            }
            else {
                (void) __sync_fetch_and_add( &shard->num_stolen_slabs, 1 );
            }
        }

        /*
         * slab is occupied, evict the current owner
         */
        hash_index = shard->addr_table[*blk_offset];
        hash_entry = shard->hash_table + hash_index;

        if ( NULL != osd_state->osd_wait ) {
            /*
             * unlock the current bucket before we lock another one
             */
            fthUnlock( osd_state->osd_wait );
        }
        osd_state->osd_wait =
            fthLock( shard->bucket_locks +
                     hash_index / shard->lock_bktsize, 1, NULL );

        bucket = shard->hash_buckets + ( hash_index / Mcd_osd_bucket_size );
        bucket_head = shard->hash_table + ( hash_index & Mcd_osd_bucket_mask );

        if ( hash_entry - bucket_head >= bucket->next_item ||
             0 == hash_entry->used || *blk_offset != hash_entry->address ) {
            /*
             * hash_entry not a valid match, search within the bucket
             */
            hash_entry = bucket_head;
            for ( i = 0; i < bucket->next_item; i++,hash_entry++ ) {
                if ( 0 == hash_entry->used ) {
                    plat_assert_always( 0 == 1 );
                }
                if ( *blk_offset == hash_entry->address ) {
                    break;
                }
            }
            if ( i == bucket->next_item || 0 == hash_entry->used
                 || *blk_offset != hash_entry->address ) {
                (void) __sync_fetch_and_add( &shard->invalid_evictions, 1 );
                fthUnlock( osd_state->osd_wait );
                osd_state->osd_wait = NULL;

                loop_cnt++;
                if ( loop_cnt && 0 == loop_cnt % 128 ) {
                    /*
                     * hit a number of invalid evictions in a row, time to
                     * take precaution against starving others
                     */
                    fthYield( 1 );
                }
                if ( 65536 < loop_cnt && 0 == loop_cnt % 8192 ) {
                    mcd_log_msg( 50045, PLAT_LOG_LEVEL_WARN,
                                 "too many invalid evictions, "
                                 "cnt=%u moff=%d boff=%lu hind=%u",
                                 loop_cnt, map_offset, *blk_offset,
                                 hash_index );
                }
                continue;
            }
        }
        if ( hash_entry->deleted ) {
            /*
             * the object is deleted with an expiry time, do not evict it
             */
            (void) __sync_fetch_and_add( &shard->invalid_evictions, 1 );
            fthUnlock( osd_state->osd_wait );
            osd_state->osd_wait = NULL;

            loop_cnt++;
            if ( loop_cnt && 0 == loop_cnt % 128 ) {
                fthYield( 1 );
            }
            if ( 65536 < loop_cnt && 0 == loop_cnt % 8192 ) {
                mcd_log_msg( 50046, PLAT_LOG_LEVEL_WARN,
                             "too many deleted evictions, cnt=%u", loop_cnt );
            }
            continue;
        }
        mcd_log_msg( 20361, PLAT_LOG_LEVEL_DEBUG,
                     "evicting, pth_id=%d hand=%u address=%u index=%lu",
                     Mcd_pthread_id, hand,
                     hash_entry->address, hash_entry - shard->hash_table );

        // eviction case
        if ( 1 == shard->persistent ) {
            log_rec.syndrome     = hash_entry->syndrome;
            log_rec.deleted      = hash_entry->deleted;
            log_rec.reserved     = 0;
            log_rec.blocks       = 0;  // distinguishes a delete record
            log_rec.bucket       = hash_index;
            log_rec.blk_offset   = hash_entry->address;
            log_rec.old_offset   = 0;
            if ( 1 == shard->replicated ) {
                // Must make RPC call to get a seqno to use for this eviction
                log_rec.seqno = rep_seqno_get((struct shard *)shard);
            } else {
                log_rec.seqno =
                    __sync_add_and_fetch( &shard->sequence, 1 );
            }
            log_rec.target_seqno = 0;
            log_write( shard, &log_rec );
        }

        if ( 1 == shard->replicated ) {
            (void) __sync_fetch_and_add( &class->dealloc_pending, 1 );
            (void) __sync_fetch_and_add( &shard->blk_dealloc_pending,
                                         class->slab_blksize );
        }

        evictions = __sync_fetch_and_add( &shard->num_slab_evictions, 1 );
        if ( 0 == evictions % MCD_OSD_EVAGE_FEQUENCY ) {
            mcd_osd_slab_evage_update( context, shard, hash_entry, evictions );
        }

        /*
         * simple eviction, no update to bitmap and freelist needed
         */
        (void) __sync_fetch_and_sub( &shard->blk_consumed,
                                     mcd_osd_lba_to_blk(hash_entry->blocks) );

        if ( 0 == bucket->next_item ) {
            plat_assert_always( 0 == 1 );
        }
        if ( bucket_head + bucket->next_item - 1 != hash_entry ) {
            *hash_entry = bucket_head[--bucket->next_item];
            *((uint64_t *)&bucket_head[bucket->next_item]) = 0;
        }
        else {
            *((uint64_t *)hash_entry) = 0;
            bucket->next_item--;
        }

        if ( Mcd_osd_bucket_size - 1 == bucket->next_item ) {
            (void) __sync_fetch_and_sub( &shard->num_full_buckets, 1 );
        }
        (void) __sync_fetch_and_sub( &shard->num_objects, 1 );

        fthUnlock( osd_state->osd_wait );

        /*
         * relock the original lock
         */
        osd_state->osd_wait =
            fthLock( osd_state->osd_lock, 1, NULL );
        return 0;       /* SUCCESS */

    } while ( 1 );
}


/*
 * try to allocate a slab for the object
 */
static inline int
mcd_fth_osd_slab_alloc( void * context, mcd_osd_shard_t * shard, int blocks,
                        uint64_t * blk_offset )
{
    int                         map_offset;
    uint64_t                    map_value;
    uint32_t                    next_slab;
    mcd_osd_slab_class_t      * class;
    mcd_osd_segment_t         * segment;

    class = shard->slab_classes + shard->class_table[blocks];

    while ( 0 < class->free_slab_curr[Mcd_pthread_id] ) {

        *blk_offset = *( class->free_slabs[Mcd_pthread_id] +
                         (--class->free_slab_curr[Mcd_pthread_id]) );

        segment =
            shard->segment_table[*blk_offset / Mcd_osd_segment_blks];

        map_offset =
            (*blk_offset - segment->blk_offset) / class->slab_blksize;
        map_value =
            __sync_fetch_and_or( &segment->bitmap[map_offset / 64],
                                 Mcd_osd_bitmap_masks[map_offset % 64] );

        if ( 0 != ( map_value & Mcd_osd_bitmap_masks[map_offset % 64] ) ) {
            (void) __sync_fetch_and_add( &shard->num_stolen_slabs, 1 );
            continue;
        }

        (void) __sync_fetch_and_add( &segment->class->used_slabs, 1 );

        return 0;       /* SUCCESS */
    }

    if ( class->used_slabs >= class->total_slabs ) {
        if ( 0 != mcd_fth_osd_grow_class( shard, class ) ) {
            return mcd_fth_osd_get_slab( context, shard, class, blocks,
                                         blk_offset );
        }
    }

    do {
        /*
         * first check the last unfilled segment in the class
         */
        int      s;
        uint32_t count = __sync_fetch_and_add( &class->num_segments, 0 );

        segment = class->segments[count - 1];

        for ( s = count - 1; s > 0; s-- ) {
            if ( NULL != class->segments[s-1] &&
                 class->slabs_per_segment <= class->segments[s-1]->next_slab ){
                break;
            }
            segment = class->segments[s-1];
        }

        while ( NULL == (segment = class->segments[s]) ) {
            mcd_log_msg( 20362, PLAT_LOG_LEVEL_DIAGNOSTIC,
                         "segment not ready yet" );
            fthYield( 1 );
        }

        if ( class->slabs_per_segment <= segment->next_slab ) {

            if ( class->segments[class->num_segments - 1] != segment ) {
                continue;
            }

            if ( class->total_slabs / 2 > class->used_slabs ) {
                /*
                 * if the class is under-utilized, allocate from existing
                 * segments before trying to grow the class
                 */
                if ( 0 == mcd_fth_osd_get_slab( context, shard, class, blocks,
                                                blk_offset ) ) {
                    return 0;   /* SUCCESS */
                }
            }

            if ( 0 != mcd_fth_osd_grow_class( shard, class ) ) {
                return mcd_fth_osd_get_slab( context, shard, class, blocks,
                                             blk_offset );
            }
            continue;
        }

        next_slab = __sync_fetch_and_add( &segment->next_slab, 1 );

        if ( next_slab < class->slabs_per_segment ) {
            (void) __sync_fetch_and_add( &class->used_slabs, 1 );
            break;
        }

        /*
         * oops, segment just became full
         */
        (void) __sync_fetch_and_sub( &segment->next_slab, 1 );
        mcd_log_msg( 20363, PLAT_LOG_LEVEL_DIAGNOSTIC, "race detected" );

        if ( class->segments[class->num_segments - 1] != segment ) {
            continue;
        }

        if ( 0 != mcd_fth_osd_grow_class( shard, class ) ) {
            return mcd_fth_osd_get_slab( context, shard, class, blocks,
                                         blk_offset );
        }

    } while ( 1 );

    if ( Mcd_aio_strip_size / Mcd_osd_blk_size >= class->slab_blksize ) {
        *blk_offset = segment->blk_offset + (next_slab % Mcd_aio_num_files)
            * Mcd_aio_strip_size / Mcd_osd_blk_size
            + (next_slab / Mcd_aio_num_files) * class->slab_blksize;
    }
    else {
        /*
         * for very large objects, take a simpler approach
         */
        *blk_offset = segment->blk_offset + next_slab * class->slab_blksize;
    }

    /*
     * update the segment slab bitmap
     */
    map_offset = (*blk_offset - segment->blk_offset) / class->slab_blksize;

    (void) __sync_fetch_and_or( &segment->bitmap[map_offset / 64],
                                Mcd_osd_bitmap_masks[map_offset % 64] );

    return 0;   /* SUCCESS */
}


/*
 * remove an entry from the hash table, free up its slab and update
 * the bitmap as well as the free list
 */
static inline int
mcd_fth_osd_remove_entry( mcd_osd_shard_t * shard,
                          mcd_osd_hash_t * hash_entry )
{
    int                 map_offset;
    int                 free_slab_curr;
    mcd_osd_segment_t * segment;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

    if ( 0 == hash_entry->used || 0 == hash_entry->blocks ) {
        plat_assert_always( 0 == 1 );
    }

    segment =
        shard->segment_table[hash_entry->address / Mcd_osd_segment_blks];

    map_offset = hash_entry->address - segment->blk_offset;
    map_offset /= segment->class->slab_blksize;

    (void) __sync_fetch_and_and( &segment->bitmap[map_offset / 64],
                                 ~Mcd_osd_bitmap_masks[map_offset % 64] );

    free_slab_curr = segment->class->free_slab_curr[Mcd_pthread_id];
    if ( free_slab_curr < segment->class->freelist_len ) {
        segment->class->free_slabs[Mcd_pthread_id][free_slab_curr] =
            hash_entry->address;
        segment->class->free_slab_curr[Mcd_pthread_id]++;
    }

    (void) __sync_fetch_and_sub( &segment->class->used_slabs, 1 );
    (void) __sync_fetch_and_sub( &shard->blk_consumed,
                                 mcd_osd_lba_to_blk(hash_entry->blocks) );

    mcd_log_msg( 20364, MCD_OSD_LOG_LVL_DEBUG,
                 "cls=%ld addr=%u off=%d map=%.16lx curr=%d slabs=%lu tid=%d",
                 segment->class - shard->slab_classes,
                 hash_entry->address,
                 map_offset,
                 segment->bitmap[map_offset / 64],
                 segment->class->free_slab_curr[Mcd_pthread_id],
                 segment->class->used_slabs,
                 Mcd_pthread_id );

    *((uint64_t *)hash_entry) = 0;

    return 0;   /* SUCCESS */
}

/*
 * Free slab for deallocated blocks, update bitmap. Called only when
 * object is overwritten in store mode to free space for old object.
 */
void
mcd_fth_osd_slab_dealloc( mcd_osd_shard_t * shard, uint32_t address )
{
    int                 map_offset;
    mcd_osd_segment_t * segment;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

    segment = shard->segment_table[address / Mcd_osd_segment_blks];

    map_offset = address - segment->blk_offset;
    map_offset /= segment->class->slab_blksize;

    (void) __sync_fetch_and_and( &segment->bitmap[map_offset / 64],
                                 ~Mcd_osd_bitmap_masks[map_offset % 64] );

    (void) __sync_fetch_and_sub( &segment->class->used_slabs, 1 );
    (void) __sync_fetch_and_sub( &segment->class->dealloc_pending, 1 );
    (void) __sync_fetch_and_sub( &shard->blk_dealloc_pending,
                                 segment->class->slab_blksize );

    mcd_log_msg( 20365, MCD_OSD_LOG_LVL_DEBUG,
                 "cls=%ld addr=%u off=%d map=%.16lx slabs=%lu, pend=%lu",
                 segment->class - shard->slab_classes,
                 address,
                 map_offset,
                 segment->bitmap[map_offset / 64],
                 segment->class->used_slabs,
                 segment->class->dealloc_pending );

    return;
}


static int mcd_osd_prefix_delete( mcd_osd_shard_t * shard, char * key,
                                  int key_len )
{
    SDF_status_t   status;

    mcd_log_msg( 50047, MCD_OSD_LOG_LVL_INFO,
                 "ENTERING key=%s len=%d", key, key_len );

    status = (*flash_settings.prefix_delete_callback)( key, key_len, shard->cntr);

    if ( SDF_SUCCESS != status ) {
        mcd_log_msg( 50048, PLAT_LOG_LEVEL_ERROR,
                     "prefix-based deletion failed, prefix=%s status=%s",
                     key, SDF_Status_Strings[status] );
        return FLASH_ENOMEM;
    }

    return FLASH_EOK;
}

static int
mcd_fth_osd_slab_set( void * context, mcd_osd_shard_t * shard, char * key,
                      int key_len, void * data, SDF_size_t data_len, int flags,
                      struct objMetaData * meta_data, uint64_t syndrome )
{
    int                         i, n, rc;
    bool                        obj_exists = false;
    bool                        dyn_buffer = false;
    SDF_size_t                  raw_len;
    int                         blocks;
    char                      * buf;
    char                      * data_buf = NULL;
    uint32_t                    chksum32 = 0;
    uint64_t                    chksum64 = 0;
    uint64_t                    tmp_offset;
    uint64_t                    blk_offset;
    uint64_t                    offset;
    uint64_t                    target_seqno = 0;
    uint64_t                    num_puts;
    uint16_t                  * index;
    mcd_osd_meta_t            * meta;
    mcd_osd_hash_t              new_entry;
    mcd_osd_hash_t            * hash_entry;
    mcd_osd_hash_t            * bucket_head;
    mcd_osd_bucket_t          * bucket;
    mcd_osd_slab_class_t      * class;
    mcd_logrec_object_t         log_rec;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

    // don't allow normal flash puts during restore
    if ( 1 == shard->restore_running &&
         0 == (flags & FLASH_PUT_RESTORE) ) {
        return FLASH_EACCES;
    }

    if ( FLASH_PUT_PREFIX_DELETE == flags ) {
        return mcd_osd_prefix_delete( shard, key, key_len );
    }

    num_puts = __sync_add_and_fetch( &shard->num_puts, 1 );

    // write the current cas_id to the log at regular intervals
    if ( 1 == shard->persistent &&
         0 == (num_puts % MCD_OSD_CAS_UPDATE_INTERVAL) ) {
        log_rec.syndrome     = 0;
        log_rec.deleted      = 0;
        log_rec.reserved     = 0;
        log_rec.blocks       = 0;
        log_rec.bucket       = 0;
        log_rec.blk_offset   = 0xffffffffu; // marks this record
        log_rec.old_offset   = 0;
        log_rec.seqno        = 0;
        log_rec.target_seqno = shard->cntr->cas_id;
        log_write( shard, &log_rec );
    }

#ifdef  MCD_OSD_DEBUGGING
    (void) __sync_fetch_and_add( &Mcd_osd_set_cmds, 1 );
    if ( 0 == Mcd_osd_set_cmds % MCD_OSD_DBG_SET_DIV ) {
        mcd_log_msg( 20329, PLAT_LOG_LEVEL_INFO,
                     "%lu sets, b_alloc=%lu overwrites=%lu evictions=%lu "
                     "soft_of=%lu hard_of=%lu p_id=%d ",
                     Mcd_osd_set_cmds,
                     shard->blk_allocated,
                     shard->num_overwrites,
                     shard->num_hash_evictions,
                     shard->num_soft_overflows,
                     shard->num_hard_overflows,
                     Mcd_pthread_id );
    }
#endif

    raw_len = sizeof(mcd_osd_meta_t) + key_len + data_len;
    blocks = ( raw_len + ( Mcd_osd_blk_size - 1 ) ) / Mcd_osd_blk_size;

    if ( MCD_OSD_OBJ_MAX_BLKS <= blocks ) {
        mcd_log_msg( 20330, PLAT_LOG_LEVEL_ERROR,
                     "object size beyond limit, raw_len=%lu", raw_len );
        return FLASH_EINVAL;
    }

    /*
     * allocate the buffer now since we may need it for reading the key
     */
    data_buf = ((osd_state_t *)context)->osd_buf;
    buf = (char *)( ( (uint64_t)data_buf + Mcd_osd_blk_size - 1 )
                    & Mcd_osd_blk_mask );

    bucket = shard->hash_buckets +
        ( ( syndrome % shard->hash_size ) / Mcd_osd_bucket_size );
    bucket_head = shard->hash_table +
        ( ( syndrome % shard->hash_size ) & Mcd_osd_bucket_mask );
    hash_entry = bucket_head;

    /*
     * check whether the object already exists
     */
    int overflow = 0;
    int end = bucket->next_item;

    for ( i = 0; i <= end; i++,hash_entry++ ) {

        if ( 0 == overflow && i == bucket->next_item ) {
            if ( 0 == bucket->overflowed ) {
                break;
            }
            i = -1, end = Mcd_osd_overflow_depth - 1;
            hash_entry = shard->overflow_table +
                ( ( syndrome % shard->hash_size ) / shard->lock_bktsize )
                * Mcd_osd_overflow_depth - 1;
            overflow = 1;
            continue;
        }

        if ( 0 == overflow ) {
            if ( 0 == hash_entry->used ) {
                plat_assert_always( 0 == 1 );
            }
        }
        else {
            if ( 0 == hash_entry->used ) {
                continue;
            }
        }

        if ( (uint16_t)(syndrome >> 48) == hash_entry->syndrome ) {
            /*
             * syndrome collision, read the key
             */
            blk_offset = hash_entry->address;

            tmp_offset = shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
                + (blk_offset % Mcd_osd_rand_blksize);
            offset = tmp_offset * Mcd_osd_blk_size;

            rc = mcd_fth_aio_blk_read( context,
                                       buf,
                                       offset,
                                       Mcd_osd_blk_size );
            if ( FLASH_EOK != rc ) {
                mcd_log_msg( 20003, PLAT_LOG_LEVEL_ERROR,
                             "failed to read blocks, rc=%d", rc );
                goto out;
            }

            meta = (mcd_osd_meta_t *)buf;
            mcd_log_msg( 20366, PLAT_LOG_LEVEL_DEBUG,
                         "syndrome collision, key_len=%d data_len=%u",
                         meta->key_len, meta->data_len );

            if ( MCD_OSD_MAGIC_NUMBER != meta->magic ) {
                mcd_log_msg( 20325, PLAT_LOG_LEVEL_FATAL,
                             "not enough magic!" );
                continue;
            }

            if ( key_len != meta->key_len ) {
                mcd_log_msg( 20326, MCD_OSD_LOG_LVL_DIAG,
                             "key length mismatch, req %d osd %d",
                             key_len, meta->key_len );
                (void) __sync_fetch_and_add( &shard->set_hash_collisions, 1 );
                continue;
            }
            if ( 0 != strncmp( buf + sizeof(mcd_osd_meta_t), key, key_len ) ) {
                mcd_log_msg( 20006, MCD_OSD_LOG_LVL_DIAG,
                             "key mismatch, req %s", key );
                (void) __sync_fetch_and_add( &shard->set_hash_collisions, 1 );
                continue;
            }

            target_seqno = meta->seqno;
            obj_exists = true;
            mcd_log_msg( 20331, PLAT_LOG_LEVEL_DEBUG, "object exists" );

            if ( FLASH_PUT_TEST_EXIST & flags ) {
                rc = FLASH_EEXIST;
                goto out;
            }

            /*
             * complete this put only if the sequence number for the put
             * is newer than the existing object's sequence number
             */
            if ( ( FLASH_PUT_IF_NEWER & flags ) &&
                 meta->seqno >= meta_data->sequence ) {
                rc = FLASH_EEXIST;
                goto out;
            }

            /*
             * FIXME: write in place if possible?
             */
            if ( NULL == data ) {

                if ( ( FLASH_PUT_DEL_EXPIRED & flags ) &&
                     ( 0 == meta->expiry_time ||
                       meta->expiry_time > (*(flash_settings.pcurrent_time)) ) ) {
                    rc = FLASH_ENOENT;
                    goto out;
                }

                if ( ( FLASH_PUT_PREFIX_DO_DEL == flags ) &&
                     meta_data->createTime != meta->create_time ) {
                    rc = FLASH_ENOENT;
                    goto out;
                }

                // explicit delete case
                if ( 1 == shard->persistent ) {
                    log_rec.syndrome     = hash_entry->syndrome;
                    log_rec.deleted      = hash_entry->deleted;
                    log_rec.reserved     = 0;
                    log_rec.blocks       = 0;  // distinguishes a delete record
                    log_rec.bucket       = syndrome % shard->hash_size;
                    log_rec.blk_offset   = hash_entry->address;
                    if ( 0 == shard->evict_to_free ) {
                        // store mode: delay flash space dealloc
                        log_rec.old_offset = ~(hash_entry->address);
                    } else {
                        log_rec.old_offset = 0;
                    }
                    log_rec.seqno        = meta_data->sequence;
                    log_rec.target_seqno = target_seqno;
                    log_write( shard, &log_rec );
                }

                if ( 1 == shard->replicated ||
                     (1 == shard->persistent && 0 == shard->evict_to_free) ) {
                    class = shard->slab_classes + shard->class_table
                        [mcd_osd_lba_to_blk(hash_entry->blocks)];
                    (void) __sync_fetch_and_add( &class->dealloc_pending, 1 );
                    (void) __sync_fetch_and_add( &shard->blk_dealloc_pending,
                                                 class->slab_blksize );
                    (void) __sync_fetch_and_sub(
                        &shard->blk_consumed,
                        mcd_osd_lba_to_blk(hash_entry->blocks) );
                    *((uint64_t *)hash_entry) = 0;
                } else {
                    mcd_fth_osd_remove_entry( shard, hash_entry );
                }

                if ( 0 == overflow ) {
                    if ( 0 == bucket->next_item ) {
                        plat_assert_always( 0 == 1 );
                    }
                    if ( bucket_head + bucket->next_item - 1 != hash_entry ) {
                        *hash_entry = bucket_head[--bucket->next_item];
                        *((uint64_t *)&bucket_head[bucket->next_item]) = 0;
                    }
                    else {
                        bucket->next_item--;
                    }
                }

                if ( bucket->overflowed ) {

                    index = shard->overflow_index +
                        ( ( syndrome % shard->hash_size )
                          / shard->lock_bktsize ) * Mcd_osd_overflow_depth;

                    hash_entry = shard->overflow_table +
                        ( ( syndrome % shard->hash_size )
                          / shard->lock_bktsize ) * Mcd_osd_overflow_depth;

                    if ( 0 == overflow ) {
                        /*
                         * bring one item back from the overflow area
                         */
                        for ( i = 0;
                              i < Mcd_osd_overflow_depth; i++,hash_entry++ ) {
                            if ( 0 == hash_entry->used ) {
                                continue;
                            }
                            if ( (bucket - shard->hash_buckets )
                                 % shard->lock_bktsize != index[i] ) {
                                continue;
                            }
                            bucket_head[bucket->next_item++] = *hash_entry;
                            *((uint64_t *)hash_entry) = 0;
                            break;
                        }
                    }

                    /*
                     * verify whether the bucket is still overflowed
                     */
                    bucket->overflowed = 0;
                    for ( i = ( 0 == overflow ) ? i : 0;
                          i < Mcd_osd_overflow_depth; i++,hash_entry++ ) {
                        if ( 0 == hash_entry->used ) {
                            continue;
                        }
                        if ( (bucket - shard->hash_buckets )
                             % shard->lock_bktsize != index[i] ) {
                            continue;
                        }
                        bucket->overflowed = 1;
                        break;
                    }
                }

                if ( Mcd_osd_bucket_size - 1 == bucket->next_item ) {
                    (void) __sync_fetch_and_sub( &shard->num_full_buckets, 1 );
                }
                (void) __sync_fetch_and_sub( &shard->num_objects, 1 );
                (void) __sync_fetch_and_add( &shard->num_deletes, 1 );

                rc = FLASH_EOK;
                goto out;
            }

            break;
        }
    }

    if ( NULL == data ) {
        rc = FLASH_ENOENT;
        goto out;
    }

    if ( false == obj_exists ) {
        if ( FLASH_PUT_TEST_NONEXIST & flags ) {
            rc = FLASH_ENOENT;
            goto out;
        }
        if ( Mcd_osd_bucket_size == bucket->next_item ) {
            hash_entry = bucket_head;
            for ( n = 0; n < Mcd_osd_bucket_size; n++, hash_entry++ ) {
                if ( 0 == hash_entry->deleted ) {
                    break;
                }
            }
            if ( Mcd_osd_bucket_size == n ) {
                rc = FLASH_ENOSPC;
                goto out;
            }
        }
    }
    else {
        if ( shard->evict_to_free ) {
            /*
             * remove the hash entry first as we may lose control over the
             * hash bucket due to eviction
             */
            // overwrite case in cache mode
            if ( 1 == shard->persistent ) {
                log_rec.syndrome     = hash_entry->syndrome;
                log_rec.deleted      = hash_entry->deleted;
                log_rec.reserved     = 0;
                log_rec.blocks       = 0;  // distinguishes a delete record
                log_rec.bucket       = syndrome % shard->hash_size;
                log_rec.blk_offset   = hash_entry->address;
                log_rec.old_offset   = 0;
                if ( 1 == shard->replicated ) {
                    log_rec.seqno = meta_data->sequence;
                } else {
                    log_rec.seqno =
                        __sync_add_and_fetch( &shard->sequence, 1 );
                }
                log_rec.target_seqno = target_seqno;
                log_write( shard, &log_rec );
            }

            if ( 1 == shard->replicated ) {
                class = shard->slab_classes +
                    shard->class_table[mcd_osd_lba_to_blk(hash_entry->blocks)];
                (void) __sync_fetch_and_add( &class->dealloc_pending, 1 );
                (void) __sync_fetch_and_add( &shard->blk_dealloc_pending,
                                             class->slab_blksize );
                (void) __sync_fetch_and_sub(
                    &shard->blk_consumed,
                    mcd_osd_lba_to_blk(hash_entry->blocks) );
                *((uint64_t *)hash_entry) = 0;
            } else {
                mcd_fth_osd_remove_entry( shard, hash_entry );
            }

            if ( 0 == bucket->next_item ) {
                plat_assert_always( 0 == 1 );
            }
            if ( bucket_head + bucket->next_item - 1 != hash_entry ) {
                *hash_entry = bucket_head[--bucket->next_item];
                *((uint64_t *)&bucket_head[bucket->next_item]) = 0;
            }
            else {
                bucket->next_item--;
            }

            if ( Mcd_osd_bucket_size - 1 == bucket->next_item ) {
                (void) __sync_fetch_and_sub( &shard->num_full_buckets, 1 );
            }
            (void) __sync_fetch_and_sub( &shard->num_objects, 1 );
            (void) __sync_fetch_and_add( &shard->num_overwrites, 1 );
        }
    }

    /*
     * re-alloc the buffer for large objects
     */
    if ( MCD_OSD_MAX_BLKS_OLD < blocks ) {
        buf = mcd_fth_osd_iobuf_alloc( blocks * Mcd_osd_blk_size, false );
        if ( NULL == buf ) {
            rc = FLASH_ENOMEM;
            goto out;
        }
        dyn_buffer = true;
    }

    if ( 0 != mcd_fth_osd_slab_alloc( context, shard, blocks, &blk_offset ) ) {
        mcd_log_msg( 20367, PLAT_LOG_LEVEL_DEBUG, "failed to allocate slab" );
        rc = FLASH_ENOSPC;
        goto out;
    }
    mcd_log_msg( 20368, PLAT_LOG_LEVEL_DEBUG,
                 "blocks allocated, key_len=%d data_len=%lu "
                 "blk_offset=%lu blocks=%d",
                 key_len, data_len, blk_offset, blocks );

    plat_assert
        ( shard->segment_table[ blk_offset /
                                Mcd_osd_segment_blks ]->class->slab_blksize >=
          mcd_osd_lba_to_blk( mcd_osd_blk_to_lba( blocks ) ) );

    /*
     * copy the data over to the aligned buffer
     * FIXME: this could be optimized away
     */
    meta = (mcd_osd_meta_t *)buf;
    meta->magic    = MCD_OSD_META_MAGIC;
    meta->version  = MCD_OSD_META_VERSION;
    meta->key_len  = key_len;
    meta->data_len = data_len;

    if ( NULL != meta_data ) {
        meta->create_time = meta_data->createTime;
        meta->expiry_time = meta_data->expTime;
        meta->seqno       = meta_data->sequence;
    }

    memcpy( buf + sizeof(mcd_osd_meta_t), key, key_len );
    memcpy( buf + sizeof(mcd_osd_meta_t) + key_len, data, data_len );

    meta->blk1_chksum = 0;
    meta->checksum    = 0;

    if ( flash_settings.chksum_metadata ) {
        chksum32 = mcd_hash( (unsigned char *)buf, Mcd_osd_blk_size, 0 );
    }
    if ( flash_settings.chksum_data ) {
        chksum64 = hash( (unsigned char *)buf, raw_len, 0 );
    }

    meta->blk1_chksum = chksum32;
    meta->checksum    = chksum64;

    tmp_offset = shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
        + ( blk_offset % Mcd_osd_rand_blksize );
    offset = tmp_offset * Mcd_osd_blk_size;

#ifndef MCD_ENABLE_SLAB_CACHE_NOSSD
    /*
     * FIXME: pad the object if size < 128KB
     */
    if ( blocks < 256 ) {
        rc = mcd_fth_aio_blk_write( context,
                                    buf,
                                    offset,
                                    (1 << shard->class_table[blocks]) *
                                    Mcd_osd_blk_size );
    }
    else {
        rc = mcd_fth_aio_blk_write( context,
                                    buf,
                                    offset,
                                    blocks * Mcd_osd_blk_size );
    }
    if ( FLASH_EOK != rc ) {
        /*
         * FIXME: free the allocated space here?
         */
        mcd_log_msg( 20008, PLAT_LOG_LEVEL_ERROR,
                     "failed to write blocks, rc=%d", rc );
        goto out;
    }
#endif

#ifdef  MCD_ENABLE_SLAB_CACHE
    /*
     * copy the data into the slab cache (for testing only)
     */
    memcpy( shard->slab_cache + offset, buf, blocks * Mcd_osd_blk_size );
    rc = FLASH_EOK;
#endif

    plat_log_msg( 20369, PLAT_LOG_CAT_SDF_SIMPLE_REPLICATION,
                  PLAT_LOG_LEVEL_DEBUG,
                  "store object [%ld][%d]: syndrome: %lx",
                  ( syndrome % shard->hash_size ) / Mcd_osd_bucket_size, i,
                  syndrome );

    /*
     * update the hash table entry
     */
    new_entry.used       = 1;
    new_entry.referenced = 1;
    new_entry.blocks     = mcd_osd_blk_to_lba( blocks );
    new_entry.syndrome   = (uint16_t)(syndrome >> 48);
    new_entry.address    = blk_offset;

    new_entry.deleted    = 0;
    if ( &Mcd_osd_cmc_cntr != shard->cntr ) {
        if ((*flash_settings.check_delete_in_future)(data)) {
            new_entry.deleted = 1;
        }
    }

    /*
     * if the shard allows eviction and is replicated, evict all
     * items with the same hash to make life easier before we insert
     * the new entry
     *
     * NOTE: shard->replicated should never be set with simple replication
     */
    if ( 1 == shard->evict_to_free && 1 == shard->replicated ) {

        hash_entry = bucket_head;
        for ( i = 0; i < bucket->next_item; i++,hash_entry++ ) {

            if ( 0 == hash_entry->used ) {
                plat_assert_always( 0 == 1 );
            }
            if ( (uint16_t)(syndrome >> 48) != hash_entry->syndrome ) {
                continue;
            }

            // special case of hash eviction in cache mode and replicated
            if ( 1 == shard->persistent ) {
                log_rec.syndrome     = hash_entry->syndrome;
                log_rec.deleted      = hash_entry->deleted;
                log_rec.reserved     = 0;
                log_rec.blocks       = 0;  // distinguishes a delete record
                log_rec.bucket       = syndrome % shard->hash_size;
                log_rec.blk_offset   = hash_entry->address;
                log_rec.old_offset   = 0;
                if ( 1 == shard->replicated ) {
                    log_rec.seqno = meta_data->sequence;
                } else {
                    log_rec.seqno =
                        __sync_add_and_fetch( &shard->sequence, 1 );
                }
                log_rec.target_seqno = 0;
                log_write( shard, &log_rec );
            }

            class = shard->slab_classes +
                shard->class_table[mcd_osd_lba_to_blk(hash_entry->blocks)];
            (void) __sync_fetch_and_add( &class->dealloc_pending, 1 );
            (void) __sync_fetch_and_add( &shard->blk_dealloc_pending,
                                         class->slab_blksize );
            (void) __sync_fetch_and_sub(
                &shard->blk_consumed,
                mcd_osd_lba_to_blk(hash_entry->blocks) );
            *((uint64_t *)hash_entry) = 0;

            if ( 0 == bucket->next_item ) {
                plat_assert_always( 0 == 1 );
            }
            if ( bucket_head + bucket->next_item - 1 != hash_entry ) {
                *hash_entry = bucket_head[--bucket->next_item];
                *((uint64_t *)&bucket_head[bucket->next_item]) = 0;
                i--, hash_entry--;      // recheck this entry after packing
            }
            else {
                bucket->next_item--;
            }

            if ( Mcd_osd_bucket_size - 1 == bucket->next_item ) {
                (void) __sync_fetch_and_sub( &shard->num_full_buckets, 1 );
            }

            (void) __sync_fetch_and_sub( &shard->num_objects, 1 );
            (void) __sync_fetch_and_add( &shard->num_hash_evictions, 1 );
        }
    }

    if ( true == obj_exists && 0 == shard->evict_to_free ) {
        /*
         * safe to reuse hash_entry here when eviction is not allowed
         */
        /*
         * overwrite case in store mode
         *
         * Here we won't write a log record or remove the hash entry;
         * instead we write a special log record at create time
         */
        if ( 1 == shard->persistent ) {
            class = shard->slab_classes +
                shard->class_table[mcd_osd_lba_to_blk(hash_entry->blocks)];
            uint64_t slabs = __sync_fetch_and_add( &class->dealloc_pending,
                                                   1 );
            uint64_t blks  = __sync_fetch_and_add( &shard->blk_dealloc_pending,
                                                   class->slab_blksize );
            (void) __sync_fetch_and_sub(
                &shard->blk_consumed,
                mcd_osd_lba_to_blk(hash_entry->blocks) );

            // insert a clever algorithm here to determine if too
            // much space is pending deallocation
            if ( (double)blks / shard->blk_allocated > 0.15 ||
                 ( (double)class->used_slabs / class->total_slabs > 0.40 &&
                   (double)slabs / class->used_slabs > 0.50 ) ) {
                log_sync( shard );
            }
        } else {
            mcd_fth_osd_remove_entry( shard, hash_entry );
        }

        (void) __sync_fetch_and_add( &shard->num_overwrites, 1 );
    }
    else {
        if ( Mcd_osd_bucket_size == bucket->next_item ) {

            if ( 0 == shard->evict_to_free ) {
                /*
                 * hash bucket is full, try the overflow table
                 */
                index = shard->overflow_index +
                    ( ( syndrome % shard->hash_size ) / shard->lock_bktsize )
                    * Mcd_osd_overflow_depth;

                hash_entry = shard->overflow_table +
                    ( ( syndrome % shard->hash_size ) / shard->lock_bktsize )
                    * Mcd_osd_overflow_depth;

                for ( i = 0; i < Mcd_osd_overflow_depth; i++,hash_entry++ ) {
                    if ( 0 == hash_entry->used ) {
                        bucket->overflowed = 1;
                        index[i] = ( bucket - shard->hash_buckets )
                            % shard->lock_bktsize;
                        mcd_log_msg( 20370, PLAT_LOG_LEVEL_DEBUG,
                                     "bucket=%lu index=%hu",
                                     bucket - shard->hash_buckets, index[i] );
                        (void) __sync_fetch_and_add( &shard->num_objects, 1 );
                        break;
                    }
                }
                if ( Mcd_osd_overflow_depth > i ) {
                    (void) __sync_fetch_and_add(&shard->num_soft_overflows, 1);
                }
                else {
                    /*
                     * sigh, even the overflow area is full
                     */
                    (void) __sync_fetch_and_add( &shard->blk_consumed,
                                                 new_entry.blocks );
                    mcd_fth_osd_remove_entry( shard, &new_entry );
                    (void)__sync_fetch_and_add(&shard->num_hard_overflows, 1);
                    rc = FLASH_ENOSPC;
                    goto out;
                }
            }
            else {
                n = 0;
                do {
                    hash_entry =
                        bucket_head + ( bucket->hand % Mcd_osd_bucket_size );
                    if ( 0 == hash_entry->used ) {
                        plat_assert_always( 0 == 1 );
                    }
                    if ( 0 == hash_entry->referenced &&
                         0 == hash_entry->deleted ) {
                        break;
                    }
                    if ( ( 2 * Mcd_osd_bucket_size ) < n ) {
                        (void) __sync_fetch_and_add( &shard->blk_consumed,
                                                     blocks );
                        mcd_fth_osd_remove_entry( shard, &new_entry );
                        rc = FLASH_ENOSPC;
                        goto out;
                    }
                    hash_entry->referenced = 0;
                    bucket->hand++;
                } while ( ++n );

                // hash eviction in cache mode
                if ( 1 == shard->persistent ) {
                    log_rec.syndrome     = hash_entry->syndrome;
                    log_rec.deleted      = hash_entry->deleted;
                    log_rec.reserved     = 0;
                    log_rec.blocks       = 0;  // distinguishes a delete record
                    log_rec.bucket       = syndrome % shard->hash_size;
                    log_rec.blk_offset   = hash_entry->address;
                    log_rec.old_offset   = 0;
                    if ( 1 == shard->replicated ) {
                        log_rec.seqno = meta_data->sequence;
                    } else {
                        log_rec.seqno =
                            __sync_add_and_fetch( &shard->sequence, 1 );
                    }
                    log_rec.target_seqno = 0;
                    log_write( shard, &log_rec );
                }

                if ( 1 == shard->replicated ) {
                    class = shard->slab_classes + shard->class_table
                        [mcd_osd_lba_to_blk(hash_entry->blocks)];
                    (void) __sync_fetch_and_add( &class->dealloc_pending, 1 );
                    (void) __sync_fetch_and_add( &shard->blk_dealloc_pending,
                                                 class->slab_blksize );
                    (void) __sync_fetch_and_sub(
                        &shard->blk_consumed,
                        mcd_osd_lba_to_blk(hash_entry->blocks) );
                    *((uint64_t *)hash_entry) = 0;
                } else {
                    mcd_fth_osd_remove_entry( shard, hash_entry );
                }

                (void) __sync_fetch_and_add( &shard->num_hash_evictions, 1 );
            }
        }
        else {
            hash_entry = bucket_head + bucket->next_item++;
            (void) __sync_fetch_and_add( &shard->num_objects, 1 );
            if ( Mcd_osd_bucket_size == bucket->next_item ) {
                (void) __sync_fetch_and_add( &shard->num_full_buckets, 1 );
            }
        }
    }
    (void) __sync_fetch_and_add( &shard->total_objects, 1 );

    // create case
    if ( 1 == shard->persistent ) {
        log_rec.syndrome   = new_entry.syndrome;
        log_rec.deleted    = new_entry.deleted;
        log_rec.reserved   = 0;
        log_rec.blocks     = new_entry.blocks;
        log_rec.bucket     = syndrome % shard->hash_size;
        log_rec.blk_offset = new_entry.address;
        log_rec.seqno      = meta_data->sequence;
        if ( true == obj_exists && 0 == shard->evict_to_free ) {
            // overwrite case in store mode
            log_rec.old_offset   = ~(hash_entry->address);
            log_rec.target_seqno = target_seqno;
        } else {
            log_rec.old_offset   = 0;
            log_rec.target_seqno = 0;
        }
        log_write( shard, &log_rec );
    }

    *hash_entry = new_entry;
    shard->addr_table[blk_offset] = hash_entry - shard->hash_table;
    (void) __sync_fetch_and_add( &shard->blk_consumed,
                                 mcd_osd_lba_to_blk( new_entry.blocks ) );

out:
    if ( dyn_buffer ) {
        mcd_fth_osd_iobuf_free( buf );
    }
    return rc;
}


static int
mcd_fth_osd_slab_get( void * context, mcd_osd_shard_t * shard, char *key,
                      int key_len, void ** ppdata, SDF_size_t * pactual_size,
                      int flags, struct objMetaData * meta_data,
                      uint64_t syndrome )
{
    int                         i, rc;
    char                      * buf;
    char                      * data_buf;
    uint64_t                    tmp_offset;
    uint64_t                    blk_offset;
    uint64_t                    offset;
    int                         nbytes;
    mcd_osd_meta_t            * meta;
    mcd_osd_hash_t            * hash_entry;
    mcd_osd_bucket_t          * bucket;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );
    (void) __sync_fetch_and_add( &shard->num_gets, 1 );

#ifdef  MCD_OSD_DEBUGGING
    (void) __sync_fetch_and_add( &Mcd_osd_get_cmds, 1 );
    if ( 0 == Mcd_osd_get_cmds % MCD_OSD_DBG_GET_DIV ) {
        mcd_log_msg( 20327, PLAT_LOG_LEVEL_INFO,
                     "%lu get cmds", Mcd_osd_get_cmds );
    }
#endif

    if ( NULL == ppdata ) {
        (void) __sync_fetch_and_add( &shard->get_exist_checks, 1 );
    }

    /*
     * look up the hash table entry
     */
    bucket = shard->hash_buckets +
        ( ( syndrome % shard->hash_size ) / Mcd_osd_bucket_size );
    hash_entry = shard->hash_table +
        ( ( syndrome % shard->hash_size ) & Mcd_osd_bucket_mask );

    int overflow = 0;
    int end = bucket->next_item;

    for ( i = 0; i <= end; i++,hash_entry++ ) {

        if ( 0 == overflow && i == bucket->next_item ) {
            if ( 0 == bucket->overflowed ) {
                break;
            }
            i = -1, end = Mcd_osd_overflow_depth - 1;
            hash_entry = shard->overflow_table +
                ( ( syndrome % shard->hash_size ) / shard->lock_bktsize )
                * Mcd_osd_overflow_depth - 1;
            overflow = 1;
            continue;
        }

        if ( 0 == overflow ) {
            if ( 0 == hash_entry->used ) {
                plat_assert_always( 0 == 1 );
            }
        }
        else {
            if ( 0 == hash_entry->used ) {
                continue;
            }
        }

        if ( (uint16_t)(syndrome >> 48) != hash_entry->syndrome ) {
            continue;
        }

        blk_offset = hash_entry->address;

        tmp_offset = shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
            + (blk_offset % Mcd_osd_rand_blksize);
        offset = tmp_offset * Mcd_osd_blk_size;

    override_retry:
        if ( NULL == ppdata ) {
            /*
             * read only one block for existence check
             */
            nbytes = Mcd_osd_blk_size;
        }
        else {
            nbytes =
                mcd_osd_lba_to_blk( hash_entry->blocks ) * Mcd_osd_blk_size;
        }

            if ( MCD_OSD_MAX_BLKS_OLD >= ( nbytes / Mcd_osd_blk_size ) ) {
                data_buf = ((osd_state_t *)context)->osd_buf;
            }
            else {
                data_buf = mcd_fth_osd_iobuf_alloc(
                    mcd_osd_lba_to_blk( hash_entry->blocks ) *
                    Mcd_osd_blk_size, false );
                if ( NULL == data_buf ) {
                    return FLASH_ENOMEM;
                }
            }
        buf = (char *)( ( (uint64_t)data_buf + Mcd_osd_blk_size - 1 )
                        & Mcd_osd_blk_mask );

#ifdef  MCD_ENABLE_SLAB_CACHE
        static uint32_t count = 0;

        if ( 0 != flash_settings.fake_miss_rate &&
             0 == count++ % flash_settings.fake_miss_rate ) {
            /*
             * read the data from ssd
             */
            rc = mcd_fth_aio_blk_read( context,
                                       buf,
                                       offset,
                                       nbytes );
            if ( FLASH_EOK != rc ) {
                mcd_log_msg( 20003, PLAT_LOG_LEVEL_ERROR,
                             "failed to read blocks, rc=%d", rc );
                mcd_fth_osd_slab_free( data_buf );
                return rc;
            }
        }
        else {
            /*
             * read the data from the slab cache
             */
            memcpy( buf, shard->slab_cache + offset, nbytes );
            rc = FLASH_EOK;
        }
#else
        rc = mcd_fth_aio_blk_read( context,
                                   buf,
                                   offset,
                                   nbytes );
        if ( FLASH_EOK != rc ) {
            mcd_log_msg( 20003, PLAT_LOG_LEVEL_ERROR,
                         "failed to read blocks, rc=%d", rc );
            mcd_fth_osd_slab_free( data_buf );
            return rc;
        }
#endif  /* MCD_ENABLE_SLAB_CACHE */

        meta = (mcd_osd_meta_t *)buf;
        mcd_log_msg( 20312, PLAT_LOG_LEVEL_DEBUG, "key_len=%d data_len=%u",
                     meta->key_len, meta->data_len );

        if ( MCD_OSD_MAGIC_NUMBER != meta->magic ) {
            mcd_log_msg( 20325, PLAT_LOG_LEVEL_FATAL, "not enough magic!" );
            continue;
        }

        if ( key_len != meta->key_len ) {
            mcd_log_msg( 20326, MCD_OSD_LOG_LVL_DIAG,
                         "key length mismatch, req %d osd %d",
                         key_len, meta->key_len );
            (void) __sync_fetch_and_add( &shard->get_hash_collisions, 1 );
            mcd_fth_osd_slab_free( data_buf );
            continue;
        }

        if ( 0 != strncmp( buf + sizeof(mcd_osd_meta_t), key, key_len ) ) {
            mcd_log_msg( 20006, MCD_OSD_LOG_LVL_DIAG,
                         "key mismatch, req %s", key );
            (void) __sync_fetch_and_add( &shard->get_hash_collisions, 1 );
            mcd_fth_osd_slab_free( data_buf );
            continue;
        }

        if ( NULL != meta_data ) {
            meta_data->createTime = meta->create_time;
            meta_data->expTime    = meta->expiry_time;
            meta_data->sequence	  = meta->seqno;
        }

        if ( NULL == ppdata ) {
            /*
             * ok only an existence check
             */
            mcd_fth_osd_slab_free( data_buf );
            return FLASH_EOK;
        }

        /*
         * workaround for #5284: a metadata inconsistency could happen
         * for persistent cache containers when an object is overwritten
         * in place but the corresponding log entries didn't get persisted
         */
        if ( sizeof(mcd_osd_meta_t) + meta->key_len + meta->data_len >
             nbytes ) {
            mcd_log_msg( 150003, PLAT_LOG_LEVEL_DIAGNOSTIC,
                         "object size doesn't match hash entry!!! "
                         "klen=%d dlen=%u nbybes=%d syn=%lu hsyn=%hu "
                         "ctime=%u",
                         meta->key_len, meta->data_len, nbytes,
                         syndrome, hash_entry->syndrome,
                         meta->create_time);
            (void) __sync_fetch_and_add( &shard->get_size_overrides, 1 );

            /*
             * update the hash table entry and try again
             */
            int blocks =
                ( sizeof(mcd_osd_meta_t) + meta->key_len + meta->data_len +
                  ( Mcd_osd_blk_size - 1 ) ) / Mcd_osd_blk_size;
            hash_entry->blocks = mcd_osd_blk_to_lba( blocks );

            mcd_fth_osd_slab_free( data_buf );
            goto override_retry;
        }

        *ppdata = data_buf;
        *pactual_size = meta->data_len;

        /*
         * meta can no longer be accessed after this
         */
        memmove( data_buf, buf + sizeof(mcd_osd_meta_t) + meta->key_len,
                 meta->data_len );

        return FLASH_EOK;
    }

    return FLASH_ENOENT;
}


uint64_t mcd_osd_shard_get_stats( struct shard * shard, int stat_key )

{
    uint64_t                    stat;
    mcd_osd_shard_t           * mcd_shard = (mcd_osd_shard_t *)shard;

    mcd_log_msg( 20371, PLAT_LOG_LEVEL_DEBUG, "ENTERING, shard=%p", shard );

    switch ( stat_key ) {

    case FLASH_SHARD_MAXBYTES:
        stat = mcd_shard->total_size;
        break;

    case FLASH_SPACE_USED:
        if ( mcd_shard->use_fifo ) {
            stat = mcd_shard->blk_consumed * Mcd_osd_blk_size;
        }
        else {
            stat = 0;
            for ( int i = 0; i < MCD_OSD_MAX_NCLASSES; i++ ) {
                stat += mcd_shard->slab_classes[i].used_slabs *
                    mcd_shard->slab_classes[i].slab_blksize * Mcd_osd_blk_size;
            }
        }
        break;

    case FLASH_SPACE_ALLOCATED:
        if ( mcd_shard->use_fifo ) {
            stat = mcd_shard->fifo.blk_allocated * Mcd_osd_blk_size;
            if ( stat > mcd_shard->total_size ) {
                stat = mcd_shard->total_size;
            }
        }
        else {
            stat = mcd_shard->blk_allocated * Mcd_osd_blk_size;
        }
        break;

    case FLASH_SPACE_CONSUMED:
        stat = mcd_shard->blk_consumed * Mcd_osd_blk_size;
        break;

    case FLASH_SLAB_CLASS_SEGS:
        for ( int i = 0; i < MCD_OSD_MAX_NCLASSES; i++ ) {
            mcd_shard->class_segments[i] =
                mcd_shard->slab_classes[i].num_segments;
        }
        stat = (uint64_t)mcd_shard->class_segments;
        break;

    case FLASH_SLAB_CLASS_SLABS:
        for ( int i = 0; i < MCD_OSD_MAX_NCLASSES; i++ ) {
            mcd_shard->class_slabs[i] =
                mcd_shard->slab_classes[i].used_slabs;
        }
        stat = (uint64_t)mcd_shard->class_slabs;
        break;

    case FLASH_NUM_OBJECTS:
        stat = mcd_shard->num_objects;
        break;

    case FLASH_NUM_CREATED_OBJECTS:
        stat = mcd_shard->total_objects;
        break;

    case FLASH_NUM_EVICTIONS:
#ifndef MCD_ENABLE_SLAB
        stat = Mcd_osd_replaces;
#else
        stat = mcd_shard->num_slab_evictions + mcd_shard->num_hash_evictions;
#endif
        break;

    case FLASH_NUM_HASH_EVICTIONS:
        stat = mcd_shard->num_hash_evictions;
        break;

    case FLASH_NUM_INVAL_EVICTIONS:
        stat = mcd_shard->invalid_evictions;
        break;

    case FLASH_NUM_SOFT_OVERFLOWS:
        stat = mcd_shard->num_soft_overflows;
        break;

    case FLASH_NUM_HARD_OVERFLOWS:
        stat = mcd_shard->num_hard_overflows;
        break;

    case FLASH_GET_HASH_COLLISIONS:
        stat = mcd_shard->get_hash_collisions;
        break;

    case FLASH_SET_HASH_COLLISIONS:
        stat = mcd_shard->set_hash_collisions;
        break;

    case FLASH_NUM_OVERWRITES:
        stat = mcd_shard->num_overwrites;
        break;

    case FLASH_READ_OPS:
        stat = Mcd_aio_read_ops;
        break;

    case FLASH_OPS:
        stat = Mcd_aio_read_ops + Mcd_aio_write_ops;
        break;

    case FLASH_NUM_GET_OPS:
        stat = mcd_shard->num_gets;
        break;

    case FLASH_NUM_PUT_OPS:
        stat = mcd_shard->num_puts;
        break;

    case FLASH_NUM_DELETE_OPS:
        stat = mcd_shard->num_deletes;
        break;

    case FLASH_NUM_EXIST_CHECKS:
        stat = mcd_shard->get_exist_checks;
        break;

    case FLASH_NUM_FULL_BUCKETS:
        stat = mcd_shard->num_full_buckets;
        break;

    case FLASH_FTH_SCHEDULER_IDLE_TIME:
        stat = fthGetSchedulerIdleTime();
        break;

    case FLASH_FTH_SCHEDULER_DISPATCH_TIME:
        stat = fthGetSchedulerDispatchTime();
        break;

    case FLASH_FTH_SCHEDULER_LOW_PRIO_DISPATCH_TIME:
        stat = fthGetSchedulerLowPrioDispatchTime();
        break;

    case FLASH_FTH_TOTAL_THREAD_RUN_TIME:
        stat = fthGetTotalThreadRunTime();
        break;

    case FLASH_FTH_NUM_DISPATCHES:
        stat = fthGetSchedulerNumDispatches();
        break;

    case FLASH_FTH_NUM_LOW_PRIO_DISPATCHES:
        stat = fthGetSchedulerNumLowPrioDispatches();
        break;

    case FLASH_FTH_AVG_DISPATCH_NANOSEC:
        stat = fthGetSchedulerAvgDispatchNanosec();
        break;

    case FLASH_TSC_TICKS_PER_MICROSECOND:
        stat = fthGetTscTicksPerMicro();
        break;

    default:
        /*
         * unknown STAT type, simply return 0
         */
        stat = 0;
    }

    return stat;
}


SDF_status_t
mcd_osd_get_shard_stats( void * pai, SDFContainer ctnr, int stat_key,
                         uint64_t * stat )
{
    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

    *stat = mcd_osd_shard_get_stats( (struct shard *)&Mcd_osd_slab_shard,
                                     stat_key );

#ifdef  USING_SDF_SHIM
    /*
     * temporary workaround
     */
    if ( FLASH_OPS > stat_key ) {
        return mcd_fth_shm_get_container_stats( pai, ctnr, stat_key, stat );
    }
#endif

    return SDF_SUCCESS;
}

static osd_state_t *mcd_osd_init_state(int aio_category)
{
    fthMbox_t   *osd_mbox;
    osd_state_t *osd_state;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING");

    osd_state = (osd_state_t *) plat_alloc(sizeof(osd_state_t));
    if (osd_state == NULL) {
	mcd_log_msg( 20066, PLAT_LOG_LEVEL_ERROR, "plat_alloc failed" );
	plat_assert_always( 0 == 1 );
    }
    
    osd_mbox = (fthMbox_t *)plat_alloc( sizeof(fthMbox_t) );
    if ( NULL == osd_mbox ) {
	mcd_log_msg( 20066, PLAT_LOG_LEVEL_ERROR, "plat_alloc failed" );
	plat_assert_always( 0 == 1 );
    }
    osd_state->osd_mbox = osd_mbox;
    fthMboxInit( osd_state->osd_mbox );
    osd_state->osd_blocks = 0;
    osd_state->osd_lock = NULL;
    osd_state->osd_wait = NULL;

    /*
     * allocate IO buffer for each worker fthread, which is suppose to be
     * be greater than ( sizeof(mcd_osd_meta_t) + key_len + max_data_len );
     * however, to conserve memory, we only allocate enough to hold
     * anything up to 1MB and will do dynamic re-allocation on-demand at
     * the flash layer
     */
    osd_state->osd_buf =
	mcd_fth_osd_iobuf_alloc( MCD_FTH_OSD_BUF_SIZE + 4096, true );
    if ( NULL == osd_state->osd_buf ) {
	mcd_log_msg( 20173, PLAT_LOG_LEVEL_ERROR,
		     "failed to alloc osd_buf" );
	plat_abort();
    }

    return(osd_state);
}

static void mcd_osd_free_state(osd_state_t *osd_state)
{
    if ( osd_state->osd_mbox ) {
        fthMboxTerm( osd_state->osd_mbox );
	plat_free(osd_state->osd_mbox);
    }
    if ( osd_state->osd_buf) {
	plat_free(osd_state->osd_buf);
    }
    memset( (void *)osd_state, 0, sizeof(osd_state_t) );
}

int mcd_osd_init( void )
{
    int   i;

    for (i=0; i<SSD_AIO_CTXT_MAX_COUNT; i++) {
        Mcd_fth_aio_ctxts[i] = 0;
    }
    for (i=0; i< AIO_MAX_CTXTS; i++) {
        Mcd_aio_states[i] = 0;
    }
    fthLockInit( &Mcd_aio_ctxt_lock );

    fthMboxInit( &Mcd_fth_admin_mbox );
    return mcd_osd_slab_init();
}


/************************************************************************
 *                                                                      *
 *                      Wrappers for SDF (SLAB)                         *
 *                                                                      *
 ************************************************************************/

int
flash_report_version( char ** bufp, int * lenp )
{
    recovery_report_version( bufp, lenp );
    backup_report_version( bufp, lenp );
    return 0;
}


struct flashDev * mcd_osd_flash_open( char * name, flash_settings_t *flash_settings, int flags )
{
    mcd_log_msg( 20374, PLAT_LOG_LEVEL_INFO,
                 "ENTERING, initializing mcd osd" );

    if ( 0 != mcd_osd_slab_init() ) {
        mcd_log_msg( 20375, PLAT_LOG_LEVEL_FATAL, "failed to init mcd osd" );
        plat_abort();
    }

    // format persistent structures in flash
    if ( ( (flags & FLASH_OPEN_REFORMAT_DEVICE) ||
           (flags & FLASH_OPEN_FORMAT_VIRGIN_DEVICE) ||
           (flags & FLASH_OPEN_REVIRGINIZE_DEVICE) ) &&
         ( 0 != flash_format( Mcd_aio_total_size ) ) ) {
        mcd_log_msg( 20376, PLAT_LOG_LEVEL_FATAL,
                     "failed to format flash rec area" );
        plat_abort();
    }

    // initialize recovery
    if ( 0 != recovery_init() ) {
        mcd_log_msg( 20377, PLAT_LOG_LEVEL_FATAL, "failed to init mcd rec" );
        plat_abort();
    }

    // initialize replication (replication needs persistence)
    if ( 0 != replication_init() ) {
        mcd_log_msg( 20378, PLAT_LOG_LEVEL_FATAL,
                     "failed to init replication" );
        plat_abort();
    }

    fthMboxInit( &Mcd_osd_writer_mbox );
    fthMboxInit( &Mcd_osd_sleeper_mbox );
    fthLockInit( &Mcd_osd_segment_lock );

    /*
     * for now just returning NULL is fine
     */
    return NULL;
}


inline void
mcd_osd_shard_free_segments( mcd_osd_shard_t * shard, int total_segments )
{
    int                         s;
    fthWaitEl_t               * wait;

    wait = fthLock( &Mcd_osd_segment_lock, 1, NULL );
    for ( s = total_segments - 1; s >= 0 ; s-- ) {
        Mcd_osd_free_segments[Mcd_osd_free_seg_curr++] = shard->segments[s];
    }
    fthUnlock( wait );

    return;
}


struct shard *
mcd_osd_shard_create( struct flashDev * dev, uint64_t shard_id,
                      int flags, uint64_t quota, unsigned max_nobjs )
{
    int                         i, rc;
    int                         total_segments = 0;
    mcd_osd_shard_t           * mcd_shard;
    fthWaitEl_t               * wait;

    mcd_log_msg( 20379, PLAT_LOG_LEVEL_INFO,
                 "ENTERING, shard_id=%lu flags=0x%x quota=%lu max_nobjs=%u",
                 shard_id, flags, quota, max_nobjs );

    if ( FLASH_SHARD_INIT_PERSISTENCE_YES ==
         (flags & FLASH_SHARD_INIT_PERSISTENCE_MASK) ) {
        for ( i = 0; i < MCD_OSD_MAX_NUM_SHARDS; i++ ) {
            if ( NULL == Mcd_osd_slab_shards[i] ) {
                continue;
            }
            if ( Mcd_osd_slab_shards[i]->shard.shardID == shard_id ) {
                mcd_shard = Mcd_osd_slab_shards[i];
                mcd_log_msg( 20380, PLAT_LOG_LEVEL_INFO,
                             "shard found, id=%lu", shard_id );
                plat_assert_always( 1 == mcd_shard->open );
                return (struct shard *)mcd_shard;
            }
        }
    }

    /*
     * allocate segments for the shard and initialize the address table
     */
    if ( quota > ((uint64_t)Mcd_osd_free_seg_curr) * Mcd_osd_segment_size ) {
        mcd_log_msg( 150027, PLAT_LOG_LEVEL_ERROR, "not enough space: quota=%lu - free_seg=%lu - seg_size=%lu",
		     quota, (uint64_t)Mcd_osd_free_seg_curr, Mcd_osd_segment_size );
        return NULL;
    }

    mcd_shard = (mcd_osd_shard_t *)plat_alloc( sizeof(mcd_osd_shard_t) );
    if ( NULL == mcd_shard ) {
        mcd_log_msg( 20382, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocate mcd_shard" );
        return NULL;
    }
    memset( (void *)mcd_shard, 0, sizeof(mcd_osd_shard_t) );

    /*
     * if quota is zero, allocate all available space to the shard
     */
    if ( 0 == quota ) {
        quota = ((uint64_t)Mcd_osd_free_seg_curr) * Mcd_osd_segment_size;
    }
    mcd_shard->total_segments =
        ( quota + Mcd_osd_segment_size - 1 ) / Mcd_osd_segment_size;

    /*
     * each shard need to contain at least two segments
     */
    if ( 2 > mcd_shard->total_segments ) {
        mcd_shard->total_segments = 2;
    }
    if ( mcd_shard->total_segments > Mcd_osd_free_seg_curr ) {
        mcd_log_msg( 160003, PLAT_LOG_LEVEL_ERROR,
                     "not enough space, needed %lu available %lu",
                     mcd_shard->total_segments, Mcd_osd_free_seg_curr );
        goto out_failed;
    }

    mcd_shard->segments = (uint32_t *)
        plat_alloc( mcd_shard->total_segments * sizeof(uint32_t) );
    if ( NULL == mcd_shard->segments ) {
        mcd_log_msg( 20384, PLAT_LOG_LEVEL_ERROR,
                     "failed to allocated segment list" );
        goto out_failed;
    }

    wait = fthLock( &Mcd_osd_segment_lock, 1, NULL );
    for ( i = 0; i < mcd_shard->total_segments; i++ ) {
        mcd_shard->segments[i] =
            Mcd_osd_free_segments[--Mcd_osd_free_seg_curr];
        if ( 0 == i % 100 ) {
            mcd_log_msg( 20385, MCD_OSD_LOG_LVL_INFO,
                         "%dth segment allocated, blk_offset=%u",
                         i, mcd_shard->segments[i] );
        }
    }
    fthUnlock( wait );
    mcd_log_msg( 160004, PLAT_LOG_LEVEL_INFO,
                 "%lu segments allcated to shard %lu, free_seg_curr=%lu",
                 mcd_shard->total_segments, shard_id, Mcd_osd_free_seg_curr );

    mcd_shard->shard.shardID    = shard_id;
    mcd_shard->shard.flags      = flags;
    mcd_shard->shard.quota      = quota;
    mcd_shard->shard.maxObjs    = max_nobjs;

    for ( i = 0; i < MCD_OSD_MAX_NUM_SHARDS; i++ ) {
        if ( NULL == Mcd_osd_slab_shards[i] ) {
            Mcd_osd_slab_shards[i] = mcd_shard;
            break;
        }
    }
    if ( MCD_OSD_MAX_NUM_SHARDS == i ) {
        mcd_log_msg( 20387, PLAT_LOG_LEVEL_ERROR, "too many shards created" );
        // here mcd_shard->total_segments should contain the actual
        // number of segments allocated, and data_blk_offset should be 0
        goto out_failed;
    }

    /*
     * format all shards
     * non-persistent shards have some persistent properties
     * persistent shards have the same, plus other persistent structures
     */
    rc = shard_format( shard_id, flags, quota, max_nobjs, mcd_shard );
    if ( 0 != rc ) {
        mcd_log_msg( 20388, PLAT_LOG_LEVEL_ERROR,
                     "shard_format() failed, rc=%d", rc );
        Mcd_osd_slab_shards[i] = NULL;
        // it's possible that only the sync failed when creating the
        // persistent shard, so "un-format" the shard to be sure it
        // doesn't get resurrected
        shard_unformat( shard_id );
        // here mcd_shard->total_segments does not include metadata segments,
        // and data_blk_offset is the number of blocks of metadata
        goto out_failed;
    }

    if ( FLASH_SHARD_INIT_PERSISTENCE_YES ==
         (flags & FLASH_SHARD_INIT_PERSISTENCE_MASK) ) {
        rc = seqno_cache_init( &mcd_shard->shard );
        if ( 0 != rc ) {
            mcd_log_msg( 20389, PLAT_LOG_LEVEL_ERROR,
                         "seqno cache init failed, rc=%d", rc );
            Mcd_osd_slab_shards[i] = NULL;
            shard_unformat( shard_id );
            goto out_failed;
        }
    }

    return &mcd_shard->shard;

out_failed:

    if ( mcd_shard ) {
        if ( mcd_shard->segments ) {
            if ( FLASH_SHARD_INIT_PERSISTENCE_YES !=
                 (flags & FLASH_SHARD_INIT_PERSISTENCE_MASK) &&
                 FLASH_SHARD_INIT_EVICTION_CACHE ==
                 (flags & FLASH_SHARD_INIT_EVICTION_MASK) ) {
                total_segments = mcd_shard->total_segments;
            } else {
                total_segments = mcd_shard->total_segments +
                    (mcd_shard->data_blk_offset / Mcd_osd_segment_blks);
            }

            mcd_osd_shard_free_segments( mcd_shard, total_segments );
            plat_free( mcd_shard->segments );
        }
        plat_free( mcd_shard );
    }

    return NULL;
}


void
mcd_osd_shard_uninit( mcd_osd_shard_t * shard )
{
    int                         i, j;
    int                         blksize;
    int                         bitmap_size;
    uint64_t                    total_alloc = 0;

    // fifo: non-persistent, cache mode
    if ( shard->use_fifo ) {
        if ( shard->fifo.wbuf_base ) {
            plat_free( shard->fifo.wbuf_base );
            shard->fifo.wbuf_base = NULL;
            total_alloc += MCD_OSD_WBUF_SIZE * MCD_OSD_NUM_WBUFS + 4095;
        }
    }
    // slab: non-persistent+store mode, persistent+(cache or store mode)
    else {

#ifdef MCD_ENABLE_SLAB_CACHE
        if ( shard->slab_cache ) {
            plat_free_large( shard->slab_cache );
            shard->slab_cache = NULL;
            total_alloc += shard->total_size;
        }
#endif

        // segment table
        if ( shard->segment_table ) {
            plat_free( shard->segment_table );
            shard->segment_table = NULL;
            total_alloc += shard->total_segments * sizeof(mcd_osd_segment_t *);
        }

        for ( i = 0, blksize = 1; i < MCD_OSD_MAX_NCLASSES; i++ ) {
            // free slabs
            for ( j = 0; j < MCD_OSD_MAX_PTHREADS; j++ ) {
                if ( shard->slab_classes[i].free_slabs[j] ) {
                    plat_free( shard->slab_classes[i].free_slabs[j] );
                    shard->slab_classes[i].free_slabs[j] = NULL;
                    total_alloc +=
                        shard->slab_classes[i].freelist_len * sizeof(uint32_t);
                }
            }

            // class segments
            if ( shard->slab_classes[i].segments ) {
                plat_free( shard->slab_classes[i].segments );
                shard->slab_classes[i].segments = NULL;
                total_alloc +=
                    shard->total_segments * sizeof(mcd_osd_segment_t *);
                mcd_log_msg( 40114, PLAT_LOG_LEVEL_DEBUG,
                             "slab class dealloc, blksize=%d, free_slabs=%lu, "
                             "segments=%lu",
                             blksize, shard->slab_classes[i].freelist_len *
                             sizeof(uint32_t), shard->total_segments *
                             sizeof(mcd_osd_segment_t *) );
            }

            blksize *= 2;
        }

        // segment bitmaps were allocated in one big chunk
        if ( shard->base_segments ) {
            bitmap_size = Mcd_osd_segment_blks / 8;
            if ( shard->base_segments[0].bitmap ) {
                plat_free_large( shard->base_segments[0].bitmap );
                shard->base_segments[0].bitmap = NULL;
                total_alloc += shard->total_segments * bitmap_size;
            }
            if ( shard->base_segments[0].update_map ) {
                plat_free_large( shard->base_segments[0].update_map );
                shard->base_segments[0].update_map = NULL;
                total_alloc += shard->total_segments * bitmap_size;
            }
            if ( shard->base_segments[0].update_map_s ) {
                plat_free_large( shard->base_segments[0].update_map_s );
                shard->base_segments[0].update_map_s = NULL;
                total_alloc += shard->total_segments * bitmap_size;
            }
            if ( shard->base_segments[0].alloc_map ) {
                plat_free_large( shard->base_segments[0].alloc_map );
                shard->base_segments[0].alloc_map = NULL;
                total_alloc += shard->total_segments * bitmap_size;
            }
            if ( shard->base_segments[0].alloc_map_s ) {
                plat_free_large( shard->base_segments[0].alloc_map_s );
                shard->base_segments[0].alloc_map_s = NULL;
                total_alloc += shard->total_segments * bitmap_size;
            }

            plat_free( shard->base_segments );
            shard->base_segments = NULL;
            total_alloc += shard->total_segments * sizeof(mcd_osd_segment_t );
        }

        mcd_log_msg( 20391, PLAT_LOG_LEVEL_INFO,
                     "segments deallocated, %lu segments, %d classes size=%lu",
                     shard->total_segments, i, total_alloc );
    }

    // the rest is common to fifo and slab shards

    // overflow index
    if ( shard->overflow_index ) {
        plat_free_large( shard->overflow_index );
        shard->overflow_index = NULL;
        total_alloc += shard->lock_buckets * Mcd_osd_overflow_depth *
            sizeof(uint16_t);
        mcd_log_msg( 20392, PLAT_LOG_LEVEL_INFO,
                     "overflow index deallocated, size=%lu",
                     shard->lock_buckets * Mcd_osd_overflow_depth
                     * sizeof(uint16_t) );
    }

    // overflow table is included in the hash table allocation

    // hash table and its overflow area (same chunk of storage)
    if ( shard->hash_table ) {
        plat_free_large( shard->hash_table );
        shard->hash_table = NULL;
        total_alloc += shard->hash_size * sizeof(mcd_osd_hash_t);
        mcd_log_msg( 20393, PLAT_LOG_LEVEL_INFO,
                     "hash table deallocated, size=%lu",
                     shard->hash_size * sizeof(mcd_osd_hash_t) );
        if ( shard->overflow_table ) {
            shard->overflow_table = NULL;
            total_alloc += shard->lock_buckets * Mcd_osd_overflow_depth *
                sizeof(mcd_osd_hash_t);
            mcd_log_msg( 20394, PLAT_LOG_LEVEL_INFO,
                         "overflow table deallocated, size=%lu",
                         shard->lock_buckets * Mcd_osd_overflow_depth
                         * sizeof(mcd_osd_hash_t) );
        }
    }

    // lock buckets
    if ( shard->bucket_locks ) {
        plat_free_large( shard->bucket_locks );
        shard->bucket_locks = NULL;
        total_alloc += shard->lock_buckets * sizeof(fthLock_t);
        mcd_log_msg( 20395, PLAT_LOG_LEVEL_INFO,
                     "lock_buckets deallocated, size=%lu",
                     shard->lock_buckets * sizeof(fthLock_t) );
    }

    // bucket table
    if ( shard->hash_buckets ) {
        plat_free_large( shard->hash_buckets );
        shard->hash_buckets = NULL;
        total_alloc += shard->hash_size / Mcd_osd_bucket_size *
            sizeof(mcd_osd_bucket_t);
        mcd_log_msg( 20396, PLAT_LOG_LEVEL_INFO,
                     "hash buckets deallocated, size=%lu",
                     shard->hash_size /
                     Mcd_osd_bucket_size * sizeof(mcd_osd_bucket_t) );
    }

    // address remapping table
    if ( shard->rand_table ) {
        plat_free( shard->rand_table );
        shard->rand_table = NULL;
        total_alloc += (shard->total_blks / Mcd_osd_rand_blksize) *
            sizeof(uint32_t);
        mcd_log_msg( 20397, PLAT_LOG_LEVEL_INFO,
                     "remapping table deallocated, size=%lu",
                     (shard->total_blks / Mcd_osd_rand_blksize) *
                     sizeof(uint32_t) );
    }

    // address lookup table
    if ( shard->addr_table ) {
        plat_free_large( shard->addr_table );
        shard->addr_table = NULL;
        total_alloc += shard->total_blks * sizeof(uint32_t);
        mcd_log_msg( 20398, PLAT_LOG_LEVEL_INFO,
                     "address table deallocated, size=%lu",
                     shard->total_blks * sizeof(uint32_t) );
    }

    mcd_log_msg( 20399, PLAT_LOG_LEVEL_INFO,
                 "shardID=%lu, total deallocated bytes=%lu",
                 shard->id, total_alloc );

    return;
}


struct shard *
mcd_osd_shard_open( struct flashDev * dev, uint64_t shard_id )
{
    int                         i, rc = 0;
    mcd_osd_shard_t           * mcd_shard = NULL;
    struct shard              * shard;
    void                      * wait;

    mcd_log_msg( 20400, PLAT_LOG_LEVEL_INFO, "ENTERING, shard_id=%lu",
                 shard_id );

    for ( i = 0; i < MCD_OSD_MAX_NUM_SHARDS; i++ ) {
        if ( NULL == Mcd_osd_slab_shards[i] ) {
            continue;
        }
        if ( Mcd_osd_slab_shards[i]->shard.shardID == shard_id ) {
            mcd_shard = Mcd_osd_slab_shards[i];
            mcd_log_msg( 20380, PLAT_LOG_LEVEL_INFO,
                         "shard found, id=%lu", shard_id );
            break;
        }
    }
    if ( MCD_OSD_MAX_NUM_SHARDS == i ) {
        mcd_log_msg( 20401, PLAT_LOG_LEVEL_WARN,
                     "could not find shard, id=%lu", shard_id );
        return NULL;
    }

    plat_assert_always( 0 == mcd_shard->open );

    if ( flash_settings.enable_fifo &&
         FLASH_SHARD_INIT_PERSISTENCE_YES !=
         (mcd_shard->shard.flags & FLASH_SHARD_INIT_PERSISTENCE_MASK) &&
         FLASH_SHARD_INIT_EVICTION_CACHE ==
         (mcd_shard->shard.flags & FLASH_SHARD_INIT_EVICTION_MASK) ) {
        rc = mcd_osd_fifo_shard_init( mcd_shard, shard_id,
                                      mcd_shard->shard.flags,
                                      mcd_shard->shard.quota,
                                      mcd_shard->shard.maxObjs );
    }
    else {
        rc = mcd_osd_slab_shard_init( mcd_shard, shard_id,
                                      mcd_shard->shard.flags,
                                      mcd_shard->shard.quota,
                                      mcd_shard->shard.maxObjs );
    }

    if ( 0 != rc ) {
        mcd_log_msg( 20402, PLAT_LOG_LEVEL_ERROR, "shard init failed, id=%lu",
                     shard_id );
        mcd_osd_shard_uninit( mcd_shard );
        return NULL;
    }

    /*
     * initialize the public shard structure
     * the following should have been set: shardID, flags, quota, maxObjs
     */
    shard = &mcd_shard->shard;

    shard->dev                  = dev;
    shard->flash_offset         = 0;          // FIXME
    shard->usedSpace            = 0;
    shard->numObjects           = 0;
    shard->numDeadObjects       = 0;
    shard->numCreatedObjects    = 0;

    for ( int i = 0; i < FTH_MAX_SCHEDS; i++ ) {
	shard->stats[i].numEvictions    = 0;
	shard->stats[i].numGetOps       = 0;
	shard->stats[i].numPutOps       = 0;
	shard->stats[i].numDeleteOps    = 0;
    }

    /*
     * recover persistent shards
     */
    if ( 1 == mcd_shard->persistent ) {
        int rc = shard_recover( mcd_shard );
        if ( 0 != rc ) {
            mcd_log_msg( 20403, PLAT_LOG_LEVEL_ERROR, "shard recover failed, id=%lu",
                         shard_id );
            mcd_osd_shard_uninit( mcd_shard );
            return NULL;
        }
    }

    /*
     * initialize backup
     */
    rc = backup_init( mcd_shard );
    if ( 0 != rc ) {
        mcd_log_msg( 20404, PLAT_LOG_LEVEL_ERROR,
                     "shard backup init failed, shardID=%lu", shard_id );
        if ( 1 == mcd_shard->persistent ) {
            shard_unrecover( mcd_shard );
        }
        mcd_osd_shard_uninit( mcd_shard );
        return NULL;
    }

    mcd_shard->open = 1;

    /*
     * slab pre-seeding for cache mode containers, this prevents
     * set failures caused by highly skewed workloads
     */
    if ( 0 == mcd_shard->blk_allocated && 0 != mcd_shard->evict_to_free ) {
        if ( mcd_shard->total_segments > mcd_shard->num_classes ) {
            for ( i = 0; i < mcd_shard->num_classes; i++ ) {
                mcd_fth_osd_grow_class( mcd_shard,
                                        &mcd_shard->slab_classes[i] );
            }
        }
    }

    wait = fthLock( &dev->lock, 1, NULL );
    shard->next = dev->shardList;
    dev->shardList = shard;
    fthUnlock( wait );

    return shard;
}


static void mcd_osd_shard_open_phase2( struct shard * shard, mcd_container_t * cntr )
{
    mcd_osd_shard_t           * mcd_shard = (mcd_osd_shard_t *)shard;

    mcd_log_msg( 20065, PLAT_LOG_LEVEL_DEBUG, "ENTERING, shardID=%lu",
                 shard->shardID );

    // set container back pointer
    mcd_shard->cntr = cntr;

    /*
     * recover persistent shards, part deux
     */
    if ( 1 == mcd_shard->persistent ) {
        shard_recover_phase2( mcd_shard );
    }

    return;
}

void
mcd_osd_shard_sync( struct shard * shard )
{
    mcd_log_msg( 20065, PLAT_LOG_LEVEL_DEBUG, "ENTERING, shardID=%lu",
                 shard->shardID );

    if ( 1 == ((mcd_osd_shard_t *)shard)->persistent ) {
        log_sync( (mcd_osd_shard_t *)shard );
    }

    return;
}


uint64_t
mcd_osd_slab_flash_get_high_sequence( struct shard * shard )
{
    mcd_log_msg( 20065, PLAT_LOG_LEVEL_DEBUG, "ENTERING, shardID=%lu",
                 shard->shardID );

    return ((mcd_osd_shard_t *)shard)->sequence;
}


void
mcd_osd_slab_flash_set_synced_sequence( struct shard * shard, uint64_t seqno )
{
    mcd_log_msg( 20405, PLAT_LOG_LEVEL_DEBUG, "ENTERING, shardID=%lu, seqno=%lu",
                 shard->shardID, seqno );
    ((mcd_osd_shard_t *)shard)->lcss = seqno;
    return;
}


uint64_t
mcd_osd_slab_flash_get_rtg( struct shard * shard )
{
    mcd_log_msg( 20065, PLAT_LOG_LEVEL_DEBUG, "ENTERING, shardID=%lu",
                 shard->shardID );
    return tombstone_get_rtg( (mcd_osd_shard_t *)shard );
}


void
mcd_osd_slab_flash_register_set_rtg_callback( void (*callback)
                                              (uint64_t shardID,
                                               uint64_t seqno) )
{
    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );
    tombstone_register_rtg_callback( callback );
    return;
}


int
mcd_osd_flash_get( struct ssdaio_ctxt * pctxt, struct shard * shard,
                        struct objMetaData * metaData, char * key,
                        char ** dataPtr, int flags )
{
    int                         ret;
    uint64_t                    syndrome;
    SDF_size_t                  actual_size = 0;
    osd_state_t               * osd_state = (osd_state_t *)pctxt;
    mcd_osd_shard_t           * mcd_shard = (mcd_osd_shard_t *)shard;

    mcd_log_msg( 20406, PLAT_LOG_LEVEL_DEBUG, "ENTERING, context=%p shardID=%lu",
                 (void *)pctxt, shard->shardID );

    syndrome = hash( (unsigned char *)key, metaData->keyLen, 0 );

    osd_state->osd_lock = (void *)( mcd_shard->bucket_locks +
        ( ( syndrome % mcd_shard->hash_size ) / mcd_shard->lock_bktsize ) );
    osd_state->osd_wait =
        fthLock( (fthLock_t *)osd_state->osd_lock, 0, NULL );

    if ( mcd_shard->use_fifo ) {
        ret = mcd_fth_osd_fifo_get( (void *)pctxt,              // context
                                    mcd_shard,
                                    key,
                                    metaData->keyLen,
                                    (void **)dataPtr,
                                    &actual_size,
                                    flags,
                                    metaData,
                                    syndrome );
    }
    else {
        ret = mcd_fth_osd_slab_get( (void *)pctxt,              // context
                                    mcd_shard,
                                    key,
                                    metaData->keyLen,
                                    (void **)dataPtr,
                                    &actual_size,
                                    flags,
                                    metaData,
                                    syndrome );
    }

    fthUnlock( osd_state->osd_wait );

    metaData->dataLen = actual_size;
    return ret;
}


int
mcd_osd_flash_put( struct ssdaio_ctxt * pctxt, struct shard * shard,
                        struct objMetaData * metaData, char * key,
                        char * data, int flags )
{
    int                         ret;
    int                         window = 0;
    uint64_t                    syndrome;
    osd_state_t               * osd_state = (osd_state_t *)pctxt;
    mcd_osd_shard_t           * mcd_shard = (mcd_osd_shard_t *)shard;

    mcd_log_msg( 20406, PLAT_LOG_LEVEL_DEBUG, "ENTERING, context=%p shardID=%lu",
                 (void *)pctxt, shard->shardID );

    syndrome = hash( (unsigned char *)key, metaData->keyLen, 0 );

    osd_state->osd_lock = (void *)( mcd_shard->bucket_locks +
        ( ( syndrome % mcd_shard->hash_size ) / mcd_shard->lock_bktsize ) );
    osd_state->osd_wait =
        fthLock( (fthLock_t *)osd_state->osd_lock, 1, NULL );

    if ( mcd_shard->use_fifo ) {
        ret = mcd_fth_osd_fifo_set( (void *)pctxt,
                                    mcd_shard,
                                    key,
                                    metaData->keyLen,
                                    data,
                                    metaData->dataLen,
                                    flags,
                                    metaData,
                                    syndrome );
    }
    else {
        if ( (shard->flags & FLASH_SHARD_SEQUENCE_EXTERNAL) == 0 ) {
            metaData->sequence =
                __sync_add_and_fetch( &mcd_shard->sequence, 1 );
            window = backup_incr_pending_seqno( mcd_shard );
        }

        ret = mcd_fth_osd_slab_set( (void *)pctxt,
                                    mcd_shard,
                                    key,
                                    metaData->keyLen,
                                    data,
                                    metaData->dataLen,
                                    flags,
                                    metaData,
                                    syndrome );

        if ( (shard->flags & FLASH_SHARD_SEQUENCE_EXTERNAL) != 0 &&
             ret == FLASH_EOK ) {
            bool success = false;
            while ( !success && metaData->sequence > mcd_shard->sequence ) {

                uint64_t old_value = mcd_shard->sequence;
                success = __sync_bool_compare_and_swap( &mcd_shard->sequence,
                                                        old_value,
                                                        metaData->sequence );
            }
        }
        if ( (shard->flags & FLASH_SHARD_SEQUENCE_EXTERNAL) == 0 ) {
            backup_decr_pending_seqno( mcd_shard, window );
        }
    }

    fthUnlock( osd_state->osd_wait );

    return ret;
}


int
mcd_osd_shard_backup_prepare( struct shard * shard, int full_backup,
                              uint32_t client_version,
                              uint32_t * server_version )
{
    mcd_bak_msg( 40120, PLAT_LOG_LEVEL_DEBUG,
                 "ENTERING, shardID=%lu, full=%d, vers=%d",
                 shard->shardID, full_backup, client_version );

    // check for compatibility
    if ( !MCD_BAK_BACKUP_PROTOCOL_COMPAT_V4( client_version ) ) {
        *server_version = MCD_BAK_BACKUP_PROTOCOL_VERSION;
        return FLASH_EINCONS;
    }

    // always return compatible version
    *server_version = client_version;

    // do command
    return backup_start_prepare( (mcd_osd_shard_t *)shard, full_backup );
}


int
mcd_osd_shard_backup( struct shard * shard, int full_backup,
                      int cancel, int complete, uint32_t client_version,
                      uint32_t * server_version, uint64_t * prev_seqno,
                      uint64_t * backup_seqno, time_t * backup_time )
{
    int                         rc = 0;

    mcd_bak_msg( 20408, PLAT_LOG_LEVEL_DEBUG,
                 "ENTERING, shardID=%lu, full=%d, can=%d, com=%d",
                 shard->shardID, full_backup, cancel, complete );

    // check for compatibility
    if ( !MCD_BAK_BACKUP_PROTOCOL_COMPAT_V4( client_version ) ) {
        *server_version = MCD_BAK_BACKUP_PROTOCOL_VERSION;
        return FLASH_EINCONS;
    }

    // always return compatible version
    *server_version = client_version;

    // do command
    if ( cancel || complete ) {
        rc = backup_end( (mcd_osd_shard_t *)shard, cancel );
    } else {
        rc = backup_start( (mcd_osd_shard_t *)shard, full_backup, prev_seqno,
                           backup_seqno, backup_time );
    }

    return rc;
}


int
mcd_osd_shard_restore( struct shard * shard, uint64_t prev_seqno,
                       uint64_t curr_seqno, int cancel, int complete,
                       uint32_t client_version, uint32_t * server_version,
                       uint64_t * err_seqno )
{
    int                         rc = 0;

    mcd_bak_msg( 20409, PLAT_LOG_LEVEL_DEBUG,
                 "ENTERING, shardID=%lu, pseq=%lu, cseq=%lu, can=%d, com=%d",
                 shard->shardID, prev_seqno, curr_seqno, cancel, complete );

    // check for compatibility
    if ( !MCD_BAK_RESTORE_PROTOCOL_COMPAT_V4( client_version ) ) {
        *server_version = MCD_BAK_RESTORE_PROTOCOL_VERSION;
        return FLASH_EINCONS;
    }

    // always return compatible version
    *server_version = client_version;

    // do command
    if ( cancel || complete ) {
        rc = restore_end( (mcd_osd_shard_t *)shard, cancel );
        if ( complete && 0 == rc ) {
            log_sync( (mcd_osd_shard_t *)shard ); // make sure restore sticks
        }
    } else {
        rc = restore_start( (mcd_osd_shard_t *)shard, prev_seqno, curr_seqno,
                            client_version, err_seqno );
    }

    return rc;
}


int
mcd_osd_shard_delete( struct shard * lshard )
{
    int                         s, rc;
    int                         total_segments;
    fthWaitEl_t               * wait;
    mcd_osd_shard_t           * shard = (mcd_osd_shard_t *)lshard;
    struct flashDev           * dev = lshard->dev;
    struct shard             ** prevPtr;

    mcd_log_msg( 20065, PLAT_LOG_LEVEL_DEBUG,
                "ENTERING, shardID=%lu", shard->id );

    if (shard->flush_fd > 0) {
        close(shard->flush_fd);
    }

    // remove shard and its properties from superblock
    // including non-persistent shards' properties
    rc = shard_unformat( shard->id );
    if ( rc != 0 ) {
        return rc;
    }

    // remove shard from flashDev shardList
    wait = fthLock( &dev->lock, 1, NULL );
    prevPtr = &dev->shardList;
    while ( *prevPtr != lshard ) {
        prevPtr = &((*prevPtr)->next);
    }
    *prevPtr = ((*prevPtr)->next);
    fthUnlock( wait );

    // remove from Mcd_osd_slab_shards[]
    for ( s = 0; s < MCD_OSD_MAX_NUM_SHARDS; s++ ) {
        if ( shard == Mcd_osd_slab_shards[s] ) {
            Mcd_osd_slab_shards[s] = NULL;
            break;
        }
    }

    // persistent container cleanup
    if ( shard->persistent ) {
        // kill log writer, free all persistence data structures
        shard_unrecover( shard );
    }

    // find the total number of segments
    if ( shard->use_fifo ) {
        total_segments = shard->total_segments;
    } else {
        total_segments = shard->total_segments +
            (shard->data_blk_offset / Mcd_osd_segment_blks);
    }

    // return all segments to free segment list,
    // including metadata segments from persistent shards
    mcd_osd_shard_free_segments( shard, total_segments );

    // shard uninit
    mcd_osd_shard_uninit( shard );

    // backup
    if ( shard->backup ) {
        plat_free( shard->backup );
    }

    // replication seqno cache
    if ( shard->logbuf_seqno_cache ) {
        plat_free( shard->logbuf_seqno_cache );
    }

    // shard segments
    if ( shard->segments ) {
        plat_free( shard->segments );
    }

    #ifdef notdef
    // delete reference to shard, but leave the mcd_container struct intact
    if ( shard->cntr ) {
        shard->cntr->shard = NULL;
    }
    #endif

    // clear mcd_container structure
    memset( (void *) shard->cntr, 0, sizeof(mcd_container_t) );

    // free the shard
    plat_free( shard );

    return(0);
}

int
mcd_osd_shard_start( struct shard * shard )
{
    mcd_log_msg( 20065, PLAT_LOG_LEVEL_DEBUG,
                 "ENTERING, shardID=%lu", shard->shardID );
    return(0);
}

int mcd_osd_container_state(mcd_container_t *container)
{
    return(container->state);
}

void mcd_osd_set_container_state(mcd_container_t *container, int state)
{
    container->state = state;
}

uint32_t mcd_osd_container_generation(mcd_container_t *container)
{
    return(container->generation);
}

int mcd_osd_container_tcp_port(mcd_container_t *container)
{
    return(container->tcp_port);
}

char *mcd_osd_container_cluster_name(mcd_container_t *container)
{
    return(container->cluster_name);
}

int mcd_osd_container_cas_num_nodes(mcd_container_t *container)
{
    return(container->cas_num_nodes);
}

int mcd_osd_container_eviction(mcd_container_t *container)
{
    return(container->eviction);
}

int mcd_osd_container_udp_port(mcd_container_t *container)
{
    return(container->udp_port);
}

SDF_cguid_t mcd_osd_container_cguid(mcd_container_t *container)
{
    return(container->cguid);
}

SDFContainer mcd_osd_container_sdf_container(mcd_container_t *container)
{
    return(&(container->sdf_container));
}

void *mcd_osd_container_shard(mcd_container_t *container)
{
    return(container->shard);
}

struct in_addr *mcd_osd_container_ip_addrs(mcd_container_t *container)
{
    return(container->ip_addrs);
}

void mcd_osd_set_container_ip_addrs(mcd_container_t *container, int i, struct in_addr addr)
{
    container->ip_addrs[i] = addr;
}

void mcd_osd_set_container_ip_s_addr(mcd_container_t *container, int i, int addr)
{
    container->ip_addrs[i].s_addr = addr;
}

int mcd_osd_container_num_ips(mcd_container_t *container)
{
    return(container->num_ips);
}

void mcd_osd_set_container_num_ips(mcd_container_t *container, int num_ips)
{
    container->num_ips = num_ips;
}

char *mcd_osd_container_cname(mcd_container_t *container)
{
    return(container->cname);
}

void mcd_osd_container_set_stopped(mcd_container_t *container)
{
    container->state = cntr_stopped;
}

int mcd_osd_container_persistent(mcd_container_t *container)
{
    return(container->persistent);
}

bool mcd_osd_container_sasl(mcd_container_t *container)
{
    return(container->sasl);
}

void mcd_osd_set_container_sasl(mcd_container_t *container, bool sasl)
{
    container->sasl = sasl;
}

bool mcd_osd_container_prefix_delete(mcd_container_t *container)
{
    return(container->prefix_delete);
}

void mcd_osd_set_container_prefix_delete(mcd_container_t *container, bool prefix_delete)
{
    container->prefix_delete = prefix_delete;
}

void *mcd_osd_container_pfx_deleter(mcd_container_t *container)
{
    return(container->pfx_deleter);
}

void mcd_osd_set_container_pfx_deleter(mcd_container_t *container, void * pfx_deleter)
{
    container->pfx_deleter = pfx_deleter;
}

int mcd_osd_container_hot_key_stats(mcd_container_t *container)
{
    return(container->hot_key_stats);
}

void *mcd_osd_container_hot_key_reporter(mcd_container_t *container)
{
    return(container->hot_key_reporter);
}

int mcd_osd_container_max_hot_keys(mcd_container_t *container)
{
    return(container->max_hot_keys);
}

void mcd_osd_set_container_hot_key_reporter(mcd_container_t *container, void *hot_key_reporter)
{
    container->hot_key_reporter = hot_key_reporter;
}

void mcd_osd_set_container_max_hot_keys(mcd_container_t *container, int max_hot_keys)
{
    container->max_hot_keys = max_hot_keys;
}

void mcd_osd_set_container_hot_key_stats(mcd_container_t *container, int hot_key_stats)
{
    container->hot_key_stats = hot_key_stats;
}

bool mcd_osd_container_binary_tracing(mcd_container_t *container)
{
    return(container->binary_tracing);
}

void mcd_osd_set_container_binary_tracing(mcd_container_t *container, bool binary_tracing)
{
    container->binary_tracing = binary_tracing;
}

int mcd_osd_container_container_id(mcd_container_t *container)
{
    return(container->container_id);
}

uint64_t mcd_osd_container_size_quota(mcd_container_t *container)
{
    return(container->size_quota);
}

uint32_t mcd_osd_container_obj_quota(mcd_container_t *container)
{
    return(container->obj_quota);
}

int mcd_osd_container_sync_backup(mcd_container_t *container)
{
    return(container->sync_backup);
}

void mcd_osd_set_container_sync_backup(mcd_container_t *container, int sync_backup)
{
    container->sync_backup = sync_backup;
}

int mcd_osd_container_sync_updates(mcd_container_t *container)
{
    return(container->sync_updates);
}

void mcd_osd_set_container_sync_updates(mcd_container_t *container, int sync_updates)
{
    container->sync_updates = sync_updates;
}

uint32_t mcd_osd_container_sync_msec(mcd_container_t *container)
{
    return(container->sync_msec);
}

int mcd_osd_container_defunct(mcd_container_t *container)
{
    return(container->defunct);
}

void mcd_osd_set_container_defunct(mcd_container_t *container, int defunct)
{
    container->defunct = defunct;
}

int mcd_osd_container_need_reinstate(mcd_container_t *container)
{
    return(container->need_reinstate);
}

void mcd_osd_set_container_need_reinstate(mcd_container_t *container, int need_reinstate)
{
    container->need_reinstate = need_reinstate;
}

time_t mcd_osd_container_flush_time(mcd_container_t *container)
{
    return(container->flush_time);
}

void mcd_osd_set_container_flush_time(mcd_container_t *container, time_t flush_time)
{
    container->flush_time = flush_time;
}

uint64_t mcd_osd_container_cas_id(mcd_container_t *container)
{
    return(container->cas_id);
}

uint64_t mcd_osd_container_incr_cas_id( mcd_container_t *container, uint64_t amount)
{
    return __sync_add_and_fetch( &container->cas_id,
                                 amount );
}

int mcd_osd_container_prev_state(mcd_container_t *container)
{
    return(container->prev_state);
}


void mcd_osd_incr_container_ref_count(mcd_container_t *container)
{
    (void) __sync_fetch_and_add( &container->ref_count, 1 );
}

void mcd_osd_check_for_stopped_container(mcd_container_t *container)
{
    uint32_t nref;
    uint32_t pending;

    nref = __sync_sub_and_fetch( &container->ref_count, 1 );
    if ( cntr_stopping == container->state && 0 == nref ) {

	pending =
	    __sync_fetch_and_add( &Mcd_adm_pending_mails, 1 );

	if ( 0 == pending && cntr_stopping == container->state ) {
	    fthMboxPost( &Mcd_fth_admin_mbox, cntr_stopping );
	    mcd_log_msg( 20141, PLAT_LOG_LEVEL_DEBUG,
			 "container stopping mail posted" );
	}
	else {
	    (void)
		__sync_fetch_and_sub( &Mcd_adm_pending_mails, 1 );
	}
    }
}

SDF_status_t mcd_fth_container_start( void * context,
                                      mcd_container_t * container )
{
    SDF_status_t                status;
    SDF_action_init_t         * pai = (SDF_action_init_t *)context;

    mcd_log_msg( 20178, PLAT_LOG_LEVEL_INFO,
                 "ENTERING, port=%d", mcd_osd_container_tcp_port(container) );

    status = SDFStartContainer((struct SDF_thread_state *) pai, mcd_osd_container_cguid(container));
    if ( SDF_SUCCESS != status ) {
        mcd_log_msg( 20179, PLAT_LOG_LEVEL_ERROR,
                     "SDFStartContainer() failed, status=%s",
                     SDF_Status_Strings[status] );
    }

    return status;
}


SDF_status_t mcd_fth_container_stop( void * context,
                                     mcd_container_t * container )
{
    SDF_status_t                status;
    SDF_action_init_t         * pai = (SDF_action_init_t *)context;

    mcd_log_msg( 20178, PLAT_LOG_LEVEL_INFO,
                 "ENTERING, port=%d", mcd_osd_container_tcp_port(container));

    status = SDFStopContainer((struct SDF_thread_state *) pai, mcd_osd_container_cguid(container));
    if ( SDF_SUCCESS != status ) {
        mcd_log_msg( 20180, PLAT_LOG_LEVEL_ERROR,
                     "SDFStopContainer() failed, status=%s",
                     SDF_Status_Strings[status] );
    }

    return status;
}


int mcd_stop_container( void *pai, mcd_container_t * container )
{
    int                         rc = -1;
    SDF_status_t                status;
    uint32_t                    ref_count;
    uint64_t                    mail;

    mcd_log_msg( 20718, PLAT_LOG_LEVEL_INFO, "stopping container, port=%d",
                 container->tcp_port );

    container->state = cntr_stopping;
    ref_count = __sync_fetch_and_or( &container->ref_count, 0 );

    if ( 0 != ref_count ) {
        mail = fthMboxWait( &Mcd_fth_admin_mbox );
        if ( cntr_stopping != mail ) {
            mcd_log_msg( 20410, PLAT_LOG_LEVEL_ERROR,
                         "invalid mail, expecting %d received %lu",
                         cntr_stopping, mail );
            goto out_stop;
        }
        mcd_log_msg( 20411, PLAT_LOG_LEVEL_DEBUG, "admin mail received" );
    }

    status = mcd_fth_container_stop( pai, container );
    if ( SDF_SUCCESS != status ) {
        mcd_log_msg( 20719, PLAT_LOG_LEVEL_ERROR,
                     "failed to stop container, status=%s",
                     SDF_Status_Strings[status] );
        goto out_stop;
    }

    rc = mcd_osd_shard_set_state( container->shard, cntr_stopped );
    if ( 0 != rc ) {
        mcd_log_msg( 20716, PLAT_LOG_LEVEL_ERROR,
                     "failed to set container state, rc=%d", rc );
        goto out_stop;
    }

    container->state = cntr_stopped;
    mcd_log_msg( 20720, PLAT_LOG_LEVEL_INFO, "container stopped, port=%d",
                 container->tcp_port );

    rc = 0;     /* SUCCESS */

 out_stop:
    if ( 0 != ref_count ) {
        (void) __sync_fetch_and_sub( &Mcd_adm_pending_mails, 1 );
    }
    return rc;
}

/*
 * FIXME: obsolete now and to be removed
 */
int mcd_start_container_internal( void * pai, int tcp_port )
{
    int                         rc;
    SDF_status_t                status;
    mcd_container_t           * container = NULL;

    mcd_log_msg( 20714, PLAT_LOG_LEVEL_INFO, "starting container, port=%d",
                 tcp_port );

    if ( flash_settings.ips_per_cntr ) {
        plat_abort();
    }

    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
        if ( tcp_port == Mcd_containers[i].tcp_port ) {
            container = &Mcd_containers[i];
        }
    }
    if ( NULL == container ) {
        return -ENOENT;
    }

    if ( cntr_running == container->state ) {
        return 0;
    }
    if ( cntr_stopping == container->state ) {
        return -EBUSY;
    }
    if ( cntr_stopped != container->state ) {
        return -EINVAL;
    }

    status = mcd_fth_container_start( pai, container );
    if ( SDF_SUCCESS != status ) {
        mcd_log_msg( 20715, PLAT_LOG_LEVEL_ERROR,
                     "failed to start container, status=%s",
                     SDF_Status_Strings[status] );
        return -1;
    }

    rc = mcd_osd_shard_set_state( container->shard, cntr_running );
    if ( 0 != rc ) {
        mcd_log_msg( 20716, PLAT_LOG_LEVEL_ERROR,
                     "failed to set container state, rc=%d", rc );
        return rc;
    }

    container->state = cntr_running;
    mcd_log_msg( 20717, PLAT_LOG_LEVEL_INFO,
                 "container started, port=%d", container->tcp_port );

    return 0;   /* SUCCESS */
}

mcd_container_t *mcd_osd_container_from_cguid(
	SDF_cguid_t cguid
	)
{
    mcd_container_t * container = NULL;

    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
        if ( cguid == Mcd_containers[i].cguid ) {
            container = &Mcd_containers[i];
            break;
        }
    }

    return container;
}

int mcd_start_container_byname_internal( void * pai, char * cname )
{
    int                         rc;
    SDF_status_t                status;
    mcd_container_t           * container = NULL;

    mcd_log_msg( 50007, PLAT_LOG_LEVEL_INFO,
                 "starting container, name=%s", cname );

    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
        if ( 0 == strncmp( cname, Mcd_containers[i].cname,
                           MCD_CNTR_NAME_MAXLEN ) ) {
            container = &Mcd_containers[i];
            break;
        }
    }
    if ( NULL == container ) {
        return -ENOENT;
    }

    if ( cntr_running == container->state ) {
        return 0;
    }
    if ( cntr_stopping == container->state ) {
        return -EBUSY;
    }
    if ( cntr_stopped != container->state ) {
        return -EINVAL;
    }

    status = mcd_fth_container_start( pai, container );
    if ( SDF_SUCCESS != status ) {
        mcd_log_msg( 20715, PLAT_LOG_LEVEL_ERROR,
                     "failed to start container, status=%s",
                     SDF_Status_Strings[status] );
        return -1;
    }

    rc = mcd_osd_shard_set_state( container->shard, cntr_running );
    if ( 0 != rc ) {
        mcd_log_msg( 20716, PLAT_LOG_LEVEL_ERROR,
                     "failed to set container state, rc=%d", rc );
        return rc;
    }

    container->state = cntr_running;
    mcd_log_msg( 20717, PLAT_LOG_LEVEL_INFO,
                 "container started, port=%d", container->tcp_port );

    return 0;   /* SUCCESS */
}

/*
 * FIXME: obsolete now and to be removed
 */
int mcd_stop_container_internal( void * pai, int tcp_port )
{
    int                         rc = -1;
    SDF_status_t                status;
    uint32_t                    ref_count;
    uint64_t                    mail;
    mcd_container_t           * container = NULL;

    mcd_log_msg( 20718, PLAT_LOG_LEVEL_INFO, "stopping container, port=%d",
                 tcp_port );

    if ( flash_settings.ips_per_cntr ) {
        plat_abort();
    }

    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
        if ( tcp_port == Mcd_containers[i].tcp_port ) {
            container = &Mcd_containers[i];
        }
    }
    if ( NULL == container ) {
        return -ENOENT;
    }

    container->state = cntr_stopping;
    ref_count = __sync_fetch_and_or( &container->ref_count, 0 );

    if ( 0 != ref_count ) {
        mail = fthMboxWait( &Mcd_fth_admin_mbox );
        if ( cntr_stopping != mail ) {
            mcd_log_msg( 20410, PLAT_LOG_LEVEL_ERROR,
                         "invalid mail, expecting %d received %lu",
                         cntr_stopping, mail );
            goto out_stop;
        }
        mcd_log_msg( 20411, PLAT_LOG_LEVEL_DEBUG, "admin mail received" );
    }

    status = mcd_fth_container_stop( pai, container );
    if ( SDF_SUCCESS != status ) {
        mcd_log_msg( 20719, PLAT_LOG_LEVEL_ERROR,
                     "failed to stop container, status=%s",
                     SDF_Status_Strings[status] );
        goto out_stop;
    }

    rc = mcd_osd_shard_set_state( container->shard, cntr_stopped );
    if ( 0 != rc ) {
        mcd_log_msg( 20716, PLAT_LOG_LEVEL_ERROR,
                     "failed to set container state, rc=%d", rc );
        goto out_stop;
    }

    container->state = cntr_stopped;
    mcd_log_msg( 20720, PLAT_LOG_LEVEL_INFO, "container stopped, port=%d",
                 container->tcp_port );

    rc = 0;     /* SUCCESS */

 out_stop:
    if ( 0 != ref_count ) {
        (void) __sync_fetch_and_sub( &Mcd_adm_pending_mails, 1 );
    }
    return rc;
}


int mcd_stop_container_byname_internal( void * pai, char * cname )
{
    int                         rc = -1;
    SDF_status_t                status;
    uint32_t                    ref_count;
    uint64_t                    mail;
    mcd_container_t           * container = NULL;

    mcd_log_msg( 50008, PLAT_LOG_LEVEL_INFO,
                 "stopping container, name=%s", cname );

    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {
        if ( 0 == strncmp( cname, Mcd_containers[i].cname,
                           MCD_CNTR_NAME_MAXLEN ) ) {
            container = &Mcd_containers[i];
            break;
        }
    }
    if ( NULL == container ) {
        return -ENOENT;
    }

    container->state = cntr_stopping;
    ref_count = __sync_fetch_and_or( &container->ref_count, 0 );

    if ( 0 != ref_count ) {
        mail = fthMboxWait( &Mcd_fth_admin_mbox );
        if ( cntr_stopping != mail ) {
            mcd_log_msg( 20410, PLAT_LOG_LEVEL_ERROR,
                         "invalid mail, expecting %d received %lu",
                         cntr_stopping, mail );
            goto out_stop;
        }
        mcd_log_msg( 20411, PLAT_LOG_LEVEL_DEBUG, "admin mail received" );
    }

    status = mcd_fth_container_stop( pai, container );
    if ( SDF_SUCCESS != status ) {
        mcd_log_msg( 20719, PLAT_LOG_LEVEL_ERROR,
                     "failed to stop container, status=%s",
                     SDF_Status_Strings[status] );
        goto out_stop;
    }

    rc = mcd_osd_shard_set_state( container->shard, cntr_stopped );
    if ( 0 != rc ) {
        mcd_log_msg( 20716, PLAT_LOG_LEVEL_ERROR,
                     "failed to set container state, rc=%d", rc );
        goto out_stop;
    }

    container->state = cntr_stopped;
    mcd_log_msg( 20720, PLAT_LOG_LEVEL_INFO, "container stopped, port=%d",
                 container->tcp_port );

    rc = 0;     /* SUCCESS */

 out_stop:
    if ( 0 != ref_count ) {
        (void) __sync_fetch_and_sub( &Mcd_adm_pending_mails, 1 );
    }
    return rc;
}

int
mcd_osd_shard_stop( struct shard * shard )
{
    uint64_t                    mail;
    mcd_osd_shard_t           * mcd_shard = (mcd_osd_shard_t *)shard;

    mcd_log_msg( 20065, PLAT_LOG_LEVEL_DEBUG,
                 "ENTERING, shardID=%lu", shard->shardID );

    /*
     * bounce a fthmail through the FIFO writer to ensure that
     * all pending writer mails for this shard are cleared
     */
    if ( 1 == mcd_shard->use_fifo ) {
        if ( flash_settings.multi_fifo_writers ) {
            while ( 0 != mcd_shard->fifo.fth_count ) {
                fthYield( 1 );
            }
        }
        fthMboxPost( &Mcd_osd_writer_mbox, (uint64_t) &Mcd_fth_admin_mbox );
        mail = fthMboxWait( &Mcd_fth_admin_mbox );
        if ( cntr_stopping != mail ) {
            mcd_log_msg( 20410, PLAT_LOG_LEVEL_ERROR,
                         "invalid mail, expecting %d received %lu",
                         cntr_stopping, mail );
            return -EINVAL;
        }
        mcd_log_msg( 20411, PLAT_LOG_LEVEL_DEBUG, "admin mail received" );
    }

    // sync persistent shard and wait for object table update to complete
    if ( 1 == ((mcd_osd_shard_t *)shard)->persistent ) {
        log_sync( (mcd_osd_shard_t *)shard );
        log_wait( (mcd_osd_shard_t *)shard );
    }

    return(0);
}

int mcd_osd_shard_set_flush_time( struct shard * shard, time_t new_time )
{
    mcd_osd_shard_t           * mcd_shard = (mcd_osd_shard_t *)shard;

    mcd_log_msg( 20065, PLAT_LOG_LEVEL_DEBUG,
                 "ENTERING, shardID=%lu", shard->shardID );

    // set new time in container
    mcd_shard->cntr->flush_time = new_time;

    //set properties will pick up new time
    return shard_set_properties( (mcd_osd_shard_t *)shard, mcd_shard->cntr );
}

int
mcd_osd_shard_set_state( struct shard * shard, int new_state )
{
    mcd_log_msg( 20065, PLAT_LOG_LEVEL_DEBUG,
                 "ENTERING, shardID=%lu", shard->shardID );

    // set new state
    return shard_set_state( (mcd_osd_shard_t *)shard, new_state );
}

int
mcd_osd_shard_get_properties( int index, mcd_container_t * cntr )
{
    mcd_log_msg( 20412, PLAT_LOG_LEVEL_DEBUG,
                 "ENTERING, index=%d", index );

    return shard_get_properties( index, cntr );
}

int
mcd_osd_shard_set_properties( struct shard * shard, mcd_container_t * cntr )
{
    mcd_log_msg( 20065, PLAT_LOG_LEVEL_DEBUG,
                 "ENTERING, shardID=%lu", shard->shardID );

    return shard_set_properties( (mcd_osd_shard_t *)shard, cntr );
}

void
mcd_osd_recovery_stats( mcd_osd_shard_t *shard, char ** ppos, int * lenp )
{
    uint64_t                    elapsed_usec = 0;
    struct timeval              tv;

    plat_snprintfcat( ppos, lenp,
                      "STAT flash_rec_num_updates %lu\r\n",
                      shard->rec_num_updates );

    if ( shard->rec_upd_running ) {
        fthGetTimeOfDay( &tv );
        elapsed_usec =
            (tv.tv_sec * PLAT_MILLION) + tv.tv_usec - shard->rec_upd_usec;
    } else {
        elapsed_usec = shard->rec_upd_usec;
    }

    plat_snprintfcat( ppos, lenp,
                      "STAT flash_rec_update_running %lu %lu %lu\r\n",
                      shard->rec_upd_running, elapsed_usec,
                      shard->rec_upd_prev_usec );

    plat_snprintfcat( ppos, lenp,
                      "STAT flash_rec_update_io %lu %lu %lu %lu %lu %lu\r\n",
                      shard->rec_log_reads, shard->rec_table_reads,
                      shard->rec_table_writes, shard->rec_log_reads_cum,
                      shard->rec_table_reads_cum, shard->rec_table_writes_cum);

    shard_recovery_stats( shard, ppos, lenp );
}

void
mcd_osd_register_ops( void )
{
    /*
     * register mcd_osd functions
     */
    Ssd_fifo_ops.flashOpen              = mcd_osd_flash_open;
    Ssd_fifo_ops.shardCreate            = mcd_osd_shard_create;
    Ssd_fifo_ops.shardOpen              = mcd_osd_shard_open;
    Ssd_fifo_ops.flashGet               = mcd_osd_flash_get;
    Ssd_fifo_ops.flashPut               = mcd_osd_flash_put;
    Ssd_fifo_ops.flashFreeBuf           = mcd_osd_release_buf;
    Ssd_fifo_ops.flashStats             = mcd_osd_shard_get_stats;
    Ssd_fifo_ops.shardSync              = mcd_osd_shard_sync;
    Ssd_fifo_ops.shardDelete            = mcd_osd_shard_delete;
    Ssd_fifo_ops.shardStart             = mcd_osd_shard_start;
    Ssd_fifo_ops.shardStop              = mcd_osd_shard_stop;
    Ssd_fifo_ops.flashGetHighSequence   = mcd_osd_slab_flash_get_high_sequence;
    Ssd_fifo_ops.flashSetSyncedSequence =
        mcd_osd_slab_flash_set_synced_sequence;
    Ssd_fifo_ops.flashGetRetainedTombstoneGuarantee =
        mcd_osd_slab_flash_get_rtg;
    Ssd_fifo_ops.flashRegisterSetRetainedTombstoneGuaranteeCallback =
        mcd_osd_slab_flash_register_set_rtg_callback;

    mcd_log_msg( 20413, PLAT_LOG_LEVEL_INFO, "mcd_osd ops registered" );

    return;
}


inline int
mcd_osd_get_object_overhead( void )
{
    return sizeof( mcd_osd_meta_t );
}


SDF_status_t
mcd_osd_raw_set( osd_state_t     *osd_state, 
                 mcd_osd_shard_t *shard, 
		 uint64_t         initial, 
		 uint8_t          keyLen, 
		 char            *key, 
		 void            *raw_data,
		 SDF_size_t       raw_len )
{
    int                         rc;
    uint64_t                    syndrome;
    char                      * data;
    mcd_osd_meta_t            * meta;
    struct objMetaData          metaData;

    mcd_bak_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

    /*
     * FIXME: only persistent containers are supported currently
     */
    if ( 1 == shard->use_fifo || 0 == shard->persistent ) {
        mcd_bak_msg( 20414, PLAT_LOG_LEVEL_ERROR, "not supported yet" );
        return SDF_UNSUPPORTED_REQUEST;
    }

    if ( 0 == shard->restore_running ) {
        return SDF_UNAVAILABLE;
    }

    #ifdef notdef
    // ripped out to remove dependency on object_data_t!
    if ( sizeof(mcd_osd_meta_t) + sizeof(object_data_t) + 2 > raw_len ) {
        mcd_bak_msg( 20415, PLAT_LOG_LEVEL_ERROR,
                     "invalid raw data size, len=%lu", raw_len );
        return SDF_INVALID_PARAMETER;
    }
    #endif

    if ( sizeof(mcd_osd_meta_t) != initial ) {
        mcd_bak_msg( 40121, PLAT_LOG_LEVEL_ERROR,
                     "invalid key offset %lu", initial );
        return SDF_INVALID_PARAMETER;
    }

    meta = (mcd_osd_meta_t *)raw_data;
    mcd_meta_ntoh( meta );
    if ( meta->key_len != keyLen || raw_len !=
         sizeof(mcd_osd_meta_t) + meta->key_len + meta->data_len ) {
        mcd_bak_msg( 20416, PLAT_LOG_LEVEL_ERROR,
                     "key/raw_data size mismatch, mkey_len=%d rkey_len=%d "
                     "md_len=%u rd_len=%lu",
                     meta->key_len, keyLen, meta->data_len, raw_len );
        return SDF_INVALID_PARAMETER;
    }

    memset( (void *)&metaData, 0, sizeof(metaData) );
    metaData.expTime = meta->expiry_time;
    metaData.createTime = meta->create_time;
    metaData.sequence = meta->seqno;

    data = raw_data + sizeof(mcd_osd_meta_t) + meta->key_len;

    if ( restore_requires_conversion( shard ) ) {
        (*flash_settings.convert_object_data)(data);
    }
    osd_set_shard_cas_id_if_higher( (struct shard *) shard, data );

    memcpy( key, raw_data + sizeof(mcd_osd_meta_t), keyLen );
    syndrome = hash( (unsigned char *)key, keyLen, 0 );

    osd_state->osd_lock = (void *)( shard->bucket_locks +
        ( ( syndrome % shard->hash_size ) / shard->lock_bktsize ) );
    osd_state->osd_wait =
        fthLock( (fthLock_t *)osd_state->osd_lock, 1, NULL );

    rc = mcd_fth_osd_slab_set( (void *)osd_state,  // context
                               shard,
                               key,
                               keyLen,
                               data,
                               meta->data_len,
                               FLASH_PUT_RESTORE | FLASH_PUT_IF_NEWER, // flags
                               &metaData,                     // meta_data
                               syndrome );

    fthUnlock( osd_state->osd_wait );

    return flash_to_sdf_status( rc );
}


static int
mcd_osd_read_bad_segment( osd_state_t * ctxt, mcd_osd_shard_t * shard,
                          mcd_osd_segment_t * segment, char * buf,
                          uint64_t seg_offset, int blks, int num_blks,
                          bool recurse )
{
    int                         i, rc;
    int                         count = 0;
    uint64_t                    blk_offset;
    uint64_t                    tmp_offset;
    uint64_t                    offset;

    /*
     * Break up a segment that got an I/O error into 1MB I/Os. If an error
     * occurs, this routine calls itself and breaks up the I/O's into the
     * same size as the segment's block size. An error will correspond to an
     * individual object. Put a marker for each such object in the segment
     * buffer. If that location was to be backed up (bit on in bitmap), it
     * will be counted as an error.
     */

    mcd_bak_msg( 40125, PLAT_LOG_LEVEL_DEBUG,
                 "Reading bad raw segment (%lu), blksize=%d, "
                 "seg_offset=%lu, num_blks=%d, blks=%d, rec=%s",
                 segment->blk_offset, segment->class->slab_blksize,
                 seg_offset, num_blks, blks, recurse ? "true" : "false" );

    // read segment, or partial segment
    for ( i = num_blks - blks; i >= 0; i -= blks ) {

        blk_offset = seg_offset + i;
        tmp_offset = shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
            + (blk_offset % Mcd_osd_rand_blksize);
        offset = tmp_offset * Mcd_osd_blk_size;

        rc = mcd_fth_aio_blk_read( (void *)ctxt,
                                   buf + (i * Mcd_osd_blk_size),
                                   offset,
                                   blks * Mcd_osd_blk_size );
        if ( rc != 0 ) {
            // error reading 1MB of this segment
            if ( recurse ) {
                mcd_bak_msg( 40126, PLAT_LOG_LEVEL_ERROR,
                             "Error reading bad raw segment (%lu), "
                             "seg_offset=%lu, blk_offset=%lu, blks=%d, "
                             "rc=%d",
                             segment->blk_offset, seg_offset, blk_offset,
                             blks, rc );

                // call myself reading this 1MB in segment-block-size pieces
                rc = mcd_osd_read_bad_segment( ctxt, shard, segment,
                                               buf + (i * Mcd_osd_blk_size),
                                               blk_offset,
                                               segment->class->slab_blksize,
                                               blks, false );

                mcd_bak_msg( 40127, PLAT_LOG_LEVEL_DEBUG,
                             "Bad block(s) found in raw segment (%lu), "
                             "seg_offset=%lu, count=%d",
                             segment->blk_offset, seg_offset,
                             rc * segment->class->slab_blksize ); //#error blks
            }
            // error reading a block-sized piece of a segment
            else {
                mcd_bak_msg( PLAT_LOG_ID_INITIAL, PLAT_LOG_LEVEL_TRACE,
                             "Giving up on raw segment (%lu), seg_offset=%lu, "
                             "blk_offset=%lu, blks=%d, rc=%d",
                             segment->blk_offset, seg_offset, blk_offset,
                             blks, rc );

                // mark this object as dead
                * (uint32_t *)(buf + (i * Mcd_osd_blk_size )) =
                    MCD_BAK_DEAD_OBJECT_MAGIC;
                count++;  // count dead objects
            }
        }
    }

    // at the bottom of the recursion, return the count
    // of block-sized errors for statistical purposes.
    if ( !recurse ) {
        return count;
    }

    return 0;
}


SDF_status_t
mcd_osd_raw_get( osd_state_t     *osd_state, 
                 mcd_osd_shard_t *shard, 
		 time_t expTime,
		 time_t flush_time,
		 bool multiKey,
                 void **ppdata, 
		 uint64_t * pactual_size,
                 uint64_t next_addr, 
		 uint64_t * curr_addr,
                 uint64_t prev_seq, 
		 uint64_t curr_seq, 
		 int num_sessions )
{
    int                         i, rc;
    int                         map_offset;
    uint64_t                    tmp_offset;
    uint64_t                    map_value;
    uint64_t                    a_map_value;
    uint64_t                    temp;
    uint64_t                    blk_offset;
    uint64_t                    offset;
    char                      * buf;
    char                      * rbuf;
    mcd_osd_segment_t         * segment;
    mcd_osd_slab_class_t      * class;
    mcd_osd_meta_t            * meta;
    mcd_bak_state_t           * bkup = shard->backup;
    mcd_bak_stats_t           * stats = &bkup->stats;

    mcd_bak_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

    /*
     * FIXME: only persistent containers are supported currently
     */
    if ( 1 == shard->use_fifo || 0 == shard->persistent ) {
        mcd_bak_msg( 20414, PLAT_LOG_LEVEL_ERROR, "not supported yet" );
        return SDF_UNSUPPORTED_REQUEST;
    }

    if ( 0 == shard->backup_running ) {
        return SDF_UNAVAILABLE;
    }

repeat:
    if ( next_addr >= shard->blk_allocated ) {
        return SDF_OBJECT_UNKNOWN;
    }

    blk_offset = next_addr - ( next_addr % Mcd_osd_segment_blks );
    segment = shard->segment_table[blk_offset / Mcd_osd_segment_blks];
    if ( NULL == segment ) {
        return SDF_OBJECT_UNKNOWN;
    }
    plat_assert_always( segment->blk_offset == blk_offset );

    if ( NULL == Raw_rbuf ) {
        rbuf = plat_alloc( sizeof(mcd_osd_rbuf_t) + Mcd_osd_segment_size +
                           Mcd_osd_blk_size - 1 );
        if ( NULL == rbuf ) {
            mcd_bak_msg( 20417, PLAT_LOG_LEVEL_ERROR,
                         "failed to allocate raw read buffer" );
            return SDF_FAILURE_MEMORY_ALLOC;
        }
        Raw_rbuf = (mcd_osd_rbuf_t *)rbuf;
        memset( Raw_rbuf, 0, sizeof(mcd_osd_rbuf_t) );
        Raw_rbuf->err_offset = 0xffffffffffffffffull;
        Raw_rbuf->blk_offset = 0xffffffffffffffffull;
        Raw_rbuf->buf =
            (char *)( ( (uint64_t)rbuf + sizeof(mcd_osd_rbuf_t) +
                        Mcd_osd_blk_size - 1 ) & Mcd_osd_blk_mask );
    }

    if ( Raw_rbuf->blk_offset > next_addr || next_addr >=
         Raw_rbuf->blk_offset + Mcd_osd_segment_size / Mcd_osd_blk_size ||
         Raw_rbuf->prev_seq != prev_seq ||
         Raw_rbuf->curr_seq != curr_seq) {

        // don't read next segment on multi-getr
        if ( multiKey ) {
            return SDF_OBJECT_UNKNOWN;
        }

        // skip empty segment; compare uint64's
        for ( i = 0; i < Mcd_osd_segment_blks / 8 / sizeof(uint64_t); i++ ) {
            if ( 0 == prev_seq ) {
                if ( segment->alloc_map_s[i] != 0 ) {
                    break;
                }
            } else {
                if ( segment->update_map_s[i] != 0 ) {
                    break;
                }
            }
        }
        if ( i == Mcd_osd_segment_blks / 8 / sizeof(uint64_t) ) {
            (void) __sync_add_and_fetch( &stats->seg_empty, 1 );
            goto next_segment;
        }

        // segment got a read error, re-read in smaller blocks
        if ( Raw_rbuf->err_offset != 0xffffffffffffffffull &&
             Raw_rbuf->err_offset == blk_offset ) {
            rc = mcd_osd_read_bad_segment( osd_state, shard, segment,
                                           Raw_rbuf->buf, blk_offset,
                                           (segment->class->slab_blksize > 2048
                                            ? segment->class->slab_blksize
                                            : 2048),
                                           Mcd_osd_segment_blks, true );
        }
        // read raw segment
        else {
            mcd_bak_msg( 20418, MCD_OSD_LOG_LVL_INFO,
                         "reading raw segment, blk_offset=%lu", blk_offset );
            tmp_offset = shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
                + (blk_offset % Mcd_osd_rand_blksize);
            offset = tmp_offset * Mcd_osd_blk_size;

            rc = mcd_fth_aio_blk_read( (void *) osd_state,
                                       Raw_rbuf->buf,
                                       offset,
                                       Mcd_osd_segment_size );
        }

        if ( FLASH_EOK != rc ) {
            Raw_rbuf->err_offset = blk_offset;
            (void) __sync_add_and_fetch( &stats->seg_error, 1 );
            mcd_bak_msg( 40122, PLAT_LOG_LEVEL_ERROR,
                         "Error reading raw segment, blk_offset=%lu, "
                         "slab_blksize=%d, next_slab=%u, rc=%d",
                         blk_offset, segment->class->slab_blksize,
                         segment->next_slab, rc );
            return flash_to_sdf_status(rc);
        }
        mcd_bak_msg( 20420, MCD_OSD_LOG_LVL_INFO,
                     "segment read, blk_offset=%lu", blk_offset );
        (void) __sync_add_and_fetch( &stats->seg_read, 1 );

        Raw_rbuf->err_offset = 0xffffffffffffffffull;
        Raw_rbuf->blk_offset = blk_offset;
        Raw_rbuf->prev_seq = prev_seq;
        Raw_rbuf->curr_seq = curr_seq;
    }

    class = segment->class;
    map_offset = ( next_addr - segment->blk_offset + class->slab_blksize - 1 )
        / class->slab_blksize;

    while ( map_offset < Mcd_osd_segment_blks / class->slab_blksize ) {

        if ( 0 == prev_seq ) {
            map_value = ~segment->alloc_map_s[map_offset / 64];
        }
        else {
            map_value = ~segment->update_map_s[map_offset / 64];
        }

        if ( 0 != map_offset % 64 ) {
            map_value |= Mcd_osd_scan_masks[(map_offset % 64) - 1];
        }

        if ( 0 != ~map_value ) {
            temp = (map_value + 1) & ~map_value;
            tmp_offset = (map_offset / 64) * 64
                + Mcd_osd_scan_table[temp % MCD_OSD_SCAN_TBSIZE];

            buf = Raw_rbuf->buf +
                tmp_offset * class->slab_blksize * Mcd_osd_blk_size;

            meta = (mcd_osd_meta_t *)buf;

            // check for dead object (i/o error)
            if ( MCD_BAK_DEAD_OBJECT_MAGIC == meta->magic ) {
                map_offset = tmp_offset + 1;
                mcd_bak_msg( 40128, PLAT_LOG_LEVEL_DEBUG,
                             "object skipped (error), blk_offset=%lu "
                             "map_offset=%lu",
                             blk_offset, tmp_offset );
                (void) __sync_add_and_fetch( &stats->error_obj_count, 1);
                (void) __sync_add_and_fetch( &stats->error_obj_blocks,
                                             segment->class->slab_blksize );
                continue;
            }

            // verify magic number
            if ( MCD_OSD_MAGIC_NUMBER != meta->magic ) {
                mcd_bak_msg( 20421, PLAT_LOG_LEVEL_FATAL, "not enough magic! "
                            "blk_off=%lu map_off=%lu key_len=%d seqno=%lu",
                            blk_offset, tmp_offset, meta->key_len,
                            meta->seqno );
                return SDF_FLASH_EINVAL;
            }

            // object deleted since last backup
            if ( 0 != prev_seq ) {
                a_map_value = segment->alloc_map_s[tmp_offset / 64] &
                    Mcd_osd_bitmap_masks[tmp_offset % 64];

                if ( 0 == a_map_value ) {
                    mcd_bak_msg( 20422, MCD_OSD_LOG_LVL_INFO,
                                 "object deleted, blk_offset=%lu "
                                 "map_offset=%lu key_len=%d seqno=%lu",
                                 blk_offset, tmp_offset,
                                 meta->key_len, meta->seqno );
                    (void) __sync_add_and_fetch( &stats->deleted_count,
                                                 1 );
                    (void) __sync_add_and_fetch( &stats->deleted_blocks,
                                  (sizeof(mcd_osd_meta_t) + meta->key_len +
                                   meta->data_len + Mcd_osd_blk_size - 1) /
                                                 Mcd_osd_blk_size );
                    *curr_addr =
                        segment->blk_offset + tmp_offset * class->slab_blksize;
                    return SDF_OBJECT_DELETED;
                }
            }

            mcd_bak_msg( 20423, MCD_OSD_LOG_LVL_INFO, "object found, "
                         "blk_offset=%lu map_offset=%lu key_len=%d seqno=%lu",
                         blk_offset, tmp_offset, meta->key_len, meta->seqno );

            if ( prev_seq > meta->seqno || curr_seq < meta->seqno ) {
                map_offset = tmp_offset + 1;
                continue;
            }

            // honor the object expiry time -- backup option?
            if ( meta->expiry_time != 0 &&
                 meta->expiry_time <= expTime ) { // current time
                map_offset = tmp_offset + 1;
                mcd_bak_msg( 40002, PLAT_LOG_LEVEL_TRACE,
                             "object skipped (expired), blk_offset=%lu "
                             "map_offset=%lu key_len=%d",
                             blk_offset, tmp_offset, meta->key_len );
                (void) __sync_add_and_fetch( &stats->expired_count, 1 );
                (void) __sync_add_and_fetch( &stats->expired_blocks,
                    (sizeof(mcd_osd_meta_t) + meta->key_len + meta->data_len +
                     Mcd_osd_blk_size - 1) / Mcd_osd_blk_size );
                continue;
            }

            // honor the flush_all time -- backup option?
            if ( meta->create_time < flush_time &&
                 expTime >= flush_time ) {
                map_offset = tmp_offset + 1;
                mcd_bak_msg( 40003, PLAT_LOG_LEVEL_TRACE,
                             "object skipped (flush), blk_offset=%lu "
                             "map_offset=%lu key_len=%d",
                             blk_offset, tmp_offset, meta->key_len );
                (void) __sync_add_and_fetch( &stats->flushed_count, 1 );
                (void) __sync_add_and_fetch( &stats->flushed_blocks,
                    (sizeof(mcd_osd_meta_t) + meta->key_len + meta->data_len +
                     Mcd_osd_blk_size - 1) / Mcd_osd_blk_size );
                continue;
            }

            (void) __sync_add_and_fetch( &stats->obj_count, 1 );
            (void) __sync_add_and_fetch( &stats->obj_blocks,
                (sizeof(mcd_osd_meta_t) + meta->key_len + meta->data_len +
                 Mcd_osd_blk_size - 1) / Mcd_osd_blk_size );

            *ppdata = buf;
            *pactual_size = sizeof(mcd_osd_meta_t) + meta->key_len +
                meta->data_len;

            *curr_addr =
                segment->blk_offset + tmp_offset * class->slab_blksize;
            return SDF_SUCCESS;   /* SUCCESS */
        }

        map_offset += 64 - (map_offset % 64);
    }

 next_segment:

    next_addr = segment->blk_offset + num_sessions * Mcd_osd_segment_blks;
    if ( next_addr < shard->blk_allocated && !multiKey ) {
        goto repeat;
    }
    return SDF_OBJECT_UNKNOWN;
}


#define MCD_OSD_AUTODEL_MAXBLKS         16384
#define MCD_OSD_AUTODEL_MAXCOUNT        8192

/*
 * scan and delete expired objects
 */
void mcd_osd_auto_delete( struct ssdaio_ctxt *pctxt)
{
    int                         stop = 0;
    static uint64_t             count = 0;
    uint32_t                    ref_count;
    uint32_t                    pending;
    mcd_container_t           * container;
    mcd_osd_shard_t           * shard;
    osd_state_t               * osd_state = (osd_state_t *) pctxt;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

    if ( NULL != Raw_rbuf && NULL != Raw_rbuf->container &&
         0xffffffffffffffffull != Raw_rbuf->blk_offset ) {

        container = Raw_rbuf->container;
        (void) __sync_fetch_and_add( &container->ref_count, 1 );

        if ( cntr_running != container->state ||
             Raw_rbuf->container_id != Raw_rbuf->container_id ) {
            /*
             * container state changed, invalidate the buffer content
             */
            Raw_rbuf->blk_offset = 0xffffffffffffffffull;
            Raw_rbuf->container = NULL;
            Raw_rbuf->container_id = 0;
        }
        else {
            mcd_osd_delete_expired( osd_state, container->shard );
            stop = 1;
        }

        ref_count = __sync_sub_and_fetch( &container->ref_count, 1 );
        if ( cntr_stopping == container->state && 0 == ref_count ) {

            pending = __sync_fetch_and_add( &Mcd_adm_pending_mails, 1 );

            if ( 0 == pending && cntr_stopping == container->state ) {
                fthMboxPost( &Mcd_fth_admin_mbox, cntr_stopping );
                mcd_log_msg( 20141, PLAT_LOG_LEVEL_DEBUG,
                             "container stopping mail posted" );
            }
            else {
                (void) __sync_fetch_and_sub( &Mcd_adm_pending_mails, 1 );
            }
        }

        if ( 1 == stop ) {
            return;
        }
    }

    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {

        container = &Mcd_containers[count++ % MCD_MAX_NUM_CNTRS];
        (void) __sync_fetch_and_add( &container->ref_count, 1 );

        if ( cntr_running == container->state ) {

            shard = container->shard;
            if ( NULL != shard && 0 == shard->use_fifo ) {
                mcd_log_msg( 20430, MCD_OSD_LOG_LVL_DEBUG,
                             "found slab mode container, tcp_port=%d",
                             container->tcp_port );
                mcd_osd_delete_expired( osd_state, shard );
                stop = 1;
            }
        }

        ref_count = __sync_sub_and_fetch( &container->ref_count, 1 );
        if ( cntr_stopping == container->state && 0 == ref_count ) {

            pending = __sync_fetch_and_add( &Mcd_adm_pending_mails, 1 );

            if ( 0 == pending && cntr_stopping == container->state ) {
                fthMboxPost( &Mcd_fth_admin_mbox, cntr_stopping );
                mcd_log_msg( 20141, PLAT_LOG_LEVEL_DEBUG,
                             "container stopping mail posted" );
            }
            else {
                (void) __sync_fetch_and_sub( &Mcd_adm_pending_mails, 1 );
            }
        }

        if ( 1 == stop ) {
            return;
        }
    }

    return;
}

/*
 * read some amount of data from flash for the specified shard and
 * remove those expired objects from the shard
 */
static SDF_status_t
mcd_osd_delete_expired( osd_state_t *osd_state, mcd_osd_shard_t * shard )
{
    int                         rc;
    int                         count = 0;
    SDF_status_t                status;
    uint64_t                    next_addr = shard->slab.ad_next_addr;
    int                         map_offset;
    int                         old_offset;
    uint64_t                    tmp_offset;
    uint64_t                    map_value;
    uint64_t                    blk_offset;
    uint64_t                    offset;
    uint64_t                    temp;
    uint64_t                    syndrome;
    int                         nbytes;
    bool                        exp_delete = false;
    bool                        pfx_delete = false;
    char                      * buf;
    char                      * rbuf;
    mcd_osd_segment_t         * segment;
    mcd_osd_slab_class_t      * class;
    mcd_osd_meta_t            * meta;
    static struct objMetaData   dummy_meta;

    mcd_log_msg( 20427, MCD_OSD_LOG_LVL_DEBUG,
                 "ENTERING, next_addr=%lu bytes=%lu scanned=%lu expired=%lu",
                 next_addr, shard->auto_del_bytes, shard->auto_del_scanned,
                 shard->auto_del_expired );

    if ( 0 == shard->blk_allocated ) {
        return SDF_SUCCESS;
    }

    if ( next_addr >= shard->blk_allocated ||
         next_addr >= shard->total_blks ) {
        next_addr = 0;
        shard->slab.ad_pfx_cursor += shard->total_blks;
    }

    blk_offset = next_addr - ( next_addr % Mcd_osd_segment_blks );
    segment = shard->segment_table[blk_offset / Mcd_osd_segment_blks];

    if ( NULL == segment ) {
        /*
         * segment is not ready, skip this round
         */
        return SDF_SUCCESS;
    }
    plat_assert_always( segment->blk_offset == blk_offset );

    /*
     * if the segment is newly allocated and still empty, skip it
     */
    class = segment->class;
    if ( 0 == segment->next_slab ) {
        next_addr = blk_offset + Mcd_osd_segment_blks;
        status = SDF_SUCCESS;
        goto out;
    }

    if ( NULL == Raw_rbuf ) {
        rbuf = plat_alloc( sizeof(mcd_osd_rbuf_t) + Mcd_osd_segment_size +
                           Mcd_osd_blk_size - 1 );
        if ( NULL == rbuf ) {
            mcd_log_msg( 20417, PLAT_LOG_LEVEL_ERROR,
                         "failed to allocate raw read buffer" );
            status = SDF_FAILURE_MEMORY_ALLOC;
            goto out;
        }
        Raw_rbuf = (mcd_osd_rbuf_t *)rbuf;
        memset( Raw_rbuf, 0, sizeof(mcd_osd_rbuf_t) );
        Raw_rbuf->blk_offset = 0xffffffffffffffffull;
        Raw_rbuf->buf =
            (char *)( ( (uint64_t)rbuf + sizeof(mcd_osd_rbuf_t) +
                        Mcd_osd_blk_size - 1 ) & Mcd_osd_blk_mask );

        memset( (void *)&dummy_meta, 0, sizeof(dummy_meta) );
    }

    if ( Raw_rbuf->blk_offset > next_addr ||
         next_addr >= Raw_rbuf->blk_offset + Mcd_osd_segment_blks ) {

        tmp_offset = shard->rand_table[blk_offset / Mcd_osd_rand_blksize]
            + (blk_offset % Mcd_osd_rand_blksize);
        offset = tmp_offset * Mcd_osd_blk_size;

        nbytes = Mcd_osd_segment_size;
        rc = mcd_fth_aio_blk_read( (void *)osd_state,
                                   Raw_rbuf->buf, offset, nbytes );
        if ( FLASH_EOK != rc ) {
            mcd_log_msg( 20419, PLAT_LOG_LEVEL_FATAL,
                         "failed to read meta, rc=%d", rc );
            next_addr = blk_offset + Mcd_osd_segment_blks;
            status = flash_to_sdf_status(rc);
            goto out;
        }
        mcd_log_msg( 20420, MCD_OSD_LOG_LVL_INFO,
                     "segment read, blk_offset=%lu", blk_offset );

        Raw_rbuf->blk_offset = blk_offset;
        Raw_rbuf->container = shard->cntr;
        Raw_rbuf->container_id = shard->cntr->container_id;
        shard->auto_del_bytes += Mcd_osd_segment_size;
    }

    map_offset = ( next_addr - segment->blk_offset + class->slab_blksize - 1 )
        / class->slab_blksize;
    old_offset = map_offset;

    while ( map_offset < Mcd_osd_segment_blks / class->slab_blksize ) {

        if ( ( map_offset - old_offset ) * class->slab_blksize
             > MCD_OSD_AUTODEL_MAXBLKS || MCD_OSD_AUTODEL_MAXCOUNT < count ) {
            break;
        }

        map_value = ~segment->bitmap[map_offset / 64];
        if ( 0 != map_offset % 64 ) {
            map_value |= Mcd_osd_scan_masks[(map_offset % 64) - 1];
        }

        if ( 0 != ~map_value ) {
            temp = (map_value + 1) & ~map_value;
            tmp_offset = (map_offset / 64) * 64
                + Mcd_osd_scan_table[temp % MCD_OSD_SCAN_TBSIZE];

            buf = Raw_rbuf->buf +
                tmp_offset * class->slab_blksize * Mcd_osd_blk_size;

            meta = (mcd_osd_meta_t *)buf;
            if ( MCD_OSD_MAGIC_NUMBER != meta->magic ) {
                /*
                 * it is possible that the segment is fully allocated but
                 * some of the slabs haven't been written to flash yet,
                 * in which case we may encounter slabs without enough
                 * magic
                 */
                mcd_log_msg( 20421, PLAT_LOG_LEVEL_DIAGNOSTIC,
                             "not enough magic! "
                             "blk_off=%lu map_off=%lu key_len=%d seqno=%lu",
                             blk_offset, tmp_offset, meta->key_len,
                             meta->seqno );
                map_offset = tmp_offset + 1;
                shard->auto_del_nomagic++;
                continue;
            }
            shard->auto_del_scanned++;
            char * key = (char *)meta + sizeof(mcd_osd_meta_t);

            exp_delete = false;
            if ( ( meta->expiry_time && meta->expiry_time < (*(flash_settings.pcurrent_time)) ) ||
                 ( meta->create_time < shard->cntr->flush_time ) ) {
                exp_delete = true;
                mcd_log_msg( 20428, MCD_OSD_LOG_LVL_INFO,
                            "expired obj, key=%s key_len=%d data_len=%d",
                            key, meta->key_len, meta->data_len );
            }
            else {
                pfx_delete = false;
                if ( shard->cntr->prefix_delete ) {
                    char      * temp;
                    void      * prefix;
                    temp = memchr( key, flash_settings.prefix_del_delimiter,
                                   meta->key_len );
                    if ( NULL != temp && key <= temp ) {
                        prefix = mcd_prefix_lookup( shard->cntr->pfx_deleter,
                                                    key, temp - key );
                        if ( NULL != prefix && meta->create_time <=
                             ((prefix_item_t *)prefix)->start_time ) {
                            /*
                             * FIXME_PREFIX_DELETE:
                             * additional checks for prefix-based deletion
                             * such as timestamp checking?
                             */
                            pfx_delete = true;
                            dummy_meta.createTime = meta->create_time;
                            mcd_log_msg( 50049,
                                         PLAT_LOG_LEVEL_TRACE_LOW,
                                         "prefix-based delete, key=%s", key );
                        }
                    }
                }
            }

            if ( exp_delete || pfx_delete ) {
                syndrome = hash( (unsigned char *)key, meta->key_len, 0 );

                osd_state->osd_lock = (void *)( shard->bucket_locks +
                    ( (syndrome % shard->hash_size) / shard->lock_bktsize ) );
                osd_state->osd_wait =
                    fthLock( (fthLock_t *) osd_state->osd_lock, 1, NULL );

                /*
                 * FIXME: may need to call rep_seqno_get() here for
                 * replicated persistent containers when full replication
                 * is supported
                 */
                rc = mcd_fth_osd_slab_set( (void *) osd_state,  // context
                                           shard,
                                           key,
                                           meta->key_len,
                                           NULL,                  // data
                                           0,                     // data_len
                                           pfx_delete ?
                                           FLASH_PUT_PREFIX_DO_DEL :
                                           FLASH_PUT_DEL_EXPIRED, // flags
                                           &dummy_meta,           // meta_data
                                           syndrome );

                fthUnlock( osd_state->osd_wait );

                if ( FLASH_EOK == rc ) {
                    if ( exp_delete ) {
                        mcd_log_msg( 20429, MCD_OSD_LOG_LVL_INFO,
                                     "expired object deleted, key=%s", key );
                        shard->auto_del_expired++;
                    }
                    else {
                        mcd_log_msg( 50050, MCD_OSD_LOG_LVL_INFO,
                                     "prefix object deleted, key=%s", key );
                        shard->auto_del_prefix++;
                    }
                    count++;
                }
            }

            map_offset = tmp_offset + 1;
            continue;
        }

        map_offset += 64 - (map_offset % 64);
    }

    next_addr = map_offset * class->slab_blksize + segment->blk_offset;
    status = SDF_SUCCESS;

out:
    if ( NULL != Raw_rbuf && Raw_rbuf->blk_offset !=
         next_addr - ( next_addr % Mcd_osd_segment_blks ) ) {

        if ( status == SDF_SUCCESS && shard->cntr->prefix_delete &&
             0xffffffffffffffffull != Raw_rbuf->blk_offset ) {
            SDF_status_t pfx_rc;
            pfx_rc = mcd_prefix_update( shard->cntr->pfx_deleter,
                                        shard->slab.ad_pfx_cursor +
                                        Raw_rbuf->blk_offset,
                                        shard->total_blks );
            if ( SDF_SUCCESS != pfx_rc ) {
                mcd_log_msg( 50051, PLAT_LOG_LEVEL_ERROR,
                             "failed to update prefix deleter, rc=%s",
                             SDF_Status_Strings[pfx_rc] );
                status = SDF_FAILURE;
            }
        }

        /*
         * invalid the buffer so that we won't be stuck with stale data
         */
        Raw_rbuf->blk_offset = 0xffffffffffffffffull;
    }

    if ( next_addr >= shard->blk_allocated ||
         next_addr >= shard->total_blks ) {
        next_addr = 0;
        shard->slab.ad_pfx_cursor += shard->total_blks;
    }
    shard->slab.ad_next_addr = next_addr;

    return status;
}


void
mcd_osd_fifo_check( void )
{
    uint32_t                    ref_count;
    uint32_t                    pending;
    uint32_t                    total;
    osd_state_t               * context;
    mcd_container_t           * container;
    mcd_osd_shard_t           * shard;

    mcd_log_msg( 20000, PLAT_LOG_LEVEL_DEBUG, "ENTERING" );

    for ( int i = 0; i < MCD_MAX_NUM_CNTRS; i++ ) {

        container = &Mcd_containers[i];
        (void) __sync_fetch_and_add( &container->ref_count, 1 );

        if ( cntr_running == container->state ) {

            shard = container->shard;
            if ( NULL != shard ) {

                if ( shard->use_fifo && 0 == shard->fifo.fth_count ) {
                    /*
                     * check to see if there is any sleeper to wake up for
                     * this shard
                     */
                    total = 0;
                    while ( 1 ) {

                        context = (osd_state_t *)fthMboxTry
                            ( &shard->fifo.sleeper_mbox );
                        if ( NULL == context ) {
                            break;
                        }
                        if ( 0 == total ) {
                            (void) __sync_fetch_and_add( &Mcd_osd_hard_wakeups,
                                                         1 );
                        }

                        fthMboxPost( (fthMbox_t *)context->osd_mbox, 0 );
                        total += context->osd_blocks;
                        mcd_log_msg( 20324, MCD_OSD_LOG_LVL_DEBUG,
                                     "sleeper waked up, blocks=%d, total=%d",
                                     context->osd_blocks, total );

                        if ( MCD_OSD_WBUF_BLKS * 2 < total ) {
                            break;
                        }
                    }
                }
            }
        }

        ref_count = __sync_sub_and_fetch( &container->ref_count, 1 );
        if ( cntr_stopping == container->state && 0 == ref_count ) {

            pending = __sync_fetch_and_add( &Mcd_adm_pending_mails, 1 );

            if ( 0 == pending && cntr_stopping == container->state ) {
                fthMboxPost( &Mcd_fth_admin_mbox, cntr_stopping );
                mcd_log_msg( 20141, PLAT_LOG_LEVEL_DEBUG,
                             "container stopping mail posted" );
            }
            else {
                (void) __sync_fetch_and_sub( &Mcd_adm_pending_mails, 1 );
            }
        }
    }

    return;
}


/*
 * we don't have to worry about ref_count here since the original
 * delete request will take care of that
 */
SDF_status_t mcd_osd_do_prefix_delete( mcd_container_t *container, char *key, int key_len)
{
    SDF_status_t                status = SDF_FAILURE;
    mcd_osd_shard_t           * shard;
    char                        temp[256];

    plat_assert( NULL != container );
    mcd_log_msg( 50052, MCD_OSD_LOG_LVL_INFO,
                 "ENTERING container=%s prefix=%s",
                 container->cname, key );

    shard = (mcd_osd_shard_t *)container->shard;
    if ( shard->use_fifo || 0 == shard->num_objects ) {
        status = SDF_SUCCESS;
    }
    else {
        uint64_t cursor = shard->slab.ad_pfx_cursor + shard->slab.ad_next_addr
            - ( shard->slab.ad_next_addr % Mcd_osd_segment_blks );
        status = mcd_prefix_register( container->pfx_deleter, key,
                                      key_len - 3, (*(flash_settings.pcurrent_time)),
                                      cursor, shard->total_blks );
        if ( SDF_SUCCESS == status ) {
            memcpy( temp, key, key_len - 3 );
            temp[key_len - 3] = 0;
            mcd_log_msg( 50053, MCD_OSD_LOG_LVL_INFO,
                         "prefix registered, key=%s time=%lu addr=%lu",
                         temp, (*(flash_settings.pcurrent_time)), shard->slab.ad_next_addr );
        }
        else {
            mcd_log_msg( 50054, PLAT_LOG_LEVEL_ERROR,
                         "failed to register prefix, key=%s", key );
        }
    }

    return(status);
}


SDF_status_t mcd_osd_prefix_stats( mcd_container_t *container, char **pdata, uint32_t *pdata_len)
{
    SDF_status_t                status;
    int                         prefixes;
    mcd_osd_shard_t            *shard;

    plat_assert( NULL != container );
    mcd_log_msg( 50055, MCD_OSD_LOG_LVL_INFO,
                 "ENTERING container=%s", container->cname );

    shard = (mcd_osd_shard_t *)container->shard;
    if ( shard->use_fifo ) {
        *pdata = NULL;
        *pdata_len = 0;
        status = SDF_SUCCESS;
    }
    else {
        uint64_t curr_blks = Mcd_osd_segment_blks +
            ( shard->blk_allocated < shard->total_blks ?
              shard->blk_allocated : shard->total_blks );
        prefixes = mcd_prefix_list( container->pfx_deleter, pdata,
                                    shard->slab.ad_pfx_cursor +
                                    shard->slab.ad_next_addr,
                                    shard->total_blks, curr_blks );
        if ( 0 > prefixes ) {
            mcd_log_msg( 50056, PLAT_LOG_LEVEL_ERROR,
                         "failed to get prefix list" );
            status = SDF_FAILURE_MEMORY_ALLOC;
        }
        else {
            *pdata_len = prefixes;
            status = SDF_SUCCESS;
            mcd_log_msg( 50057, MCD_OSD_LOG_LVL_INFO,
                         "number of prefixes is %d", prefixes );
        }
    }

    return(status);
}


void mcd_osd_auto_del_stats( mcd_osd_shard_t *shard, char ** ppos, int * lenp )
{
    plat_snprintfcat( ppos, lenp,
                      "STAT flash_auto_delete %lu %lu %lu %lu %lu\r\n",
                      shard->auto_del_bytes / 1048576, shard->auto_del_scanned,
                      shard->auto_del_expired, shard->auto_del_prefix,
                      shard->auto_del_nomagic );
}

void osd_set_shard_cas_id_if_higher( struct shard *shard, void * data )
{
    mcd_osd_shard_t           * mcd_shard = (mcd_osd_shard_t *) shard;
    mcd_osd_set_shard_cas_id_if_higher( mcd_shard->cntr,  data );
}

void mcd_osd_set_shard_cas_id_if_higher( mcd_container_t *container, void * data )
{
    uint64_t            old_id;
    uint64_t            new_id;
    uint64_t            cas_id;

    cas_id = (*flash_settings.get_cas_id)(data);
    do {
        old_id = container->cas_id;
        if ( cas_id <= old_id ) {
            break;
        }
        new_id = ( container->cas_num_nodes -
                   ( ( cas_id - container->cas_node_id ) %
                     container->cas_num_nodes ) ) + cas_id;
    } while ( true !=
              __sync_bool_compare_and_swap( &container->cas_id,
                                            old_id, new_id ) );
}

#define MCD_OSD_MIN_SHARD_SIZE  (256 * 1024 * 1024)

uint64_t mcd_osd_get_shard_minsize( void )
{
    uint64_t            min_size = MCD_OSD_MIN_SHARD_SIZE;

    if ( MCD_OSD_NUM_WBUFS * MCD_OSD_WBUF_BLKS * 4 > MCD_OSD_MIN_SHARD_SIZE ) {
        min_size = MCD_OSD_NUM_WBUFS * MCD_OSD_WBUF_BLKS * 4;
    }

    return min_size;
}


/*
 * Exportable version of mcd_osd_blk_to_lba.
 */
uint32_t
mcd_osd_blk_to_lba_x(uint32_t blocks) {
    return mcd_osd_blk_to_lba(blocks);
}


/*
 * Exportable version of mcd_fth_osd_grow_class.
 */
int
mcd_fth_osd_grow_class_x(mcd_osd_shard_t *shard, mcd_osd_slab_class_t *class)
{
    return mcd_fth_osd_grow_class(shard, class);
}

/************************************************************************
 *                                                                      *
 *                      Memcached AIO subsystem                         *
 *                                                                      *
 ************************************************************************/

osd_state_t *mcd_fth_init_aio_ctxt( int category )
{
    void               *self = (void *)fthSelf();
    uint32_t            next;
    osd_state_t        *oss;
    fthWaitEl_t        *wait;

    mcd_log_msg( 20175, PLAT_LOG_LEVEL_DEBUG, "ENTERING, self=%p", self );

    if ( 0 > category || SSD_AIO_CTXT_MAX_COUNT <= category ) {
        mcd_log_msg( 50077, PLAT_LOG_LEVEL_FATAL,
                     "invalid aio context category %d", category );
        plat_abort();
    }

    for ( int i = 0; i < Mcd_aio_state_next; i++ ) {
        if (!Mcd_aio_states[i]) {
            /*  This slot has been assigned, but the pointer
             *  hasn't been set yet.
             */
            continue;
        }
        if ( self == Mcd_aio_states[i]->osd_aio_state->aio_self) {
            mcd_log_msg( 20176, PLAT_LOG_LEVEL_DEBUG,
                         "self found, use context %d", i );
            return (void *) Mcd_aio_states[i];
        }
    }

    /*
     * ok, unknown fthread, need a new context
     */
    next = __sync_fetch_and_add( &Mcd_aio_state_next, 1 );

    wait = fthLock( &Mcd_aio_ctxt_lock, 1, NULL );

    if ((next >= AIO_MAX_CTXTS) || ( next >= flash_settings.aio_queue_len ))
    {
        mcd_log_msg( 20172, PLAT_LOG_LEVEL_FATAL,
                     "Please reduce the number of fthreads" );
        for ( int i = 0; i < SSD_AIO_CTXT_MAX_COUNT; i++ ) {
            mcd_log_msg( 50075, PLAT_LOG_LEVEL_INFO,
                         "aio context category %s count=%d",
                         Ssd_aio_ctxt_names[i], Mcd_fth_aio_ctxts[i] );
        }
        plat_abort();
    }

    (void) __sync_fetch_and_add( &Mcd_fth_aio_ctxts[category], 1 );
    mcd_log_msg( 50076, PLAT_LOG_LEVEL_DEBUG,
                 "new aio context allocated, category=%s count=%d",
                 Ssd_aio_ctxt_names[category],
                 Mcd_fth_aio_ctxts[category] );

    oss                  = mcd_osd_init_state(category);
    oss->index           = next;
    oss->osd_aio_state   = mcd_aio_init_state();
    Mcd_aio_states[next] = oss;
    fthUnlock( wait );

    mcd_log_msg( 150004, PLAT_LOG_LEVEL_DEBUG, "Mcd_aio_states[%d] = %p", next, Mcd_aio_states[next]);

    return (void *) Mcd_aio_states[next];
}


int mcd_fth_free_aio_ctxt( osd_state_t * osd_state, int category )
{
    mcd_log_msg( 50078, PLAT_LOG_LEVEL_DEBUG,
                 "ENTERING category=%d", category );

    if ( 0 > category || SSD_AIO_CTXT_MAX_COUNT <= category ) {
        mcd_log_msg( 50077, PLAT_LOG_LEVEL_FATAL,
                     "invalid aio context category %d", category );
        plat_abort();
    }
    (void) __sync_fetch_and_sub( &Mcd_fth_aio_ctxts[category], 1 );

    mcd_aio_free_state(osd_state->osd_aio_state);
    Mcd_aio_states[osd_state->index] = NULL;
    mcd_osd_free_state(osd_state);

    return 0;   /* SUCCESS */
}

/********************************************************
 *
 *  IMPORTANT NOTE: the definition of object_data_t MUST
 *  be identical to that in the memcached/command.h file!!!!
 *
 ********************************************************/

#ifdef SDFAPI
typedef struct {
    uint8_t         version;        /* nonvolatile version number */
    uint8_t         internal_flags; /* internal flags */
    uint16_t        reserved;       /* alignment */
    uint32_t        client_flags;   /* opaque flags from client */
    uint64_t        cas_id;         /* the CAS identifier */
} object_data_t;
#endif

  /*
   *   Enumerate the next object for this container.
   *   Very similar to process_raw_get_command().
   */
SDF_status_t process_raw_get_command_enum( 
                                           mcd_osd_shard_t  *shard,
					   osd_state_t      *osd_state,
					   uint64_t          addr, 
					   uint64_t          prev_seq,
					   uint64_t          curr_seq, 
					   int               num_sessions, 
					   int               session_id,
					   int               max_objs,
					   char            **key_out,
					   uint32_t         *keylen_out,
					   char            **data_out,
					   uint64_t         *datalen_out,
					   uint64_t         *addr_out
				       )
{

    SDF_status_t                status = SDF_FAILURE;
    uint64_t                    next_addr = addr;
    uint64_t                    curr_addr;
    uint64_t                    real_len = 0;
    char                       *raw_data = NULL;
    mcd_osd_meta_t             *meta;
    object_data_t              *obj_data;
    char                       *key     = NULL;
    uint32_t                    keylen  = 0;
    char                       *data    = NULL;
    uint64_t                    datalen = 0;

    while ( session_id !=
            ( ( next_addr / Mcd_osd_segment_blks ) % num_sessions ) ) {
        next_addr = next_addr - ( next_addr % Mcd_osd_segment_blks )
            + Mcd_osd_segment_blks;
    }

    for ( int r = 0; r < max_objs; r++ ) {

        if ( r > 0 ) {
	    // xxxzzz TODO: support multikey enum gets!
	    mcd_log_msg( 30665, PLAT_LOG_LEVEL_FATAL,
			 "process_raw_get_command_enum() failed" );
	    plat_assert_always( 0 == 1 );
        }

        status = mcd_osd_raw_get( osd_state,
				  shard,
				  0, // expTime
				  0, // flushTime
				  0, // multiKey
	                          (void **)&raw_data, &real_len,
                                  next_addr, &curr_addr, prev_seq, curr_seq,
                                  num_sessions );

        if ( SDF_SUCCESS == status ) {

            // get metadata pointers
            meta     = (mcd_osd_meta_t *) raw_data;
            obj_data = (object_data_t *)
                (raw_data + sizeof(mcd_osd_meta_t) + meta->key_len);

	    keylen = meta->key_len;
	    key = (char *) plat_alloc(keylen);
	    if (key == NULL) {
	        status = SDF_FAILURE_MEMORY_ALLOC;
		break;
	    }
            memcpy( key, raw_data + sizeof(mcd_osd_meta_t), meta->key_len );
	    datalen = real_len - sizeof(mcd_osd_meta_t) - meta->key_len; // - sizeof(object_data_t);
	    data = (char *) plat_alloc(datalen);
	    if (data == NULL) {
	        plat_free(key);
	        status = SDF_FAILURE_MEMORY_ALLOC;
		break;
	    }
            memcpy( data, raw_data + sizeof(mcd_osd_meta_t) + meta->key_len /* + sizeof(object_data_t)*/, datalen);

            continue;
        }
        else if ( SDF_OBJECT_DELETED == status ) {
	    // purposefully empty
        }
        else if ( SDF_OBJECT_UNKNOWN != status ) {
	    // purposefully empty
        }

        break;  // SDF_OBJECT_UNKNOWN == status
    }

    *keylen_out  = keylen;
    *key_out     = key;
    *datalen_out = datalen;
    *data_out    = data;
    *addr_out    = curr_addr + 1;

    return(status);
}
