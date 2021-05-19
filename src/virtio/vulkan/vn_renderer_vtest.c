/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 *
 * based in part on virgl which is:
 * Copyright 2014, 2015 Red Hat.
 */

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "util/os_file.h"
#include "util/u_process.h"
#define VIRGL_RENDERER_UNSTABLE_APIS
#include "virtio-gpu/virglrenderer_hw.h"
#include "vtest/vtest_protocol.h"

#include "vn_renderer.h"

#define VTEST_PCI_VENDOR_ID 0x1af4
#define VTEST_PCI_DEVICE_ID 0x1050

struct vtest;

struct vtest_bo {
   struct vn_renderer_bo base;
   struct vtest *vtest;

   uint32_t blob_flags;
   VkDeviceSize size;
   /* might be closed after mmap */
   int res_fd;

   void *res_ptr;
};

struct vtest_sync {
   struct vn_renderer_sync base;
   struct vtest *vtest;
};

struct vtest {
   struct vn_renderer base;

   struct vn_instance *instance;

   mtx_t sock_mutex;
   int sock_fd;

   uint32_t protocol_version;
   uint32_t max_sync_queue_count;

   struct {
      enum virgl_renderer_capset id;
      uint32_t version;
      struct virgl_renderer_capset_venus data;
   } capset;
};

static int
vtest_connect_socket(struct vn_instance *instance, const char *path)
{
   struct sockaddr_un un;
   int sock;

   sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (sock < 0) {
      vn_log(instance, "failed to create a socket");
      return -1;
   }

   memset(&un, 0, sizeof(un));
   un.sun_family = AF_UNIX;
   memcpy(un.sun_path, path, strlen(path));

   if (connect(sock, (struct sockaddr *)&un, sizeof(un)) == -1) {
      vn_log(instance, "failed to connect to %s: %s", path, strerror(errno));
      close(sock);
      return -1;
   }

   return sock;
}

static void
vtest_read(struct vtest *vtest, void *buf, size_t size)
{
   do {
      const ssize_t ret = read(vtest->sock_fd, buf, size);
      if (unlikely(ret < 0)) {
         vn_log(vtest->instance,
                "lost connection to rendering server on %zu read %zi %d",
                size, ret, errno);
         abort();
      }

      buf += ret;
      size -= ret;
   } while (size);
}

static int
vtest_receive_fd(struct vtest *vtest)
{
   char cmsg_buf[CMSG_SPACE(sizeof(int))];
   char dummy;
   struct msghdr msg = {
      .msg_iov =
         &(struct iovec){
            .iov_base = &dummy,
            .iov_len = sizeof(dummy),
         },
      .msg_iovlen = 1,
      .msg_control = cmsg_buf,
      .msg_controllen = sizeof(cmsg_buf),
   };

   if (recvmsg(vtest->sock_fd, &msg, 0) < 0) {
      vn_log(vtest->instance, "recvmsg failed: %s", strerror(errno));
      abort();
   }

   struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
   if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
       cmsg->cmsg_type != SCM_RIGHTS) {
      vn_log(vtest->instance, "invalid cmsghdr");
      abort();
   }

   return *((int *)CMSG_DATA(cmsg));
}

static void
vtest_write(struct vtest *vtest, const void *buf, size_t size)
{
   do {
      const ssize_t ret = write(vtest->sock_fd, buf, size);
      if (unlikely(ret < 0)) {
         vn_log(vtest->instance,
                "lost connection to rendering server on %zu write %zi %d",
                size, ret, errno);
         abort();
      }

      buf += ret;
      size -= ret;
   } while (size);
}

static void
vtest_vcmd_create_renderer(struct vtest *vtest, const char *name)
{
   const size_t size = strlen(name) + 1;

   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = size;
   vtest_hdr[VTEST_CMD_ID] = VCMD_CREATE_RENDERER;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, name, size);
}

