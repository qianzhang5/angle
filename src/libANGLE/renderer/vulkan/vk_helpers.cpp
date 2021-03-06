//
// Copyright 2018 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// vk_helpers:
//   Helper utilitiy classes that manage Vulkan resources.

#include "libANGLE/renderer/vulkan/vk_helpers.h"

#include "common/utilities.h"
#include "libANGLE/Context.h"
#include "libANGLE/renderer/vulkan/BufferVk.h"
#include "libANGLE/renderer/vulkan/ContextVk.h"
#include "libANGLE/renderer/vulkan/FramebufferVk.h"
#include "libANGLE/renderer/vulkan/RendererVk.h"
#include "libANGLE/renderer/vulkan/vk_utils.h"
#include "third_party/trace_event/trace_event.h"

namespace rx
{
namespace vk
{
namespace
{
constexpr VkBufferUsageFlags kLineLoopDynamicBufferUsage =
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
constexpr int kLineLoopDynamicBufferMinSize = 1024 * 1024;

// This is an arbitrary max. We can change this later if necessary.
constexpr uint32_t kDefaultDescriptorPoolMaxSets = 128;

struct ImageMemoryBarrierData
{
    // The Vk layout corresponding to the ImageLayout key.
    VkImageLayout layout;
    // The stage in which the image is used (or Bottom/Top if not using any specific stage).  Unless
    // Bottom/Top (Bottom used for transition to and Top used for transition from), the two values
    // should match.
    VkPipelineStageFlags dstStageMask;
    VkPipelineStageFlags srcStageMask;
    // Access mask when transitioning into this layout.
    VkAccessFlags dstAccessMask;
    // Access mask when transitioning out from this layout.  Note that source access mask never
    // needs a READ bit, as WAR hazards don't need memory barriers (just execution barriers).
    VkAccessFlags srcAccessMask;

