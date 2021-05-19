/* This file is generated by venus-protocol git-2564dba1. */

/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VN_PROTOCOL_DRIVER_DESCRIPTOR_POOL_H
#define VN_PROTOCOL_DRIVER_DESCRIPTOR_POOL_H

#include "vn_device.h"
#include "vn_protocol_driver_structs.h"

/* struct VkDescriptorPoolSize */

static inline size_t
vn_sizeof_VkDescriptorPoolSize(const VkDescriptorPoolSize *val)
{
    size_t size = 0;
    size += vn_sizeof_VkDescriptorType(&val->type);
    size += vn_sizeof_uint32_t(&val->descriptorCount);
    return size;
}

static inline void
vn_encode_VkDescriptorPoolSize(struct vn_cs_encoder *enc, const VkDescriptorPoolSize *val)
{
    vn_encode_VkDescriptorType(enc, &val->type);
    vn_encode_uint32_t(enc, &val->descriptorCount);
}

/* struct VkDescriptorPoolCreateInfo chain */

static inline size_t
vn_sizeof_VkDescriptorPoolCreateInfo_pnext(const void *val)
{
    /* no known/supported struct */
    return vn_sizeof_simple_pointer(NULL);
}

static inline size_t
vn_sizeof_VkDescriptorPoolCreateInfo_self(const VkDescriptorPoolCreateInfo *val)
{
    size_t size = 0;
    /* skip val->{sType,pNext} */
    size += vn_sizeof_VkFlags(&val->flags);
    size += vn_sizeof_uint32_t(&val->maxSets);
    size += vn_sizeof_uint32_t(&val->poolSizeCount);
    if (val->pPoolSizes) {
        size += vn_sizeof_array_size(val->poolSizeCount);
        for (uint32_t i = 0; i < val->poolSizeCount; i++)
            size += vn_sizeof_VkDescriptorPoolSize(&val->pPoolSizes[i]);
    } else {
        size += vn_sizeof_array_size(0);
    }
    return size;
}

static inline size_t
vn_sizeof_VkDescriptorPoolCreateInfo(const VkDescriptorPoolCreateInfo *val)
{
    size_t size = 0;

    size += vn_sizeof_VkStructureType(&val->sType);
    size += vn_sizeof_VkDescriptorPoolCreateInfo_pnext(val->pNext);
    size += vn_sizeof_VkDescriptorPoolCreateInfo_self(val);

    return size;
}

static inline void
vn_encode_VkDescriptorPoolCreateInfo_pnext(struct vn_cs_encoder *enc, const void *val)
{
    /* no known/supported struct */
    vn_encode_simple_pointer(enc, NULL);
}

static inline void
vn_encode_VkDescriptorPoolCreateInfo_self(struct vn_cs_encoder *enc, const VkDescriptorPoolCreateInfo *val)
{
    /* skip val->{sType,pNext} */
    vn_encode_VkFlags(enc, &val->flags);
    vn_encode_uint32_t(enc, &val->maxSets);
    vn_encode_uint32_t(enc, &val->poolSizeCount);
    if (val->pPoolSizes) {
        vn_encode_array_size(enc, val->poolSizeCount);
        for (uint32_t i = 0; i < val->poolSizeCount; i++)
            vn_encode_VkDescriptorPoolSize(enc, &val->pPoolSizes[i]);
    } else {
        vn_encode_array_size(enc, 0);
    }
}

static inline void
vn_encode_VkDescriptorPoolCreateInfo(struct vn_cs_encoder *enc, const VkDescriptorPoolCreateInfo *val)
{
    assert(val->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    vn_encode_VkStructureType(enc, &(VkStructureType){ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO });
    vn_encode_VkDescriptorPoolCreateInfo_pnext(enc, val->pNext);
    vn_encode_VkDescriptorPoolCreateInfo_self(enc, val);
}

static inline size_t vn_sizeof_vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool)
{
    const VkCommandTypeEXT cmd_type = VK_COMMAND_TYPE_vkCreateDescriptorPool_EXT;
    const VkFlags cmd_flags = 0;
    size_t cmd_size = vn_sizeof_VkCommandTypeEXT(&cmd_type) + vn_sizeof_VkFlags(&cmd_flags);

    cmd_size += vn_sizeof_VkDevice(&device);
    cmd_size += vn_sizeof_simple_pointer(pCreateInfo);
    if (pCreateInfo)
        cmd_size += vn_sizeof_VkDescriptorPoolCreateInfo(pCreateInfo);
    cmd_size += vn_sizeof_simple_pointer(pAllocator);
    if (pAllocator)
        assert(false);
    cmd_size += vn_sizeof_simple_pointer(pDescriptorPool);
    if (pDescriptorPool)
        cmd_size += vn_sizeof_VkDescriptorPool(pDescriptorPool);

    return cmd_size;
}

