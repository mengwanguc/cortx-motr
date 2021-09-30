#include "motr/client.h"
#include <stdlib.h>
#include <errno.h>

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_CLIENT
#include "lib/trace.h"

static struct m0_idx_dix_config  motr_dix_conf;
static struct m0_config motr_conf;
static struct m0_client *m0_instance = NULL;
static int instance_init_done = 0;

// struct m0_uint128		obj_id = { 12345, 9999999999 };


int m0_init_instance(const char* ha_addr, const char* local_addr, 
		const char* profile_fid, const char* process_fid) {
        int rc;
	if (instance_init_done == 1) {
		M0_LOG(M0_ALWAYS,
			"Instance already initialized. HA addr: %s, local addr:%s, profile fid: %s, process fid: %s:\n",
			motr_conf.mc_ha_addr,
			motr_conf.mc_local_addr,
			motr_conf.mc_profile,
			motr_conf.mc_process_fid);
		return 0;
	}

	M0_LOG(M0_ALWAYS,
			"Init instance. HA addr: %s, local addr:%s, profile fid: %s, process fid: %s:\n",
			ha_addr,
			local_addr,
			profile_fid,
			process_fid);

        motr_dix_conf.kc_create_meta = false;
        motr_conf.mc_is_oostore            = true;
        motr_conf.mc_is_read_verify        = false;
        motr_conf.mc_ha_addr               = ha_addr;
        motr_conf.mc_local_addr            = local_addr;
        motr_conf.mc_profile               = profile_fid;
        motr_conf.mc_process_fid           = process_fid;
        motr_conf.mc_tm_recv_queue_min_len = M0_NET_TM_RECV_QUEUE_DEF_LEN;
        motr_conf.mc_max_rpc_msg_size      = M0_RPC_DEF_MAX_RPC_MSG_SIZE;
        motr_conf.mc_idx_service_id        = M0_IDX_DIX;
        motr_conf.mc_idx_service_conf      = (void *)&motr_dix_conf;
        rc = m0_client_init(&m0_instance, &motr_conf, true);

	instance_init_done = 1;

        return rc;
}
M0_EXPORTED(m0_init_instance);


static int object_open(struct m0_obj *obj)
{
	struct m0_op *ops[1] = {NULL};
	int           rc;

	rc = m0_entity_open(&obj->ob_entity, &ops[0]);
	if (rc != 0) {
		return rc;
	}

	m0_op_launch(ops, 1);
	rc = m0_op_wait(ops[0],
			M0_BITS(M0_OS_FAILED, M0_OS_STABLE),
			M0_TIME_NEVER);
	if (rc == 0)
		rc = ops[0]->op_rc;

	m0_op_fini(ops[0]);
	m0_op_free(ops[0]);
	ops[0] = NULL;

	/* obj is valid if open succeeded */
	return rc;
}


static int read_data_from_object(struct m0_obj      *obj,
				 struct m0_indexvec *ext,
				 struct m0_bufvec   *data,
				 struct m0_bufvec   *attr)
{
	int          rc;
	struct m0_op *ops[1] = { NULL };

	/* Create the read request */
	m0_obj_op(obj, M0_OC_READ, ext, data, attr, 0, 0, &ops[0]);
	if (ops[0] == NULL) {
		return -EINVAL;
	}

	/* Launch the read request*/
	m0_op_launch(ops, 1);

	/* wait */
	rc = m0_op_wait(ops[0],
			M0_BITS(M0_OS_FAILED,
				M0_OS_STABLE),
			M0_TIME_NEVER);
	rc = rc ? : ops[0]->op_sm.sm_rc;

	/* fini and release the ops */
	m0_op_fini(ops[0]);
	m0_op_free(ops[0]);
	return rc;
}

static int alloc_vecs(struct m0_indexvec *ext,
		      struct m0_bufvec   *data,
		      struct m0_bufvec   *attr,
		      uint32_t            block_count,
		      uint64_t            block_size)
{
	int      rc;

	rc = m0_indexvec_alloc(ext, block_count);
	if (rc != 0)
		return rc;

	/*
	 * this allocates <block_count> * <block_size>  buffers for data,
	 * and initialises the bufvec for us.
	 */

	rc = m0_bufvec_alloc(data, block_count, block_size);
	if (rc != 0) {
		m0_indexvec_free(ext);
		return rc;
	}
	rc = m0_bufvec_alloc(attr, block_count, 1);
	if (rc != 0) {
		m0_indexvec_free(ext);
		m0_bufvec_free(data);
		return rc;
	}
	return rc;
}