    // If access is read-only, the execution barrier can be skipped altogether if retransitioning to
    // the same layout. This is because read-after-read does not need an execution or memory
    // barrier.
    bool isReadOnlyAccess;
};

// clang-format off
constexpr angle::PackedEnumMap<ImageLayout, ImageMemoryBarrierData> kImageMemoryBarrierData = {
    {
        ImageLayout::Undefined,
        {
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            // Transition to: we don't expect to transition into Undefined.
            0,
            // Transition from: there's no data in the image to care about.
            0,
            true,
        },
    },
    {
        ImageLayout::PreInitialized,
        {
            VK_IMAGE_LAYOUT_PREINITIALIZED,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            // Transition to: we don't expect to transition into PreInitialized.
            0,
            // Transition from: all writes must finish before barrier.
            VK_ACCESS_HOST_WRITE_BIT,
            false,
        },
    },
    {
        ImageLayout::TransferSrc,
        {
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            // Transition to: all reads must happen after barrier.
            VK_ACCESS_TRANSFER_READ_BIT,
            // Transition from: RAR and WAR don't need memory barrier.
            0,
            true,
        },
    },
    {
        ImageLayout::TransferDst,
        {
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            // Transition to: all writes must happen after barrier.
            VK_ACCESS_TRANSFER_WRITE_BIT,
            // Transition from: all writes must finish before barrier.
            VK_ACCESS_TRANSFER_WRITE_BIT,
            false,
        },
    },
    {
        ImageLayout::ComputeShaderReadOnly,
        {
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            // Transition to: all reads must happen after barrier.
            VK_ACCESS_SHADER_READ_BIT,
            // Transition from: RAR and WAR don't need memory barrier.
            0,
            true,
        },
    },
    {
        ImageLayout::ComputeShaderWrite,
        {
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            // Transition to: all reads and writes must happen after barrier.
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            // Transition from: all writes must finish before barrier.
            VK_ACCESS_SHADER_WRITE_BIT,
            false,
        },
    },
    {
        ImageLayout::FragmentShaderReadOnly,
        {
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            // Transition to: all reads must happen after barrier.
            VK_ACCESS_SHADER_READ_BIT,
            // Transition from: RAR and WAR don't need memory barrier.
            0,
            true,
        },
    },
    {
        ImageLayout::ColorAttachment,
        {
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            // Transition to: all reads and writes must happen after barrier.
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            // Transition from: all writes must finish before barrier.
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            false,
        },
    },
    {
        ImageLayout::DepthStencilAttachment,
        {
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            // Transition to: all reads and writes must happen after barrier.
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            // Transition from: all writes must finish before barrier.
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            false,
        },
    },
    {
        ImageLayout::Present,
        {
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            // transition to: vkQueuePresentKHR automatically performs the appropriate memory barriers:
            //
            // > Any writes to memory backing the images referenced by the pImageIndices and
            // > pSwapchains members of pPresentInfo, that are available before vkQueuePresentKHR
            // > is executed, are automatically made visible to the read access performed by the
            // > presentation engine.
            0,
            // Transition from: RAR and WAR don't need memory barrier.
            0,
            true,
        },
    },
};
// clang-format on

VkImageCreateFlags GetImageCreateFlags(gl::TextureType textureType)
{
    if (textureType == gl::TextureType::CubeMap)
    {
        return VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    else
    {
        return 0;
    }
}
}  // anonymous namespace

// DynamicBuffer implementation.
DynamicBuffer::DynamicBuffer(VkBufferUsageFlags usage, size_t minSize, bool hostVisible)
    : mUsage(usage),
      mHostVisible(hostVisible),
      mMinSize(minSize),
      mBuffer(nullptr),
      mNextAllocationOffset(0),
      mLastFlushOrInvalidateOffset(0),
      mSize(0),
      mAlignment(0)
{}

DynamicBuffer::DynamicBuffer(DynamicBuffer &&other)
    : mUsage(other.mUsage),
      mHostVisible(other.mHostVisible),
      mMinSize(other.mMinSize),
      mBuffer(other.mBuffer),
      mNextAllocationOffset(other.mNextAllocationOffset),
      mLastFlushOrInvalidateOffset(other.mLastFlushOrInvalidateOffset),
      mSize(other.mSize),
      mAlignment(other.mAlignment),
      mRetainedBuffers(std::move(other.mRetainedBuffers))
{
    other.mBuffer = nullptr;
}

void DynamicBuffer::init(size_t alignment, RendererVk *renderer)
{
    // Workaround for the mock ICD not supporting allocations greater than 0x1000.
    // Could be removed if https://github.com/KhronosGroup/Vulkan-Tools/issues/84 is fixed.
    if (renderer->isMockICDEnabled())
    {
        mMinSize = std::min<size_t>(mMinSize, 0x1000);
    }

    ASSERT(alignment > 0);
    mAlignment = std::max(
        alignment,
        static_cast<size_t>(renderer->getPhysicalDeviceProperties().limits.nonCoherentAtomSize));
}

DynamicBuffer::~DynamicBuffer()
{
    ASSERT(mBuffer == nullptr);
}

angle::Result DynamicBuffer::allocate(Context *context,
                                      size_t sizeInBytes,
                                      uint8_t **ptrOut,
                                      VkBuffer *bufferOut,
                                      VkDeviceSize *offsetOut,
                                      bool *newBufferAllocatedOut)
{
    size_t sizeToAllocate = roundUp(sizeInBytes, mAlignment);

    angle::base::CheckedNumeric<size_t> checkedNextWriteOffset = mNextAllocationOffset;
    checkedNextWriteOffset += sizeToAllocate;

    if (!checkedNextWriteOffset.IsValid() || checkedNextWriteOffset.ValueOrDie() >= mSize)
    {
        if (mBuffer)
        {
            ANGLE_TRY(flush(context));
            mBuffer->unmap(context->getDevice());

            mRetainedBuffers.push_back(mBuffer);
            mBuffer = nullptr;
        }

        mSize = std::max(sizeToAllocate, mMinSize);

        std::unique_ptr<BufferHelper> buffer = std::make_unique<BufferHelper>();

        VkBufferCreateInfo createInfo    = {};
        createInfo.sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        createInfo.flags                 = 0;
        createInfo.size                  = mSize;
        createInfo.usage                 = mUsage;
        createInfo.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.queueFamilyIndexCount = 0;
        createInfo.pQueueFamilyIndices   = nullptr;

        const VkMemoryPropertyFlags memoryProperty = mHostVisible
                                                         ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                                         : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        ANGLE_TRY(buffer->init(context, createInfo, memoryProperty));
        mBuffer = buffer.release();

        mNextAllocationOffset        = 0;
        mLastFlushOrInvalidateOffset = 0;

        if (newBufferAllocatedOut != nullptr)
        {
            *newBufferAllocatedOut = true;
        }
    }
    else if (newBufferAllocatedOut != nullptr)
    {
        *newBufferAllocatedOut = false;
    }

    ASSERT(mBuffer != nullptr);

    if (bufferOut != nullptr)
    {
        *bufferOut = mBuffer->getBuffer().getHandle();
    }

    // Optionally map() the buffer if possible
    if (ptrOut)
    {
        ASSERT(mHostVisible);
        uint8_t *mappedMemory;
        ANGLE_TRY(mBuffer->map(context, &mappedMemory));
        *ptrOut = mappedMemory + mNextAllocationOffset;
    }

    *offsetOut = static_cast<VkDeviceSize>(mNextAllocationOffset);
    mNextAllocationOffset += static_cast<uint32_t>(sizeToAllocate);
    return angle::Result::Continue;
}

angle::Result DynamicBuffer::flush(Context *context)
{
    if (mHostVisible && (mNextAllocationOffset > mLastFlushOrInvalidateOffset))
    {
        ASSERT(mBuffer != nullptr);
        ANGLE_TRY(mBuffer->flush(context, mLastFlushOrInvalidateOffset,
                                 mNextAllocationOffset - mLastFlushOrInvalidateOffset));
        mLastFlushOrInvalidateOffset = mNextAllocationOffset;
    }
    return angle::Result::Continue;
}

angle::Result DynamicBuffer::invalidate(Context *context)
{
    if (mHostVisible && (mNextAllocationOffset > mLastFlushOrInvalidateOffset))
    {
        ASSERT(mBuffer != nullptr);
        ANGLE_TRY(mBuffer->invalidate(context, mLastFlushOrInvalidateOffset,
                                      mNextAllocationOffset - mLastFlushOrInvalidateOffset));
        mLastFlushOrInvalidateOffset = mNextAllocationOffset;
    }
    return angle::Result::Continue;
}

void DynamicBuffer::release(RendererVk *renderer)
{
    reset();
    releaseRetainedBuffers(renderer);

    if (mBuffer)
    {
        mBuffer->unmap(renderer->getDevice());

        // The buffers may not have been recording commands, but they could be used to store data so
        // they should live until at most this frame.  For example a vertex buffer filled entirely
        // by the CPU currently never gets a chance to have its serial set.
        mBuffer->updateQueueSerial(renderer->getCurrentQueueSerial());
        mBuffer->release(renderer);
        delete mBuffer;
        mBuffer = nullptr;
    }
}

void DynamicBuffer::releaseRetainedBuffers(RendererVk *renderer)
{
    for (BufferHelper *toFree : mRetainedBuffers)
    {
        // See note in release().
        toFree->updateQueueSerial(renderer->getCurrentQueueSerial());
        toFree->release(renderer);
        delete toFree;
    }

    mRetainedBuffers.clear();
}

void DynamicBuffer::destroy(VkDevice device)
{
    reset();

    for (BufferHelper *toFree : mRetainedBuffers)
    {
        toFree->destroy(device);
        delete toFree;
    }

    mRetainedBuffers.clear();

    if (mBuffer)
    {
        mBuffer->unmap(device);
        mBuffer->destroy(device);
        delete mBuffer;
        mBuffer = nullptr;
    }
}

void DynamicBuffer::setMinimumSizeForTesting(size_t minSize)
{
    // This will really only have an effect next time we call allocate.
    mMinSize = minSize;

    // Forces a new allocation on the next allocate.
    mSize = 0;
}

void DynamicBuffer::reset()
{
    mSize                        = 0;
    mNextAllocationOffset        = 0;
    mLastFlushOrInvalidateOffset = 0;
}

// DescriptorPoolHelper implementation.
DescriptorPoolHelper::DescriptorPoolHelper() : mFreeDescriptorSets(0) {}

DescriptorPoolHelper::~DescriptorPoolHelper() = default;

bool DescriptorPoolHelper::hasCapacity(uint32_t descriptorSetCount) const
{
    return mFreeDescriptorSets >= descriptorSetCount;
}

angle::Result DescriptorPoolHelper::init(Context *context,
                                         const std::vector<VkDescriptorPoolSize> &poolSizes,
                                         uint32_t maxSets)
{
    if (mDescriptorPool.valid())
    {
        // This could be improved by recycling the descriptor pool.
        mDescriptorPool.destroy(context->getDevice());
    }

    VkDescriptorPoolCreateInfo descriptorPoolInfo = {};
    descriptorPoolInfo.sType                      = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolInfo.flags                      = 0;
    descriptorPoolInfo.maxSets                    = maxSets;
    descriptorPoolInfo.poolSizeCount              = static_cast<uint32_t>(poolSizes.size());
    descriptorPoolInfo.pPoolSizes                 = poolSizes.data();

    mFreeDescriptorSets = maxSets;

    ANGLE_VK_TRY(context, mDescriptorPool.init(context->getDevice(), descriptorPoolInfo));
    return angle::Result::Continue;
}

void DescriptorPoolHelper::destroy(VkDevice device)
{
    mDescriptorPool.destroy(device);
}

angle::Result DescriptorPoolHelper::allocateSets(Context *context,
                                                 const VkDescriptorSetLayout *descriptorSetLayout,
                                                 uint32_t descriptorSetCount,
                                                 VkDescriptorSet *descriptorSetsOut)
{
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType                       = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool              = mDescriptorPool.getHandle();
    allocInfo.descriptorSetCount          = descriptorSetCount;
    allocInfo.pSetLayouts                 = descriptorSetLayout;

    ASSERT(mFreeDescriptorSets >= descriptorSetCount);
    mFreeDescriptorSets -= descriptorSetCount;

    ANGLE_VK_TRY(context, mDescriptorPool.allocateDescriptorSets(context->getDevice(), allocInfo,
                                                                 descriptorSetsOut));
    return angle::Result::Continue;
}

// DynamicDescriptorPool implementation.
DynamicDescriptorPool::DynamicDescriptorPool()
    : mMaxSetsPerPool(kDefaultDescriptorPoolMaxSets), mCurrentPoolIndex(0)
{}

DynamicDescriptorPool::~DynamicDescriptorPool() = default;

angle::Result DynamicDescriptorPool::init(Context *context,
                                          const VkDescriptorPoolSize *setSizes,
                                          uint32_t setSizeCount)
{
    ASSERT(mCurrentPoolIndex == 0);
    ASSERT(mDescriptorPools.empty() || (mDescriptorPools.size() == 1 &&
                                        mDescriptorPools[0]->get().hasCapacity(mMaxSetsPerPool)));

    mPoolSizes.assign(setSizes, setSizes + setSizeCount);
    for (uint32_t i = 0; i < setSizeCount; ++i)
    {
        mPoolSizes[i].descriptorCount *= mMaxSetsPerPool;
    }

    mDescriptorPools.push_back(new SharedDescriptorPoolHelper());
    return mDescriptorPools[0]->get().init(context, mPoolSizes, mMaxSetsPerPool);
}

void DynamicDescriptorPool::destroy(VkDevice device)
{
    for (SharedDescriptorPoolHelper *pool : mDescriptorPools)
    {
        ASSERT(!pool->isReferenced());
        pool->get().destroy(device);
        delete pool;
    }

    mDescriptorPools.clear();
}

angle::Result DynamicDescriptorPool::allocateSets(Context *context,
                                                  const VkDescriptorSetLayout *descriptorSetLayout,
                                                  uint32_t descriptorSetCount,
                                                  SharedDescriptorPoolBinding *bindingOut,
                                                  VkDescriptorSet *descriptorSetsOut)
{
    if (!bindingOut->valid() || !bindingOut->get().hasCapacity(descriptorSetCount))
    {
        if (!mDescriptorPools[mCurrentPoolIndex]->get().hasCapacity(descriptorSetCount))
        {
            ANGLE_TRY(allocateNewPool(context));
        }

        // Make sure the old binding knows the descriptor sets can still be in-use. We only need
        // to update the serial when we move to a new pool. This is because we only check serials
        // when we move to a new pool.
        if (bindingOut->valid())
        {
            Serial currentSerial = context->getRenderer()->getCurrentQueueSerial();
            bindingOut->get().updateSerial(currentSerial);
        }

        bindingOut->set(mDescriptorPools[mCurrentPoolIndex]);
    }

    return bindingOut->get().allocateSets(context, descriptorSetLayout, descriptorSetCount,
                                          descriptorSetsOut);
}

angle::Result DynamicDescriptorPool::allocateNewPool(Context *context)
{
    RendererVk *renderer = context->getRenderer();

    bool found = false;

    for (size_t poolIndex = 0; poolIndex < mDescriptorPools.size(); ++poolIndex)
    {
        if (!mDescriptorPools[poolIndex]->isReferenced() &&
            !renderer->isSerialInUse(mDescriptorPools[poolIndex]->get().getSerial()))
        {
            mCurrentPoolIndex = poolIndex;
            found             = true;
            break;
        }
    }

    if (!found)
    {
        mDescriptorPools.push_back(new SharedDescriptorPoolHelper());
        mCurrentPoolIndex = mDescriptorPools.size() - 1;

        static constexpr size_t kMaxPools = 99999;
        ANGLE_VK_CHECK(context, mDescriptorPools.size() < kMaxPools, VK_ERROR_TOO_MANY_OBJECTS);
    }

    return mDescriptorPools[mCurrentPoolIndex]->get().init(context, mPoolSizes, mMaxSetsPerPool);
}

void DynamicDescriptorPool::setMaxSetsPerPoolForTesting(uint32_t maxSetsPerPool)
{
    mMaxSetsPerPool = maxSetsPerPool;
}

// DynamicallyGrowingPool implementation
template <typename Pool>
DynamicallyGrowingPool<Pool>::DynamicallyGrowingPool()
    : mPoolSize(0), mCurrentPool(0), mCurrentFreeEntry(0)
{}

template <typename Pool>
DynamicallyGrowingPool<Pool>::~DynamicallyGrowingPool() = default;

template <typename Pool>
angle::Result DynamicallyGrowingPool<Pool>::initEntryPool(Context *context, uint32_t poolSize)
{
    ASSERT(mPools.empty() && mPoolStats.empty());
    mPoolSize = poolSize;
    return angle::Result::Continue;
}

template <typename Pool>
void DynamicallyGrowingPool<Pool>::destroyEntryPool()
{
    mPools.clear();
    mPoolStats.clear();
}

template <typename Pool>
bool DynamicallyGrowingPool<Pool>::findFreeEntryPool(Context *context)
{
    Serial lastCompletedQueueSerial = context->getRenderer()->getLastCompletedQueueSerial();
    for (size_t i = 0; i < mPools.size(); ++i)
    {
        if (mPoolStats[i].freedCount == mPoolSize &&
            mPoolStats[i].serial <= lastCompletedQueueSerial)
        {
            mCurrentPool      = i;
            mCurrentFreeEntry = 0;

            mPoolStats[i].freedCount = 0;

            return true;
        }
    }

    return false;
}

template <typename Pool>
angle::Result DynamicallyGrowingPool<Pool>::allocateNewEntryPool(Context *context, Pool &&pool)
{
    mPools.push_back(std::move(pool));

    PoolStats poolStats = {0, Serial()};
    mPoolStats.push_back(poolStats);

    mCurrentPool      = mPools.size() - 1;
    mCurrentFreeEntry = 0;

    return angle::Result::Continue;
}

template <typename Pool>
void DynamicallyGrowingPool<Pool>::onEntryFreed(Context *context, size_t poolIndex)
{
    ASSERT(poolIndex < mPoolStats.size() && mPoolStats[poolIndex].freedCount < mPoolSize);

    // Take note of the current serial to avoid reallocating a query in the same pool
    mPoolStats[poolIndex].serial = context->getRenderer()->getCurrentQueueSerial();
    ++mPoolStats[poolIndex].freedCount;
}

// DynamicQueryPool implementation
DynamicQueryPool::DynamicQueryPool() = default;

DynamicQueryPool::~DynamicQueryPool() = default;

angle::Result DynamicQueryPool::init(Context *context, VkQueryType type, uint32_t poolSize)
{
    ANGLE_TRY(initEntryPool(context, poolSize));

    mQueryType = type;
    ANGLE_TRY(allocateNewPool(context));

    return angle::Result::Continue;
}

void DynamicQueryPool::destroy(VkDevice device)
{
    for (QueryPool &queryPool : mPools)
    {
        queryPool.destroy(device);
    }

    destroyEntryPool();
}

angle::Result DynamicQueryPool::allocateQuery(Context *context, QueryHelper *queryOut)
{
    ASSERT(!queryOut->getQueryPool());

    size_t poolIndex    = 0;
    uint32_t queryIndex = 0;
    ANGLE_TRY(allocateQuery(context, &poolIndex, &queryIndex));

    queryOut->init(this, poolIndex, queryIndex);

    return angle::Result::Continue;
}

void DynamicQueryPool::freeQuery(Context *context, QueryHelper *query)
{
    if (query->getQueryPool())
    {
        size_t poolIndex = query->getQueryPoolIndex();
        ASSERT(query->getQueryPool()->valid());

        freeQuery(context, poolIndex, query->getQuery());

        query->deinit();
    }
}

angle::Result DynamicQueryPool::allocateQuery(Context *context,
                                              size_t *poolIndex,
                                              uint32_t *queryIndex)
{
    if (mCurrentFreeEntry >= mPoolSize)
    {
        // No more queries left in this pool, create another one.
        ANGLE_TRY(allocateNewPool(context));
    }

    *poolIndex  = mCurrentPool;
    *queryIndex = mCurrentFreeEntry++;

    return angle::Result::Continue;
}

void DynamicQueryPool::freeQuery(Context *context, size_t poolIndex, uint32_t queryIndex)
{
    ANGLE_UNUSED_VARIABLE(queryIndex);
    onEntryFreed(context, poolIndex);
}

angle::Result DynamicQueryPool::allocateNewPool(Context *context)
{
    if (findFreeEntryPool(context))
    {
        return angle::Result::Continue;
    }

    VkQueryPoolCreateInfo queryPoolInfo = {};
    queryPoolInfo.sType                 = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPoolInfo.flags                 = 0;
    queryPoolInfo.queryType             = mQueryType;
    queryPoolInfo.queryCount            = mPoolSize;
    queryPoolInfo.pipelineStatistics    = 0;

    vk::QueryPool queryPool;

    ANGLE_VK_TRY(context, queryPool.init(context->getDevice(), queryPoolInfo));

    return allocateNewEntryPool(context, std::move(queryPool));
}

// QueryHelper implementation
QueryHelper::QueryHelper() : mDynamicQueryPool(nullptr), mQueryPoolIndex(0), mQuery(0) {}

QueryHelper::~QueryHelper() {}

void QueryHelper::init(const DynamicQueryPool *dynamicQueryPool,
                       const size_t queryPoolIndex,
                       uint32_t query)
{
    mDynamicQueryPool = dynamicQueryPool;
    mQueryPoolIndex   = queryPoolIndex;
    mQuery            = query;
}

void QueryHelper::deinit()
{
    mDynamicQueryPool = nullptr;
    mQueryPoolIndex   = 0;
    mQuery            = 0;
}

void QueryHelper::beginQuery(vk::Context *context)
{
    RendererVk *renderer = context->getRenderer();
    renderer->getCommandGraph()->beginQuery(getQueryPool(), getQuery());
    mMostRecentSerial = renderer->getCurrentQueueSerial();
}

void QueryHelper::endQuery(vk::Context *context)
{
    RendererVk *renderer = context->getRenderer();
    renderer->getCommandGraph()->endQuery(getQueryPool(), getQuery());
    mMostRecentSerial = renderer->getCurrentQueueSerial();
}

void QueryHelper::writeTimestamp(vk::Context *context)
{
    RendererVk *renderer = context->getRenderer();
    renderer->getCommandGraph()->writeTimestamp(getQueryPool(), getQuery());
    mMostRecentSerial = renderer->getCurrentQueueSerial();
}

bool QueryHelper::hasPendingWork(RendererVk *renderer)
{
    // If the renderer has a queue serial higher than the stored one, the command buffers that
    // recorded this query have already been submitted, so there is no pending work.
    return mMostRecentSerial == renderer->getCurrentQueueSerial();
}

// DynamicSemaphorePool implementation
DynamicSemaphorePool::DynamicSemaphorePool() = default;

DynamicSemaphorePool::~DynamicSemaphorePool() = default;

angle::Result DynamicSemaphorePool::init(Context *context, uint32_t poolSize)
{
    ANGLE_TRY(initEntryPool(context, poolSize));
    ANGLE_TRY(allocateNewPool(context));
    return angle::Result::Continue;
}

void DynamicSemaphorePool::destroy(VkDevice device)
{
    for (auto &semaphorePool : mPools)
    {
        for (Semaphore &semaphore : semaphorePool)
        {
            semaphore.destroy(device);
        }
    }

    destroyEntryPool();
}

angle::Result DynamicSemaphorePool::allocateSemaphore(Context *context,
                                                      SemaphoreHelper *semaphoreOut)
{
    ASSERT(!semaphoreOut->getSemaphore());

    if (mCurrentFreeEntry >= mPoolSize)
    {
        // No more queries left in this pool, create another one.
        ANGLE_TRY(allocateNewPool(context));
    }

    semaphoreOut->init(mCurrentPool, &mPools[mCurrentPool][mCurrentFreeEntry++]);

    return angle::Result::Continue;
}

void DynamicSemaphorePool::freeSemaphore(Context *context, SemaphoreHelper *semaphore)
{
    if (semaphore->getSemaphore())
    {
        onEntryFreed(context, semaphore->getSemaphorePoolIndex());
        semaphore->deinit();
    }
}

angle::Result DynamicSemaphorePool::allocateNewPool(Context *context)
{
    if (findFreeEntryPool(context))
    {
        return angle::Result::Continue;
    }

    std::vector<Semaphore> newPool(mPoolSize);

    for (Semaphore &semaphore : newPool)
    {
        ANGLE_VK_TRY(context, semaphore.init(context->getDevice()));
    }

    // This code is safe as long as the growth of the outer vector in vector<vector<T>> is done by
    // moving the inner vectors, making sure references to the inner vector remain intact.
    Semaphore *assertMove = mPools.size() > 0 ? mPools[0].data() : nullptr;

    ANGLE_TRY(allocateNewEntryPool(context, std::move(newPool)));

    ASSERT(assertMove == nullptr || assertMove == mPools[0].data());

    return angle::Result::Continue;
}

// SemaphoreHelper implementation
SemaphoreHelper::SemaphoreHelper() : mSemaphorePoolIndex(0), mSemaphore(0) {}

SemaphoreHelper::~SemaphoreHelper() {}

SemaphoreHelper::SemaphoreHelper(SemaphoreHelper &&other)
    : mSemaphorePoolIndex(other.mSemaphorePoolIndex), mSemaphore(other.mSemaphore)
{
    other.mSemaphore = nullptr;
}

SemaphoreHelper &SemaphoreHelper::operator=(SemaphoreHelper &&other)
{
    std::swap(mSemaphorePoolIndex, other.mSemaphorePoolIndex);
    std::swap(mSemaphore, other.mSemaphore);
    return *this;
}

void SemaphoreHelper::init(const size_t semaphorePoolIndex, const vk::Semaphore *semaphore)
{
    mSemaphorePoolIndex = semaphorePoolIndex;
    mSemaphore          = semaphore;
}

void SemaphoreHelper::deinit()
{
    mSemaphorePoolIndex = 0;
    mSemaphore          = nullptr;
}

// LineLoopHelper implementation.
LineLoopHelper::LineLoopHelper(RendererVk *renderer)
    : mDynamicIndexBuffer(kLineLoopDynamicBufferUsage, kLineLoopDynamicBufferMinSize, true)
{
    // We need to use an alignment of the maximum size we're going to allocate, which is
    // VK_INDEX_TYPE_UINT32. When we switch from a drawElement to a drawArray call, the allocations
    // can vary in size. According to the Vulkan spec, when calling vkCmdBindIndexBuffer: 'The
    // sum of offset and the address of the range of VkDeviceMemory object that is backing buffer,
    // must be a multiple of the type indicated by indexType'.
    mDynamicIndexBuffer.init(sizeof(uint32_t), renderer);
}

LineLoopHelper::~LineLoopHelper() = default;

angle::Result LineLoopHelper::getIndexBufferForDrawArrays(ContextVk *contextVk,
                                                          uint32_t clampedVertexCount,
                                                          GLint firstVertex,
                                                          vk::BufferHelper **bufferOut,
                                                          VkDeviceSize *offsetOut)
{
    uint32_t *indices    = nullptr;
    size_t allocateBytes = sizeof(uint32_t) * (static_cast<size_t>(clampedVertexCount) + 1);

    mDynamicIndexBuffer.releaseRetainedBuffers(contextVk->getRenderer());
    ANGLE_TRY(mDynamicIndexBuffer.allocate(contextVk, allocateBytes,
                                           reinterpret_cast<uint8_t **>(&indices), nullptr,
                                           offsetOut, nullptr));
    *bufferOut = mDynamicIndexBuffer.getCurrentBuffer();

    // Note: there could be an overflow in this addition.
    uint32_t unsignedFirstVertex = static_cast<uint32_t>(firstVertex);
    uint32_t vertexCount         = (clampedVertexCount + unsignedFirstVertex);
    for (uint32_t vertexIndex = unsignedFirstVertex; vertexIndex < vertexCount; vertexIndex++)
    {
        *indices++ = vertexIndex;
    }
    *indices = unsignedFirstVertex;

    // Since we are not using the VK_MEMORY_PROPERTY_HOST_COHERENT_BIT flag when creating the
    // device memory in the StreamingBuffer, we always need to make sure we flush it after
    // writing.
    ANGLE_TRY(mDynamicIndexBuffer.flush(contextVk));

    return angle::Result::Continue;
}

angle::Result LineLoopHelper::getIndexBufferForElementArrayBuffer(ContextVk *contextVk,
                                                                  BufferVk *elementArrayBufferVk,
                                                                  gl::DrawElementsType glIndexType,
                                                                  int indexCount,
                                                                  intptr_t elementArrayOffset,
                                                                  vk::BufferHelper **bufferOut,
                                                                  VkDeviceSize *bufferOffsetOut)
{
    if (glIndexType == gl::DrawElementsType::UnsignedByte)
    {
        TRACE_EVENT0("gpu.angle", "LineLoopHelper::getIndexBufferForElementArrayBuffer");
        // Needed before reading buffer or we could get stale data.
        ANGLE_TRY(contextVk->getRenderer()->finish(contextVk));

        void *srcDataMapping = nullptr;
        ANGLE_TRY(elementArrayBufferVk->mapImpl(contextVk, &srcDataMapping));
        ANGLE_TRY(streamIndices(contextVk, glIndexType, indexCount,
                                static_cast<const uint8_t *>(srcDataMapping) + elementArrayOffset,
                                bufferOut, bufferOffsetOut));
        ANGLE_TRY(elementArrayBufferVk->unmapImpl(contextVk));
        return angle::Result::Continue;
    }

    VkIndexType indexType = gl_vk::kIndexTypeMap[glIndexType];
    ASSERT(indexType == VK_INDEX_TYPE_UINT16 || indexType == VK_INDEX_TYPE_UINT32);
    uint32_t *indices = nullptr;

    auto unitSize = (indexType == VK_INDEX_TYPE_UINT16 ? sizeof(uint16_t) : sizeof(uint32_t));
    size_t allocateBytes = unitSize * (indexCount + 1) + 1;

    mDynamicIndexBuffer.releaseRetainedBuffers(contextVk->getRenderer());
    ANGLE_TRY(mDynamicIndexBuffer.allocate(contextVk, allocateBytes,
                                           reinterpret_cast<uint8_t **>(&indices), nullptr,
                                           bufferOffsetOut, nullptr));
    *bufferOut = mDynamicIndexBuffer.getCurrentBuffer();

    VkDeviceSize sourceOffset                  = static_cast<VkDeviceSize>(elementArrayOffset);
    uint64_t unitCount                         = static_cast<VkDeviceSize>(indexCount);
    angle::FixedVector<VkBufferCopy, 3> copies = {
        {sourceOffset, *bufferOffsetOut, unitCount * unitSize},
        {sourceOffset, *bufferOffsetOut + unitCount * unitSize, unitSize},
    };
    if (contextVk->getRenderer()->getFeatures().extraCopyBufferRegion)
        copies.push_back({sourceOffset, *bufferOffsetOut + (unitCount + 1) * unitSize, 1});

    ANGLE_TRY(
        elementArrayBufferVk->copyToBuffer(contextVk, *bufferOut, copies.size(), copies.data()));
    ANGLE_TRY(mDynamicIndexBuffer.flush(contextVk));
    return angle::Result::Continue;
}

angle::Result LineLoopHelper::streamIndices(ContextVk *contextVk,
                                            gl::DrawElementsType glIndexType,
                                            GLsizei indexCount,
                                            const uint8_t *srcPtr,
                                            vk::BufferHelper **bufferOut,
                                            VkDeviceSize *bufferOffsetOut)
{
    VkIndexType indexType = gl_vk::kIndexTypeMap[glIndexType];

    uint8_t *indices = nullptr;

    auto unitSize = (indexType == VK_INDEX_TYPE_UINT16 ? sizeof(uint16_t) : sizeof(uint32_t));
    size_t allocateBytes = unitSize * (indexCount + 1);
    ANGLE_TRY(mDynamicIndexBuffer.allocate(contextVk, allocateBytes,
                                           reinterpret_cast<uint8_t **>(&indices), nullptr,
                                           bufferOffsetOut, nullptr));
    *bufferOut = mDynamicIndexBuffer.getCurrentBuffer();

    if (glIndexType == gl::DrawElementsType::UnsignedByte)
    {
        // Vulkan doesn't support uint8 index types, so we need to emulate it.
        ASSERT(indexType == VK_INDEX_TYPE_UINT16);
        uint16_t *indicesDst = reinterpret_cast<uint16_t *>(indices);
        for (int i = 0; i < indexCount; i++)
        {
            indicesDst[i] = srcPtr[i];
        }

        indicesDst[indexCount] = srcPtr[0];
    }
    else
    {
        memcpy(indices, srcPtr, unitSize * indexCount);
        memcpy(indices + unitSize * indexCount, srcPtr, unitSize);
    }

    ANGLE_TRY(mDynamicIndexBuffer.flush(contextVk));
    return angle::Result::Continue;
}

void LineLoopHelper::release(RendererVk *renderer)
{
    mDynamicIndexBuffer.release(renderer);
}

void LineLoopHelper::destroy(VkDevice device)
{
    mDynamicIndexBuffer.destroy(device);
}

// static
void LineLoopHelper::Draw(uint32_t count, CommandBuffer *commandBuffer)
{
    // Our first index is always 0 because that's how we set it up in createIndexBuffer*.
    // Note: this could theoretically overflow and wrap to zero.
    commandBuffer->drawIndexed(count + 1, 1, 0, 0, 0);
}

// BufferHelper implementation.
BufferHelper::BufferHelper()
    : CommandGraphResource(CommandGraphResourceType::Buffer),
      mMemoryPropertyFlags{},
      mSize(0),
      mMappedMemory(nullptr),
      mViewFormat(nullptr),
      mCurrentWriteAccess(0),
      mCurrentReadAccess(0)
{}

BufferHelper::~BufferHelper() = default;

angle::Result BufferHelper::init(Context *context,
                                 const VkBufferCreateInfo &createInfo,
                                 VkMemoryPropertyFlags memoryPropertyFlags)
{
    mSize = createInfo.size;
    ANGLE_VK_TRY(context, mBuffer.init(context->getDevice(), createInfo));
    return vk::AllocateBufferMemory(context, memoryPropertyFlags, &mMemoryPropertyFlags, &mBuffer,
                                    &mDeviceMemory);
}

void BufferHelper::destroy(VkDevice device)
{
    unmap(device);
    mSize       = 0;
    mViewFormat = nullptr;

    mBuffer.destroy(device);
    mBufferView.destroy(device);
    mDeviceMemory.destroy(device);
}

void BufferHelper::release(RendererVk *renderer)
{
    unmap(renderer->getDevice());
    mSize       = 0;
    mViewFormat = nullptr;

    renderer->releaseObject(getStoredQueueSerial(), &mBuffer);
    renderer->releaseObject(getStoredQueueSerial(), &mBufferView);
    renderer->releaseObject(getStoredQueueSerial(), &mDeviceMemory);
}

void BufferHelper::onWrite(VkAccessFlagBits writeAccessType)
{
    if (mCurrentReadAccess != 0 || mCurrentWriteAccess != 0)
    {
        addGlobalMemoryBarrier(mCurrentReadAccess | mCurrentWriteAccess, writeAccessType);
    }

    mCurrentWriteAccess = writeAccessType;
    mCurrentReadAccess  = 0;
}

angle::Result BufferHelper::copyFromBuffer(Context *context,
                                           const Buffer &buffer,
                                           const VkBufferCopy &copyRegion)
{
    // 'recordCommands' will implicitly stop any reads from using the old buffer data.
    vk::CommandBuffer *commandBuffer = nullptr;
    ANGLE_TRY(recordCommands(context, &commandBuffer));

    if (mCurrentReadAccess != 0 || mCurrentWriteAccess != 0)
    {
        // Insert a barrier to ensure reads/writes are complete.
        // Use a global memory barrier to keep things simple.
        VkMemoryBarrier memoryBarrier = {};
        memoryBarrier.sType           = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memoryBarrier.srcAccessMask   = mCurrentReadAccess | mCurrentWriteAccess;
        memoryBarrier.dstAccessMask   = VK_ACCESS_TRANSFER_WRITE_BIT;

        commandBuffer->pipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memoryBarrier, 0,
                                       nullptr, 0, nullptr);

        mCurrentWriteAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
        mCurrentReadAccess  = 0;
    }