static inline void vn_encode_vkCreateDescriptorPool(struct vn_cs_encoder *enc, VkCommandFlagsEXT cmd_flags, VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool)
{
    const VkCommandTypeEXT cmd_type = VK_COMMAND_TYPE_vkCreateDescriptorPool_EXT;

    vn_encode_VkCommandTypeEXT(enc, &cmd_type);
    vn_encode_VkFlags(enc, &cmd_flags);

    vn_encode_VkDevice(enc, &device);
    if (vn_encode_simple_pointer(enc, pCreateInfo))
        vn_encode_VkDescriptorPoolCreateInfo(enc, pCreateInfo);
    if (vn_encode_simple_pointer(enc, pAllocator))
        assert(false);
    if (vn_encode_simple_pointer(enc, pDescriptorPool))
        vn_encode_VkDescriptorPool(enc, pDescriptorPool);
}

static inline size_t vn_sizeof_vkCreateDescriptorPool_reply(VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool)
{
    const VkCommandTypeEXT cmd_type = VK_COMMAND_TYPE_vkCreateDescriptorPool_EXT;
    size_t cmd_size = vn_sizeof_VkCommandTypeEXT(&cmd_type);

    VkResult ret;
    cmd_size += vn_sizeof_VkResult(&ret);
    /* skip device */
    /* skip pCreateInfo */
    /* skip pAllocator */
    cmd_size += vn_sizeof_simple_pointer(pDescriptorPool);
    if (pDescriptorPool)
        cmd_size += vn_sizeof_VkDescriptorPool(pDescriptorPool);

    return cmd_size;
}

static inline VkResult vn_decode_vkCreateDescriptorPool_reply(struct vn_cs_decoder *dec, VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool)
{
    VkCommandTypeEXT command_type;
    vn_decode_VkCommandTypeEXT(dec, &command_type);
    assert(command_type == VK_COMMAND_TYPE_vkCreateDescriptorPool_EXT);

    VkResult ret;
    vn_decode_VkResult(dec, &ret);
    /* skip device */
    /* skip pCreateInfo */
    /* skip pAllocator */
    if (vn_decode_simple_pointer(dec)) {
        vn_decode_VkDescriptorPool(dec, pDescriptorPool);
    } else {
        pDescriptorPool = NULL;
    }

    return ret;
}

static inline size_t vn_sizeof_vkDestroyDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks* pAllocator)
{
    const VkCommandTypeEXT cmd_type = VK_COMMAND_TYPE_vkDestroyDescriptorPool_EXT;
    const VkFlags cmd_flags = 0;
    size_t cmd_size = vn_sizeof_VkCommandTypeEXT(&cmd_type) + vn_sizeof_VkFlags(&cmd_flags);

    cmd_size += vn_sizeof_VkDevice(&device);
    cmd_size += vn_sizeof_VkDescriptorPool(&descriptorPool);
    cmd_size += vn_sizeof_simple_pointer(pAllocator);
    if (pAllocator)
        assert(false);

    return cmd_size;
}

static inline void vn_encode_vkDestroyDescriptorPool(struct vn_cs_encoder *enc, VkCommandFlagsEXT cmd_flags, VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks* pAllocator)
{
    const VkCommandTypeEXT cmd_type = VK_COMMAND_TYPE_vkDestroyDescriptorPool_EXT;

    vn_encode_VkCommandTypeEXT(enc, &cmd_type);
    vn_encode_VkFlags(enc, &cmd_flags);

    vn_encode_VkDevice(enc, &device);
    vn_encode_VkDescriptorPool(enc, &descriptorPool);
    if (vn_encode_simple_pointer(enc, pAllocator))
        assert(false);
}

static inline size_t vn_sizeof_vkDestroyDescriptorPool_reply(VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks* pAllocator)
{
    const VkCommandTypeEXT cmd_type = VK_COMMAND_TYPE_vkDestroyDescriptorPool_EXT;
    size_t cmd_size = vn_sizeof_VkCommandTypeEXT(&cmd_type);

    /* skip device */
    /* skip descriptorPool */
    /* skip pAllocator */

    return cmd_size;
}