static void prepare_ext_vecs(struct m0_indexvec *ext,
			     struct m0_bufvec   *data,
			     struct m0_bufvec   *attr,
			     uint32_t            block_count,
			     uint64_t            block_size,
			     uint64_t           *last_index,
			     char                c)
{
	int      i;

	M0_LOG(M0_ALWAYS,"block_count: %d, block_size: %d, last: %d",
			(int)block_count, (int)block_size, (int)*last_index);

	for (i = 0; i < block_count; ++i) {
		ext->iv_index[i]       = *last_index;
		ext->iv_vec.v_count[i] = block_size;
		*last_index           += block_size;

		/* Fill the buffer with all `c`. */
			M0_LOG(M0_ALWAYS,"start do do memset for block %d", i);
		memset(data->ov_buf[i], c, data->ov_vec.v_count[i]);
		/* we don't want any attributes */
		attr->ov_vec.v_count[i] = 0;
	}
}


static void prepare_ext_vecs_write(struct m0_indexvec *ext,
			     struct m0_bufvec   *data,
			     struct m0_bufvec   *attr,
			     uint32_t            block_count,
			     uint64_t            block_size,
			     uint64_t           *last_index,
			     char*                s)
{
	int      i;
	int idx = 0;

	M0_LOG(M0_ALWAYS,"block_count: %d, block_size: %d, last: %d",
			(int)block_count, (int)block_size, (int)*last_index);

	for (i = 0; i < block_count; ++i) {
		ext->iv_index[i]       = *last_index;
		ext->iv_vec.v_count[i] = block_size;
		*last_index           += block_size;

		/* Fill the buffer with all `c`. */
		M0_LOG(M0_ALWAYS,"start do do memset for block %d", i);
		memcpy(data->ov_buf[i], &s[idx], data->ov_vec.v_count[i]);
		idx += block_size;
		/* we don't want any attributes */
		attr->ov_vec.v_count[i] = 0;
	}
}


static void cleanup_vecs(struct m0_indexvec *ext,
			 struct m0_bufvec   *data,
			 struct m0_bufvec   *attr)
{
	/* Free bufvec's and indexvec's */
	m0_indexvec_free(ext);
	m0_bufvec_free(data);
	m0_bufvec_free(attr);
}


static void print_object_data(struct m0_bufvec *data)
{
	int i;

	for (i = 0; i < data->ov_vec.v_nr; ++i) {
		M0_LOG(M0_ALWAYS,"Block %6d:  len:%d\n", i, (int)data->ov_vec.v_count[i]);
		M0_LOG(M0_ALWAYS,"%.*s", (int)data->ov_vec.v_count[i],
			       (char *)data->ov_buf[i]);
		M0_LOG(M0_ALWAYS,"\n");
	}
}

static void copy_object_data(struct read_result* rres, struct m0_bufvec *data,
		uint64_t start, uint64_t len, uint64_t block_size)
{
	int i;

	rres->len = 0;

	if (data->ov_vec.v_nr == 0) {
		return;
	}

	rres->len = len;

	rres->data = malloc( sizeof(char) * (rres->len));
	M0_LOG(M0_ALWAYS,"Copy res size %6d:\n", (int)rres->len);


	int pos = 0;
	int copy_count = 0;
	int copy_off = 0;
	for (i = 0; i < data->ov_vec.v_nr; ++i) {
//		M0_LOG(M0_ALWAYS,"Copying Block %6d:\n", i);
		copy_off = 0;
		if (i == 0) {
			copy_count = block_size - (start % block_size);
			copy_off = start % block_size;
		}
		else if (i + 1 == data->ov_vec.v_nr) {
			if ((start + len) % block_size == 0)
				copy_count = block_size;
			else
				copy_count = (start + len) % block_size;
		}
		else
			copy_count = block_size;

		if (copy_count > data->ov_vec.v_count[i])
			copy_count = data->ov_vec.v_count[i];

		M0_LOG(M0_ALWAYS,"Copying Block %6d:  len:%d\n", i, copy_count);
		memcpy(&rres->data[pos], (char *)(data->ov_buf[i] + copy_off), copy_count);
		pos += copy_count;
	}
	M0_LOG(M0_ALWAYS,"Copy done. pos: %6d:\n", pos);

	return;
}


struct read_result * m0_object_read(uint64_t obj_hi, uint64_t obj_low, uint64_t start, uint64_t len) {
	int rc = 0;
	struct read_result *rres = malloc(sizeof(struct read_result));
	struct m0_uint128 read_obj_id = { obj_hi, obj_low };

	rres->data = NULL;
	rres->len = 0;
	rres->rc = rc;