    commandBuffer->copyBuffer(buffer, mBuffer, 1, &copyRegion);

    return angle::Result::Continue;
}

angle::Result BufferHelper::initBufferView(Context *context, const Format &format)
{
    ASSERT(format.valid());

    if (mBufferView.valid())
    {
        ASSERT(mViewFormat->vkBufferFormat == format.vkBufferFormat);
        return angle::Result::Continue;
    }

    VkBufferViewCreateInfo viewCreateInfo = {};
    viewCreateInfo.sType                  = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    viewCreateInfo.buffer                 = mBuffer.getHandle();
    viewCreateInfo.format                 = format.vkBufferFormat;
    viewCreateInfo.offset                 = 0;
    viewCreateInfo.range                  = mSize;

    ANGLE_VK_TRY(context, mBufferView.init(context->getDevice(), viewCreateInfo));
    mViewFormat = &format;

    return angle::Result::Continue;
}

angle::Result BufferHelper::mapImpl(Context *context)
{
    ANGLE_VK_TRY(context, mDeviceMemory.map(context->getDevice(), 0, mSize, 0, &mMappedMemory));
    return angle::Result::Continue;
}

void BufferHelper::unmap(VkDevice device)
{
    if (mMappedMemory)
    {
        mDeviceMemory.unmap(device);
        mMappedMemory = nullptr;
    }
}

angle::Result BufferHelper::flush(Context *context, size_t offset, size_t size)
{
    bool hostVisible  = mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    bool hostCoherent = mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (hostVisible && !hostCoherent)
    {
        VkMappedMemoryRange range = {};
        range.sType               = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory              = mDeviceMemory.getHandle();
        range.offset              = offset;
        range.size                = size;
        ANGLE_VK_TRY(context, vkFlushMappedMemoryRanges(context->getDevice(), 1, &range));
    }
    return angle::Result::Continue;
}

angle::Result BufferHelper::invalidate(Context *context, size_t offset, size_t size)
{
    bool hostVisible  = mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    bool hostCoherent = mMemoryPropertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (hostVisible && !hostCoherent)
    {
        VkMappedMemoryRange range = {};
        range.sType               = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory              = mDeviceMemory.getHandle();
        range.offset              = offset;
        range.size                = size;
        ANGLE_VK_TRY(context, vkInvalidateMappedMemoryRanges(context->getDevice(), 1, &range));
    }
    return angle::Result::Continue;
}

namespace
{
constexpr VkBufferUsageFlags kStagingBufferFlags =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr size_t kStagingBufferSize = 1024 * 16;

}  // anonymous namespace

// ImageHelper implementation.
ImageHelper::ImageHelper()
    : CommandGraphResource(CommandGraphResourceType::Image),
      mFormat(nullptr),
      mSamples(0),
      mCurrentLayout(ImageLayout::Undefined),
      mLayerCount(0),
      mLevelCount(0),
      mStagingBuffer(kStagingBufferFlags, kStagingBufferSize, true)
{}

ImageHelper::ImageHelper(ImageHelper &&other)
    : CommandGraphResource(CommandGraphResourceType::Image),
      mImage(std::move(other.mImage)),
      mDeviceMemory(std::move(other.mDeviceMemory)),
      mExtents(other.mExtents),
      mFormat(other.mFormat),
      mSamples(other.mSamples),
      mCurrentLayout(other.mCurrentLayout),
      mLayerCount(other.mLayerCount),
      mLevelCount(other.mLevelCount),
      mStagingBuffer(std::move(other.mStagingBuffer)),
      mSubresourceUpdates(std::move(other.mSubresourceUpdates))
{
    other.mCurrentLayout = ImageLayout::Undefined;
    other.mLayerCount    = 0;
    other.mLevelCount    = 0;
}

ImageHelper::~ImageHelper()
{
    ASSERT(!valid());
}

void ImageHelper::initStagingBuffer(RendererVk *renderer)
{
    // vkCmdCopyBufferToImage must have an offset that is a multiple of 4.
    // https://www.khronos.org/registry/vulkan/specs/1.0/man/html/VkBufferImageCopy.html
    mStagingBuffer.init(4, renderer);
}

angle::Result ImageHelper::init(Context *context,
                                gl::TextureType textureType,
                                const gl::Extents &extents,
                                const Format &format,
                                GLint samples,
                                VkImageUsageFlags usage,
                                uint32_t mipLevels,
                                uint32_t layerCount)
{
    ASSERT(!valid());

    // Validate that the input layerCount is compatible with the texture type
    ASSERT(textureType != gl::TextureType::_3D || layerCount == 1);
    ASSERT(textureType != gl::TextureType::External || layerCount == 1);
    ASSERT(textureType != gl::TextureType::Rectangle || layerCount == 1);
    ASSERT(textureType != gl::TextureType::CubeMap || layerCount == gl::kCubeFaceCount);

    mExtents    = extents;
    mFormat     = &format;
    mSamples    = samples;
    mLayerCount = layerCount;
    mLevelCount = mipLevels;

    VkImageCreateInfo imageInfo     = {};
    imageInfo.sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags                 = GetImageCreateFlags(textureType);
    imageInfo.imageType             = gl_vk::GetImageType(textureType);
    imageInfo.format                = format.vkTextureFormat;
    imageInfo.extent.width          = static_cast<uint32_t>(extents.width);
    imageInfo.extent.height         = static_cast<uint32_t>(extents.height);
    imageInfo.extent.depth          = 1;
    imageInfo.mipLevels             = mipLevels;
    imageInfo.arrayLayers           = mLayerCount;
    imageInfo.samples               = gl_vk::GetSamples(samples);
    imageInfo.tiling                = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage                 = usage;
    imageInfo.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.queueFamilyIndexCount = 0;
    imageInfo.pQueueFamilyIndices   = nullptr;
    imageInfo.initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED;

    mCurrentLayout = ImageLayout::Undefined;

    ANGLE_VK_TRY(context, mImage.init(context->getDevice(), imageInfo));

    return angle::Result::Continue;
}

void ImageHelper::releaseImage(RendererVk *renderer)
{
    renderer->releaseObject(getStoredQueueSerial(), &mImage);
    renderer->releaseObject(getStoredQueueSerial(), &mDeviceMemory);
}

void ImageHelper::releaseStagingBuffer(RendererVk *renderer)
{
    // Remove updates that never made it to the texture.
    for (SubresourceUpdate &update : mSubresourceUpdates)
    {
        update.release(renderer);
    }
    mStagingBuffer.release(renderer);
    mSubresourceUpdates.clear();
}

void ImageHelper::resetImageWeakReference()
{
    mImage.reset();
}

angle::Result ImageHelper::initMemory(Context *context,
                                      const MemoryProperties &memoryProperties,
                                      VkMemoryPropertyFlags flags)
{
    // TODO(jmadill): Memory sub-allocation. http://anglebug.com/2162
    ANGLE_TRY(AllocateImageMemory(context, flags, &mImage, &mDeviceMemory));
    return angle::Result::Continue;
}

angle::Result ImageHelper::initImageView(Context *context,
                                         gl::TextureType textureType,
                                         VkImageAspectFlags aspectMask,
                                         const gl::SwizzleState &swizzleMap,
                                         ImageView *imageViewOut,
                                         uint32_t baseMipLevel,
                                         uint32_t levelCount)
{
    return initLayerImageView(context, textureType, aspectMask, swizzleMap, imageViewOut,
                              baseMipLevel, levelCount, 0, mLayerCount);
}

angle::Result ImageHelper::initLayerImageView(Context *context,
                                              gl::TextureType textureType,
                                              VkImageAspectFlags aspectMask,
                                              const gl::SwizzleState &swizzleMap,
                                              ImageView *imageViewOut,
                                              uint32_t baseMipLevel,
                                              uint32_t levelCount,
                                              uint32_t baseArrayLayer,
                                              uint32_t layerCount)
{
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType                 = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.flags                 = 0;
    viewInfo.image                 = mImage.getHandle();
    viewInfo.viewType              = gl_vk::GetImageViewType(textureType);
    viewInfo.format                = mFormat->vkTextureFormat;
    if (swizzleMap.swizzleRequired())
    {
        viewInfo.components.r = gl_vk::GetSwizzle(swizzleMap.swizzleRed);
        viewInfo.components.g = gl_vk::GetSwizzle(swizzleMap.swizzleGreen);
        viewInfo.components.b = gl_vk::GetSwizzle(swizzleMap.swizzleBlue);
        viewInfo.components.a = gl_vk::GetSwizzle(swizzleMap.swizzleAlpha);
    }
    else
    {
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    }
    viewInfo.subresourceRange.aspectMask     = aspectMask;
    viewInfo.subresourceRange.baseMipLevel   = baseMipLevel;
    viewInfo.subresourceRange.levelCount     = levelCount;
    viewInfo.subresourceRange.baseArrayLayer = baseArrayLayer;
    viewInfo.subresourceRange.layerCount     = layerCount;

    ANGLE_VK_TRY(context, imageViewOut->init(context->getDevice(), viewInfo));
    return angle::Result::Continue;
}

void ImageHelper::destroy(VkDevice device)
{
    mImage.destroy(device);
    mDeviceMemory.destroy(device);
    mCurrentLayout = ImageLayout::Undefined;
    mLayerCount    = 0;
    mLevelCount    = 0;
}

void ImageHelper::init2DWeakReference(VkImage handle,
                                      const gl::Extents &extents,
                                      const Format &format,
                                      GLint samples)
{
    ASSERT(!valid());

    mExtents    = extents;
    mFormat     = &format;
    mSamples    = samples;
    mCurrentLayout = ImageLayout::Undefined;
    mLayerCount = 1;
    mLevelCount = 1;

    mImage.setHandle(handle);
}

angle::Result ImageHelper::init2DStaging(Context *context,
                                         const MemoryProperties &memoryProperties,
                                         const gl::Extents &extents,
                                         const Format &format,
                                         VkImageUsageFlags usage,
                                         uint32_t layerCount)
{
    ASSERT(!valid());

    mExtents    = extents;
    mFormat     = &format;
    mSamples    = 1;
    mLayerCount = layerCount;
    mLevelCount = 1;

    mCurrentLayout = ImageLayout::Undefined;

    VkImageCreateInfo imageInfo     = {};
    imageInfo.sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags                 = 0;
    imageInfo.imageType             = VK_IMAGE_TYPE_2D;
    imageInfo.format                = format.vkTextureFormat;
    imageInfo.extent.width          = static_cast<uint32_t>(extents.width);
    imageInfo.extent.height         = static_cast<uint32_t>(extents.height);
    imageInfo.extent.depth          = 1;
    imageInfo.mipLevels             = 1;
    imageInfo.arrayLayers           = mLayerCount;
    imageInfo.samples               = gl_vk::GetSamples(mSamples);
    imageInfo.tiling                = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage                 = usage;
    imageInfo.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.queueFamilyIndexCount = 0;
    imageInfo.pQueueFamilyIndices   = nullptr;
    imageInfo.initialLayout         = getCurrentLayout();

    ANGLE_VK_TRY(context, mImage.init(context->getDevice(), imageInfo));

    // Allocate and bind device-local memory.
    VkMemoryPropertyFlags memoryPropertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    ANGLE_TRY(initMemory(context, memoryProperties, memoryPropertyFlags));

    return angle::Result::Continue;
}

VkImageAspectFlags ImageHelper::getAspectFlags() const
{
    return GetFormatAspectFlags(mFormat->textureFormat());
}

void ImageHelper::dumpResources(Serial serial, std::vector<GarbageObject> *garbageQueue)
{
    mImage.dumpResources(serial, garbageQueue);
    mDeviceMemory.dumpResources(serial, garbageQueue);
}

const Image &ImageHelper::getImage() const
{
    return mImage;
}

const DeviceMemory &ImageHelper::getDeviceMemory() const
{
    return mDeviceMemory;
}

const gl::Extents &ImageHelper::getExtents() const
{
    return mExtents;
}

const Format &ImageHelper::getFormat() const
{
    return *mFormat;
}

GLint ImageHelper::getSamples() const
{
    return mSamples;
}

VkImageLayout ImageHelper::getCurrentLayout() const
{
    return kImageMemoryBarrierData[mCurrentLayout].layout;
}

bool ImageHelper::isLayoutChangeNecessary(ImageLayout newLayout)
{
    const ImageMemoryBarrierData &layoutData = kImageMemoryBarrierData[mCurrentLayout];

    // If transitioning to the same read-only layout (RAR), don't generate a barrier.
    bool sameLayoutReadAfterRead = mCurrentLayout == newLayout && layoutData.isReadOnlyAccess;

    return !sameLayoutReadAfterRead;
}

void ImageHelper::changeLayout(VkImageAspectFlags aspectMask,
                               ImageLayout newLayout,
                               CommandBuffer *commandBuffer)
{
    if (!isLayoutChangeNecessary(newLayout))
    {
        return;
    }

    const ImageMemoryBarrierData &transitionFrom = kImageMemoryBarrierData[mCurrentLayout];
    const ImageMemoryBarrierData &transitionTo   = kImageMemoryBarrierData[newLayout];

    VkImageMemoryBarrier imageMemoryBarrier = {};
    imageMemoryBarrier.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageMemoryBarrier.srcAccessMask        = transitionFrom.srcAccessMask;
    imageMemoryBarrier.dstAccessMask        = transitionTo.dstAccessMask;
    imageMemoryBarrier.oldLayout            = transitionFrom.layout;
    imageMemoryBarrier.newLayout            = transitionTo.layout;
    imageMemoryBarrier.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
    imageMemoryBarrier.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
    imageMemoryBarrier.image                = mImage.getHandle();

    // TODO(jmadill): Is this needed for mipped/layer images?
    imageMemoryBarrier.subresourceRange.aspectMask     = aspectMask;
    imageMemoryBarrier.subresourceRange.baseMipLevel   = 0;
    imageMemoryBarrier.subresourceRange.levelCount     = mLevelCount;
    imageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
    imageMemoryBarrier.subresourceRange.layerCount     = mLayerCount;

    commandBuffer->pipelineBarrier(transitionFrom.srcStageMask, transitionTo.dstStageMask, 0, 0,
                                   nullptr, 0, nullptr, 1, &imageMemoryBarrier);

    mCurrentLayout = newLayout;
}

void ImageHelper::clearColor(const VkClearColorValue &color,
                             uint32_t baseMipLevel,
                             uint32_t levelCount,
                             CommandBuffer *commandBuffer)

{
    clearColorLayer(color, baseMipLevel, levelCount, 0, mLayerCount, commandBuffer);
}

void ImageHelper::clearColorLayer(const VkClearColorValue &color,
                                  uint32_t baseMipLevel,
                                  uint32_t levelCount,
                                  uint32_t baseArrayLayer,
                                  uint32_t layerCount,
                                  CommandBuffer *commandBuffer)
{
    ASSERT(valid());

    changeLayout(VK_IMAGE_ASPECT_COLOR_BIT, ImageLayout::TransferDst, commandBuffer);

    VkImageSubresourceRange range = {};
    range.aspectMask              = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel            = baseMipLevel;
    range.levelCount              = levelCount;
    range.baseArrayLayer          = baseArrayLayer;
    range.layerCount              = layerCount;

    commandBuffer->clearColorImage(mImage, getCurrentLayout(), color, 1, &range);
}

void ImageHelper::clearDepthStencil(VkImageAspectFlags imageAspectFlags,
                                    VkImageAspectFlags clearAspectFlags,
                                    const VkClearDepthStencilValue &depthStencil,
                                    CommandBuffer *commandBuffer)
{
    ASSERT(valid());

    changeLayout(imageAspectFlags, ImageLayout::TransferDst, commandBuffer);

    VkImageSubresourceRange clearRange = {
        /*aspectMask*/ clearAspectFlags,
        /*baseMipLevel*/ 0,
        /*levelCount*/ 1,
        /*baseArrayLayer*/ 0,
        /*layerCount*/ 1,
    };

    commandBuffer->clearDepthStencilImage(mImage, getCurrentLayout(), depthStencil, 1, &clearRange);
}

gl::Extents ImageHelper::getSize(const gl::ImageIndex &index) const
{
    ASSERT(mExtents.depth == 1);
    GLint mipLevel = index.getLevelIndex();
    // Level 0 should be the size of the extents, after that every time you increase a level
    // you shrink the extents by half.
    return gl::Extents(std::max(1, mExtents.width >> mipLevel),
                       std::max(1, mExtents.height >> mipLevel), mExtents.depth);
}

// static
void ImageHelper::Copy(ImageHelper *srcImage,
                       ImageHelper *dstImage,
                       const gl::Offset &srcOffset,
                       const gl::Offset &dstOffset,
                       const gl::Extents &copySize,
                       VkImageAspectFlags aspectMask,
                       CommandBuffer *commandBuffer)
{
    ASSERT(commandBuffer->valid() && srcImage->valid() && dstImage->valid());

    srcImage->changeLayout(srcImage->getAspectFlags(), ImageLayout::TransferSrc, commandBuffer);
    dstImage->changeLayout(dstImage->getAspectFlags(), ImageLayout::TransferDst, commandBuffer);

    VkImageCopy region                   = {};
    region.srcSubresource.aspectMask     = aspectMask;
    region.srcSubresource.mipLevel       = 0;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount     = 1;
    region.srcOffset.x                   = srcOffset.x;
    region.srcOffset.y                   = srcOffset.y;
    region.srcOffset.z                   = srcOffset.z;
    region.dstSubresource.aspectMask     = aspectMask;
    region.dstSubresource.mipLevel       = 0;
    region.dstSubresource.baseArrayLayer = 0;
    region.dstSubresource.layerCount     = 1;
    region.dstOffset.x                   = dstOffset.x;
    region.dstOffset.y                   = dstOffset.y;
    region.dstOffset.z                   = dstOffset.z;
    region.extent.width                  = copySize.width;
    region.extent.height                 = copySize.height;
    region.extent.depth                  = copySize.depth;

    commandBuffer->copyImage(srcImage->getImage(), srcImage->getCurrentLayout(),
                             dstImage->getImage(), dstImage->getCurrentLayout(), 1, &region);
}

angle::Result ImageHelper::generateMipmapsWithBlit(ContextVk *contextVk, GLuint maxLevel)
{
    vk::CommandBuffer *commandBuffer = nullptr;
    ANGLE_TRY(recordCommands(contextVk, &commandBuffer));

    changeLayout(VK_IMAGE_ASPECT_COLOR_BIT, ImageLayout::TransferDst, commandBuffer);

    // We are able to use blitImage since the image format we are using supports it. This
    // is a faster way we can generate the mips.
    int32_t mipWidth  = mExtents.width;
    int32_t mipHeight = mExtents.height;

    // Manually manage the image memory barrier because it uses a lot more parameters than our
    // usual one.
    VkImageMemoryBarrier barrier            = {};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image                           = mImage.getHandle();
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = mLayerCount;
    barrier.subresourceRange.levelCount     = 1;

    for (uint32_t mipLevel = 1; mipLevel <= maxLevel; mipLevel++)
    {
        int32_t nextMipWidth  = std::max<int32_t>(1, mipWidth >> 1);
        int32_t nextMipHeight = std::max<int32_t>(1, mipHeight >> 1);

        barrier.subresourceRange.baseMipLevel = mipLevel - 1;
        barrier.oldLayout                     = getCurrentLayout();
        barrier.newLayout                     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask                 = VK_ACCESS_TRANSFER_READ_BIT;

        // We can do it for all layers at once.
        commandBuffer->pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                       &barrier);

        VkImageBlit blit                   = {};
        blit.srcOffsets[0]                 = {0, 0, 0};
        blit.srcOffsets[1]                 = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel       = mipLevel - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount     = mLayerCount;
        blit.dstOffsets[0]                 = {0, 0, 0};
        blit.dstOffsets[1]                 = {nextMipWidth, nextMipHeight, 1};
        blit.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel       = mipLevel;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount     = mLayerCount;

        mipWidth  = nextMipWidth;
        mipHeight = nextMipHeight;

        commandBuffer->blitImage(mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mImage,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }

    // Transition the last mip level to the same layout as all the other ones, so we can declare
    // our whole image layout to be SRC_OPTIMAL.
    barrier.subresourceRange.baseMipLevel = maxLevel;
    barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout                     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // We can do it for all layers at once.
    commandBuffer->pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, 0, nullptr, 0, nullptr, 1, &barrier);