static inline void vn_decode_vkDestroyDescriptorPool_reply(struct vn_cs_decoder *dec, VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks* pAllocator)
{
    VkCommandTypeEXT command_type;
    vn_decode_VkCommandTypeEXT(dec, &command_type);
    assert(command_type == VK_COMMAND_TYPE_vkDestroyDescriptorPool_EXT);

    /* skip device */
    /* skip descriptorPool */
    /* skip pAllocator */
}

static inline size_t vn_sizeof_vkResetDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags)
{
    const VkCommandTypeEXT cmd_type = VK_COMMAND_TYPE_vkResetDescriptorPool_EXT;
    const VkFlags cmd_flags = 0;
    size_t cmd_size = vn_sizeof_VkCommandTypeEXT(&cmd_type) + vn_sizeof_VkFlags(&cmd_flags);

    cmd_size += vn_sizeof_VkDevice(&device);
    cmd_size += vn_sizeof_VkDescriptorPool(&descriptorPool);
    cmd_size += vn_sizeof_VkFlags(&flags);

    return cmd_size;
}

static inline void vn_encode_vkResetDescriptorPool(struct vn_cs_encoder *enc, VkCommandFlagsEXT cmd_flags, VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags)
{
    const VkCommandTypeEXT cmd_type = VK_COMMAND_TYPE_vkResetDescriptorPool_EXT;

    vn_encode_VkCommandTypeEXT(enc, &cmd_type);
    vn_encode_VkFlags(enc, &cmd_flags);

    vn_encode_VkDevice(enc, &device);
    vn_encode_VkDescriptorPool(enc, &descriptorPool);
    vn_encode_VkFlags(enc, &flags);
}

static inline size_t vn_sizeof_vkResetDescriptorPool_reply(VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags)
{
    const VkCommandTypeEXT cmd_type = VK_COMMAND_TYPE_vkResetDescriptorPool_EXT;
    size_t cmd_size = vn_sizeof_VkCommandTypeEXT(&cmd_type);

    VkResult ret;
    cmd_size += vn_sizeof_VkResult(&ret);
    /* skip device */
    /* skip descriptorPool */
    /* skip flags */

    return cmd_size;
}

static inline VkResult vn_decode_vkResetDescriptorPool_reply(struct vn_cs_decoder *dec, VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags)
{
    VkCommandTypeEXT command_type;
    vn_decode_VkCommandTypeEXT(dec, &command_type);
    assert(command_type == VK_COMMAND_TYPE_vkResetDescriptorPool_EXT);

    VkResult ret;
    vn_decode_VkResult(dec, &ret);
    /* skip device */
    /* skip descriptorPool */
    /* skip flags */

    return ret;
}

static inline void vn_submit_vkCreateDescriptorPool(struct vn_instance *vn_instance, VkCommandFlagsEXT cmd_flags, VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool, struct vn_instance_submit_command *submit)
{
    uint8_t local_cmd_data[VN_SUBMIT_LOCAL_CMD_SIZE];
    void *cmd_data = local_cmd_data;
    size_t cmd_size = vn_sizeof_vkCreateDescriptorPool(device, pCreateInfo, pAllocator, pDescriptorPool);
    if (cmd_size > sizeof(local_cmd_data)) {
        cmd_data = malloc(cmd_size);
        if (!cmd_data)
            cmd_size = 0;
    }

    submit->command = VN_CS_ENCODER_INITIALIZER(cmd_data, cmd_size);
    if (cmd_size)
        vn_encode_vkCreateDescriptorPool(&submit->command, cmd_flags, device, pCreateInfo, pAllocator, pDescriptorPool);
    submit->reply_size = cmd_flags & VK_COMMAND_GENERATE_REPLY_BIT_EXT ? vn_sizeof_vkCreateDescriptorPool_reply(device, pCreateInfo, pAllocator, pDescriptorPool) : 0;
    vn_instance_submit_command(vn_instance, submit);

    if (cmd_data != local_cmd_data)
        free(cmd_data);
}

static inline void vn_submit_vkDestroyDescriptorPool(struct vn_instance *vn_instance, VkCommandFlagsEXT cmd_flags, VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks* pAllocator, struct vn_instance_submit_command *submit)
{
    uint8_t local_cmd_data[VN_SUBMIT_LOCAL_CMD_SIZE];
    void *cmd_data = local_cmd_data;
    size_t cmd_size = vn_sizeof_vkDestroyDescriptorPool(device, descriptorPool, pAllocator);
    if (cmd_size > sizeof(local_cmd_data)) {
        cmd_data = malloc(cmd_size);
        if (!cmd_data)
            cmd_size = 0;
    }

    submit->command = VN_CS_ENCODER_INITIALIZER(cmd_data, cmd_size);
    if (cmd_size)
        vn_encode_vkDestroyDescriptorPool(&submit->command, cmd_flags, device, descriptorPool, pAllocator);
    submit->reply_size = cmd_flags & VK_COMMAND_GENERATE_REPLY_BIT_EXT ? vn_sizeof_vkDestroyDescriptorPool_reply(device, descriptorPool, pAllocator) : 0;
    vn_instance_submit_command(vn_instance, submit);

    if (cmd_data != local_cmd_data)
        free(cmd_data);
}