	if (instance_init_done == 0) {
		M0_LOG(M0_ERROR,"Error! reading object before initializing instance!\n");
		return rres;
	}
	M0_LOG(M0_ALWAYS,"---- menglog: done m0_client_init\n");

	struct m0_container       motr_container;

	m0_container_init(&motr_container, NULL, &M0_UBER_REALM, m0_instance);
	rc = motr_container.co_realm.re_entity.en_sm.sm_rc;
	if (rc != 0) {
		M0_LOG(M0_ALWAYS,"error in m0_container_init: %d\n", rc);
		goto client_out;
	}
	M0_LOG(M0_ALWAYS, "---- menglog: done m0_container_init\n");
	
	struct m0_container *container = &motr_container;
	
	struct m0_obj      obj;
	struct m0_client  *instance;

	struct m0_indexvec ext;
	struct m0_bufvec   data;
	struct m0_bufvec   attr;

	uint64_t	block_size = 4096;
	uint64_t	block_start = start / block_size;
	uint64_t	last_offset = block_start * block_size;
	uint64_t	end = start + len;
	uint64_t	block_end;
	if (end % block_size == 0)
		block_end = end / block_size;
	else
		block_end = end / block_size + 1;
	
	uint32_t	block_count = (uint32_t)(block_end - block_start);


	M0_SET0(&obj);
	instance = container->co_realm.re_instance;
	m0_obj_init(&obj, &container->co_realm, &read_obj_id,
		    m0_client_layout_id(instance));

	M0_LOG(M0_ALWAYS, "---- menglog: done m0_obj_init\n");

	rc = object_open(&obj);
	if (rc != 0) {
		M0_LOG(M0_ALWAYS, "Failed to open object: rc=%d\n", rc);
		
		rres->rc = rc;
		return rres;
	}

	M0_LOG(M0_ALWAYS, "---- menglog: done object_open\n");


	/*
	 * alloc & prepare ext, data and attr. We will write 4k * 2.
	 */
	rc = alloc_vecs(&ext, &data, &attr, block_count, block_size);
	if (rc != 0) {
		M0_LOG(M0_ALWAYS, "Failed to alloc ext & data & attr: %d\n", rc);
		goto out;
	}

	M0_LOG(M0_ALWAYS, "---- menglog: done alloc_vecs\n");


	prepare_ext_vecs(&ext, &data, &attr, block_count, block_size, &last_offset, '\0');

	M0_LOG(M0_ALWAYS, "---- menglog: done prepare_ext_vecs\n");

	/* Start to read data to object */
	rc = read_data_from_object(&obj, &ext, &data, &attr);


	if (rc == 0) {
		print_object_data(&data);
		// print_object_data(&attr);
		copy_object_data(rres, &data, start, len, block_size);
	}
	M0_LOG(M0_ALWAYS, "start: %d  end: %d  len: %d\n", (int)start, (int)end, (int)len);
	cleanup_vecs(&ext, &data, &attr);

out:
	/* Similar to close() */
	m0_entity_fini(&obj.ob_entity);

	M0_LOG(M0_ALWAYS, "Object read: %d\n", rc);
	M0_LOG(M0_ALWAYS, "res len: %d\n", (int)((rres->len)));
	M0_LOG(M0_ALWAYS,"%.*s", (int)(rres->len),
			       (char *)rres->data);
	
	rres->rc = rc;
	return rres;

client_out:
//	m0_client_fini(m0_instance, true);
	rres->rc = rc;
	return rres;
}
M0_EXPORTED(m0_object_read);


int m0_object_create(uint64_t obj_hi, uint64_t obj_low)
{
	if (instance_init_done == 0) {
		M0_LOG(M0_ERROR,"Error! reading object before initializing instance!\n");
		return -1;
	}
	M0_LOG(M0_ALWAYS,"---- menglog: done m0_client_init\n");


	struct m0_container       motr_container;
	int	rc = 0;

	m0_container_init(&motr_container, NULL, &M0_UBER_REALM, m0_instance);
	rc = motr_container.co_realm.re_entity.en_sm.sm_rc;
	if (rc != 0) {
		M0_LOG(M0_ALWAYS,"error in m0_container_init: %d\n", rc);
		return rc;
	}
	M0_LOG(M0_ALWAYS, "---- menglog: done m0_container_init\n");


	struct m0_obj     obj;
	struct m0_client *instance;
	struct m0_op     *ops[1] = {NULL};

	struct m0_container *container = &motr_container;
	struct m0_uint128 create_obj_id = { obj_hi, obj_low };
	

	M0_SET0(&obj);
	instance = container->co_realm.re_instance;
	m0_obj_init(&obj, &container->co_realm, &create_obj_id,
		    m0_client_layout_id(instance));

	rc = m0_entity_create(NULL, &obj.ob_entity, &ops[0]);
	if (rc != 0) {
		printf("Failed to create object: %d\n", rc);
		return rc;
	}

	m0_op_launch(ops, 1);
	rc = m0_op_wait(ops[0],
			M0_BITS(M0_OS_FAILED, M0_OS_STABLE),
			M0_TIME_NEVER);
	if (rc == 0)
		rc = ops[0]->op_rc;

	m0_op_fini(ops[0]);
	m0_op_free(ops[0]);
	ops[0] = NULL;

	m0_entity_fini(&obj.ob_entity);

	printf("Object (id=%lu) creation result: %d\n",
	       (unsigned long)create_obj_id.u_lo, rc);
	return rc;
}
M0_EXPORTED(m0_object_create);


