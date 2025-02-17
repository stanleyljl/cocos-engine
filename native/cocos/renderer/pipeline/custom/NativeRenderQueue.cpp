#include <algorithm>
#include <iterator>
#include "GslUtils.h"
#include "NativePipelineTypes.h"
#include "cocos/renderer/pipeline/Define.h"
#include "cocos/renderer/pipeline/PipelineStateManager.h"

namespace cc {

namespace render {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void RenderDrawQueue::add(const scene::Model &model, float depth, uint32_t subModelIdx, uint32_t passIdx) {
    const auto *subModel = model.getSubModels()[subModelIdx].get();
    const auto *const pass = subModel->getPass(passIdx);

    auto passPriority = static_cast<uint32_t>(pass->getPriority());
    auto modelPriority = static_cast<uint32_t>(subModel->getPriority());
    auto shaderId = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(subModel->getShader(passIdx)));
    const auto hash = (0 << 30) | (passPriority << 16) | (modelPriority << 8) | passIdx;
    const auto priority = model.getPriority();

    instances.emplace_back(DrawInstance{subModel, priority, hash, depth, shaderId, passIdx});
}

void RenderDrawQueue::sortOpaqueOrCutout() {
    std::sort(instances.begin(), instances.end(), [](const DrawInstance &lhs, const DrawInstance &rhs) {
        return std::forward_as_tuple(lhs.hash, lhs.depth, lhs.shaderID) <
               std::forward_as_tuple(rhs.hash, rhs.depth, rhs.shaderID);
    });
}

void RenderDrawQueue::sortTransparent() {
    std::sort(instances.begin(), instances.end(), [](const DrawInstance &lhs, const DrawInstance &rhs) {
        return std::forward_as_tuple(lhs.priority, lhs.hash, -lhs.depth, lhs.shaderID) <
               std::forward_as_tuple(rhs.priority, rhs.hash, -rhs.depth, rhs.shaderID);
    });
}

void RenderDrawQueue::recordCommandBuffer(
    gfx::Device * /*device*/, const scene::Camera * /*camera*/,
    gfx::RenderPass *renderPass, gfx::CommandBuffer *cmdBuff,
    uint32_t subpassIndex) const {
    for (const auto &instance : instances) {
        const auto *subModel = instance.subModel;

        const auto passIdx = instance.passIndex;
        auto *inputAssembler = subModel->getInputAssembler();
        const auto *pass = subModel->getPass(passIdx);
        auto *shader = subModel->getShader(passIdx);
        auto *pso = pipeline::PipelineStateManager::getOrCreatePipelineState(pass, shader, inputAssembler, renderPass, subpassIndex);

        cmdBuff->bindPipelineState(pso);
        cmdBuff->bindDescriptorSet(pipeline::materialSet, pass->getDescriptorSet());
        cmdBuff->bindDescriptorSet(pipeline::localSet, subModel->getDescriptorSet());
        cmdBuff->bindInputAssembler(inputAssembler);
        cmdBuff->draw(inputAssembler);
    }
}

void RenderInstancingQueue::add(pipeline::InstancedBuffer &instancedBuffer) {
    batches.emplace(&instancedBuffer);
}

void RenderInstancingQueue::sort() {
    sortedBatches.reserve(batches.size());
    std::copy(batches.begin(), batches.end(), std::back_inserter(sortedBatches));
}

void RenderInstancingQueue::uploadBuffers(gfx::CommandBuffer *cmdBuffer) const {
    for (const auto *instanceBuffer : batches) {
        if (instanceBuffer->hasPendingModels()) {
            instanceBuffer->uploadBuffers(cmdBuffer);
        }
    }
}

void RenderInstancingQueue::recordCommandBuffer(
    gfx::RenderPass *renderPass, gfx::CommandBuffer *cmdBuffer,
    gfx::DescriptorSet *ds, uint32_t offset, const ccstd::vector<uint32_t> *dynamicOffsets) const {
    const auto &renderQueue = sortedBatches;
    for (const auto *instanceBuffer : renderQueue) {
        if (!instanceBuffer->hasPendingModels()) {
            continue;
        }
        const auto &instances = instanceBuffer->getInstances();
        const auto *drawPass = instanceBuffer->getPass();
        cmdBuffer->bindDescriptorSet(pipeline::materialSet, drawPass->getDescriptorSet());
        gfx::PipelineState *lastPSO = nullptr;
        for (const auto &instance : instances) {
            if (!instance.count) {
                continue;
            }
            auto *pso = pipeline::PipelineStateManager::getOrCreatePipelineState(
                drawPass, instance.shader, instance.ia, renderPass);
            if (lastPSO != pso) {
                cmdBuffer->bindPipelineState(pso);
                lastPSO = pso;
            }
            if (ds) {
                cmdBuffer->bindDescriptorSet(pipeline::globalSet, ds, 1, &offset);
            }
            if (dynamicOffsets) {
                cmdBuffer->bindDescriptorSet(pipeline::localSet, instance.descriptorSet, *dynamicOffsets);
            } else {
                cmdBuffer->bindDescriptorSet(pipeline::localSet, instance.descriptorSet, instanceBuffer->dynamicOffsets());
            }
            cmdBuffer->bindInputAssembler(instance.ia);
            cmdBuffer->draw(instance.ia);
        }
    }
}

void NativeRenderQueue::sort() {
    opaqueQueue.sortOpaqueOrCutout();
    transparentQueue.sortTransparent();
    opaqueInstancingQueue.sort();
    transparentInstancingQueue.sort();
}

} // namespace render

} // namespace cc