static bool
vtest_vcmd_ping_protocol_version(struct vtest *vtest)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_PING_PROTOCOL_VERSION_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_PING_PROTOCOL_VERSION;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));

   /* send a dummy busy wait to avoid blocking in vtest_read in case ping
    * protocol version is not supported
    */
   uint32_t vcmd_busy_wait[VCMD_BUSY_WAIT_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_BUSY_WAIT_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_RESOURCE_BUSY_WAIT;
   vcmd_busy_wait[VCMD_BUSY_WAIT_HANDLE] = 0;
   vcmd_busy_wait[VCMD_BUSY_WAIT_FLAGS] = 0;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_busy_wait, sizeof(vcmd_busy_wait));

   uint32_t dummy;
   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   if (vtest_hdr[VTEST_CMD_ID] == VCMD_PING_PROTOCOL_VERSION) {
      /* consume the dummy busy wait result */
      vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
      assert(vtest_hdr[VTEST_CMD_ID] == VCMD_RESOURCE_BUSY_WAIT);
      vtest_read(vtest, &dummy, sizeof(dummy));
      return true;
   } else {
      /* no ping protocol version support */
      assert(vtest_hdr[VTEST_CMD_ID] == VCMD_RESOURCE_BUSY_WAIT);
      vtest_read(vtest, &dummy, sizeof(dummy));
      return false;
   }
}

static uint32_t
vtest_vcmd_protocol_version(struct vtest *vtest)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_protocol_version[VCMD_PROTOCOL_VERSION_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_PROTOCOL_VERSION_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_PROTOCOL_VERSION;
   vcmd_protocol_version[VCMD_PROTOCOL_VERSION_VERSION] =
      VTEST_PROTOCOL_VERSION;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_protocol_version, sizeof(vcmd_protocol_version));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == VCMD_PROTOCOL_VERSION_SIZE);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_PROTOCOL_VERSION);
   vtest_read(vtest, vcmd_protocol_version, sizeof(vcmd_protocol_version));

   return vcmd_protocol_version[VCMD_PROTOCOL_VERSION_VERSION];
}

static uint32_t
vtest_vcmd_get_param(struct vtest *vtest, enum vcmd_param param)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_get_param[VCMD_GET_PARAM_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_GET_PARAM_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_GET_PARAM;
   vcmd_get_param[VCMD_GET_PARAM_PARAM] = param;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_get_param, sizeof(vcmd_get_param));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == 2);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_GET_PARAM);

   uint32_t resp[2];
   vtest_read(vtest, resp, sizeof(resp));

   return resp[0] ? resp[1] : 0;
}

static bool
vtest_vcmd_get_capset(struct vtest *vtest,
                      enum virgl_renderer_capset id,
                      uint32_t version,
                      void *capset,
                      size_t capset_size)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_get_capset[VCMD_GET_CAPSET_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_GET_CAPSET_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_GET_CAPSET;
   vcmd_get_capset[VCMD_GET_CAPSET_ID] = id;
   vcmd_get_capset[VCMD_GET_CAPSET_VERSION] = version;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_get_capset, sizeof(vcmd_get_capset));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_GET_CAPSET);

   uint32_t valid;
   vtest_read(vtest, &valid, sizeof(valid));
   if (!valid)
      return false;

   size_t read_size = (vtest_hdr[VTEST_CMD_LEN] - 1) * 4;
   if (capset_size >= read_size) {
      vtest_read(vtest, capset, read_size);
      memset(capset + read_size, 0, capset_size - read_size);
   } else {
      vtest_read(vtest, capset, capset_size);

      char temp[256];
      read_size -= capset_size;
      while (read_size) {
         const size_t temp_size = MIN2(read_size, ARRAY_SIZE(temp));
         vtest_read(vtest, temp, temp_size);
         read_size -= temp_size;
      }
   }

   return true;
}

static void
vtest_vcmd_context_init(struct vtest *vtest,
                        enum virgl_renderer_capset capset_id)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_context_init[VCMD_CONTEXT_INIT_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_CONTEXT_INIT_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_CONTEXT_INIT;
   vcmd_context_init[VCMD_CONTEXT_INIT_CAPSET_ID] = capset_id;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_context_init, sizeof(vcmd_context_init));
}