static int write_data_to_object(struct m0_obj      *obj,
				struct m0_indexvec *ext,
				struct m0_bufvec   *data,
				struct m0_bufvec   *attr)
{
	int          rc;
	struct m0_op *ops[1] = { NULL };

	/* Create the write request */
	m0_obj_op(obj, M0_OC_WRITE, ext, data, attr, 0, 0, &ops[0]);
	if (ops[0] == NULL) {
		printf("Failed to init a write op\n");
		return -EINVAL;
	}

	/* Launch the write request*/
	m0_op_launch(ops, 1);

	/* wait */
	rc = m0_op_wait(ops[0],
			M0_BITS(M0_OS_FAILED,
				M0_OS_STABLE),
			M0_TIME_NEVER);
	rc = rc ? : ops[0]->op_sm.sm_rc;

	/* fini and release the ops */
	m0_op_fini(ops[0]);
	m0_op_free(ops[0]);
	printf("Object write result: %d\n", rc);
	return rc;
}

int m0_object_write(uint64_t obj_hi, uint64_t obj_low, uint64_t start, uint64_t len, char* d)
{
	struct m0_obj      obj;
	struct m0_client  *instance;
	int                rc;

	// char d[8192];
	// int x;
	// for (x = 0; x < 4096; x++) {
	// 	d[x] = 'a';
	// 	d[x+4096] = 'b';
	// }

	struct m0_uint128 write_obj_id = { obj_hi, obj_low };

	if (instance_init_done == 0) {
		M0_LOG(M0_ERROR,"Error! reading object before initializing instance!\n");
		return -1;
	}
	M0_LOG(M0_ALWAYS,"---- menglog: done m0_client_init\n");

	struct m0_container       motr_container;

	m0_container_init(&motr_container, NULL, &M0_UBER_REALM, m0_instance);
	rc = motr_container.co_realm.re_entity.en_sm.sm_rc;
	if (rc != 0) {
		M0_LOG(M0_ALWAYS,"error in m0_container_init: %d\n", rc);
		return rc;
	}
	M0_LOG(M0_ALWAYS, "---- menglog: done m0_container_init\n");
	
	struct m0_container *container = &motr_container;

    struct m0_indexvec ext;
	struct m0_bufvec   data;
    struct m0_bufvec   attr;

	uint64_t	last_offset = start;
	int block_count = len / 4096;

	M0_SET0(&obj);
	instance = container->co_realm.re_instance;
	m0_obj_init(&obj, &container->co_realm, &write_obj_id,
		    m0_client_layout_id(instance));

	rc = object_open(&obj);
	if (rc != 0) {
		printf("Failed to open object: rc=%d\n", rc);
		return rc;
	}

	/*
	 * alloc & prepare ext, data and attr. We will write 4k * 2.
	 */
	rc = alloc_vecs(&ext, &data, &attr, block_count, 4096);
	if (rc != 0) {
		printf("Failed to alloc ext & data & attr: %d\n", rc);
		goto out;
	}
	// prepare_ext_vecs(&ext, &data, &attr, 2, 4096, &last_offset, 'A');
	prepare_ext_vecs_write(&ext, &data, &attr, block_count, 4096, &last_offset, d);

	/* Start to write data to object */
	rc = write_data_to_object(&obj, &ext, &data, &attr);
	cleanup_vecs(&ext, &data, &attr);

out:
	/* Similar to close() */
	m0_entity_fini(&obj.ob_entity);

	printf("Object write: %d\n", rc);
	return rc;
}
M0_EXPORTED(m0_object_write);


#undef M0_TRACE_SUBSYSTEM