static inline void vn_submit_vkResetDescriptorPool(struct vn_instance *vn_instance, VkCommandFlagsEXT cmd_flags, VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags, struct vn_instance_submit_command *submit)
{
    uint8_t local_cmd_data[VN_SUBMIT_LOCAL_CMD_SIZE];
    void *cmd_data = local_cmd_data;
    size_t cmd_size = vn_sizeof_vkResetDescriptorPool(device, descriptorPool, flags);
    if (cmd_size > sizeof(local_cmd_data)) {
        cmd_data = malloc(cmd_size);
        if (!cmd_data)
            cmd_size = 0;
    }

    submit->command = VN_CS_ENCODER_INITIALIZER(cmd_data, cmd_size);
    if (cmd_size)
        vn_encode_vkResetDescriptorPool(&submit->command, cmd_flags, device, descriptorPool, flags);
    submit->reply_size = cmd_flags & VK_COMMAND_GENERATE_REPLY_BIT_EXT ? vn_sizeof_vkResetDescriptorPool_reply(device, descriptorPool, flags) : 0;
    vn_instance_submit_command(vn_instance, submit);

    if (cmd_data != local_cmd_data)
        free(cmd_data);
}

static inline VkResult vn_call_vkCreateDescriptorPool(struct vn_instance *vn_instance, VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool)
{
    struct vn_instance_submit_command submit;
    vn_submit_vkCreateDescriptorPool(vn_instance, VK_COMMAND_GENERATE_REPLY_BIT_EXT, device, pCreateInfo, pAllocator, pDescriptorPool, &submit);
    if (submit.reply_bo) {
        const VkResult ret = vn_decode_vkCreateDescriptorPool_reply(&submit.reply, device, pCreateInfo, pAllocator, pDescriptorPool);
        vn_renderer_bo_unref(submit.reply_bo);
        return ret;
    } else {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

static inline void vn_async_vkCreateDescriptorPool(struct vn_instance *vn_instance, VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool)
{
    struct vn_instance_submit_command submit;
    vn_submit_vkCreateDescriptorPool(vn_instance, 0, device, pCreateInfo, pAllocator, pDescriptorPool, &submit);
}

static inline void vn_call_vkDestroyDescriptorPool(struct vn_instance *vn_instance, VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks* pAllocator)
{
    struct vn_instance_submit_command submit;
    vn_submit_vkDestroyDescriptorPool(vn_instance, VK_COMMAND_GENERATE_REPLY_BIT_EXT, device, descriptorPool, pAllocator, &submit);
    if (submit.reply_bo) {
        vn_decode_vkDestroyDescriptorPool_reply(&submit.reply, device, descriptorPool, pAllocator);
        vn_renderer_bo_unref(submit.reply_bo);
    }
}

static inline void vn_async_vkDestroyDescriptorPool(struct vn_instance *vn_instance, VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks* pAllocator)
{
    struct vn_instance_submit_command submit;
    vn_submit_vkDestroyDescriptorPool(vn_instance, 0, device, descriptorPool, pAllocator, &submit);
}

static inline VkResult vn_call_vkResetDescriptorPool(struct vn_instance *vn_instance, VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags)
{
    struct vn_instance_submit_command submit;
    vn_submit_vkResetDescriptorPool(vn_instance, VK_COMMAND_GENERATE_REPLY_BIT_EXT, device, descriptorPool, flags, &submit);
    if (submit.reply_bo) {
        const VkResult ret = vn_decode_vkResetDescriptorPool_reply(&submit.reply, device, descriptorPool, flags);
        vn_renderer_bo_unref(submit.reply_bo);
        return ret;
    } else {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

static inline void vn_async_vkResetDescriptorPool(struct vn_instance *vn_instance, VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags)
{
    struct vn_instance_submit_command submit;
    vn_submit_vkResetDescriptorPool(vn_instance, 0, device, descriptorPool, flags, &submit);
}

#endif /* VN_PROTOCOL_DRIVER_DESCRIPTOR_POOL_H */