static uint32_t
vtest_vcmd_resource_create_blob(struct vtest *vtest,
                                enum vcmd_blob_type type,
                                uint32_t flags,
                                VkDeviceSize size,
                                vn_object_id blob_id,
                                int *res_fd)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_RES_CREATE_BLOB_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_RESOURCE_CREATE_BLOB;

   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_TYPE] = type;
   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_FLAGS] = flags;
   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_SIZE_LO] = (uint32_t)size;
   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_SIZE_HI] =
      (uint32_t)(size >> 32);
   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_ID_LO] = (uint32_t)blob_id;
   vcmd_res_create_blob[VCMD_RES_CREATE_BLOB_ID_HI] =
      (uint32_t)(blob_id >> 32);

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_res_create_blob, sizeof(vcmd_res_create_blob));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == 1);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_RESOURCE_CREATE_BLOB);

   uint32_t res_id;
   vtest_read(vtest, &res_id, sizeof(res_id));

   *res_fd = vtest_receive_fd(vtest);

   return res_id;
}

static void
vtest_vcmd_resource_unref(struct vtest *vtest, uint32_t res_id)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_res_unref[VCMD_RES_UNREF_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_RES_UNREF_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_RESOURCE_UNREF;
   vcmd_res_unref[VCMD_RES_UNREF_RES_HANDLE] = res_id;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_res_unref, sizeof(vcmd_res_unref));
}

static uint32_t
vtest_vcmd_sync_create(struct vtest *vtest, uint64_t initial_val)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_sync_create[VCMD_SYNC_CREATE_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_SYNC_CREATE_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_SYNC_CREATE;

   vcmd_sync_create[VCMD_SYNC_CREATE_VALUE_LO] = (uint32_t)initial_val;
   vcmd_sync_create[VCMD_SYNC_CREATE_VALUE_HI] =
      (uint32_t)(initial_val >> 32);

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_sync_create, sizeof(vcmd_sync_create));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == 1);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_SYNC_CREATE);

   uint32_t sync_id;
   vtest_read(vtest, &sync_id, sizeof(sync_id));

   return sync_id;
}

static void
vtest_vcmd_sync_unref(struct vtest *vtest, uint32_t sync_id)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_sync_unref[VCMD_SYNC_UNREF_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_SYNC_UNREF_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_SYNC_UNREF;
   vcmd_sync_unref[VCMD_SYNC_UNREF_ID] = sync_id;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_sync_unref, sizeof(vcmd_sync_unref));
}

static uint64_t
vtest_vcmd_sync_read(struct vtest *vtest, uint32_t sync_id)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_sync_read[VCMD_SYNC_READ_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_SYNC_READ_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_SYNC_READ;

   vcmd_sync_read[VCMD_SYNC_READ_ID] = sync_id;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_sync_read, sizeof(vcmd_sync_read));

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == 2);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_SYNC_READ);

   uint64_t val;
   vtest_read(vtest, &val, sizeof(val));

   return val;
}

static void
vtest_vcmd_sync_write(struct vtest *vtest, uint32_t sync_id, uint64_t val)
{
   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   uint32_t vcmd_sync_write[VCMD_SYNC_WRITE_SIZE];

   vtest_hdr[VTEST_CMD_LEN] = VCMD_SYNC_WRITE_SIZE;
   vtest_hdr[VTEST_CMD_ID] = VCMD_SYNC_WRITE;

   vcmd_sync_write[VCMD_SYNC_WRITE_ID] = sync_id;
   vcmd_sync_write[VCMD_SYNC_WRITE_VALUE_LO] = (uint32_t)val;
   vcmd_sync_write[VCMD_SYNC_WRITE_VALUE_HI] = (uint32_t)(val >> 32);

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, vcmd_sync_write, sizeof(vcmd_sync_write));
}