    // This is just changing the internal state of the image helper so that the next call
    // to changeLayout will use this layout as the "oldLayout" argument.
    mCurrentLayout = ImageLayout::TransferSrc;

    return angle::Result::Continue;
}

void ImageHelper::removeStagedUpdates(RendererVk *renderer, const gl::ImageIndex &index)
{
    // Find any staged updates for this index and removes them from the pending list.
    uint32_t levelIndex = index.getLevelIndex();
    uint32_t layerIndex = index.hasLayer() ? index.getLayerIndex() : 0;

    for (size_t index = 0; index < mSubresourceUpdates.size();)
    {
        auto update = mSubresourceUpdates.begin() + index;
        if (update->isUpdateToLayerLevel(layerIndex, levelIndex))
        {
            update->release(renderer);
            mSubresourceUpdates.erase(update);
        }
        else
        {
            index++;
        }
    }
}

angle::Result ImageHelper::stageSubresourceUpdate(ContextVk *contextVk,
                                                  const gl::ImageIndex &index,
                                                  const gl::Extents &extents,
                                                  const gl::Offset &offset,
                                                  const gl::InternalFormat &formatInfo,
                                                  const gl::PixelUnpackState &unpack,
                                                  GLenum type,
                                                  const uint8_t *pixels)
{
    GLuint inputRowPitch = 0;
    ANGLE_VK_CHECK_MATH(contextVk, formatInfo.computeRowPitch(type, extents.width, unpack.alignment,
                                                              unpack.rowLength, &inputRowPitch));

    GLuint inputDepthPitch = 0;
    ANGLE_VK_CHECK_MATH(contextVk, formatInfo.computeDepthPitch(extents.height, unpack.imageHeight,
                                                                inputRowPitch, &inputDepthPitch));

    // Note: skip images for 3D Textures.
    ASSERT(!index.usesTex3D());
    bool applySkipImages = false;

    GLuint inputSkipBytes = 0;
    ANGLE_VK_CHECK_MATH(contextVk,
                        formatInfo.computeSkipBytes(type, inputRowPitch, inputDepthPitch, unpack,
                                                    applySkipImages, &inputSkipBytes));

    RendererVk *renderer = contextVk->getRenderer();

    const vk::Format &vkFormat         = renderer->getFormat(formatInfo.sizedInternalFormat);
    const angle::Format &storageFormat = vkFormat.textureFormat();

    size_t outputRowPitch   = storageFormat.pixelBytes * extents.width;
    size_t outputDepthPitch = outputRowPitch * extents.height;

    VkBuffer bufferHandle = VK_NULL_HANDLE;

    uint8_t *stagingPointer    = nullptr;
    VkDeviceSize stagingOffset = 0;
    size_t allocationSize      = outputDepthPitch * extents.depth;
    ANGLE_TRY(mStagingBuffer.allocate(contextVk, allocationSize, &stagingPointer, &bufferHandle,
                                      &stagingOffset, nullptr));

    const uint8_t *source = pixels + inputSkipBytes;

    LoadImageFunctionInfo loadFunction = vkFormat.textureLoadFunctions(type);

    loadFunction.loadFunction(extents.width, extents.height, extents.depth, source, inputRowPitch,
                              inputDepthPitch, stagingPointer, outputRowPitch, outputDepthPitch);

    VkBufferImageCopy copy = {};

    copy.bufferOffset                    = stagingOffset;
    copy.bufferRowLength                 = extents.width;
    copy.bufferImageHeight               = extents.height;
    copy.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel       = index.getLevelIndex();
    copy.imageSubresource.baseArrayLayer = index.hasLayer() ? index.getLayerIndex() : 0;
    copy.imageSubresource.layerCount     = index.getLayerCount();

    gl_vk::GetOffset(offset, &copy.imageOffset);
    gl_vk::GetExtent(extents, &copy.imageExtent);

    mSubresourceUpdates.emplace_back(bufferHandle, copy);

    return angle::Result::Continue;
}

angle::Result ImageHelper::stageSubresourceUpdateAndGetData(ContextVk *contextVk,
                                                            size_t allocationSize,
                                                            const gl::ImageIndex &imageIndex,
                                                            const gl::Extents &extents,
                                                            const gl::Offset &offset,
                                                            uint8_t **destData)
{
    VkBuffer bufferHandle;
    VkDeviceSize stagingOffset = 0;
    ANGLE_TRY(mStagingBuffer.allocate(contextVk, allocationSize, destData, &bufferHandle,
                                      &stagingOffset, nullptr));

    VkBufferImageCopy copy               = {};
    copy.bufferOffset                    = stagingOffset;
    copy.bufferRowLength                 = extents.width;
    copy.bufferImageHeight               = extents.height;
    copy.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel       = imageIndex.getLevelIndex();
    copy.imageSubresource.baseArrayLayer = imageIndex.hasLayer() ? imageIndex.getLayerIndex() : 0;
    copy.imageSubresource.layerCount     = imageIndex.getLayerCount();

    gl_vk::GetOffset(offset, &copy.imageOffset);
    gl_vk::GetExtent(extents, &copy.imageExtent);

    mSubresourceUpdates.emplace_back(bufferHandle, copy);

    return angle::Result::Continue;
}

angle::Result ImageHelper::stageSubresourceUpdateFromFramebuffer(
    const gl::Context *context,
    const gl::ImageIndex &index,
    const gl::Rectangle &sourceArea,
    const gl::Offset &dstOffset,
    const gl::Extents &dstExtent,
    const gl::InternalFormat &formatInfo,
    FramebufferVk *framebufferVk)
{
    ContextVk *contextVk = vk::GetImpl(context);

    // If the extents and offset is outside the source image, we need to clip.
    gl::Rectangle clippedRectangle;
    const gl::Extents readExtents = framebufferVk->getReadImageExtents();
    if (!ClipRectangle(sourceArea, gl::Rectangle(0, 0, readExtents.width, readExtents.height),
                       &clippedRectangle))
    {
        // Empty source area, nothing to do.
        return angle::Result::Continue;
    }

    bool isViewportFlipEnabled = contextVk->isViewportFlipEnabledForDrawFBO();
    if (isViewportFlipEnabled)
    {
        clippedRectangle.y = readExtents.height - clippedRectangle.y - clippedRectangle.height;
    }

    // 1- obtain a buffer handle to copy to
    RendererVk *renderer = contextVk->getRenderer();

    const vk::Format &vkFormat         = renderer->getFormat(formatInfo.sizedInternalFormat);
    const angle::Format &storageFormat = vkFormat.textureFormat();
    LoadImageFunctionInfo loadFunction = vkFormat.textureLoadFunctions(formatInfo.type);

    size_t outputRowPitch   = storageFormat.pixelBytes * clippedRectangle.width;
    size_t outputDepthPitch = outputRowPitch * clippedRectangle.height;

    VkBuffer bufferHandle = VK_NULL_HANDLE;

    uint8_t *stagingPointer    = nullptr;
    VkDeviceSize stagingOffset = 0;

    // The destination is only one layer deep.
    size_t allocationSize = outputDepthPitch;
    ANGLE_TRY(mStagingBuffer.allocate(contextVk, allocationSize, &stagingPointer, &bufferHandle,
                                      &stagingOffset, nullptr));

    const angle::Format &copyFormat =
        GetFormatFromFormatType(formatInfo.internalFormat, formatInfo.type);
    PackPixelsParams params(clippedRectangle, copyFormat, static_cast<GLuint>(outputRowPitch),
                            isViewportFlipEnabled, nullptr, 0);

    // 2- copy the source image region to the pixel buffer using a cpu readback
    if (loadFunction.requiresConversion)
    {
        // When a conversion is required, we need to use the loadFunction to read from a temporary
        // buffer instead so its an even slower path.
        size_t bufferSize =
            storageFormat.pixelBytes * clippedRectangle.width * clippedRectangle.height;
        angle::MemoryBuffer *memoryBuffer = nullptr;
        ANGLE_VK_CHECK_ALLOC(contextVk, context->getScratchBuffer(bufferSize, &memoryBuffer));

        // Read into the scratch buffer
        ANGLE_TRY(framebufferVk->readPixelsImpl(
            contextVk, clippedRectangle, params, VK_IMAGE_ASPECT_COLOR_BIT,
            framebufferVk->getColorReadRenderTarget(), memoryBuffer->data()));

        // Load from scratch buffer to our pixel buffer
        loadFunction.loadFunction(clippedRectangle.width, clippedRectangle.height, 1,
                                  memoryBuffer->data(), outputRowPitch, 0, stagingPointer,
                                  outputRowPitch, 0);
    }
    else
    {
        // We read directly from the framebuffer into our pixel buffer.
        ANGLE_TRY(framebufferVk->readPixelsImpl(
            contextVk, clippedRectangle, params, VK_IMAGE_ASPECT_COLOR_BIT,
            framebufferVk->getColorReadRenderTarget(), stagingPointer));
    }

    // 3- enqueue the destination image subresource update
    VkBufferImageCopy copyToImage               = {};
    copyToImage.bufferOffset                    = static_cast<VkDeviceSize>(stagingOffset);
    copyToImage.bufferRowLength                 = 0;  // Tightly packed data can be specified as 0.
    copyToImage.bufferImageHeight               = clippedRectangle.height;
    copyToImage.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copyToImage.imageSubresource.mipLevel       = index.getLevelIndex();
    copyToImage.imageSubresource.baseArrayLayer = index.hasLayer() ? index.getLayerIndex() : 0;
    copyToImage.imageSubresource.layerCount     = index.getLayerCount();
    gl_vk::GetOffset(dstOffset, &copyToImage.imageOffset);
    gl_vk::GetExtent(dstExtent, &copyToImage.imageExtent);

    // 3- enqueue the destination image subresource update
    mSubresourceUpdates.emplace_back(bufferHandle, copyToImage);
    return angle::Result::Continue;
}

void ImageHelper::stageSubresourceUpdateFromImage(vk::ImageHelper *image,
                                                  const gl::ImageIndex &index,
                                                  const gl::Offset &destOffset,
                                                  const gl::Extents &extents)
{
    VkImageCopy copyToImage                   = {};
    copyToImage.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copyToImage.srcSubresource.layerCount     = index.getLayerCount();
    copyToImage.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copyToImage.dstSubresource.mipLevel       = index.getLevelIndex();
    copyToImage.dstSubresource.baseArrayLayer = index.hasLayer() ? index.getLayerIndex() : 0;
    copyToImage.dstSubresource.layerCount     = index.getLayerCount();
    gl_vk::GetOffset(destOffset, &copyToImage.dstOffset);
    gl_vk::GetExtent(extents, &copyToImage.extent);

    mSubresourceUpdates.emplace_back(image, copyToImage);
}

angle::Result ImageHelper::allocateStagingMemory(ContextVk *contextVk,
                                                 size_t sizeInBytes,
                                                 uint8_t **ptrOut,
                                                 VkBuffer *handleOut,
                                                 VkDeviceSize *offsetOut,
                                                 bool *newBufferAllocatedOut)
{
    return mStagingBuffer.allocate(contextVk, sizeInBytes, ptrOut, handleOut, offsetOut,
                                   newBufferAllocatedOut);
}

angle::Result ImageHelper::flushStagedUpdates(Context *context,
                                              uint32_t baseLevel,
                                              uint32_t levelCount,
                                              vk::CommandBuffer *commandBuffer)
{
    if (mSubresourceUpdates.empty())
    {
        return angle::Result::Continue;
    }

    RendererVk *renderer = context->getRenderer();

    ANGLE_TRY(mStagingBuffer.flush(context));

    std::vector<SubresourceUpdate> updatesToKeep;

    for (SubresourceUpdate &update : mSubresourceUpdates)
    {
        ASSERT((update.updateSource == SubresourceUpdate::UpdateSource::Buffer &&
                update.buffer.bufferHandle != VK_NULL_HANDLE) ||
               (update.updateSource == SubresourceUpdate::UpdateSource::Image &&
                update.image.image != nullptr && update.image.image->valid()));

        const uint32_t updateMipLevel = update.dstSubresource().mipLevel;

        // It's possible we've accumulated updates that are no longer applicable if the image has
        // never been flushed but the image description has changed. Check if this level exist for
        // this image.
        if (updateMipLevel < baseLevel || updateMipLevel >= baseLevel + levelCount)
        {
            updatesToKeep.emplace_back(update);
            continue;
        }

        // Conservatively flush all writes to the image. We could use a more restricted barrier.
        // Do not move this above the for loop, otherwise multiple updates can have race conditions
        // and not be applied correctly as seen in:
        // dEQP-gles2.functional_texture_specification_texsubimage2d_align_2d* tests on Windows AMD
        changeLayout(VK_IMAGE_ASPECT_COLOR_BIT, vk::ImageLayout::TransferDst, commandBuffer);

        if (update.updateSource == SubresourceUpdate::UpdateSource::Buffer)
        {
            commandBuffer->copyBufferToImage(update.buffer.bufferHandle, mImage, getCurrentLayout(),
                                             1, &update.buffer.copyRegion);
        }
        else
        {
            // Note: currently, the staging images are only made through color attachment writes. If
            // they were written to otherwise in the future, the src stage of this transition should
            // be adjusted appropriately.
            update.image.image->changeLayout(VK_IMAGE_ASPECT_COLOR_BIT,
                                             vk::ImageLayout::TransferSrc, commandBuffer);

            update.image.image->addReadDependency(this);

            commandBuffer->copyImage(update.image.image->getImage(),
                                     update.image.image->getCurrentLayout(), mImage,
                                     getCurrentLayout(), 1, &update.image.copyRegion);
        }

        update.release(renderer);
    }

    // Only remove the updates that were actually applied to the image.
    mSubresourceUpdates = std::move(updatesToKeep);

    if (mSubresourceUpdates.empty())
    {
        mStagingBuffer.releaseRetainedBuffers(context->getRenderer());
    }
    else
    {
        WARN() << "Internal Vulkan buffer could not be released. This is likely due to having "
                  "extra images defined in the Texture.";
    }

    return angle::Result::Continue;
}

bool ImageHelper::hasStagedUpdates() const
{
    return !mSubresourceUpdates.empty();
}

// ImageHelper::SubresourceUpdate implementation
ImageHelper::SubresourceUpdate::SubresourceUpdate()
    : updateSource(UpdateSource::Buffer), buffer{VK_NULL_HANDLE}
{}

ImageHelper::SubresourceUpdate::SubresourceUpdate(VkBuffer bufferHandleIn,
                                                  const VkBufferImageCopy &copyRegionIn)
    : updateSource(UpdateSource::Buffer), buffer{bufferHandleIn, copyRegionIn}
{}

ImageHelper::SubresourceUpdate::SubresourceUpdate(vk::ImageHelper *imageIn,
                                                  const VkImageCopy &copyRegionIn)
    : updateSource(UpdateSource::Image), image{imageIn, copyRegionIn}
{}

ImageHelper::SubresourceUpdate::SubresourceUpdate(const SubresourceUpdate &other)
    : updateSource(other.updateSource)
{
    if (updateSource == UpdateSource::Buffer)
    {
        buffer = other.buffer;
    }
    else
    {
        image = other.image;
    }
}

void ImageHelper::SubresourceUpdate::release(RendererVk *renderer)
{
    if (updateSource == UpdateSource::Image)
    {
        image.image->releaseImage(renderer);
        image.image->releaseStagingBuffer(renderer);
        SafeDelete(image.image);
    }
}

bool ImageHelper::SubresourceUpdate::isUpdateToLayerLevel(uint32_t layerIndex,
                                                          uint32_t levelIndex) const
{
    const VkImageSubresourceLayers &dst = dstSubresource();
    return dst.baseArrayLayer == layerIndex && dst.mipLevel == levelIndex;
}

// FramebufferHelper implementation.
FramebufferHelper::FramebufferHelper() : CommandGraphResource(CommandGraphResourceType::Framebuffer)
{}

FramebufferHelper::~FramebufferHelper() = default;

angle::Result FramebufferHelper::init(ContextVk *contextVk,
                                      const VkFramebufferCreateInfo &createInfo)
{
    ANGLE_VK_TRY(contextVk, mFramebuffer.init(contextVk->getDevice(), createInfo));
    return angle::Result::Continue;
}

void FramebufferHelper::release(RendererVk *renderer)
{
    renderer->releaseObject(getStoredQueueSerial(), &mFramebuffer);
}

// ShaderProgramHelper implementation.
ShaderProgramHelper::ShaderProgramHelper() = default;

ShaderProgramHelper::~ShaderProgramHelper() = default;

bool ShaderProgramHelper::valid() const
{
    // This will need to be extended for compute shader support.
    return mShaders[gl::ShaderType::Vertex].valid();
}

void ShaderProgramHelper::destroy(VkDevice device)
{
    mGraphicsPipelines.destroy(device);
    mComputePipeline.destroy(device);
    for (BindingPointer<ShaderAndSerial> &shader : mShaders)
    {
        shader.reset();
    }
}

void ShaderProgramHelper::release(RendererVk *renderer)
{
    mGraphicsPipelines.release(renderer);
    renderer->releaseObject(mComputePipeline.getSerial(), &mComputePipeline.get());
    for (BindingPointer<ShaderAndSerial> &shader : mShaders)
    {
        shader.reset();
    }
}

void ShaderProgramHelper::setShader(gl::ShaderType shaderType, RefCounted<ShaderAndSerial> *shader)
{
    mShaders[shaderType].set(shader);
}

angle::Result ShaderProgramHelper::getComputePipeline(Context *context,
                                                      const PipelineLayout &pipelineLayout,
                                                      PipelineAndSerial **pipelineOut)
{
    if (mComputePipeline.valid())
    {
        *pipelineOut = &mComputePipeline;
        return angle::Result::Continue;
    }

    RendererVk *renderer = context->getRenderer();

    VkPipelineShaderStageCreateInfo shaderStage = {};
    VkComputePipelineCreateInfo createInfo      = {};

    shaderStage.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.flags               = 0;
    shaderStage.stage               = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module              = mShaders[gl::ShaderType::Compute].get().get().getHandle();
    shaderStage.pName               = "main";
    shaderStage.pSpecializationInfo = nullptr;

    createInfo.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    createInfo.flags              = 0;
    createInfo.stage              = shaderStage;
    createInfo.layout             = pipelineLayout.getHandle();
    createInfo.basePipelineHandle = VK_NULL_HANDLE;
    createInfo.basePipelineIndex  = 0;

    ANGLE_VK_TRY(context, mComputePipeline.get().initCompute(context->getDevice(), createInfo,
                                                             renderer->getPipelineCache()));

    *pipelineOut = &mComputePipeline;
    return angle::Result::Continue;
}

}  // namespace vk
}  // namespace rx