static int
vtest_vcmd_sync_wait(struct vtest *vtest,
                     uint32_t flags,
                     int poll_timeout,
                     struct vn_renderer_sync *const *syncs,
                     const uint64_t *vals,
                     uint32_t count)
{
   const uint32_t timeout = poll_timeout >= 0 && poll_timeout <= INT32_MAX
                               ? poll_timeout
                               : UINT32_MAX;

   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = VCMD_SYNC_WAIT_SIZE(count);
   vtest_hdr[VTEST_CMD_ID] = VCMD_SYNC_WAIT;

   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));
   vtest_write(vtest, &flags, sizeof(flags));
   vtest_write(vtest, &timeout, sizeof(timeout));
   for (uint32_t i = 0; i < count; i++) {
      const uint64_t val = vals[i];
      const uint32_t sync[3] = {
         syncs[i]->sync_id,
         (uint32_t)val,
         (uint32_t)(val >> 32),
      };
      vtest_write(vtest, sync, sizeof(sync));
   }

   vtest_read(vtest, vtest_hdr, sizeof(vtest_hdr));
   assert(vtest_hdr[VTEST_CMD_LEN] == 0);
   assert(vtest_hdr[VTEST_CMD_ID] == VCMD_SYNC_WAIT);

   return vtest_receive_fd(vtest);
}

static void
submit_cmd2_sizes(const struct vn_renderer_submit *submit,
                  size_t *header_size,
                  size_t *cs_size,
                  size_t *sync_size)
{
   if (!submit->batch_count) {
      *header_size = 0;
      *cs_size = 0;
      *sync_size = 0;
      return;
   }

   *header_size = sizeof(uint32_t) +
                  sizeof(struct vcmd_submit_cmd2_batch) * submit->batch_count;

   *cs_size = 0;
   *sync_size = 0;
   for (uint32_t i = 0; i < submit->batch_count; i++) {
      const struct vn_renderer_submit_batch *batch = &submit->batches[i];
      assert(batch->cs_size % sizeof(uint32_t) == 0);
      *cs_size += batch->cs_size;
      *sync_size += (sizeof(uint32_t) + sizeof(uint64_t)) * batch->sync_count;
   }

   assert(*header_size % sizeof(uint32_t) == 0);
   assert(*cs_size % sizeof(uint32_t) == 0);
   assert(*sync_size % sizeof(uint32_t) == 0);
}

static void
vtest_vcmd_submit_cmd2(struct vtest *vtest,
                       const struct vn_renderer_submit *submit)
{
   size_t header_size;
   size_t cs_size;
   size_t sync_size;
   submit_cmd2_sizes(submit, &header_size, &cs_size, &sync_size);
   const size_t total_size = header_size + cs_size + sync_size;
   if (!total_size)
      return;

   uint32_t vtest_hdr[VTEST_HDR_SIZE];
   vtest_hdr[VTEST_CMD_LEN] = total_size / sizeof(uint32_t);
   vtest_hdr[VTEST_CMD_ID] = VCMD_SUBMIT_CMD2;
   vtest_write(vtest, vtest_hdr, sizeof(vtest_hdr));

   /* write batch count and batch headers */
   const uint32_t batch_count = submit->batch_count;
   size_t cs_offset = header_size;
   size_t sync_offset = cs_offset + cs_size;
   vtest_write(vtest, &batch_count, sizeof(batch_count));
   for (uint32_t i = 0; i < submit->batch_count; i++) {
      const struct vn_renderer_submit_batch *batch = &submit->batches[i];
      struct vcmd_submit_cmd2_batch dst = {
         .cmd_offset = cs_offset / sizeof(uint32_t),
         .cmd_size = batch->cs_size / sizeof(uint32_t),
         .sync_offset = sync_offset / sizeof(uint32_t),
         .sync_count = batch->sync_count,
      };
      if (!batch->sync_queue_cpu) {
         dst.flags = VCMD_SUBMIT_CMD2_FLAG_SYNC_QUEUE;
         dst.sync_queue_index = batch->sync_queue_index;
         dst.sync_queue_id = batch->vk_queue_id;
      }
      vtest_write(vtest, &dst, sizeof(dst));

      cs_offset += batch->cs_size;
      sync_offset +=
         (sizeof(uint32_t) + sizeof(uint64_t)) * batch->sync_count;
   }

   /* write cs */
   if (cs_size) {
      for (uint32_t i = 0; i < submit->batch_count; i++) {
         const struct vn_renderer_submit_batch *batch = &submit->batches[i];
         if (batch->cs_size)
            vtest_write(vtest, batch->cs_data, batch->cs_size);
      }
   }

   /* write syncs */
   for (uint32_t i = 0; i < submit->batch_count; i++) {
      const struct vn_renderer_submit_batch *batch = &submit->batches[i];

      for (uint32_t j = 0; j < batch->sync_count; j++) {
         const uint64_t val = batch->sync_values[j];
         const uint32_t sync[3] = {
            batch->syncs[j]->sync_id,
            (uint32_t)val,
            (uint32_t)(val >> 32),
         };
         vtest_write(vtest, sync, sizeof(sync));
      }
   }
}

static VkResult
vtest_sync_write(struct vn_renderer_sync *_sync, uint64_t val)
{
   struct vtest_sync *sync = (struct vtest_sync *)_sync;
   struct vtest *vtest = sync->vtest;

   mtx_lock(&vtest->sock_mutex);
   vtest_vcmd_sync_write(vtest, sync->base.sync_id, val);
   mtx_unlock(&vtest->sock_mutex);

   return VK_SUCCESS;
}

static VkResult
vtest_sync_read(struct vn_renderer_sync *_sync, uint64_t *val)
{
   struct vtest_sync *sync = (struct vtest_sync *)_sync;
   struct vtest *vtest = sync->vtest;

   mtx_lock(&vtest->sock_mutex);
   *val = vtest_vcmd_sync_read(vtest, sync->base.sync_id);
   mtx_unlock(&vtest->sock_mutex);

   return VK_SUCCESS;
}

static VkResult
vtest_sync_reset(struct vn_renderer_sync *sync, uint64_t initial_val)
{
   /* same as write */
   return vtest_sync_write(sync, initial_val);
}

static void
vtest_sync_release(struct vn_renderer_sync *_sync)
{
   struct vtest_sync *sync = (struct vtest_sync *)_sync;
   struct vtest *vtest = sync->vtest;

   mtx_lock(&vtest->sock_mutex);
   vtest_vcmd_sync_unref(vtest, sync->base.sync_id);
   mtx_unlock(&vtest->sock_mutex);

   sync->base.sync_id = 0;
}

static VkResult
vtest_sync_init(struct vn_renderer_sync *_sync,
                uint64_t initial_val,
                uint32_t flags)
{
   struct vtest_sync *sync = (struct vtest_sync *)_sync;
   struct vtest *vtest = sync->vtest;

   mtx_lock(&vtest->sock_mutex);
   sync->base.sync_id = vtest_vcmd_sync_create(vtest, initial_val);
   mtx_unlock(&vtest->sock_mutex);

   return VK_SUCCESS;
}

static void
vtest_sync_destroy(struct vn_renderer_sync *_sync)
{
   struct vtest_sync *sync = (struct vtest_sync *)_sync;

   if (sync->base.sync_id)
      vtest_sync_release(&sync->base);

   free(sync);
}

static struct vn_renderer_sync *
vtest_sync_create(struct vn_renderer *renderer)
{
   struct vtest *vtest = (struct vtest *)renderer;

   struct vtest_sync *sync = calloc(1, sizeof(*sync));
   if (!sync)
      return NULL;

   sync->vtest = vtest;

   sync->base.ops.destroy = vtest_sync_destroy;
   sync->base.ops.init = vtest_sync_init;
   sync->base.ops.init_syncobj = NULL;
   sync->base.ops.release = vtest_sync_release;
   sync->base.ops.export_syncobj = NULL;
   sync->base.ops.reset = vtest_sync_reset;
   sync->base.ops.read = vtest_sync_read;
   sync->base.ops.write = vtest_sync_write;

   return &sync->base;
}

static void
vtest_bo_invalidate(struct vn_renderer_bo *bo,
                    VkDeviceSize offset,
                    VkDeviceSize size)
{
   /* nop */
}

static void
vtest_bo_flush(struct vn_renderer_bo *bo,
               VkDeviceSize offset,
               VkDeviceSize size)
{
   /* nop */
}

static void *
vtest_bo_map(struct vn_renderer_bo *_bo)
{
   struct vtest_bo *bo = (struct vtest_bo *)_bo;
   struct vtest *vtest = bo->vtest;
   const bool mappable = bo->blob_flags & VCMD_BLOB_FLAG_MAPPABLE;
   const bool shareable = bo->blob_flags & VCMD_BLOB_FLAG_SHAREABLE;

   /* not thread-safe but is fine */
   if (!bo->res_ptr && mappable) {
      /* We wrongly assume that mmap(dmabuf) and vkMapMemory(VkDeviceMemory)
       * are equivalent when the blob type is VCMD_BLOB_TYPE_HOST3D.  While we
       * check for VCMD_PARAM_HOST_COHERENT_DMABUF_BLOB, we know vtest can
       * lie.
       */
      void *ptr = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       bo->res_fd, 0);
      if (ptr == MAP_FAILED) {
         vn_log(vtest->instance,
                "failed to mmap %d of size %" PRIu64 " rw: %s", bo->res_fd,
                bo->size, strerror(errno));
      } else {
         bo->res_ptr = ptr;
         /* we don't need the fd anymore */
         if (!shareable) {
            close(bo->res_fd);
            bo->res_fd = -1;
         }
      }
   }

   return bo->res_ptr;
}

static int
vtest_bo_export_dmabuf(struct vn_renderer_bo *_bo)
{
   const struct vtest_bo *bo = (struct vtest_bo *)_bo;
   /* this suffices because vtest_bo_init_cpu does not set the bit */
   const bool shareable = bo->blob_flags & VCMD_BLOB_FLAG_SHAREABLE;
   return shareable ? os_dupfd_cloexec(bo->res_fd) : -1;
}

static uint32_t
vtest_bo_blob_flags(VkMemoryPropertyFlags flags,
                    VkExternalMemoryHandleTypeFlags external_handles)
{
   uint32_t blob_flags = 0;
   if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      blob_flags |= VCMD_BLOB_FLAG_MAPPABLE;
   if (external_handles)
      blob_flags |= VCMD_BLOB_FLAG_SHAREABLE;
   if (external_handles & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
      blob_flags |= VCMD_BLOB_FLAG_CROSS_DEVICE;

   return blob_flags;
}

static VkResult
vtest_bo_init_gpu(struct vn_renderer_bo *_bo,
                  VkDeviceSize size,
                  vn_object_id mem_id,
                  VkMemoryPropertyFlags flags,
                  VkExternalMemoryHandleTypeFlags external_handles)
{
   struct vtest_bo *bo = (struct vtest_bo *)_bo;
   struct vtest *vtest = bo->vtest;

   bo->blob_flags = vtest_bo_blob_flags(flags, external_handles);
   bo->size = size;

   mtx_lock(&vtest->sock_mutex);
   bo->base.res_id = vtest_vcmd_resource_create_blob(
      vtest, VCMD_BLOB_TYPE_HOST3D, bo->blob_flags, bo->size, mem_id,
      &bo->res_fd);
   mtx_unlock(&vtest->sock_mutex);

   return VK_SUCCESS;
}

static VkResult
vtest_bo_init_cpu(struct vn_renderer_bo *_bo, VkDeviceSize size)
{
   struct vtest_bo *bo = (struct vtest_bo *)_bo;
   struct vtest *vtest = bo->vtest;

   bo->blob_flags = VCMD_BLOB_FLAG_MAPPABLE;
   bo->size = size;

   mtx_lock(&vtest->sock_mutex);
   bo->base.res_id = vtest_vcmd_resource_create_blob(
      vtest, VCMD_BLOB_TYPE_GUEST, bo->blob_flags, bo->size, 0, &bo->res_fd);
   mtx_unlock(&vtest->sock_mutex);

   return VK_SUCCESS;
}

static void
vtest_bo_destroy(struct vn_renderer_bo *_bo)
{
   struct vtest_bo *bo = (struct vtest_bo *)_bo;
   struct vtest *vtest = bo->vtest;

   if (bo->base.res_id) {
      if (bo->res_ptr)
         munmap(bo->res_ptr, bo->size);
      if (bo->res_fd >= 0)
         close(bo->res_fd);

      mtx_lock(&vtest->sock_mutex);
      vtest_vcmd_resource_unref(vtest, bo->base.res_id);
      mtx_unlock(&vtest->sock_mutex);
   }

   free(bo);
}

static struct vn_renderer_bo *
vtest_bo_create(struct vn_renderer *renderer)
{
   struct vtest *vtest = (struct vtest *)renderer;

   struct vtest_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return NULL;

   bo->vtest = vtest;
   bo->res_fd = -1;

   bo->base.ops.destroy = vtest_bo_destroy;
   bo->base.ops.init_cpu = vtest_bo_init_cpu;
   bo->base.ops.init_gpu = vtest_bo_init_gpu;
   bo->base.ops.init_dmabuf = NULL;
   bo->base.ops.export_dmabuf = vtest_bo_export_dmabuf;
   bo->base.ops.map = vtest_bo_map;
   bo->base.ops.flush = vtest_bo_flush;
   bo->base.ops.invalidate = vtest_bo_invalidate;

   return &bo->base;
}

static VkResult
sync_wait_poll(int fd, int poll_timeout)
{
   struct pollfd pollfd = {
      .fd = fd,
      .events = POLLIN,
   };
   int ret;
   do {
      ret = poll(&pollfd, 1, poll_timeout);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   if (ret < 0 || (ret > 0 && !(pollfd.revents & POLLIN))) {
      return (ret < 0 && errno == ENOMEM) ? VK_ERROR_OUT_OF_HOST_MEMORY
                                          : VK_ERROR_DEVICE_LOST;
   }

   return ret ? VK_SUCCESS : VK_TIMEOUT;
}

static int
timeout_to_poll_timeout(uint64_t timeout)
{
   const uint64_t ns_per_ms = 1000000;
   const uint64_t ms = (timeout + ns_per_ms - 1) / ns_per_ms;
   if (!ms && timeout)
      return -1;
   return ms <= INT_MAX ? ms : -1;
}

static VkResult
vtest_wait(struct vn_renderer *renderer, const struct vn_renderer_wait *wait)
{
   struct vtest *vtest = (struct vtest *)renderer;
   const uint32_t flags = wait->wait_any ? VCMD_SYNC_WAIT_FLAG_ANY : 0;
   const int poll_timeout = timeout_to_poll_timeout(wait->timeout);

   /*
    * vtest_vcmd_sync_wait (and some other sync commands) is executed after
    * all prior commands are dispatched.  That is far from ideal.
    *
    * In virtio-gpu, a drm_syncobj wait ioctl is executed immediately.  It
    * works because it uses virtio-gpu interrupts as a side channel.  vtest
    * needs a side channel to perform well.
    *
    * virtio-gpu or vtest, we should also set up a 1-byte coherent memory that
    * is set to non-zero by GPU after the syncs signal.  That would allow us
    * to do a quick check (or spin a bit) before waiting.
    */
   mtx_lock(&vtest->sock_mutex);
   const int fd =
      vtest_vcmd_sync_wait(vtest, flags, poll_timeout, wait->syncs,
                           wait->sync_values, wait->sync_count);
   mtx_unlock(&vtest->sock_mutex);

   VkResult result = sync_wait_poll(fd, poll_timeout);
   close(fd);

   return result;
}

static VkResult
vtest_submit(struct vn_renderer *renderer,
             const struct vn_renderer_submit *submit)
{
   struct vtest *vtest = (struct vtest *)renderer;

   mtx_lock(&vtest->sock_mutex);
   vtest_vcmd_submit_cmd2(vtest, submit);
   mtx_unlock(&vtest->sock_mutex);

   return VK_SUCCESS;
}

static void
vtest_get_info(struct vn_renderer *renderer, struct vn_renderer_info *info)
{
   struct vtest *vtest = (struct vtest *)renderer;

   memset(info, 0, sizeof(*info));

   info->pci.vendor_id = VTEST_PCI_VENDOR_ID;
   info->pci.device_id = VTEST_PCI_DEVICE_ID;

   info->has_dmabuf_import = false;
   info->has_cache_management = false;
   info->has_timeline_sync = true;
   info->has_external_sync = false;

   info->max_sync_queue_count = vtest->max_sync_queue_count;

   const struct virgl_renderer_capset_venus *capset = &vtest->capset.data;
   info->wire_format_version = capset->wire_format_version;
   info->vk_xml_version = capset->vk_xml_version;
   info->vk_ext_command_serialization_spec_version =
      capset->vk_ext_command_serialization_spec_version;
   info->vk_mesa_venus_protocol_spec_version =
      capset->vk_mesa_venus_protocol_spec_version;
}

static void
vtest_destroy(struct vn_renderer *renderer,
              const VkAllocationCallbacks *alloc)
{
   struct vtest *vtest = (struct vtest *)renderer;

   if (vtest->sock_fd >= 0) {
      shutdown(vtest->sock_fd, SHUT_RDWR);
      close(vtest->sock_fd);
   }

   mtx_destroy(&vtest->sock_mutex);

   vk_free(alloc, vtest);
}

static VkResult
vtest_init_capset(struct vtest *vtest)
{
   vtest->capset.id = VIRGL_RENDERER_CAPSET_VENUS;
   vtest->capset.version = 0;

   if (!vtest_vcmd_get_capset(vtest, vtest->capset.id, vtest->capset.version,
                              &vtest->capset.data,
                              sizeof(vtest->capset.data))) {
      vn_log(vtest->instance, "no venus capset");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

static VkResult
vtest_init_params(struct vtest *vtest)
{
   uint32_t val =
      vtest_vcmd_get_param(vtest, VCMD_PARAM_MAX_SYNC_QUEUE_COUNT);
   if (!val) {
      vn_log(vtest->instance, "no sync queue support");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   vtest->max_sync_queue_count = val;

   return VK_SUCCESS;
}

static VkResult
vtest_init_protocol_version(struct vtest *vtest)
{
   const uint32_t min_protocol_version = 3;

   const uint32_t ver = vtest_vcmd_ping_protocol_version(vtest)
                           ? vtest_vcmd_protocol_version(vtest)
                           : 0;
   if (ver < min_protocol_version) {
      vn_log(vtest->instance, "vtest protocol version (%d) too old", ver);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   vtest->protocol_version = ver;

   return VK_SUCCESS;
}

static VkResult
vtest_init(struct vtest *vtest)
{
   mtx_init(&vtest->sock_mutex, mtx_plain);
   vtest->sock_fd =
      vtest_connect_socket(vtest->instance, VTEST_DEFAULT_SOCKET_NAME);
   if (vtest->sock_fd < 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   const char *renderer_name = util_get_process_name();
   if (!renderer_name)
      renderer_name = "venus";
   vtest_vcmd_create_renderer(vtest, renderer_name);

   VkResult result = vtest_init_protocol_version(vtest);
   if (result == VK_SUCCESS)
      result = vtest_init_params(vtest);
   if (result == VK_SUCCESS)
      result = vtest_init_capset(vtest);
   if (result != VK_SUCCESS)
      return result;

   vtest_vcmd_context_init(vtest, vtest->capset.id);

   vtest->base.ops.destroy = vtest_destroy;
   vtest->base.ops.get_info = vtest_get_info;
   vtest->base.ops.submit = vtest_submit;
   vtest->base.ops.wait = vtest_wait;
   vtest->base.ops.bo_create = vtest_bo_create;
   vtest->base.ops.sync_create = vtest_sync_create;

   return VK_SUCCESS;
}

VkResult
vn_renderer_create_vtest(struct vn_instance *instance,
                         const VkAllocationCallbacks *alloc,
                         struct vn_renderer **renderer)
{
   struct vtest *vtest = vk_zalloc(alloc, sizeof(*vtest), VN_DEFAULT_ALIGN,
                                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!vtest)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   vtest->instance = instance;
   vtest->sock_fd = -1;

   VkResult result = vtest_init(vtest);
   if (result != VK_SUCCESS) {
      vtest_destroy(&vtest->base, alloc);
      return result;
   }

   *renderer = &vtest->base;

   return VK_SUCCESS;
}
